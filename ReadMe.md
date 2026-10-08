简体中文 | [English](./Doc/ReadMe.en-US.md) | [Русский](./Doc/ReadMe.ru-RU.md) | [Bahasa Indonesia](./Doc/ReadMe.id-ID.md)

![banner](./Doc/banner.png)

**ScreenCapture** 一个小巧但功能强大的Windows截图工具。

## 特性

- 截图、绘图标注、滚动截图（截长图）、录屏（GIF/MP4）、文字识别（OCR）、二维码识别。
- 取景框（拾色器），支持快捷键复制 RGB 颜色（`Ctrl+R`）、 HEX 颜色（`Ctrl+H`）与 CMYK 颜色（`Ctrl+K`）。
- 绘制椭圆、正圆（按住`Shift`）、矩形、正方形（按住`Shift`）、箭头、标号等。
- 绘制曲线、直线（按住`Shift`）、马赛克、橡皮擦、文本。
- 可以随时修改、删除已绘制的元素（鼠标移到元素上）。
- 撤销（`Ctrl+Z`）、重做（`Ctrl+Y`）、保存为文件（`Ctrl+S`）、保存到剪贴板（`Ctrl+C`或双击）。
- 运行速度快、内存占用低。
- 体积小、仅一个可执行文件，无需安装，不依赖任何动态链接库（文字识别除外）。
- 支持多种命令行参数直接启动指定的功能。
- 支持用完即走（进程不驻留在系统中）。
- 多语言支持。

## 本分支新增：截图翻译

- 工具条上多了一个 **译** 按钮：识别选区里的文字，译文直接盖回原位，并把结果钉到桌面上（可以继续标注、复制、保存）。
- 文字识别用的是 Windows 自带的 OCR，不需要额外下载插件；工具条上的"文字识别"在没装 `ImageReader.exe` 时也走它。
- 翻译走微软 Edge 翻译接口（不通时自动换 Google），不需要密钥，需要联网。
- 目标语言默认跟随界面语言（中文界面：外文→中文，中文→英文）。要固定目标语言，在 `config.json` 里加 `"translate":{"target":"en"}`（`zh-Hans` / `en` / `ja` / `ko` …）。
- 命令行：`ScreenCapture.exe --enter=translate` 框选完直接翻译。
- 启动后只驻留托盘，不自动截图。两个全局快捷键：快捷截图（默认 `Ctrl+Alt+A`）、截图翻译（默认 `Ctrl+Alt+T`，框完直接翻译），都可以在设置里改。
- 想截右键菜单这类一按键盘就消失的东西：在 设置 → 快捷键 里把"鼠标触发截图"设成鼠标中键 / 侧键，或者用托盘菜单里的"3 秒后截图"。
- 设置 → 通用设置 里可以改主题颜色。
- **剪切板历史**：自动记录复制过的文本、图片、文件（最多 300 条，收藏的不限）。按 `Ctrl+Alt+V`（可改）或托盘菜单打开面板：直接打字搜索，点一条（或上下键 + 回车）粘贴到原来的窗口，右键只复制不粘贴，☆ 收藏，✕ 删除。记录存在 `%appdata%\ScreenCapture\clip`，可以在通用设置里关掉。
- 本分支关闭了自动升级（否则会被升回不带翻译的官方版本）。

## 下载

[Release](https://github.com/xland/ScreenCapture/releases/) （1MB）

## 常用功能与问题

- 框选截图区域后直接进入图像标记窗口（钉图窗口），标注、文字识别、翻译都在它的工具条上
- 按住 `Ctrl键` 框选则停在截图窗口，出现长截图 / 录屏 / 二维码识别工具条，此时还可以调整选区
- 按住 `Ctrl键` 滚动鼠标滚轮可以放大、缩小图像标记窗口（钉图窗口）
- 长截图拼接不符合预期时，尝试调整截图区域往往能解决问题
- 如手动下载新版本，则必须退出老版本再启动新版本

## 支持的操作系统

- Windows 10 1803 or Later

## 编译

- main分支依赖 [Ling](https://github.com/xland/Ling) GUI 框架.
- 使用 Visual Studio 2026（With C++ Desktop Dev Kit）即可编译项目。
- [2.4.25（基于D2D）](https://github.com/xland/ScreenCapture/tree/2.4.25)或 [2.3.3（基于Qt）](https://github.com/xland/ScreenCapture/tree/2.3.3_qt)是以前的稳定分支。

## 命令行

```
// 截图完成后即退出进程。
> ScreenCapture.exe --auto-quit=true

// 框选完成后不显示工具条，直接进入指定功能：
// pin 钉图/图像标记
> ScreenCapture.exe --enter=pin
// long 长截图
> ScreenCapture.exe --enter=long
// video 屏幕录制
> ScreenCapture.exe --enter=video
// ocr 文字识别
> ScreenCapture.exe --enter=ocr
// qr 二维码识别
> ScreenCapture.exe --enter=qr
// tray 仅注册托盘图标，不执行任何操作
> ScreenCapture.exe --enter=tray

// 两个参数可以联合使用，比如：不注册托盘图标，截完长图后进程直接退出
> ScreenCapture.exe --enter=long --auto-quit=true
```

## 文字识别插件

下载最新版本的文字识别工具 [ImageReader.exe](https://github.com/xland/ImageReader/releases) (约25MB) 并把此文件放置到 `%appdata%\ScreenCapture\plugin`目录下，然后重启应用即可使用（或 `ScreenCapture.exe` 同目录下亦可）

## 便携能力

默认情况下 ScreenCapture 会从 `%appdata%\ScreenCapture\Lang` 目录下读取配置信息、语言文件及插件。

但用户可以在 `ScreenCapture.exe` 同目录下创建一个 `config.json` 的空文件，重启应用，`ScreenCapture` 即会在此文件中设置配置信息。

`ScreenCapture.exe` 同目录下创建一个 `Lang` 子目录，然后把语言文件放置到此目录下，`ScreenCapture` 即会读取此目录下的语言文件。

文字识别插件 `ImageReader.exe` 也可以放置在 `ScreenCapture.exe` 同目录下

## 赞助

<table>
  <tr>
    <td align="center">
      <img alt="支付宝赞助" src="./Doc/alipay.jpg" width="160" height="160">
      <p>支付宝赞助</p>
    </td>
    <td align="center">
      <img alt="微信赞助" src="./Doc/wechat.png" width="160" height="160">
      <p>微信赞助</p>
    </td>
    <td align="center">
      <img alt="作者微信" src="./Doc/author.jpg" width="160" height="160">
      <p>作者微信</p>
    </td>
    <td align="center">
      <img alt="公众号二维码" src="./Doc/gongzhonghao.jpg" width="160" height="160">
      <p>公众号：桌面软件</p>
    </td>
  </tr>
</table>