#pragma once

// 异常上报：把 log.txt 里新出现的崩溃记录和错误记录传到服务器（后台"异常日志"能看到），方便远程排查。
// 崩溃的那一刻不传（进程已经不可信了），而是下次启动时、以及之后每半小时扫一遍日志，把还没传过的传上去。
// 传到哪儿了记在配置目录的 report.pos 里，传过的不会重复传
class Report
{
public:
	// 要在 Log::init 之后调，只在 UI 线程调
	static void init();
};
