#include "pch.h"
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
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
    // 敲色值用的键盘、鼠标订阅。同下面那个回调：先确认自己还活着再碰成员
    hexCharToken = win->onChar.add([this, weakThis](UINT code) {
        if (!weakThis.lock() || hexTarget.empty()) return;
        onHexChar(code);
    });
    hexKeyToken = win->onKeyDown.add([this, weakThis](UINT key) {
        if (!weakThis.lock() || hexTarget.empty()) return;
        onHexKey(key);
    });
    hexMouseToken = win->onMouseDown.add([this, weakThis](POINT pos, bool isRight) {
        if (!weakThis.lock() || hexTarget.empty()) return;
        // 点到正在敲的那个色值按钮上由它自己的 onClick 处理，点别处就是不敲了
        auto it = hexBtns.find(hexTarget);
        if (it != hexBtns.end() && it->second->isPosIn(pos)) return;
        endHexInput(false);
    });
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
    win->onChar.remove(hexCharToken);
    win->onKeyDown.remove(hexKeyToken);
    win->onMouseDown.remove(hexMouseToken);
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

// 矩形 / 圆形的默认颜色和线条粗细。和标注工具条上的色块、滑块存的是同一份值
// （toolPin.<tool>.colorIndex / width），所以在标注时改过，这里显示的也会跟着变
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
        return std::clamp((int)std::lround(Setting::get()->getToolNum(tool, L"width", 1.f)), 1, 26);
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
    makeColorCtrls(box, tool);

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

namespace {
    // 与 ToolSub 里的 colors 一一对应（第一格可以自定义，见下面的 getColor）
    const std::vector<UINT> presetColors{ 0xCF1322, 0xD48806, 0x389E0D, 0x13C2C2, 0x0958D9, 0x722ED1, 0xEB2F96, 0x000000, 0xFFFFFF };

    std::wstring hexStr(UINT rgb)
    {
        return std::format(L"#{:06X}", rgb & 0xFFFFFF);
    }

    // 从一段文字里认出色值：#RGB 或 #RRGGBB，井号可有可无，前后的空白不管。认不出返回 false
    bool parseHex(const std::wstring& text, UINT& rgb)
    {
        std::wstring digits;
        for (auto c : text) {
            if (c == L'#' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
            if (!iswxdigit(c)) return false;
            digits += c;
        }
        if (digits.size() == 3) digits = { digits[0], digits[0], digits[1], digits[1], digits[2], digits[2] };
        if (digits.size() != 6) return false;
        rgb = static_cast<UINT>(wcstoul(digits.c_str(), nullptr, 16));
        return true;
    }
}

UINT WinSettingCommon::getColor(const std::wstring& target)
{
    auto setting = Setting::get();
    if (target == L"theme") return setting->getThemeColor();
    // 工具的默认色：选的是色表第一格就用那一格的自定义值，否则是色表里固定的那几种
    auto idx = static_cast<size_t>(setting->getToolNum(target, L"colorIndex", 0.f));
    if (idx == 0 || idx >= presetColors.size()) {
        return static_cast<UINT>(setting->getToolNum(target, L"color0", static_cast<float>(presetColors[0]))) & 0xFFFFFF;
    }
    return presetColors[idx];
}

// 注意：改主题色会把设置窗口关掉重开（窗口自己的高亮也用主题色），
// 那之后本节点就没了，所以调用方调完这个函数必须立刻返回
void WinSettingCommon::setColor(const std::wstring& target, UINT rgb)
{
    rgb &= 0xFFFFFF;
    auto setting = Setting::get();
    if (target == L"theme") {
        setting->setThemeColor(rgb);
        win->close();
        Ling::App::get()->dq.TryEnqueue([]() {
            WinSetting::init();
        });
        return;
    }
    // 自定义色放进色表第一格，并把默认选中的格子指回第一格
    setting->setToolNum(target, L"color0", static_cast<float>(rgb));
    setting->setToolNum(target, L"colorIndex", 0.f);
    const uint32_t rgba = (rgb << 8) | 0xFF;
    if (auto it = swatches.find(target); it != swatches.end()) {
        it->second->setBg(rgba);
        it->second->setHoverBg(rgba);
    }
    if (auto it = hexBtns.find(target); it != hexBtns.end()) it->second->setText(hexStr(rgb));
}

void WinSettingCommon::makeColorCtrls(Ling::Node* box, const std::wstring& target)
{
    const UINT rgb = getColor(target);
    const uint32_t rgba = (rgb << 8) | 0xFF;
    auto swatch = box->makeChild<Ling::Button>();
    swatch->setId(target);
    swatch->setHeight(22.f);
    swatch->setWidth(52.f);
    swatch->setMarginRight(8.f);
    swatch->setBg(rgba);
    swatch->setHoverBg(rgba);
    swatch->setBorder(1.f, 0xE0E0E0FF);
    swatch->onClick.add([this](Ling::Button* btn) {
        auto target = btn->id;
        endHexInput(false);
        static COLORREF custom[16]{};
        const UINT cur = getColor(target);
        CHOOSECOLOR cc{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = win->hwnd;
        cc.lpCustColors = custom;
        // COLORREF 是 0x00BBGGRR，和配置里存的 0xRRGGBB 字节序相反
        cc.rgbResult = RGB((cur >> 16) & 0xFF, (cur >> 8) & 0xFF, cur & 0xFF);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (!ChooseColor(&cc)) return;
        setColor(target, (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult));
    });
    swatches[target] = swatch;

    auto hex = box->makeChild<Ling::Button>();
    hex->setId(target);
    hex->setText(hexStr(rgb));
    hex->setHeight(28.f);
    hex->setWidth(96.f);
    hex->setMarginRight(target == L"theme" ? 0.f : 12.f);
    hex->setBg(0xFFFFFFFF);
    hex->setHoverBg(0xFFFFFFFF);
    hex->setBorder(1.f, 0xE0E0E0FF);
    hex->onClick.add([this](Ling::Button* btn) {
        // 再点一下正在敲的那个 = 不敲了
        const bool same = hexTarget == btn->id;
        endHexInput(false);
        if (same) return;
        hexTarget = btn->id;
        hexText.clear();
        showHexInput();
    });
    hexBtns[target] = hex;
}

// 把已经敲了的几位显示到按钮上，后面跟一个下划线表示还在等输入
void WinSettingCommon::showHexInput()
{
    auto it = hexBtns.find(hexTarget);
    if (it != hexBtns.end()) it->second->setText(L"#" + hexText + L"_");
}

void WinSettingCommon::endHexInput(bool commit)
{
    if (hexTarget.empty()) return;
    auto target = hexTarget;
    auto text = hexText;
    hexTarget.clear();
    hexText.clear();
    UINT rgb{ 0 };
    if (commit && parseHex(text, rgb)) {
        setColor(target, rgb); //改主题色时窗口会重开，这之后不能再碰成员，见 setColor
        return;
    }
    // 没敲完 / 取消：按钮上的字恢复成现在生效的色值
    auto it = hexBtns.find(target);
    if (it != hexBtns.end()) it->second->setText(hexStr(getColor(target)));
}

void WinSettingCommon::onHexChar(UINT code)
{
    if (!iswxdigit(static_cast<wint_t>(code)) || hexText.size() >= 6) return;
    hexText += static_cast<wchar_t>(towupper(static_cast<wint_t>(code)));
    // 敲满六位直接生效，不用再按回车
    if (hexText.size() == 6) endHexInput(true);
    else showHexInput();
}

void WinSettingCommon::onHexKey(UINT key)
{
    if (key == VK_ESCAPE) {
        endHexInput(false);
    }
    else if (key == VK_RETURN) {
        endHexInput(true); //三位的简写（#RGB）敲完按回车也认
    }
    else if (key == VK_BACK) {
        if (!hexText.empty()) hexText.pop_back();
        showHexInput();
    }
    else if (key == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        // 粘贴：剪切板里是 #RRGGBB 这样的色值就直接用
        UINT rgb{ 0 };
        if (!parseHex(Ling::Util::getTextFromClipboard(), rgb)) return;
        hexText = hexStr(rgb).substr(1);
        endHexInput(true);
    }
}

// 主题色
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

    makeColorCtrls(box, L"theme");

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
