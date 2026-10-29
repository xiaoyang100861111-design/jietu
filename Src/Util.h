#pragma once
#include <include/Ling.h>
#include <fstream>
#include <array>

// 图像输出相关的工具函数。data 一律要求 BGRA、top-down、行紧凑（步长 = w*4），
// 这也是 WinPin::getImagePixels 交出来的格式。
class Util
{
public:
	// 同时写入 CF_DIBV5（Office / 微信 / WPS 这类原生程序认）和 "PNG" 注册格式
	//（浏览器 / Electron 程序认），两份都带 alpha
	// 返回 false = 剪切板一直被别的程序占着，没写进去（调用方要提示用户，别当成功关窗）
	static bool saveToClipboard(const int w, const int h, BYTE* data);
	// 写文字。别的程序（剪切板管理器、远程桌面、输入法……）正开着剪切板时会重试一小会儿，
	// 还不行就返回 false。不再用 Ling::Util::setTextToClipboard：它只试一次、失败不吭声
	static bool setTextToClipboard(const std::wstring& text);
	// 打开剪切板，打不开就隔 25 毫秒再试，最多约半秒。打开时用自己的隐藏窗口当主人：
	// 用 NULL 打开再 EmptyClipboard，文档上写着之后的 SetClipboardData 会失败
	static bool openClipboard();
	// 编码成 PNG 放进内存（上传给在线识别接口用）
	static bool encodePngBytes(const int w, const int h, BYTE* data, std::vector<BYTE>& out);
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data);
	// 弹系统另存为对话框，返回空串表示用户取消
	static std::wstring getSaveFilePath(HWND hwnd, const std::wstring& ext = L"png");
	// 以当前时间生成默认文件名，精确到毫秒，避免连续保存时重名
	static std::wstring createFileName(const std::wstring& ext);
	// GDI 抓屏。返回 BGRA、top-down、行紧凑（步长 = w*4），与本类其他函数的入参格式一致
	static std::vector<BYTE> captureScreen(const int x, const int y, const int w, const int h);
	// 把文件路径以 CF_HDROP 写进剪切板，粘贴到资源管理器/聊天窗口就是一个文件
	static void addFileToClipboard(const std::wstring& filePath);
	// 把图存成缓存文件，再交给外部插件 ImageReader.exe 做文字识别。插件先在本 exe
	// 同目录找，再找 %appdata%\ScreenCapture\plugin，都找不到就用默认浏览器打开它的
	// release 页面让用户自己下。缓存图由插件读完后自己删。
	// 文字识别插件装了没有。没装的时候文字识别走系统自带的 OCR（见 Translate）
	static bool hasImageReader();
	static bool openWithImageReader(const int w, const int h, BYTE* data);
	// 用 quirc 识别图里的二维码，返回识别到的内容，没识别到返回空串。
	// 图里有多个码时用换行拼在一起
	static std::wstring decodeQrCode(const int w, const int h, BYTE* data);
};
