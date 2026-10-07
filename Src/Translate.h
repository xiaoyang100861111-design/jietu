#pragma once
#include <include/Ling.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 截图翻译 / 内置文字识别。
// 文字和位置由系统自带的 OCR（Windows.Media.Ocr）认出来，不带任何模型文件、也不起外部进程；
// 译文走在线接口（微软 Edge 翻译，不通再试 Google），拿回来之后盖回原图上对应的位置。
// 像素格式与 Util 一致：BGRA、top-down、行紧凑（步长 = w*4）。
class Translate
{
public:
	// 一个段落：相邻的几行正文并成一段再翻译，句子才不会被折行拦腰截断
	struct Block {
		D2D1_RECT_F rect{};     // 段落在图里的包围盒（图像像素）
		float lineH{ 0.f };     // 原文单行的平均高度，译文字号按它来定
		std::wstring src, dst;  // 原文 / 译文
	};
	struct Result {
		bool ok{ false };
		std::wstring err;       // 失败时给用户看的话（已经是当前界面语言的文案）
		std::wstring text;      // 识别出的全文，段落之间换行
		std::vector<Block> blocks;
	};
	using Callback = std::function<void(std::shared_ptr<Result>)>;
	// 必须在 UI 线程调。活在后台线程干，干完回到 UI 线程调 cb（无论成败都会调，且只调一次）。
	// needTranslate 为 false 时只做文字识别，blocks 里的 dst 是空的
	static void start(int w, int h, const std::vector<BYTE>& pixels, bool needTranslate, Callback cb);
	// 把译文盖到 pixels 上（原地改）。要用 D2D 设备，只能在 UI 线程调
	static bool render(int w, int h, std::vector<BYTE>& pixels, const std::vector<Block>& blocks);
};
