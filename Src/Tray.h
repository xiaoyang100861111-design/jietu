#pragma once
#include <include/Ling.h>

class Tray
{
public:
	~Tray();
	static void init();
	static Tray* get();
	// 3 秒后截图：留出时间让用户把右键菜单、下拉框这些一碰键盘就消失的东西先弹出来
	static void delayCapture();
private:
	Tray();
	void onTrayRightClick();
};
