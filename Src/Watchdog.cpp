#include "pch.h"
#include <atomic>
#include <format>
#include <string>
#include <thread>
#include "Watchdog.h"
#include "Log.h"

namespace {
	// 探测用的窗口和它所在的线程（界面线程）。只在 init 里写一次，后台线程只读
	HWND probeHwnd{ nullptr };
	DWORD uiThreadId{ 0 };
	std::atomic<bool> stopped{ false };
	constexpr DWORD intervalMs{ 3000 };     // 两次探测之间隔多久
	constexpr DWORD probeTimeoutMs{ 3000 }; // 一次探测最多等多久
	constexpr int maxMisses{ 10 };          // 连着这么多次没人接才算卡死（约 30 秒以上）

	// 把界面线程挂起、取它的寄存器现场、走一遍调用栈，再放它继续。
	// 记栈要趁它停着的时候，不然栈内存一边走一边变
	std::wstring uiStack()
	{
		std::wstring stack;
		HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, uiThreadId);
		if (!thread) return stack;
		if (SuspendThread(thread) != (DWORD)-1) {
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_FULL;
			if (GetThreadContext(thread, &ctx)) stack = Log::describeStack(&ctx);
			ResumeThread(thread);
		}
		CloseHandle(thread);
		return stack;
	}

	void watch()
	{
		int misses{ 0 };
		ULONGLONG hungSince{ 0 };
		while (!stopped) {
			Sleep(intervalMs);
			if (stopped) break;
			// 窗口没了只会是界面线程正常退出了（只收消息的窗口随线程一起销毁），不用再盯
			if (!IsWindow(probeHwnd)) break;
			const auto before = GetTickCount64();
			DWORD_PTR res{ 0 };
			// 界面线程只要还在泵消息（包括模态框那种嵌套的消息循环）就能接到这条同步消息
			if (SendMessageTimeout(probeHwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, probeTimeoutMs, &res)) {
				misses = 0;
				continue;
			}
			if (stopped || !IsWindow(probeHwnd)) break;
			if (misses++ == 0) hungSince = before;
			if (misses < maxMisses) continue;
			// 真卡死了。写日志要抢日志的锁，万一界面线程正好卡在写日志的路上，这里也会跟着卡住，
			// 所以先另起一条线程兜底：几秒后不管日志写没写完都把进程结束掉
			std::thread([]() {
				Sleep(5000);
				TerminateProcess(GetCurrentProcess(), 0xDEAD);
			}).detach();
			const auto seconds = (GetTickCount64() - hungSince) / 1000;
			const auto stack = uiStack();
			Log::write(std::format(L"ERROR ui hung {}s, exiting\r\n  stack:{}", seconds, stack));
			TerminateProcess(GetCurrentProcess(), 0xDEAD);
		}
	}
}

void Watchdog::init()
{
	if (probeHwnd || IsDebuggerPresent()) return;
	static const wchar_t* className = L"UUWatchdogProbe";
	auto inst = GetModuleHandle(nullptr);
	WNDCLASSW wc{};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = inst;
	wc.lpszClassName = className;
	if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
	// 只收消息的窗口：不显示、不占 z 序。建在界面线程上，发给它的消息就得由界面线程来接
	probeHwnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
	if (!probeHwnd) return;
	uiThreadId = GetCurrentThreadId();
	std::thread(watch).detach();
}

void Watchdog::stop()
{
	stopped = true;
}
