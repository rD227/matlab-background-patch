# MATLAB Background Patcher

给 MATLAB R2025a 的桌面添加可自定义的背景图片。

[English](README.md)

![截图](Pictures/ScreenShot2026-06-14-190154.png)

**喜欢就加星标吧**

## 功能

- **Scope 1** — 仅在命令行窗口区域叠加背景（Win32 分层窗口）
- **Scope 2** — 在整个 MATLAB 窗口上叠加背景（Win32 分层窗口）
- **Scope 3** — 通过 Chrome DevTools Protocol (CDP) 直接在 MATLAB 桌面 DOM 中注入 CSS/JS。支持在任意 CSS 选择器上添加图片背景 + 变暗遮罩。

所有模式均支持：可调节的透明度、黑色变暗层、5 种图片缩放模式（适应/填充/拉伸/居中/平铺）。

## 环境要求

- MATLAB R2025a（其他版本可能可用，但未测试）
- MinGW-w64（从源码编译时需要）
- Windows 10/11

## 快速开始

### 方式 A：下载 Release

1. 从 [Releases](../../releases) 下载 `matlab-bg-patcher.zip`
2. 解压到 `D:\GNUMATLAB\matlab\bin\`（或 `matlab.exe` 所在目录的任意子目录）
3. 编辑 `matlab_bg.ini` 设置图片路径
4. 运行 `matlab_bg.exe`

### 方式 B：从源码编译

```powershell
git clone https://github.com/rD227/matlab-background-patch.git
cd matlab-background-patch
# 或者直接克隆到 MATLAB 的 bin 目录下：
# git clone https://github.com/rD227/matlab-background-patch.git D:\GNUMATLAB\matlab\bin\matlab_bg_patcher
```

然后运行 `build.bat`（需要 MinGW-w64 的 g++ 在 PATH 中）。

## 使用方法

### 方法 1：INI 配置文件

编辑 `matlab_bg.ini`，双击 `matlab_bg.exe`：

```ini
[Background]
Image=C:\Users\xvsu\Desktop\bg.png
Scope=3
Opacity=30
Dimming=30
ScaleMode=1
Speed=150
CdpPort=0
CdpTarget=#commandWindowWrapper
HttpPort=9221
DebugLog=1
```

### 方法 2：命令行参数

```powershell
.\matlab_bg.exe "C:\path\to\bg.png" 30 30 1 150 3 9222 "#commandWindowWrapper"
# 参数顺序：[图片路径] [透明度] [变暗程度] [缩放模式] [刷新速度] [范围] [CDP端口] [CDP选择器]
```

### 自动启动 MATLAB（Scope 3）

当 `Scope=3` 时，patcher 会自动：

1. 检查 MATLAB 是否已经在运行
2. 如果未运行，搜索 `matlab.exe`（从 patcher 目录向上搜索 4 层）
3. 用 `--remote-debugging-port=9222` 参数和 `MW_CEF_STARTUP_OPTIONS` 环境变量启动 MATLAB
4. 等待 CDP 调试端口可用
5. 注入背景 CSS/JS
6. 监控 MATLAB 进程 — MATLAB 退出时自动关闭自己

> **注意：** MATLAB R2025a 的桌面使用 Chromium Embedded Framework (CEF) 渲染。Scope 3 通过 WebSocket 上的 Chrome DevTools Protocol 直接操作 DOM。

> **已知问题：** 如果 MATLAB 启动后背景图片没有显示，请确认 MATLAB 的 CEF 调试端口已正确开启。可以运行 `setenv('MW_CEF_STARTUP_OPTIONS', '--remote-debugging-port=9222')` 然后重启 MATLAB。

## INI 文件参考

| 选项 | 默认值 | 说明 |
|---|---|---|
| `Image` | *(空)* | 背景图片的完整路径（支持 PNG / JPG / BMP / GIF） |
| `Scope` | `2` | `1` = 仅命令行窗口区域（Win32 叠加层） <br> `2` = 整个 MATLAB 窗口（Win32 叠加层） <br> `3` = 通过 CDP 注入 CSS/JS 到 DOM |
| `Opacity` | `30` | 图片透明度 0–100（100 = 完全不透明） |
| `Dimming` | `30` | 黑色遮罩透明度 0–100（0 = 不变暗） |
| `ScaleMode` | `1` | `0` = 适应（保持比例） <br> `1` = 填充（裁切） <br> `2` = 拉伸 <br> `3` = 居中（原始尺寸） <br> `4` = 平铺 |
| `Speed` | `150` | 刷新间隔（毫秒，50–2000）。Scope 3 下此值影响 CDP 的轮询检查间隔 |
| `CdpPort` | `0` | Scope 3 的 CDP 调试端口。`0` = 自动检测（尝试 9222–9229） |
| `CdpTarget` | `#commandWindowWrapper` | Scope 3 的 CSS 选择器。常用目标：<br>`#commandWindowWrapper` — 命令行窗口<br>`.editorWindow` — 所有编辑器标签页<br>`.editorWindow.active` — 当前活跃编辑器<br>`.editorWindow.liveCode` — Live Editor<br>`#commandWindowWrapper,.editorWindow` — 命令行+编辑器 |
| `HttpPort` | `9221` | Scope 3 本地 HTTP 图片服务器端口。端口冲突时可更改 |
| `DebugLog` | `1` | `0` = 不写日志文件，`1` = 写入 `matlab_bg.log` |

## 编译

```batch
build.bat
```

需要 MinGW-w64，g++ 在 PATH 中。生成 `matlab_bg.exe`。

## 工作原理（Scope 3）

1. Patcher 通过 WebSocket 连接到 MATLAB 的 CEF 调试端口（CDP）
2. 发送 `Runtime.evaluate` 命令注入 JavaScript
3. JS 在目标 CSS 选择器下创建两个 DOM 元素：
   - `<img>` 标签承载背景图片（使用 `blob:` URL，数据通过分片传输）
   - `<div>` 标签作为黑色变暗遮罩
4. CSS 每 5 秒重新注入一次，以应对页面刷新

此方案避开了：
- `file://` URL 限制（CEF 禁止 https:// 页面加载 file:// 资源）
- 混合内容问题（无实际 HTTP 请求 — 图片数据通过 CDP 分片传输）
- MATLAB UI 干扰（注入元素设置了 `pointer-events: none`）

## 许可证

GPL3.0
