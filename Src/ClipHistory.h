#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 剪切板历史：监听系统剪切板，把复制过的文本 / 图片 / 文件记下来，之后可以挑一条重新粘贴。
// 记录存在 %appdata%\ScreenCapture\clip 下：history.json 是索引，图片各存一个 <id>.png。
// 所有成员都只在 UI 线程上用。
class ClipHistory
{
public:
	enum class Type { Text = 0, Image = 1, Files = 2 };
	struct Item {
		long long id{ 0 };          // 记下来那一刻的毫秒时间戳，同时是图片的文件名
		Type type{ Type::Text };
		bool pinned{ false };       // 收藏的不会被挤掉，清空时也留着
		std::wstring text;          // Text：内容；Files：各路径用 \n 连起来；Image：空
		int imgW{ 0 }, imgH{ 0 };
		unsigned long long hash{ 0 }; // 去重用：同样的东西再复制一遍只是挪到最前面
		// 下面两项只有快捷话术用：备注名（可空）和所在分组（空 = 未分组）。
		// 分组最多三级，存的是完整路径，各级之间用 / 隔开，如 "美国/第一阶段/首充"
		std::wstring title, group;
	};
	~ClipHistory();
	static void init();
	static void dispose();
	static ClipHistory* get();
	// 新的在前
	const std::vector<std::shared_ptr<Item>>& getItems() const { return items; }
	void remove(long long id);
	// 清掉所有没收藏的
	void clear();
	void togglePin(long long id);
	// 把这一条重新写回系统剪切板。历史记录会顺便挪到最前面，话术的顺序不动
	bool writeToClipboard(long long id);
	// —— 快捷话术：常用的文字 / 图片 / 文件（语音也是文件），永久保存，按分组放 ——
	const std::vector<std::shared_ptr<Item>>& getPhrases() const { return phrases; }
	const std::vector<std::wstring>& getGroups() const { return groups; }
	// 名字为空或者已经有了返回 false
	bool addGroup(const std::wstring& name);
	// 连同下面的子分组一起删。不删话术，里面的话术挪到上一级分组（没有上一级就是未分组）
	void removeGroup(const std::wstring& name);
	// 改名：name 是完整路径，newName 只是它自己那一级的新名字。子分组和话术跟着一起改。
	// 新名字为空、含 /、或者同一级里已经有同名的，返回 false
	bool renameGroup(const std::wstring& name, const std::wstring& newName);
	// 交换两个分组的先后位置（都是完整路径，应当是同一级的兄弟）
	void swapGroups(const std::wstring& a, const std::wstring& b);
	// prefix 下面的直接子分组的名字（只是那一级的名字，不是完整路径）。prefix 为空取一级分组
	std::vector<std::wstring> getChildGroups(const std::wstring& prefix) const;
	// 所有分组的完整路径，按树的顺序（父在前，紧跟着它的子孙）
	std::vector<std::wstring> getGroupTree() const;
	// 把一条历史记录抄进话术
	bool addPhraseFromItem(long long historyId, const std::wstring& group);
	// 把系统剪切板里现在的内容存成一条话术
	bool addPhraseFromClipboard(const std::wstring& group, const std::wstring& title = L"");
	// 直接存一段文字
	bool addPhraseText(const std::wstring& group, const std::wstring& title, const std::wstring& text);
	// 改一条话术的备注名和文字（图片 / 文件类的只改备注名，text 不管）
	void updatePhrase(long long id, const std::wstring& title, const std::wstring& text);
	// 只看一眼系统剪切板里现在是什么（类型、图片尺寸、文件路径），不保存
	bool peekClipboard(Item& item);
	void removePhrase(long long id);
	void setPhraseTitle(long long id, const std::wstring& title);
	// 换分组。group 为空 = 放回未分组
	void setPhraseGroup(long long id, const std::wstring& group);
	// 把前台还给 target，稍等一下再替用户按一次 Ctrl+V
	void pasteTo(HWND target);
	std::filesystem::path getImagePath(long long id) const;
	// 解码图片文件。maxSize 非 0 时等比缩到长边不超过它（给缩略图用）；
	// premultiplied 为 true 给 D2D 位图用，false 给剪切板用。输出 BGRA、top-down、行紧凑
	static bool loadImage(const std::filesystem::path& path, UINT maxSize, bool premultiplied,
		int& w, int& h, std::vector<BYTE>& pixels);
public:
	// 记录有增删改时调，历史面板靠它刷新
	std::function<void()> onChanged;
private:
	ClipHistory();
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void capture();
	void add(std::shared_ptr<Item> item, const std::vector<BYTE>& pngBytes, int w, int h, std::vector<BYTE>& pixels);
	// 读系统剪切板里现在的内容。图片的数据放在 pngBytes 或 pixels 里（二选一），由 saveImage 落盘
	bool readClipboard(Item& item, std::vector<BYTE>& pngBytes, int& w, int& h, std::vector<BYTE>& pixels);
	bool saveImage(long long id, const std::vector<BYTE>& pngBytes, int w, int h, std::vector<BYTE>& pixels);
	// 历史和话术共用一套 id（图片文件名靠它），新 id 不能和任何一边撞
	long long newId() const;
	void loadPhrases();
	void savePhrases();
	void trim();
	void load();
	void save();
	void notify();
private:
	HWND hwnd{ nullptr };
	std::vector<std::shared_ptr<Item>> items;
	std::vector<std::shared_ptr<Item>> phrases;
	std::vector<std::wstring> groups;
	std::filesystem::path dir;
	// 自己往剪切板写东西之后的一小段时间内不记录，免得把刚写回去的那条又记一遍
	ULONGLONG ignoreUntil{ 0 };
	int retry{ 0 };
};
