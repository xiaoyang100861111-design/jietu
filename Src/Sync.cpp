#include "pch.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include "Sync.h"
#include "Setting.h"
#include "Lang.h"
#include "Log.h"

namespace {
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using winrt::Windows::Storage::Streams::UnicodeEncoding;

	constexpr std::wstring_view baseUrl{ L"https://dk.zx3777.com" };
	UINT_PTR timer{ 0 };
	bool busy{ false };
	// 上一次和服务端对齐时的内容。之后拿现在的内容和它比，不一样就是有改动要上传
	std::wstring lastPushed;

	// 一次请求。只能在后台线程上调（里面是阻塞等待）。
	// 返回 HTTP 状态码，连都没连上时返回 0（这时 text 里是错误原因）
	int request(const std::wstring& method, const std::wstring& path, const std::wstring& body,
		const std::wstring& token, std::wstring& text, int timeoutSec)
	{
		try {
			HttpBaseProtocolFilter filter;
			filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
			filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
			HttpClient client{ filter };
			auto verb = method == L"POST" ? HttpMethod::Post() : method == L"PUT" ? HttpMethod::Put() : HttpMethod::Get();
			HttpRequestMessage req{ verb, Uri{ std::wstring{ baseUrl } + path } };
			if (!token.empty()) req.Headers().TryAppendWithoutValidation(L"Authorization", L"Bearer " + token);
			if (method != L"GET") req.Content(HttpStringContent{ body, UnicodeEncoding::Utf8, L"application/json" });
			auto op = client.SendRequestAsync(req);
			if (op.wait_for(std::chrono::seconds(timeoutSec)) == winrt::Windows::Foundation::AsyncStatus::Started) {
				op.Cancel();
				throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			auto resp = op.GetResults();
			auto read = resp.Content().ReadAsStringAsync();
			if (read.wait_for(std::chrono::seconds(timeoutSec)) == winrt::Windows::Foundation::AsyncStatus::Started) {
				read.Cancel();
				throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			text = std::wstring{ read.GetResults() };
			return static_cast<int>(resp.StatusCode());
		}
		catch (...) {
			text = Log::describe();
			Log::write(L"ERROR sync " + method + L" " + path + L": " + text);
			return 0;
		}
	}

	// 异步版：后台线程上发，做完回到 UI 线程调 done
	winrt::fire_and_forget send(std::wstring method, std::wstring path, std::wstring body, std::wstring token,
		std::function<void(int status, std::wstring text)> done)
	{
		co_await winrt::resume_background();
		std::wstring text;
		const int status = request(method, path, body, token, text, 15);
		Ling::App::get()->dq.TryEnqueue([done, status, text]() { done(status, text); });
	}

	std::filesystem::path phrasesPath()
	{
		return Setting::get()->getDataPath() / L"clip" / L"phrases.json";
	}

	std::wstring errorOf(const std::wstring& json, int status)
	{
		JsonObject obj{ nullptr };
		std::wstring why;
		if (JsonObject::TryParse(json, obj)) why = std::wstring{ obj.GetNamedString(L"error", L"") };
		if (!why.empty()) return why;
		return status == 0 ? Lang::get(L"sync.errNet") + L"\n" + json : std::format(L"{} ({})", Lang::get(L"sync.errServer"), status);
	}

	// 要同步的两样东西打成一个请求体：{ "config": "...", "phrases": "..." }（两个都是 JSON 文本，当字符串装进去）
	std::wstring payload()
	{
		JsonObject body;
		body.SetNamedValue(L"config", JsonValue::CreateStringValue(Setting::get()->exportForSync()));
		body.SetNamedValue(L"phrases", JsonValue::CreateStringValue(Ling::Util::readFileText(phrasesPath())));
		return std::wstring{ body.Stringify() };
	}

	// 把服务端的那一份装到本机。backup：先把本机现有的备份一份（登录时用，免得把本机原来的话术冲没了）
	void applyRemote(const std::wstring& config, const std::wstring& phrases, long long version, bool backup)
	{
		auto setting = Setting::get();
		std::error_code ec;
		if (backup) {
			SYSTEMTIME st{};
			GetLocalTime(&st);
			auto dir = setting->getDataPath() / std::format(L"backup-{:04}{:02}{:02}-{:02}{:02}{:02}",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			std::filesystem::create_directories(dir, ec);
			std::filesystem::copy_file(setting->getDataPath() / L"config.json", dir / L"config.json", ec);
			std::filesystem::copy_file(phrasesPath(), dir / L"phrases.json", ec);
			Log::write(L"sync: local config backed up to " + dir.wstring());
		}
		if (!config.empty()) setting->importFromSync(config);
		if (!phrases.empty()) {
			std::filesystem::create_directories(phrasesPath().parent_path(), ec);
			Ling::Util::saveFile(phrasesPath().wstring(), phrases);
		}
		setting->setSyncVersion(version);
	}

	// 软件自己重启一遍：等本进程退了再起（不然会被"已经有一个在运行"拦下来），起来后只挂托盘
	void relaunch()
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileName(nullptr, exe, MAX_PATH);
		std::wstring cmd = std::format(L"cmd.exe /c ping -n 3 127.0.0.1 >nul & start \"\" \"{}\" --enter=tray", exe);
		STARTUPINFO si{ .cb{ sizeof(STARTUPINFO) } };
		PROCESS_INFORMATION pi{};
		if (CreateProcess(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		Ling::App::get()->quit(0);
	}

	void push(Sync::Done done)
	{
		auto setting = Setting::get();
		if (busy || !setting || setting->getAccountToken().empty()) {
			if (done) done(false, L"");
			return;
		}
		busy = true;
		auto body = payload();
		send(L"PUT", L"/api/account/data", body, setting->getAccountToken(), [body, done](int status, std::wstring text) {
			busy = false;
			auto setting = Setting::get();
			if (!setting) return;
			JsonObject obj{ nullptr };
			if (status == 200 && JsonObject::TryParse(text, obj)) {
				lastPushed = body;
				setting->setSyncVersion(static_cast<long long>(obj.GetNamedNumber(L"version", 0)));
				if (done) done(true, Lang::get(L"sync.pushed"));
				return;
			}
			// 令牌不认了（在别处改了密码之类）：退回未登录，本机配置不动
			if (status == 401) setting->setAccount(L"", L"");
			if (done) done(false, errorOf(text, status));
		});
	}

	void CALLBACK onTimer(HWND, UINT, UINT_PTR, DWORD)
	{
		auto setting = Setting::get();
		if (!setting || busy || setting->getAccountToken().empty()) return;
		// 有改动才传。比的是整份内容，所以不管是哪里改的（设置、话术、打卡团队）都能发现
		if (payload() != lastPushed) push(nullptr);
	}
}

void Sync::init()
{
	auto setting = Setting::get();
	const auto token = setting->getAccountToken();
	if (!token.empty()) {
		// 启动时先看服务端有没有比本机新的。这一步要在别的模块读配置之前做完，所以在这里等它；
		// 但不能让启动卡住：最多等 3 秒，连不上就先用本机的，下次启动再说
		struct Result {
			std::mutex mutex;
			std::condition_variable cv;
			bool finished{ false };
			int status{ 0 };
			std::wstring text;
		};
		auto result = std::make_shared<Result>();
		const auto path = std::format(L"/api/account/data?since={}", setting->getSyncVersion());
		std::thread([result, token, path]() {
			winrt::init_apartment();
			std::wstring text;
			const int status = request(L"GET", path, L"", token, text, 3);
			{
				std::lock_guard lock{ result->mutex };
				result->status = status;
				result->text = text;
				result->finished = true;
			}
			result->cv.notify_all();
			winrt::uninit_apartment();
		}).detach();
		std::unique_lock lock{ result->mutex };
		if (result->cv.wait_for(lock, std::chrono::milliseconds(3500), [&]() { return result->finished; })) {
			JsonObject obj{ nullptr };
			if (result->status == 401) {
				setting->setAccount(L"", L"");
			}
			else if (result->status == 200 && JsonObject::TryParse(result->text, obj) && !obj.GetNamedBoolean(L"unchanged", false)) {
				const auto version = static_cast<long long>(obj.GetNamedNumber(L"version", 0));
				if (version > setting->getSyncVersion()) {
					applyRemote(std::wstring{ obj.GetNamedString(L"config", L"") }, std::wstring{ obj.GetNamedString(L"phrases", L"") }, version, false);
					Log::write(std::format(L"sync: pulled version {}", version));
				}
			}
		}
	}
	lastPushed = payload();
	if (!timer) timer = SetTimer(nullptr, 0, 30000, onTimer);
}

void Sync::dispose()
{
	if (timer) KillTimer(nullptr, timer);
	timer = 0;
}

bool Sync::isLoggedIn()
{
	auto setting = Setting::get();
	return setting && !setting->getAccountToken().empty();
}

std::wstring Sync::username()
{
	auto setting = Setting::get();
	return setting ? setting->getAccountName() : L"";
}

void Sync::login(const std::wstring& user, const std::wstring& pwd, bool isRegister, Done done)
{
	if (busy) return;
	busy = true;
	JsonObject body;
	body.SetNamedValue(L"username", JsonValue::CreateStringValue(user));
	body.SetNamedValue(L"password", JsonValue::CreateStringValue(pwd));
	send(L"POST", isRegister ? L"/api/account/register" : L"/api/account/login", std::wstring{ body.Stringify() }, L"",
		[done](int status, std::wstring text) {
			busy = false;
			auto setting = Setting::get();
			if (!setting) return;
			JsonObject obj{ nullptr };
			std::wstring token;
			if (status == 200 && JsonObject::TryParse(text, obj)) token = std::wstring{ obj.GetNamedString(L"token", L"") };
			if (token.empty()) {
				if (done) done(false, errorOf(text, status));
				return;
			}
			setting->setAccount(token, std::wstring{ obj.GetNamedString(L"username", L"") });
			const auto version = static_cast<long long>(obj.GetNamedNumber(L"version", 0));
			Log::write(L"sync: logged in");
			if (version <= 0) {
				// 新账号，服务端还是空的：把本机现在的配置传上去
				push([done](bool ok, const std::wstring& msg) {
					if (done) done(true, Lang::get(ok ? L"sync.loginPushed" : L"sync.loginOnly"));
				});
				return;
			}
			// 服务端存过配置：取回来覆盖本机（本机原来的先备份），然后重启让它生效
			busy = true;
			send(L"GET", L"/api/account/data", L"", token, [done, version](int status, std::wstring text) {
				busy = false;
				JsonObject data{ nullptr };
				if (status != 200 || !JsonObject::TryParse(text, data)) {
					if (done) done(true, Lang::get(L"sync.loginOnly"));
					return;
				}
				applyRemote(std::wstring{ data.GetNamedString(L"config", L"") }, std::wstring{ data.GetNamedString(L"phrases", L"") },
					static_cast<long long>(data.GetNamedNumber(L"version", static_cast<double>(version))), true);
				if (done) done(true, Lang::get(L"sync.loginPulled"));
				relaunch();
			});
		});
}

void Sync::logout()
{
	auto setting = Setting::get();
	if (!setting) return;
	const auto token = setting->getAccountToken();
	if (!token.empty()) send(L"POST", L"/api/account/logout", L"{}", token, [](int, std::wstring) {});
	setting->setAccount(L"", L"");
}

void Sync::pushNow(Done done)
{
	if (!isLoggedIn()) {
		if (done) done(false, Lang::get(L"sync.notLoggedIn"));
		return;
	}
	if (busy) return;
	push(done);
}
