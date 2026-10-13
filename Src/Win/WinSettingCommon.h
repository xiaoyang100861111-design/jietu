#pragma once
#include <include/Ling.h>
#include <unordered_map>
#include <functional>
#include <string>
#include <utility>
#include <vector>
class WinSettingCommon:public Ling::Node
{
public:
	WinSettingCommon(Ling::WinBase* parent);
	~WinSettingCommon();
	// 收掉语言下拉框。它挂在 win->body 上而不是挂在本节点里，所以本节点被换掉 / 销毁时
	// 它不会跟着走，得由外面在合适的时机显式收掉
	void hideSelectBox();
private:
	void initAutoStartCtrls();
	void initLangCtrls();
	void initThemeCtrls();
	void initDarkCtrls();
	void initOpacityCtrls();
	// 鼠标在滑块上的横坐标 → 不透明度，更新滑块和面板；save 为 true 时存进配置
	void setOpacityAt(LONG posX, bool save);
	void initClipCtrls();
	void initUpdateCtrls();
	void initEngineCtrls();
	void initDockCtrls();
	void initWidthCtrls(const std::wstring& tool);
	// 颜色设置（target：theme 主题色，rect / ellipse 该工具的默认色）：
	// 一个色块，点开系统调色板；旁边是十六进制色值，点一下可以直接敲（或 Ctrl+V 粘贴）#RRGGBB
	void makeColorCtrls(Ling::Node* box, const std::wstring& target);
	UINT getColor(const std::wstring& target);
	void setColor(const std::wstring& target, UINT rgb);
	void showHexInput();
	void endHexInput(bool commit);
	void onHexChar(UINT code);
	void onHexKey(UINT key);
	void setAutoStartBtn(Ling::Button* btn);
	void showSelectBox(Ling::Button* btn);
	void showOptions(Ling::Button* btn, const std::vector<std::pair<std::wstring, std::wstring>>& options,
		std::function<void(const std::wstring& value)> onPick);
private:
	Ling::Button* selectBtn{ nullptr };
	// 现在开着的下拉框是挂在哪个按钮底下的
	Ling::Button* anchorBtn{ nullptr };
	Ling::ScrollerBox* selectBox{ nullptr };
	winrt::event_token onMouseDownToken;
	std::unordered_map<std::wstring, Ling::Button*> swatches, hexBtns;
	// 正在敲哪个颜色的色值（空 = 没在敲）和已经敲了的几位
	std::wstring hexTarget, hexText;
	winrt::event_token hexCharToken, hexKeyToken, hexMouseToken;
	// 透明度滑块：整条、左边已填的一段、显示数值的字；是不是正拖着
	Ling::Node* opacityBar{ nullptr };
	Ling::Node* opacityFill{ nullptr };
	Ling::Label* opacityLabel{ nullptr };
	bool opacityDragging{ false };
	int opacityValue{ 100 };
	winrt::event_token opacityDownToken, opacityMoveToken, opacityUpToken;
};

