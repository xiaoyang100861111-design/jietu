#pragma once
#include <Windows.h>

// 自己画的提示框，用来替掉系统那个灰扑扑的 MessageBox。用法、参数、返回值都和 MessageBox 一样
// （MB_OK / MB_OKCANCEL / MB_YESNO / MB_YESNOCANCEL，图标，MB_DEFBUTTON2；返回 IDOK / IDCANCEL / IDYES / IDNO），
// 也是弹出来就卡住调用的地方，等用户点了才返回。跟着黑夜模式和主题色走。
// 只能在 UI 线程调。崩溃报告那两处还是用系统的（那时候程序状态已经不可信了）
int UUMessageBox(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags);
