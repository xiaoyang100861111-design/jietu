#include "pch.h"
#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <ctime>
#include "WinClip.h"
#include "WinCap.h"
#include "../Lang.h"
#include "../Setting.h"

using Microsoft::WRL::ComPtr;

namespace {
	std::unique_ptr<WinClip> winClip;
	constexpr UINT timerDock{ 1 }, timerToast{ 2 }, timerCaret{ 3 };
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
			const LONG margin = (to - from) * 15 / 100;
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
		// 连着两次都在边上才算，光标只是划过去不触发
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

	std::wstring fileName(const std::wstring& path)
	{
		auto pos = path.find_last_of(L"\\/");
		return pos == std::wstring::npos ? path : path.substr(pos + 1);
	}
}

WinClip::WinClip(HWND prevHwnd, int mode, int edge) : Ling::WinBase(), prevHwnd{ prevHwnd }, mode{ mode }, dockEdge{ edge }
{
	setTitle(L"UU截图");
	POINT cursor{};
	GetCursorPos(&cursor);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
	auto& work = mi.rcWork;
	// 尺寸用上次拖出来的；高度没调过就从上到下占满工作区。都不超过工作区
	auto [savedW, savedH] = Setting::get()->getClipSize();
	setSize(savedW, savedH > 0.f ? savedH : 600.f);
	const float workW = (float)(work.right - work.left), workH = (float)(work.bottom - work.top);
	if (savedH <= 0.f || h > workH) h = workH;
	if (w > workW) w = workW;
	const int winPxW = (int)w, winPxH = (int)h;
	int posX{ 0 }, posY{ 0 };
	if (dockEdge == 0) {
		// 弹在光标右下方
		posX = cursor.x + 12;
		posY = cursor.y + 12;
	}
	else {
		// 贴着那条边，沿边的方向以光标为中心
		posX = cursor.x - winPxW / 2;
		posY = cursor.y - winPxH / 2;
		if (dockEdge == 1) posX = work.left;
		else if (dockEdge == 2) posY = work.top;
		else if (dockEdge == 3) posX = work.right - winPxW;
		else posY = work.bottom - winPxH;
	}
	// 放不下就往回收，始终留在光标所在的那块屏幕的工作区里
	if (posX + winPxW > work.right) posX = work.right - winPxW;
	if (posY + winPxH > work.bottom) posY = work.bottom - winPxH;
	x = (std::max)(posX, (int)work.left);
	y = (std::max)(posY, (int)work.top);

	onMouseDown.add([this](POINT pos, bool isRight) { onDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onMove(pos); });
	onMouseUp.add([this](POINT pos, bool isRight) { onUp(pos, isRight); });
	onMouseWheel.add([this](POINT pos, float space) { onWheel(space); });
	onKeyDown.add([this](UINT key) { onKey(key); });
	onChar.add([this](UINT code) { onCharInput(code); });
	onTimer.add([this](UINT id) { onTick(id); });
	onFocus.add([this]() {
		focused = true;
		caretOn = true;
		refresh();
	});
	// 点到别处去了：面板就是个临时弹层，直接收掉
	onBlur.add([this]() {
		focused = false;
		if (!modal) requestClose(); //菜单、确认框开着时也会失焦，那不算
	});
	// 拖边改了大小：列表能显示的行数变了，滚动位置要重新夹一下
	onSizeChanged.add([this]() {
		if (!created) return;
		resized = true;
		clampScroll();
		refresh();
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
	winClip.reset(new WinClip(prev, mode, 0));
}

void WinClip::openDocked(int edge)
{
	if (winClip || !ClipHistory::get()) return;
	auto prev = GetForegroundWindow();
	winClip.reset(new WinClip(prev, Phrase, edge));
}

void WinClip::applyDock()
{
	const int edge = Setting::get()->getDockEdge();
	if (edge != 0 && !dockTimer) {
		dockTimer = SetTimer(nullptr, 0, 200, onDockTimer);
	}
	else if (edge == 0 && dockTimer) {
		KillTimer(nullptr, dockTimer);
		dockTimer = 0;
	}
}

void WinClip::dispose()
{
	if (dockTimer) {
		KillTimer(nullptr, dockTimer);
		dockTimer = 0;
	}
	winClip.reset();
}

void WinClip::requestClose()
{
	if (closing) return;
	closing = true;
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
	rebuild();
	// 面板开着的时候又复制了东西、或者在面板里增删了一条，列表要跟着变
	ClipHistory::get()->onChanged = []() {
		if (!winClip || winClip->closing) return;
		winClip->rebuild();
		winClip->refresh();
	};
	created = true;
	setTimer(530, timerCaret);
	if (dockEdge != 0) {
		// 贴边滑出来的不抢焦点：用户多半正在聊天窗口里打字，面板只是凑过来给他点一下。
		// 既然不拿焦点，也就等不到失焦，改由定时器看光标移开没有
		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
		setTimer(200, timerDock);
	}
	else {
		show();
		// 要能收键盘输入（搜索、上下键），得把自己弄到前台
		SetForegroundWindow(hwnd);
		SetFocus(hwnd);
	}
}

void WinClip::onMinMaxInfo(MINMAXINFO* mmi)
{
	// 再小标签和按钮就挤不下了
	mmi->ptMinTrackSize.x = (LONG)(300.f * dpi);
	mmi->ptMinTrackSize.y = (LONG)(260.f * dpi);
}

LRESULT WinClip::onHitTest(const POINT pos)
{
	// 进来的是屏幕坐标，borderHitTest 认的是窗口内的坐标
	POINT pt{ pos };
	ScreenToClient(hwnd, &pt);
	return borderHitTest(pt);
}

float WinClip::listTop() const
{
	return (90.f + 28.f * tabRows) * dpi;
}

float WinClip::listBottom() const
{
	// 话术页底下多一条"添加"的操作栏
	return h - (mode == Phrase ? 50.f : 6.f) * dpi;
}

std::wstring WinClip::groupPrefix(int depth) const
{
	std::wstring result;
	for (int i = 0; i < depth && i < (int)path.size(); i++) {
		if (!result.empty()) result += L'/';
		result += path[i];
	}
	return result;
}

std::wstring WinClip::curGroup() const
{
	return mode == Phrase ? groupPrefix((int)path.size()) : L"";
}

// which：从右往左数第几个小按钮
D2D1_RECT_F WinClip::btnRect(float rowY, float rowH, int which) const
{
	const float size = 24.f * dpi;
	const float right = w - 10.f * dpi - which * (size + 2.f * dpi);
	const float top = rowY + (rowH - size) / 2.f;
	return D2D1::RectF(right - size, top, right, top + size);
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
	path.clear();
	selRow = 0;
	scrollY = 0.f;
	query.clear();
	input = Input::Search;
	inputText.clear();
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
	// 选中的分组可能刚被删掉了：从哪一级开始对不上，就把那一级往后的都丢掉
	if (mode == Phrase) {
		auto& groups = history->getGroups();
		for (int depth = 1; depth <= (int)path.size(); depth++) {
			if (std::find(groups.begin(), groups.end(), groupPrefix(depth)) != groups.end()) continue;
			path.resize(depth - 1);
			break;
		}
	}
	// 标签。话术页：第 r 排列的是已选的前 r 级下面的子分组，后面一个"＋"新建；
	// 最多三级，所以最多三排。一个分组都不选时列表里是全部话术。剪切板页就一排，按类型分
	tabRows = mode == Phrase ? (std::min)((int)path.size() + 1, 3) : 1;
	for (int row = 0; row < tabRows; row++) {
		float left = 10.f * dpi;
		auto addTab = [&](Tab::Kind kind, const std::wstring& label, bool selected) {
			Tab item;
			item.kind = kind;
			item.label = label;
			item.row = row;
			item.selected = selected;
			item.left = left;
			item.right = left + textWidth(label, 13.f) + 22.f * dpi;
			left = item.right;
			tabs.push_back(std::move(item));
		};
		if (mode == Phrase) {
			const bool hasSel = (int)path.size() > row;
			for (auto& name : history->getChildGroups(groupPrefix(row))) {
				addTab(Tab::Kind::Item, name, hasSel && path[row] == name);
			}
			addTab(Tab::Kind::Plus, L"＋", false);
		}
		else {
			for (int i = 0; i < 5; i++) addTab(Tab::Kind::Item, Lang::get(clipTabKeys[i]), i == tab);
		}
	}

	const auto key = toLower(query);
	const auto group = curGroup();
	std::vector<std::shared_ptr<ClipHistory::Item>> list;
	for (auto& item : mode == Phrase ? history->getPhrases() : history->getItems()) {
		if (mode == Phrase) {
			// 选到哪一级，就显示那一级和它下面所有子分组里的话术
			if (!group.empty() && item->group != group && !item->group.starts_with(group + L"/")) continue;
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
	// 第一排：两个页签
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
	// 第二排：搜索框。没有真的输入框控件：按键直接进字符串，这里把它画出来，末尾补一条竖线当光标。
	// 新建分组 / 改备注名时也借它来输入
	auto searchRect = D2D1::RectF(pad, 44.f * dpi, w - pad, 76.f * dpi);
	auto& typed = input == Input::Search ? query : inputText;
	// 有焦点（能打字）或者里面有字时边框亮起来
	const bool active = focused || input != Input::Search || !typed.empty();
	brush->SetColor(D2D1::ColorF(0xFFFFFF));
	ctx->FillRoundedRectangle(D2D1::RoundedRect(searchRect, 6.f * dpi, 6.f * dpi), brush.Get());
	brush->SetColor(D2D1::ColorF(active ? theme : 0xDDDDDD));
	ctx->DrawRoundedRectangle(D2D1::RoundedRect(searchRect, 6.f * dpi, 6.f * dpi), brush.Get(), dpi);
	auto searchText = D2D1::RectF(searchRect.left + 10.f * dpi, searchRect.top, searchRect.right - 10.f * dpi, searchRect.bottom);
	float caretX = searchText.left;
	if (typed.empty()) {
		auto hint = input == Input::Group || input == Input::Rename ? L"clip.inputGroup"
			: input == Input::Title ? L"clip.inputTitle" : L"clip.search";
		// 提示文字往右让一点，给光标留个位置
		auto hintRect = searchText;
		hintRect.left += 4.f * dpi;
		drawText(ctx, Lang::get(hint), 13.f, hintRect, 0xAAAAAA, false, true);
	}
	else {
		drawText(ctx, typed, 13.f, searchText, 0x333333, false, true);
		caretX = (std::min)(searchText.left + textWidth(typed, 13.f) + dpi, searchText.right);
	}
	// 光标：跟在已输入的字后面，一亮一灭
	if (focused && caretOn) {
		brush->SetColor(D2D1::ColorF(0x333333));
		ctx->FillRectangle(D2D1::RectF(caretX, searchRect.top + 8.f * dpi, caretX + (std::max)(1.f, dpi), searchRect.bottom - 8.f * dpi), brush.Get());
	}

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

	// 话术页底部：把剪切板里现在的东西存成话术
	if (mode == Phrase) {
		brush->SetColor(D2D1::ColorF(0xE3E3E5));
		ctx->FillRectangle(D2D1::RectF(0.f, bottom + 2.f * dpi, w, bottom + 2.f * dpi + dpi), brush.Get());
		auto addRect = D2D1::RectF(pad, h - 42.f * dpi, w - pad, h - 10.f * dpi);
		brush->SetColor(D2D1::ColorF(theme, hover == Hit::AddClip ? 0.2f : 0.1f));
		ctx->FillRoundedRectangle(D2D1::RoundedRect(addRect, 6.f * dpi, 6.f * dpi), brush.Get());
		drawText(ctx, Lang::get(L"clip.addFromClip"), 13.f, addRect, theme, true, true);
	}
	if (!toastText.empty()) {
		const float toastW = textWidth(toastText, 13.f) + 28.f * dpi;
		const float toastBottom = listBottom() - 12.f * dpi;
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
	const int btnCount = 3;
	const float left = pad + 4.f * dpi;
	const float right = btnRect(y, row.height, btnCount - 1).left - 6.f * dpi; //右边留给小按钮
	const float metaTop = y + row.height - 22.f * dpi;
	auto mainRect = D2D1::RectF(left, y + 8.f * dpi, right, metaTop);
	// 备注名：文字类的直接顶在内容前面，图片 / 文件的放到下面那行小字里
	const std::wstring titleTag = item.title.empty() ? L"" : L"【" + item.title + L"】";
	// 话术的小字里写它在哪个分组，各级之间用 › 隔开
	std::wstring meta = timeStr(item.id);
	if (mode == Phrase) {
		meta.clear();
		for (auto c : item.group) {
			if (c == L'/') meta += L" › ";
			else meta += c;
		}
	}
	auto addMeta = [&meta](const std::wstring& part) {
		if (part.empty()) return;
		if (!meta.empty()) meta += L"   ";
		meta += part;
	};
	if (item.type == ClipHistory::Type::Text) {
		drawText(ctx, titleTag + preview(item.text, 200), 13.f, mainRect, 0x222222);
		addMeta(std::format(L"{} {}", item.text.size(), Lang::get(L"clip.chars")));
	}
	else if (item.type == ClipHistory::Type::Files) {
		// 只显示文件名，多个文件用逗号隔开；完整路径放在下面那行小字里
		std::wstring names, first;
		int count{ 0 };
		size_t start{ 0 };
		while (start <= item.text.size()) {
			auto pos = item.text.find(L'\n', start);
			auto path = item.text.substr(start, pos == std::wstring::npos ? pos : pos - start);
			if (!path.empty()) {
				if (count == 0) first = path;
				if (count < 6) names += (names.empty() ? L"" : L"，") + fileName(path);
				count++;
			}
			if (pos == std::wstring::npos) break;
			start = pos + 1;
		}
		drawText(ctx, titleTag + L"📄 " + names, 13.f, mainRect, 0x222222);
		addMeta(count > 1 ? std::format(L"{} {}", count, Lang::get(L"clip.fileCount")) : first);
	}
	else {
		addMeta(titleTag);
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
	// 话术页：✕ 删除、✎ 改备注名、⇄ 换分组；剪切板页：✕ 删除、☆ 收藏、＋ 存为话术
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
	if (py >= 8.f * dpi && py < 38.f * dpi) {
		const float modeW = 96.f * dpi;
		const int i = (int)((px - pad) / modeW);
		if (px >= pad && i >= 0 && i < 2) {
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
	if (mode == Phrase && py >= h - 42.f * dpi && py < h - 10.f * dpi && px >= pad && px < w - pad) return Hit::AddClip;
	if (py < listTop() || py >= listBottom()) return Hit::None;
	const float contentY = py - listTop() + scrollY;
	const int btnCount = 3;
	for (int i = 0; i < (int)rows.size(); i++) {
		auto& row = rows[i];
		if (contentY < row.top || contentY >= row.top + row.height) continue;
		index = i;
		const float rowY = listTop() + row.top - scrollY;
		for (int which = 0; which < btnCount; which++) {
			auto rect = btnRect(rowY, row.height, which);
			if (px >= rect.left && px < rect.right && py >= rect.top && py < rect.bottom) {
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
			// 拖到同一排的另一个分组上：两个换位置。被拖的那个还是它，接着拖可以一路换过去
			if (tabDragged && hit == Hit::Tab && tabs[index].kind == Tab::Kind::Item
				&& tabs[index].row == pressRow && tabs[index].label != pressLabel) {
				auto parent = groupPrefix(pressRow);
				auto head = parent.empty() ? L"" : parent + L"/";
				auto other = tabs[index].label;
				if (auto history = ClipHistory::get()) history->swapGroups(head + pressLabel, head + other);
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
			// 在这一排对应的那一级下面新建分组
			if (!isRight) {
				inputParent = groupPrefix(item.row);
				beginInput(Input::Group);
			}
			return;
		}
		if (isRight) {
			groupMenu(item.row, item.label);
			return;
		}
		// 左键按下先记着，松开时再决定是点击还是拖动换位置（见 onMove / onUp）
		tabPressed = true;
		tabDragged = false;
		pressRow = item.row;
		pressLabel = item.label;
		pressPos = pos;
		return;
	}
	tabPressed = false;
	if (isRight) return;
	if (hit == Hit::Mode) {
		setMode(index);
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
		history->clear(); //onChanged 里会重建列表并重画
	}
	else if (hit == Hit::AddClip) {
		toast(Lang::get(history->addPhraseFromClipboard(curGroup()) ? L"clip.added" : L"clip.addFailed"));
	}
	else if (hit == Hit::Btn0) {
		const auto id = rows[index].item->id;
		if (mode == Phrase) history->removePhrase(id);
		else history->remove(id);
	}
	else if (hit == Hit::Btn1) {
		const auto id = rows[index].item->id;
		if (mode == Phrase) beginInput(Input::Title, id, rows[index].item->title);
		else history->togglePin(id);
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
		auto shown = next.empty() ? Lang::get(L"clip.ungrouped") : next;
		for (size_t pos = 0; (pos = shown.find(L'/', pos)) != std::wstring::npos; pos += 3) shown.replace(pos, 1, L" › ");
		toast(Lang::get(L"clip.movedTo") + shown);
	}
}

void WinClip::onUp(POINT pos, bool isRight)
{
	if (isRight || !tabPressed) return;
	tabPressed = false;
	if (tabDragged || closing) return;
	// 没拖动 = 点击：选中这个分组；点的是已经选中的那个则取消选中（回到上一级）。
	// 比它更深的选择一并清掉
	const bool wasSelected = (int)path.size() > pressRow && path[pressRow] == pressLabel;
	path.resize((std::min)((int)path.size(), pressRow));
	if (!wasSelected) path.push_back(pressLabel);
	scrollY = 0.f;
	selRow = 0;
	rebuild();
	refresh();
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

void WinClip::groupMenu(int row, const std::wstring& label)
{
	auto history = ClipHistory::get();
	if (!history) return;
	const auto parent = groupPrefix(row);
	const auto full = parent.empty() ? label : parent + L"/" + label;
	auto siblings = history->getChildGroups(parent);
	const auto it = std::find(siblings.begin(), siblings.end(), label);
	if (it == siblings.end()) return;
	const size_t at = it - siblings.begin();
	enum { cmdRename = 1, cmdLeft, cmdRight, cmdDelete };
	auto menu = CreatePopupMenu();
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
	const auto head = parent.empty() ? L"" : parent + L"/";
	if (cmd == cmdRename) {
		inputParent = parent;
		inputOld = label;
		beginInput(Input::Rename, 0, label);
	}
	else if (cmd == cmdLeft && at > 0) {
		history->swapGroups(full, head + siblings[at - 1]);
	}
	else if (cmd == cmdRight && at + 1 < siblings.size()) {
		history->swapGroups(full, head + siblings[at + 1]);
	}
	else if (cmd == cmdDelete) {
		if (!confirmTwice(std::format(L"{}\n\n{}", Lang::get(L"clip.delAsk1"), label),
			std::format(L"{}\n\n{}", Lang::get(L"clip.delAsk2"), label))) return;
		if ((int)path.size() > row && path[row] == label) path.resize(row);
		history->removeGroup(full);
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

void WinClip::beginInput(Input kind, long long id, const std::wstring& init)
{
	input = kind;
	inputId = id;
	inputText = init;
	// 贴边滑出来的面板本来没拿焦点，要打字就得拿过来
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	refresh();
}

void WinClip::endInput(bool commit)
{
	auto kind = input;
	auto text = inputText;
	auto id = inputId;
	input = Input::Search;
	inputText.clear();
	inputId = 0;
	auto history = ClipHistory::get();
	if (commit && history) {
		// 掐掉两头的空白
		const auto from = text.find_first_not_of(L" \t");
		const auto to = text.find_last_not_of(L" \t");
		text = from == std::wstring::npos ? L"" : text.substr(from, to - from + 1);
		// 名字里不能有 /，那是各级分组之间的分隔符
		std::erase(text, L'/');
		if (kind == Input::Group && !text.empty()) {
			// 建好（或者本来就有同名的）直接切过去
			auto parent = inputParent;
			history->addGroup(parent.empty() ? text : parent + L"/" + text);
			path.clear();
			size_t start{ 0 };
			while (!parent.empty() && start <= parent.size()) {
				auto pos = parent.find(L'/', start);
				path.push_back(parent.substr(start, pos == std::wstring::npos ? pos : pos - start));
				if (pos == std::wstring::npos) break;
				start = pos + 1;
			}
			path.push_back(text);
		}
		else if (kind == Input::Rename && !text.empty() && text != inputOld) {
			auto parent = inputParent;
			auto old = inputOld;
			auto oldFull = parent.empty() ? old : parent + L"/" + old;
			auto ask = std::format(L"{} → {}", old, text);
			if (confirmTwice(std::format(L"{}\n\n{}", Lang::get(L"clip.renameAsk1"), ask),
				std::format(L"{}\n\n{}", Lang::get(L"clip.renameAsk2"), ask))) {
				// 正选着的就是它（或它下面的）：选中的那一级跟着换成新名字
				const int depth = (int)std::count(oldFull.begin(), oldFull.end(), L'/');
				const bool onPath = (int)path.size() > depth && groupPrefix(depth + 1) == oldFull;
				if (history->renameGroup(oldFull, text)) {
					if (onPath) path[depth] = text;
				}
				else {
					toast(Lang::get(L"clip.renameFailed"));
				}
			}
		}
		else if (kind == Input::Title) {
			history->setPhraseTitle(id, text); //留空 = 去掉备注名
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

void WinClip::onTick(UINT id)
{
	if (id == timerToast) {
		killTimer(timerToast);
		toastText.clear();
		refresh();
		return;
	}
	if (id == timerCaret) {
		// 焦点状态顺便对一下表：有些拿到焦点的路径不发 WM_SETFOCUS 给我们
		focused = GetFocus() == hwnd;
		caretOn = !caretOn;
		if (focused) refresh();
		return;
	}
	if (id != timerDock || closing || modal) return;
	// 正按着鼠标（多半是在拖边改大小，光标会跑到窗口外面去）：不收
	if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
		outTicks = 0;
		return;
	}
	// 贴边面板：光标移开一会儿就收回去。正在输入分组名 / 备注名时不收
	POINT cursor{};
	RECT rect{};
	if (!GetCursorPos(&cursor) || !GetWindowRect(hwnd, &rect)) return;
	InflateRect(&rect, (int)(24 * dpi), (int)(24 * dpi));
	if (PtInRect(&rect, cursor)) {
		mouseEntered = true;
		outTicks = 0;
		return;
	}
	if (input != Input::Search) return;
	// 刚滑出来时光标可能还在屏幕边上、没进面板（比如面板被任务栏顶开了），先宽限两秒
	if (!mouseEntered && ++graceTicks < 10) return;
	if (++outTicks >= 3) requestClose();
}

void WinClip::onKey(UINT key)
{
	if (closing) return;
	if (input != Input::Search) {
		// 正在输入分组名 / 备注名：回车确认，Esc 取消
		if (key == VK_RETURN) endInput(true);
		else if (key == VK_ESCAPE) endInput(false);
		else if (key == VK_BACK && !inputText.empty()) {
			inputText.pop_back();
			refresh();
		}
		return;
	}
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
		if (selRow < 0 || selRow >= (int)rows.size()) return;
		auto history = ClipHistory::get();
		if (!history) return;
		const auto id = rows[selRow].item->id;
		if (mode == Phrase) history->removePhrase(id);
		else history->remove(id);
	}
	else if (key == VK_BACK) {
		if (query.empty()) return;
		query.pop_back();
		scrollY = 0.f;
		selRow = 0;
		rebuild();
		refresh();
	}
}

void WinClip::onCharInput(UINT code)
{
	// 控制字符（退格、回车、Esc 这些）在 onKey 里处理过了，这里只收能显示的字
	if (closing || code < 32 || code == 127) return;
	caretOn = true;
	if (input != Input::Search) {
		if (inputText.size() < 30) inputText += static_cast<wchar_t>(code);
		refresh();
		return;
	}
	if (query.size() >= 60) return;
	query += static_cast<wchar_t>(code);
	scrollY = 0.f;
	selRow = 0;
	rebuild();
	refresh();
}

void WinClip::activate(int index, bool paste)
{
	if (index < 0 || index >= (int)rows.size()) return;
	auto history = ClipHistory::get();
	if (!history) return;
	const auto id = rows[index].item->id;
	auto target = prevHwnd;
	// 先收面板再写剪切板：writeToClipboard 会触发 onChanged，那时已经是 closing，不会再去重建列表
	requestClose();
	if (!history->writeToClipboard(id)) return;
	if (paste) history->pasteTo(target);
}
