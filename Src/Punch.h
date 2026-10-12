#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// 打卡：上班 / 下班，以及上班期间的各项活动（吃饭、上厕所、抽烟、中途离岗……）和回座。
// 现在是单机版：记录存在数据目录的 punch 文件夹里，一天一个文件。
// 对外的接口按"将来要和服务端同步"来设计 —— 界面只跟这个类打交道，
// 以后把记录上传、规则下发接进来时，界面不用动。
// 所有成员只在 UI 线程上用。时间一律是毫秒时间戳（1970 年以来）。
class Punch
{
public:
	// 一项活动的规则。limitMin / maxCount 为 0 表示不限
	struct Activity {
		std::wstring id;      // eat / wc / smoke / leave ……
		std::wstring name;    // 显示出来的名字
		int limitMin{ 0 };    // 单次最多几分钟，超了算超时并提醒
		int maxCount{ 0 };    // 每天最多几次
	};
	// 一次活动：从离开到回座。end 为 0 表示还没回来
	struct Record {
		std::wstring act;
		long long start{ 0 }, end{ 0 };
	};
	// 一段上班：从上班打卡到下班打卡。out 为 0 表示还在上班
	struct Segment {
		long long in{ 0 }, out{ 0 };
	};
	// 某项活动今天的汇总
	struct Stat {
		int count{ 0 };
		long long seconds{ 0 };       // 总时长
		int overCount{ 0 };           // 超时了几次
		long long overSeconds{ 0 };   // 一共超了多久
	};
	enum class State { Off, Working, Away, Done };   // 没上班 / 上班中 / 活动中 / 已下班

	~Punch();
	static void init();
	static void dispose();
	static Punch* get();

	const std::vector<Activity>& getActivities() const { return activities; }
	const Activity* findActivity(const std::wstring& id) const;
	const std::vector<Record>& getRecords() const { return records; }
	const std::vector<Segment>& getSegments() const { return segments; }
	State getState() const;
	// 正在进行的那项活动；不在活动中返回 nullptr
	const Record* current() const;
	Stat getStat(const std::wstring& act) const;
	Stat getTotal() const;                 // 所有活动合计
	long long workSeconds() const;         // 今天上班的总时长（含活动）
	long long firstIn() const;             // 今天第一次上班的时间，没有返回 0
	long long lastOut() const;             // 今天最后一次下班的时间
	// 这一天是哪天（考勤日：按每日重置时间切，不按零点），如 2026-10-12
	std::wstring getDay() const { return day; }

	// 下面四个动作，做不了时返回 false 并把原因放进 why（已经是给用户看的文案）
	bool clockIn(std::wstring& why);
	bool clockOut(std::wstring& why);
	bool start(const std::wstring& act, std::wstring& why);
	bool back(std::wstring& why);

	static long long now();
	// 把秒数写成"1 小时 05 分"这种样子
	static std::wstring fmtDuration(long long seconds);
	// 毫秒时间戳 → "14:57"
	static std::wstring fmtClock(long long ms);
public:
	// 记录有变化（打了卡、换了一天）时调，界面靠它刷新
	std::function<void()> onChanged;
	// 活动超时了要提醒用户时调，参数是提醒的文案
	std::function<void(const std::wstring&)> onOvertime;
private:
	Punch();
	static void CALLBACK onTimer(HWND, UINT, UINT_PTR, DWORD);
	void tick();
	// 考勤日变了（过了每日重置时间）且人不在班上：收掉旧的一天，换成新的一天
	void rollDay();
	std::wstring calcDay() const;
	void loadConfig();
	void load();
	void save();
	void notify();
private:
	std::filesystem::path dir;
	std::vector<Activity> activities;
	// 每日重置时间（几点）。夜班要跨零点，所以"一天"不按零点切。默认早上 6 点
	int resetHour{ 6 };
	std::wstring day;
	std::vector<Segment> segments;
	std::vector<Record> records;
	UINT_PTR timer{ 0 };
	long long lastRemind{ 0 };   // 上次提醒超时是什么时候，免得刷屏
};
