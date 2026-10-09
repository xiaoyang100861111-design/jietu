#pragma once
#include <include/Ling.h>
#include "../ClipHistory.h"

// 添加 / 编辑一条快捷话术的小窗口：一个备注框、一个内容框。
// 内容默认是自己打（或粘贴）的文字；要存图片、文件，先在别处复制好，再点"改用剪切板里的图片 / 文件"。
// 编辑已有的话术时，图片 / 文件类的只能改备注。
class WinPhraseEdit : public Ling::WinBase
{
public:
	~WinPhraseEdit();
	// group：新话术放进哪个分组（完整路径，空 = 未分组）。editId 非 0 表示编辑这一条
	static void open(const std::wstring& group, long long editId = 0);
	static void dispose();
private:
	WinPhraseEdit(const std::wstring& group, long long editId);
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 顶上那一条当标题栏用，可以拖着挪窗口
	LRESULT onHitTest(const POINT pos) override;
	void save();
	void useClipboard();
private:
	std::wstring group;
	long long editId{ 0 };
	// 编辑时那条话术的类型；新建时一开始是文字，点了"改用剪切板"之后变成剪切板里那样东西的类型
	ClipHistory::Type type{ ClipHistory::Type::Text };
	bool fromClip{ false };
	Ling::TextBox* titleBox{ nullptr };
	Ling::TextBox* textBox{ nullptr };
	Ling::Label* clipLabel{ nullptr };
};
