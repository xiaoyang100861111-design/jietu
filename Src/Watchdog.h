#pragma once
#include <include/Ling.h>

// 界面线程看门狗。
// 截图窗口的失败提示框曾把界面线程卡成残影：框还留在屏幕上，任务管理器结束调用方也没用，
// 只有结束本进程才消失。这里由一条后台线程每隔几秒往界面线程上的一个只收消息的窗口发一条
// 同步消息，连着几十秒都没人接就认定界面线程卡死了：把它当时的调用栈记进日志
//（ERROR 开头，Report 会传回服务器），然后结束进程 —— 留一个关不掉的窗口在桌面上更糟。
class Watchdog
{
public:
	// 在界面线程上调，要在 Log::init 之后（卡死时要写日志）。
	// 附着了调试器时不启用，不然断点一停就被当成卡死
	static void init();
	// 消息循环退出之后调：收尾阶段界面线程不再泵消息是正常的，别再盯着
	static void stop();
	// 界面线程要故意等一会儿（停录屏时等编码线程收尾：GIF 的 gifski 收尾、MP4 的 Finalize，
	// 长录像可能要几十秒）时先 pause，等完 resume：这期间不探测，之前数的次数也作废。
	// 是计数的，可以嵌套；必须成对，所有返回路径都得 resume —— 用下面的 Pause 最省心
	static void pause();
	static void resume();
	// 作用域内暂停：构造 pause、析构 resume，异常和提前 return 都不会漏掉 resume
	struct Pause {
		Pause() { Watchdog::pause(); }
		~Pause() { Watchdog::resume(); }
		Pause(const Pause&) = delete;
		Pause& operator=(const Pause&) = delete;
	};
};
