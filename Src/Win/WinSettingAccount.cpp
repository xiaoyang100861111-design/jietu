#include "pch.h"
#include "../MsgBox.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Theme.h"
#include "../Sync.h"
#include "WinSetting.h"
#include "WinSettingAccount.h"

namespace {
    // 现在显示着的那一页。异步请求回来时拿它判断页面还在不在（可能已经切到别的菜单、或者窗口关了）
    WinSettingAccount* current{ nullptr };

    // 文本框是多行的，这里只要一行：换行去掉，再掐掉两头的空白
    std::wstring oneLine(const std::wstring& text)
    {
        std::wstring result;
        for (auto c : text) {
            if (c != L'\r' && c != L'\n') result += c;
        }
        const auto from = result.find_first_not_of(L" \t");
        if (from == std::wstring::npos) return L"";
        const auto to = result.find_last_not_of(L" \t");
        return result.substr(from, to - from + 1);
    }

    // 登录状态变了：把设置窗口关掉重开一遍，页面就换成另一副样子了（同切换语言的做法）
    void reopen(Ling::WinBase* win)
    {
        win->close();
        Ling::App::get()->dq.TryEnqueue([]() { WinSetting::init(); });
    }
}

WinSettingAccount::WinSettingAccount(Ling::WinBase* parent) :Ling::Node(parent)
{
    current = this;
    if (Sync::isLoggedIn()) buildProfile();
    else buildLogin();
}

WinSettingAccount::~WinSettingAccount()
{
    if (current == this) current = nullptr;
}

void WinSettingAccount::tip(const std::wstring& text, bool warn)
{
    if (!tipLabel) return;
    tipLabel->setColor(warn ? Theme::t(0xE64340FF) : Theme::t(0x888888FF));
    tipLabel->setText(text);
}

void WinSettingAccount::buildLogin()
{
    auto makeLabel = [this](const std::wstring& text) {
        auto label = makeChild<Ling::Label>();
        label->setColor(Theme::t(0x333333FF));
        label->setText(text);
        label->setHeight(30.f);
        label->setJustifyContent(Ling::Justify::Center);
        label->setColor(Theme::t(0x666666FF));
    };
    auto makeBox = [this]() {
        auto box = makeChild<Ling::TextBox>();
        box->setColor(Theme::t(0x333333FF));
        box->setWidth(320.f);
        box->setHeight(34.f);
        box->setPadding(8.f, 2.f, 8.f, 2.f);
        box->setBg(Theme::c(0xFFFFFFFF));
        box->setBorder(1.f, Theme::c(0xD9D9D9FF));
        box->setFontSize(14.f);
        box->setVerticalCenter(true);
        return box;
    };
    makeLabel(Lang::get(L"sync.user"));
    userBox = makeBox();
    makeLabel(Lang::get(L"sync.pwd"));
    pwdBox = makeBox();

    auto row = makeChild<Ling::Node>();
    row->setHeight(52.f);
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setAlignItems(Ling::Align::Center);
    const uint32_t theme = (Setting::get()->getThemeColor() << 8) | 0xFF;
    auto loginBtn = row->makeChild<Ling::Button>();
    loginBtn->setColor(Theme::t(0x333333FF));
    loginBtn->setHoverColor(Theme::t(0x333333FF));
    loginBtn->setText(Lang::get(L"sync.login"));
    loginBtn->setSize(100.f, 32.f);
    loginBtn->setBg(theme);
    loginBtn->setHoverBg(theme);
    loginBtn->setColor(Theme::t(0xFFFFFFFF));
    loginBtn->setHoverColor(Theme::t(0xFFFFFFFF));
    loginBtn->onClick.add([this](Ling::Button*) { submit(false); });
    auto regBtn = row->makeChild<Ling::Button>();
    regBtn->setColor(Theme::t(0x333333FF));
    regBtn->setHoverColor(Theme::t(0x333333FF));
    regBtn->setText(Lang::get(L"sync.register"));
    regBtn->setSize(120.f, 32.f);
    regBtn->setMarginLeft(10.f);
    regBtn->setBg(Theme::c(0xFFFFFFFF));
    regBtn->setHoverBg(Theme::c(0xF2F2F2FF));
    regBtn->setBorder(1.f, Theme::c(0xD9D9D9FF));
    regBtn->onClick.add([this](Ling::Button*) { submit(true); });

    tipLabel = makeChild<Ling::Label>();
    tipLabel->setColor(Theme::t(0x333333FF));
    tipLabel->setHeight(120.f);
    tipLabel->setFontSize(12.f);
    tip(Lang::get(L"sync.intro"), false);
}

void WinSettingAccount::buildProfile()
{
    auto name = makeChild<Ling::Label>();
    name->setColor(Theme::t(0x333333FF));
    name->setText(Lang::get(L"sync.loggedIn") + Sync::username());
    name->setHeight(40.f);
    name->setJustifyContent(Ling::Justify::Center);
    name->setFontSize(15.f);

    auto row = makeChild<Ling::Node>();
    row->setHeight(52.f);
    row->setFlexDirection(Ling::FlexDirection::Row);
    row->setAlignItems(Ling::Align::Center);
    auto makeBtn = [row](const std::wstring& text, float left) {
        auto btn = row->makeChild<Ling::Button>();
        btn->setColor(Theme::t(0x333333FF));
        btn->setHoverColor(Theme::t(0x333333FF));
        btn->setText(text);
        btn->setSize(110.f, 32.f);
        btn->setMarginLeft(left);
        btn->setBg(Theme::c(0xFFFFFFFF));
        btn->setHoverBg(Theme::c(0xF2F2F2FF));
        btn->setBorder(1.f, Theme::c(0xD9D9D9FF));
        return btn;
    };
    makeBtn(Lang::get(L"sync.pushNow"), 0.f)->onClick.add([this](Ling::Button*) {
        tip(Lang::get(L"sync.working"), false);
        Sync::pushNow([](bool ok, const std::wstring& msg) {
            if (current && !msg.empty()) current->tip(msg, !ok);
        });
    });
    makeBtn(Lang::get(L"sync.logout"), 10.f)->onClick.add([this](Ling::Button*) {
        Sync::logout();
        reopen(win);
    });

    tipLabel = makeChild<Ling::Label>();
    tipLabel->setColor(Theme::t(0x333333FF));
    tipLabel->setHeight(120.f);
    tipLabel->setFontSize(12.f);
    tip(Lang::get(L"sync.introIn"), false);
}

void WinSettingAccount::submit(bool isRegister)
{
    const auto user = oneLine(userBox->getText());
    const auto pwd = oneLine(pwdBox->getText());
    if (user.empty() || pwd.empty()) {
        tip(Lang::get(L"sync.empty"), true);
        return;
    }
    tip(Lang::get(L"sync.working"), false);
    Sync::login(user, pwd, isRegister, [](bool ok, const std::wstring& msg) {
        if (!current) return;
        if (!ok) {
            current->tip(msg, true);
            return;
        }
        // 成功了：结果用弹框告诉用户（取回了服务端配置的话软件马上要重启，页面上的字来不及看）
        UUMessageBox(nullptr, msg.data(), L"UU截图", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        if (current) reopen(current->win);
    });
}
