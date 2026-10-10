#include "pch.h"
#include "App.h"
#include "Setting.h"
#include "Tray.h"
#include "Lang.h"
#include "Update.h"
#include "./Win/WinCap.h"
#include "./Win/WinPin.h"
#include "./Win/WinSetting.h"
#include "./Win/WinClip.h"
#include "./Win/WinPhraseEdit.h"
#include "ClipHistory.h"
#include "Log.h"
#include <shobjidl.h>
#include <shlguid.h>
#include <filesystem>

std::unique_ptr<App> app;


App::~App()
{
}

void App::init()
{
    auto ptr = new App();
    app.reset(ptr);
}

void App::dispose()
{
    // 窗口对象是文件级静态变量，交给静态析构就晚了（那时 CoUninitialize 已经跑完），
    // 所以趁这里把还开着的窗口先放掉
    keepAlive.reset();
    WinPin::dispose();
    WinCap::dispose();
    WinSetting::dispose();
    WinPhraseEdit::dispose();
    WinClip::dispose();
    ClipHistory::dispose();
    Lang::dispose();
    Setting::dispose();
    app.reset();
}

App* App::get()
{
    return app.get();
}

void App::takeScreenShot(int x, int y, int w, int h, ID2D1Bitmap1** img)
{
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    auto oldObj = SelectObject(hDC, hBitmap);
    BOOL bRet = BitBlt(hDC, 0, 0, w, h, hScreen, x, y, SRCCOPY);
    ReleaseDC(NULL, hScreen);
    std::vector<BYTE> data(w * 4 * h);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hDC, hBitmap, 0, h, data.data(), &bmi, DIB_RGB_COLORS);
    SelectObject(hDC, oldObj);
    DeleteDC(hDC);
    DeleteObject(hBitmap);
    D2D1_BITMAP_PROPERTIES1 props = {
       .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)},
       .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    auto d2d = Ling::D2D::get();
    auto hr = d2d->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data.data(), w * 4, props, img);
}

std::tuple<int, int, int, int> App::getScreenArea()
{
	return std::make_tuple(GetSystemMetrics(SM_XVIRTUALSCREEN), 
        GetSystemMetrics(SM_YVIRTUALSCREEN), 
        GetSystemMetrics(SM_CXVIRTUALSCREEN), 
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

void App::excludeFromCapture(HWND hwnd)
{
    if (!hwnd) return;
    // 老系统上这个调用不但不失败，还会把窗口变成捕获画面里的一整块黑（实测 build 18363：
    // 返回 TRUE，读回来的 affinity 就是 0x11 —— 内核照存，可那会儿的 DWM 只认"非零即
    // 受保护内容"，一律涂黑）。所以必须自己拦住，让老系统退回"照旧被录进去"。
    // GetVersionEx 会被兼容性清单骗，只有 RtlGetVersion 给的是真版本号
    static const bool supported = []() {
        OSVERSIONINFOW vi{ sizeof(vi) };
        auto rtlGetVersion = (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        return rtlGetVersion && rtlGetVersion(&vi) == 0 && vi.dwBuildNumber >= 19041;
    }();
    if (!supported) return;
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
}

namespace {
    // 让图形设备一直留着。
    // Ling 在"一个窗口都不剩"时会把整套 D3D / D2D / DWrite 设备销毁，下次建窗口再重建。
    // 只挂着托盘图标待命时正是一个窗口都没有，于是每按一次截图快捷键都要先花几百毫秒
    // 把设备和字体重新建一遍 —— 开着设置窗口时截图快、缩到托盘后截图慢，就是这个原因。
    // 办法是放一个永远不建实际窗口的窗口对象占着位：Ling 数窗口数的是对象，不是句柄。
    // 代价是待命时多占几十 MB 内存，换来截图随叫随到
    class KeepAlive : public Ling::WinBase {};
    std::unique_ptr<KeepAlive> keepAlive;

    // 启动后稍等一下，把图形设备提前建好：第一次截图也不用等
    void CALLBACK onWarmTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        if (Ling::App::get()) Ling::D2D::get();
    }

    // 桌面上放一个指向本程序的快捷方式。没有就建；有但指的不是现在这个 exe
    // （程序被挪了地方、换了名字，快捷方式就失效了）就重新写一遍
    void ensureDesktopShortcut()
    {
        wchar_t exe[MAX_PATH]{};
        if (GetModuleFileName(nullptr, exe, MAX_PATH) == 0) return;
        PWSTR desktop{ nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) || !desktop) return;
        auto lnk = std::filesystem::path{ desktop } / L"UU截图.lnk";
        CoTaskMemFree(desktop);
        Microsoft::WRL::ComPtr<IShellLink> link;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.GetAddressOf())))) return;
        if (FAILED(link.As(&file))) return;
        std::error_code ec;
        if (std::filesystem::exists(lnk, ec) && SUCCEEDED(file->Load(lnk.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH]{};
            // 已经指着现在这个 exe：不用动（路径不分大小写）
            if (SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) && _wcsicmp(target, exe) == 0) return;
        }
        auto dir = std::filesystem::path{ exe }.parent_path().wstring();
        link->SetPath(exe);
        link->SetArguments(L"");
        link->SetWorkingDirectory(dir.c_str());
        link->SetIconLocation(exe, 0);
        link->SetDescription(L"UU截图");
        if (FAILED(file->Save(lnk.c_str(), TRUE))) Log::write(L"ERROR desktop shortcut: save failed " + lnk.wstring());
        else Log::write(L"desktop shortcut -> " + std::wstring{ exe });
    }
}

App::App()
{
    // 公开仓库里的 Ling（CI 钉住的那个版本）的 init 还不带应用名这个参数
    Ling::init();
    auto app = Ling::App::get();
    app->initArgs();
    Ling::D2D::addFonts({ L"icon.ttf" });
    // 录制中直接退出会让编码线程和 D3D 设备一起卡住，退出前先把录制停掉
    app->onBeforeQuit.add([]() { WinCap::stopIfRecording(); });
    Setting::init();
    Log::init();
    Lang::init();
    if (app->args[L"--auto-quit"] == L"true") {
        WinCap::init();
    }
    else {
        bool flag = app->refuseSecondInstance();
        if (flag) return;
        Tray::init();
        keepAlive = std::make_unique<KeepAlive>();
        SetTimer(nullptr, 0, 1500, onWarmTimer);
        ensureDesktopShortcut();
        ClipHistory::init(); //常驻托盘时才记剪切板历史，用完即走的那种进程不记
        WinClip::applyDock();
        Update::checkOnStart();
		// 开机自启不启动截图；--enter=tray 也一样，升级完重启新版本走的就是它 ——
		// 都是"只挂个托盘图标待命"，这条路上一个窗口都不建
		if (app->args[L"--auto-start"] == L"true" || app->args[L"--enter"] == L"tray") {
			Update::checkLater();
			return;
		}
		// 启动只挂托盘，不自动截图：截图走快捷键 / 托盘图标。
		// 命令行明确给了 --enter=xxx 的除外，那是用户点名要直接进某个功能
		// （上面用 operator[] 查过 --enter，没给这个参数时表里也会多出一个空值，所以要看值）
		auto enter = app->args.find(L"--enter");
		if (enter != app->args.end() && !enter->second.empty()) {
			WinCap::init();
		}
    }
}
