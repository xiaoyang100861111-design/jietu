#include "pch.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include "Report.h"
#include "Log.h"
#include "Setting.h"
#include "Punch.h"
#include "Update.h"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;

namespace {
	constexpr std::wstring_view reportUrl{ L"https://dk.4ee.net/api/report" };
	std::atomic<bool> busy{ false };

	// 一条要传的记录
	struct Item {
		std::wstring kind, version, os, summary, detail;
	};

	std::wstring fromUtf8(const std::string& text)
	{
		if (text.empty()) return L"";
		const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
		std::wstring result(len, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), len);
		return result;
	}

	// 日志里一行的正文：去掉开头的 "[时间] "
	std::wstring bodyOf(const std::wstring& line)
	{
		auto pos = line.find(L"] ");
		return line.starts_with(L"[") && pos != std::wstring::npos ? line.substr(pos + 2) : line;
	}

	// "START version=2026.10.13.1226 file=… windows=10.0.19045" 里取某一项
	std::wstring fieldOf(const std::wstring& line, const std::wstring& key)
	{
		auto pos = line.find(key + L"=");
		if (pos == std::wstring::npos) return L"";
		pos += key.size() + 1;
		auto end = line.find(L' ', pos);
		return line.substr(pos, end == std::wstring::npos ? end : end - pos);
	}

	// 从日志里找出 from（字节偏移）之后新出现的崩溃和错误。size 带回这次读到的日志有多大
	std::vector<Item> scan(const std::filesystem::path& path, uintmax_t from, uintmax_t& size)
	{
		std::vector<Item> items;
		std::ifstream file{ path, std::ios::binary };
		if (!file) return items;
		std::stringstream buffer;
		buffer << file.rdbuf();
		const std::string bytes = buffer.str();
		size = bytes.size();
		if (from > size) from = 0;   //日志被换过新的了（太大时会挪成 log.old.txt），从头看
		// 按行拆，记下每一行是不是"新的"（在 from 之后）
		struct Line {
			std::wstring text;
			bool fresh;
		};
		std::vector<Line> lines;
		size_t start{ 0 };
		while (start < bytes.size()) {
			auto end = bytes.find('\n', start);
			if (end == std::string::npos) end = bytes.size();
			auto text = fromUtf8(bytes.substr(start, end - start));
			while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
			lines.push_back({ std::move(text), start >= from });
			start = end + 1;
		}
		std::wstring version, os;
		Item errors;   //普通错误并成一条
		int errorCount{ 0 };
		for (size_t i = 0; i < lines.size(); i++) {
			const auto body = bodyOf(lines[i].text);
			if (body.starts_with(L"START ")) {
				// 往后的记录都是这一版程序留下的
				version = fieldOf(body, L"version");
				os = L"windows=" + fieldOf(body, L"windows");
				continue;
			}
			if (!lines[i].fresh) continue;
			if (body.starts_with(L"CRASH ")) {
				// 崩溃：这一行加上后面的调用栈（不以 "[" 开头的那些行），再带上之前的几十行，看得出崩之前在干什么
				Item item{ L"crash", version, os, body, L"" };
				const size_t ctxFrom = i > 60 ? i - 60 : 0;
				for (size_t k = ctxFrom; k < i; k++) item.detail += lines[k].text + L"\n";
				item.detail += L"\n>>> " + lines[i].text + L"\n";
				size_t k = i + 1;
				for (; k < lines.size() && !lines[k].text.starts_with(L"["); k++) item.detail += lines[k].text + L"\n";
				i = k - 1;
				if (items.size() < 5) items.push_back(std::move(item));
				continue;
			}
			// 网络不通这类不是程序的毛病（断网时每半分钟一条），不传；自己上传失败的也不传
			if (body.starts_with(L"ERROR ") && !body.starts_with(L"ERROR punch ") && !body.starts_with(L"ERROR report")
				&& !body.starts_with(L"ERROR sync ")) {
				if (errorCount == 0) {
					errors = Item{ L"error", version, os, body, L"" };
				}
				if (errorCount < 80) errors.detail += lines[i].text + L"\n";
				errorCount++;
			}
		}
		if (errorCount > 0) {
			if (errorCount > 80) errors.detail += std::format(L"…… 共 {} 条，只带了前 80 条\n", errorCount);
			items.push_back(std::move(errors));
		}
		return items;
	}

	// 传一条。连不上、服务器不收都算失败
	bool post(const HttpClient& client, const Item& item, const std::wstring& user, const std::wstring& machine)
	{
		JsonObject body;
		auto put = [&](const wchar_t* key, const std::wstring& value) { body.SetNamedValue(key, JsonValue::CreateStringValue(value)); };
		put(L"version", item.version.empty() ? Update::version() : item.version);
		put(L"kind", item.kind);
		put(L"summary", item.summary);
		put(L"detail", item.detail);
		put(L"os", item.os);
		put(L"user", user);
		put(L"machine", machine);
		HttpRequestMessage req{ HttpMethod::Post(), Uri{ reportUrl } };
		req.Content(HttpStringContent{ body.Stringify(), UnicodeEncoding::Utf8, L"application/json" });
		auto op = client.SendRequestAsync(req);
		if (op.wait_for(std::chrono::seconds(15)) == winrt::Windows::Foundation::AsyncStatus::Started) {
			op.Cancel();
			return false;
		}
		const auto code = static_cast<int>(op.GetResults().StatusCode());
		// 429（太频繁）下次再传；其余的 4xx 是这条内容服务器不收，再传也一样，当它传过了
		return code == 200 || (code >= 400 && code < 500 && code != 429);
	}

	winrt::fire_and_forget run(std::filesystem::path dir, std::wstring user, std::wstring machine)
	{
		co_await winrt::resume_background();
		try {
			const auto logPath = dir / L"log.txt", posPath = dir / L"report.pos";
			uintmax_t from{ 0 };
			if (std::ifstream posFile{ posPath }; posFile) {
				posFile >> from;
			}
			else {
				// 第一次用上这个功能：日志里以前的那些（多半是早就修掉的老问题）不传，从现在开始算
				std::error_code ec;
				const auto cur = std::filesystem::file_size(logPath, ec);
				std::ofstream first{ posPath, std::ios::trunc };
				first << (ec ? 0 : cur);
				busy = false;
				co_return;
			}
			uintmax_t size{ 0 };
			auto items = scan(logPath, from, size);
			bool ok{ true };
			if (!items.empty()) {
				HttpClient client;
				for (auto& item : items) {
					if (!post(client, item, user, machine)) {
						ok = false;
						break;
					}
				}
			}
			// 都传上去了（或者本来就没有新的）才往前挪；没传成的下次再来
			if (ok && size != from) {
				std::ofstream posFile{ posPath, std::ios::trunc };
				posFile << size;
			}
		}
		catch (...) {
			Log::write(L"ERROR report: " + Log::describe());
		}
		busy = false;
	}

	void CALLBACK onTimer(HWND, UINT, UINT_PTR id, DWORD)
	{
		// 第一次是启动后几秒，之后改成半小时一次
		static bool first{ true };
		if (first) {
			first = false;
			KillTimer(nullptr, id);
			SetTimer(nullptr, 0, 30 * 60 * 1000, onTimer);
		}
		if (busy) return;
		busy = true;
		// 是谁的电脑：团队 / 打卡用的名字、登录的账号、电脑名。这些只能在 UI 线程上取
		std::wstring user;
		if (auto punch = Punch::get(); punch && punch->isJoined()) user = punch->getTeam() + L"/" + punch->getName();
		if (auto account = Setting::get()->getAccountName(); !account.empty()) user += (user.empty() ? L"" : L" ") + (L"(" + account + L")");
		wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
		DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
		GetComputerName(name, &len);
		run(Setting::get()->getDataPath(), user, name);
	}
}

void Report::init()
{
	SetTimer(nullptr, 0, 8000, onTimer);
}
