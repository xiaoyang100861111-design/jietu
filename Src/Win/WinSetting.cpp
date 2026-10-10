#include "pch.h"
#include <filesystem>
#include "../App.h"
#include "../Lang.h"
#include "WinSetting.h"
#include "../Setting.h"
#include "../Update.h"
#include "WinSettingCommon.h"
#include "WinSettingShortcut.h"
#include "WinSettingAbout.h"
#include "WinSettingData.h"

std::unique_ptr<WinSetting> winSetting;

WinSetting::WinSetting() :Ling::WinBase()
{
	// 关窗按钮是 body 的子节点，而 close() 正是从它的点击回调里一路进来的 ——
	// 在那里同步 winSetting.reset() 就是 use-after-free，所以推迟到下一轮消息循环。
	// 不放掉的话这个对象会一直活着，Ling 那边就永远看不到"一个窗口都不剩"，D2D 设备
	// 也就永远还不回去
	onDestroy.add([]() {
		Update::onStateChanged = nullptr;
		Ling::App::get()->dq.TryEnqueue([]() { winSetting.reset(); });
	});
	setTitle(Lang::get(L"setting.title"));
	setSize(680, 560);
	setCenter();
	createNativeWindow();
}

WinSetting::~WinSetting()
{

}

namespace {
	// 主题色换成 Ling::Color 认的 0xRRGGBBAA
	uint32_t themeRgba()
	{
		return (Setting::get()->getThemeColor() << 8) | 0xFF;
	}
}

void WinSetting::init()
{
	// 已经开着就拉到前台，不建第二个。原来是"关掉旧的再建新的"，那样会和上面那个
	// 延迟释放撞车：排在队列里的 reset 跑起来时放掉的是刚建好的这一个
	if (winSetting) {
		SetForegroundWindow(winSetting->hwnd);
		return;
	}
	winSetting.reset(new WinSetting());
}

void WinSetting::dispose()
{
	winSetting.reset();
}

void WinSetting::onCreated()
{
	enableShadow();
	body->setBg(0xFAFAFAFF);
	body->setFlexDirection(Ling::FlexDirection::Row);
	auto menuBox = body->makeChild<Ling::Node>();
	menuBox->setBg(0xEEEEF0FF);
	menuBox->setWidth(160.f);
	menuBox->setHeightPercent(100.f);
	menuBox->setPaddingTop(40.f);
	initMenuItems(menuBox);

	content = body->makeChild<WinSettingCommon>();
	content->setFlexGrow(1.0);
	content->setHeightPercent(100.f);
	content->setPaddingTop(40.f);
	content->setPadding(20.f, 40.f, 20.f, 40.f);
	content->setFlexDirection(Ling::FlexDirection::Column);

	// 顶上一条浅灰色的状态栏（菜单右边那一段）：左边版本号，右边有没有新版本，点一下去更新。
	// 绝对定位，盖在内容区顶部留出来的那 40 像素上；关闭按钮建在它后面，所以叠在它上面
	auto header = body->makeChild<Ling::Node>();
	header->setPositionType(Ling::Position::Absolute);
	header->setPosition(Ling::Edge::Left, 160.f);
	header->setPosition(Ling::Edge::Top, 0.f);
	header->setPosition(Ling::Edge::Right, 0.f);
	header->setHeight(32.f);
	header->setBg(0xEEEEF0FF);
	header->setFlexDirection(Ling::FlexDirection::Row);
	header->setAlignItems(Ling::Align::Center);
	auto verLabel = header->makeChild<Ling::Label>();
	verLabel->setText(std::format(L"UU截图   {} {}", Lang::get(L"about.version"), Update::version()));
	verLabel->setHeightPercent(100.f);
	verLabel->setJustifyContent(Ling::Justify::Center);
	verLabel->setFlexGrow(1.f);
	verLabel->setMarginLeft(20.f);
	verLabel->setColor(0x666666FF);
	updateBtn = header->makeChild<Ling::Button>();
	updateBtn->setHeight(24.f);
	updateBtn->setFontSize(12.f);
	updateBtn->setMarginRight(50.f); //给右上角的关闭按钮让位
	updateBtn->setBg(0);
	updateBtn->setHoverBg(0);
	// 点它 = 手动检查：有新版弹升级窗口，没有就说一声已是最新
	updateBtn->onClick.add([](Ling::Button*) { Update::checkNow(); });
	refreshUpdateBtn();
	Update::onStateChanged = []() {
		if (winSetting) winSetting->refreshUpdateBtn();
	};
	Update::checkQuiet(); //打开设置时悄悄查一次，结果回来会刷新上面那行字

	auto closeBtn = body->makeChild<Ling::Button>();
	closeBtn->setSize(42.f, 32.f);
	closeBtn->setPositionType(Ling::Position::Absolute);
	closeBtn->setPosition(Ling::Edge::Right, 0);
	closeBtn->setPosition(Ling::Edge::Top, 0);
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->onClick.add([](Ling::Button* btn) {
		btn->win->close();
		});
	show();
}
void WinSetting::initMenuItems(Ling::Node* menuBox)
{
	for (size_t i = 0; i < 4; i++)
	{
		auto menuItem = menuBox->makeChild<Ling::Button>();
		menuItem->setFontSize(14.f);
		menuItem->setHeight(40.f);
		if (i == 0) {
			menuItem->setColor(0xFFFFFFFF);
			menuItem->setBg(themeRgba());
			menuItem->setHoverColor(0xFFFFFFFF);
			menuItem->setHoverBg(themeRgba());
			menuItem->setText(Lang::get(L"setting.common"));
		}
		else {
			menuItem->setHoverColor(0x000000ff);
			menuItem->setHoverBg(0xE1E1E3ff);
			if (i == 1) {
				menuItem->setText(Lang::get(L"setting.shortcut"));
			}
			else if (i == 2) {
				menuItem->setText(Lang::get(L"setting.about"));
			}
			else {
				menuItem->setText(Lang::get(L"setting.dataDir"));
			}
		}
		menuItem->onClick.add([this](auto menuItem) {this->onMenuItemClick(menuItem);});
		menus.push_back(menuItem);
	}
}
void WinSetting::onMenuItemClick(Ling::Button* menuItem)
{
	auto index = Ling::Util::getIndex(menus, menuItem);
	if (index < 0 || index == menuIndex) return;
	// 通用设置里的语言下拉框是挂在 body 上的（要能盖住下面的控件），content 被换掉
	// 它不会跟着消失，所以切菜单之前先收掉
	if (menuIndex == 0) {
		static_cast<WinSettingCommon*>(content)->hideSelectBox();
	}
	auto oldItem = menus[menuIndex];
	oldItem->setColor(0x333333FF);
	oldItem->setBg(0x00000000);
	oldItem->setHoverColor(0x000000ff);
	oldItem->setHoverBg(0xE1E1E3ff);
	menuIndex = index;
	menuItem->setColor(0xFFFFFFFF);
	menuItem->setBg(themeRgba());
	menuItem->setHoverColor(0xFFFFFFFF);
	menuItem->setHoverBg(themeRgba());

	body->removeChild(content);
	if (menuIndex == 0) {
		content = body->makeChild<WinSettingCommon>();
	}
	else if (menuIndex == 1) {
		content = body->makeChild<WinSettingShortcut>();
	}
	else if (menuIndex == 2) {
		content = body->makeChild<WinSettingAbout>();
	}
	else {
		content = body->makeChild<WinSettingData>();
	}
	content->setFlexGrow(1.0);
	content->setHeightPercent(100.f);
	content->setPadding(20.f,40.f,20.f,40.f);
	content->setFlexDirection(Ling::FlexDirection::Column);
}

void WinSetting::refreshUpdateBtn()
{
	if (!updateBtn) return;
	const int state = Update::state();
	std::wstring text = state == 2 ? std::format(L"{} {}  {}", Lang::get(L"update.found"), Update::latestVersion(), Lang::get(L"update.clickToUpdate"))
		: state == 1 ? Lang::get(L"update.latest")
		: state == 3 ? Lang::get(L"update.checkRetry")
		: Lang::get(L"update.checking");
	// 有新版本用红色，够显眼；其余是不起眼的灰色
	const uint32_t color = state == 2 ? 0xE64340FF : 0x888888FF;
	updateBtn->setText(text);
	// 按钮只有文字那么宽：整条状态栏其余的地方是拖窗口用的，不该一点就去检查更新
	float textW{ 120.f };
	if (auto layout = Ling::D2D::makeTextLayout(text, 12.f * dpi)) {
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		textW = tm.widthIncludingTrailingWhitespace / dpi;
	}
	updateBtn->setWidth(textW + 16.f);
	updateBtn->setColor(color);
	updateBtn->setHoverColor(state == 2 ? 0xC0392BFF : 0x555555FF);
}

LRESULT WinSetting::onHitTest(const POINT pos)
{
	POINT pt = pos;
	ScreenToClient(hwnd, &pt);
	// 状态栏右边那行字是要能点的，别被下面"顶上一条当标题栏"的规则吃掉
	if (updateBtn && updateBtn->isPosIn(pt)) return HTCLIENT;
	if (!isMaximized) {
		auto result = borderHitTest(pt);
		if (result != HTCLIENT) return result;
	}
	if (pt.x > 0 && pt.y > 0 && pt.x < w - 32 * dpi && pt.y < 40 * dpi) {
		return HTCAPTION;
	}
	// 左边菜单下面的空白也能拖窗口。起点要在最后一个菜单项之下：
	// 顶上留白 40 + 四个菜单项各 40（原来按三项算，第四项"配置目录"就被当成标题栏，点不到）
	if (pt.x > 0 && pt.y > 40 * 5 * dpi && pt.x < 120 * dpi && pt.y < h) {
		return HTCAPTION;
	}
	return HTCLIENT;
}


