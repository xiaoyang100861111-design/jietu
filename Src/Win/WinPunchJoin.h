#pragma once
#include <include/Ling.h>

// 打卡：第一次用时填邀请码和姓名、加入团队的小窗口。
// 提交给服务端，成功了就关掉自己（打卡页会自己刷新成正常的样子），失败了把原因显示在窗口里。
class WinPunchJoin : public Ling::WinBase
{
public:
	~WinPunchJoin();
	static void open();
	static void dispose();
private:
	WinPunchJoin();
	void onCreated() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 顶上那一条当标题栏用，可以拖着挪窗口
	LRESULT onHitTest(const POINT pos) override;
	void submit();
private:
	Ling::TextBox* codeBox{ nullptr };
	Ling::TextBox* nameBox{ nullptr };
	Ling::Label* tipLabel{ nullptr };
};
