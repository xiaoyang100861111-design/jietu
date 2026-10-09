#include "pch.h"
#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include "WinClip.h"
#include "WinCap.h"
#include "WinPhraseEdit.h"
#include "../Lang.h"
#include "../Setting.h"

using Microsoft::WRL::ComPtr;

namespace {
	std::unique_ptr<WinClip> winClip;
	constexpr UINT timerDock{ 1 }, timerToast{ 2 }, timerAnim{ 3 }, timerMoveEnd{ 4 };
	constexpr int animSteps{ 9 };   //滑动动画走几步，一步 15 毫秒
	const wchar_t* clipTabKeys[5]{ L"clip.all", L"clip.text", L"clip.image", L"clip.files", L"clip.pinned" };

	// —— 贴边监视：隔一小会儿看一眼光标是不是顶在设定的那条屏幕边上 ——
	UINT_PTR dockTimer{ 0 };
	int dockHits{ 0 };
	// 面板收回去之后，光标得先离开那条边一次才会再次触发，不然刚收回又弹出来
	bool dockArmed{ true };

	bool atEdge(int edge, POINT pt)
	{
		MONITORINFO mi{ sizeof(MONITORINFO) };
		if (!GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) return false;
		auto& rc = mi.rcMonitor;
		// 只认边的中间那一段：四个角上有关闭按钮、开始菜单、显示桌面这些，顶过去不该弹面板
		auto inMiddle = [](LONG val, LONG from, LONG to) {
			const LONG margin = (to - from) * 8 / 100;
			return val >= from + margin && val <= to - margin;
		};
		POINT outside{ pt };
		if (edge == 1) { if (pt.x > rc.left || !inMiddle(pt.y, rc.top, rc.bottom)) return false; outside.x = rc.left - 1; }
		else if (edge == 2) { if (pt.y > rc.top || !inMiddle(pt.x, rc.left, rc.right)) return false; outside.y = rc.top - 1; }
		else if (edge == 3) { if (pt.x < rc.right - 1 || !inMiddle(pt.y, rc.top, rc.bottom)) return false; outside.x = rc.right; }
		else if (edge == 4) { if (pt.y < rc.bottom - 1 || !inMiddle(pt.x, rc.left, rc.right)) return false; outside.y = rc.bottom; }
		else return false;
		// 多屏时两块屏幕相接的那条边不算边缘：光标只是路过去另一块屏
		return MonitorFromPoint(outside, MONITOR_DEFAULTTONULL) == nullptr;
	}

	void CALLBACK onDockTimer(HWND, UINT, UINT_PTR, DWORD)
	{
		const int edge = Setting::get() ? Setting::get()->getDockEdge() : 0;
		POINT pt{};
		if (edge == 0 || !GetCursorPos(&pt)) return;
		if (!atEdge(edge, pt)) {
			dockArmed = true;
			dockHits = 0;
			return;
		}
		if (!dockArmed || winClip || WinCap::get()) return;
		// 按着鼠标键顶到边上多半是在拖窗口、拖选文字，不打扰
		if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
			dockHits = 0;
			return;
		}
		// 全屏游戏、全屏视频、演示模式下不弹
		QUERY_USER_NOTIFICATION_STATE state{};
		if (SUCCEEDED(SHQueryUserNotificationState(&state)) && (state == QUNS_BUSY
			|| state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE)) return;
		// 连着两次（约 0.1 秒）都在边上才算，光标只是划过去不触发
		if (++dockHits < 2) return;
		dockHits = 0;
		dockArmed = false;
		WinClip::openDocked(edge);
	}

	std::wstring toLower(std::wstring str)
	{
		std::transform(str.begin(), str.end(), str.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
		return str;
	}

	// 列表里只显示开头一小段，并把换行、连续空白压成一个空格 —— 两行的位置要尽量多放点内容
	std::wstring preview(const std::wstring& text, size_t maxLen)
	{
		std::wstring result;
		bool space{ true }; //开头的空白直接丢掉
		for (auto c : text) {
			if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
				if (!space) result += L' ';
				space = true;
			}
			else {
				result += c;
				space = false;
			}
			if (result.size() >= maxLen) break;
		}
		return result;
	}

	// id 就是记录时的毫秒时间戳
	std::wstring timeStr(long long id)
	{
		time_t sec = static_cast<time_t>(id / 1000);
		tm local{};
		if (localtime_s(&local, &sec) != 0) return L"";
		wchar_t buf[32]{};
		wcsftime(buf, 32, L"%m-%d %H:%M", &local);
		return buf;
	}

	// —— 分组路径（各级之间用 / 隔开）的几个小工具 ——
	std::vector<std::wstring> splitPath(const std::wstring& full)
	{
		std::vector<std::wstring> result;
		size_t start{ 0 };
		while (!full.empty() && start <= full.size()) {
			auto pos = full.find(L'/', start);
			result.push_back(full.substr(start, pos == std::wstring::npos ? pos : pos - start));
			if (pos == std::wstring::npos) break;
			start = pos + 1;
		}
		return result;
	}

	std::wstring parentOf(const std::wstring& full)
	{
		auto pos = full.find_last_of(L'/');
		return pos == std::wstring::npos ? L"" : full.substr(0, pos);
	}

	std::wstring nameOf(const std::wstring& full)
	{
		auto pos = full.find_last_of(L'/');
		return pos == std::wstring::npos ? full : full.substr(pos + 1);
	}

	std::wstring joinPath(const std::wstring& parent, const std::wstring& name)
	{
		return parent.empty() ? name : parent + L"/" + name;
	}

	// 给人看的路径：各级之间用 › 隔开
	std::wstring showPath(const std::wstring& full)
	{
		std::wstring result;
		for (auto c : full) {
			if (c == L'/') result += L" › ";
			else result += c;
		}
		return result;
	}

	std::wstring fileName(const std::wstring& path)
	{
		auto pos = path.find_last_of(L"\\/");
		return pos == std::wstring::npos ? path : path.substr(pos + 1);
	}
}

WinClip::WinClip(HWND prevHwnd, int mode, int edge, bool byHotkey) : Ling::WinBase(),
	prevHwnd{ prevHwnd }, mode{ mode }, byHotkey{ byHotkey }, dockEdge{ edge }
{
	setTitle(L"UU截图");
	auto setting = Setting::get();
	pinned = setting->getClipPinned();
	path = splitPath(setting->getPhrasePath()); //上次停在哪个分组，这次还在那儿（分组没了的话 rebuild 会收掉）
	POINT cursor{};
	GetCursorPos(&cursor);
	// 上次摆的位置还在某块屏幕上就用它，否则跟着光标走
	int savedX{ 0 }, savedY{ 0 };
	const bool hasPos = setting->getClipPos(savedX, savedY)
		&& MonitorFromPoint(POINT{ savedX + 40, savedY + 20 }, MONITOR_DEFAULTTONULL) != nullptr;
	// 悬浮的按上次的位置找屏幕；贴边的是鼠标顶在哪块屏幕上就在哪块
	const POINT anchor = dockEdge == 0 && hasPos ? POINT{ savedX + 40, savedY + 20 } : cursor;
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
	auto& work = mi.rcWork;
	// 尺寸用上次拖出来的；高度没调过就从上到下占满工作区。都不超过工作区
	auto [savedW, savedH] = setting->getClipSize();
	setSize(savedW, savedH > 0.f ? savedH : 600.f);
	const float workW = (float)(work.right - work.left), workH = (float)(work.bottom - work.top);
	if (savedH <= 0.f || h > workH) h = workH;
	if (w > workW) w = workW;
	const int winPxW = (int)w, winPxH = (int)h;
	int posX{ 0 }, posY{ 0 };
	if (dockEdge == 0) {
		posX = hasPos ? savedX : cursor.x + 12;
		posY = hasPos ? savedY : cursor.y + 12;
	}
	else {
		// 贴着那条边；沿边的方向用上次的位置，没有就以光标为中心
		posX = hasPos ? savedX : cursor.x - winPxW / 2;
		posY = hasPos ? savedY : cursor.y - winPxH / 2;
		if (dockEdge == 1) posX = work.left;
		else if (dockEdge == 2) posY = work.top;
		else if (dockEdge == 3) posX = work.right - winPxW;
		else posY = work.bottom - winPxH;
	}
	// 放不下就往回收，始终留在工作区里
	if (posX + winPxW > work.right) posX = work.right - winPxW;
	if (posY + winPxH > work.bottom) posY = work.bottom - winPxH;
	x = (std::max)(posX, (int)work.left);
	y = (std::max)(posY, (int)work.top);
	if (dockEdge != 0) {
		// 贴边的先摆在屏幕外面，显示出来之后再滑进来（见 onCreated / onTick）
		animTo = POINT{ x, y };
		animFrom = hiddenPos(animTo);
		x = animFrom.x;
		y = animFrom.y;
	}

	onMouseDown.add([this](POINT pos, bool isRight) { onDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onMove(pos); });
	onMouseUp.add([this](POINT pos, bool isRight) { onUp(pos, isRight); });
	onMouseWheel.add([this](POINT pos, float space) { onWheel(space); });
	onKeyDown.add([this](UINT key) { onKey(key); });
	onTimer.add([this](UINT id) { onTick(id); });
	// 点到别处去了：贴边又没固定的缩回去；悬浮的、固定的都不动（那是个正常待着的窗口）。
	// 菜单、确认框、编辑窗口开着时也会失焦，那不算
	onBlur.add([this]() {
		if (modal || closing) return;
		if (dockEdge != 0 && !pinned) requestClose();
	});
	// 拖边改了大小：列表能显示的行数变了，滚动位置要重新夹一下
	onSizeChanged.add([this]() {
		if (!created) return;
		resized = true;
		if (textBox) textBox->setWidth(w / dpi - 28.f);
		clampScroll();
		refresh();
	});
	// 窗口挪动了。是用户在拖的话，等他松手（见 timerMoveEnd）再看停在了哪
	onMoved.add([this]() {
		if (!created || animating || closing) return;
		setTimer(200, timerMoveEnd);
	});
	// 同 WinSetting：窗口句柄已经没了，C++ 对象推迟到下一轮消息循环再放
	onDestroy.add([this]() {
		if (resized && dpi > 0.f) Setting::get()->setClipSize(w / dpi, h / dpi);
		if (auto history = ClipHistory::get()) history->onChanged = nullptr;
		Ling::App::get()->dq.TryEnqueue([]() { winClip.reset(); });
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

WinClip::~WinClip()
{
}

void WinClip::toggle(int mode)
{
	if (winClip) {
		if (winClip->closing) return;
		if (winClip->mode == mode) winClip->requestClose();
		else winClip->setMode(mode);
		return;
	}
	if (!ClipHistory::get()) return;
	// 先记下现在谁在前台，再建窗口 —— 窗口一出来前台就是它自己了
	auto prev = GetForegroundWindow();
	winClip.reset(new WinClip(prev, mode, Setting::get()->getDockEdge(), true));
}

void WinClip::openDocked(int edge)
{
	if (winClip || !ClipHistory::get()) return;
	auto prev = GetForegroundWindow();
	winClip.reset(new WinClip(prev, Phrase, edge, false));
}

void WinClip::applyDock()
{
	const int edge = Setting::get()->getDockEdge();
	if (edge != 0 && !dockTimer) {
		dockTimer = SetTimer(nullptr, 0, 50, onDockTimer);
	}
	else if (edge == 0 && dockTimer) {
		KillTimer(nullptr, dockTimer);
		dockTimer = 0;
	}
}

void WinClip::setModal(bool val)
{
	if (!winClip) return;
	winClip->modal = val;
	// 编辑窗口关掉了：贴边的面板重新开始数"鼠标移开多久了"
	if (!val) winClip->outTicks = 0;
}

void WinClip::dispose()
{
	if (dockTimer) {
		KillTimer(nullptr, dockTimer);
		dockTimer = 0;
	}
	winClip.reset();
}

POINT WinClip::hiddenPos(POINT base) const
{
	POINT pos{ base };
	if (dockEdge == 1) pos.x -= (LONG)w;
	else if (dockEdge == 2) pos.y -= (LONG)h;
	else if (dockEdge == 3) pos.x += (LONG)w;
	else if (dockEdge == 4) pos.y += (LONG)h;
	return pos;
}

void WinClip::requestClose()
{
	if (closing) return;
	closing = true;
	killTimer(timerMoveEnd);
	// 贴边的先滑回屏幕外面再关（动画走完由 onTick 来关）
	RECT rect{};
	if (dockEdge != 0 && created && hwnd && GetWindowRect(hwnd, &rect)) {
		animOut = true;
		animating = true;
		animStep = 0;
		animFrom = POINT{ rect.left, rect.top };
		animTo = hiddenPos(animFrom);
		setTimer(15, timerAnim);
		return;
	}
	Ling::App::get()->dq.TryEnqueue([]() {
		if (winClip && winClip->hwnd) winClip->close();
	});
}

void WinClip::onCreated()
{
	enableShadow();
	canvas = body->makeChild<Ling::Canvas>();
	canvas->enableSwapChain();
	canvas->setSizePercent(100.f, 100.f);
	Ling::D2D::get()->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush.GetAddressOf());
	// 文本框建在画布之后：Composition 的子 visual 按插入顺序叠放，它要盖在画布上面。
	// 绝对定位摆进画布上画的那个圆角框里；宽度跟着窗口走（见 onSizeChanged）
	textBox = body->makeChild<Ling::TextBox>();
	textBox->setPositionType(Ling::Position::Absolute);
	textBox->setPosition(Ling::Edge::Left, 14.f);
	textBox->setPosition(Ling::Edge::Top, 46.f);
	textBox->setWidth(w / dpi - 28.f);
	textBox->setHeight(28.f);
	textBox->setPadding(6.f, 2.f, 6.f, 2.f);
	textBox->setBg(0xFFFFFFFF);
	textBox->setFontSize(13.f);
	textBox->setVerticalCenter(true);
	textBox->onTextChanged.add([this](Ling::TextBox*, const std::wstring& text) { onTextEdit(text); });
	updatePlaceholder();
	rebuild();
	// 面板开着的时候又复制了东西、或者增删改了话术，列表要跟着变
	ClipHistory::get()->onChanged = []() {
		if (!winClip || winClip->closing) return;
		winClip->rebuild();
		winClip->refresh();
	};
	created = true;
	// 这个定时器一直开着：记"面板之外谁在前台"（粘贴要粘回它那儿），贴边时顺便看光标移开没有
	setTimer(200, timerDock);
	if (dockEdge != 0) {
		animating = true;
		animStep = 0;
		setTimer(15, timerAnim);
	}
	if (byHotkey) {
		show();
		// 要能收键盘输入（搜索、上下键），得把自己弄到前台
		SetForegroundWindow(hwnd);
		SetFocus(hwnd);
		textBox->focus(); //打开就能直接打字搜索
	}
	else {
		// 鼠标顶边滑出来的不抢焦点：用户多半正在聊天窗口里打字，面板只是凑过来给他点一下
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	}
}

void WinClip::setBoxText(const std::wstring& text)
{
	if (!textBox) return;
	syncing = true;
	textBox->setText(text);
	syncing = false;
}

void WinClip::updatePlaceholder()
{
	if (!textBox) return;
	textBox->setPlaceholder(Lang::get(input == Input::Search ? L"clip.search" : L"clip.inputGroup"));
}

void WinClip::onTextEdit(const std::wstring& text)
{
	if (syncing || closing) return;
	// 文本框是多行的，回车会插进一个换行。这里只要单行：换行一律去掉（回车的动作在 onKey 里处理）
	std::wstring clean;
	for (auto c : text) {
		if (c != L'\r' && c != L'\n') clean += c;
	}
	if (clean != text) setBoxText(clean);
	if (input != Input::Search) {
		inputText = clean;
		return;
	}
	if (clean == query) return;
	query = clean;
	scrollY = 0.f;
	selRow = 0;
	rebuild();
	refresh();
}

void WinClip::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 再小标签和按钮就挤不下了
	mmi->ptMinTrackSize.x = (LONG)(300.f * dpi);
	mmi->ptMinTrackSize.y = (LONG)(260.f * dpi);
}

LRESULT WinClip::onHitTest(const POINT pos)
{
	// 进来的是屏幕坐标，下面认的都是窗口内的坐标
	POINT pt{ pos };
	ScreenToClient(hwnd, &pt);
	auto border = borderHitTest(pt);
	if (border != HTCLIENT) return border;
	// 顶上那一条：两个页签和右上角的按钮照常点，其余的空白当标题栏，可以拖着挪窗口
	if (pt.y >= 0 && pt.y < (LONG)(42.f * dpi)) {
		const float px = (float)pt.x;
		const bool onModes = px >= 10.f * dpi && px < (10.f + 96.f * 2.f) * dpi;
		const bool onBtns = px >= topBtnRect(2).left - 4.f * dpi;
		if (!onModes && !onBtns) return HTCAPTION;
	}
	return HTCLIENT;
}

float WinClip::listTop() const
{
	return (90.f + 28.f * tabRows) * dpi;
}

float WinClip::listBottom() const
{
	return h - 6.f * dpi;
}

std::wstring WinClip::curGroup() const
{
	if (mode != Phrase) return L"";
	std::wstring result;
	for (auto& name : path) result = joinPath(result, name);
	return result;
}

// which：从右往左数第几个小按钮
D2D1_RECT_F WinClip::btnRect(float rowY, float rowH, int which) const
{
	const float size = 24.f * dpi;
	const float right = w - 10.f * dpi - which * (size + 2.f * dpi);
	const float top = rowY + (rowH - size) / 2.f;
	return D2D1::RectF(right - size, top, right, top + size);
}

// 窗口右上角的按钮，从右往左：0 关闭、1 固定、2 添加内容（只有话术页有）
D2D1_RECT_F WinClip::topBtnRect(int which) const
{
	const float size = 28.f * dpi;
	const float right = w - 10.f * dpi - which * (size + 4.f * dpi);
	return D2D1::RectF(right - size, 9.f * dpi, right, 9.f * dpi + size);
}

float WinClip::textWidth(const std::wstring& text, float fontSize)
{
	auto layout = Ling::D2D::makeTextLayout(text, fontSize * dpi);
	if (!layout) return 0.f;
	DWRITE_TEXT_METRICS tm{};
	layout->GetMetrics(&tm);
	return tm.widthIncludingTrailingWhitespace;
}

void WinClip::setMode(int val)
{
	if (mode == val) return;
	mode = val;
	tab = 0;
	selRow = 0;
	scrollY = 0.f;
	query.clear();
	input = Input::Search;
	inputText.clear();
	setBoxText(L"");
	updatePlaceholder();
	rebuild();
	refresh();
}

void WinClip::selectGroup(const std::wstring& full)
{
	// 点的是已经选中的那个（当前选中的最深一级就是它）= 取消选中，退回上一级
	if (!full.empty() && curGroup() == full) path = splitPath(parentOf(full));
	else path = splitPath(full);
	Setting::get()->setPhrasePath(curGroup());
	scrollY = 0.f;
	selRow = 0;
	rebuild();
	refresh();
}

void WinClip::rebuild()
{
	auto history = ClipHistory::get();
	rows.clear();
	tabs.clear();
	contentH = 0.f;
	if (!history) return;
	// 选中的分组可能刚被删掉 / 改名了：从哪一级开始对不上，就把那一级往后的都丢掉
	if (mode == Phrase) {
		auto& groups = history->getGroups();
		std::wstring prefix;
		for (size_t depth = 0; depth < path.size(); depth++) {
			prefix = joinPath(prefix, path[depth]);
			if (std::find(groups.begin(), groups.end(), prefix) != groups.end()) continue;
			path.resize(depth);
			break;
		}
	}
	// 标签。剪切板页就一排，按类型分。
	// 话术页每级分组一排，最多三排：第一排是全部一级分组；选中某个分组之后，它下面的各级
	// 全部展开 —— 下一排是它的子分组，再下一排是这些子分组的子分组（不用一级一级点进去才看得到）。
	// 一个分组都不选时列表里是全部话术
	tabRows = 1;
	std::vector<std::wstring> prevRow; //上一排各个分组的完整路径
	const auto selected = curGroup();
	for (int row = 0; row < 3; row++) {
		float left = 10.f * dpi;
		auto addTab = [&](Tab::Kind kind, const std::wstring& label, const std::wstring& full, bool isSel) {
			Tab item;
			item.kind = kind;
			item.label = label;
			item.full = full;
			item.row = row;
			item.selected = isSel;
			item.left = left;
			item.right = left + textWidth(label, 13.f) + 22.f * dpi;
			left = item.right;
			tabs.push_back(std::move(item));
		};
		if (mode != Phrase) {
			for (int i = 0; i < 5; i++) addTab(Tab::Kind::Item, Lang::get(clipTabKeys[i]), L"", i == tab);
			break;
		}
		// 这一排要列哪些分组的子分组
		std::vector<std::wstring> parents;
		if (row == 0) parents.push_back(L"");
		else if (row <= (int)path.size()) {
			// 上一级选了具体的分组：只列它的子分组
			std::wstring prefix;
			for (int i = 0; i < row; i++) prefix = joinPath(prefix, path[i]);
			parents.push_back(prefix);
		}
		else if (!path.empty()) {
			// 上一级没选具体哪个：把上一排所有分组的子分组都列出来
			parents = prevRow;
		}
		std::vector<std::wstring> fulls;
		for (auto& parent : parents) {
			for (auto& name : history->getChildGroups(parent)) fulls.push_back(joinPath(parent, name));
		}
		if (fulls.empty()) {
			// 一个分组都还没有：给个入口。有了分组之后，新建都走分组上的右键菜单
			if (row == 0) addTab(Tab::Kind::Plus, Lang::get(L"clip.addGroupTab"), L"", false);
			break;
		}
		tabRows = row + 1;
		for (auto& full : fulls) {
			// 选中的那个分组，以及它的各级上级，都亮着
			const bool isSel = !selected.empty() && (selected == full || selected.starts_with(full + L"/"));
			addTab(Tab::Kind::Item, nameOf(full), full, isSel);
		}
		prevRow = std::move(fulls);
	}

	const auto key = toLower(query);
	std::vector<std::shared_ptr<ClipHistory::Item>> list;
	for (auto& item : mode == Phrase ? history->getPhrases() : history->getItems()) {
		if (mode == Phrase) {
			// 选到哪一级，就显示那一级和它下面所有子分组里的话术
			if (!selected.empty() && item->group != selected && !item->group.starts_with(selected + L"/")) continue;
		}
		else {
			if (tab == 1 && item->type != ClipHistory::Type::Text) continue;
			if (tab == 2 && item->type != ClipHistory::Type::Image) continue;
			if (tab == 3 && item->type != ClipHistory::Type::Files) continue;
			if (tab == 4 && !item->pinned) continue;
		}
		if (!key.empty()) {
			// 备注名和内容都搜。图片没有内容文字，只能靠备注名搜到
			const bool inTitle = toLower(item->title).find(key) != std::wstring::npos;
			const bool inText = item->type != ClipHistory::Type::Image && toLower(item->text).find(key) != std::wstring::npos;
			if (!inTitle && !inText) continue;
		}
		list.push_back(item);
	}
	// 剪切板页里收藏的排在最前面，各自内部仍然是新的在前
	if (mode == Clip) std::stable_partition(list.begin(), list.end(), [](auto& item) { return item->pinned; });
	for (auto& item : list) {
		Row row;
		row.item = item;
		row.top = contentH;
		row.height = (item->type == ClipHistory::Type::Image ? 92.f : 58.f) * dpi;
		contentH += row.height;
		rows.push_back(std::move(row));
	}
	if (selRow >= (int)rows.size()) selRow = (int)rows.size() - 1;
	if (selRow < 0) selRow = 0;
	hover = Hit::None;
	hoverIndex = -1;
	clampScroll();
}

void WinClip::clampScroll()
{
	const float maxScroll = (std::max)(0.f, contentH - (listBottom() - listTop()));
	scrollY = std::clamp(scrollY, 0.f, maxScroll);
}

void WinClip::scrollIntoView(int index)
{
	if (index < 0 || index >= (int)rows.size()) return;
	const float viewH = listBottom() - listTop();
	auto& row = rows[index];
	if (row.top < scrollY) scrollY = row.top;
	else if (row.top + row.height > scrollY + viewH) scrollY = row.top + row.height - viewH;
	clampScroll();
}

ID2D1Bitmap1* WinClip::getThumb(const ClipHistory::Item& item)
{
	auto it = thumbs.find(item.id);
	if (it != thumbs.end()) return it->second.Get();
	// 解码失败也往缓存里放一个空的，免得每画一帧都去读一遍盘
	auto& slot = thumbs[item.id];
	int imgW{ 0 }, imgH{ 0 };
	std::vector<BYTE> pixels;
	if (!ClipHistory::loadImage(ClipHistory::get()->getImagePath(item.id), 480, true, imgW, imgH, pixels)) return nullptr;
	D2D1_BITMAP_PROPERTIES1 props{};
	props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
	props.dpiX = 96.0f;
	props.dpiY = 96.0f;
	Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(imgW, imgH), pixels.data(), imgW * 4, &props, slot.GetAddressOf());
	return slot.Get();
}

void WinClip::layout()
{
	Ling::WinBase::layout();
	if (!canvas || !brush) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	paint(ctx);
	canvas->finishPaint();
}

void WinClip::drawText(ID2D1DeviceContext* ctx, const std::wstring& text, float fontSize, const D2D1_RECT_F& rect,
	UINT rgb, bool hCenter, bool vCenter)
{
	const float boxW = rect.right - rect.left, boxH = rect.bottom - rect.top;
	if (text.empty() || boxW <= 0.f || boxH <= 0.f) return;
	auto layout = Ling::D2D::makeTextLayout(text, fontSize * dpi, boxW, boxH);
	if (!layout) return;
	if (hCenter) layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	if (vCenter) layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	brush->SetColor(D2D1::ColorF(rgb));
	// CLIP：放不下的部分直接裁掉，不让它画到别的行上去
	ctx->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void WinClip::paint(ID2D1DeviceContext* ctx)
{
	const UINT theme = Setting::get()->getThemeColor();
	const float pad = 10.f * dpi;
	ctx->Clear(D2D1::ColorF(0xF7F7F8));
	// 第一排：左边两个页签，右边几个小按钮，中间的空白可以拖着挪窗口
	const float modeTop = 8.f * dpi, modeBottom = 38.f * dpi, modeW = 96.f * dpi;
	for (int i = 0; i < 2; i++) {
		auto rect = D2D1::RectF(pad + modeW * i, modeTop, pad + modeW * (i + 1), modeBottom);
		const bool cur = i == mode;
		if (cur) {
			brush->SetColor(D2D1::ColorF(theme));
			ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.f * dpi, 6.f * dpi), brush.Get());
		}
		const bool isHover = hover == Hit::Mode && hoverIndex == i;
		drawText(ctx, Lang::get(i == 0 ? L"clip.modePhrase" : L"clip.modeClip"), 14.f, rect,
			cur ? 0xFFFFFF : isHover ? 0x222222 : 0x666666, true, true);
	}
	auto topBtn = [&](int which, Hit hit, const std::wstring& text, float size, UINT rgb, bool filled) {
		auto rect = topBtnRect(which);
		if (filled || hover == hit) {
			brush->SetColor(D2D1::ColorF(filled ? theme : 0x000000, filled ? 0.16f : 0.07f));
			ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 5.f * dpi, 5.f * dpi), brush.Get());
		}
		drawText(ctx, text, size, rect, rgb, true, true);
	};
	topBtn(0, Hit::Top0, L"✕", 13.f, hover == Hit::Top0 ? 0xE64340 : 0x777777, false);
	// 固定：按下去是亮的（主题色、带底色）
	topBtn(1, Hit::Top1, L"📌", 13.f, pinned ? theme : 0x999999, pinned);
	if (mode == Phrase) topBtn(2, Hit::Top2, L"＋", 16.f, theme, false);

	// 第二排：搜索框。里面的字、光标都是文本框控件自己画的，这里只画外面那个圆角框，有焦点时边框亮起来。
	// 新建 / 重命名分组时也借它来输入
	auto searchRect = D2D1::RectF(pad, 44.f * dpi, w - pad, 76.f * dpi);
	const bool active = (textBox && textBox->isFocused()) || input != Input::Search || !query.empty();
	brush->SetColor(D2D1::ColorF(0xFFFFFF));
	ctx->FillRoundedRectangle(D2D1::RoundedRect(searchRect, 6.f * dpi, 6.f * dpi), brush.Get());
	brush->SetColor(D2D1::ColorF(active ? theme : 0xDDDDDD));
	ctx->DrawRoundedRectangle(D2D1::RoundedRect(searchRect, 6.f * dpi, 6.f * dpi), brush.Get(), dpi);

	// 分类 / 分组标签，一排或几排
	const float tabTop = 82.f * dpi, tabH = 28.f * dpi;
	const float tabBottom = tabTop + tabH * tabRows;
	// 剪切板页右边留给"清空"，标签多了画不下的就裁掉
	const float tabsRight = w - pad - (mode == Clip ? 54.f * dpi : 0.f);
	ctx->PushAxisAlignedClip(D2D1::RectF(0.f, tabTop, tabsRight, tabBottom), D2D1_ANTIALIAS_MODE_ALIASED);
	for (int i = 0; i < (int)tabs.size(); i++) {
		auto& item = tabs[i];
		const float rowTop = tabTop + tabH * item.row;
		auto rect = D2D1::RectF(item.left, rowTop, item.right, rowTop + tabH);
		const bool isHover = hover == Hit::Tab && hoverIndex == i;
		drawText(ctx, item.label, 13.f, rect, item.selected ? theme : isHover ? 0x333333 : 0x777777, true, true);
		if (item.selected) {
			brush->SetColor(D2D1::ColorF(theme));
			ctx->FillRectangle(D2D1::RectF(rect.left + 8.f * dpi, rect.bottom - 2.f * dpi, rect.right - 8.f * dpi, rect.bottom), brush.Get());
		}
	}
	ctx->PopAxisAlignedClip();
	if (mode == Clip) {
		auto clearRect = D2D1::RectF(w - pad - 50.f * dpi, tabTop, w - pad, tabBottom);
		drawText(ctx, Lang::get(L"clip.clear"), 13.f, clearRect, hover == Hit::Clear ? 0xE64340 : 0x999999, true, true);
	}
	brush->SetColor(D2D1::ColorF(0xE3E3E5));
	ctx->FillRectangle(D2D1::RectF(0.f, tabBottom + 3.f * dpi, w, tabBottom + 3.f * dpi + dpi), brush.Get());

	// 列表
	const float top = listTop(), bottom = listBottom();
	if (rows.empty()) {
		drawText(ctx, Lang::get(mode == Phrase ? L"clip.emptyPhrase" : L"clip.empty"), 13.f,
			D2D1::RectF(pad * 2.f, top, w - pad * 2.f, bottom), 0xAAAAAA, true, true);
	}
	else {
		ctx->PushAxisAlignedClip(D2D1::RectF(0.f, top, w, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
		for (int i = 0; i < (int)rows.size(); i++) {
			const float rowY = top + rows[i].top - scrollY;
			if (rowY + rows[i].height < top) continue;
			if (rowY > bottom) break;
			paintRow(ctx, rows[i], i, rowY);
		}
		ctx->PopAxisAlignedClip();
		// 内容比窗口长：右边画一条细细的滚动位置指示
		const float viewH = bottom - top;
		if (contentH > viewH) {
			const float barH = (std::max)(24.f * dpi, viewH * viewH / contentH);
			const float barY = top + (viewH - barH) * scrollY / (contentH - viewH);
			brush->SetColor(D2D1::ColorF(0x000000, 0.22f));
			ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(w - 5.f * dpi, barY, w - 2.f * dpi, barY + barH), 1.5f * dpi, 1.5f * dpi), brush.Get());
		}
	}
	if (!toastText.empty()) {
		const float toastW = textWidth(toastText, 13.f) + 28.f * dpi;
		const float toastBottom = bottom - 12.f * dpi;
		auto rect = D2D1::RectF((w - toastW) / 2.f, toastBottom - 32.f * dpi, (w + toastW) / 2.f, toastBottom);
		brush->SetColor(D2D1::ColorF(0x000000, 0.75f));
		ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 16.f * dpi, 16.f * dpi), brush.Get());
		drawText(ctx, toastText, 13.f, rect, 0xFFFFFF, true, true);
	}
}

void WinClip::paintRow(ID2D1DeviceContext* ctx, const Row& row, int index, float y)
{
	const UINT theme = Setting::get()->getThemeColor();
	const float pad = 10.f * dpi;
	auto& item = *row.item;
	const bool isHover = hoverIndex == index && hover >= Hit::Row && hover <= Hit::Btn2;
	if (index == selRow) {
		brush->SetColor(D2D1::ColorF(theme, 0.12f));
		ctx->FillRectangle(D2D1::RectF(0.f, y, w, y + row.height), brush.Get());
	}
	else if (isHover) {
		brush->SetColor(D2D1::ColorF(0x000000, 0.05f));
		ctx->FillRectangle(D2D1::RectF(0.f, y, w, y + row.height), brush.Get());
	}
	const float left = pad + 4.f * dpi;
	const float right = btnRect(y, row.height, 2).left - 6.f * dpi; //右边留给三个小按钮
	const float metaTop = y + row.height - 22.f * dpi;
	auto mainRect = D2D1::RectF(left, y + 8.f * dpi, right, metaTop);
	// 下面那行小字：话术写它在哪个分组，剪切板写复制的时间；后面再跟上各类型自己的说明
	std::wstring meta = mode == Phrase ? showPath(item.group) : timeStr(item.id);
	auto addMeta = [&meta](const std::wstring& part) {
		if (part.empty()) return;
		if (!meta.empty()) meta += L"   ";
		meta += part;
	};
	// 有备注名的：第一行是备注（主题色，一眼认出是哪条），内容退到第二行；没有的话内容占两行
	auto drawMain = [&](const std::wstring& content) {
		if (item.title.empty()) {
			drawText(ctx, content, 13.f, mainRect, 0x222222);
			return;
		}
		const float mid = mainRect.top + 19.f * dpi;
		drawText(ctx, item.title, 13.f, D2D1::RectF(mainRect.left, mainRect.top, mainRect.right, mid), theme);
		drawText(ctx, content, 12.f, D2D1::RectF(mainRect.left, mid, mainRect.right, mainRect.bottom + 2.f * dpi), 0x555555);
	};
	if (item.type == ClipHistory::Type::Text) {
		drawMain(preview(item.text, 200));
		addMeta(std::format(L"{} {}", item.text.size(), Lang::get(L"clip.chars")));
	}
	else if (item.type == ClipHistory::Type::Files) {
		// 只显示文件名，多个文件用逗号隔开；完整路径放在下面那行小字里
		std::wstring names, first;
		int count{ 0 };
		size_t start{ 0 };
		while (start <= item.text.size()) {
			auto pos = item.text.find(L'\n', start);
			auto file = item.text.substr(start, pos == std::wstring::npos ? pos : pos - start);
			if (!file.empty()) {
				if (count == 0) first = file;
				if (count < 6) names += (names.empty() ? L"" : L"，") + fileName(file);
				count++;
			}
			if (pos == std::wstring::npos) break;
			start = pos + 1;
		}
		drawMain(L"📄 " + names);
		addMeta(count > 1 ? std::format(L"{} {}", count, Lang::get(L"clip.fileCount")) : first);
	}
	else {
		// 图片：缩略图占着主区域，备注名放到小字里
		addMeta(item.title);
		addMeta(std::format(L"{} × {}", item.imgW, item.imgH));
		if (auto thumb = getThumb(item)) {
			// 等比缩进主区域里，靠左摆
			auto size = thumb->GetSize();
			const float boxW = mainRect.right - mainRect.left, boxH = mainRect.bottom - mainRect.top;
			const float scale = (std::min)({ boxW / size.width, boxH / size.height, 1.f });
			auto dest = D2D1::RectF(mainRect.left, mainRect.top, mainRect.left + size.width * scale, mainRect.top + size.height * scale);
			ctx->DrawBitmap(thumb, dest, 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
			brush->SetColor(D2D1::ColorF(0x000000, 0.12f));
			ctx->DrawRectangle(dest, brush.Get(), 1.f);
		}
	}
	drawText(ctx, meta, 11.f, D2D1::RectF(left, metaTop, right, y + row.height - 4.f * dpi), 0x999999, false, true);
	// 右边的小按钮，平时淡淡地显示，光标移上去才加深。
	// 话术页：✕ 删除、✎ 编辑、⇄ 换分组；剪切板页：✕ 删除、☆ 收藏、＋ 存为话术
	auto btnColor = [&](Hit which, UINT hoverRgb) -> UINT {
		return isHover && hover == which ? hoverRgb : 0xBBBBBB;
	};
	drawText(ctx, L"✕", 12.f, btnRect(y, row.height, 0), btnColor(Hit::Btn0, 0xE64340), true, true);
	if (mode == Phrase) {
		drawText(ctx, L"✎", 14.f, btnRect(y, row.height, 1), btnColor(Hit::Btn1, 0x555555), true, true);
		drawText(ctx, L"⇄", 14.f, btnRect(y, row.height, 2), btnColor(Hit::Btn2, theme), true, true);
	}
	else {
		drawText(ctx, item.pinned ? L"★" : L"☆", 15.f, btnRect(y, row.height, 1),
			item.pinned ? theme : btnColor(Hit::Btn1, 0x555555), true, true);
		drawText(ctx, L"＋", 15.f, btnRect(y, row.height, 2), btnColor(Hit::Btn2, theme), true, true);
	}
	brush->SetColor(D2D1::ColorF(0xEBEBED));
	ctx->FillRectangle(D2D1::RectF(pad, y + row.height - dpi, w - pad, y + row.height), brush.Get());
}

WinClip::Hit WinClip::hitTest(POINT pos, int& index)
{
	index = -1;
	const float px = (float)pos.x, py = (float)pos.y;
	const float pad = 10.f * dpi;
	auto inRect = [&](const D2D1_RECT_F& rect) {
		return px >= rect.left && px < rect.right && py >= rect.top && py < rect.bottom;
	};
	if (py < 42.f * dpi) {
		if (inRect(topBtnRect(0))) return Hit::Top0;
		if (inRect(topBtnRect(1))) return Hit::Top1;
		if (mode == Phrase && inRect(topBtnRect(2))) return Hit::Top2;
		const float modeW = 96.f * dpi;
		const int i = (int)((px - pad) / modeW);
		if (py >= 8.f * dpi && py < 38.f * dpi && px >= pad && i >= 0 && i < 2) {
			index = i;
			return Hit::Mode;
		}
		return Hit::None;
	}
	const float tabTop = 82.f * dpi, tabH = 28.f * dpi;
	if (py >= tabTop && py < tabTop + tabH * tabRows) {
		if (mode == Clip && px >= w - pad - 50.f * dpi && px < w - pad) return Hit::Clear;
		if (px >= w - pad - (mode == Clip ? 54.f * dpi : 0.f)) return Hit::None;
		const int row = (int)((py - tabTop) / tabH);
		for (int i = 0; i < (int)tabs.size(); i++) {
			if (tabs[i].row == row && px >= tabs[i].left && px < tabs[i].right) {
				index = i;
				return Hit::Tab;
			}
		}
		return Hit::None;
	}
	if (py < listTop() || py >= listBottom()) return Hit::None;
	const float contentY = py - listTop() + scrollY;
	for (int i = 0; i < (int)rows.size(); i++) {
		auto& row = rows[i];
		if (contentY < row.top || contentY >= row.top + row.height) continue;
		index = i;
		const float rowY = listTop() + row.top - scrollY;
		for (int which = 0; which < 3; which++) {
			if (inRect(btnRect(rowY, row.height, which))) {
				return which == 0 ? Hit::Btn0 : which == 1 ? Hit::Btn1 : Hit::Btn2;
			}
		}
		return Hit::Row;
	}
	return Hit::None;
}

void WinClip::onMove(POINT pos)
{
	int index{ -1 };
	auto hit = hitTest(pos, index);
	if (tabPressed) {
		// 松开鼠标的那一下如果落在窗口外面，这边收不到，靠按键状态兜一下
		if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
			tabPressed = false;
		}
		else {
			if (std::abs(pos.x - pressPos.x) > (int)(6 * dpi)) tabDragged = true;
			// 拖到同一级（同一个上级下面）的另一个分组上：两个换位置。被拖的那个还是它，接着拖可以一路换过去
			if (tabDragged && hit == Hit::Tab && tabs[index].kind == Tab::Kind::Item
				&& tabs[index].full != pressFull && parentOf(tabs[index].full) == parentOf(pressFull)) {
				auto other = tabs[index].full;
				if (auto history = ClipHistory::get()) history->swapGroups(pressFull, other);
				return; //swapGroups 触发的 onChanged 已经重建并重画了
			}
		}
	}
	if (hit == hover && index == hoverIndex) return;
	hover = hit;
	hoverIndex = index;
	refresh();
}

void WinClip::onDown(POINT pos, bool isRight)
{
	if (closing) return;
	int index{ -1 };
	auto hit = hitTest(pos, index);
	auto history = ClipHistory::get();
	if (!history) return;
	if (hit == Hit::Row) {
		// 左键：复制并粘贴到原来的窗口；右键：只放进剪切板，不替用户粘贴
		activate(index, !isRight);
		return;
	}
	if (hit == Hit::Tab && mode == Phrase) {
		auto item = tabs[index]; //下面可能会重建 tabs，先抄一份出来
		if (item.kind == Tab::Kind::Plus) {
			// 还没有任何分组时的那个入口：建第一个一级分组
			if (!isRight) {
				inputParent.clear();
				beginInput(Input::Group);
			}
			return;
		}
		if (isRight) {
			groupMenu(item.full);
			return;
		}
		// 左键按下先记着，松开时再决定是点击还是拖动换位置（见 onMove / onUp）
		tabPressed = true;
		tabDragged = false;
		pressFull = item.full;
		pressPos = pos;
		return;
	}
	tabPressed = false;
	if (isRight) return;
	if (hit == Hit::Mode) {
		setMode(index);
	}
	else if (hit == Hit::Top0) {
		requestClose();
	}
	else if (hit == Hit::Top1) {
		pinned = !pinned;
		Setting::get()->setClipPinned(pinned);
		// 刚取消固定：别因为光标正好不在面板上就立刻缩回去，重新数
		mouseEntered = true;
		outTicks = 0;
		toast(Lang::get(pinned ? L"clip.pinOn" : L"clip.pinOff"));
	}
	else if (hit == Hit::Top2) {
		WinPhraseEdit::open(curGroup());
	}
	else if (hit == Hit::Tab) {
		if (tab == index) return;
		tab = index;
		scrollY = 0.f;
		selRow = 0;
		rebuild();
		refresh();
	}
	else if (hit == Hit::Clear) {
		if (!confirmOnce(Lang::get(L"clip.clearAsk"))) return;
		history->clear(); //onChanged 里会重建列表并重画
	}
	else if (hit == Hit::Btn0) {
		const auto id = rows[index].item->id;
		// 话术是特意存下来的，删之前问一句；剪切板历史本来就是流水，直接删
		if (mode != Phrase) history->remove(id);
		else if (confirmOnce(Lang::get(L"clip.delPhraseAsk"))) history->removePhrase(id);
	}
	else if (hit == Hit::Btn1) {
		auto item = rows[index].item;
		if (mode == Phrase) WinPhraseEdit::open(item->group, item->id);
		else history->togglePin(item->id);
	}
	else if (hit == Hit::Btn2 && mode == Clip) {
		toast(Lang::get(history->addPhraseFromItem(rows[index].item->id, L"") ? L"clip.added" : L"clip.addFailed"));
	}
	else if (hit == Hit::Btn2) {
		// 换分组：点一下挪到下一个分组（按树的顺序，子分组紧跟在父分组后面），轮一圈回到未分组
		auto groups = history->getGroupTree();
		if (groups.empty()) {
			toast(Lang::get(L"clip.noGroup"));
			return;
		}
		auto item = rows[index].item;
		auto it = std::find(groups.begin(), groups.end(), item->group);
		// 现在未分组（找不到）→ 第一个分组；最后一个分组 → 未分组
		std::wstring next = it == groups.end() ? groups.front() : (it + 1 == groups.end() ? L"" : *(it + 1));
		history->setPhraseGroup(item->id, next);
		toast(Lang::get(L"clip.movedTo") + (next.empty() ? Lang::get(L"clip.ungrouped") : showPath(next)));
	}
}

void WinClip::onUp(POINT pos, bool isRight)
{
	if (isRight || !tabPressed) return;
	tabPressed = false;
	if (tabDragged || closing) return;
	// 没拖动 = 点击
	selectGroup(pressFull);
}

bool WinClip::confirmTwice(const std::wstring& first, const std::wstring& second)
{
	auto title = Lang::get(L"about.sysTip");
	const UINT flags = MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST;
	modal = true;
	const bool ok = MessageBox(hwnd, first.data(), title.data(), flags) == IDYES
		&& MessageBox(hwnd, second.data(), title.data(), flags) == IDYES;
	modal = false;
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	return ok;
}

bool WinClip::confirmOnce(const std::wstring& text)
{
	modal = true;
	const bool ok = MessageBox(hwnd, text.data(), Lang::get(L"about.sysTip").data(),
		MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST) == IDYES;
	modal = false;
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	return ok;
}

void WinClip::groupMenu(const std::wstring& full)
{
	auto history = ClipHistory::get();
	if (!history || full.empty()) return;
	const auto parent = parentOf(full);
	const auto label = nameOf(full);
	const int depth = (int)splitPath(full).size(); //第几级，从 1 数
	auto siblings = history->getChildGroups(parent);
	const auto it = std::find(siblings.begin(), siblings.end(), label);
	if (it == siblings.end()) return;
	const size_t at = it - siblings.begin();
	enum { cmdRename = 1, cmdLeft, cmdRight, cmdDelete, cmdAdd, cmdAddChild, cmdAddItem };
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, cmdAddItem, Lang::get(L"clip.menuAddItem").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, cmdAdd, Lang::get(L"clip.menuAdd").data());
	// 最多三级，第三级下面不能再建了
	if (depth < 3) AppendMenu(menu, MF_STRING, cmdAddChild, Lang::get(L"clip.menuAddChild").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, cmdRename, Lang::get(L"clip.menuRename").data());
	AppendMenu(menu, MF_STRING | (at == 0 ? MF_GRAYED : 0), cmdLeft, Lang::get(L"clip.menuLeft").data());
	AppendMenu(menu, MF_STRING | (at + 1 >= siblings.size() ? MF_GRAYED : 0), cmdRight, Lang::get(L"clip.menuRight").data());
	AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenu(menu, MF_STRING, cmdDelete, Lang::get(L"clip.menuDelete").data());
	POINT pt{};
	GetCursorPos(&pt);
	modal = true;
	// 菜单的 owner 得在前台，点到菜单外面它才会自己收起来
	SetForegroundWindow(hwnd);
	const UINT cmd = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, pt.x, pt.y, hwnd, nullptr);
	DestroyMenu(menu);
	modal = false;
	SetFocus(hwnd);
	if (cmd == cmdAddItem) {
		// 往这个分组里加一条话术
		WinPhraseEdit::open(full);
	}
	else if (cmd == cmdAdd || cmd == cmdAddChild) {
		// 同级：建在它的上一级下面；下级：建在它自己下面
		inputParent = cmd == cmdAdd ? parent : full;
		beginInput(Input::Group);
	}
	else if (cmd == cmdRename) {
		inputParent = parent;
		inputOld = label;
		beginInput(Input::Rename, label);
	}
	else if (cmd == cmdLeft && at > 0) {
		history->swapGroups(full, joinPath(parent, siblings[at - 1]));
	}
	else if (cmd == cmdRight && at + 1 < siblings.size()) {
		history->swapGroups(full, joinPath(parent, siblings[at + 1]));
	}
	else if (cmd == cmdDelete) {
		if (!confirmTwice(std::format(L"{}\n\n{}", Lang::get(L"clip.delAsk1"), showPath(full)),
			std::format(L"{}\n\n{}", Lang::get(L"clip.delAsk2"), showPath(full)))) return;
		history->removeGroup(full); //正选着它的话，rebuild 会把选中退回上一级
		Setting::get()->setPhrasePath(curGroup());
	}
}

void WinClip::onWheel(float space)
{
	// space > 0 是滚轮往上推，内容该往下走
	scrollY -= space;
	clampScroll();
	hover = Hit::None;
	hoverIndex = -1;
	refresh();
}

void WinClip::beginInput(Input kind, const std::wstring& init)
{
	input = kind;
	inputText = init;
	setBoxText(init);
	updatePlaceholder();
	// 鼠标顶边滑出来的面板本来没拿焦点，要打字就得拿过来
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	textBox->focus();
	textBox->selectAll(); //重命名时原来的名字全选上，直接打字就是替换
	refresh();
}

void WinClip::endInput(bool commit)
{
	auto kind = input;
	auto text = inputText;
	input = Input::Search;
	inputText.clear();
	// 文本框还给搜索用，搜索词从头来
	query.clear();
	setBoxText(L"");
	updatePlaceholder();
	auto history = ClipHistory::get();
	if (commit && history) {
		// 掐掉两头的空白；名字里不能有 /，那是各级分组之间的分隔符
		const auto from = text.find_first_not_of(L" \t");
		const auto to = text.find_last_not_of(L" \t");
		text = from == std::wstring::npos ? L"" : text.substr(from, to - from + 1);
		std::erase(text, L'/');
		if (kind == Input::Group && !text.empty()) {
			// 建好（或者本来就有同名的）直接切过去
			const auto full = joinPath(inputParent, text);
			history->addGroup(full);
			path = splitPath(full);
			Setting::get()->setPhrasePath(full);
		}
		else if (kind == Input::Rename && !text.empty() && text != inputOld) {
			const auto oldFull = joinPath(inputParent, inputOld);
			const auto newFull = joinPath(inputParent, text);
			auto ask = std::format(L"{} → {}", inputOld, text);
			if (confirmTwice(std::format(L"{}\n\n{}", Lang::get(L"clip.renameAsk1"), ask),
				std::format(L"{}\n\n{}", Lang::get(L"clip.renameAsk2"), ask))) {
				// 正选着的就是它（或它下面的）：选中的路径开头那一段跟着换成新名字
				auto cur = curGroup();
				if (history->renameGroup(oldFull, text)) {
					if (cur == oldFull) cur = newFull;
					else if (cur.starts_with(oldFull + L"/")) cur = newFull + cur.substr(oldFull.size());
					path = splitPath(cur);
					Setting::get()->setPhrasePath(cur);
				}
				else {
					toast(Lang::get(L"clip.renameFailed"));
				}
			}
		}
	}
	rebuild();
	refresh();
}

void WinClip::toast(const std::wstring& text)
{
	toastText = text;
	setTimer(1500, timerToast);
	refresh();
}

void WinClip::onMoveEnd()
{
	RECT rect{};
	POINT cursor{};
	if (closing || !GetWindowRect(hwnd, &rect) || !GetCursorPos(&cursor)) return;
	MONITORINFO mi{ sizeof(MONITORINFO) };
	if (!GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi)) return;
	auto& mon = mi.rcMonitor;
	auto& work = mi.rcWork;
	// 停在哪条边上：左右看窗口有没有顶到 / 探出屏幕；上下看松手时光标是不是顶在屏幕边上
	// （窗口默认就是从上到下一整条，上下两头本来就挨着屏幕边，不能拿它判断）
	const int slack = (int)(4 * dpi);
	int edge{ 0 };
	if (rect.left <= mon.left + slack) edge = 1;
	else if (rect.right >= mon.right - slack) edge = 3;
	else if (cursor.y <= mon.top + slack) edge = 2;
	else if (cursor.y >= mon.bottom - 1 - slack) edge = 4;
	// 多屏时两块屏幕相接的那条边不算
	if (edge != 0) {
		POINT outside{ cursor };
		if (edge == 1) outside.x = mon.left - 1;
		else if (edge == 2) outside.y = mon.top - 1;
		else if (edge == 3) outside.x = mon.right;
		else outside.y = mon.bottom;
		if (MonitorFromPoint(outside, MONITOR_DEFAULTTONULL) != nullptr) edge = 0;
	}
	const int winW = rect.right - rect.left, winH = rect.bottom - rect.top;
	int posX = rect.left, posY = rect.top;
	if (edge != 0) {
		// 贴边：那一个方向上紧贴工作区的边，另一个方向收进工作区里
		posX = std::clamp(posX, (int)work.left, (std::max)((int)work.left, (int)work.right - winW));
		posY = std::clamp(posY, (int)work.top, (std::max)((int)work.top, (int)work.bottom - winH));
		if (edge == 1) posX = work.left;
		else if (edge == 2) posY = work.top;
		else if (edge == 3) posX = work.right - winW;
		else posY = work.bottom - winH;
		if (posX != rect.left || posY != rect.top) {
			animating = true; //这一下是程序在挪，别又当成用户拖动
			SetWindowPos(hwnd, nullptr, posX, posY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			animating = false;
		}
	}
	const bool changed = edge != dockEdge;
	dockEdge = edge;
	// 光标这会儿就在面板上，等它移开了再缩
	mouseEntered = true;
	outTicks = 0;
	auto setting = Setting::get();
	setting->setClipPos(posX, posY);
	if (changed) {
		setting->setDockEdge(edge); //里面会按新的设置启停"鼠标顶边"的监视
		toast(Lang::get(edge != 0 ? L"clip.dockOn" : L"clip.dockOff"));
	}
}

void WinClip::onTick(UINT id)
{
	if (id == timerToast) {
		killTimer(timerToast);
		toastText.clear();
		refresh();
		return;
	}
	if (id == timerMoveEnd) {
		// 还按着鼠标 = 还在拖，等下一次
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) return;
		killTimer(timerMoveEnd);
		onMoveEnd();
		return;
	}
	if (id == timerAnim) {
		// 滑进来先快后慢，滑出去先慢后快，看着像被弹出来 / 吸回去
		const float t = (std::min)(1.f, ++animStep / (float)animSteps);
		const float e = animOut ? t * t : 1.f - (1.f - t) * (1.f - t) * (1.f - t);
		const int posX = animFrom.x + (int)std::lround((animTo.x - animFrom.x) * e);
		const int posY = animFrom.y + (int)std::lround((animTo.y - animFrom.y) * e);
		SetWindowPos(hwnd, nullptr, posX, posY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		if (t < 1.f) return;
		killTimer(timerAnim);
		animating = false;
		if (animOut) {
			Ling::App::get()->dq.TryEnqueue([]() {
				if (winClip && winClip->hwnd) winClip->close();
			});
		}
		return;
	}
	if (id != timerDock || closing) return;
	// 记下面板之外谁在前台：粘贴要粘回它那儿。本进程自己的窗口（面板、编辑窗口、确认框）不算
	if (auto fg = GetForegroundWindow(); fg && fg != hwnd) {
		DWORD pid{ 0 };
		GetWindowThreadProcessId(fg, &pid);
		if (pid != GetCurrentProcessId()) prevHwnd = fg;
	}
	// 下面是贴边面板的自动缩回：固定了的、悬浮的、正开着菜单 / 编辑窗口的都不缩
	if (dockEdge == 0 || pinned || modal || animating) return;
	// 正按着鼠标（多半是在拖窗口、拖边改大小，光标会跑到窗口外面去）：不缩
	if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
		outTicks = 0;
		return;
	}
	POINT cursor{};
	RECT rect{};
	if (!GetCursorPos(&cursor) || !GetWindowRect(hwnd, &rect)) return;
	InflateRect(&rect, (int)(24 * dpi), (int)(24 * dpi));
	if (PtInRect(&rect, cursor)) {
		mouseEntered = true;
		outTicks = 0;
		return;
	}
	// 正在输入分组名时不缩
	if (input != Input::Search) return;
	if (!mouseEntered) {
		// 快捷键叫出来的：光标可能离得老远，得等它进来过一次再说（或者等失焦，见 onBlur）
		if (byHotkey) return;
		// 鼠标顶边滑出来的：光标可能还没进面板（比如面板被任务栏顶开了），先宽限两秒
		if (++graceTicks < 10) return;
	}
	if (++outTicks >= 3) requestClose();
}

void WinClip::onKey(UINT key)
{
	if (closing) return;
	if (input != Input::Search) {
		// 正在输入分组名：回车确认，Esc 取消
		if (key == VK_RETURN) endInput(true);
		else if (key == VK_ESCAPE) endInput(false);
		return;
	}
	// 焦点不在文本框上（比如刚点过列表）时按了键：把焦点还给它，这样随时直接打字都是搜索。
	// 本函数比文本框自己的按键处理先跑，所以这一下按键它也收得到
	if (key != VK_ESCAPE && key != VK_RETURN && textBox && !textBox->isFocused()) textBox->focus();
	if (key == VK_ESCAPE) {
		requestClose();
	}
	else if (key == VK_UP || key == VK_DOWN) {
		if (rows.empty()) return;
		selRow = std::clamp(selRow + (key == VK_UP ? -1 : 1), 0, (int)rows.size() - 1);
		scrollIntoView(selRow);
		refresh();
	}
	else if (key == VK_RETURN) {
		activate(selRow, true);
	}
	else if (key == VK_DELETE) {
		// 搜索框里有字时 Delete 是在删字，不是删选中的那一行
		if (!query.empty()) return;
		if (selRow < 0 || selRow >= (int)rows.size()) return;
		auto history = ClipHistory::get();
		if (!history) return;
		const auto id = rows[selRow].item->id;
		if (mode != Phrase) history->remove(id);
		else if (confirmOnce(Lang::get(L"clip.delPhraseAsk"))) history->removePhrase(id);
	}
}

void WinClip::activate(int index, bool paste)
{
	if (index < 0 || index >= (int)rows.size()) return;
	auto history = ClipHistory::get();
	if (!history) return;
	const auto id = rows[index].item->id;
	auto target = prevHwnd;
	// 没固定：用完就收（先收再写剪切板，那样 writeToClipboard 触发的 onChanged 就不会再去重建列表）。
	// 固定了：面板留着，接着点下一条
	if (!pinned) requestClose();
	if (!history->writeToClipboard(id)) return;
	if (paste) history->pasteTo(target);
}
