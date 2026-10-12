#include "pch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Theme.h"
#include "WinSetting.h"
#include "WinSettingData.h"

WinSettingData::WinSettingData(Ling::WinBase* parent) :Ling::Node(parent)
{
    const auto path = Setting::get()->getDataPath().wstring();

    auto title = makeChild<Ling::Label>();
    title->setColor(Theme::t(0x333333FF));
    title->setText(Lang::get(L"setting.dataDirLabel"));
    title->setHeight(34.f);
    title->setJustifyContent(Ling::Justify::Center);

    auto box = makeChild<Ling::Node>();
    box->setHeight(36.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto text = box->makeChild<Ling::TextBox>();
    text->setColor(Theme::t(0x333333FF));
    text->setHeight(30.f);
    text->setFlexGrow(1.f);
    text->setPadding(8.f, 2.f, 8.f, 2.f);
    text->setBg(Theme::c(0xFFFFFFFF));
    text->setBorder(1.f, Theme::c(0xE0E0E0FF));
    text->setFontSize(13.f);
    text->setVerticalCenter(true);
    text->setText(path);

    auto makeBtn = [box](const std::wstring& label) {
        auto btn = box->makeChild<Ling::Button>();
        btn->setColor(Theme::t(0x333333FF));
        btn->setHoverColor(Theme::t(0x333333FF));
        btn->setText(label);
        btn->setHeight(30.f);
        btn->setWidth(72.f);
        btn->setMarginLeft(8.f);
        btn->setBg(Theme::c(0xFFFFFFFF));
        btn->setHoverBg(Theme::c(0xF2F2F2FF));
        btn->setBorder(1.f, Theme::c(0xE0E0E0FF));
        return btn;
    };
    // 路径在回调里重新取，不捕获：免得和这个节点的生命期纠缠
    makeBtn(Lang::get(L"setting.dataDirOpen"))->onClick.add([](Ling::Button* btn) {
        ShellExecute(btn->win->hwnd, L"open", Setting::get()->getDataPath().wstring().data(), nullptr, nullptr, SW_SHOWNORMAL);
    });
    makeBtn(Lang::get(L"setting.dataDirCopy"))->onClick.add([](Ling::Button* btn) {
        Ling::Util::setTextToClipboard(Setting::get()->getDataPath().wstring());
        btn->setText(Lang::get(L"setting.dataDirCopied"));
    });

    auto tip = makeChild<Ling::Label>();
    tip->setColor(Theme::t(0x333333FF));
    tip->setText(Lang::get(L"setting.dataDirTip"));
    tip->setHeight(34.f);
    tip->setJustifyContent(Ling::Justify::Center);
    tip->setFontSize(12.f);
    tip->setColor(Theme::t(0x999999FF));
}

WinSettingData::~WinSettingData()
{
}
