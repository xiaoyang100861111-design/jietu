#include "pch.h"
#include <include/Ling.h>
#include "Setting.h"
#include "Util.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "App.h"
#include "Win/WinSetting.h"
#include "Win/WinClip.h"
#include "Tray.h"

namespace {
    std::unique_ptr<Setting> setting;
    constexpr int capShortcutMsgId{ 100 };
    constexpr int translateShortcutMsgId{ 101 };
    constexpr int clipShortcutMsgId{ 102 };
    constexpr int phraseShortcutMsgId{ 103 };
    constexpr int delayShortcutMsgId{ 104 };

    int shortcutMsgId(const std::wstring& type)
    {
        if (type == L"translate") return translateShortcutMsgId;
        if (type == L"clip") return clipShortcutMsgId;
        if (type == L"phrase") return phraseShortcutMsgId;
        if (type == L"delay") return delayShortcutMsgId;
        return capShortcutMsgId;
    }

    // 低级鼠标钩子：把选定的那个鼠标键变成截图键。钩子回调必须马上返回，
    // 所以真正开截图窗口的活排进消息队列里去做
    HHOOK mouseHook{ nullptr };
    int mouseTrigger{ 0 };
    bool swallowUp{ false };

    LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && mouseTrigger != 0) {
            bool down{ false }, up{ false };
            if (mouseTrigger == 1) {
                down = wParam == WM_MBUTTONDOWN;
                up = wParam == WM_MBUTTONUP;
            }
            else if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
                auto info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
                if (HIWORD(info->mouseData) == (mouseTrigger == 2 ? XBUTTON1 : XBUTTON2)) {
                    down = wParam == WM_XBUTTONDOWN;
                    up = !down;
                }
            }
            // 把这一下吞掉（返回非零），底下的程序收不到，弹着的菜单也就不会被这次点击关掉。
            // 已经在截图了就放行，免得截图过程中这个键失灵
            if (down && !WinCap::get()) {
                swallowUp = true;
                Ling::App::get()->dq.TryEnqueue([]() { WinCap::init(); });
                return 1;
            }
            if (up && swallowUp) {
                swallowUp = false;
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 只有真的选了某个鼠标键才装钩子，关闭时拆掉，不白白拦着全系统的鼠标消息
    void applyMouseTrigger(int val)
    {
        mouseTrigger = val;
        if (val != 0 && !mouseHook) {
            mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, GetModuleHandle(nullptr), 0);
        }
        else if (val == 0 && mouseHook) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
        }
    }
    // 配置文件的默认内容。空文件、坏 JSON、缺键都拿它兜底，所以这里列出的每一项
    // 都是代码里会直接按名字取的（见 getLang / getAutoStart / initShortcutKeys）
    constexpr std::wstring_view defaultConfig{ LR"""({"common":{"autoStart":false,"language":"zh-CN"},"shortcutKey":{"cap":"Ctrl+Alt+A"}})""" };
}


Setting::Setting() :dataPath{ initDataPath() }, configPath{ initConfigPath() }
{
    if (std::filesystem::exists(configPath)) {
        auto content = Ling::Util::readFileText(configPath);
        if (content.empty() || content.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            configObj = JsonObject::Parse(defaultConfig);
            save();
            return;
        }
        JsonObject obj{ nullptr };
        if (JsonObject::TryParse(content, obj)) {
            configObj = obj;
            return;
        }
        MessageBox(nullptr, L"config.json parse error，use default config", L"ScreenCapture", MB_OK | MB_ICONWARNING);
    }
    configObj = JsonObject::Parse(defaultConfig); 
}



Setting::~Setting()
{

}

void Setting::init()
{
    auto ptr = new Setting();
    setting.reset(ptr);
}

void Setting::dispose()
{
    setting.reset();
}

Setting* Setting::get()
{
    return setting.get();
}

std::filesystem::path Setting::getDataPath()
{
    return dataPath; //复制一份路径对象，不允许就地修改
}

const JsonObject Setting::getConfigObj()
{
    return configObj;
}

void Setting::setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys)
{
    std::wstring str;
    for (size_t i = 0; i < keys.size(); i++)
    {
        str += L"+" + keys[i];
    }
    str.erase(0,1);
    auto shortcutKey = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!shortcutKey) {
        shortcutKey = JsonObject();
        configObj.SetNamedValue(L"shortcutKey", shortcutKey);
    }
    shortcutKey.SetNamedValue(type, JsonValue::CreateStringValue(str));
    auto app = Ling::App::get();
    app->unRegHotKey(shortcutMsgId(type));
    app->regHotKey(str, shortcutMsgId(type));
    save();
}

std::wstring Setting::getShortcutKey(const std::wstring& type)
{
    // 一路用带默认值的重载：启动时 ensureDefaults 已经补齐过，这里只是别让运行期
    // 意外（配置被外部改动、问了个没配过的 type）变成一次崩溃
    // 没配过的给默认组合（老配置文件里没有 translate 这一项）
    std::wstring def = type == L"cap" ? L"Ctrl+Alt+A" : type == L"translate" ? L"Ctrl+Alt+T"
        : type == L"clip" ? L"Ctrl+Alt+V" : type == L"phrase" ? L"Ctrl+Alt+Q"
        : type == L"delay" ? L"Ctrl+Alt+D" : L"";
    auto obj = configObj.GetNamedObject(L"shortcutKey", nullptr);
    if (!obj) return def;
    std::wstring val{ obj.GetNamedString(type, L"") };
    return val.empty() ? def : val;
}

UINT Setting::getThemeColor()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0x1677ff;
    return static_cast<UINT>(common.GetNamedNumber(L"themeColor", 0x1677ff)) & 0xFFFFFF;
}

void Setting::setThemeColor(UINT rgb)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"themeColor", JsonValue::CreateNumberValue(static_cast<double>(rgb & 0xFFFFFF)));
    save();
}

bool Setting::getClipEnabled()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return !common || common.GetNamedBoolean(L"clipHistory", true);
}

void Setting::setClipEnabled(bool val)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"clipHistory", JsonValue::CreateBooleanValue(val));
    save();
}

int Setting::getDockEdge()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    auto val = static_cast<int>(common.GetNamedNumber(L"dockEdge", 0));
    return val >= 0 && val <= 4 ? val : 0;
}

void Setting::setDockEdge(int val)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"dockEdge", JsonValue::CreateNumberValue(val));
    save();
    WinClip::applyDock();
}

int Setting::getMouseTrigger()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    auto val = static_cast<int>(common.GetNamedNumber(L"mouseTrigger", 0));
    return val >= 0 && val <= 3 ? val : 0;
}

void Setting::setMouseTrigger(int val)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"mouseTrigger", JsonValue::CreateNumberValue(val));
    save();
    applyMouseTrigger(val);
}

void Setting::setAutoStart(bool autoStart)
{
    std::wstring runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (autoStart) {
        wchar_t buffer[MAX_PATH];
        GetModuleFileName(nullptr, buffer, MAX_PATH);
        auto curPath = std::filesystem::path(buffer);
        std::wstring commandLine = std::format(L"\"{}\" --auto-start", curPath.wstring());
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegSetValueEx(hKey, L"ScreenCapture", 0, REG_SZ, (const BYTE*)commandLine.data(), (commandLine.size() + 1) * sizeof(wchar_t));
            RegCloseKey(hKey);
        }
    }
    else {
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, runKey.data(), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValue(hKey, L"ScreenCapture");
            RegCloseKey(hKey);
        }
    }
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"autoStart", JsonValue::CreateBooleanValue(autoStart));
    save();
}

bool Setting::getAutoStart()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    return common && common.GetNamedBoolean(L"autoStart", false);
}

std::filesystem::path Setting::initDataPath()
{
    PWSTR pathTmp;
    auto hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr)) {
        _ASSERT_EXPR(FALSE, L"get roaming path，error");
        return L"";
    }
    auto dataPath = std::filesystem::path{ pathTmp };
    CoTaskMemFree(pathTmp);
    dataPath.append("ScreenCapture");
    if (!std::filesystem::exists(dataPath)) {
        if (!std::filesystem::create_directories(dataPath)) {
            _ASSERT_EXPR(FALSE, L"create data path，error");
        }
    }
    return dataPath;
}

std::filesystem::path Setting::initConfigPath()
{
    // 与插件的查找顺序一致（见 Util.cpp 里的 findImageReader）：先看 exe 同目录。
    // 只有那份文件本来就存在时才认它 —— 不存在就不要在程序目录里新建，
    // 装在 Program Files 下时那儿通常没有写权限，况且默认位置该是 appdata
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileName(nullptr, buffer, MAX_PATH);
    auto path = std::filesystem::path{ buffer }.parent_path().append(L"config.json");
    if (std::filesystem::exists(path)) return path;
    auto fallback = this->dataPath; //复制一份路径对象，append 会就地改
    return fallback.append(L"config.json");
}

void Setting::save()
{
    std::wstring str{ configObj.Stringify() };
    Ling::Util::saveFile(configPath.wstring(), str);
}

std::wstring Setting::getLang()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return L"zh-CN";
    return std::wstring{ common.GetNamedString(L"language", L"zh-CN") };
}

std::wstring Setting::getTranslateTarget()
{
    auto obj = configObj.GetNamedObject(L"translate", nullptr);
    if (!obj) return L"auto";
    return std::wstring{ obj.GetNamedString(L"target", L"auto") };
}

void Setting::setLang(const std::wstring& langCode)
{
    auto common = setting->configObj.GetNamedObject(L"common", nullptr);
    if (!common) {
        common = JsonObject();
        setting->configObj.SetNamedValue(L"common", common);
    }
    common.SetNamedValue(L"language", JsonValue::CreateStringValue(langCode));
    setting->save();
	Lang::get()->initLang(langCode);
}

JsonObject Setting::getToolObj(const std::wstring& tool)
{
    // 用带默认值的重载：这两层在旧配置文件里都不存在，直接 GetNamedObject 会抛异常，
    // 值被手工改成非对象时它也一样返回默认值，不会炸
    auto root = configObj.GetNamedObject(L"toolPin", nullptr);
    if (!root) {
        root = JsonObject();
        configObj.SetNamedValue(L"toolPin", root);
    }
    auto obj = root.GetNamedObject(tool, nullptr);
    if (!obj) {
        obj = JsonObject();
        root.SetNamedValue(tool, obj);
    }
    return obj;
}

bool Setting::getToolFlag(const std::wstring& tool, const std::wstring& key, bool def)
{
    return getToolObj(tool).GetNamedBoolean(key, def);
}

void Setting::setToolFlag(const std::wstring& tool, const std::wstring& key, bool val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateBooleanValue(val));
    save();
}

float Setting::getToolNum(const std::wstring& tool, const std::wstring& key, float def)
{
    return static_cast<float>(getToolObj(tool).GetNamedNumber(key, def));
}

void Setting::setToolNum(const std::wstring& tool, const std::wstring& key, float val)
{
    getToolObj(tool).SetNamedValue(key, JsonValue::CreateNumberValue(val));
    save();
}

long long Setting::getUpdateCheckDay()
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return 0;
    return static_cast<long long>(common.GetNamedNumber(L"updateCheckDay", 0));
}

void Setting::setUpdateCheckDay(long long day)
{
    auto common = configObj.GetNamedObject(L"common", nullptr);
    if (!common) return;
    // 这项不写进 defaultConfig：它是程序自己的记账，不是给用户改的配置
    common.SetNamedValue(L"updateCheckDay", JsonValue::CreateNumberValue(static_cast<double>(day)));
    save();
}

void Setting::initShortcutKeys()
{
    auto lingApp = Ling::App::get();
    // 取不到就用默认的那个组合：热键注册不上顶多是快捷键不好用，不该让程序起不来
    lingApp->regHotKey(getShortcutKey(L"cap"), capShortcutMsgId);
    lingApp->regHotKey(getShortcutKey(L"translate"), translateShortcutMsgId);
    lingApp->regHotKey(getShortcutKey(L"clip"), clipShortcutMsgId);
    lingApp->regHotKey(getShortcutKey(L"phrase"), phraseShortcutMsgId);
    lingApp->regHotKey(getShortcutKey(L"delay"), delayShortcutMsgId);
    applyMouseTrigger(getMouseTrigger());

    lingApp->onHotKey.add([this](UINT msg) {
        if (msg == capShortcutMsgId) {
            WinCap::init();
        }
        else if (msg == translateShortcutMsgId) {
            WinCap::init(true); //框完选区直接翻译
        }
        else if (msg == clipShortcutMsgId) {
            WinClip::toggle(WinClip::Clip);
        }
        else if (msg == phraseShortcutMsgId) {
            WinClip::toggle(WinClip::Phrase);
        }
        else if (msg == delayShortcutMsgId) {
            Tray::delayCapture();
        }
    });
    // 已经在运行时又双击了一次 exe：不截图（截图只走快捷键 / 托盘），
    // 把设置窗口打开，让用户知道程序在托盘里待着
    lingApp->onSecondInstance.add([this]() {
        WinSetting::init();
    });
}
