#pragma once
#include <algorithm>
#include <cstdint>
#include "Setting.h"

// 白天 / 黑夜两套配色。界面里的颜色还是照白天的写（0xFFFFFF 白底、0x222222 深字……），
// 用的时候过一下这里：白天原样返回，黑夜把灰阶换成深色的那一套，彩色（主题色、红、绿、标签色）不动。
namespace Theme {
	inline bool dark() { return Setting::get()->getDarkMode(); }
	// 底色、边框、普通文字
	inline uint32_t fill(uint32_t rgb)
	{
		if (!dark()) return rgb;
		const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
		const int hi = (std::max)({ r, g, b }), lo = (std::min)({ r, g, b });
		if (hi - lo > 16) return rgb;   // 彩色：不动
		const int level = (r + g + b) / 3;
		int out;
		if (level >= 240) out = 0x1E + (level - 240) * (0x2D - 0x1E) / 15;        // 窗口底 / 卡片底：深灰，卡片比窗口底亮一点
		else if (level >= 192) out = 0x38 + (level - 192) * (0x4A - 0x38) / 47;   // 边框、分隔线、滚动条槽
		else out = 255 - level / 2;                                               // 文字：深字变浅字
		return (uint32_t)((out << 16) | (out << 8) | (std::min)(255, out + 2));
	}
	// 文字专用：纯白是写在彩色按钮上的字，黑夜里也还是白的
	inline uint32_t text(uint32_t rgb) { return (rgb & 0xFFFFFF) == 0xFFFFFF ? rgb : fill(rgb); }
	// 下面两个给 Ling 的控件用，颜色是 0xRRGGBBAA（带透明度）
	inline uint32_t c(uint32_t rgba) { return (fill(rgba >> 8) << 8) | (rgba & 0xFF); }
	inline uint32_t t(uint32_t rgba) { return (text(rgba >> 8) << 8) | (rgba & 0xFF); }
}
