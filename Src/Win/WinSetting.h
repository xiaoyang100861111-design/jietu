#pragma once
#include <include/Ling.h>

class WinSetting :public Ling::WinBase
{
public:
	~WinSetting();
	static void init();
	// 设置窗口开着的话把它弄到最前面来（最小化了就还原），并闪两下提醒位置；没开就什么都不做。
	// 点托盘图标时调：窗口常常被别的窗口盖住了、或者缩在任务栏里，用户找不到
	static void raise();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
private:
	WinSetting();
	void initMenuItems(Ling::Node* menuBox);
	void onCreated() override;
	void onMenuItemClick(Ling::Button* menu);
	LRESULT onHitTest(const POINT pos) override;
	// 顶上那条状态栏右边的字：有没有新版本。查到结果后刷新
	void refreshUpdateBtn();
private:
	std::vector<Ling::Button*> menus;
	int menuIndex{ 0 };
	Ling::Node* content{nullptr};
	Ling::Button* updateBtn{ nullptr };
};

