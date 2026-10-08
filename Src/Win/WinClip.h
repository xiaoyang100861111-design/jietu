#pragma once
#include <include/Ling.h>
#include <unordered_map>
#include "../ClipHistory.h"

// 快捷话术 / 剪切板历史 共用的面板，顶上两个页签切换。整块内容自己画在一张画布上（同 WinCap / WinPin）。
// 点一条（或回车）把它写回剪切板并粘贴到原来的窗口；右键只复制不粘贴。直接打字就是搜索。
// 两种出现方式：快捷键 / 托盘菜单弹在光标旁边，失去焦点就关；
// 贴边模式下鼠标顶到屏幕边缘滑出来（同老版 QQ），鼠标移开自动收回。
class WinClip : public Ling::WinBase
{
public:
	enum Mode { Phrase = 0, Clip = 1 };
	~WinClip();
	// 没开就按 mode 打开；开着且就是这个页签就关掉，是另一个页签就切过去
	static void toggle(int mode);
	// 贴边滑出。edge：1 左 2 上 3 右 4 下
	static void openDocked(int edge);
	// 按设置里的贴边选项启停"鼠标顶边"的监视。启动时、改设置后调
	static void applyDock();
	// 退出流程里调，同其他窗口：别留给 CoUninitialize 之后的静态析构
	static void dispose();
private:
	// 列表里的一行。top 是相对列表内容顶部的偏移，滚动时不变
	struct Row {
		std::shared_ptr<ClipHistory::Item> item;
		float top{ 0.f }, height{ 0.f };
	};
	// 搜索框下面的分类 / 分组标签。left、right 是窗口坐标。
	// 剪切板页只有一排（按类型分）；话术页每一级分组一排，选中上一级才出现下一级，最多三排
	struct Tab {
		enum class Kind { All, Item, Plus };
		Kind kind{ Kind::Item };
		std::wstring label;
		float left{ 0.f }, right{ 0.f };
		int row{ 0 };
		bool selected{ false };
	};
	// 光标落在什么东西上。Btn0~2 是一行右边的小按钮，从右往左数
	enum class Hit { None, Mode, Tab, Clear, Row, Btn0, Btn1, Btn2, AddClip };
	// 搜索框现在在收什么：平时是搜索词，新建分组 / 改备注名时临时借用它
	enum class Input { Search, Group, Title };
	WinClip(HWND prevHwnd, int mode, int edge);
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 四条边可以拖着改大小，改完的尺寸关窗时记进配置
	LRESULT onHitTest(const POINT pos) override;
	// 按当前的页签、分类和搜索词重新筛一遍
	void rebuild();
	void setMode(int val);
	void paint(ID2D1DeviceContext* ctx);
	void paintRow(ID2D1DeviceContext* ctx, const Row& row, int index, float y);
	void drawText(ID2D1DeviceContext* ctx, const std::wstring& text, float fontSize, const D2D1_RECT_F& rect,
		UINT rgb, bool hCenter = false, bool vCenter = false);
	float textWidth(const std::wstring& text, float fontSize);
	Hit hitTest(POINT pos, int& index);
	void onDown(POINT pos, bool isRight);
	void onMove(POINT pos);
	void onWheel(float space);
	void onKey(UINT key);
	void onCharInput(UINT code);
	void onTick(UINT id);
	void scrollIntoView(int index);
	void clampScroll();
	// 把这一条写回剪切板并关窗；paste 为 true 时再替用户粘贴到原来的窗口
	void activate(int index, bool paste);
	// 进入 / 退出"借搜索框输入"的状态
	void beginInput(Input kind, long long id = 0, const std::wstring& init = L"");
	void endInput(bool commit);
	// 底部弹一句提示，过一会儿自己消失
	void toast(const std::wstring& text);
	// 关窗一律排进消息队列：很多入口是在失焦 / 鼠标回调里，原地销毁窗口不安全
	void requestClose();
	ID2D1Bitmap1* getThumb(const ClipHistory::Item& item);
	float listTop() const;
	float listBottom() const;
	// 当前选中的话术分组的完整路径，一级都没选是空串
	std::wstring curGroup() const;
	// path 的前 depth 级拼成的路径
	std::wstring groupPrefix(int depth) const;
	D2D1_RECT_F btnRect(float rowY, float rowH, int which) const;
private:
	// 弹出面板之前在前台的窗口，粘贴要粘回它那儿去
	HWND prevHwnd{ nullptr };
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	std::vector<Row> rows;
	std::vector<Tab> tabs;
	float contentH{ 0.f }, scrollY{ 0.f };
	int mode{ Phrase };
	int tab{ 0 };            // 剪切板页的分类：0 全部 1 文本 2 图片 3 文件 4 收藏
	// 话术页当前选中的分组，一级一个名字，最多三级。空 = 全部
	std::vector<std::wstring> path;
	int tabRows{ 1 };        // 标签现在有几排，列表的起始位置跟着它走
	int selRow{ 0 };         // 键盘选中的行
	int hoverIndex{ -1 };
	Hit hover{ Hit::None };
	std::wstring query;      // 搜索词
	Input input{ Input::Search };
	std::wstring inputText;  // 新建分组 / 改备注名时正在输入的字
	long long inputId{ 0 };  // 改备注名时是哪一条
	std::wstring inputParent; // 新建分组时建在哪个分组下面（空 = 一级分组）
	std::wstring toastText;
	bool closing{ false };
	// 输入框的光标：窗口有键盘焦点时才画，靠定时器一亮一灭
	bool focused{ false }, caretOn{ true };
	// 用户拖过窗口边改了大小（建窗口时那一下 WM_SIZE 不算）
	bool created{ false }, resized{ false };
	// 贴边滑出的：0 不是。鼠标移开就收，靠定时器看光标位置
	int dockEdge{ 0 };
	bool mouseEntered{ false };
	int outTicks{ 0 }, graceTicks{ 0 };
	// 缩略图，按记录 id 缓存。只活在这个窗口的生命期里（窗口全关掉时 D2D 设备会被销毁）
	std::unordered_map<long long, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> thumbs;
};
