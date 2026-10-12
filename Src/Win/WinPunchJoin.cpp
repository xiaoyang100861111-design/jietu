#include "pch.h"
#include "WinPunchJoin.h"
#include "WinClip.h"
#include "../Punch.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Theme.h"

namespace {
	std::unique_ptr<WinPunchJoin> winJoin;

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
}

WinPunchJoin::WinPunchJoin() : Ling::WinBase()
{
	// 同 WinSetting：关窗多半是从自己身上的按钮回调里进来的，C++ 对象推迟到下一轮消息循环再放
	onDestroy.add([]() {
		WinClip::setModal(false);
		Ling::App::get()->dq.TryEnqueue([]() { winJoin.reset(); });
	});
	onKeyDown.add([this](UINT key) {
		if (key == VK_ESCAPE) close();
		else if (key == VK_RETURN) submit();
	});
	setTitle(L"UU截图");
	setSize(420.f, 330.f);
	setCenter();
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

WinPunchJoin::~WinPunchJoin()
{
}

void WinPunchJoin::open()
{
	if (winJoin) {
		SetForegroundWindow(winJoin->hwnd);
		return;
	}
	// 这个窗口开着的时候打卡面板会失焦，不能让它因此缩回去
	WinClip::setModal(true);
	winJoin.reset(new WinPunchJoin());
}

void WinPunchJoin::dispose()
{
	winJoin.reset();
}

void WinPunchJoin::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

LRESULT WinPunchJoin::onHitTest(const POINT pos)
{
	POINT pt{ pos };
	ScreenToClient(hwnd, &pt);
	if (pt.y >= 0 && pt.y < (LONG)(44.f * dpi) && pt.x < (LONG)(w - 60.f * dpi)) return HTCAPTION;
	return HTCLIENT;
}

void WinPunchJoin::onCreated()
{
	enableShadow();
	const uint32_t theme = (Setting::get()->getThemeColor() << 8) | 0xFF;
	body->setBg(Theme::c(0xFAFAFAFF));
	body->setFlexDirection(Ling::FlexDirection::Column);
	body->setPadding(18.f, 8.f, 18.f, 16.f);

	auto header = body->makeChild<Ling::Node>();
	header->setHeight(36.f);
	header->setFlexDirection(Ling::FlexDirection::Row);
	header->setAlignItems(Ling::Align::Center);
	auto caption = header->makeChild<Ling::Label>();
	caption->setColor(Theme::t(0x333333FF));
	caption->setText(Lang::get(L"punch.joinTitle"));
	caption->setFontSize(15.f);
	caption->setHeightPercent(100.f);
	caption->setJustifyContent(Ling::Justify::Center);
	caption->setFlexGrow(1.f);
	auto closeBtn = header->makeChild<Ling::Button>();
	closeBtn->setColor(Theme::t(0x333333FF));
	closeBtn->setHoverColor(Theme::t(0x333333FF));
	closeBtn->setSize(36.f, 28.f);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->setHoverColor(Theme::t(0xFFFFFFFF));
	closeBtn->setHoverBg(Theme::c(0xE81123FF));
	closeBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });

	auto makeLabel = [this](const std::wstring& text) {
		auto label = body->makeChild<Ling::Label>();
		label->setColor(Theme::t(0x333333FF));
		label->setText(text);
		label->setHeight(28.f);
		label->setJustifyContent(Ling::Justify::Center);
		label->setColor(Theme::t(0x666666FF));
		return label;
	};
	auto makeBox = [this]() {
		auto box = body->makeChild<Ling::TextBox>();
		box->setColor(Theme::t(0x333333FF));
		box->setWidthPercent(100.f);
		box->setHeight(34.f);
		box->setPadding(8.f, 2.f, 8.f, 2.f);
		box->setBg(Theme::c(0xFFFFFFFF));
		box->setBorder(1.f, Theme::c(0xD9D9D9FF));
		box->setFontSize(14.f);
		box->setVerticalCenter(true);
		return box;
	};
	makeLabel(Lang::get(L"punch.joinCode"));
	codeBox = makeBox();
	makeLabel(Lang::get(L"punch.joinName"));
	nameBox = makeBox();

	// 说明 / 出错原因
	tipLabel = body->makeChild<Ling::Label>();
	tipLabel->setColor(Theme::t(0x333333FF));
	tipLabel->setText(Lang::get(L"punch.joinTip"));
	tipLabel->setFlexGrow(1.f);
	tipLabel->setJustifyContent(Ling::Justify::Center);
	tipLabel->setFontSize(12.f);
	tipLabel->setColor(Theme::t(0x999999FF));

	auto footer = body->makeChild<Ling::Node>();
	footer->setHeight(36.f);
	footer->setFlexDirection(Ling::FlexDirection::Row);
	footer->setAlignItems(Ling::Align::Center);
	auto spacer = footer->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	auto cancelBtn = footer->makeChild<Ling::Button>();
	cancelBtn->setColor(Theme::t(0x333333FF));
	cancelBtn->setHoverColor(Theme::t(0x333333FF));
	cancelBtn->setText(Lang::get(L"clip.editCancel"));
	cancelBtn->setSize(88.f, 30.f);
	cancelBtn->setBg(Theme::c(0xFFFFFFFF));
	cancelBtn->setHoverBg(Theme::c(0xF2F2F2FF));
	cancelBtn->setBorder(1.f, Theme::c(0xD9D9D9FF));
	cancelBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });
	auto okBtn = footer->makeChild<Ling::Button>();
	okBtn->setColor(Theme::t(0x333333FF));
	okBtn->setHoverColor(Theme::t(0x333333FF));
	okBtn->setText(Lang::get(L"punch.joinBtn"));
	okBtn->setSize(88.f, 30.f);
	okBtn->setMarginLeft(10.f);
	okBtn->setBg(theme);
	okBtn->setHoverBg(theme);
	okBtn->setColor(Theme::t(0xFFFFFFFF));
	okBtn->setHoverColor(Theme::t(0xFFFFFFFF));
	okBtn->onClick.add([this](Ling::Button*) { submit(); });

	show();
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	codeBox->focus();
}

void WinPunchJoin::submit()
{
	auto punch = Punch::get();
	if (!punch || punch->isBusy()) return;
	const auto code = oneLine(codeBox->getText());
	const auto name = oneLine(nameBox->getText());
	if (code.empty() || name.empty()) {
		tipLabel->setColor(Theme::t(0xE64340FF));
		tipLabel->setText(Lang::get(L"punch.joinEmpty"));
		return;
	}
	tipLabel->setColor(Theme::t(0x999999FF));
	tipLabel->setText(Lang::get(L"punch.joining"));
	// 回调来的时候窗口可能已经被关掉了，所以不捕获 this，回来再看 winJoin 还在不在
	punch->join(code, name, [](bool ok, const std::wstring& why) {
		if (!winJoin) return;
		if (ok) {
			winJoin->close();
			return;
		}
		winJoin->tipLabel->setColor(Theme::t(0xE64340FF));
		winJoin->tipLabel->setText(why);
	});
}
