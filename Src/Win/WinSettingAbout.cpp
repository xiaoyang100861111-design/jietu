#include "pch.h"
#include "../MsgBox.h"
#include "../Lang.h"
#include "WinSetting.h"
#include "../Theme.h"
#include "WinSettingAbout.h"
#include "../Util.h"
#include "../Update.h"

WinSettingAbout::WinSettingAbout(Ling::WinBase* parent):Ling::Node(parent)
{
    std::vector<std::wstring> keys = { L"version",L"project",L"update",L"exit" };
    for (auto& key : keys)
    {
        auto box = makeChild<Ling::Node>();
        box->setHeight(39.f);
        box->setFlexDirection(Ling::FlexDirection::Row);
        box->setAlignItems(Ling::Align::Center);

        auto label = box->makeChild<Ling::Label>();
        label->setColor(Theme::t(0x333333FF));
        label->setText(Lang::get(L"about." + key));
        label->setHeightPercent(100.f);
        label->setJustifyContent(Ling::Justify::Center);
        label->setFlexGrow(1.f);

        auto btn = box->makeChild<Ling::Button>();
        btn->setColor(Theme::t(0x333333FF));
        btn->setHoverColor(Theme::t(0x333333FF));
        btn->setId(key);
        if (key == L"version") {
            // 版本号就是这一版的编译时间
            btn->setText(Update::version());
        }
        else if (key == L"project") {
            btn->setText(L"UU截图");
        }
        else if (key == L"exit") {
            // 托盘图标现在左右键都是唤出面板，退出软件放在这里
            btn->setText(Lang::get(L"about.exitApp"));
            btn->setColor(Theme::t(0xE64340FF));
            btn->setHoverColor(Theme::t(0xE64340FF));
            btn->onClick.add([](Ling::Button*) {
                if (UUMessageBox(nullptr, Lang::get(L"about.exitAsk").data(), L"UU截图",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND) == IDYES) {
                    Ling::App::get()->quit(0);
                }
            });
        }
        else {
            btn->setText(Lang::get(L"about.checkUpdate"));
            btn->setColor(Theme::t(0x597ef7ff));
            btn->setHoverColor(Theme::t(0x597ef7ff));
            btn->onClick.add([](Ling::Button*) { Update::checkNow(); });
        }
        btn->setAlignItems(Ling::Align::FlexEnd);
        btn->setHeight(28.f);
        btn->setWidth(160.f);
        btn->setBg(0);
        btn->setHoverBg(0);
        btns.push_back(btn);

        auto border = makeChild<Ling::Node>();
        border->setHeight(1.f);
        border->setBg(Theme::c(0xE0E0E0FF));
    }
}

WinSettingAbout::~WinSettingAbout()
{

}
