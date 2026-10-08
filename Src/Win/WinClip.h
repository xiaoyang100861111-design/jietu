#pragma once
#include <include/Ling.h>
#include <unordered_map>
#include "../ClipHistory.h"

// 剪切板历史面板。在光标旁边弹出来，整块内容自己画在一张画布上（同 WinCap / WinPin）。
// 直接打字就是搜索；点一条（或回车）把它写回剪切板并粘贴到原来的窗口；右键只复制不粘贴。
// 失去焦点自动关掉。
class WinClip : public Ling::WinBase
{
public:
	~WinClip();
	// 开着就关掉，没开就打开
	static void toggle();
	// 退出流程里调，同其他窗口：别留给 CoUninitialize 之后的静态析构
	static void dispose();
private:
	// 列表里的一行。top 是相对列表内容顶部的偏移，滚动时不变
	struct Row {
		std::shared_ptr<ClipHistory::Item> item;
		float top{ 0.f }, height{ 0.f };
	};
	// 光标落在什么东西上
	enum class Hit { None, Tab, Clear, Row, Pin, Del };
	WinClip(HWND prevHwnd);
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 按当前的分类和搜索词重新筛一遍
	void rebuild();
	void paint(ID2D1DeviceContext* ctx);
	void paintRow(ID2D1DeviceContext* ctx, const Row& row, int index, float y);
	void drawText(ID2D1DeviceContext* ctx, const std::wstring& text, float fontSize, const D2D1_RECT_F& rect,
		UINT rgb, bool hCenter = false, bool vCenter = false);
	Hit hitTest(POINT pos, int& index);
	void onDown(POINT pos, bool isRight);
	void onMove(POINT pos);
	void onWheel(float space);
	void onKey(UINT key);
	void onCharInput(UINT code);
	void scrollIntoView(int index);
	void clampScroll();
	// 把这一条写回剪切板并关窗；paste 为 true 时再替用户粘贴到原来的窗口
	void activate(int index, bool paste);
	// 关窗一律排进消息队列：很多入口是在失焦 / 鼠标回调里，原地销毁窗口不安全
	void requestClose();
	ID2D1Bitmap1* getThumb(const ClipHistory::Item& item);
	float listTop() const;
	float listBottom() const;
	// 一行右边的两个小按钮（收藏、删除）
	D2D1_RECT_F btnRect(float rowY, float rowH, int which) const;
private:
	// 弹出面板之前在前台的窗口，粘贴要粘回它那儿去
	HWND prevHwnd{ nullptr };
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	std::vector<Row> rows;
	float contentH{ 0.f }, scrollY{ 0.f };
	int tab{ 0 };            // 0 全部 1 文本 2 图片 3 文件 4 收藏
	int selRow{ 0 };         // 键盘选中的行
	int hoverIndex{ -1 };
	Hit hover{ Hit::None };
	std::wstring query;
	bool closing{ false };
	// 缩略图，按记录 id 缓存。只活在这个窗口的生命期里（窗口全关掉时 D2D 设备会被销毁）
	std::unordered_map<long long, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> thumbs;
};
