#include "pch.h"
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include "ClipHistory.h"
#include "Setting.h"
#include "Util.h"

using Microsoft::WRL::ComPtr;

namespace {
	std::unique_ptr<ClipHistory> clipHistory;
	constexpr UINT_PTR timerCapture{ 1 };
	constexpr UINT_PTR timerPaste{ 2 };
	constexpr size_t maxItems{ 300 };        //没收藏的最多留这么多条
	constexpr size_t maxTextLen{ 100000 };   //再长的文本不记：多半是整份文件，记下来只会拖慢索引
	constexpr long long maxImgPixels{ 50000000 };

	unsigned long long fnv(const void* data, size_t size)
	{
		auto bytes = static_cast<const unsigned char*>(data);
		unsigned long long hash{ 14695981039346656037ULL };
		for (size_t i = 0; i < size; i++) {
			hash ^= bytes[i];
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	long long nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	bool readFiles(ClipHistory::Item& item)
	{
		if (!IsClipboardFormatAvailable(CF_HDROP)) return false;
		auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
		if (!drop) return false;
		const UINT count = DragQueryFile(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT i = 0; i < count; i++) {
			const UINT len = DragQueryFile(drop, i, nullptr, 0);
			if (len == 0) continue;
			std::wstring path(len + 1, L'\0');
			DragQueryFile(drop, i, path.data(), len + 1);
			path.resize(len);
			if (!item.text.empty()) item.text += L'\n';
			item.text += path;
		}
		if (item.text.empty()) return false;
		item.type = ClipHistory::Type::Files;
		item.hash = fnv(item.text.data(), item.text.size() * sizeof(wchar_t));
		return true;
	}

	bool readText(ClipHistory::Item& item)
	{
		if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
		auto handle = GetClipboardData(CF_UNICODETEXT);
		if (!handle) return false;
		auto ptr = static_cast<const wchar_t*>(GlobalLock(handle));
		if (!ptr) return false;
		const size_t maxLen = GlobalSize(handle) / sizeof(wchar_t);
		std::wstring text(ptr, wcsnlen(ptr, maxLen));
		GlobalUnlock(handle);
		if (text.size() > maxTextLen) return false;
		//全是空白的不记，面板上看就是一条空行
		if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return false;
		item.type = ClipHistory::Type::Text;
		item.text = std::move(text);
		item.hash = fnv(item.text.data(), item.text.size() * sizeof(wchar_t));
		return true;
	}

	// 图片优先拿 "PNG" 这个注册格式（浏览器、本程序的截图都会放，带透明通道，原样存盘就行）；
	// 没有再拿 CF_BITMAP —— 剪切板里只要有任何一种位图格式，系统都会合成出它来
	bool readImage(ClipHistory::Item& item, std::vector<BYTE>& pngBytes, int& w, int& h, std::vector<BYTE>& pixels)
	{
		static const UINT fmtPng = RegisterClipboardFormat(L"PNG");
		if (IsClipboardFormatAvailable(fmtPng)) {
			auto handle = GetClipboardData(fmtPng);
			auto ptr = handle ? static_cast<const BYTE*>(GlobalLock(handle)) : nullptr;
			if (ptr) {
				const size_t size = GlobalSize(handle);
				static const BYTE sign[8]{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
				// 宽高在 IHDR 里：文件头 8 字节 + 块长度 4 + "IHDR" 4 之后，各 4 字节大端
				if (size > 24 && memcmp(ptr, sign, 8) == 0) {
					w = (ptr[16] << 24) | (ptr[17] << 16) | (ptr[18] << 8) | ptr[19];
					h = (ptr[20] << 24) | (ptr[21] << 16) | (ptr[22] << 8) | ptr[23];
					pngBytes.assign(ptr, ptr + size);
				}
				GlobalUnlock(handle);
				if (!pngBytes.empty() && w > 0 && h > 0) {
					item.type = ClipHistory::Type::Image;
					item.imgW = w;
					item.imgH = h;
					item.hash = fnv(pngBytes.data(), pngBytes.size());
					return true;
				}
				pngBytes.clear();
			}
		}
		if (!IsClipboardFormatAvailable(CF_BITMAP)) return false;
		auto bmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
		BITMAP info{};
		if (!bmp || !GetObject(bmp, sizeof(info), &info)) return false;
		w = info.bmWidth;
		h = info.bmHeight;
		if (w <= 0 || h <= 0 || (long long)w * h > maxImgPixels) return false;
		BITMAPINFO bmi{};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = w;
		bmi.bmiHeader.biHeight = -h; //top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		pixels.resize((size_t)w * h * 4);
		auto dc = GetDC(nullptr);
		const int lines = GetDIBits(dc, bmp, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS);
		ReleaseDC(nullptr, dc);
		if (lines != h) return false;
		// GDI 位图的 alpha 没有意义（多半全 0），存成 PNG 会变成全透明，统一按不透明补上
		for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
		item.type = ClipHistory::Type::Image;
		item.imgW = w;
		item.imgH = h;
		item.hash = fnv(pixels.data(), pixels.size());
		return true;
	}

	bool setClipText(HWND owner, const std::wstring& text)
	{
		const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
		auto mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (!mem) return false;
		memcpy(GlobalLock(mem), text.c_str(), bytes);
		GlobalUnlock(mem);
		if (!OpenClipboard(owner)) {
			GlobalFree(mem);
			return false;
		}
		EmptyClipboard();
		// 成功后内存归剪切板所有，只在失败时自己释放
		const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
		if (!ok) GlobalFree(mem);
		CloseClipboard();
		return ok;
	}

	// paths：各路径用 \n 连起来。CF_HDROP 要的是 DROPFILES 头 + 一串以 \0 分隔、\0\0 结尾的路径
	bool setClipFiles(HWND owner, const std::wstring& paths)
	{
		std::wstring list = paths;
		for (auto& c : list) {
			if (c == L'\n') c = L'\0';
		}
		list += L'\0';
		const size_t listBytes = (list.size() + 1) * sizeof(wchar_t);
		auto mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + listBytes);
		if (!mem) return false;
		auto drop = static_cast<DROPFILES*>(GlobalLock(mem));
		drop->pFiles = sizeof(DROPFILES);
		drop->fWide = TRUE;
		memcpy(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
		GlobalUnlock(mem);
		if (!OpenClipboard(owner)) {
			GlobalFree(mem);
			return false;
		}
		EmptyClipboard();
		const bool ok = SetClipboardData(CF_HDROP, mem) != nullptr;
		if (!ok) GlobalFree(mem);
		CloseClipboard();
		return ok;
	}
}

ClipHistory::ClipHistory()
{
	dir = Setting::get()->getDataPath() / L"clip";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	load();
	loadPhrases();
	// 只收消息的隐藏窗口：剪切板变化的通知、去抖和延时粘贴的定时器都挂在它身上
	WNDCLASSEX wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = wndProc;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.lpszClassName = L"UUJietuClipListener";
	RegisterClassEx(&wc);
	hwnd = CreateWindowEx(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
	if (hwnd) AddClipboardFormatListener(hwnd);
}

ClipHistory::~ClipHistory()
{
	if (!hwnd) return;
	RemoveClipboardFormatListener(hwnd);
	DestroyWindow(hwnd);
}

void ClipHistory::init()
{
	if (clipHistory) return;
	clipHistory.reset(new ClipHistory());
}

void ClipHistory::dispose()
{
	clipHistory.reset();
}

ClipHistory* ClipHistory::get()
{
	return clipHistory.get();
}

LRESULT CALLBACK ClipHistory::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	auto self = clipHistory.get();
	if (self && self->hwnd == hwnd) {
		if (msg == WM_CLIPBOARDUPDATE) {
			// 一次复制往往连着来好几条通知（程序分几次把各种格式放进去），等它消停了再读
			if (Setting::get()->getClipEnabled()) {
				self->retry = 0;
				SetTimer(hwnd, timerCapture, 150, nullptr);
			}
			return 0;
		}
		if (msg == WM_TIMER && wParam == timerCapture) {
			KillTimer(hwnd, timerCapture);
			self->capture();
			return 0;
		}
		if (msg == WM_TIMER && wParam == timerPaste) {
			KillTimer(hwnd, timerPaste);
			INPUT inputs[4]{};
			for (auto& input : inputs) input.type = INPUT_KEYBOARD;
			inputs[0].ki.wVk = VK_CONTROL;
			inputs[1].ki.wVk = 'V';
			inputs[2].ki.wVk = 'V';
			inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
			inputs[3].ki.wVk = VK_CONTROL;
			inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
			SendInput(4, inputs, sizeof(INPUT));
			return 0;
		}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

void ClipHistory::capture()
{
	if (GetTickCount64() < ignoreUntil) return;
	if (GetClipboardOwner() == hwnd) return; //自己写回去的
	// 密码管理器这类程序会放这个标记，意思是"别把这次复制记进历史"
	static const UINT fmtExclude = RegisterClipboardFormat(L"ExcludeClipboardContentFromMonitorProcessing");
	if (IsClipboardFormatAvailable(fmtExclude)) return;
	auto item = std::make_shared<Item>();
	std::vector<BYTE> pngBytes, pixels;
	int w{ 0 }, h{ 0 };
	if (!OpenClipboard(hwnd)) {
		// 别的程序正占着剪切板，过一会儿再试，试几次还不行就算了
		if (++retry <= 5) SetTimer(hwnd, timerCapture, 100, nullptr);
		return;
	}
	retry = 0;
	const bool ok = readClipboard(*item, pngBytes, w, h, pixels);
	CloseClipboard();
	if (ok) add(item, pngBytes, w, h, pixels);
}

// 调用前剪切板必须已经打开。
// 顺序有讲究：复制文件时剪切板里也有文件名文本，Office 复制文字时也会带一张图，
// 所以文件优先于文本、文本优先于图片
bool ClipHistory::readClipboard(Item& item, std::vector<BYTE>& pngBytes, int& w, int& h, std::vector<BYTE>& pixels)
{
	return readFiles(item) || readText(item) || readImage(item, pngBytes, w, h, pixels);
}

bool ClipHistory::saveImage(long long id, const std::vector<BYTE>& pngBytes, int w, int h, std::vector<BYTE>& pixels)
{
	auto path = getImagePath(id);
	if (!pngBytes.empty()) {
		std::ofstream file{ path, std::ios::binary | std::ios::trunc };
		if (!file) return false;
		file.write(reinterpret_cast<const char*>(pngBytes.data()), pngBytes.size());
		return file.good();
	}
	if (pixels.empty()) return false;
	return Util::saveToFile(path.wstring(), w, h, pixels.data());
}

long long ClipHistory::newId() const
{
	auto id = nowMs();
	auto used = [&](long long val) {
		auto same = [val](auto& item) { return item->id == val; };
		return std::any_of(items.begin(), items.end(), same) || std::any_of(phrases.begin(), phrases.end(), same);
	};
	while (used(id)) id++;
	return id;
}

void ClipHistory::add(std::shared_ptr<Item> item, const std::vector<BYTE>& pngBytes, int w, int h, std::vector<BYTE>& pixels)
{
	// 以前记过同样的东西：挪到最前面就行，收藏状态和图片文件都沿用旧的
	for (size_t i = 0; i < items.size(); i++) {
		auto& old = items[i];
		if (old->type != item->type || old->hash != item->hash) continue;
		if (old->type != Type::Image && old->text != item->text) continue;
		auto keep = old;
		items.erase(items.begin() + i);
		items.insert(items.begin(), keep);
		save();
		notify();
		return;
	}
	item->id = newId();
	if (item->type == Type::Image && !saveImage(item->id, pngBytes, w, h, pixels)) return;
	items.insert(items.begin(), item);
	trim();
	save();
	notify();
}

void ClipHistory::trim()
{
	size_t count{ 0 };
	for (auto& item : items) {
		if (!item->pinned) count++;
	}
	// 从最老的开始删，收藏的跳过
	for (size_t i = items.size(); i > 0 && count > maxItems; i--) {
		auto& item = items[i - 1];
		if (item->pinned) continue;
		if (item->type == Type::Image) {
			std::error_code ec;
			std::filesystem::remove(getImagePath(item->id), ec);
		}
		items.erase(items.begin() + (i - 1));
		count--;
	}
}

void ClipHistory::remove(long long id)
{
	for (size_t i = 0; i < items.size(); i++) {
		if (items[i]->id != id) continue;
		if (items[i]->type == Type::Image) {
			std::error_code ec;
			std::filesystem::remove(getImagePath(id), ec);
		}
		items.erase(items.begin() + i);
		save();
		notify();
		return;
	}
}

void ClipHistory::clear()
{
	std::vector<std::shared_ptr<Item>> keep;
	for (auto& item : items) {
		if (item->pinned) {
			keep.push_back(item);
		}
		else if (item->type == Type::Image) {
			std::error_code ec;
			std::filesystem::remove(getImagePath(item->id), ec);
		}
	}
	items = std::move(keep);
	save();
	notify();
}

void ClipHistory::togglePin(long long id)
{
	for (auto& item : items) {
		if (item->id != id) continue;
		item->pinned = !item->pinned;
		save();
		notify();
		return;
	}
}

bool ClipHistory::writeToClipboard(long long id)
{
	std::shared_ptr<Item> item;
	size_t index{ 0 };
	for (size_t i = 0; i < items.size(); i++) {
		if (items[i]->id == id) {
			item = items[i];
			index = i;
			break;
		}
	}
	bool isPhrase{ false };
	if (!item) {
		for (auto& phrase : phrases) {
			if (phrase->id == id) {
				item = phrase;
				isPhrase = true;
				break;
			}
		}
	}
	if (!item) return false;
	ignoreUntil = GetTickCount64() + 800;
	bool ok{ false };
	if (item->type == Type::Text) {
		ok = setClipText(hwnd, item->text);
	}
	else if (item->type == Type::Files) {
		ok = setClipFiles(hwnd, item->text);
	}
	else {
		int w{ 0 }, h{ 0 };
		std::vector<BYTE> pixels;
		if (loadImage(getImagePath(id), 0, false, w, h, pixels)) {
			Util::saveToClipboard(w, h, pixels.data());
			ok = true;
		}
	}
	if (!ok) return false;
	if (!isPhrase && index != 0) {
		items.erase(items.begin() + index);
		items.insert(items.begin(), item);
		save();
		notify();
	}
	return true;
}

void ClipHistory::pasteTo(HWND target)
{
	if (target && IsWindow(target)) SetForegroundWindow(target);
	// 前台切回去要一点时间，马上发按键会落到还没让出焦点的历史面板上
	SetTimer(hwnd, timerPaste, 150, nullptr);
}

std::filesystem::path ClipHistory::getImagePath(long long id) const
{
	return dir / (std::to_wstring(id) + L".png");
}

bool ClipHistory::loadImage(const std::filesystem::path& path, UINT maxSize, bool premultiplied,
	int& w, int& h, std::vector<BYTE>& pixels)
{
	ComPtr<IWICImagingFactory> factory;
	auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
	if (FAILED(hr)) return false;
	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
	if (FAILED(hr)) return false;
	ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, frame.GetAddressOf());
	if (FAILED(hr)) return false;
	UINT srcW{ 0 }, srcH{ 0 };
	frame->GetSize(&srcW, &srcH);
	if (srcW == 0 || srcH == 0) return false;
	UINT dstW{ srcW }, dstH{ srcH };
	ComPtr<IWICBitmapSource> source;
	hr = frame.As(&source);
	if (FAILED(hr)) return false;
	if (maxSize != 0 && (srcW > maxSize || srcH > maxSize)) {
		const double scale = (std::min)((double)maxSize / srcW, (double)maxSize / srcH);
		dstW = (std::max)(1u, (UINT)(srcW * scale));
		dstH = (std::max)(1u, (UINT)(srcH * scale));
		ComPtr<IWICBitmapScaler> scaler;
		hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = scaler->Initialize(frame.Get(), dstW, dstH, WICBitmapInterpolationModeFant);
		if (FAILED(hr)) return false;
		hr = scaler.As(&source);
		if (FAILED(hr)) return false;
	}
	ComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(converter.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = converter->Initialize(source.Get(),
		premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA,
		WICBitmapDitherTypeNone, nullptr, 0., WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) return false;
	pixels.resize((size_t)dstW * dstH * 4);
	hr = converter->CopyPixels(nullptr, dstW * 4, (UINT)pixels.size(), pixels.data());
	if (FAILED(hr)) return false;
	w = (int)dstW;
	h = (int)dstH;
	return true;
}

void ClipHistory::load()
{
	auto content = Ling::Util::readFileText(dir / L"history.json");
	JsonArray arr{ nullptr };
	if (content.empty() || !JsonArray::TryParse(content, arr)) return;
	for (uint32_t i = 0; i < arr.Size(); i++) {
		if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
		auto obj = arr.GetObjectAt(i);
		auto item = std::make_shared<Item>();
		item->id = static_cast<long long>(obj.GetNamedNumber(L"id", 0));
		const int type = static_cast<int>(obj.GetNamedNumber(L"type", 0));
		if (item->id <= 0 || type < 0 || type > 2) continue;
		item->type = static_cast<Type>(type);
		item->pinned = obj.GetNamedBoolean(L"pin", false);
		item->text = std::wstring{ obj.GetNamedString(L"text", L"") };
		item->imgW = static_cast<int>(obj.GetNamedNumber(L"w", 0));
		item->imgH = static_cast<int>(obj.GetNamedNumber(L"h", 0));
		// 64 位的 hash 放不进 JSON 的数字（double），存成字符串
		item->hash = wcstoull(std::wstring{ obj.GetNamedString(L"hash", L"0") }.c_str(), nullptr, 10);
		if (item->type == Type::Image) {
			std::error_code ec;
			if (!std::filesystem::exists(getImagePath(item->id), ec)) continue; //图片文件被删了
		}
		else if (item->text.empty()) {
			continue;
		}
		items.push_back(std::move(item));
	}
}

void ClipHistory::save()
{
	JsonArray arr;
	for (auto& item : items) {
		JsonObject obj;
		obj.SetNamedValue(L"id", JsonValue::CreateNumberValue(static_cast<double>(item->id)));
		obj.SetNamedValue(L"type", JsonValue::CreateNumberValue(static_cast<int>(item->type)));
		obj.SetNamedValue(L"pin", JsonValue::CreateBooleanValue(item->pinned));
		obj.SetNamedValue(L"text", JsonValue::CreateStringValue(item->text));
		obj.SetNamedValue(L"w", JsonValue::CreateNumberValue(item->imgW));
		obj.SetNamedValue(L"h", JsonValue::CreateNumberValue(item->imgH));
		obj.SetNamedValue(L"hash", JsonValue::CreateStringValue(std::to_wstring(item->hash)));
		arr.Append(obj);
	}
	Ling::Util::saveFile((dir / L"history.json").wstring(), std::wstring{ arr.Stringify() });
}

bool ClipHistory::addGroup(const std::wstring& name)
{
	if (name.empty() || std::find(groups.begin(), groups.end(), name) != groups.end()) return false;
	groups.push_back(name);
	savePhrases();
	notify();
	return true;
}

void ClipHistory::removeGroup(const std::wstring& name)
{
	if (name.empty()) return;
	const auto prefix = name + L"/";
	auto inside = [&](const std::wstring& path) { return path == name || path.starts_with(prefix); };
	if (std::none_of(groups.begin(), groups.end(), inside)) return;
	std::erase_if(groups, inside);
	const auto slash = name.find_last_of(L'/');
	const std::wstring parent = slash == std::wstring::npos ? L"" : name.substr(0, slash);
	for (auto& phrase : phrases) {
		if (inside(phrase->group)) phrase->group = parent;
	}
	savePhrases();
	notify();
}

bool ClipHistory::renameGroup(const std::wstring& name, const std::wstring& newName)
{
	if (name.empty() || newName.empty() || newName.find(L'/') != std::wstring::npos) return false;
	const auto slash = name.find_last_of(L'/');
	const std::wstring target = slash == std::wstring::npos ? newName : name.substr(0, slash + 1) + newName;
	if (target == name) return true;
	if (std::find(groups.begin(), groups.end(), target) != groups.end()) return false;
	if (std::find(groups.begin(), groups.end(), name) == groups.end()) return false;
	const auto prefix = name + L"/";
	// 自己和所有子孙的路径，把开头那一段换成新的
	auto fix = [&](std::wstring& path) {
		if (path == name) path = target;
		else if (path.starts_with(prefix)) path = target + L"/" + path.substr(prefix.size());
	};
	for (auto& path : groups) fix(path);
	for (auto& phrase : phrases) fix(phrase->group);
	savePhrases();
	notify();
	return true;
}

void ClipHistory::swapGroups(const std::wstring& a, const std::wstring& b)
{
	auto itA = std::find(groups.begin(), groups.end(), a);
	auto itB = std::find(groups.begin(), groups.end(), b);
	if (itA == groups.end() || itB == groups.end() || itA == itB) return;
	// 子分组各有各的条目，顺序是按父分组筛出来的，所以只换这两条就够了
	std::iter_swap(itA, itB);
	savePhrases();
	notify();
}

std::vector<std::wstring> ClipHistory::getChildGroups(const std::wstring& prefix) const
{
	std::vector<std::wstring> result;
	const auto head = prefix.empty() ? L"" : prefix + L"/";
	for (auto& path : groups) {
		if (!path.starts_with(head)) continue;
		auto name = path.substr(head.size());
		// 只要紧挨着的那一级：剩下的部分里不能再有 /
		if (name.empty() || name.find(L'/') != std::wstring::npos) continue;
		result.push_back(std::move(name));
	}
	return result;
}

std::vector<std::wstring> ClipHistory::getGroupTree() const
{
	std::vector<std::wstring> result;
	auto walk = [&](auto& self, const std::wstring& prefix) -> void {
		for (auto& name : getChildGroups(prefix)) {
			auto path = prefix.empty() ? name : prefix + L"/" + name;
			result.push_back(path);
			self(self, path);
		}
	};
	walk(walk, L"");
	return result;
}

bool ClipHistory::addPhraseFromItem(long long historyId, const std::wstring& group)
{
	for (auto& src : items) {
		if (src->id != historyId) continue;
		auto phrase = std::make_shared<Item>(*src);
		phrase->id = newId();
		phrase->pinned = false;
		phrase->group = group;
		if (phrase->type == Type::Image) {
			// 图片另抄一份：历史那边的会被挤掉、被清空，话术这份得一直在
			std::error_code ec;
			std::filesystem::copy_file(getImagePath(src->id), getImagePath(phrase->id), ec);
			if (ec) return false;
		}
		phrases.insert(phrases.begin(), phrase);
		savePhrases();
		notify();
		return true;
	}
	return false;
}

bool ClipHistory::addPhraseFromClipboard(const std::wstring& group)
{
	if (!OpenClipboard(hwnd)) return false;
	auto phrase = std::make_shared<Item>();
	std::vector<BYTE> pngBytes, pixels;
	int w{ 0 }, h{ 0 };
	const bool ok = readClipboard(*phrase, pngBytes, w, h, pixels);
	CloseClipboard();
	if (!ok) return false;
	phrase->id = newId();
	phrase->group = group;
	if (phrase->type == Type::Image && !saveImage(phrase->id, pngBytes, w, h, pixels)) return false;
	phrases.insert(phrases.begin(), phrase);
	savePhrases();
	notify();
	return true;
}

void ClipHistory::removePhrase(long long id)
{
	for (size_t i = 0; i < phrases.size(); i++) {
		if (phrases[i]->id != id) continue;
		if (phrases[i]->type == Type::Image) {
			std::error_code ec;
			std::filesystem::remove(getImagePath(id), ec);
		}
		phrases.erase(phrases.begin() + i);
		savePhrases();
		notify();
		return;
	}
}

void ClipHistory::setPhraseTitle(long long id, const std::wstring& title)
{
	for (auto& phrase : phrases) {
		if (phrase->id != id) continue;
		phrase->title = title;
		savePhrases();
		notify();
		return;
	}
}

void ClipHistory::setPhraseGroup(long long id, const std::wstring& group)
{
	for (auto& phrase : phrases) {
		if (phrase->id != id) continue;
		phrase->group = group;
		savePhrases();
		notify();
		return;
	}
}

// phrases.json：{ "groups": [分组名...], "items": [话术...] }，话术的字段比历史多 title / group 两项
void ClipHistory::loadPhrases()
{
	auto content = Ling::Util::readFileText(dir / L"phrases.json");
	JsonObject root{ nullptr };
	if (content.empty() || !JsonObject::TryParse(content, root)) return;
	auto groupArr = root.GetNamedArray(L"groups", nullptr);
	for (uint32_t i = 0; groupArr && i < groupArr.Size(); i++) {
		if (groupArr.GetAt(i).ValueType() != JsonValueType::String) continue;
		std::wstring name{ groupArr.GetStringAt(i) };
		if (!name.empty() && std::find(groups.begin(), groups.end(), name) == groups.end()) groups.push_back(name);
	}
	auto arr = root.GetNamedArray(L"items", nullptr);
	for (uint32_t i = 0; arr && i < arr.Size(); i++) {
		if (arr.GetAt(i).ValueType() != JsonValueType::Object) continue;
		auto obj = arr.GetObjectAt(i);
		auto item = std::make_shared<Item>();
		item->id = static_cast<long long>(obj.GetNamedNumber(L"id", 0));
		const int type = static_cast<int>(obj.GetNamedNumber(L"type", 0));
		if (item->id <= 0 || type < 0 || type > 2) continue;
		item->type = static_cast<Type>(type);
		item->text = std::wstring{ obj.GetNamedString(L"text", L"") };
		item->imgW = static_cast<int>(obj.GetNamedNumber(L"w", 0));
		item->imgH = static_cast<int>(obj.GetNamedNumber(L"h", 0));
		item->title = std::wstring{ obj.GetNamedString(L"title", L"") };
		item->group = std::wstring{ obj.GetNamedString(L"group", L"") };
		// 分组被手工从文件里删掉了：话术留着，归到未分组
		if (std::find(groups.begin(), groups.end(), item->group) == groups.end()) item->group.clear();
		if (item->type == Type::Image) {
			std::error_code ec;
			if (!std::filesystem::exists(getImagePath(item->id), ec)) continue;
		}
		else if (item->text.empty()) {
			continue;
		}
		phrases.push_back(std::move(item));
	}
}

void ClipHistory::savePhrases()
{
	JsonArray groupArr;
	for (auto& name : groups) groupArr.Append(JsonValue::CreateStringValue(name));
	JsonArray arr;
	for (auto& item : phrases) {
		JsonObject obj;
		obj.SetNamedValue(L"id", JsonValue::CreateNumberValue(static_cast<double>(item->id)));
		obj.SetNamedValue(L"type", JsonValue::CreateNumberValue(static_cast<int>(item->type)));
		obj.SetNamedValue(L"text", JsonValue::CreateStringValue(item->text));
		obj.SetNamedValue(L"w", JsonValue::CreateNumberValue(item->imgW));
		obj.SetNamedValue(L"h", JsonValue::CreateNumberValue(item->imgH));
		obj.SetNamedValue(L"title", JsonValue::CreateStringValue(item->title));
		obj.SetNamedValue(L"group", JsonValue::CreateStringValue(item->group));
		arr.Append(obj);
	}
	JsonObject root;
	root.SetNamedValue(L"groups", groupArr);
	root.SetNamedValue(L"items", arr);
	Ling::Util::saveFile((dir / L"phrases.json").wstring(), std::wstring{ root.Stringify() });
}

void ClipHistory::notify()
{
	if (onChanged) onChanged();
}
