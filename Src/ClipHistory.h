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
	// 把这一条重新写回系统剪切板，并挪到最前面
	bool writeToClipboard(long long id);
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
	void trim();
	void load();
	void save();
	void notify();
private:
	HWND hwnd{ nullptr };
	std::vector<std::shared_ptr<Item>> items;
	std::filesystem::path dir;
	// 自己往剪切板写东西之后的一小段时间内不记录，免得把刚写回去的那条又记一遍
	ULONGLONG ignoreUntil{ 0 };
	int retry{ 0 };
};
