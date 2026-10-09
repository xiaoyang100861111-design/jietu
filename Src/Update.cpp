#include "pch.h"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <format>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Storage.Streams.h>
#include "Update.h"
#include "BuildInfo.h"
#include "Setting.h"
#include "Lang.h"
#include "Log.h"

namespace {
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Data::Json;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using namespace winrt::Windows::Storage::Streams;

	// 两个地址是同一份文件：raw 域名直连快；有的网络下它不通，再试 github.com（它会跳转过去）
	constexpr std::wstring_view versionUrls[]{
		L"https://raw.githubusercontent.com/xiaoyang100861111-design/jietu/dist/version.json",
		L"https://github.com/xiaoyang100861111-design/jietu/raw/dist/version.json",
	};
	constexpr std::wstring_view defaultExeUrl{ L"https://github.com/xiaoyang100861111-design/jietu/raw/dist/UU%E6%88%AA%E5%9B%BE.exe" };
	// 下载下来的新版 exe 就叫这个名字。跟正式的 exe 名字区分开 —— 用户可能把 exe 改过名，
	// 覆盖的时候按当前 exe 的实际路径来，不能按这个名字来
	constexpr std::wstring_view newExeName{ L"UUjietu.update.exe" };

	struct Info {
		std::wstring version, notes, url;
	};

	bool busy{ false };          //正在查 / 正在下载，别重复起
	UINT_PTR startTimer{ 0 };

	std::filesystem::path selfPath()
	{
		std::vector<wchar_t> buf(MAX_PATH);
		auto len = GetModuleFileName(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
		if (len == 0) return {};
		return std::filesystem::path{ std::wstring{ buf.data(), len } };
	}

	// exe 所在的目录能不能写。装在 Program Files 下（没提权）、或者放在只读介质上就不能，
	// 这种情况下下载了也覆盖不上去
	bool canWrite(const std::filesystem::path& dir)
	{
		auto probe = (dir / L"uu.update.probe").wstring();
		auto file = CreateFile(probe.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		CloseHandle(file); //DELETE_ON_CLOSE，关掉就没了，不用自己删
		return true;
	}

	//路径要放进 PowerShell 的单引号字符串里，里面本来有的单引号得翻倍
	std::wstring quotePath(const std::filesystem::path& path)
	{
		std::wstring str = path.wstring();
		size_t pos{ 0 };
		while ((pos = str.find(L'\'', pos)) != std::wstring::npos) {
			str.insert(pos, 1, L'\'');
			pos += 2;
		}
		return L"'" + str + L"'";
	}

	// 起一段 PowerShell 收尾：等本进程退出（exe 开着的时候覆盖不掉自己）、把新 exe 覆盖过去、
	// 用 --enter=tray 把新版本拉起来（只挂托盘，不弹截图窗口）、最后把中间文件和脚本自己删掉。
	// 返回脚本有没有起成功 —— 没起成功就别退出进程了，不然用户以为在升级，其实什么也没发生
	bool startScript(const std::filesystem::path& newExe, const std::filesystem::path& exePath)
	{
		auto scriptPath = Setting::get()->getDataPath() / L"update.ps1";
		std::wstring pid = std::to_wstring(GetCurrentProcessId());
		std::wstring script;
		script += L"$ErrorActionPreference='SilentlyContinue'\r\n";
		script += L"Wait-Process -Id " + pid + L" -Timeout 60\r\n";
		script += L"Start-Sleep -Milliseconds 500\r\n";
		//直接覆盖，不先删旧的：万一拷贝失败，用户手上至少还有个能用的旧版本
		script += L"Copy-Item -LiteralPath " + quotePath(newExe) + L" -Destination " + quotePath(exePath) + L" -Force\r\n";
		script += L"Start-Process -FilePath " + quotePath(exePath) + L" -ArgumentList '--enter=tray'\r\n";
		script += L"Remove-Item -LiteralPath " + quotePath(newExe) + L" -Force\r\n";
		//脚本删自己：PowerShell 已经把整个文件读完了，删得掉
		script += L"Remove-Item -LiteralPath " + quotePath(scriptPath) + L" -Force\r\n";
		{
			// Windows PowerShell 读没有 BOM 的 .ps1 会按 ANSI 解码，路径里带中文（中文用户名下的
			// %appdata%、中文的 exe 名）会整段乱掉，所以写 UTF-8 + BOM
			std::ofstream file{ scriptPath, std::ios::binary | std::ios::trunc };
			if (!file) return false;
			file << "\xEF\xBB\xBF" << Ling::Util::convertToStr(script);
			if (!file.good()) return false;
		}
		std::wstring cmd{ L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" };
		cmd += scriptPath.wstring() + L"\"";
		STARTUPINFO si{ .cb{ sizeof(STARTUPINFO) } };
		PROCESS_INFORMATION pi{};
		// CREATE_NO_WINDOW：不让 PowerShell 的黑窗口闪出来
		auto flag = CreateProcess(nullptr, cmd.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
		if (!flag) return false;
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return true;
	}

	void tip(const std::wstring& text, UINT icon = MB_ICONINFORMATION)
	{
		MessageBox(nullptr, text.data(), L"UU截图", MB_OK | icon | MB_TOPMOST | MB_SETFOREGROUND);
	}

	// 等一个 WinRT 异步操作，超时就取消并抛。只能在后台线程用
	template<typename T>
	auto waitOp(const T& op, int seconds)
	{
		if (op.wait_for(std::chrono::seconds(seconds)) == winrt::Windows::Foundation::AsyncStatus::Started) {
			op.Cancel();
			throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		}
		return op.GetResults();
	}

	HttpClient makeClient()
	{
		HttpBaseProtocolFilter filter;
		// 每次都真的去问服务端，不吃本机缓存：不然刚发的新版本可能半天看不到
		filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
		filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
		return HttpClient{ filter };
	}

	// 弹窗问用户。返回 1 立即升级，2 忽略这个版本，0 以后再说；never 是"以后都不检查"勾没勾
	int prompt(const Info& info, bool& never)
	{
		never = false;
		auto title = std::format(L"{} {}", Lang::get(L"update.found"), info.version);
		auto content = std::format(L"{} {}\n\n{}\n{}", Lang::get(L"update.current"), Update::version(),
			Lang::get(L"update.notes"), info.notes.empty() ? Lang::get(L"update.noNotes") : info.notes);
		// 带自定义按钮和复选框的对话框要 comctl32 6.0 版里的 TaskDialogIndirect。
		// 用 GetProcAddress 现找而不是直接调：进程里加载的要是老版 comctl32，直接调会让整个程序起不来
		using TaskDialogFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
		TaskDialogFn taskDialog{ nullptr };
		if (auto mod = LoadLibrary(L"comctl32.dll")) {
			taskDialog = reinterpret_cast<TaskDialogFn>(GetProcAddress(mod, "TaskDialogIndirect"));
		}
		if (taskDialog) {
			auto btnUpgrade = Lang::get(L"update.btnUpgrade"), btnIgnore = Lang::get(L"update.btnIgnore"),
				btnLater = Lang::get(L"update.btnLater"), check = Lang::get(L"update.never");
			const TASKDIALOG_BUTTON buttons[]{ { 100, btnUpgrade.data() }, { 101, btnIgnore.data() }, { 102, btnLater.data() } };
			TASKDIALOGCONFIG cfg{};
			cfg.cbSize = sizeof(cfg);
			cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
			cfg.pszWindowTitle = L"UU截图";
			cfg.pszMainIcon = TD_INFORMATION_ICON;
			cfg.pszMainInstruction = title.data();
			cfg.pszContent = content.data();
			cfg.cButtons = 3;
			cfg.pButtons = buttons;
			cfg.nDefaultButton = 100;
			cfg.pszVerificationText = check.data();
			int button{ 0 };
			BOOL checked{ FALSE };
			if (SUCCEEDED(taskDialog(&cfg, &button, nullptr, &checked))) {
				never = checked != FALSE;
				return button == 100 ? 1 : button == 101 ? 2 : 0;
			}
		}
		// 退路：普通消息框只有 是 / 否 / 取消 三个键，把含义写在正文里
		auto text = title + L"\n\n" + content + L"\n\n" + Lang::get(L"update.fallbackTip");
		auto ret = MessageBox(nullptr, text.data(), L"UU截图", MB_YESNOCANCEL | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
		return ret == IDYES ? 1 : ret == IDNO ? 2 : 0;
	}

	winrt::fire_and_forget download(Info info)
	{
		co_await winrt::resume_background();
		std::filesystem::path target;
		try {
			auto client = makeClient();
			auto buffer = waitOp(client.GetBufferAsync(Uri{ info.url.empty() ? std::wstring{ defaultExeUrl } : info.url }), 180);
			std::vector<BYTE> bytes(buffer.Length());
			DataReader::FromBuffer(buffer).ReadBytes(bytes);
			//不是 PE 文件、或者小得离谱：多半下回来的是个错误页，不是 exe
			if (bytes.size() < 500 * 1024 || bytes[0] != 'M' || bytes[1] != 'Z') throw winrt::hresult_error(E_FAIL);
			target = Setting::get()->getDataPath() / newExeName;
			std::ofstream file{ target, std::ios::binary | std::ios::trunc };
			file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			if (!file.good()) throw winrt::hresult_error(E_FAIL);
		}
		catch (...) {
			Log::exception(L"update download");
			target.clear();
		}
		// 回 UI 线程：弹窗、退出进程都得在那边做
		Ling::App::get()->dq.TryEnqueue([target, info]() {
			busy = false;
			auto exePath = selfPath();
			if (!target.empty() && !exePath.empty() && startScript(target, exePath)) {
				Log::write(L"UPDATE to " + info.version);
				Ling::App::get()->quit(0); //退出让位，剩下的活交给脚本
				return;
			}
			// 自动升级没走通：问用户要不要自己去下
			auto text = Lang::get(L"update.failed");
			if (MessageBox(nullptr, text.data(), L"UU截图", MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) == IDOK) {
				ShellExecute(nullptr, L"open", defaultExeUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
			}
		});
	}

	// UI 线程上：拿到服务端的版本信息之后
	void onChecked(bool ok, const Info& info, bool manual)
	{
		busy = false;
		auto setting = Setting::get();
		if (!setting) return;
		const auto cur = Update::version();
		if (!ok) {
			if (manual) tip(Lang::get(L"update.checkFailed"), MB_ICONWARNING);
			return;
		}
		// 版本号是等长的"年.月.日.时分"，按字符串比就是按时间比。自己编的（dev）不参与
		if (cur == L"dev" || info.version <= cur) {
			if (manual) tip(std::format(L"{}\n\n{} {}", Lang::get(L"update.latest"), Lang::get(L"update.current"), cur));
			return;
		}
		if (!manual && setting->getUpdateIgnore() == info.version) return;
		bool never{ false };
		const int choice = prompt(info, never);
		if (never) setting->setUpdateEnabled(false);
		if (choice == 2) {
			setting->setUpdateIgnore(info.version);
		}
		else if (choice == 1) {
			// exe 所在目录写不进去：自动升级这条路走不通，直接带用户去下载
			auto exePath = selfPath();
			if (exePath.empty() || !canWrite(exePath.parent_path())) {
				tip(Lang::get(L"update.noPermission"), MB_ICONWARNING);
				ShellExecute(nullptr, L"open", defaultExeUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
				return;
			}
			busy = true;
			download(info);
		}
	}

	winrt::fire_and_forget check(bool manual)
	{
		// 网络请求挪到后台线程：可能卡好几秒，挂在 UI 线程上整个应用就不动了
		co_await winrt::resume_background();
		Info info;
		bool ok{ false };
		for (auto url : versionUrls) {
			try {
				auto client = makeClient();
				std::wstring body{ waitOp(client.GetStringAsync(Uri{ url }), 12) };
				JsonObject obj{ nullptr };
				//返回的东西不是合法 JSON（被网关塞了个错误页之类）：当这个地址不通
				if (!JsonObject::TryParse(body, obj)) continue;
				info.version = std::wstring{ obj.GetNamedString(L"version", L"") };
				info.notes = std::wstring{ obj.GetNamedString(L"notes", L"") };
				info.url = std::wstring{ obj.GetNamedString(L"url", L"") };
				if (info.version.empty()) continue;
				ok = true;
				break;
			}
			catch (...) {
				Log::exception(L"update check");
			}
		}
		Ling::App::get()->dq.TryEnqueue([ok, info, manual]() { onChecked(ok, info, manual); });
	}

	void CALLBACK onStartTimer(HWND, UINT, UINT_PTR id, DWORD)
	{
		KillTimer(nullptr, id);
		startTimer = 0;
		if (busy) return;
		busy = true;
		check(false);
	}
}

std::wstring Update::version()
{
	return BUILD_VERSION;
}

void Update::checkOnStart()
{
	if (startTimer || busy) return;
	auto setting = Setting::get();
	if (!setting || !setting->getUpdateEnabled()) return;
	// 等几秒再查：启动这会儿让给托盘、热键这些要紧的
	startTimer = SetTimer(nullptr, 0, 4000, onStartTimer);
}

void Update::checkNow()
{
	if (busy) return;
	busy = true;
	check(true);
}

void Update::checkLater()
{
}
