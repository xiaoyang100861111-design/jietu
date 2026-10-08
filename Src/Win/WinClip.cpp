#include "pch.h"
#include <algorithm>
#include <cwctype>
#include <ctime>
#include "WinClip.h"
#include "../Lang.h"
#include "../Setting.h"

using Microsoft::WRL::ComPtr;

namespace {
	std::unique_ptr<WinClip> winClip;
	constexpr float winW{ 400.f }, winH{ 540.f };   //逻辑像素
	constexpr int tabCount{ 5 };
	const wchar_t* tabKeys[tabCount]{ L"clip.all", L"clip.text", L"clip.image", L"clip.files", L"clip.pinned" };

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

WinClip::WinClip(HWND prevHwnd) : Ling::WinBase(), prevHwnd{ prevHwnd }
{
	setTitle(L"UU截图");
	setSize(winW, winH);
	// 弹在光标右下方，放不下就往回收，始终留在光标所在的那块屏幕的工作区里
	POINT cursor{};
	GetCursorPos(&cursor);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
	int posX = cursor.x + 12, posY = cursor.y + 12;
	if (posX + (int)w > mi.rcWork.right) posX = mi.rcWork.right - (int)w;
	if (posY + (int)h > mi.rcWork.bottom) posY = mi.rcWork.bottom - (int)h;
	x = (std::max)(posX, (int)mi.rcWork.left);
	y = (std::max)(posY, (int)mi.rcWork.top);

	onMouseDown.add([this](POINT pos, bool isRight) { onDown(pos, isRight); });
	onMouseMove.add([this](POINT pos) { onMove(pos); });
	onMouseWheel.add([this](POINT pos, float space) { onWheel(space); });
	onKeyDown.add([this](UINT key) { onKey(key); });
	onChar.add([this](UINT code) { onCharInput(code); });
	// 点到别处去了：面板就是个临时弹层，直接收掉
	onBlur.add([this]() { requestClose(); });
	// 同 WinSetting：窗口句柄已经没了，C++ 对象推迟到下一轮消息循环再放
	onDestroy.add([]() {
		if (auto history = ClipHistory::get()) history->onChanged = nullptr;
		Ling::App::get()->dq.TryEnqueue([]() { winClip.reset(); });
	});
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

WinClip::~WinClip()
{
}

void WinClip::toggle()
{
	if (winClip) {
		winClip->requestClose();
		return;
	}
	if (!ClipHistory::get()) return;
	// 先记下现在谁在前台，再建窗口 —— 窗口一出来前台就是它自己了
	auto prev = GetForegroundWindow();
	winClip.reset(new WinClip(prev));
}

void WinClip::dispose()
{
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
	// 面板开着的时候又复制了东西、或者在面板里删了一条，列表要跟着变
	ClipHistory::get()->onChanged = []() {
		if (!winClip || winClip->closing) return;
		winClip->rebuild();
		winClip->refresh();
	};
	show();
	// 要能收键盘输入（搜索、上下键），得把自己弄到前台
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
}

void WinClip::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

float WinClip::listTop() const
{
	return 90.f * dpi;
}

float WinClip::listBottom() const
{
	return h - 6.f * dpi;
}

D2D1_RECT_F WinClip::btnRect(float rowY, float rowH, int which) const
{
	const float size = 24.f * dpi;
	const float right = w - 10.f * dpi - (which == 1 ? 0.f : size + 2.f * dpi); //0 收藏 1 删除
	const float top = rowY + (rowH - size) / 2.f;
	return D2D1::RectF(right - size, top, right, top + size);
}

void WinClip::rebuild()
{
	auto history = ClipHistory::get();
	rows.clear();
	contentH = 0.f;
	if (!history) return;
	const auto key = toLower(query);
	std::vector<std::shared_ptr<ClipHistory::Item>> list;
	for (auto& item : history->getItems()) {
		if (tab == 1 && item->type != ClipHistory::Type::Text) continue;
		if (tab == 2 && item->type != ClipHistory::Type::Image) continue;
		if (tab == 3 && item->type != ClipHistory::Type::Files) continue;
		if (tab == 4 && !item->pinned) continue;
		if (!key.empty()) {
			// 图片没有可搜的文字，有搜索词时不显示
			if (item->type == ClipHistory::Type::Image) continue;
			if (toLower(item->text).find(key) == std::wstring::npos) continue;
		}
		list.push_back(item);
	}
	// 收藏的排在最前面，各自内部仍然是新的在前
	std::stable_partition(list.begin(), list.end(), [](auto& item) { return item->pinned; });
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
	// 搜索框。没有真的输入框控件：按键直接进 query，这里把它画出来，末尾补一条竖线当光标
	auto searchRect = D2D1::RectF(pad, pad, w - pad, 46.f * dpi);
	brush->SetColor(D2D1::ColorF(0xFFFFFF));
	ctx->FillRoundedRectangle(D2D1::RoundedRect(searchRect, 6.f * dpi, 6.f * dpi), brush.Get());
	brush->SetColor(D2D1::ColorF(query.empty() ? 0xDDDDDD : theme));
	ctx->DrawRoundedRectangle(D2D1::RoundedRect(searchRect, 6.f * dpi, 6.f * dpi), brush.Get(), dpi);
	auto searchText = D2D1::RectF(searchRect.left + 10.f * dpi, searchRect.top, searchRect.right - 10.f * dpi, searchRect.bottom);
	if (query.empty()) drawText(ctx, Lang::get(L"clip.search"), 13.f, searchText, 0xAAAAAA, false, true);
	else drawText(ctx, query + L"|", 13.f, searchText, 0x333333, false, true);

	// 分类
	const float tabTop = 54.f * dpi, tabBottom = 82.f * dpi, tabW = 58.f * dpi;
	for (int i = 0; i < tabCount; i++) {
		auto rect = D2D1::RectF(pad + tabW * i, tabTop, pad + tabW * (i + 1), tabBottom);
		const bool isHover = hover == Hit::Tab && hoverIndex == i;
		drawText(ctx, Lang::get(tabKeys[i]), 13.f, rect, i == tab ? theme : isHover ? 0x333333 : 0x777777, true, true);
		if (i == tab) {
			brush->SetColor(D2D1::ColorF(theme));
			ctx->FillRectangle(D2D1::RectF(rect.left + 16.f * dpi, tabBottom - 2.f * dpi, rect.right - 16.f * dpi, tabBottom), brush.Get());
		}
	}
	auto clearRect = D2D1::RectF(w - pad - 50.f * dpi, tabTop, w - pad, tabBottom);
	drawText(ctx, Lang::get(L"clip.clear"), 13.f, clearRect, hover == Hit::Clear ? 0xE64340 : 0x999999, true, true);
	brush->SetColor(D2D1::ColorF(0xE3E3E5));
	ctx->FillRectangle(D2D1::RectF(0.f, tabBottom + 3.f * dpi, w, tabBottom + 3.f * dpi + dpi), brush.Get());

	// 列表
	const float top = listTop(), bottom = listBottom();
	if (rows.empty()) {
		drawText(ctx, Lang::get(L"clip.empty"), 13.f, D2D1::RectF(0.f, top, w, bottom), 0xAAAAAA, true, true);
		return;
	}
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

void WinClip::paintRow(ID2D1DeviceContext* ctx, const Row& row, int index, float y)
{
	const UINT theme = Setting::get()->getThemeColor();
	const float pad = 10.f * dpi;
	auto& item = *row.item;
	const bool isHover = hoverIndex == index && (hover == Hit::Row || hover == Hit::Pin || hover == Hit::Del);
	if (index == selRow) {
		brush->SetColor(D2D1::ColorF(theme, 0.12f));
		ctx->FillRectangle(D2D1::RectF(0.f, y, w, y + row.height), brush.Get());
	}
	else if (isHover) {
		brush->SetColor(D2D1::ColorF(0x000000, 0.05f));
		ctx->FillRectangle(D2D1::RectF(0.f, y, w, y + row.height), brush.Get());
	}
	const float left = pad + 4.f * dpi;
	const float right = w - pad - 60.f * dpi; //右边留给收藏、删除两个按钮
	const float metaTop = y + row.height - 22.f * dpi;
	auto mainRect = D2D1::RectF(left, y + 8.f * dpi, right, metaTop);
	std::wstring meta = timeStr(item.id);
	if (item.type == ClipHistory::Type::Text) {
		drawText(ctx, preview(item.text, 200), 13.f, mainRect, 0x222222);
		meta += std::format(L"   {} {}", item.text.size(), Lang::get(L"clip.chars"));
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
		drawText(ctx, L"📄 " + names, 13.f, mainRect, 0x222222);
		meta += count > 1 ? std::format(L"   {} {}", count, Lang::get(L"clip.fileCount")) : L"   " + first;
	}
	else {
		meta += std::format(L"   {} × {}", item.imgW, item.imgH);
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
	// 收藏、删除。收藏了的一直亮着，其余的平时淡淡地显示，光标移上去才加深
	const bool pinHover = isHover && hover == Hit::Pin, delHover = isHover && hover == Hit::Del;
	drawText(ctx, item.pinned ? L"★" : L"☆", 15.f, btnRect(y, row.height, 0),
		item.pinned ? theme : pinHover ? 0x555555 : 0xBBBBBB, true, true);
	drawText(ctx, L"✕", 12.f, btnRect(y, row.height, 1), delHover ? 0xE64340 : 0xBBBBBB, true, true);
	brush->SetColor(D2D1::ColorF(0xEBEBED));
	ctx->FillRectangle(D2D1::RectF(pad, y + row.height - dpi, w - pad, y + row.height), brush.Get());
}

WinClip::Hit WinClip::hitTest(POINT pos, int& index)
{
	index = -1;
	const float px = (float)pos.x, py = (float)pos.y;
	const float pad = 10.f * dpi;
	const float tabTop = 54.f * dpi, tabBottom = 82.f * dpi, tabW = 58.f * dpi;
	if (py >= tabTop && py < tabBottom) {
		if (px >= w - pad - 50.f * dpi && px < w - pad) return Hit::Clear;
		const int i = (int)((px - pad) / tabW);
		if (px >= pad && i >= 0 && i < tabCount) {
			index = i;
			return Hit::Tab;
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
		for (int which = 0; which < 2; which++) {
			auto rect = btnRect(rowY, row.height, which);
			if (px >= rect.left && px < rect.right && py >= rect.top && py < rect.bottom) {
				return which == 0 ? Hit::Pin : Hit::Del;
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
	if (isRight) return;
	if (hit == Hit::Tab) {
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
	else if (hit == Hit::Pin) {
		history->togglePin(rows[index].item->id);
	}
	else if (hit == Hit::Del) {
		history->remove(rows[index].item->id);
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

void WinClip::onKey(UINT key)
{
	if (closing) return;
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
		if (auto history = ClipHistory::get()) history->remove(rows[selRow].item->id);
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
