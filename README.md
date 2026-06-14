# MATLAB Background Patcher

Add a customizable background image to MATLAB R2025a's desktop.

[中文版](README_CN.md)

![like this](/Pictures/ScreenShot2026-06-14 190154.png)

## Features

- **Scope 1** — Overlay on the Command Window area only (Win32 layered window)
- **Scope 2** — Overlay on the entire MATLAB window (Win32 layered window)
- **Scope 3** — Inject CSS/JS directly into MATLAB's desktop DOM via Chrome DevTools Protocol (CDP). Supports image background + dimming overlay on any CSS selector.

All modes support: adjustable opacity, black dimming layer, and 5 image scale modes (Fit / Fill / Stretch / Center / Tile).

## Requirements

- MATLAB R2025a (other versions may work but are untested)
- MinGW-w64 (for building from source)
- Windows 10/11

## Quick Start

### Option A: Download Release

1. Download `matlab-bg-patcher.zip` from [Releases](../../releases)
2. Extract to `D:\GNUMATLAB\matlab\bin\` (or anywhere next to `matlab.exe`)
3. Edit `matlab_bg.ini` to set your image path
4. Run `matlab_bg.exe`

### Option B: Build from Source

```powershell
git clone https://github.com/rD227/matlab-background-patch.git
cd matlab-background-patch
# Or better: clone directly into MATLAB's bin directory
# git clone https://github.com/rD227/matlab-background-patch.git D:\GNUMATLAB\matlab\bin\matlab_bg_patcher
```

Then run `build.bat` (requires MinGW-w64 g++ in PATH).

## Usage

### Method 1: INI File

Edit `matlab_bg.ini` and double-click `matlab_bg.exe`:

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

### Method 2: Command Line

```powershell
.\matlab_bg.exe "C:\path\to\bg.png" 30 30 1 150 3 9222 "#commandWindowWrapper"
# Arguments: [image] [opacity] [dimming] [scale] [speed] [scope] [cdp_port] [cdp_target]
```

### Auto-Launch (Scope 3)

When `Scope=3`, the patcher automatically:

1. Checks if MATLAB is already running
2. If not, searches for `matlab.exe` (up to 4 directory levels up)
3. Launches MATLAB with `--remote-debugging-port=9222` + the `MW_CEF_STARTUP_OPTIONS` environment variable
4. Waits for the CDP debug port to become available
5. Injects the background CSS/JS
6. Monitors MATLAB — exits when MATLAB closes

> **Note:** MATLAB R2025a's desktop is rendered with Chromium Embedded Framework (CEF). Scope 3 uses Chrome DevTools Protocol over WebSocket to directly manipulate the DOM.

## INI File Reference

| Option | Default | Description |
|---|---|---|
| `Image` | *(empty)* | Full path to background image (PNG / JPG / BMP / GIF) |
| `Scope` | `2` | `1` = Command Window area only (Win32 overlay) <br> `2` = Entire MATLAB window (Win32 overlay) <br> `3` = CSS/JS DOM injection via CDP |
| `Opacity` | `30` | Image opacity 0–100 (100 = fully opaque) |
| `Dimming` | `30` | Black overlay opacity 0–100 (0 = no darkening) |
| `ScaleMode` | `1` | `0` = Fit (keep aspect ratio) <br> `1` = Fill (cover, crop) <br> `2` = Stretch <br> `3` = Center (original size) <br> `4` = Tile |
| `Speed` | `150` | Refresh interval in ms (50–2000). For Scope 3, this only affects the CDP re-injection check interval |
| `CdpPort` | `0` | CDP debug port for Scope 3. `0` = auto-detect (tries 9222–9229) |
| `CdpTarget` | `#commandWindowWrapper` | CSS selector for Scope 3. Common targets:<br>`#commandWindowWrapper` — Command Window<br>`.editorWindow` — All editor tabs<br>`.editorWindow.active` — Active editor<br>`.editorWindow.liveCode` — Live Editor<br>`#commandWindowWrapper,.editorWindow` — Both |
| `HttpPort` | `9221` | Local HTTP image server port (Scope 3). Change if port conflicts |
| `DebugLog` | `1` | `0` = no log file, `1` = write `matlab_bg.log` |

## Build

```batch
build.bat
```

Requires MinGW-w64 with g++ in PATH. Produces `matlab_bg.exe`.

## How It Works (Scope 3)

1. Patcher connects to MATLAB's CEF debug port (CDP over WebSocket)
2. Sends `Runtime.evaluate` commands to inject JavaScript
3. JS creates two DOM elements as direct children of the target CSS selector:
   - `<img>` with the background image (via `blob:` URL from chunked base64 data)
   - `<div>` with black background for dimming
4. CSS is re-injected every 5 seconds to survive page reloads

This approach avoids:
- `file://` URL restrictions (CEF blocks file:// from https:// pages)
- Mixed content issues (no actual HTTP requests — image data is transferred via CDP chunks)
- MATLAB UI interference (elements are appended with `pointer-events: none`)

## License

GPL3.0
