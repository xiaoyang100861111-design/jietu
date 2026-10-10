#include "pch.h"
#include "Tray.h"
#include "App.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinSetting.h"
#include "Win/WinClip.h"
#include "Setting.h"

namespace {
	static std::unique_ptr<Tray> trayIns;
	static constexpr UINT settingMsg = 163;
	static constexpr UINT exitMsg = 164;
	static constexpr UINT capMsg = 165;
	static constexpr UINT delayMsg = 166;
	static constexpr UINT clipMsg = 167;
	static constexpr UINT phraseMsg = 168;

	void CALLBACK onDelayTimer(HWND, UINT, UINT_PTR id, DWORD)
	{
		KillTimer(nullptr, id);
		WinCap::init();
	}
}

Tray::Tray()
{
	auto lingApp = Ling::App::get();
	lingApp->initTray(100, L"UU截图");
	Setting::get()->initShortcutKeys();
	// 左键点托盘图标打开设置窗口（截图只走快捷键 / 右键菜单），右键出菜单
	lingApp->onTrayMouseEvent.add([this](bool isDown, bool isRight) {
		if (isDown && isRight) {
			this->onTrayRightClick();
		}
		else if (isDown) {
			WinSetting::init();
		}
	});
}

Tray::~Tray()
{
}

void Tray::init()
{
	auto ptr = new Tray();
	trayIns.reset(ptr);
}

void Tray::delayCapture()
{
	SetTimer(nullptr, 0, 3000, onDelayTimer);
}

Tray* Tray::get()
{
	return trayIns.get();
}

void Tray::onTrayRightClick()
{
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, capMsg, Lang::get(L"tray.cap").data());
	AppendMenu(menu, MF_STRING, delayMsg, Lang::get(L"tray.delay").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, phraseMsg, Lang::get(L"tray.phrase").data());
	AppendMenu(menu, MF_STRING, clipMsg, Lang::get(L"tray.clip").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, settingMsg, Lang::get(L"tray.setting").data());
	AppendMenu(menu, MF_STRING, exitMsg, Lang::get(L"tray.exit").data());
	auto menuId = Ling::App::get()->popupMenu(menu);
	if (menuId == capMsg)
	{
		WinCap::init();
	}
	else if (menuId == delayMsg)
	{
		delayCapture();
	}
	else if (menuId == clipMsg)
	{
		WinClip::toggle(WinClip::Clip);
	}
	else if (menuId == phraseMsg)
	{
		WinClip::toggle(WinClip::Phrase);
	}
	else if (menuId == settingMsg)
	{
		WinSetting::init();
	}
	else if (menuId == exitMsg)
	{
		Ling::App::get()->quit(0);
	}
}
