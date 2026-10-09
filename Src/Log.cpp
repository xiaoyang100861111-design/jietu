#include "pch.h"
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <mutex>
#include "Log.h"
#include "Setting.h"
#include "Update.h"

namespace {
	std::mutex logMutex;
	std::filesystem::path logPath;
	constexpr uintmax_t maxLogSize{ 512 * 1024 };

	std::wstring nowStr()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	}

	// 地址落在哪个模块里：写成"模块文件名+偏移"。偏移是相对模块基址的，不受每次加载位置不同的影响
	std::wstring describeAddr(DWORD64 addr)
	{
		HMODULE mod{ nullptr };
		if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
			wchar_t name[MAX_PATH]{};
			GetModuleFileName(mod, name, MAX_PATH);
			std::wstring file{ name };
			auto pos = file.find_last_of(L"\\/");
			if (pos != std::wstring::npos) file = file.substr(pos + 1);
			return std::format(L"{}+0x{:X}", file, addr - reinterpret_cast<DWORD64>(mod));
		}
		return std::format(L"0x{:X}", addr);
	}

	// 从出事那一刻的寄存器现场一帧一帧往回退，记下每一帧的返回地址
	std::wstring stackOf(const CONTEXT* origin)
	{
		std::wstring result;
#ifdef _M_X64
		CONTEXT ctx = *origin;
		for (int i = 0; i < 40 && ctx.Rip != 0; i++) {
			result += L"\r\n    " + describeAddr(ctx.Rip);
			DWORD64 base{ 0 };
			auto func = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
			if (!func) {
				// 叶子函数没有展开信息：返回地址就在栈顶
				if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), sizeof(DWORD64))) break;
				ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
				ctx.Rsp += sizeof(DWORD64);
				continue;
			}
			void* handlerData{ nullptr };
			DWORD64 establisher{ 0 };
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, func, &ctx, &handlerData, &establisher, nullptr);
		}
#endif
		return result;
	}

	LONG WINAPI onCrash(EXCEPTION_POINTERS* info)
	{
		// 记日志的过程中又出事了：别再进来，直接让进程结束
		static LONG entered{ 0 };
		if (InterlockedExchange(&entered, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;
		auto rec = info->ExceptionRecord;
		auto text = std::format(L"CRASH code=0x{:08X} at {}", static_cast<DWORD>(rec->ExceptionCode),
			describeAddr(reinterpret_cast<DWORD64>(rec->ExceptionAddress)));
		// 访问违例：第一个参数是读(0)/写(1)/执行(8)，第二个是访问的地址
		if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
			text += std::format(L" ({} 0x{:X})", rec->ExceptionInformation[0] == 1 ? L"write" : L"read",
				static_cast<DWORD64>(rec->ExceptionInformation[1]));
		}
		text += L"\r\n  stack:" + stackOf(info->ContextRecord);
		Log::write(text);
		auto tip = L"UU截图 出错了，需要关闭。\n\n错误日志已保存到：\n" + logPath.wstring();
		MessageBox(nullptr, tip.data(), L"UU截图", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// 没人接的 C++ 异常（包括 WinRT 调用失败抛出来的）最后会走到这里
	void onTerminate()
	{
		Log::exception(L"terminate");
		auto tip = L"UU截图 出错了，需要关闭。\n\n错误日志已保存到：\n" + logPath.wstring();
		MessageBox(nullptr, tip.data(), L"UU截图", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
		TerminateProcess(GetCurrentProcess(), 3);
	}
}

void Log::init()
{
	logPath = Setting::get()->getDataPath() / L"log.txt";
	// 太大了就把旧的挪到 log.old.txt，只留一份
	std::error_code ec;
	if (std::filesystem::exists(logPath, ec) && std::filesystem::file_size(logPath, ec) > maxLogSize) {
		auto oldPath = logPath;
		oldPath.replace_filename(L"log.old.txt");
		std::filesystem::remove(oldPath, ec);
		std::filesystem::rename(logPath, oldPath, ec);
	}
	SetUnhandledExceptionFilter(onCrash);
	std::set_terminate(onTerminate);

	// 每次启动记一行：程序版本、编译时间、系统版本，查问题时先得知道是哪一版
	auto ver = Ling::Util::getVerNum();
	OSVERSIONINFOW os{ sizeof(os) };
	auto rtlGetVersion = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(
		GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
	if (rtlGetVersion) rtlGetVersion(&os);
	write(std::format(L"START version={} file={}.{}.{} built={} {} windows={}.{}.{}", Update::version(), ver[0], ver[1], ver[2],
		Ling::Util::convertToWStr(__DATE__), Ling::Util::convertToWStr(__TIME__),
		os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber));
}

void Log::write(const std::wstring& text)
{
	std::lock_guard lock{ logMutex };
	if (logPath.empty()) return;
	std::ofstream file{ logPath, std::ios::binary | std::ios::app };
	if (!file) return;
	file << Ling::Util::convertToStr(L"[" + nowStr() + L"] " + text + L"\r\n");
}

std::wstring Log::describe()
{
	auto cur = std::current_exception();
	if (!cur) return L"no active exception";
	try {
		std::rethrow_exception(cur);
	}
	catch (const winrt::hresult_error& e) {
		std::wstring msg{ e.message() };
		// 系统给的解释常常带着结尾的换行
		while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
		return std::format(L"0x{:08X} {}", static_cast<uint32_t>(e.code().value), msg);
	}
	catch (const std::exception& e) {
		return Ling::Util::convertToWStr(e.what());
	}
	catch (...) {
	}
	return L"unknown exception";
}

void Log::exception(const std::wstring& where)
{
	write(L"ERROR " + where + L": " + describe());
}

std::filesystem::path Log::getPath()
{
	return logPath;
}
