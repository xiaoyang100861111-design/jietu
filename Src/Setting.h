#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
using namespace winrt::Windows::Data::Json;

class Setting
{
public:
	~Setting();
	static void init();
	// 必须在 CoUninitialize 之前调用：configObj 是 WinRT 对象，晚一步释放就是野内存
	static void dispose();
	static Setting* get();
	std::filesystem::path getDataPath();
	const JsonObject getConfigObj();
	void setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys);
	std::wstring getShortcutKey(const std::wstring& type);
	// 主题色（选区边框、贴图窗口边框、设置窗口的高亮），0xRRGGBB
	UINT getThemeColor();
	void setThemeColor(UINT rgb);
	// 不按键盘、用鼠标键触发截图：0 关闭，1 中键，2 侧键（后退），3 侧键（前进）。
	// 按键盘上的 Alt 会把已经弹出来的右键菜单关掉，想截菜单时用它
	// 要不要记录剪切板历史，默认记
	bool getClipEnabled();
	void setClipEnabled(bool val);
	// 快捷话术面板贴边：0 关闭，1 左，2 上，3 右，4 下。鼠标顶到那条屏幕边上面板就滑出来
	// 话术 / 剪切板面板的尺寸（逻辑像素）。高度 0 表示没调过：占满屏幕工作区的整个高度
	std::pair<float, float> getClipSize();
	void setClipSize(float w, float h);
	// 面板的"固定"开关：固定后贴边的面板不会自己缩回去，用完一条话术也不关
	bool getClipPinned();
	void setClipPinned(bool val);
	// 面板上次摆在哪（屏幕坐标）。没摆过返回 false
	bool getClipPos(int& x, int& y);
	void setClipPos(int x, int y);
	// 话术页上次选中的分组（完整路径），下次打开还停在那儿
	std::wstring getPhrasePath();
	void setPhrasePath(const std::wstring& path);
	int getDockEdge();
	void setDockEdge(int val);
	int getMouseTrigger();
	void setMouseTrigger(int val);
	void setAutoStart(bool autoStart);
	bool getAutoStart();
	std::wstring getLang();
	// 截图翻译的目标语言（config.json 里的 translate.target，微软翻译的语言代码，如 zh-Hans / en / ja）。
	// 没配或者配成 auto 就返回 auto：跟着界面语言走，由 Translate 自己换算
	std::wstring getTranslateTarget();
	void setLang(const std::wstring& lang);
	void initShortcutKeys();
	// 贴图窗口子工具栏（ToolSub）的状态。每个工具在 config.json 的 toolPin 下各占一组，
	// 组名就是 ToolMain 上的按钮 id（rect / ellipse / ... / eraser），键名由调用方给
	// （fill、width、colorIndex 之类，各工具语义不同）。
	// 取不到就返回 def —— 老版本的配置文件里没有这些键，用户手工改坏了也算取不到，都不该抛异常。
	// set 一律立即落盘：用户调一次工具状态就得记住一次。
	bool getToolFlag(const std::wstring& tool, const std::wstring& key, bool def);
	void setToolFlag(const std::wstring& tool, const std::wstring& key, bool val);
	float getToolNum(const std::wstring& tool, const std::wstring& key, float def);
	void setToolNum(const std::wstring& tool, const std::wstring& key, float val);
	// 上次检查更新是哪一天（std::chrono::days 的计数，即 1970-01-01 以来的天数），
	// 从来没查过返回 0。一天最多查一次服务端，靠它记账 —— 每次空闲都去请求纯属浪费人家的流量
	long long getUpdateCheckDay();
	void setUpdateCheckDay(long long day);
private:
	Setting();
	// toolPin.<tool> 那个 JsonObject。缺哪一层就现建一层挂上去 ——
	// SetNamedValue 得有个落脚的对象，而这两层在旧配置文件里都不存在
	JsonObject getToolObj(const std::wstring& tool);
	std::filesystem::path initDataPath();
	// 决定配置文件用哪一份：exe 同目录有 config.json 就用它（绿色版，配置跟着程序走），
	// 否则用 %appdata%\ScreenCapture\config.json。二者只认一个，读哪儿就写哪儿。
	std::filesystem::path initConfigPath();
	void save();
private:
	const std::filesystem::path dataPath;
	// 必须声明在 dataPath 之后：initConfigPath 找不到 exe 同目录的配置时要回落到 dataPath 上，
	// 成员按声明顺序初始化
	const std::filesystem::path configPath;
	JsonObject configObj;
};

