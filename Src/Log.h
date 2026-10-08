#pragma once
#include <include/Ling.h>
#include <filesystem>
#include <string>

// 运行日志，写在配置目录下的 log.txt 里（UTF-8，追加）。
// 记三类东西：每次启动的版本和系统信息；程序自己发现的错误；崩溃时的异常码和调用栈。
// 调用栈记的是"模块名+偏移"，配合编译时生成的 map 文件就能查出是哪个函数。
class Log
{
public:
	// 要在 Setting::init 之后调（日志放在它给的数据目录里）。顺便装上崩溃处理
	static void init();
	// 任何线程都可以调
	static void write(const std::wstring& text);
	// 在 catch 块里调：把当前正在处理的那个异常的类型和错误信息记下来。where 说明是在干什么的时候出的错
	static void exception(const std::wstring& where);
	static std::filesystem::path getPath();
};
