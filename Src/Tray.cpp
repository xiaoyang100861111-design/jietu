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

	// 延时截图：留出几秒让用户把右键菜单、下拉框这些一碰键盘就消失的东西先弹出来
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
	// 左键单击 / 双击 都进入截图
	lingApp->onTrayMouseEvent.add([this](bool isDown, bool isRight) {
		if (isDown && !isRight) {
			WinCap::init();
		}
		else if (isDown && isRight) {
			this->onTrayRightClick();
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

Tray* Tray::get()
{
	return trayIns.get();
}

void Tray::onTrayRightClick()
{
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, capMsg, Lang::get(L"tray.cap").data());
	AppendMenu(menu, MF_STRING, delayMsg, Lang::get(L"tray.delay").data());
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
		SetTimer(nullptr, 0, 3000, onDelayTimer);
	}
	else if (menuId == clipMsg)
	{
		WinClip::toggle();
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
