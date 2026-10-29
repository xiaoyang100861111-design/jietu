#include "pch.h"
#include <algorithm>
#include <string>
#include <vector>
#include <dwmapi.h>
#include <windowsx.h>
#include "MsgBox.h"
#include "Lang.h"
#include "Setting.h"
#include "Theme.h"

#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

namespace {
	struct Btn {
		int id{ 0 };
		std::wstring text;
		D2D1_RECT_F rect{};
	};
	struct State {
		std::wstring title, text;
		UINT flags{ 0 };
		std::vector<Btn> btns;
		int focus{ 0 }, hover{ -1 }, pressed{ -1 };
		int result{ 0 };
		bool done{ false };
		float s{ 1.f };              // 缩放倍数（dpi / 96）
		float w{ 0.f }, h{ 0.f };    // 窗口大小，物理像素
		float bodyTop{ 0.f }, bodyH{ 0.f };
		ComPtr<ID2D1HwndRenderTarget> rt;
		ComPtr<ID2D1SolidColorBrush> brush;
		ComPtr<IDWriteTextLayout> titleLayout, textLayout;
	};

	D2D1_COLOR_F col(UINT rgb, float alpha = 1.f)
	{
		return D2D1::ColorF(Theme::fill(rgb), alpha);
	}

	// 图标那个圆的颜色和里面的字
	void iconOf(UINT flags, UINT& rgb, const wchar_t*& glyph)
	{
		const UINT icon = flags & MB_ICONMASK;
		if (icon == MB_ICONERROR) { rgb = 0xE64340; glyph = L"×"; }
		else if (icon == MB_ICONWARNING) { rgb = 0xFA8C16; glyph = L"!"; }
		else if (icon == MB_ICONQUESTION) { rgb = Setting::get()->getThemeColor(); glyph = L"?"; }
		else { rgb = Setting::get()->getThemeColor(); glyph = L"i"; }
	}

	// 摁了 ESC / 点了关闭算哪个按钮
	int cancelId(const State& st)
	{
		for (auto& btn : st.btns) if (btn.id == IDCANCEL) return IDCANCEL;
		for (auto& btn : st.btns) if (btn.id == IDNO) return IDNO;
		return st.btns.empty() ? IDOK : st.btns.back().id;
	}

	float widthOf(IDWriteTextLayout* layout)
	{
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		return tm.widthIncludingTrailingWhitespace;
	}
	float heightOf(IDWriteTextLayout* layout)
	{
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		return tm.height;
	}

	// 算出各部分的位置和窗口大小。maxH 是正文最多能占多高（太长的只显示前面一截）
	void layoutAll(State& st, float maxBodyH)
	{
		const float s = st.s, pad = 24.f * s;
		// 字多就宽一点，免得拉成细长一条
		st.w = (st.text.size() > 160 ? 540.f : st.text.size() > 60 ? 440.f : 380.f) * s;
		st.titleLayout = Ling::D2D::makeTextLayout(st.title, 15.f * s, st.w - pad * 2.f - 40.f * s);
		if (st.titleLayout) {
			st.titleLayout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{ 0, (UINT32)st.title.size() });
			st.titleLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		}
		st.textLayout = Ling::D2D::makeTextLayout(st.text, 13.5f * s, st.w - pad * 2.f);
		float textH{ 20.f * s };
		if (st.textLayout) {
			st.textLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 22.f * s, 16.5f * s);
			textH = heightOf(st.textLayout.Get());
		}
		st.bodyTop = pad + 28.f * s + 14.f * s;
		st.bodyH = (std::min)(textH, maxBodyH);
		// 按钮从右往左排
		const float btnH = 34.f * s, btnTop = st.bodyTop + st.bodyH + 22.f * s;
		float right = st.w - pad;
		for (int i = (int)st.btns.size() - 1; i >= 0; i--) {
			float bw{ 88.f * s };
			if (auto layout = Ling::D2D::makeTextLayout(st.btns[i].text, 13.5f * s)) bw = (std::max)(bw, widthOf(layout.Get()) + 36.f * s);
			st.btns[i].rect = D2D1::RectF(right - bw, btnTop, right, btnTop + btnH);
			right -= bw + 10.f * s;
		}
		st.h = btnTop + btnH + 20.f * s;
	}

	bool ensureTarget(HWND hwnd, State& st)
	{
		if (st.rt) return true;
		auto factory = Ling::D2D::get()->d2dFactory.Get();
		if (!factory) return false;
		auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.f, 96.f);
		auto hr = factory->CreateHwndRenderTarget(props,
			D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU((UINT32)st.w, (UINT32)st.h)), st.rt.GetAddressOf());
		if (FAILED(hr)) return false;
		st.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
		return SUCCEEDED(st.rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), st.brush.GetAddressOf()));
	}

	void paint(HWND hwnd, State& st)
	{
		if (!ensureTarget(hwnd, st)) return;
		auto rt = st.rt.Get();
		auto brush = st.brush.Get();
		const float s = st.s, pad = 24.f * s;
		const UINT theme = Setting::get()->getThemeColor();
		rt->BeginDraw();
		rt->Clear(col(0xFFFFFF));
		// 一圈细边
		brush->SetColor(col(0xD9D9DC));
		rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, st.w - 0.5f, st.h - 0.5f), brush, 1.f);
		// 图标：一个带颜色的圆，里面一个白色的符号
		UINT iconRgb{ 0 };
		const wchar_t* glyph{ L"i" };
		iconOf(st.flags, iconRgb, glyph);
		const float iconR = 14.f * s;
		const auto center = D2D1::Point2F(pad + iconR, pad + iconR);
		brush->SetColor(D2D1::ColorF(iconRgb));
		rt->FillEllipse(D2D1::Ellipse(center, iconR, iconR), brush);
		if (auto layout = Ling::D2D::makeTextLayout(glyph, 17.f * s)) {
			layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_RANGE{ 0, 1 });
			brush->SetColor(D2D1::ColorF(0xFFFFFF));
			rt->DrawTextLayout(D2D1::Point2F(center.x - widthOf(layout.Get()) / 2.f, center.y - heightOf(layout.Get()) / 2.f), layout.Get(), brush);
		}
		// 标题
		if (st.titleLayout) {
			brush->SetColor(col(0x222222));
			rt->DrawTextLayout(D2D1::Point2F(pad + iconR * 2.f + 12.f * s, center.y - heightOf(st.titleLayout.Get()) / 2.f), st.titleLayout.Get(), brush);
		}
		// 正文（太长的裁掉后面）
		if (st.textLayout) {
			rt->PushAxisAlignedClip(D2D1::RectF(0.f, st.bodyTop, st.w, st.bodyTop + st.bodyH), D2D1_ANTIALIAS_MODE_ALIASED);
			brush->SetColor(col(0x555555));
			rt->DrawTextLayout(D2D1::Point2F(pad, st.bodyTop), st.textLayout.Get(), brush);
			rt->PopAxisAlignedClip();
		}
		// 按钮："确定 / 是"是实心的（警告、出错时是红的），其余是描边的；键盘选中的那个外面多一圈
		const UINT icon = st.flags & MB_ICONMASK;
		const UINT accent = (icon == MB_ICONWARNING || icon == MB_ICONERROR) && st.btns.size() > 1 ? 0xE64340 : theme;
		for (int i = 0; i < (int)st.btns.size(); i++) {
			auto& btn = st.btns[i];
			const bool primary = btn.id == IDOK || btn.id == IDYES;
			const bool hot = st.hover == i;
			auto round = D2D1::RoundedRect(btn.rect, 7.f * s, 7.f * s);
			if (primary) {
				brush->SetColor(D2D1::ColorF(accent, hot ? 0.86f : 1.f));
				rt->FillRoundedRectangle(round, brush);
			}
			else {
				brush->SetColor(col(hot ? 0xF0F0F2 : 0xFFFFFF));
				rt->FillRoundedRectangle(round, brush);
				brush->SetColor(col(0xD0D0D4));
				rt->DrawRoundedRectangle(round, brush, s);
			}
			if (st.focus == i && st.btns.size() > 1) {
				auto outer = D2D1::RoundedRect(D2D1::RectF(btn.rect.left - 3.f * s, btn.rect.top - 3.f * s, btn.rect.right + 3.f * s, btn.rect.bottom + 3.f * s), 9.f * s, 9.f * s);
				brush->SetColor(D2D1::ColorF(primary ? accent : theme, 0.35f));
				rt->DrawRoundedRectangle(outer, brush, 2.f * s);
			}
			if (auto layout = Ling::D2D::makeTextLayout(btn.text, 13.5f * s)) {
				if (primary) brush->SetColor(D2D1::ColorF(0xFFFFFF));
				else brush->SetColor(col(0x333333));
				rt->DrawTextLayout(D2D1::Point2F((btn.rect.left + btn.rect.right - widthOf(layout.Get())) / 2.f,
					(btn.rect.top + btn.rect.bottom - heightOf(layout.Get())) / 2.f), layout.Get(), brush);
			}
		}
		if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
			// 显卡设备丢了：下次重画时重建
			st.rt.Reset();
			st.brush.Reset();
		}
	}

	int btnAt(const State& st, float px, float py)
	{
		for (int i = 0; i < (int)st.btns.size(); i++) {
			auto& rect = st.btns[i].rect;
			if (px >= rect.left && px < rect.right && py >= rect.top && py < rect.bottom) return i;
		}
		return -1;
	}

	void finish(State& st, int id)
	{
		st.result = id;
		st.done = true;
	}

	LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		auto st = reinterpret_cast<State*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (!st) return DefWindowProc(hwnd, msg, wp, lp);
		switch (msg) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT: {
			paint(hwnd, *st);
			ValidateRect(hwnd, nullptr);
			return 0;
		}
		case WM_NCHITTEST: {
			// 按钮照常点，其余的地方按住可以拖着挪
			POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
			ScreenToClient(hwnd, &pt);
			return btnAt(*st, (float)pt.x, (float)pt.y) >= 0 ? HTCLIENT : HTCAPTION;
		}
		case WM_NCMOUSEMOVE:
		case WM_MOUSELEAVE:
			if (st->hover != -1) {
				st->hover = -1;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			return msg == WM_MOUSELEAVE ? 0 : DefWindowProc(hwnd, msg, wp, lp);
		case WM_MOUSEMOVE: {
			const int at = btnAt(*st, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
			if (at != st->hover) {
				st->hover = at;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			TRACKMOUSEEVENT track{ sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
			TrackMouseEvent(&track);
			return 0;
		}
		case WM_LBUTTONDOWN:
			st->pressed = btnAt(*st, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
			return 0;
		case WM_LBUTTONUP: {
			const int at = btnAt(*st, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
			if (at >= 0 && at == st->pressed) finish(*st, st->btns[at].id);
			st->pressed = -1;
			return 0;
		}
		case WM_KEYDOWN: {
			const int count = (int)st->btns.size();
			if (wp == VK_RETURN || wp == VK_SPACE) finish(*st, st->btns[st->focus].id);
			else if (wp == VK_ESCAPE) finish(*st, cancelId(*st));
			else if (wp == VK_LEFT || (wp == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000))) st->focus = (st->focus + count - 1) % count;
			else if (wp == VK_RIGHT || wp == VK_TAB) st->focus = (st->focus + 1) % count;
			else if (wp == 'Y' || wp == 'N') {
				for (auto& btn : st->btns) {
					if (btn.id == (wp == 'Y' ? IDYES : IDNO)) finish(*st, btn.id);
				}
			}
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		case WM_CLOSE:
			finish(*st, cancelId(*st));
			return 0;
		case WM_DESTROY:
			// 主人窗口被关掉时系统会连带销毁这个框（比如 OCR 结果框开着，用户点了贴图工具条上的 ×）。
			// 不在这里收尾的话 done 永远等不到，下面的消息循环就一直空转，进程退不掉
			if (!st->done) finish(*st, cancelId(*st));
			return 0;
		}
		return DefWindowProc(hwnd, msg, wp, lp);
	}
}

int UUMessageBox(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags)
{
	State st;
	st.text = text ? text : L"";
	st.title = title && *title ? title : L"UU截图";
	st.flags = flags;
	const UINT type = flags & MB_TYPEMASK;
	auto add = [&](int id, const wchar_t* key) { st.btns.push_back(Btn{ id, Lang::get(key) }); };
	if (type == MB_OKCANCEL) { add(IDOK, L"msgbox.ok"); add(IDCANCEL, L"msgbox.cancel"); }
	else if (type == MB_YESNO) { add(IDYES, L"msgbox.yes"); add(IDNO, L"msgbox.no"); }
	else if (type == MB_YESNOCANCEL) { add(IDYES, L"msgbox.yes"); add(IDNO, L"msgbox.no"); add(IDCANCEL, L"msgbox.cancel"); }
	else add(IDOK, L"msgbox.ok");
	const UINT def = flags & MB_DEFMASK;
	st.focus = def == MB_DEFBUTTON2 ? 1 : def == MB_DEFBUTTON3 ? 2 : 0;
	if (st.focus >= (int)st.btns.size()) st.focus = 0;

	static const wchar_t* className = L"UUMessageBox";
	static bool registered{ false };
	auto inst = GetModuleHandle(nullptr);
	if (!registered) {
		WNDCLASS wc{};
		wc.style = CS_DROPSHADOW;
		wc.lpfnWndProc = wndProc;
		wc.hInstance = inst;
		wc.lpszClassName = className;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		RegisterClass(&wc);
		registered = true;
	}
	if (owner && (!IsWindow(owner) || !IsWindowVisible(owner))) owner = nullptr;
	// 摆在哪块屏幕上：有主人就跟主人，没有就跟光标
	POINT cursor{};
	GetCursorPos(&cursor);
	RECT ownerRect{};
	const bool hasOwnerRect = owner && GetWindowRect(owner, &ownerRect);
	auto monitor = hasOwnerRect ? MonitorFromRect(&ownerRect, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{ sizeof(MONITORINFO) };
	GetMonitorInfo(monitor, &mi);
	auto& work = mi.rcWork;
	auto hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, className, st.title.c_str(), WS_POPUP,
		work.left, work.top, 100, 100, owner, nullptr, inst, nullptr);
	if (!hwnd) return MessageBoxW(owner, text, title, flags);   //窗口都建不出来：退回系统的
	st.s = GetDpiForWindow(hwnd) / 96.f;
	layoutAll(st, (work.bottom - work.top) * 0.6f);
	// 屏幕正中偏上一点；有主人且放得下就在主人正中
	int posX = work.left + ((work.right - work.left) - (int)st.w) / 2;
	int posY = work.top + ((work.bottom - work.top) - (int)st.h) * 2 / 5;
	if (hasOwnerRect && ownerRect.right - ownerRect.left >= (int)st.w) {
		posX = ownerRect.left + ((ownerRect.right - ownerRect.left) - (int)st.w) / 2;
		posY = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - (int)st.h) * 2 / 5;
	}
	posX = (std::max)((int)work.left, (std::min)(posX, (int)work.right - (int)st.w));
	posY = (std::max)((int)work.top, (std::min)(posY, (int)work.bottom - (int)st.h));
	SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, (int)st.w, (int)st.h, SWP_NOACTIVATE);
	// Windows 11 上给个圆角（老系统不认这个属性，忽略就是了）
	const int corner = 2;   // DWMWCP_ROUND
	DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
	SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));

	if (owner) EnableWindow(owner, FALSE);   //弹着的时候主人不能点
	ShowWindow(hwnd, SW_SHOW);
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	MSG msg{};
	while (!st.done) {
		const BOOL got = GetMessage(&msg, nullptr, 0, 0);
		if (got <= 0) {
			// 程序要退了：把退出消息放回去，让外面的消息循环接着收
			if (got == 0) PostQuitMessage((int)msg.wParam);
			break;
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	if (owner) EnableWindow(owner, TRUE);
	SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
	st.rt.Reset();
	DestroyWindow(hwnd);
	if (owner) SetForegroundWindow(owner);
	return st.result != 0 ? st.result : cancelId(st);
}
