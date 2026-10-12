#pragma once
#include <include/Ling.h>
// 设置窗口里的"账号"页。
// 没登录：账号、密码两个输入框，登录 / 注册两个按钮；
// 登录着：显示账号名，"立即同步"和"退出登录"。
// 登录后软件的配置和快捷话术会存到服务端，换电脑登录同一个账号就能拿回来（见 Sync）。
class WinSettingAccount :public Ling::Node
{
public:
	WinSettingAccount(Ling::WinBase* parent);
	~WinSettingAccount();
private:
	void buildLogin();
	void buildProfile();
	void submit(bool isRegister);
	// 页面上那行说明 / 结果。warn 用红字
	void tip(const std::wstring& text, bool warn);
private:
	Ling::TextBox* userBox{ nullptr };
	Ling::TextBox* pwdBox{ nullptr };
	Ling::Label* tipLabel{ nullptr };
};
