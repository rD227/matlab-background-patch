/*
 * MATLAB Background Image Patcher v2.0 — Pure Win32 overlay
 *
 * Architecture (adapted from octave-bg-patcher):
 *   Scope=1 (Command Window):  find MATLAB's Command Window CEF area
 *                              (Chrome_RenderWidgetHostHWND child) and
 *                              overlay only that sub-window.
 *   Scope=2 (Entire Window):   find MATLAB's main window (by process name)
 *                              and overlay the entire window.
 *   Scope=3 (CDP CSS Inject):  connect to MATLAB's CEF remote debugging port
 *                              (Chrome DevTools Protocol) via WebSocket and
 *                              inject a <style> element directly into the DOM.
 *                              Targets the CSS selector set by CdpTarget.
 *
 * Build (MinGW-w64):
 *   g++ -O2 -static -mwindows -municode matlab_bg.cpp -o matlab_bg.exe
 *     -luser32 -lgdi32 -lgdiplus -lole32 -lcomctl32 -ldwmapi -lws2_32
 *
 * Usage:
 *   matlab_bg.exe [image_path] [opacity] [dimming] [scale_mode] [speed] [scope] [cdp_port] [cdp_target]
 *
 *   Or edit matlab_bg.ini:
 *     [Background]
 *     Image=C:\path\to\bg.png
 *     Scope=2              ; 1=Command Window, 2=Entire window, 3=CDP CSS inject
 *     Opacity=30
 *     Dimming=30
 *     ScaleMode=1
 *     Speed=150            ; timer interval in ms (50-2000)
 *     CdpPort=0            ; CDP debug port (0=auto-detect 9222-9229)
 *     CdpTarget=#commandWindowWrapper  ; CSS selector for Scope=3
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// ─── Configuration ───────────────────────────────────────────────────

struct Config {
    WCHAR imagePath[MAX_PATH] = {};
    int   scope     = 2;    // 1=Command Window HWND, 2=Entire window, 3=CDP CSS inject
    int   opacity   = 30;   // 0-100
    int   dimming   = 30;   // 0-100
    int   scaleMode = 1;    // 0=fit, 1=fill, 2=stretch, 3=center, 4=tile
    int   speed     = 150;  // timer interval ms (50-2000)
    int   cdpPort   = 0;    // CDP debug port (0=auto-detect 9222-9229)
    WCHAR cdpTarget[128] = L"#commandWindowWrapper"; // CSS selector for Scope=3
};

// ─── Globals ──────────────────────────────────────────────────────────

static Config        g_cfg;
static HWND           g_hwndMATLAB  = nullptr;  // MATLAB main window
static HWND           g_hwndTarget  = nullptr;  // actual overlay target (same as MATLAB or a child)
static HWND           g_hwndOverlay = nullptr;
static HWINEVENTHOOK  g_hWinEventHook = nullptr;
static UINT_PTR       g_timerId     = 1;
static ULONG_PTR      g_gdiToken    = 0;
static int            g_retryCount  = 0;
static WCHAR          g_targetClass[256] = {};  // class name of target we found

// ─── Forward declarations ─────────────────────────────────────────────

static void          Log(const WCHAR *fmt, ...);
static void          InitLog();
static void          LoadConfig();
static bool          FindMATLABTarget();
static bool          IsMATLABProcess(HWND hwnd, WCHAR *outExeName, size_t outLen);
static bool          CreateOverlay();
static void          UpdateOverlay();
static LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
static void CALLBACK  WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
static Gdiplus::Bitmap* LoadImage(const WCHAR *path);
static void          RepaintOverlay(HDC hdcScreen = nullptr);

// ─── Logging ──────────────────────────────────────────────────────────

static WCHAR g_logPath[MAX_PATH] = {};
static bool  g_logFile = true;

static void InitLog()
{
    GetModuleFileNameW(nullptr, g_logPath, MAX_PATH);
    WCHAR *slash = wcsrchr(g_logPath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat(g_logPath, L"matlab_bg.log");
    HANDLE hFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
}

static void Log(const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = _vsnwprintf(buf, 1024, fmt, args);
    va_end(args);
    if (len < 0 || len >= 1024) len = 1023;

    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR ts[64];
    wsprintfW(ts, L"[%02d:%02d:%02d.%03d] ",
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    if (g_logFile) {
        HANDLE hFile = CreateFileW(g_logPath, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD w;
            char tsA[128], bufA[2048];
            WideCharToMultiByte(CP_UTF8, 0, ts,  -1, tsA,  128, nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, bufA, 2048, nullptr, nullptr);
            WriteFile(hFile, tsA, (DWORD)strlen(tsA), &w, nullptr);
            WriteFile(hFile, bufA, (DWORD)strlen(bufA), &w, nullptr);
            WriteFile(hFile, "\r\n", 2, &w, nullptr);
            CloseHandle(hFile);
        }
    }
    OutputDebugStringW(ts);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

// ─── Configuration I/O ────────────────────────────────────────────────

static std::wstring GetExeDir()
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.rfind(L'\\');
    if (pos != std::wstring::npos) s.resize(pos);
    return s;
}

static void LoadConfig()
{
    std::wstring iniPath = GetExeDir() + L"\\matlab_bg.ini";
    g_cfg = Config{};

    GetPrivateProfileStringW(L"Background", L"Image",     L"", g_cfg.imagePath, MAX_PATH, iniPath.c_str());
    g_cfg.scope     = GetPrivateProfileIntW(L"Background", L"Scope",     2,   iniPath.c_str());
    g_cfg.opacity   = GetPrivateProfileIntW(L"Background", L"Opacity",   30,  iniPath.c_str());
    g_cfg.dimming   = GetPrivateProfileIntW(L"Background", L"Dimming",   30,  iniPath.c_str());
    g_cfg.scaleMode = GetPrivateProfileIntW(L"Background", L"ScaleMode", 1,   iniPath.c_str());
    g_cfg.speed     = GetPrivateProfileIntW(L"Background", L"Speed",     150, iniPath.c_str());
    g_cfg.cdpPort   = GetPrivateProfileIntW(L"Background", L"CdpPort",   0,   iniPath.c_str());
    GetPrivateProfileStringW(L"Background", L"CdpTarget", L"#commandWindowWrapper", g_cfg.cdpTarget, 128, iniPath.c_str());

    // Clamp
    if (g_cfg.scope < 1 || g_cfg.scope > 3)     g_cfg.scope = 2;
    if (g_cfg.cdpPort < 0 || g_cfg.cdpPort > 65535) g_cfg.cdpPort = 0;
    if (g_cfg.opacity < 0)   g_cfg.opacity = 0;
    if (g_cfg.opacity > 100) g_cfg.opacity = 100;
    if (g_cfg.dimming < 0)   g_cfg.dimming = 0;
    if (g_cfg.dimming > 100) g_cfg.dimming = 100;
    if (g_cfg.scaleMode < 0 || g_cfg.scaleMode > 4) g_cfg.scaleMode = 1;
    if (g_cfg.speed < 50)    g_cfg.speed = 50;
    if (g_cfg.speed > 2000)  g_cfg.speed = 2000;

    // Command-line override
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2) wcscpy(g_cfg.imagePath, argv[1]);
        if (argc >= 3) g_cfg.opacity   = _wtoi(argv[2]);
        if (argc >= 4) g_cfg.dimming   = _wtoi(argv[3]);
        if (argc >= 5) g_cfg.scaleMode = _wtoi(argv[4]);
        if (argc >= 6) g_cfg.speed     = _wtoi(argv[5]);
        if (argc >= 7) g_cfg.scope     = _wtoi(argv[6]);
        if (argc >= 8) g_cfg.cdpPort   = _wtoi(argv[7]);
        if (argc >= 9) wcsncpy(g_cfg.cdpTarget, argv[8], 127);
        LocalFree(argv);
    }

    Log(L"Config: scope=%d image='%s' opacity=%d dimming=%d scaleMode=%d speed=%d cdpPort=%d cdpTarget='%s'",
        g_cfg.scope, g_cfg.imagePath, g_cfg.opacity, g_cfg.dimming, g_cfg.scaleMode, g_cfg.speed,
        g_cfg.cdpPort, g_cfg.cdpTarget);
}

// ─── Process detection ────────────────────────────────────────────────

static bool IsMATLABProcess(HWND hwnd, WCHAR *outExeName, size_t outLen)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    WCHAR exePath[MAX_PATH] = {};
    DWORD dwSize = MAX_PATH;
    bool found = QueryFullProcessImageNameW(hProc, 0, exePath, &dwSize);
    CloseHandle(hProc);
    if (!found) return false;

    WCHAR *fname = wcsrchr(exePath, L'\\');
    if (fname) fname++; else fname = exePath;

    if (outExeName && outLen > 0) {
        wcsncpy(outExeName, fname, outLen);
        outExeName[outLen - 1] = L'\0';
    }

    return (wcsstr(fname, L"MATLAB") != nullptr || wcsstr(fname, L"matlab") != nullptr);
}

// ─── Find Target Window (Scope=2: main window, Scope=1: Command Window child) ──

struct EnumData {
    HWND   bestMain;     // Best main MATLAB window (largest)
    HWND   bestCmdWnd;   // Best Command Window child (CEF render area)
    int    bestMainArea;
    int    bestCmdArea;
    bool   firstPass;
};

// Scope=2 helper: pick the largest visible MATLAB top-level window
static void PickMainWindow(HWND hwnd, EnumData *data, const WCHAR *exeName)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);
    if (area > data->bestMainArea) {
        data->bestMain     = hwnd;
        data->bestMainArea = area;
    }
}

// Context for recursive child enumeration
struct ChildCtx {
    EnumData *data;
};

// Recursive child window enumeration callback
static BOOL CALLBACK EnumChildProc(HWND child, LPARAM lp)
{
    ChildCtx *c = (ChildCtx*)lp;
    WCHAR cls[256] = {};
    GetClassNameW(child, cls, 256);

    bool isCEF = (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0);
    bool isSwing = (wcscmp(cls, L"SunAwtCanvas") == 0);

    if (isCEF || isSwing) {
        RECT r;
        GetWindowRect(child, &r);
        int area = (r.right - r.left) * (r.bottom - r.top);
        if (area > c->data->bestCmdArea) {
            c->data->bestCmdWnd = child;
            c->data->bestCmdArea = area;
        }
    }

    // Recurse into children with children
    EnumChildWindows(child, EnumChildProc, lp);
    return TRUE;
}

// Scope=1 helper: find the CEF Command Window inside MATLAB
// R2025a: Chrome_RenderWidgetHostHWND
// Older:  SunAwtCanvas (Java Swing Command Window area)
// Strategy: enumerate all child windows, pick the largest
//           Chrome_RenderWidgetHostHWND inside the main MATLAB window.
static void FindCommandWindowChild(HWND hwndMain, EnumData *data)
{
    ChildCtx ctx = { data };
    EnumChildWindows(hwndMain, EnumChildProc, (LPARAM)&ctx);
}

static BOOL CALLBACK EnumMATLABProc(HWND hwnd, LPARAM lParam)
{
    EnumData *data = (EnumData*)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    LONG style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) return TRUE;

    WCHAR cls[256] = {}, title[256] = {};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);

    WCHAR exeName[64] = {};
    if (!IsMATLABProcess(hwnd, exeName, 64)) return TRUE;

    if (data->firstPass) {
        RECT r; GetWindowRect(hwnd, &r);
        Log(L"  [scan] hwnd=0x%p class='%s' title='%s' exe=%s rect=(%d,%d,%d,%d)",
            (void*)hwnd, cls, title, exeName, r.left, r.top, r.right, r.bottom);
    }

    PickMainWindow(hwnd, data, exeName);
    return TRUE;
}

static bool FindMATLABTarget()
{
    g_hwndMATLAB  = nullptr;
    g_hwndTarget  = nullptr;
    g_targetClass[0] = L'\0';

    // Step 1: Find MATLAB main window
    EnumData data = { nullptr, nullptr, 0, 0, true };
    EnumWindows(EnumMATLABProc, (LPARAM)&data);

    if (!data.bestMain || !IsWindow(data.bestMain)) {
        Log(L"No MATLAB window found. Dumping all visible top-level windows:");
        EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
            WCHAR cls[256], title[256]; DWORD pid;
            GetClassNameW(hwnd, cls, 256);
            GetWindowTextW(hwnd, title, 256);
            GetWindowThreadProcessId(hwnd, &pid);
            RECT r; GetWindowRect(hwnd, &r);
            Log(L"  hwnd=0x%p class='%s' title='%s' pid=%lu rect=(%d,%d,%d,%d)",
                (void*)hwnd, cls, title, pid, r.left, r.top, r.right, r.bottom);
            return TRUE;
        }, 0);
        return false;
    }

    g_hwndMATLAB = data.bestMain;

    WCHAR cls[256], title[256];
    GetClassNameW(data.bestMain, cls, 256);
    GetWindowTextW(data.bestMain, title, 256);

    if (g_cfg.scope == 1) {
        // Scope=1: Find Command Window child inside MATLAB
        FindCommandWindowChild(data.bestMain, &data);

        if (data.bestCmdWnd && IsWindow(data.bestCmdWnd)) {
            g_hwndTarget = data.bestCmdWnd;
            GetClassNameW(g_hwndTarget, g_targetClass, 256);

            RECT r; GetWindowRect(g_hwndTarget, &r);
            Log(L"Scope=1: Found Command Window: hwnd=0x%p class='%s' area=%d rect=(%d,%d,%d,%d)",
                (void*)g_hwndTarget, g_targetClass, data.bestCmdArea,
                r.left, r.top, r.right, r.bottom);
        } else {
            // Fallback: use whole main window
            Log(L"Scope=1: No Command Window child found (Chrome_RenderWidgetHostHWND / SunAwtCanvas).");
            Log(L"         Falling back to entire MATLAB window.");
            g_hwndTarget = data.bestMain;
            GetClassNameW(g_hwndTarget, g_targetClass, 256);
        }
    } else {
        // Scope=2: Entire MATLAB window
        g_hwndTarget = data.bestMain;
        GetClassNameW(g_hwndTarget, g_targetClass, 256);
    }

    RECT r; GetWindowRect(g_hwndTarget, &r);
    Log(L"Target: hwnd=0x%p class='%s' title='%s' rect=(%d,%d,%d,%d) scope=%d",
        (void*)g_hwndTarget, g_targetClass, title,
        r.left, r.top, r.right, r.bottom, g_cfg.scope);

    return true;
}

// ─── Image Loading (GDI+) ─────────────────────────────────────────────

static Bitmap* LoadImage(const WCHAR *path)
{
    if (path[0] == 0) return nullptr;
    Bitmap *bmp = Bitmap::FromFile(path);
    if (bmp && bmp->GetLastStatus() == Ok) {
        Log(L"Image loaded: %s (%d x %d)", path, (int)bmp->GetWidth(), (int)bmp->GetHeight());
        return bmp;
    }
    if (bmp) { Log(L"GDI+ failed: %s (status=%d)", path, (int)bmp->GetLastStatus()); delete bmp; }
    return nullptr;
}

// ─── Overlay Window ───────────────────────────────────────────────────

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RepaintOverlay(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreateOverlay()
{
    static const WCHAR *OVERLAY_CLASS = L"MATLAB_BG_Overlay_Class";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = OVERLAY_CLASS;
    if (!GetClassInfoW(wc.hInstance, OVERLAY_CLASS, &wc))
        RegisterClassW(&wc);

    // Scope=1 (Command Window only): we need WS_EX_TRANSPARENT for mouse passthrough
    // on the specific CEF area.  Scope=2 (full window): also transparent.
    // Both modes use a frameless tool window with layered alpha.
    g_hwndOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, L"MATLAB BG Overlay", WS_POPUP,
        0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_hwndOverlay) {
        Log(L"CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    BYTE alpha = (BYTE)(g_cfg.opacity * 255 / 100);
    SetLayeredWindowAttributes(g_hwndOverlay, 0, alpha, LWA_ALPHA);
    ShowWindow(g_hwndOverlay, SW_SHOWNOACTIVATE);

    MARGINS m = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwndOverlay, &m);

    Log(L"Overlay created: hwnd=0x%p", (void*)g_hwndOverlay);
    return true;
}

// ─── Paint ────────────────────────────────────────────────────────────

static void RepaintOverlay(HDC hdcScreen)
{
    static Bitmap *s_lastBmp = nullptr;
    static WCHAR   s_lastPath[MAX_PATH] = {};

    if (wcscmp(g_cfg.imagePath, s_lastPath) != 0) {
        if (s_lastBmp) { delete s_lastBmp; s_lastBmp = nullptr; }
        s_lastBmp = LoadImage(g_cfg.imagePath);
        wcscpy(s_lastPath, g_cfg.imagePath);
    }

    Bitmap *bmp = s_lastBmp;
    if (!bmp || !g_hwndOverlay) return;

    RECT clientRect;
    GetClientRect(g_hwndOverlay, &clientRect);
    int w = clientRect.right - clientRect.left;
    int h = clientRect.bottom - clientRect.top;
    if (w <= 0 || h <= 0) return;

    HDC hdcMem = CreateCompatibleDC(nullptr);
    if (!hdcMem) return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp) { DeleteDC(hdcMem); return; }
    HBITMAP hBmpOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    Graphics g(hdcMem);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    int imgW = (int)bmp->GetWidth();
    int imgH = (int)bmp->GetHeight();
    if (imgW <= 0 || imgH <= 0) {
        SelectObject(hdcMem, hBmpOld); DeleteObject(hBmp); DeleteDC(hdcMem); return;
    }

    Bitmap scaled(w, h, PixelFormat32bppARGB);
    Graphics gScaled(&scaled);
    gScaled.SetSmoothingMode(SmoothingModeHighQuality);
    gScaled.Clear(Color(0, 0, 0, 0));

    int dx = 0, dy = 0, sw = w, sh = h;
    double ratio = (double)imgW / imgH;

    switch (g_cfg.scaleMode) {
    case 0: // Fit
        if (w / (double)h > ratio) { sh = h; sw = (int)(h * ratio); dx = (w - sw) / 2; }
        else                       { sw = w; sh = (int)(w / ratio); dy = (h - sh) / 2; }
        gScaled.DrawImage(bmp, dx, dy, sw, sh);
        break;
    case 1: // Fill (default)
        if (w / (double)h > ratio) { sw = w; sh = (int)(w / ratio); dy = (h - sh) / 2; }
        else                       { sh = h; sw = (int)(h * ratio); dx = (w - sw) / 2; }
        gScaled.DrawImage(bmp, dx, dy, sw, sh);
        break;
    case 2: // Stretch
        gScaled.DrawImage(bmp, 0, 0, w, h);
        break;
    case 3: // Center
        dx = (w - imgW) / 2; dy = (h - imgH) / 2;
        gScaled.DrawImage(bmp, dx, dy, imgW, imgH);
        break;
    case 4: // Tile
        { TextureBrush brush(bmp, WrapModeTile); gScaled.FillRectangle(&brush, 0, 0, w, h); }
        break;
    default:
        gScaled.DrawImage(bmp, 0, 0, w, h);
        break;
    }

    ColorMatrix cm = {};
    cm.m[0][0] = 1.0f; cm.m[1][1] = 1.0f; cm.m[2][2] = 1.0f;
    cm.m[3][3] = g_cfg.opacity / 100.0f;
    cm.m[4][4] = 1.0f;
    ImageAttributes ia;
    ia.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    g.DrawImage(&scaled, Rect(0, 0, w, h), 0, 0, w, h, UnitPixel, &ia);

    if (g_cfg.dimming > 0) {
        SolidBrush dimBrush(Color((BYTE)(g_cfg.dimming * 255 / 100), 0, 0, 0));
        g.FillRectangle(&dimBrush, 0, 0, w, h);
    }

    g.Flush(FlushIntentionSync);

    if (hdcScreen) {
        BitBlt(hdcScreen, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
    } else {
        InvalidateRect(g_hwndOverlay, nullptr, FALSE);
    }

    SelectObject(hdcMem, hBmpOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
}

// ─── Update Overlay Position ──────────────────────────────────────────

static void UpdateOverlay()
{
    if (!g_hwndMATLAB || !g_hwndTarget || !g_hwndOverlay) return;

    // Check if target window is still alive
    if (!IsWindow(g_hwndMATLAB)) {
        Log(L"MATLAB window destroyed, exiting");
        DestroyWindow(g_hwndOverlay);
        return;
    }

    // For Scope=1, re-validate the target child window
    if (g_cfg.scope == 1) {
        if (!IsWindow(g_hwndTarget)) {
            Log(L"Target child window (Command Window) destroyed. Re-scanning...");
            FindMATLABTarget();
            if (!g_hwndTarget) return;
        }
    }

    // Get target window rect in screen coordinates
    RECT r;
    GetWindowRect(g_hwndTarget, &r);
    int x = r.left;
    int y = r.top;
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    // Update window opacity
    BYTE alpha = (BYTE)(g_cfg.opacity * 255 / 100);
    SetLayeredWindowAttributes(g_hwndOverlay, 0, alpha, LWA_ALPHA);

    // Move and resize overlay
    SetWindowPos(g_hwndOverlay, nullptr, x, y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // GWLP_HWNDPARENT to the MATLAB main window (not the child!)
    // This keeps the overlay on top of MATLAB but below other apps.
    SetWindowLongPtrW(g_hwndOverlay, GWLP_HWNDPARENT, (LONG_PTR)g_hwndMATLAB);

    // Trigger repaint
    InvalidateRect(g_hwndOverlay, nullptr, FALSE);
}

// ─── WinEvent Hook + Timer ────────────────────────────────────────────

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event,
                                  HWND hwnd, LONG idObject, LONG idChild,
                                  DWORD, DWORD)
{
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    // Track BOTH the main window AND the target child
    if (hwnd != g_hwndMATLAB && hwnd != g_hwndTarget) return;
    switch (event) {
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_VALUECHANGE:
        UpdateOverlay();
        break;
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_HIDE:
        Log(L"Target window hidden/destroyed");
        break;
    }
}

static void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (!g_hwndMATLAB) {
        FindMATLABTarget();
        if (g_hwndMATLAB) UpdateOverlay();
    } else if (!IsWindow(g_hwndMATLAB)) {
        g_hwndMATLAB = nullptr;
        g_hwndTarget = nullptr;
    } else {
        // For Scope=1, periodically re-verify the child
        if (g_cfg.scope == 1 && (!g_hwndTarget || !IsWindow(g_hwndTarget))) {
            FindMATLABTarget();
        }
        UpdateOverlay();
    }
}

// ─── CDP (Chrome DevTools Protocol) WebSocket client ──────────────────
// Used by Scope=3 to inject CSS directly into MATLAB's CEF browser DOM.
// Connects to the CEF remote debugging port and sends Runtime.evaluate
// to create a <style> element that sets a background image on the target
// CSS selector (e.g. #commandWindowWrapper).

// Simple base64 encoder for WebSocket handshake key
static void b64encode(const BYTE *in, int len, char *out)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        BYTE a = in[i], b = (i+1 < len) ? in[i+1] : 0, c = (i+2 < len) ? in[i+2] : 0;
        out[o++] = T[a >> 2];
        out[o++] = T[((a & 3) << 4) | (b >> 4)];
        out[o++] = (i+1 < len) ? T[((b & 15) << 2) | (c >> 6)] : '=';
        out[o++] = (i+2 < len) ? T[c & 63] : '=';
    }
    out[o] = '\0';
}

// HTTP GET /json – returns the WebSocket URL of the MATLAB main page.
// The /json endpoint lists all CEF pages; we need to find the one whose URL
// contains "index-jsd.html" (the MATLAB desktop), not undocked containers.
static bool CdpGetPageUrl(int port, char *hostOut, int *portOut, char *pathOut, int pathCap)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { Log(L"CDP: socket() failed: %d", WSAGetLastError()); return false; }

    // Set timeouts so we don't block forever
    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    SOCKADDR_IN addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    Log(L"CDP: connecting to 127.0.0.1:%d ...", port);
    if (connect(s, (SOCKADDR*)&addr, sizeof(addr)) != 0) {
        Log(L"CDP: connect() to port %d failed: %d", port, WSAGetLastError());
        closesocket(s); return false;
    }
    Log(L"CDP: connected, sending GET /json");

    const char *req = "GET /json HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(s, req, (int)strlen(req), 0);

    // Read response in a loop – TCP may fragment
    char buf[16384] = {};
    int total = 0;
    int n;
    while (total < (int)sizeof(buf) - 1) {
        n = recv(s, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    closesocket(s);
    if (total <= 0) { Log(L"CDP: no response from /json"); return false; }
    buf[total] = '\0';

    // ── Find the MATLAB main desktop page ───────────────────────────────
    // The /json response is a JSON array. We want the page whose "url" field
    // contains "index-jsd.html" (the main MATLAB desktop that has
    // #commandWindowWrapper in its DOM).
    //
    // Strategy: walk through each "webSocketDebuggerUrl" entry; use the
    // preceding "url" field to decide whether this is the right page.

    const char *wsKey = "\"webSocketDebuggerUrl\": \"";
    const char *urlKey = "\"url\": \"";
    const char *idxSig = "index-jsd.html";    // MATLAB main desktop signature
    const char *bestWs = nullptr;
    const char *searchPos = buf;

    while ((searchPos = strstr(searchPos, wsKey)) != nullptr) {
        searchPos += strlen(wsKey);

        // Look backwards from this match to find the nearest preceding "url" field
        // to decide if this page is the MATLAB desktop
        const char *urlPtr = searchPos;
        const char *prevUrl = nullptr;
        // Search backwards for "url" entries before this ws URL (within same page object)
        const char *scan = searchPos;
        while (scan > buf) {
            scan--;
            if (strncmp(scan, urlKey, strlen(urlKey)) == 0) {
                prevUrl = scan + strlen(urlKey);
                break;
            }
        }

        bool isMain = false;
        if (prevUrl) {
            // Check if this URL contains index-jsd.html
            const char *urlEnd = prevUrl;
            while (*urlEnd && *urlEnd != '"') urlEnd++;
            int urlLen = (int)(urlEnd - prevUrl);
            if (urlLen > 0 && urlLen < 512) {
                // Check for index-jsd signature
                for (int i = 0; i <= urlLen - 12; i++) {
                    if (strncmp(prevUrl + i, idxSig, 13) == 0) {
                        isMain = true;
                        break;
                    }
                }
            }
        }

        if (isMain) {
            bestWs = searchPos;
            Log(L"CDP: found MATLAB desktop page via index-jsd.html in url");
            break;
        }

        // Fallback: if no index-jsd found yet, remember the first ws URL
        // whose page title contains "MATLAB"
        if (!bestWs) {
            // Check title field preceding this ws URL (same backwards scan)
            const char *titleKey = "\"title\": \"";
            const char *scan2 = searchPos;
            const char *prevTitle = nullptr;
            while (scan2 > buf) {
                scan2--;
                if (strncmp(scan2, titleKey, strlen(titleKey)) == 0) {
                    prevTitle = scan2 + strlen(titleKey);
                    break;
                }
            }
            if (prevTitle && strncmp(prevTitle, "MATLAB", 6) == 0) {
                bestWs = searchPos;
                Log(L"CDP: found MATLAB desktop page via title 'MATLAB'");
            }
        }
    }

    // If still not found, fall back to the first ws URL
    if (!bestWs) {
        bestWs = strstr(buf, wsKey);
        if (bestWs) {
            bestWs += strlen(wsKey);
            Log(L"CDP: index-jsd.html not found, using first available page");
        }
    }

    if (!bestWs) {
        Log(L"CDP: webSocketDebuggerUrl not found in /json response");
        return false;
    }

    const char *p = bestWs;

    // Parse ws://host:port/path
    if (strncmp(p, "ws://", 5) != 0) { Log(L"CDP: unexpected ws URL"); return false; }
    p += 5;
    const char *hostStart = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hostLen = (int)(p - hostStart);
    if (hostLen > 63) hostLen = 63;
    memcpy(hostOut, hostStart, hostLen); hostOut[hostLen] = '\0';

    *portOut = (*p == ':') ? atoi(p + 1) : port;  // default to CDP port
    while (*p && *p != '/') p++;
    const char *pathStart = p;
    while (*p && *p != '"') p++;
    int pathLen = (int)(p - pathStart);
    if (pathLen > pathCap - 1) pathLen = pathCap - 1;
    memcpy(pathOut, pathStart, pathLen); pathOut[pathLen] = '\0';

    // Strip trailing double-quote if any
    if (pathLen > 0 && pathOut[pathLen - 1] == '"') pathOut[pathLen - 1] = '\0';

    Log(L"CDP: using page ws://%hs:%d%hs", hostOut, *portOut, pathOut);
    return true;
}

// WebSocket connect + handshake
static SOCKET CdpWsConnect(const char *host, int port, const char *path)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    SOCKADDR_IN addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    // Set send timeout
    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    if (connect(s, (SOCKADDR*)&addr, sizeof(addr)) != 0) {
        Log(L"CDP: ws connect() failed: %d", WSAGetLastError());
        closesocket(s); return INVALID_SOCKET;
    }

    // Generate random 16-byte key
    BYTE keyBytes[16];
    for (int i = 0; i < 16; i++) keyBytes[i] = (BYTE)(rand() & 0xFF);
    char keyB64[32];
    b64encode(keyBytes, 16, keyB64);

    char req[1024];
    int reqLen = _snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n", path, host, port, keyB64);

    if (send(s, req, reqLen, 0) != reqLen) {
        Log(L"CDP: ws handshake send failed");
        closesocket(s); return INVALID_SOCKET;
    }

    char resp[4096] = {};
    int n = recv(s, resp, sizeof(resp) - 1, 0);
    if (n <= 0 || !strstr(resp, "101")) {
        Log(L"CDP: ws handshake failed (no 101)");
        closesocket(s); return INVALID_SOCKET;
    }

    Log(L"CDP: WebSocket connected to %hs:%d%hs", host, port, path);
    return s;
}

// Send a WebSocket text frame (masked, client→server)
static bool CdpWsSend(SOCKET s, const char *json)
{
    int len = (int)strlen(json);
    BYTE frame[10];
    int hdrLen;

    // FIN + text opcode
    frame[0] = 0x81;
    // Mask bit + length
    if (len < 126) {
        frame[1] = (BYTE)(0x80 | len);
        hdrLen = 2;
    } else if (len < 65536) {
        frame[1] = (BYTE)(0x80 | 126);
        frame[2] = (BYTE)(len >> 8);
        frame[3] = (BYTE)(len);
        hdrLen = 4;
    } else {
        frame[1] = (BYTE)(0x80 | 127);
        for (int i = 0; i < 8; i++) frame[2+i] = (BYTE)(len >> (56 - 8*i));
        hdrLen = 10;
    }

    // Masking key (random)
    BYTE mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (BYTE)(rand() & 0xFF);

    // Build masked payload
    std::vector<BYTE> packet(hdrLen + 4 + len);
    memcpy(packet.data(), frame, hdrLen);
    memcpy(packet.data() + hdrLen, mask, 4);
    for (int i = 0; i < len; i++)
        packet[hdrLen + 4 + i] = (BYTE)json[i] ^ mask[i % 4];

    return send(s, (char*)packet.data(), (int)packet.size(), 0) == (int)packet.size();
}

// Receive and log a WebSocket frame (for CDP response)
static void CdpWsRecv(SOCKET s)
{
    BYTE hdr[2];
    int n = recv(s, (char*)hdr, 2, 0);
    if (n < 2) return;
    BYTE opcode = hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    int len = hdr[1] & 0x7F;
    if (len == 126) { BYTE ext[2]; recv(s, (char*)ext, 2, 0); len = (ext[0] << 8) | ext[1]; }
    else if (len == 127) { BYTE ext[8]; recv(s, (char*)ext, 8, 0); len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | ext[i]; }

    BYTE maskKey[4] = {};
    if (masked) recv(s, (char*)maskKey, 4, 0);

    std::vector<char> payload(len + 1);
    if (len > 0) recv(s, payload.data(), len, 0);
    for (int i = 0; i < len; i++) payload[i] ^= maskKey[i % 4];
    payload[len] = '\0';

    Log(L"CDP response (op=%d): %hs", opcode, payload.data());
}

// Auto-detect CDP port by trying 9222..9229
static int CdpAutoDetectPort()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (int port = 9222; port <= 9229; port++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        // Non-blocking-ish: short timeout
        int timeout = 1000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
        SOCKADDR_IN addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((u_short)port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        int ok = connect(s, (SOCKADDR*)&addr, sizeof(addr));
        closesocket(s);
        if (ok == 0) {
            Log(L"CDP: auto-detected port %d", port);
            WSACleanup();
            return port;
        }
    }
    WSACleanup();
    return 0;
}

// Inject CSS via CDP – creates a <style> element in the target page
static bool CdpInjectCSS()
{
    // Auto-detect or use configured port
    int port = g_cfg.cdpPort;
    if (port == 0) port = CdpAutoDetectPort();
    if (port == 0) {
        Log(L"CDP: no debug port found. Enable CEF debugging or set CdpPort in ini.");
        return false;
    }

    Log(L"CDP: CdpInjectCSS starting, port=%d", port);

    // Start WinSock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // 1. GET /json to discover the MATLAB page WebSocket URL
    char host[64] = "127.0.0.1";
    int  wsPort   = 0;
    char path[256] = {};
    if (!CdpGetPageUrl(port, host, &wsPort, path, sizeof(path))) {
        WSACleanup();
        return false;
    }

    // 2. Connect WebSocket
    SOCKET ws = CdpWsConnect(host, wsPort, path);
    if (ws == INVALID_SOCKET) { WSACleanup(); return false; }

    // 3. Convert wide-string config to UTF-8 narrow strings
    char cssSelector[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.cdpTarget, -1, cssSelector, sizeof(cssSelector), nullptr, nullptr);

    // For CSS url(), convert backslashes to forward slashes
    char cssPath[MAX_PATH * 4] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.imagePath, -1, cssPath, sizeof(cssPath), nullptr, nullptr);
    std::string cssPathFixed;
    for (int i = 0; cssPath[i]; i++) {
        if (cssPath[i] == '\\') cssPathFixed += '/';
        else cssPathFixed += cssPath[i];
    }

    // Determine background-size and background-repeat based on scaleMode
    const char *bgSize;
    switch (g_cfg.scaleMode) {
        case 2: bgSize = "100% 100%"; break;     // stretch
        case 0: bgSize = "contain";   break;     // fit
        case 3: bgSize = "auto";      break;     // center
        default:bgSize = "cover";     break;     // fill
    }
    const char *bgRepeat = (g_cfg.scaleMode == 4) ? "repeat" : "no-repeat";

    // 4. Build the CSS text directly (avoid _snprintf with %s for wide strings)
    // The <style> element will be injected into the target page's <head>.
    // Uses ::before for the image and ::after for the dimming overlay.
    std::string css;
    css += cssSelector;
    css += " { position: relative; } ";
    css += cssSelector;
    css += "::before { ";
    css += "content: ''; position: absolute; top: 0; left: 0; right: 0; bottom: 0; ";
    css += "background-image: url('file:///";
    css += cssPathFixed;
    css += "'); ";
    css += "background-size: "; css += bgSize; css += "; ";
    css += "background-position: center; ";
    css += "background-repeat: "; css += bgRepeat; css += "; ";
    css += "opacity: ";
    char numBuf[16];
    _snprintf(numBuf, sizeof(numBuf), "%.2f", g_cfg.opacity / 100.0);
    css += numBuf;
    css += "; z-index: 0; pointer-events: none; } ";
    css += cssSelector;
    css += "::after { ";
    css += "content: ''; position: absolute; top: 0; left: 0; right: 0; bottom: 0; ";
    css += "background: black; ";
    css += "opacity: ";
    _snprintf(numBuf, sizeof(numBuf), "%.2f", g_cfg.dimming / 100.0);
    css += numBuf;
    css += "; z-index: 1; pointer-events: none; }";

    // 4b. Base64-encode the CSS text to avoid ANY escaping issues
    // CSS is ASCII, so simple byte→base64 works.
    std::string cssB64;
    {
        int len = (int)css.size();
        int padLen = ((len + 2) / 3) * 4;
        char *buf = new char[padLen + 1];
        b64encode((const BYTE*)css.data(), len, buf);
        cssB64 = buf;   // std::string copies up to null, excluding it
        delete[] buf;
    }
    Log(L"CDP: CSS base64 (%d bytes): %hs", (int)cssB64.size(), cssB64.c_str());

    // 5. Build CDP Runtime.evaluate command
    // CSS is passed as base64 → atob() in JS, no escaping needed
    std::string cdpCmd;
    cdpCmd += "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{";
    cdpCmd += "\"expression\":\"";
    cdpCmd += "(function(){";
    cdpCmd += "var s=document.getElementById('matlab_bg_custom_css');";
    cdpCmd += "if(s)s.remove();";
    cdpCmd += "s=document.createElement('style');";
    cdpCmd += "s.id='matlab_bg_custom_css';";
    cdpCmd += "s.textContent=atob('";
    cdpCmd += cssB64;
    cdpCmd += "');";
    cdpCmd += "document.head.appendChild(s);";
    cdpCmd += "})()\"";
    cdpCmd += "}}";

    // Hex dump first 80 bytes for debugging JSON parse errors
    {
        char hexBuf[256] = {};
        int dumpLen = cdpCmd.size() < 80 ? (int)cdpCmd.size() : 80;
        for (int i = 0; i < dumpLen; i++) {
            char t[4];
            sprintf(t, "%02X ", (unsigned char)cdpCmd[i]);
            strcat(hexBuf, t);
        }
        Log(L"CDP: cmd hex[0-%d]: %hs", dumpLen - 1, hexBuf);
    }

    Log(L"CDP: injecting CSS for target '%s' (selector='%hs')", g_cfg.cdpTarget, cssSelector);

    // 6. Send CDP command
    if (!CdpWsSend(ws, cdpCmd.c_str())) {
        Log(L"CDP: send failed");
        closesocket(ws); WSACleanup();
        return false;
    }

    CdpWsRecv(ws);

    // 7. Done – the <style> element lives in the page DOM until page reload
    closesocket(ws);
    WSACleanup();

    Log(L"CDP: CSS injected successfully (%d bytes)", (int)cdpCmd.size());
    return true;
}

// ─── CDP Timer Callback ────────────────────────────────────────────────

static void CALLBACK CdpTimerProc(HWND hwnd, UINT, UINT_PTR, DWORD)
{
    CdpInjectCSS();
}

// ─── Main ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    InitLog();
    Log(L"=== MATLAB BG Patcher v2.0 ===");

    GdiplusStartupInput gdiStartup;
    GdiplusStartup(&g_gdiToken, &gdiStartup, nullptr);

    LoadConfig();

    if (g_cfg.imagePath[0] == 0) {
        MessageBoxW(nullptr,
            L"MATLAB Background Image Patcher v2.0\n\n"
            L"No background image configured.\n\n"
            L"Create matlab_bg.ini alongside matlab_bg.exe:\n"
            L"  [Background]\n"
            L"  Image=C:\\path\\to\\bg.png\n"
            L"  Scope=2       ; 1=Command Window, 2=Full window, 3=CDP CSS\n"
            L"  Opacity=30    ; 0-100\n"
            L"  Dimming=30    ; 0-100 black overlay\n"
            L"  ScaleMode=1   ; 0=Fit 1=Fill 2=Stretch 3=Center 4=Tile\n"
            L"  Speed=150     ; refresh interval ms\n"
            L"  CdpPort=0     ; 0=auto-detect 9222-9229\n"
            L"  CdpTarget=#commandWindowWrapper\n",
            L"MATLAB BG Patcher - No Image", MB_ICONINFORMATION);
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    // ── Scope=3: CDP CSS Injection ──────────────────────────────────
    if (g_cfg.scope == 3) {
        Log(L"Entering Scope=3 CDP mode");
        // Inject CSS immediately
        bool ok = CdpInjectCSS();
        Log(L"CDP: first injection %s", ok ? L"succeeded" : L"FAILED");

        // Create a hidden helper window for the re-injection timer.
        // We need a window handle for SetTimer + GetMessage message pump.
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MATLAB_BG_CdpHelper";
        if (!GetClassInfoW(wc.hInstance, wc.lpszClassName, &wc))
            RegisterClassW(&wc);

        HWND hwndHelper = CreateWindowExW(
            0, L"MATLAB_BG_CdpHelper", L"", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        // Re-inject every 5 seconds in case MATLAB restarts
        SetTimer(hwndHelper, g_timerId, 5000, CdpTimerProc);

        Log(L"CDP mode active: re-injecting CSS every 5s");

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        KillTimer(hwndHelper, g_timerId);
        DestroyWindow(hwndHelper);
        GdiplusShutdown(g_gdiToken);

        Log(L"=== MATLAB BG Patcher exiting ===");
        return 0;
    }

    // ── Scope=1 / Scope=2: Win32 overlay ────────────────────────────
    if (!CreateOverlay()) {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"Error", MB_ICONERROR);
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    // Try to find MATLAB
    g_retryCount = 0;
    while (!FindMATLABTarget() && g_retryCount < 60) {
        Sleep(500);
        g_retryCount++;
    }
    if (!g_hwndMATLAB) {
        Log(L"MATLAB not found after %d retries. Will keep polling.", g_retryCount);
    }

    // WinEvent hook
    if (g_hwndMATLAB) {
        DWORD tid = GetWindowThreadProcessId(g_hwndMATLAB, nullptr);
        g_hWinEventHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, tid, 0, WINEVENT_OUTOFCONTEXT);
        Log(L"WinEvent hook installed for thread %lu", tid);
    }

    // Timer
    SetTimer(nullptr, g_timerId, g_cfg.speed, TimerProc);

    if (g_hwndMATLAB) UpdateOverlay();

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hWinEventHook) UnhookWinEvent(g_hWinEventHook);
    KillTimer(nullptr, g_timerId);
    GdiplusShutdown(g_gdiToken);

    Log(L"=== MATLAB BG Patcher exiting ===");
    return 0;
}