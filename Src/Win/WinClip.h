#pragma once
#include <include/Ling.h>
#include <unordered_map>
#include "../ClipHistory.h"

// 快捷话术 / 剪切板历史 共用的面板，顶上两个页签切换。整块内容自己画在一张画布上（同 WinCap / WinPin），
// 只有搜索框是个真正的文本框控件。
// 点一条（或回车）把它复制到系统剪切板（只复制，不替用户粘贴）。
//
// 摆放方式同老版 QQ 的主面板：拖着顶上那一条可以挪动；
//   - 拖到屏幕边上就贴边：平时缩在屏幕外面，鼠标顶到那条边滑出来，移开又缩回去；
//   - 拖离屏幕边就是个普通的悬浮窗口，一直待在那儿，直到按关闭 / Esc / 快捷键；
//   - 右上角的"固定"按下去之后：贴边的不再自己缩回去，用完一条话术面板也不关。
class WinClip : public Ling::WinBase
{
	using ctx_t = ID2D1DeviceContext*;
public:
	// 三个页签。打卡排最前面（叫 Work 是为了不和 Punch 这个类重名）
	enum Mode { Work = 0, Ledger = 1, Phrase = 2, Clip = 3 };
	~WinClip();
	// 快捷键 / 托盘菜单：没开就按 mode 打开；开着且就是这个页签就关掉，是另一个页签就切过去
	static void toggle(int mode);
	// 鼠标顶到屏幕边触发的贴边滑出。edge：1 左 2 上 3 右 4 下
	static void openDocked(int edge);
	// 点托盘图标：没开就打开（停在上次的页签），开着就提到最前面闪一下
	// （不能叫 show：那会把基类的 show() 盖住，onCreated 里显示窗口的那句就变成调它，没完没了地建窗口）
	static void popup();
	// 软件启动时：面板是"固定"的话直接显示出来（不抢焦点）
	static void showAtStartup();
	// 按设置里的贴边选项启停"鼠标顶边"的监视。启动时、改设置后调
	static void applyDock();
	// 别的窗口（添加 / 编辑话术的小窗口）开着的时候面板会失焦，这期间不许它缩回去
	static void setModal(bool val);
	// 打卡的活动超时了：把面板叫出来切到打卡页，红字提示并闪一下
	static void alertPunch(const std::wstring& text);
	// 退出流程里调，同其他窗口：别留给 CoUninitialize 之后的静态析构
	static void dispose();
private:
	// 列表里的一行。top 是相对列表内容顶部的偏移，滚动时不变
	struct Row {
		std::shared_ptr<ClipHistory::Item> item;
		float top{ 0.f }, height{ 0.f };
	};
	// 搜索框下面的分类 / 分组标签。left、right 是窗口坐标。
	// 剪切板页只有一排（按类型分）；话术页每一级分组一排，最多三排
	struct Tab {
		enum class Kind { Item, Plus };
		Kind kind{ Kind::Item };
		std::wstring label;
		std::wstring full;      // 话术分组的完整路径
		float left{ 0.f }, right{ 0.f };
		int row{ 0 };
		bool selected{ false };
	};
	// 光标落在什么东西上。Btn0~2 是一行右边的小按钮，Top0~2 是窗口右上角的按钮，都从右往左数
	enum class Hit { None, Mode, Tab, Clear, Row, Btn0, Btn1, Btn2, Top0, Top1, Top2, Top3, PunchBtn };
	// 搜索框现在在收什么：平时是搜索词，新建 / 重命名分组时临时借用它
	enum class Input { Search, Group, Rename };
	WinClip(HWND prevHwnd, int mode, int edge, bool byHotkey);
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	// 四条边可以拖着改大小；顶上那一条（页签和按钮之外的空白）可以拖着挪窗口
	LRESULT onHitTest(const POINT pos) override;
	// 按当前的页签、分类和搜索词重新筛一遍
	void rebuild();
	void setMode(int val);
	void paint(ID2D1DeviceContext* ctx);
	void paintRow(ID2D1DeviceContext* ctx, const Row& row, int index, float y);
	// 打卡页整页的内容：状态卡、操作按钮、今日统计、今日记录
	void paintPunch(ID2D1DeviceContext* ctx);
	void paintToast(ID2D1DeviceContext* ctx);
	// 点了打卡页上的某个按钮。id：in 上班 / out 下班 / back 回座 / act:<活动 id>
	void onPunchClick(const std::wstring& id);
	// —— 记账页 ——
	// 页面内容：我所在的小组（标签）、常用操作按钮、服务端回的账单 / 文字
	void paintLedger(ctx_t ctx);
	void onLedgerClick(const std::wstring& id);
	// 取我所在的记账小组；取当日账单；发一条指令（和原来在群里发消息一样）
	void loadLedgerGroups();
	void loadLedgerBill(bool mine);
	void loadLedgerLog();
	// 把服务端回的账单（JSON）拆进 lbill；拆不出来返回 false，这时按纯文字显示
	bool parseBill(const winrt::Windows::Data::Json::JsonObject& obj);
	// 账单自己排版：金额、线路、会员 ID 分开显示，点哪个复制哪个
	void paintBill(ctx_t ctx, const D2D1_RECT_F& card);
	void sendLedger(const std::wstring& text);
	void drawText(ID2D1DeviceContext* ctx, const std::wstring& text, float fontSize, const D2D1_RECT_F& rect,
		UINT rgb, bool hCenter = false, bool vCenter = false);
	float textWidth(const std::wstring& text, float fontSize);
	Hit hitTest(POINT pos, int& index);
	void onDown(POINT pos, bool isRight);
	void onMove(POINT pos);
	void onUp(POINT pos, bool isRight);
	void onWheel(float space);
	void onKey(UINT key);
	void onTick(UINT id);
	// 文本框里的字变了
	void onTextEdit(const std::wstring& text);
	// 改文本框的内容但不当成用户输入处理
	void setBoxText(const std::wstring& text);
	void updatePlaceholder();
	// 右键点分组弹出的菜单。full 是分组的完整路径
	void groupMenu(const std::wstring& full);
	// 连问两遍，两遍都点"是"才算数。重命名、删除分组这种改了就回不去的操作用
	bool confirmTwice(const std::wstring& first, const std::wstring& second);
	bool confirmOnce(const std::wstring& text);
	// 选中 / 取消选中某个分组，并记进配置，下次打开还停在这儿
	void selectGroup(const std::wstring& full);
	void scrollIntoView(int index);
	void clampScroll();
	// 把这一条写回系统剪切板并提示一句。paste 现在不用了：只复制，不替用户粘贴
	void activate(int index, bool paste);
	// 进入 / 退出"借搜索框输入"的状态
	void beginInput(Input kind, const std::wstring& init = L"");
	void endInput(bool commit);
	// 底部弹一句提示，过一会儿自己消失
	// warn：出错 / 警告用红字，其余用绿字
	void toast(const std::wstring& text, UINT ms = 1000, bool warn = false);
	// 关窗一律排进消息队列：很多入口是在失焦 / 鼠标回调里，原地销毁窗口不安全
	void requestClose();
	// 用户拖完窗口松手了：看看停在哪，贴边还是悬浮，并记进配置
	void onMoveEnd();
	ID2D1Bitmap1* getThumb(const ClipHistory::Item& item);
	float listTop() const;
	float listBottom() const;
	// 当前选中的话术分组的完整路径，一级都没选是空串
	std::wstring curGroup() const;
	D2D1_RECT_F btnRect(float rowY, float rowH, int which) const;
	D2D1_RECT_F topBtnRect(int which) const;
	// 屏幕边缘外面、刚好看不见的那个位置（base 是贴边停好的位置）
	POINT hiddenPos(POINT base) const;
private:
	// 粘贴要粘回去的那个窗口：面板之外、最近一次在前台的窗口
	HWND prevHwnd{ nullptr };
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	// 搜索 / 输入用的文本框，是个真正的输入控件（输入法、光标、选中、粘贴都归它管）
	Ling::TextBox* textBox{ nullptr };
	// 正在用代码改文本框的内容，这期间的 onTextChanged 不算用户输入
	bool syncing{ false };
	std::vector<Row> rows;
	std::vector<Tab> tabs;
	float contentH{ 0.f }, scrollY{ 0.f };
	int mode{ Phrase };
	int tab{ 0 };            // 剪切板页的分类：0 全部 1 文本 2 图片 3 文件 4 收藏
	// 话术页当前选中的分组，一级一个名字，最多三级。空 = 没选（显示全部话术）
	std::vector<std::wstring> path;
	int tabRows{ 1 };        // 标签现在有几排，列表的起始位置跟着它走
	int selRow{ 0 };         // 键盘选中的行
	int hoverIndex{ -1 };
	Hit hover{ Hit::None };
	std::wstring query;      // 搜索词
	Input input{ Input::Search };
	std::wstring inputText;  // 新建 / 重命名分组时正在输入的字
	std::wstring inputParent; // 新建 / 重命名分组时，它的上一级分组（空 = 一级分组）
	std::wstring inputOld;    // 重命名分组时它原来的名字
	std::wstring toastText;
	bool toastWarn{ false };
	// 打卡页上这一帧画出来的按钮（位置每帧算，点击时照着这张表认）
	struct PunchBtn {
		D2D1_RECT_F rect{};
		std::wstring id;
		bool enabled{ true };
	};
	std::vector<PunchBtn> punchBtns;
	// 记账页的状态。小组和回复都是从服务端取的
	struct LGroup {
		long long id{ 0 };
		std::wstring name, role;
		bool allOperator{ false };
	};
	std::vector<LGroup> lgroups;
	long long lgroupId{ 0 };
	bool ledgerLoaded{ false }, ledgerBusy{ false }, ledgerFailed{ false };
	std::wstring ledgerText;      // 服务端回的文字（帮助、配置、日志这些），原样显示
	// 账单的一笔 / 一整张。有账单时（ledgerHasBill）按它排版，不用上面那段文字
	struct LRow {
		std::wstring id, time, amount, actor, line, tag, rest;
	};
	struct LBill {
		std::wstring title, sub, totalIn, totalOut, net, extra;
		int inCount{ 0 }, outCount{ 0 };
		std::vector<LRow> incomes, expenses;
	};
	LBill lbill;
	bool ledgerHasBill{ false };
	std::wstring ledgerInput;     // 输入框里正在写的指令
	float ledgerTop{ 0.f };       // 回复区域从哪儿开始（按钮排了几行不一定）
	bool closing{ false };
	// 正开着菜单 / 确认框 / 编辑窗口：这期间面板会失焦，但不能因此缩回去
	bool modal{ false };
	// 右上角的"固定"
	bool pinned{ false };
	// 是快捷键 / 菜单叫出来的（拿了键盘焦点），不是鼠标顶边滑出来的
	bool byHotkey{ false };
	// 用户拖过窗口边改了大小（建窗口时那一下 WM_SIZE 不算）
	bool created{ false }, resized{ false };
	// 在分组标签上按下了左键还没松：松开时没拖动过算点击（选中 / 取消选中），
	// 拖到同一级的另一个分组上就是和它换位置
	bool tabPressed{ false }, tabDragged{ false };
	std::wstring pressFull;
	POINT pressPos{ 0, 0 };
	// 贴在哪条屏幕边上：0 没贴（悬浮）。贴边且没固定时鼠标移开就缩回去
	int dockEdge{ 0 };
	bool mouseEntered{ false };
	int outTicks{ 0 }, graceTicks{ 0 };
	// 滑进 / 滑出的动画：窗口在 from 和 to 两个位置之间挪，animStep 是走到第几步了。
	// animating 期间窗口位置是程序在改，不是用户在拖
	bool animOut{ false }, animating{ false };
	int animStep{ 0 };
	POINT animFrom{ 0, 0 }, animTo{ 0, 0 };
	// 缩略图，按记录 id 缓存。只活在这个窗口的生命期里（窗口全关掉时 D2D 设备会被销毁）
	std::unordered_map<long long, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> thumbs;
};
