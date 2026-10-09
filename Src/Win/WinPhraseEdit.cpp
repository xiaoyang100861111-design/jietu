#include "pch.h"
#include <algorithm>
#include "WinPhraseEdit.h"
#include "WinClip.h"
#include "../Lang.h"
#include "../Setting.h"

namespace {
	std::unique_ptr<WinPhraseEdit> winEdit;

	// 文本框是多行的，备注只要一行：换行压成空格，再掐掉两头的空白
	std::wstring oneLine(const std::wstring& text)
	{
		std::wstring result;
		for (auto c : text) {
			if (c == L'\r') continue;
			result += c == L'\n' ? L' ' : c;
		}
		const auto from = result.find_first_not_of(L" \t");
		if (from == std::wstring::npos) return L"";
		const auto to = result.find_last_not_of(L" \t");
		return result.substr(from, to - from + 1);
	}

	// 文本框里的换行是单个 \n，粘贴到记事本、聊天窗口这些地方要 \r\n 才是换行
	std::wstring toCrLf(const std::wstring& text)
	{
		std::wstring result;
		for (auto c : text) {
			if (c == L'\r') continue;
			if (c == L'\n') result += L'\r';
			result += c;
		}
		return result;
	}

	std::wstring fromCrLf(const std::wstring& text)
	{
		std::wstring result;
		for (auto c : text) {
			if (c != L'\r') result += c;
		}
		return result;
	}
}

WinPhraseEdit::WinPhraseEdit(const std::wstring& group, long long editId) : Ling::WinBase(), group{ group }, editId{ editId }
{
	// 同 WinSetting：关窗多半是从自己身上的按钮回调里进来的，C++ 对象推迟到下一轮消息循环再放
	onDestroy.add([]() {
		WinClip::setModal(false);
		Ling::App::get()->dq.TryEnqueue([]() { winEdit.reset(); });
	});
	onKeyDown.add([this](UINT key) {
		if (key == VK_ESCAPE) close();
		// Ctrl+回车保存（单按回车在内容框里是换行）
		else if (key == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) save();
	});
	setTitle(L"UU截图");
	setSize(480.f, 440.f);
	setCenter();
	createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP);
}

WinPhraseEdit::~WinPhraseEdit()
{
}

void WinPhraseEdit::open(const std::wstring& group, long long editId)
{
	if (winEdit) {
		SetForegroundWindow(winEdit->hwnd);
		return;
	}
	if (!ClipHistory::get()) return;
	// 编辑窗口开着的时候话术面板会失焦，不能让它因此缩回去
	WinClip::setModal(true);
	winEdit.reset(new WinPhraseEdit(group, editId));
}

void WinPhraseEdit::dispose()
{
	winEdit.reset();
}

void WinPhraseEdit::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

LRESULT WinPhraseEdit::onHitTest(const POINT pos)
{
	POINT pt{ pos };
	ScreenToClient(hwnd, &pt);
	// 顶上 44 像素高的一条是标题栏，右上角的关闭按钮除外
	if (pt.y >= 0 && pt.y < (LONG)(44.f * dpi) && pt.x < (LONG)(w - 60.f * dpi)) return HTCAPTION;
	return HTCLIENT;
}

void WinPhraseEdit::onCreated()
{
	enableShadow();
	const uint32_t theme = (Setting::get()->getThemeColor() << 8) | 0xFF;
	std::shared_ptr<ClipHistory::Item> editing;
	if (editId != 0) {
		for (auto& phrase : ClipHistory::get()->getPhrases()) {
			if (phrase->id == editId) editing = phrase;
		}
	}
	if (editing) type = editing->type;

	body->setBg(0xFAFAFAFF);
	body->setFlexDirection(Ling::FlexDirection::Column);
	body->setPadding(18.f, 8.f, 18.f, 16.f);

	// 标题栏
	auto header = body->makeChild<Ling::Node>();
	header->setHeight(36.f);
	header->setFlexDirection(Ling::FlexDirection::Row);
	header->setAlignItems(Ling::Align::Center);
	auto caption = header->makeChild<Ling::Label>();
	caption->setText(Lang::get(editing ? L"clip.editTitleEdit" : L"clip.editTitleNew"));
	caption->setFontSize(15.f);
	caption->setHeightPercent(100.f);
	caption->setJustifyContent(Ling::Justify::Center);
	caption->setFlexGrow(1.f);
	auto closeBtn = header->makeChild<Ling::Button>();
	closeBtn->setSize(36.f, 28.f);
	closeBtn->setText(L"\ue62d");
	closeBtn->setFontFamily(L"icon");
	closeBtn->setHoverColor(0xFFFFFFFF);
	closeBtn->setHoverBg(0xE81123FF);
	closeBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });

	auto makeLabel = [this](const std::wstring& text) {
		auto label = body->makeChild<Ling::Label>();
		label->setText(text);
		label->setHeight(28.f);
		label->setJustifyContent(Ling::Justify::Center);
		label->setColor(0x666666FF);
		return label;
	};
	auto makeBox = [this]() {
		auto box = body->makeChild<Ling::TextBox>();
		box->setWidthPercent(100.f);
		box->setBg(0xFFFFFFFF);
		box->setBorder(1.f, 0xD9D9D9FF);
		box->setFontSize(13.f);
		return box;
	};

	makeLabel(Lang::get(L"clip.editNote"));
	titleBox = makeBox();
	titleBox->setHeight(32.f);
	titleBox->setPadding(8.f, 2.f, 8.f, 2.f);
	titleBox->setVerticalCenter(true);
	if (editing) titleBox->setText(editing->title);

	makeLabel(Lang::get(L"clip.editContent"));
	textBox = makeBox();
	textBox->setFlexGrow(1.f);
	textBox->setPadding(8.f);
	if (editing) {
		// 图片 / 文件类的内容不能在这里改，摆一行说明占位
		if (type == ClipHistory::Type::Text) textBox->setText(fromCrLf(editing->text));
		else textBox->setText(type == ClipHistory::Type::Image
			? std::format(L"{} {} × {}", Lang::get(L"clip.editImage"), editing->imgW, editing->imgH)
			: Lang::get(L"clip.editFiles") + L"\n" + editing->text);
	}

	// 改用剪切板里的图片 / 文件（只有新建时才有）
	auto clipRow = body->makeChild<Ling::Node>();
	clipRow->setHeight(40.f);
	clipRow->setFlexDirection(Ling::FlexDirection::Row);
	clipRow->setAlignItems(Ling::Align::Center);
	if (!editing) {
		auto clipBtn = clipRow->makeChild<Ling::Button>();
		clipBtn->setText(Lang::get(L"clip.editUseClip"));
		clipBtn->setHeight(28.f);
		clipBtn->setWidth(210.f);
		clipBtn->setBg(0xFFFFFFFF);
		clipBtn->setHoverBg(0xF2F2F2FF);
		clipBtn->setBorder(1.f, 0xD9D9D9FF);
		clipBtn->onClick.add([this](Ling::Button*) { useClipboard(); });
	}
	clipLabel = clipRow->makeChild<Ling::Label>();
	clipLabel->setHeightPercent(100.f);
	clipLabel->setJustifyContent(Ling::Justify::Center);
	clipLabel->setFlexGrow(1.f);
	clipLabel->setMarginLeft(10.f);
	clipLabel->setFontSize(12.f);
	clipLabel->setColor(0x999999FF);

	// 底部：取消 / 保存，靠右
	auto footer = body->makeChild<Ling::Node>();
	footer->setHeight(36.f);
	footer->setFlexDirection(Ling::FlexDirection::Row);
	footer->setAlignItems(Ling::Align::Center);
	auto spacer = footer->makeChild<Ling::Node>();
	spacer->setFlexGrow(1.f);
	auto cancelBtn = footer->makeChild<Ling::Button>();
	cancelBtn->setText(Lang::get(L"clip.editCancel"));
	cancelBtn->setSize(88.f, 30.f);
	cancelBtn->setBg(0xFFFFFFFF);
	cancelBtn->setHoverBg(0xF2F2F2FF);
	cancelBtn->setBorder(1.f, 0xD9D9D9FF);
	cancelBtn->onClick.add([](Ling::Button* btn) { btn->win->close(); });
	auto saveBtn = footer->makeChild<Ling::Button>();
	saveBtn->setText(Lang::get(L"clip.editSave"));
	saveBtn->setSize(88.f, 30.f);
	saveBtn->setMarginLeft(10.f);
	saveBtn->setBg(theme);
	saveBtn->setHoverBg(theme);
	saveBtn->setColor(0xFFFFFFFF);
	saveBtn->setHoverColor(0xFFFFFFFF);
	saveBtn->onClick.add([this](Ling::Button*) { save(); });

	show();
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	// 新建时光标直接进内容框，编辑时进备注框（多半是来补备注的）
	if (editing) titleBox->focus();
	else textBox->focus();
}

void WinPhraseEdit::useClipboard()
{
	ClipHistory::Item item;
	auto history = ClipHistory::get();
	// 剪切板里得是图片或文件才算数；文字的话直接粘进内容框就行了，用不着这个按钮
	if (!history || !history->peekClipboard(item) || item.type == ClipHistory::Type::Text) {
		fromClip = false;
		clipLabel->setText(Lang::get(L"clip.editClipNone"));
		return;
	}
	fromClip = true;
	type = item.type;
	if (type == ClipHistory::Type::Image) {
		clipLabel->setText(std::format(L"{} {} × {}", Lang::get(L"clip.editClipImage"), item.imgW, item.imgH));
	}
	else {
		const auto count = std::count(item.text.begin(), item.text.end(), L'\n') + 1;
		clipLabel->setText(std::format(L"{} × {}", Lang::get(L"clip.editClipFiles"), count));
	}
}

void WinPhraseEdit::save()
{
	auto history = ClipHistory::get();
	if (!history) return;
	const auto title = oneLine(titleBox->getText());
	const auto text = toCrLf(textBox->getText());
	bool ok{ false };
	if (editId != 0) {
		history->updatePhrase(editId, title, text);
		ok = true;
	}
	else if (fromClip) {
		ok = history->addPhraseFromClipboard(group, title);
		if (!ok) clipLabel->setText(Lang::get(L"clip.editClipNone"));
	}
	else {
		ok = history->addPhraseText(group, title, text);
		if (!ok) clipLabel->setText(Lang::get(L"clip.editEmpty"));
	}
	if (ok) close();
}
