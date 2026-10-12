#include "pch.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include "Punch.h"
#include "Setting.h"
#include "Lang.h"
#include "Log.h"

namespace {
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Web::Http;
	using namespace winrt::Windows::Web::Http::Filters;
	using winrt::Windows::Storage::Streams::UnicodeEncoding;

	std::unique_ptr<Punch> punch;
	constexpr std::wstring_view baseUrl{ L"https://dk.zx3777.com" };
	constexpr long long remindGapMs{ 120 * 1000 };   //超时之后每隔这么久再提醒一次

	// 整个进程共用一个 HTTP 客户端，一直留着（连接可以复用；故意不释放，理由同 Translate.cpp 里的那个）。
	// WinRT 的类型不让直接 new，包一层普通结构体再放到堆上
	struct ClientHolder {
		HttpClient client{ nullptr };
	};
	HttpClient& httpClient()
	{
		static ClientHolder* holder = []() {
			auto ptr = new ClientHolder();
			HttpBaseProtocolFilter filter;
			// 状态必须每次都真的去问服务端，不能吃本机缓存
			filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::NoCache);
			filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
			ptr->client = HttpClient{ filter };
			return ptr;
		}();
		return holder->client;
	}

	// 发一个请求。在后台线程上做，做完回到 UI 线程调 done。
	// status 是 HTTP 状态码，连都没连上时是 0（这时 text 里是错误原因）
	winrt::fire_and_forget send(bool post, std::wstring path, std::wstring body, std::wstring token,
		std::function<void(int status, std::wstring text)> done)
	{
		co_await winrt::resume_background();
		int status{ 0 };
		std::wstring text;
		try {
			HttpRequestMessage req{ post ? HttpMethod::Post() : HttpMethod::Get(), Uri{ std::wstring{ baseUrl } + path } };
			if (!token.empty()) req.Headers().TryAppendWithoutValidation(L"Authorization", L"Bearer " + token);
			if (post) req.Content(HttpStringContent{ body, UnicodeEncoding::Utf8, L"application/json" });
			auto op = httpClient().SendRequestAsync(req);
			if (op.wait_for(std::chrono::seconds(12)) == winrt::Windows::Foundation::AsyncStatus::Started) {
				op.Cancel();
				throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			auto resp = op.GetResults();
			auto read = resp.Content().ReadAsStringAsync();
			if (read.wait_for(std::chrono::seconds(12)) == winrt::Windows::Foundation::AsyncStatus::Started) {
				read.Cancel();
				throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
			}
			text = std::wstring{ read.GetResults() };
			status = static_cast<int>(resp.StatusCode());
		}
		catch (...) {
			status = 0;
			text = Log::describe();
			Log::write(L"ERROR punch " + path + L": " + text);
		}
		Ling::App::get()->dq.TryEnqueue([done, status, text]() { done(status, text); });
	}

	// 毫秒时间戳 → 本地时间
	bool toLocal(long long ms, tm& local)
	{
		time_t sec = static_cast<time_t>(ms / 1000);
		return localtime_s(&local, &sec) == 0;
	}

	long long localNow()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	// 服务端出错时返回的是 { "error": "给用户看的原因" }
	std::wstring errorOf(const std::wstring& json)
	{
		JsonObject obj{ nullptr };
		if (!JsonObject::TryParse(json, obj)) return L"";
		return std::wstring{ obj.GetNamedString(L"error", L"") };
	}
}

Punch::Punch()
{
	token = Setting::get()->getPunchToken();
	// 半分钟一次：刷新状态（管理员改了规则、替人补了卡都能跟上），顺便看活动超时没有
	timer = SetTimer(nullptr, 0, 30000, onTimer);
	refresh();
}

Punch::~Punch()
{
	if (timer) KillTimer(nullptr, timer);
}

void Punch::init()
{
	if (!punch) punch.reset(new Punch());
}

void Punch::dispose()
{
	punch.reset();
}

Punch* Punch::get()
{
	return punch.get();
}

long long Punch::now() const
{
	return localNow() + skew;
}

std::wstring Punch::fmtDuration(long long seconds)
{
	if (seconds < 0) seconds = 0;
	const long long hours = seconds / 3600, minutes = (seconds % 3600) / 60;
	if (hours > 0) return std::format(L"{} {} {:02} {}", hours, Lang::get(L"punch.hour"), minutes, Lang::get(L"punch.minute"));
	if (minutes > 0) return std::format(L"{} {}", minutes, Lang::get(L"punch.minute"));
	return std::format(L"{} {}", seconds, Lang::get(L"punch.second"));
}

std::wstring Punch::fmtClock(long long ms)
{
	tm local{};
	if (ms <= 0 || !toLocal(ms, local)) return L"--:--";
	return std::format(L"{:02}:{:02}", local.tm_hour, local.tm_min);
}

const Punch::Activity* Punch::findActivity(const std::wstring& id) const
{
	for (auto& item : activities) {
		if (item.id == id) return &item;
	}
	return nullptr;
}

Punch::State Punch::getState() const
{
	if (segments.empty()) return State::Off;
	if (segments.back().out != 0) return State::Done;
	return current() ? State::Away : State::Working;
}

const Punch::Record* Punch::current() const
{
	if (records.empty() || records.back().end != 0) return nullptr;
	return &records.back();
}

Punch::Stat Punch::getStat(const std::wstring& act) const
{
	Stat stat;
	const auto cur = now();
	for (auto& record : records) {
		if (!act.empty() && record.act != act) continue;
		const long long seconds = ((record.end != 0 ? record.end : cur) - record.start) / 1000;
		stat.count++;
		stat.seconds += (std::max)(0LL, seconds);
		auto rule = findActivity(record.act);
		if (rule && rule->limitMin > 0 && seconds > rule->limitMin * 60LL) {
			stat.overCount++;
			stat.overSeconds += seconds - rule->limitMin * 60LL;
		}
	}
	return stat;
}

Punch::Stat Punch::getTotal() const
{
	return getStat(L"");
}

long long Punch::workSeconds() const
{
	long long total{ 0 };
	const auto cur = now();
	for (auto& seg : segments) total += ((seg.out != 0 ? seg.out : cur) - seg.in) / 1000;
	return (std::max)(0LL, total);
}

long long Punch::firstIn() const
{
	return segments.empty() ? 0 : segments.front().in;
}

long long Punch::lastOut() const
{
	return segments.empty() ? 0 : segments.back().out;
}

// 服务端的状态：{ now, day, team, name, state, activities:[{key,name,limitMin,maxCount}],
//               segments:[{in,out}], records:[{act,start,end}] }
bool Punch::apply(const std::wstring& json)
{
	JsonObject obj{ nullptr };
	if (!JsonObject::TryParse(json, obj) || !obj.HasKey(L"segments")) return false;
	skew = static_cast<long long>(obj.GetNamedNumber(L"now", 0)) - localNow();
	day = std::wstring{ obj.GetNamedString(L"day", L"") };
	team = std::wstring{ obj.GetNamedString(L"team", L"") };
	name = std::wstring{ obj.GetNamedString(L"name", L"") };
	activities.clear();
	segments.clear();
	records.clear();
	if (auto arr = obj.GetNamedArray(L"activities", nullptr)) {
		for (uint32_t i = 0; i < arr.Size(); i++) {
			if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
			auto item = arr.GetObjectAt(i);
			Activity rule;
			rule.id = std::wstring{ item.GetNamedString(L"key", L"") };
			rule.name = std::wstring{ item.GetNamedString(L"name", L"") };
			rule.limitMin = static_cast<int>(item.GetNamedNumber(L"limitMin", 0));
			rule.maxCount = static_cast<int>(item.GetNamedNumber(L"maxCount", 0));
			if (!rule.id.empty()) activities.push_back(std::move(rule));
		}
	}
	if (auto arr = obj.GetNamedArray(L"segments", nullptr)) {
		for (uint32_t i = 0; i < arr.Size(); i++) {
			if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
			auto item = arr.GetObjectAt(i);
			Segment seg;
			seg.in = static_cast<long long>(item.GetNamedNumber(L"in", 0));
			seg.out = static_cast<long long>(item.GetNamedNumber(L"out", 0));
			if (seg.in > 0) segments.push_back(seg);
		}
	}
	if (auto arr = obj.GetNamedArray(L"records", nullptr)) {
		for (uint32_t i = 0; i < arr.Size(); i++) {
			if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
			auto item = arr.GetObjectAt(i);
			Record record;
			record.act = std::wstring{ item.GetNamedString(L"act", L"") };
			record.start = static_cast<long long>(item.GetNamedNumber(L"start", 0));
			record.end = static_cast<long long>(item.GetNamedNumber(L"end", 0));
			if (!record.act.empty() && record.start > 0) records.push_back(std::move(record));
		}
	}
	loaded = true;
	return true;
}

void Punch::dropToken()
{
	token.clear();
	loaded = false;
	team.clear();
	name.clear();
	activities.clear();
	segments.clear();
	records.clear();
	Setting::get()->setPunchToken(L"");
}

void Punch::join(const std::wstring& code, const std::wstring& memberName, Done done)
{
	if (busy) return;
	busy = true;
	JsonObject body;
	body.SetNamedValue(L"code", JsonValue::CreateStringValue(code));
	body.SetNamedValue(L"name", JsonValue::CreateStringValue(memberName));
	send(true, L"/api/join", std::wstring{ body.Stringify() }, L"", [done](int status, std::wstring text) {
		if (!punch) return;
		punch->busy = false;
		JsonObject obj{ nullptr };
		std::wstring newToken;
		if (status == 200 && JsonObject::TryParse(text, obj)) newToken = std::wstring{ obj.GetNamedString(L"token", L"") };
		if (newToken.empty()) {
			auto why = status == 0 ? Lang::get(L"punch.errNet") + L"\n" + text : errorOf(text);
			if (why.empty()) why = std::format(L"{} ({})", Lang::get(L"punch.errServer"), status);
			punch->notify();
			if (done) done(false, why);
			return;
		}
		punch->token = newToken;
		Setting::get()->setPunchToken(newToken);
		Log::write(L"punch: joined a team");
		punch->refresh();
		if (done) done(true, L"");
	});
	notify();
}

void Punch::leave()
{
	dropToken();
	notify();
}

void Punch::refresh()
{
	if (token.empty() || busy) return;
	send(false, L"/api/state", L"", token, [](int status, std::wstring text) {
		if (!punch) return;
		if (status == 401) {
			punch->dropToken();   //令牌不认了：被管理员停用或删除了
		}
		else if (status != 200 || !punch->apply(text)) {
			return;               //这次没取到，界面上接着显示上一次的，下一轮再试
		}
		punch->notify();
	});
}

void Punch::act(const std::wstring& action, const std::wstring& actId, Done done)
{
	if (busy || token.empty()) return;
	busy = true;
	JsonObject body;
	body.SetNamedValue(L"action", JsonValue::CreateStringValue(action));
	body.SetNamedValue(L"act", JsonValue::CreateStringValue(actId));
	send(true, L"/api/punch", std::wstring{ body.Stringify() }, token, [done](int status, std::wstring text) {
		if (!punch) return;
		punch->busy = false;
		std::wstring why;
		if (status == 0) {
			// 没连上服务端：卡没打上，明确告诉用户，不在本机偷偷记一笔
			why = Lang::get(L"punch.errNet") + L"\n" + text;
		}
		else if (status == 401) {
			punch->dropToken();
			why = Lang::get(L"punch.errAuth");
		}
		else {
			// 成功和"规则不允许"都会带回最新的状态；后者同时带着 error
			JsonObject obj{ nullptr };
			if (JsonObject::TryParse(text, obj)) {
				if (auto st = obj.GetNamedObject(L"state", nullptr)) punch->apply(std::wstring{ st.Stringify() });
				why = std::wstring{ obj.GetNamedString(L"error", L"") };
			}
			if (why.empty() && status != 200) why = std::format(L"{} ({})", Lang::get(L"punch.errServer"), status);
		}
		punch->lastRemind = 0;
		punch->notify();
		if (done) done(why.empty(), why);
	});
	notify();
}

void Punch::api(bool post, const std::wstring& path, const std::wstring& body,
	std::function<void(int status, std::wstring text)> done)
{
	send(post, path, body, punch ? punch->token : L"", done);
}

void CALLBACK Punch::onTimer(HWND, UINT, UINT_PTR, DWORD)
{
	if (punch) punch->tick();
}

void Punch::tick()
{
	refresh();
	auto cur = current();
	if (!cur) return;
	auto rule = findActivity(cur->act);
	if (!rule || rule->limitMin <= 0) return;
	const auto t = now();
	const long long over = (t - cur->start) / 1000 - rule->limitMin * 60LL;
	if (over <= 0 || t - lastRemind < remindGapMs) return;
	lastRemind = t;
	if (onOvertime) {
		onOvertime(std::format(L"{}{} {}，{}", rule->name, Lang::get(L"punch.overtime"), fmtDuration(over), Lang::get(L"punch.pleaseBack")));
	}
}

void Punch::notify()
{
	if (onChanged) onChanged();
}
