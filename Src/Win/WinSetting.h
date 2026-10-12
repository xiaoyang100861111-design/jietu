#pragma once

// 设置已经并进面板（WinClip）里了：点面板右上角的齿轮、托盘菜单的"设置"，都是在面板里显示设置内容，
// 不再有单独的设置窗口。这个类只留一个入口，原来到处调的 WinSetting::init() 不用改
class WinSetting
{
public:
	// 把面板叫出来并切到设置（面板没开就开一个）
	static void init();
};
