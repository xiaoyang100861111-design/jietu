#include "pch.h"
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include "../Lang.h"
#include "../Setting.h"
#include "WinSetting.h"
#include "WinSettingCommon.h"

WinSettingCommon::WinSettingCommon(Ling::WinBase* parent):Ling::Node(parent)
{    
    initAutoStartCtrls();
    initLangCtrls();
    initThemeCtrls();
    initClipCtrls();
    initDockCtrls();
    initWidthCtrls(L"rect");
    initWidthCtrls(L"ellipse");
    auto weakThis = getWeakThis();
    // 这个回调一直挂在窗口上，而本节点可能在窗口关闭之前就被菜单切换换掉了，
    // 所以先确认自己还活着再去碰成员
    win->onDestroy.add([this, weakThis]() {
        if (!weakThis.lock()) return;
        this->hideSelectBox();
    });
}

WinSettingCommon::~WinSettingCommon()
{
    win->onMouseDown.remove(onMouseDownToken);
}

void WinSettingCommon::initAutoStartCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.autoStart"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto btn = box->makeChild<Ling::Button>();
    btn->setText(L"\ue687");
    btn->setFontFamily(L"icon");
    btn->setHeightPercent(100.f);
    btn->setFontSize(18.f);
    btn->setWidth(60.f);
    setAutoStartBtn(btn);

    btn->onClick.add([this](Ling::Button* btn) {
        auto setting = Setting::get();
        auto isAutoStart = setting->getAutoStart();
        setting->setAutoStart(!isAutoStart);
        setAutoStartBtn(btn);
    });

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

void WinSettingCommon::initLangCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.language"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto langCode = Setting::get()->getLang();
    auto langs = Lang::get()->getSupportedLang();
    std::wstring langName{ L"简体中文" };
    for (auto& pair:langs)
    {
        if (pair.second == langCode) {
            langName = pair.first;
            break;
        }
    }
    selectBtn = box->makeChild<Ling::Button>();
    selectBtn->setText(langName);
    selectBtn->setHeight(28.f);
    selectBtn->setWidth(160.f);
    selectBtn->setBorder(1.f, 0xE0E0E0FF);
    selectBtn->setHoverBg(0XFFFFFFFF);
    selectBtn->onClick.add([this](Ling::Button* btn) {
        if (selectBox) return;
        this->showSelectBox(btn);
        });
    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

// 快捷话术面板贴边：点一下按钮在 关闭 / 左 / 上 / 右 / 下 之间轮换
void WinSettingCommon::initDockCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.dock"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto btn = box->makeChild<Ling::Button>();
    btn->setText(Lang::get(std::format(L"setting.dock{}", Setting::get()->getDockEdge())));
    btn->setHeight(28.f);
    btn->setWidth(160.f);
    btn->setBorder(1.f, 0xE0E0E0FF);
    btn->setHoverBg(0XFFFFFFFF);
    btn->onClick.add([](Ling::Button* btn) {
        auto val = (Setting::get()->getDockEdge() + 1) % 5;
        Setting::get()->setDockEdge(val);
        btn->setText(Lang::get(std::format(L"setting.dock{}", val)));
    });

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

// 矩形 / 圆形的默认线条粗细。和标注工具条上那个滑块存的是同一个值（toolPin.<tool>.width），
// 所以在标注时拖过滑块，这里显示的也会跟着变
void WinSettingCommon::initWidthCtrls(const std::wstring& tool)
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting." + tool + L"Width"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    // 值域与 ToolSub 里滑块的一致
    auto read = [tool]() {
        return std::clamp((int)std::lround(Setting::get()->getToolNum(tool, L"width", 2.f)), 1, 26);
    };
    auto makeBtn = [box](const std::wstring& text, float width) {
        auto btn = box->makeChild<Ling::Button>();
        btn->setText(text);
        btn->setHeight(28.f);
        btn->setWidth(width);
        btn->setBorder(1.f, 0xE0E0E0FF);
        btn->setHoverBg(0XFFFFFFFF);
        return btn;
    };
    auto minus = makeBtn(L"－", 36.f);
    auto value = makeBtn(std::to_wstring(read()), 52.f);
    auto plus = makeBtn(L"＋", 36.f);
    auto step = [tool, read, value](int delta) {
        const int val = std::clamp(read() + delta, 1, 26);
        Setting::get()->setToolNum(tool, L"width", (float)val);
        value->setText(std::to_wstring(val));
    };
    minus->onClick.add([step](Ling::Button*) { step(-1); });
    plus->onClick.add([step](Ling::Button*) { step(1); });

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

// 记录剪切板历史的开关，样子和开机自启动那个一样
void WinSettingCommon::initClipCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.clipHistory"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    auto apply = [](Ling::Button* btn) {
        const bool on = Setting::get()->getClipEnabled();
        btn->setText(on ? L"\ue688" : L"\ue687");
        btn->setColor(on ? 0x597ef7ff : 0x666666FF);
        btn->setHoverColor(on ? 0x597ef7ff : 0x666666FF);
    };
    auto btn = box->makeChild<Ling::Button>();
    btn->setFontFamily(L"icon");
    btn->setHeightPercent(100.f);
    btn->setFontSize(18.f);
    btn->setWidth(60.f);
    apply(btn);
    btn->onClick.add([apply](Ling::Button* btn) {
        Setting::get()->setClipEnabled(!Setting::get()->getClipEnabled());
        apply(btn);
    });

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

// 主题色：一个色块按钮，点开系统的调色板随便挑
void WinSettingCommon::initThemeCtrls()
{
    auto box = makeChild<Ling::Node>();
    box->setHeight(39.f);
    box->setFlexDirection(Ling::FlexDirection::Row);
    box->setAlignItems(Ling::Align::Center);

    auto label = box->makeChild<Ling::Label>();
    label->setText(Lang::get(L"setting.themeColor"));
    label->setHeightPercent(100.f);
    label->setJustifyContent(Ling::Justify::Center);
    label->setFlexGrow(1.f);

    const uint32_t rgba = (Setting::get()->getThemeColor() << 8) | 0xFF;
    auto btn = box->makeChild<Ling::Button>();
    btn->setHeight(22.f);
    btn->setWidth(60.f);
    btn->setBg(rgba);
    btn->setHoverBg(rgba);
    btn->setBorder(1.f, 0xE0E0E0FF);
    btn->onClick.add([this](Ling::Button* btn) {
        static COLORREF custom[16]{};
        const UINT cur = Setting::get()->getThemeColor();
        CHOOSECOLOR cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = win->hwnd;
        cc.lpCustColors = custom;
        // COLORREF 是 0x00BBGGRR，和配置里存的 0xRRGGBB 字节序相反
        cc.rgbResult = RGB((cur >> 16) & 0xFF, (cur >> 8) & 0xFF, cur & 0xFF);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColor(&cc)) return;
        const UINT rgb = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
        Setting::get()->setThemeColor(rgb);
        // 设置窗口自己的高亮也用主题色，同切语言一样：关掉重开一遍
        win->close();
        Ling::App::get()->dq.TryEnqueue([]() {
            WinSetting::init();
        });
    });

    auto border = makeChild<Ling::Node>();
    border->setHeight(1.f);
    border->setBg(0xE0E0E0FF);
}

void WinSettingCommon::setAutoStartBtn(Ling::Button* btn)
{
    auto setting = Setting::get();
    auto isAutoStart = setting->getAutoStart();
    if (isAutoStart) {
        btn->setText(L"\ue688");
        btn->setColor(0x597ef7ff);
        btn->setHoverColor(0x597ef7ff);
    }
    else {
        btn->setText(L"\ue687");
        btn->setColor(0x666666FF);
        btn->setHoverColor(0x666666FF);
    }
}

void WinSettingCommon::hideSelectBox()
{
    if (!selectBox) return;
    win->onMouseDown.remove(onMouseDownToken);
    win->body->removeChild(selectBox);
    selectBox = nullptr;
}

void WinSettingCommon::showSelectBox(Ling::Button* btn)
{
    auto weakThis = getWeakThis();
    onMouseDownToken = win->onMouseDown.add([this,weakThis](POINT pos, bool isRight) {
        if (!weakThis.lock()) return;
        if (!this->selectBox) return;
        if (this->selectBtn->isPosIn(pos)) return;
        if (this->selectBox->isPosIn(pos)) return;
        win->body->removeChild(selectBox);
        this->selectBox = nullptr;
        this->win->onMouseDown.remove(this->onMouseDownToken);
    });
    if (selectBox) {
        win->body->removeChild(selectBox);
    }
    auto langs = Lang::get()->getSupportedLang();
    auto itemH{ 30.f };
    auto totalH = std::min(320.f, itemH * (langs.size()+1));

    selectBox = win->body->makeChild<Ling::ScrollerBox>();
    selectBox->setSize(btn->w/win->dpi, totalH);
    selectBox->setPositionType(Ling::Position::Absolute);
    selectBox->setPosition(Ling::Edge::Left, btn->x/win->dpi);
    selectBox->setPosition(Ling::Edge::Top, btn->y/win->dpi);
    selectBox->setBg(0xFFFFFFFF);
    selectBox->setBorder(1.f, 0x597ef766);
    for (auto& pair:langs)
    {
        auto btn = selectBox->makeChild<Ling::Button>();
        btn->setText(pair.first);
        btn->setHeight(itemH);
        btn->setWidthPercent(100.f);
        btn->setHoverBg(0Xf2f2f2FF);
        btn->setHoverColor(0X000000FF);
        btn->onClick.add([this](Ling::Button* btn) {
            auto lang = Lang::get();
            auto langName = btn->getText();
            auto langs = lang->getSupportedLang();
            for (auto& pair : langs)
            {
                if (pair.first == langName) {
                    Setting::get()->setLang(pair.second);
                    win->close();
                    Ling::App::get()->dq.TryEnqueue([this]() {
                        WinSetting::init();
                    });
                    break;
                }
            }
        });
    }
    auto lastItem = selectBox->makeChild<Ling::Button>();
    lastItem->setText(Lang::get(L"setting.getMoreLang"));
    lastItem->setHeight(itemH);
    lastItem->setWidthPercent(100.f);
    lastItem->setHoverBg(0Xf2f2f2FF);
    lastItem->setHoverColor(0X000000FF);
    lastItem->onClick.add([this](Ling::Button* btn) {
        win->onMouseDown.remove(onMouseDownToken);
        std::wstring downloadUrl{ L"https://github.com/xland/ScreenCapture/tree/main/Lang" };
        ShellExecute(win->hwnd, L"open", downloadUrl.data(), nullptr, nullptr, SW_SHOWNORMAL);
        win->body->removeChild(selectBox);
        selectBox = nullptr;
    });
}
