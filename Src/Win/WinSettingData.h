#pragma once
#include <include/Ling.h>
// 设置窗口里的"配置目录"页：把存放配置、话术、剪切板历史的那个文件夹的路径摆出来
// （在文本框里，可以选中复制），旁边一个按钮直接用资源管理器打开它
class WinSettingData :public Ling::Node
{
public:
	WinSettingData(Ling::WinBase* parent);
	~WinSettingData();
};
