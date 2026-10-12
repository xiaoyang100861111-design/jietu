#pragma once
#include <include/Ling.h>
#include <functional>
#include <string>

// 账号与配置同步。
// 登录之后，软件的配置（config.json 里除了"只属于这台电脑"的那几项）和快捷话术都会存到服务端
// （https://dk.zx3777.com，和打卡是同一个服务），换一台电脑登录同一个账号就能拿回来。
// 打卡的团队身份也在配置里，所以它跟着账号一起走，不用再填邀请码。
//   - 启动时：服务端比本机新就先拉下来（最多等 3 秒，连不上就用本机的）；
//   - 运行中：每半分钟看一眼，配置或话术有改动就传上去；
//   - 两台电脑同时改，以后保存的那一次为准。
// 不同步的：窗口位置和大小、开机自启、忽略的更新版本（这些是每台电脑自己的事），
// 以及话术里的图片本身和文件类话术指向的文件（只同步文字和路径）。
class Sync
{
public:
	using Done = std::function<void(bool ok, const std::wstring& msg)>;
	// 要在 Setting::init 之后、其他模块读配置之前调
	static void init();
	static void dispose();
	static bool isLoggedIn();
	static std::wstring username();
	// 登录 / 注册。成功后：服务端有存过配置就取回来覆盖本机（本机原来的先备份）并重启软件；
	// 服务端是空的（新账号）就把本机的传上去
	static void login(const std::wstring& user, const std::wstring& pwd, bool isRegister, Done done);
	// 退出登录：本机的配置原样留着，只是不再同步
	static void logout();
	// 现在就把本机的配置传上去
	static void pushNow(Done done);
};
