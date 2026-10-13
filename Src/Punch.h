#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>
#include <vector>

// 打卡：上班 / 下班，以及上班期间的各项活动（吃饭、上厕所、抽烟、中途离岗……）和回座。
// 数据全在服务端（https://dk.zx3777.com，和网页版打卡是同一套）：
//   - 先用管理员给的邀请码加入团队，拿到一个令牌存在本机配置里；
//   - 之后每次打卡都是向服务端提交，以服务端的时间为准，后台实时看得到；
//   - 活动项、限时限次这些规则由管理后台下发。
// 这个类手里只是服务端状态的一份缓存，隔一会儿刷新一次。
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
	// 一次请求做完之后的回调：成没成，没成的话给用户看的原因
	using Done = std::function<void(bool ok, const std::wstring& why)>;

	~Punch();
	static void init();
	static void dispose();
	static Punch* get();

	// 加没加入团队（本机有没有令牌）
	bool isJoined() const { return !token.empty(); }
	// 是不是重要岗位（后台给他设了岗位，或者是记账小组的负责人）：面板上多一个"管理后台"入口
	bool isManager() const { return isJoined() && manager; }
	// 加入了，并且已经从服务端拿到过一次状态
	bool isLoaded() const { return loaded; }
	// 正有一个请求在路上（这期间按钮不让再点）
	bool isBusy() const { return busy; }
	const std::wstring& getTeam() const { return team; }
	const std::wstring& getName() const { return name; }

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
	// 这一天是哪天（考勤日，由服务端按每日重置时间算），如 2026-10-12
	std::wstring getDay() const { return day; }

	// 用邀请码加入团队
	void join(const std::wstring& code, const std::wstring& memberName, Done done);
	// 退出团队：只是把本机的令牌丢掉，服务端的记录都还在；之后要重新填邀请码
	void leave();
	// 从服务端重新取一次状态（面板打开时、定时都会调）
	void refresh();
	// 打卡。action：in 上班 / out 下班 / start 开始一项活动（act 是活动 id）/ back 回座
	void act(const std::wstring& action, const std::wstring& actId, Done done);

	// 用打卡的团队身份（令牌）调服务端的其他接口，记账页用它。做完回到 UI 线程调 done：
	// status 是 HTTP 状态码，没连上是 0（这时 text 是错误原因）
	static void api(bool post, const std::wstring& path, const std::wstring& body,
		std::function<void(int status, std::wstring text)> done);
	// 现在的时间，按服务端的钟（本机时间不准也没关系）
	long long now() const;
	// 把秒数写成"1 小时 05 分"这种样子
	static std::wstring fmtDuration(long long seconds);
	// 毫秒时间戳 → "14:57"
	static std::wstring fmtClock(long long ms);
public:
	// 状态有变化（打了卡、刷新到了新数据、加入 / 退出了团队）时调，界面靠它重画
	std::function<void()> onChanged;
	// 活动超时了要提醒用户时调，参数是提醒的文案
	std::function<void(const std::wstring&)> onOvertime;
private:
	Punch();
	static void CALLBACK onTimer(HWND, UINT, UINT_PTR, DWORD);
	void tick();
	// 把服务端给的状态（JSON）装进来。格式不对返回 false
	bool apply(const std::wstring& json);
	// 服务端说令牌不认了（被管理员停用、删除）：丢掉令牌，回到"未加入"
	void dropToken();
	void notify();
private:
	std::wstring token, team, name, day, state;
	bool manager{ false };
	std::vector<Activity> activities;
	std::vector<Segment> segments;
	std::vector<Record> records;
	bool loaded{ false }, busy{ false };
	long long skew{ 0 };          // 服务端时间 - 本机时间
	UINT_PTR timer{ 0 };
	long long lastRemind{ 0 };    // 上次提醒超时是什么时候，免得刷屏
};
