#include "pch.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include "Punch.h"
#include "Setting.h"
#include "Lang.h"
#include "Log.h"

namespace {
	std::unique_ptr<Punch> punch;
	constexpr long long remindGapMs{ 120 * 1000 };   //超时之后每隔这么久再提醒一次

	// 毫秒时间戳 → 本地时间
	bool toLocal(long long ms, tm& local)
	{
		time_t sec = static_cast<time_t>(ms / 1000);
		return localtime_s(&local, &sec) == 0;
	}
}

Punch::Punch()
{
	dir = Setting::get()->getDataPath() / L"punch";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	loadConfig();
	day = calcDay();
	load();
	// 半分钟看一眼：活动超时没有、是不是该换一天了
	timer = SetTimer(nullptr, 0, 30000, onTimer);
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

long long Punch::now()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
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

// 考勤日：把现在的时间往回拨"每日重置时间"那么多小时，再取日期。
// 这样凌晨 4 点下班的夜班，算的还是前一天
std::wstring Punch::calcDay() const
{
	tm local{};
	if (!toLocal(now() - resetHour * 3600LL * 1000LL, local)) return L"";
	return std::format(L"{:04}-{:02}-{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
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

bool Punch::clockIn(std::wstring& why)
{
	rollDay();
	if (getState() == State::Working || getState() == State::Away) {
		why = Lang::get(L"punch.errWorking");
		return false;
	}
	segments.push_back(Segment{ now(), 0 });
	save();
	notify();
	return true;
}

bool Punch::clockOut(std::wstring& why)
{
	auto state = getState();
	if (state != State::Working && state != State::Away) {
		why = Lang::get(L"punch.errNotWorking");
		return false;
	}
	const auto cur = now();
	// 活动中直接下班：这项活动算到下班这一刻为止
	if (!records.empty() && records.back().end == 0) records.back().end = cur;
	segments.back().out = cur;
	save();
	notify();
	return true;
}

bool Punch::start(const std::wstring& act, std::wstring& why)
{
	auto state = getState();
	if (state == State::Away) {
		why = Lang::get(L"punch.errAway");
		return false;
	}
	if (state != State::Working) {
		why = Lang::get(L"punch.errNotWorking");
		return false;
	}
	auto rule = findActivity(act);
	if (!rule) return false;
	if (rule->maxCount > 0 && getStat(act).count >= rule->maxCount) {
		why = std::format(L"{}{}{} {} {}", rule->name, Lang::get(L"punch.errMaxCount"), Lang::get(L"punch.limit"),
			rule->maxCount, Lang::get(L"punch.times"));
		return false;
	}
	records.push_back(Record{ act, now(), 0 });
	lastRemind = 0;
	save();
	notify();
	return true;
}

bool Punch::back(std::wstring& why)
{
	if (getState() != State::Away) {
		why = Lang::get(L"punch.errNotAway");
		return false;
	}
	records.back().end = now();
	save();
	notify();
	return true;
}

void CALLBACK Punch::onTimer(HWND, UINT, UINT_PTR, DWORD)
{
	if (punch) punch->tick();
}

void Punch::tick()
{
	rollDay();
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

void Punch::rollDay()
{
	auto today = calcDay();
	if (today.empty() || today == day) return;
	// 人还在班上（夜班拖过了重置时间）：这一天先不结，等下班之后再换
	auto state = getState();
	if (state == State::Working || state == State::Away) return;
	day = today;
	segments.clear();
	records.clear();
	load();
	notify();
}

// 活动规则。现在是本机的 punch/config.json，没有就用默认的那几项；
// 将来改成由管理后台下发
void Punch::loadConfig()
{
	activities = {
		{ L"eat", Lang::get(L"punch.actEat"), 30, 3 },
		{ L"wc", Lang::get(L"punch.actWc"), 10, 0 },
		{ L"smoke", Lang::get(L"punch.actSmoke"), 10, 0 },
		{ L"leave", Lang::get(L"punch.actLeave"), 15, 0 },
	};
	auto content = Ling::Util::readFileText(dir / L"config.json");
	JsonObject root{ nullptr };
	if (content.empty() || !JsonObject::TryParse(content, root)) return;
	resetHour = std::clamp(static_cast<int>(root.GetNamedNumber(L"resetHour", 6)), 0, 23);
	auto arr = root.GetNamedArray(L"activities", nullptr);
	if (!arr || arr.Size() == 0) return;
	std::vector<Activity> list;
	for (uint32_t i = 0; i < arr.Size(); i++) {
		if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
		auto obj = arr.GetObjectAt(i);
		Activity item;
		item.id = std::wstring{ obj.GetNamedString(L"id", L"") };
		item.name = std::wstring{ obj.GetNamedString(L"name", L"") };
		item.limitMin = (std::max)(0, static_cast<int>(obj.GetNamedNumber(L"limitMin", 0)));
		item.maxCount = (std::max)(0, static_cast<int>(obj.GetNamedNumber(L"maxCount", 0)));
		if (!item.id.empty() && !item.name.empty()) list.push_back(std::move(item));
	}
	if (!list.empty()) activities = std::move(list);
}

void Punch::load()
{
	auto content = Ling::Util::readFileText(dir / (day + L".json"));
	JsonObject root{ nullptr };
	if (content.empty() || !JsonObject::TryParse(content, root)) return;
	if (auto arr = root.GetNamedArray(L"segments", nullptr)) {
		for (uint32_t i = 0; i < arr.Size(); i++) {
			if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
			auto obj = arr.GetObjectAt(i);
			Segment seg;
			seg.in = static_cast<long long>(obj.GetNamedNumber(L"in", 0));
			seg.out = static_cast<long long>(obj.GetNamedNumber(L"out", 0));
			if (seg.in > 0) segments.push_back(seg);
		}
	}
	if (auto arr = root.GetNamedArray(L"records", nullptr)) {
		for (uint32_t i = 0; i < arr.Size(); i++) {
			if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
			auto obj = arr.GetObjectAt(i);
			Record record;
			record.act = std::wstring{ obj.GetNamedString(L"act", L"") };
			record.start = static_cast<long long>(obj.GetNamedNumber(L"start", 0));
			record.end = static_cast<long long>(obj.GetNamedNumber(L"end", 0));
			if (!record.act.empty() && record.start > 0) records.push_back(std::move(record));
		}
	}
}

void Punch::save()
{
	JsonArray segArr, recArr;
	for (auto& seg : segments) {
		JsonObject obj;
		obj.SetNamedValue(L"in", JsonValue::CreateNumberValue(static_cast<double>(seg.in)));
		obj.SetNamedValue(L"out", JsonValue::CreateNumberValue(static_cast<double>(seg.out)));
		segArr.Append(obj);
	}
	for (auto& record : records) {
		JsonObject obj;
		obj.SetNamedValue(L"act", JsonValue::CreateStringValue(record.act));
		obj.SetNamedValue(L"start", JsonValue::CreateNumberValue(static_cast<double>(record.start)));
		obj.SetNamedValue(L"end", JsonValue::CreateNumberValue(static_cast<double>(record.end)));
		recArr.Append(obj);
	}
	JsonObject root;
	root.SetNamedValue(L"day", JsonValue::CreateStringValue(day));
	root.SetNamedValue(L"segments", segArr);
	root.SetNamedValue(L"records", recArr);
	Ling::Util::saveFile((dir / (day + L".json")).wstring(), std::wstring{ root.Stringify() });
}

void Punch::notify()
{
	if (onChanged) onChanged();
}
