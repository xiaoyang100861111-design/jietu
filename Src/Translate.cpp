#include "pch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include "Translate.h"
#include "Setting.h"
#include "Lang.h"

using Microsoft::WRL::ComPtr;

namespace {
	using namespace winrt::Windows::Foundation;
	using namespace winrt::Windows::Data::Json;
	using namespace winrt::Windows::Graphics::Imaging;
	using namespace winrt::Windows::Media::Ocr;
	using namespace winrt::Windows::Web::Http;
	using winrt::Windows::Globalization::Language;
	using winrt::Windows::Security::Cryptography::CryptographicBuffer;
	using winrt::Windows::Storage::Streams::UnicodeEncoding;

	constexpr std::wstring_view msAuthUrl{ L"https://edge.microsoft.com/translate/auth" };
	constexpr std::wstring_view msTransUrl{ L"https://api-edge.cognitive.microsofttranslator.com/translate?api-version=3.0&textType=plain&to=" };
	constexpr std::wstring_view googleUrl{ L"https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&dt=t&tl=" };

	// OCR 认出来的一行
	struct Line {
		D2D1_RECT_F rect{};
		std::wstring text;
	};

	// 给用户看的失败原因。带着文案的 key 一路抛到 start 里的 catch，在那儿统一换成文案
	struct WorkError {
		std::wstring key;
	};

	// 微软接口的令牌，有效期十分钟，留点余量按八分钟用
	std::mutex tokenMutex;
	std::wstring msToken;
	std::chrono::steady_clock::time_point msTokenTime{};

	bool isHan(wchar_t c) { return c >= 0x4E00 && c <= 0x9FFF; }
	bool isKana(wchar_t c) { return c >= 0x3040 && c <= 0x30FF; }
	bool isHangul(wchar_t c) { return c >= 0xAC00 && c <= 0xD7AF; }
	bool isLatin(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
	// 中日文、全角标点这类字符之间不加空格（韩文是分词书写的，不在此列）
	bool isTight(wchar_t c) { return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xFF00 && c <= 0xFFEF); }

	// 两段文字接起来。中日文之间直接连，其余的中间补一个空格
	void appendText(std::wstring& text, const std::wstring& part)
	{
		if (part.empty()) return;
		if (!text.empty() && !isTight(text.back()) && !isTight(part.front())) text += L' ';
		text += part;
	}

	// 等一个 WinRT 异步操作，超时就取消并抛。只能在后台线程用（STA 上 wait_for 会断言）
	template<typename T>
	auto waitOp(const T& op, int seconds = 8)
	{
		if (op.wait_for(std::chrono::seconds(seconds)) == AsyncStatus::Started) {
			op.Cancel();
			throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		}
		return op.GetResults(); //出错 / 被取消时这里会抛
	}

	// 放大两倍（双线性）。屏幕上的字通常只有十几个像素高，系统 OCR 对这么小的字认得很差，
	// 放大之后准确率明显上去
	std::vector<BYTE> scale2x(const std::vector<BYTE>& src, int w, int h)
	{
		const int dw = w * 2, dh = h * 2;
		std::vector<BYTE> dst((size_t)dw * dh * 4);
		// 目标像素 d 落在源像素 a、b 之间，a 的权重是 wa/4
		auto pick = [](int d, int n, int& a, int& b, int& wa) {
			int s = d >> 1;
			if (d & 1) { a = s; b = (std::min)(s + 1, n - 1); wa = 3; }
			else { a = (std::max)(s - 1, 0); b = s; wa = 1; }
		};
		for (int y = 0; y < dh; y++) {
			int y0, y1, wy;
			pick(y, h, y0, y1, wy);
			auto row0 = src.data() + (size_t)y0 * w * 4;
			auto row1 = src.data() + (size_t)y1 * w * 4;
			auto out = dst.data() + (size_t)y * dw * 4;
			for (int x = 0; x < dw; x++) {
				int x0, x1, wx;
				pick(x, w, x0, x1, wx);
				for (int c = 0; c < 4; c++) {
					int top = wx * row0[x0 * 4 + c] + (4 - wx) * row0[x1 * 4 + c];
					int bottom = wx * row1[x0 * 4 + c] + (4 - wx) * row1[x1 * 4 + c];
					out[x * 4 + c] = (BYTE)((wy * top + (4 - wy) * bottom + 8) >> 4);
				}
			}
		}
		return dst;
	}

	// inv：识别用的图相对原图放大了多少倍的倒数，坐标要换回原图
	std::vector<Line> toLines(const OcrResult& result, float inv)
	{
		std::vector<Line> lines;
		for (auto&& line : result.Lines()) {
			Line item;
			bool first{ true };
			// 不用 line.Text()：中文引擎会在每个汉字之间都塞一个空格，自己按词拼
			for (auto&& word : line.Words()) {
				std::wstring text{ word.Text() };
				if (text.empty()) continue;
				auto r = word.BoundingRect();
				D2D1_RECT_F rect{ r.X * inv, r.Y * inv, (r.X + r.Width) * inv, (r.Y + r.Height) * inv };
				if (first) {
					item.rect = rect;
					first = false;
				}
				else {
					item.rect.left = (std::min)(item.rect.left, rect.left);
					item.rect.top = (std::min)(item.rect.top, rect.top);
					item.rect.right = (std::max)(item.rect.right, rect.right);
					item.rect.bottom = (std::max)(item.rect.bottom, rect.bottom);
				}
				appendText(item.text, text);
			}
			if (!first) lines.push_back(std::move(item));
		}
		return lines;
	}

	// 统计各种文字的个数，用来猜原文是什么语言
	struct CharStat {
		int han{ 0 }, kana{ 0 }, hangul{ 0 }, latin{ 0 };
		int total() const { return han + kana + hangul + latin; }
	};
	CharStat countChars(const std::vector<Line>& lines)
	{
		CharStat stat;
		for (auto& line : lines) {
			for (auto c : line.text) {
				if (isHan(c)) stat.han++;
				else if (isKana(c)) stat.kana++;
				else if (isHangul(c)) stat.hangul++;
				else if (isLatin(c)) stat.latin++;
			}
		}
		return stat;
	}

	std::vector<Line> recognize(int w, int h, const std::vector<BYTE>& pixels)
	{
		auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
		auto langs = OcrEngine::AvailableRecognizerLanguages();
		if (!engine && langs.Size() > 0) engine = OcrEngine::TryCreateFromLanguage(langs.GetAt(0));
		if (!engine) throw WorkError{ L"translate.noEngine" };
		const int maxDim = (int)OcrEngine::MaxImageDimension();
		if (w > maxDim || h > maxDim) throw WorkError{ L"translate.tooLarge" };
		// 选区不大时放大两倍再认。太大的图本来字就不会小到哪去，放大只是白白变慢
		const bool enlarge = w * 2 <= maxDim && h * 2 <= maxDim && (long long)w * h <= 1600000;
		std::vector<BYTE> scaled;
		if (enlarge) scaled = scale2x(pixels, w, h);
		auto& data = enlarge ? scaled : pixels;
		const int bw = enlarge ? w * 2 : w, bh = enlarge ? h * 2 : h;
		const float inv = enlarge ? 0.5f : 1.f;
		auto buffer = CryptographicBuffer::CreateFromByteArray(
			winrt::array_view<const uint8_t>(data.data(), data.data() + data.size()));
		auto bitmap = SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8, bw, bh, BitmapAlphaMode::Premultiplied);
		auto lines = toLines(waitOp(engine.RecognizeAsync(bitmap), 15), inv);
		// 中文引擎认英文是能认的，但不如英文引擎准。认出来基本全是拉丁字母、
		// 系统里又正好装了英文识别包，就拿英文引擎重认一遍
		auto stat = countChars(lines);
		std::wstring curTag{ engine.RecognizerLanguage().LanguageTag() };
		if (stat.latin > 0 && stat.latin > 3 * (stat.han + stat.kana + stat.hangul) && !curTag.starts_with(L"en")) {
			for (auto&& lang : langs) {
				std::wstring tag{ lang.LanguageTag() };
				if (!tag.starts_with(L"en")) continue;
				auto enEngine = OcrEngine::TryCreateFromLanguage(lang);
				if (!enEngine) break;
				auto enLines = toLines(waitOp(enEngine.RecognizeAsync(bitmap), 15), inv);
				// 英文引擎认出来的字少了一截，说明图里其实有它不认识的文字，还是用原来的
				if (countChars(enLines).latin * 10 >= stat.latin * 9) lines = std::move(enLines);
				break;
			}
		}
		return lines;
	}

	// 把行并成段。只把"看着像正文折行"的并到一起：左对齐、行高相近、行距正常、
	// 上一行够长。菜单、表格这类一行一项的东西各自成段，不然译文会被揉成一坨
	std::vector<Translate::Block> toBlocks(const std::vector<Line>& lines)
	{
		std::vector<Translate::Block> blocks;
		std::vector<int> lineCounts;
		D2D1_RECT_F prev{};
		for (auto& line : lines) {
			const float lineH = line.rect.bottom - line.rect.top;
			if (lineH < 1.f) continue;
			bool merged{ false };
			if (!blocks.empty()) {
				auto& block = blocks.back();
				const float prevH = prev.bottom - prev.top;
				const float prevW = prev.right - prev.left;
				const float blockW = block.rect.right - block.rect.left;
				const float gap = line.rect.top - prev.bottom;
				const bool sameSize = lineH < prevH * 1.35f && prevH < lineH * 1.35f;
				const bool aligned = std::abs(line.rect.left - block.rect.left) < lineH * 1.2f;
				const bool close = gap > -lineH * 0.3f && gap < lineH * 0.9f;
				const bool prevFull = prevW >= lineH * 8.f && prevW >= blockW * 0.75f
					&& prevW >= (line.rect.right - line.rect.left) * 0.75f;
				if (sameSize && aligned && close && prevFull) {
					block.rect.left = (std::min)(block.rect.left, line.rect.left);
					block.rect.right = (std::max)(block.rect.right, line.rect.right);
					block.rect.bottom = line.rect.bottom;
					auto& count = lineCounts.back();
					block.lineH = (block.lineH * count + lineH) / (count + 1);
					count++;
					appendText(block.src, line.text);
					merged = true;
				}
			}
			if (!merged) {
				Translate::Block block;
				block.rect = line.rect;
				block.lineH = lineH;
				block.src = line.text;
				blocks.push_back(std::move(block));
				lineCounts.push_back(1);
			}
			prev = line.rect;
		}
		return blocks;
	}

	// 界面语言 → 翻译接口的目标语言代码（微软的写法）
	std::wstring langToTarget(const std::wstring& lang)
	{
		if (lang == L"zh-CN") return L"zh-Hans";
		if (lang == L"zh-TW" || lang == L"zh-HK") return L"zh-Hant";
		auto pos = lang.find(L'-');
		return pos == std::wstring::npos ? lang : lang.substr(0, pos);
	}

	// 原文本来就是目标语言时反过来翻：中文界面下截到中文就译成英文，其余的译成中文
	std::wstring pickTarget(std::wstring target, const CharStat& stat)
	{
		const int total = stat.total();
		if (total == 0) return target;
		if (target.starts_with(L"zh")) {
			if (stat.han * 2 > total && stat.kana * 10 < total) return L"en";
		}
		else if (target == L"en") {
			if (stat.latin * 10 > total * 9) return L"zh-Hans";
		}
		return target;
	}

	std::wstring getMsToken(const HttpClient& client)
	{
		std::lock_guard lock{ tokenMutex };
		auto now = std::chrono::steady_clock::now();
		if (!msToken.empty() && now - msTokenTime < std::chrono::minutes(8)) return msToken;
		msToken = std::wstring{ waitOp(client.GetStringAsync(Uri{ msAuthUrl })) };
		msTokenTime = now;
		return msToken;
	}

	// 一次请求翻一批：blocks[from, to)
	void msTranslateBatch(const HttpClient& client, const std::wstring& token, const std::wstring& target,
		std::vector<Translate::Block>& blocks, size_t from, size_t to)
	{
		JsonArray body;
		for (size_t i = from; i < to; i++) {
			JsonObject item;
			item.SetNamedValue(L"Text", JsonValue::CreateStringValue(blocks[i].src));
			body.Append(item);
		}
		HttpRequestMessage req{ HttpMethod::Post(), Uri{ std::wstring{ msTransUrl } + target } };
		req.Content(HttpStringContent{ body.Stringify(), UnicodeEncoding::Utf8, L"application/json" });
		req.Headers().TryAppendWithoutValidation(L"Authorization", L"Bearer " + token);
		auto resp = waitOp(client.SendRequestAsync(req));
		resp.EnsureSuccessStatusCode();
		std::wstring text{ waitOp(resp.Content().ReadAsStringAsync()) };
		auto arr = JsonArray::Parse(text);
		if (arr.Size() != to - from) throw winrt::hresult_error(E_FAIL);
		for (uint32_t i = 0; i < arr.Size(); i++) {
			auto trans = arr.GetObjectAt(i).GetNamedArray(L"translations");
			blocks[from + i].dst = std::wstring{ trans.GetObjectAt(0).GetNamedString(L"text") };
		}
	}

	void msTranslate(const HttpClient& client, const std::wstring& target, std::vector<Translate::Block>& blocks)
	{
		auto token = getMsToken(client);
		// 接口对单次请求的条数和总字数都有上限，分批发
		size_t from{ 0 };
		while (from < blocks.size()) {
			size_t to{ from }, chars{ 0 };
			while (to < blocks.size() && to - from < 100 && (to == from || chars + blocks[to].src.size() <= 8000)) {
				chars += blocks[to].src.size();
				to++;
			}
			msTranslateBatch(client, token, target, blocks, from, to);
			from = to;
		}
	}

	// Google 的免密钥接口一次只收一段文本，所以把各段用换行连起来一起发，回来再按换行拆开
	void googleTranslate(const HttpClient& client, const std::wstring& target, std::vector<Translate::Block>& blocks)
	{
		std::wstring tl = target == L"zh-Hans" ? L"zh-CN" : target == L"zh-Hant" ? L"zh-TW" : target;
		std::wstring joined;
		for (auto& block : blocks) {
			if (!joined.empty()) joined += L'\n';
			joined += block.src;
		}
		HttpRequestMessage req{ HttpMethod::Post(), Uri{ std::wstring{ googleUrl } + tl } };
		req.Content(HttpStringContent{ L"q=" + std::wstring{ Uri::EscapeComponent(joined) },
			UnicodeEncoding::Utf8, L"application/x-www-form-urlencoded" });
		auto resp = waitOp(client.SendRequestAsync(req));
		resp.EnsureSuccessStatusCode();
		std::wstring text{ waitOp(resp.Content().ReadAsStringAsync()) };
		auto segs = JsonArray::Parse(text).GetArrayAt(0);
		std::wstring all;
		for (uint32_t i = 0; i < segs.Size(); i++) {
			auto seg = segs.GetAt(i);
			if (seg.ValueType() != JsonValueType::Array) continue;
			auto first = seg.GetArray().GetAt(0);
			if (first.ValueType() == JsonValueType::String) all += std::wstring_view{ first.GetString() };
		}
		std::vector<std::wstring> parts;
		size_t start{ 0 };
		while (true) {
			auto pos = all.find(L'\n', start);
			parts.push_back(all.substr(start, pos == std::wstring::npos ? pos : pos - start));
			if (pos == std::wstring::npos) break;
			start = pos + 1;
		}
		if (parts.size() != blocks.size()) throw winrt::hresult_error(E_FAIL);
		for (size_t i = 0; i < blocks.size(); i++) blocks[i].dst = parts[i];
	}

	void translate(const std::wstring& target, std::vector<Translate::Block>& blocks)
	{
		HttpClient client;
		client.DefaultRequestHeaders().UserAgent().TryParseAdd(
			L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0.0");
		try {
			msTranslate(client, target, blocks);
			return;
		}
		catch (...) {
			// 令牌可能是被服务端提前作废的，清掉，下次重新要
			std::lock_guard lock{ tokenMutex };
			msToken.clear();
		}
		try {
			googleTranslate(client, target, blocks);
		}
		catch (...) {
			throw WorkError{ L"translate.netError" };
		}
	}

	void work(int w, int h, const std::vector<BYTE>& pixels, bool needTranslate, const std::wstring& target, Translate::Result& result)
	{
		auto lines = recognize(w, h, pixels);
		result.blocks = toBlocks(lines);
		if (result.blocks.empty()) throw WorkError{ L"translate.noText" };
		for (auto& block : result.blocks) {
			if (!result.text.empty()) result.text += L"\r\n";
			result.text += block.src;
		}
		if (needTranslate) {
			translate(pickTarget(target, countChars(lines)), result.blocks);
		}
		result.ok = true;
	}

	winrt::fire_and_forget run(int w, int h, std::shared_ptr<std::vector<BYTE>> pixels, bool needTranslate,
		std::wstring target, Translate::Callback cb)
	{
		// 识别和网络请求都可能卡上一两秒，挂在 UI 线程上整个应用就不动了
		co_await winrt::resume_background();
		auto result = std::make_shared<Translate::Result>();
		std::wstring errKey;
		try {
			work(w, h, *pixels, needTranslate, target, *result);
		}
		catch (const WorkError& e) {
			errKey = e.key;
		}
		catch (...) {
			errKey = L"translate.failed";
		}
		// Lang 和回调里要碰的窗口都只在 UI 线程上用
		Ling::App::get()->dq.TryEnqueue([result, errKey, cb]() {
			if (!errKey.empty()) {
				result->ok = false;
				result->err = Lang::get(errKey);
			}
			cb(result);
		});
	}

	D2D1_COLOR_F toColor(double b, double g, double r)
	{
		return D2D1::ColorF((float)(r / 255.), (float)(g / 255.), (float)(b / 255.), 1.f);
	}

	// 从原图上猜这一段的底色和字色。底色取包围盒外面一圈里出现最多的颜色；
	// 字色取盒子里离底色最远的那批像素的平均
	void pickColors(int w, int h, const std::vector<BYTE>& pixels, const D2D1_RECT_F& rect,
		D2D1_COLOR_F& bg, D2D1_COLOR_F& fg)
	{
		const int left = std::clamp((int)rect.left - 1, 0, w - 1);
		const int top = std::clamp((int)rect.top - 1, 0, h - 1);
		const int right = std::clamp((int)rect.right + 1, left, w - 1);
		const int bottom = std::clamp((int)rect.bottom + 1, top, h - 1);
		struct Bucket { int count{ 0 }; double b{ 0 }, g{ 0 }, r{ 0 }; };
		std::unordered_map<int, Bucket> buckets;
		auto add = [&](int x, int y) {
			auto px = pixels.data() + ((size_t)y * w + x) * 4;
			auto& bucket = buckets[(px[0] >> 4) | ((px[1] >> 4) << 4) | ((px[2] >> 4) << 8)];
			bucket.count++;
			bucket.b += px[0]; bucket.g += px[1]; bucket.r += px[2];
		};
		for (int x = left; x <= right; x++) { add(x, top); add(x, bottom); }
		for (int y = top; y <= bottom; y++) { add(left, y); add(right, y); }
		const Bucket* best{ nullptr };
		for (auto& [key, bucket] : buckets) {
			if (!best || bucket.count > best->count) best = &bucket;
		}
		const double bb = best->b / best->count, bgc = best->g / best->count, br = best->r / best->count;
		bg = toColor(bb, bgc, br);
		// 段落大的时候隔着取样，没必要每个像素都看
		const int step = (std::max)(1, (int)std::sqrt((double)(right - left + 1) * (bottom - top + 1) / 20000.));
		auto dist = [&](const BYTE* px) {
			const double db = px[0] - bb, dg = px[1] - bgc, dr = px[2] - br;
			return db * db + dg * dg + dr * dr;
		};
		double maxDist{ 0 };
		for (int y = top; y <= bottom; y += step) {
			for (int x = left; x <= right; x += step) {
				maxDist = (std::max)(maxDist, dist(pixels.data() + ((size_t)y * w + x) * 4));
			}
		}
		const bool darkBg = (br * 299 + bgc * 587 + bb * 114) / 1000 < 128;
		fg = darkBg ? D2D1::ColorF(D2D1::ColorF::White) : D2D1::ColorF(D2D1::ColorF::Black);
		// 盒子里跟底色都差不多：没认出字色，就用黑 / 白
		if (maxDist < 60. * 60.) return;
		double sb{ 0 }, sg{ 0 }, sr{ 0 };
		int count{ 0 };
		for (int y = top; y <= bottom; y += step) {
			for (int x = left; x <= right; x += step) {
				auto px = pixels.data() + ((size_t)y * w + x) * 4;
				if (dist(px) < maxDist * 0.5) continue;
				sb += px[0]; sg += px[1]; sr += px[2];
				count++;
			}
		}
		if (count > 0) fg = toColor(sb / count, sg / count, sr / count);
	}
}

void Translate::start(int w, int h, const std::vector<BYTE>& pixels, bool needTranslate, Callback cb)
{
	// 配置对象只在 UI 线程上碰，目标语言在这里先取好
	auto target = Setting::get()->getTranslateTarget();
	if (target.empty() || target == L"auto") target = langToTarget(Setting::get()->getLang());
	run(w, h, std::make_shared<std::vector<BYTE>>(pixels), needTranslate, target, std::move(cb));
}

// 离屏画：底图铺上去，每一段先拿底色把原文盖掉，再把译文写在原来的位置上。
// 与 WinPin::getImagePixels 同一套做法（SetTarget → BeginDraw → EndDraw → SetTarget(nullptr) 闭环）
bool Translate::render(int w, int h, std::vector<BYTE>& pixels, const std::vector<Block>& blocks)
{
	if (w <= 0 || h <= 0 || pixels.size() < (size_t)w * h * 4) return false;
	auto d2d = Ling::D2D::get();
	auto ctx = d2d->deviceContext.Get();
	const auto size = D2D1::SizeU((UINT32)w, (UINT32)h);
	const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
	D2D1_BITMAP_PROPERTIES1 srcProps{
		.pixelFormat{ format }, .dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_NONE }
	};
	ComPtr<ID2D1Bitmap1> srcBmp;
	auto hr = ctx->CreateBitmap(size, pixels.data(), (UINT32)w * 4, &srcProps, srcBmp.GetAddressOf());
	if (FAILED(hr)) return false;
	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ format }, .dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> targetBmp;
	hr = ctx->CreateBitmap(size, nullptr, 0, &targetProps, targetBmp.GetAddressOf());
	if (FAILED(hr)) return false;
	ComPtr<ID2D1SolidColorBrush> brush;
	hr = ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), brush.GetAddressOf());
	if (FAILED(hr)) return false;

	ctx->SetTarget(targetBmp.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0, 0.0f));
	ctx->DrawBitmap(srcBmp.Get(), D2D1::RectF(0.f, 0.f, (float)w, (float)h));
	for (auto& block : blocks) {
		if (block.dst.empty()) continue;
		D2D1_COLOR_F bg{}, fg{};
		pickColors(w, h, pixels, block.rect, bg, fg);
		const float pad = (std::max)(1.f, block.lineH * 0.12f);
		const float boxH = block.rect.bottom - block.rect.top;
		// 译文往往比原文长一点，右边多让出一个字的宽度（不出图）
		const float boxW = (std::min)(block.rect.right - block.rect.left + block.lineH, (float)w - block.rect.left);
		// 字号从原文的行高起步，放不下就一点点缩，缩到下限还放不下就让它往下溢出
		float fontSize = std::clamp(block.lineH * 0.9f, 9.f, 96.f);
		const float minSize = (std::max)(9.f, block.lineH * 0.55f);
		ComPtr<IDWriteTextLayout> layout;
		DWRITE_TEXT_METRICS tm{};
		while (true) {
			layout = Ling::D2D::makeTextLayout(block.dst, fontSize, boxW);
			if (!layout) break;
			layout->GetMetrics(&tm);
			if (tm.height <= boxH + block.lineH * 0.35f && tm.width <= boxW + 1.f) break;
			if (fontSize <= minSize) break;
			fontSize = (std::max)(minSize, fontSize * 0.93f);
		}
		if (!layout) continue;
		D2D1_RECT_F fill{
			(std::max)(0.f, block.rect.left - pad),
			(std::max)(0.f, block.rect.top - pad),
			(std::min)((float)w, (std::max)(block.rect.right, block.rect.left + tm.width) + pad),
			(std::min)((float)h, (std::max)(block.rect.bottom, block.rect.top + tm.height) + pad)
		};
		brush->SetColor(bg);
		ctx->FillRectangle(fill, brush.Get());
		brush->SetColor(fg);
		// 比原来的盒子矮就竖直居中，高了就从顶上开始排
		const float offsetY = tm.height < boxH ? (boxH - tm.height) / 2.f : 0.f;
		ctx->DrawTextLayout(D2D1::Point2F(block.rect.left, block.rect.top + offsetY), layout.Get(), brush.Get());
	}
	hr = ctx->EndDraw();
	// 解绑，下面 CopyFromBitmap 才能把它当 source 读
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return false;

	// GPU 上的 target 位图不能直接 Map，得先拷到一块带 CPU_READ 的位图上
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ format }, .dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBmp;
	hr = ctx->CreateBitmap(size, nullptr, 0, &cpuProps, cpuBmp.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = cpuBmp->CopyFromBitmap(nullptr, targetBmp.Get(), nullptr);
	if (FAILED(hr)) return false;
	D2D1_MAPPED_RECT mapped{};
	hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) return false;
	const UINT32 rowBytes = (UINT32)w * 4;
	for (int row = 0; row < h; row++) {
		CopyMemory(pixels.data() + (size_t)row * rowBytes, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
	}
	cpuBmp->Unmap();
	return true;
}
