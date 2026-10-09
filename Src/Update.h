#pragma once
#include <include/Ling.h>
#include <string>

// 升级。
// 版本号就是编译时间（见 BuildInfo.h），服务端是 GitHub 仓库的 dist 分支：
//   version.json —— { "version": 最新版本号, "notes": 这一版改了什么, "url": exe 的下载地址 }
//   UU截图.exe   —— 最新版的 exe
// 每次启动查一次。有新版就弹窗告诉用户改了什么，可以立即升级、忽略这一版、或者以后都不查。
// 升级：新 exe 下到数据目录 -> 起一段 PowerShell 等本进程退出、把新 exe 覆盖过去、
// 用 --enter=tray 重新拉起来。
class Update
{
public:
	// 当前版本号（给"关于"页和日志用）
	static std::wstring version();
	// 启动后调一次：过几秒去查，不跟启动抢资源。设置里关了自动检查就什么都不做
	static void checkOnStart();
	// "关于"页上的"检查更新"按钮：马上查，没有新版也会告诉用户一声，忽略过的版本也照样提示
	static void checkNow();
	// 老接口，原来是"回到空闲状态时顺便查一下"，现在不用了，留着是因为好几处还在调
	static void checkLater();
};
