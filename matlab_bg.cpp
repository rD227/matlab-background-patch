/*
 * MATLAB Background Image Patcher — Pure Win32 overlay (no Qt dependency)
 *
 * Architecture (adapted from octave-bg-patcher's ScopeWindow):
 *   1. Find MATLAB's main window (SunAwtFrame class, title "MATLAB R2025a")
 *   2. Create a WS_EX_LAYERED | WS_EX_TRANSPARENT tool window
 *   3. Bind overlay to MATLAB via GWLP_HWNDPARENT (owner relationship)
 *   4. Use UpdateLayeredWindow() to draw image with per-pixel alpha
 *   5. Track MATLAB window position/size via EVENT_OBJECT_LOCATIONCHANGE
 *
 * Build (MinGW-w64):
 *   g++ -O2 -static -mwindows -o matlab_bg.exe matlab_bg.cpp -luser32 -lgdi32 -lmsimg32 -lole32 -lcomctl32
 *
 * Usage:
 *   matlab_bg.exe [image_path] [opacity] [dimming] [scale_mode]
 *     image_path: path to PNG/JPG/BMP/GIF (required)
 *     opacity: 0-100 (default: 30)
 *     dimming: 0-100 (default: 30)
 *     scale_mode: 0=fit, 1=fill, 2=stretch, 3=center, 4=tile (default: 1)
 *
 *   Or put settings in matlab_bg.ini alongside the exe:
 *     [Background]
 *     Image=C:\path\to\bg.png
 *     Opacity=30
 *     Dimming=30
 *     ScaleMode=1
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// ─── Configuration ───────────────────────────────────────────────────

struct Config {
    WCHAR imagePath[MAX_PATH] = {};
    int   opacity   = 30;   // 0–100  (image alpha)
    int   dimming   = 30;   // 0–100  (black overlay alpha)
    int   scaleMode = 1;    // 0=fit, 1=fill, 2=stretch, 3=center, 4=tile
};

// ─── Globals ──────────────────────────────────────────────────────────

static Config        g_cfg;
static HWND           g_hwndOverlay = nullptr;
static HWND           g_hwndMATLAB  = nullptr;
static HWINEVENTHOOK  g_hWinEventHook = nullptr;
static UINT_PTR       g_timerId     = 1;
static ULONG_PTR      g_gdiToken    = 0;
static int            g_retryCount  = 0;

// ─── Forward declarations ─────────────────────────────────────────────

static void          Log(const WCHAR *fmt, ...);
static void          InitLog();
static void          LoadConfig();
static bool          FindMATLABWindow();
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
    // Log to same directory as exe
    GetModuleFileNameW(nullptr, g_logPath, MAX_PATH);
    WCHAR *slash = wcsrchr(g_logPath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat(g_logPath, L"matlab_bg.log");

    // Clear log file
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
            // Write timestamp as UTF-8 for readability
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

    // Defaults
    g_cfg = Config{};
    g_cfg.opacity = 30;
    g_cfg.dimming = 30;
    g_cfg.scaleMode = 1;

    // Read INI
    GetPrivateProfileStringW(L"Background", L"Image", L"", g_cfg.imagePath, MAX_PATH, iniPath.c_str());
    g_cfg.opacity   = GetPrivateProfileIntW(L"Background", L"Opacity",   30, iniPath.c_str());
    g_cfg.dimming   = GetPrivateProfileIntW(L"Background", L"Dimming",   30, iniPath.c_str());
    g_cfg.scaleMode = GetPrivateProfileIntW(L"Background", L"ScaleMode", 1,  iniPath.c_str());

    // Clamp
    if (g_cfg.opacity < 0)   g_cfg.opacity = 0;
    if (g_cfg.opacity > 100) g_cfg.opacity = 100;
    if (g_cfg.dimming < 0)   g_cfg.dimming = 0;
    if (g_cfg.dimming > 100) g_cfg.dimming = 100;
    if (g_cfg.scaleMode < 0 || g_cfg.scaleMode > 4) g_cfg.scaleMode = 1;

    // Command-line override
    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2) wcscpy(g_cfg.imagePath, argv[1]);
        if (argc >= 3) g_cfg.opacity   = _wtoi(argv[2]);
        if (argc >= 4) g_cfg.dimming   = _wtoi(argv[3]);
        if (argc >= 5) g_cfg.scaleMode = _wtoi(argv[4]);
        LocalFree(argv);
    }

    Log(L"Config: image='%s' opacity=%d dimming=%d scaleMode=%d",
        g_cfg.imagePath, g_cfg.opacity, g_cfg.dimming, g_cfg.scaleMode);
}

// ─── Find MATLAB Main Window ──────────────────────────────────────────

// MATLAB R2025a desktop is a CEF-based window (Chrome_WidgetWin_* class).
// R2023b and earlier used Java Swing (SunAwtFrame class).
// We match by process (MATLAB.exe / MATLABWindow.exe) and fall back to
// known class-name patterns with permissive title matching.

static std::vector<DWORD> FindMATLABProcessIDs()
{
    std::vector<DWORD> pids;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"MATLAB.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"MATLABWindow.exe") == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pids;
}

// Returns true if the window's process EXE name contains "matlab" (case-insensitive)
static bool IsMATLABProcess(HWND hwnd, WCHAR *outExeName = nullptr, size_t outLen = 0)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return false;

    // Skip our own process (matlab_bg.exe also contains "matlab")
    if (pid == GetCurrentProcessId()) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    WCHAR exePath[MAX_PATH] = {};
    DWORD dwSize = MAX_PATH;
    bool found = QueryFullProcessImageNameW(hProc, 0, exePath, &dwSize);
    CloseHandle(hProc);
    if (!found) return false;

    // Extract just the filename from the path
    WCHAR *fname = wcsrchr(exePath, L'\\');
    if (fname) fname++; else fname = exePath;

    if (outExeName && outLen > 0) {
        wcsncpy(outExeName, fname, outLen);
        outExeName[outLen - 1] = L'\0';
    }

    return (wcsstr(fname, L"MATLAB") != nullptr || wcsstr(fname, L"matlab") != nullptr);
}

struct EnumMATLABData {
    HWND   bestWindow;
    DWORD  bestPid;
    int    bestArea;
    bool   firstPass;  // true = diagnostic log everything
};

static BOOL CALLBACK EnumMATLABProc(HWND hwnd, LPARAM lParam)
{
    EnumMATLABData *data = (EnumMATLABData*)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;

    WCHAR cls[256] = {};
    WCHAR title[256] = {};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 256);

    // Skip child windows and tool windows owned by a parent
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;

    // Strategy: find any window whose process contains "MATLAB" in its EXE name.
    // This is version-independent — works for CEF, Java Swing, future Qt, etc.
    WCHAR exeName[64] = {};
    if (!IsMATLABProcess(hwnd, exeName, 64)) return TRUE;

    // Diagnostic: log every candidate in the first scan
    if (data->firstPass) {
        RECT r;
        GetWindowRect(hwnd, &r);
        Log(L"  [scan] hwnd=0x%p class='%s' title='%s' exe=%s rect=(%d,%d,%d,%d)",
            (void*)hwnd, cls, title, exeName, r.left, r.top, r.right, r.bottom);
    }

    // Pick the largest visible window from a MATLAB process
    RECT r;
    GetWindowRect(hwnd, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);

    if (area > data->bestArea) {
        data->bestWindow = hwnd;
        data->bestArea   = area;
        GetWindowThreadProcessId(hwnd, &data->bestPid);
    }

    return TRUE;
}

static bool FindMATLABWindow()
{
    g_hwndMATLAB = nullptr;

    // Check if we have any MATLAB process running at all
    std::vector<DWORD> pids = FindMATLABProcessIDs();
    if (pids.empty()) {
        Log(L"No MATLAB process found (checked MATLAB.exe, MATLABWindow.exe).");
        Log(L"Is MATLAB running? Start MATLAB first, then run matlab_bg.exe.");
        return false;
    }

    WCHAR pidStr[256] = {};
    for (DWORD p : pids) {
        WCHAR tmp[32];
        wsprintfW(tmp, L"%lu ", p);
        wcscat(pidStr, tmp);
    }
    Log(L"MATLAB PIDs found: %s— scanning windows...", pidStr);

    EnumMATLABData data = { nullptr, 0, 0, true };
    EnumWindows(EnumMATLABProc, (LPARAM)&data);

    if (data.bestWindow && IsWindow(data.bestWindow)) {
        g_hwndMATLAB = data.bestWindow;
        WCHAR cls[256], title[256];
        GetClassNameW(g_hwndMATLAB, cls, 256);
        GetWindowTextW(g_hwndMATLAB, title, 256);
        RECT r;
        GetWindowRect(g_hwndMATLAB, &r);
        Log(L"Selected MATLAB window: hwnd=0x%p class='%s' title='%s' pid=%lu rect=(%d,%d,%d,%d) area=%d",
            (void*)g_hwndMATLAB, cls, title, data.bestPid,
            r.left, r.top, r.right, r.bottom, data.bestArea);
        return true;
    }

    Log(L"MATLAB process(es) exist but no usable top-level window found. Dumping ALL visible top-level windows:");
    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
        WCHAR cls[256], title[256];
        GetClassNameW(hwnd, cls, 256);
        GetWindowTextW(hwnd, title, 256);
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        RECT r;
        GetWindowRect(hwnd, &r);
        Log(L"  hwnd=0x%p class='%s' title='%s' pid=%lu rect=(%d,%d,%d,%d)",
            (void*)hwnd, cls, title, pid, r.left, r.top, r.right, r.bottom);
        return TRUE;
    }, 0);
    return false;
}

// ─── Image Loading (GDI+) ─────────────────────────────────────────────

static Bitmap* LoadImage(const WCHAR *path)
{
    if (path[0] == 0) return nullptr;

    Bitmap *bmp = nullptr;
    // Try GDI+
    bmp = Bitmap::FromFile(path);
    if (bmp && bmp->GetLastStatus() == Ok) {
        Log(L"Image loaded: %s (%d x %d)", path, (int)bmp->GetWidth(), (int)bmp->GetHeight());
        return bmp;
    }
    if (bmp) {
        Log(L"GDI+ failed to load image: %s (status=%d)", path, (int)bmp->GetLastStatus());
        delete bmp;
    }
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
    case WM_ERASEBKGND:
        return 1; // skip background erase

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool CreateOverlay()
{
    // Register a minimal window class for the overlay
    static const WCHAR *OVERLAY_CLASS = L"MATLAB_BG_Overlay_Class";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = OVERLAY_CLASS;
    if (!GetClassInfoW(wc.hInstance, OVERLAY_CLASS, &wc))
        RegisterClassW(&wc);

    // Create the overlay as a frameless layered tool window
    g_hwndOverlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS,
        L"MATLAB BG Overlay",
        WS_POPUP,
        0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_hwndOverlay) {
        Log(L"CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    // Set initial opacity via LWA_ALPHA (avoids UpdateLayeredWindow which
    // cannot stack above hardware-accelerated CEF content).
    BYTE alpha = (BYTE)(g_cfg.opacity * 255 / 100);
    SetLayeredWindowAttributes(g_hwndOverlay, 0, alpha, LWA_ALPHA);

    // Show the window
    ShowWindow(g_hwndOverlay, SW_SHOWNOACTIVATE);

    Log(L"Overlay created: hwnd=0x%p", (void*)g_hwndOverlay);
    return true;
}

static void RepaintOverlay(HDC hdcScreen)
{
    static Bitmap *s_lastBmp = nullptr;
    static WCHAR   s_lastPath[MAX_PATH] = {};

    // Reload image if path changed
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

    // Create a 32-bit alpha bitmap
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp) { DeleteDC(hdcMem); return; }
    HBITMAP hBmpOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    // Draw onto the GDI+ context backed by our bitmap bits
    Graphics g(hdcMem);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.Clear(Color(0, 0, 0, 0)); // fully transparent black

    int imgW = (int)bmp->GetWidth();
    int imgH = (int)bmp->GetHeight();
    if (imgW <= 0 || imgH <= 0) {
        SelectObject(hdcMem, hBmpOld);
        DeleteObject(hBmp);
        DeleteDC(hdcMem);
        return;
    }

    // Scale image according to mode
    Bitmap scaled(w, h, PixelFormat32bppARGB);
    Graphics gScaled(&scaled);
    gScaled.SetSmoothingMode(SmoothingModeHighQuality);
    gScaled.Clear(Color(0, 0, 0, 0));

    int dx = 0, dy = 0, sw = w, sh = h;

    switch (g_cfg.scaleMode) {
    case 0: // Fit (keep aspect ratio, fit within)
    {
        double ratio = (double)imgW / imgH;
        if (w / (double)h > ratio) { sh = h; sw = (int)(h * ratio); dx = (w - sw) / 2; }
        else                       { sw = w; sh = (int)(w / ratio); dy = (h - sh) / 2; }
        gScaled.DrawImage(bmp, dx, dy, sw, sh);
        break;
    }
    case 1: // Fill (keep aspect ratio, expand to cover — default)
    {
        double ratio = (double)imgW / imgH;
        if (w / (double)h > ratio) { sw = w; sh = (int)(w / ratio); dy = (h - sh) / 2; }
        else                       { sh = h; sw = (int)(h * ratio); dx = (w - sw) / 2; }
        gScaled.DrawImage(bmp, dx, dy, sw, sh);
        break;
    }
    case 2: // Stretch
    {
        gScaled.DrawImage(bmp, 0, 0, w, h);
        break;
    }
    case 3: // Center (no scaling)
    {
        dx = (w - imgW) / 2;
        dy = (h - imgH) / 2;
        gScaled.DrawImage(bmp, dx, dy, imgW, imgH);
        break;
    }
    case 4: // Tile
    {
        TextureBrush brush(bmp, WrapModeTile);
        gScaled.FillRectangle(&brush, 0, 0, w, h);
        break;
    }
    default:
        gScaled.DrawImage(bmp, 0, 0, w, h);
        break;
    }

    // Draw the scaled image onto the main buffer with opacity
    ColorMatrix cm = {};
    cm.m[0][0] = 1.0f; cm.m[1][1] = 1.0f; cm.m[2][2] = 1.0f;
    cm.m[3][3] = g_cfg.opacity / 100.0f; // alpha
    cm.m[4][4] = 1.0f;
    ImageAttributes ia;
    ia.SetColorMatrix(&cm, ColorMatrixFlagsDefault, ColorAdjustTypeBitmap);
    g.DrawImage(&scaled, Rect(0, 0, w, h), 0, 0, w, h, UnitPixel, &ia);

    // Apply dimming (black overlay with alpha)
    if (g_cfg.dimming > 0) {
        int dimAlpha = g_cfg.dimming * 255 / 100;
        SolidBrush dimBrush(Color((BYTE)dimAlpha, 0, 0, 0));
        g.FillRectangle(&dimBrush, 0, 0, w, h);
    }

    // Flush GDI+ before blitting
    g.Flush(FlushIntentionSync);

    // Blit the rendered frame to the target DC (WM_PAINT path)
    // or trigger a repaint via InvalidateRect (timer/event path).
    if (hdcScreen) {
        BitBlt(hdcScreen, 0, 0, w, h, hdcMem, 0, 0, SRCCOPY);
    } else {
        InvalidateRect(g_hwndOverlay, nullptr, FALSE);
    }

    SelectObject(hdcMem, hBmpOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
}

static void UpdateOverlay()
{
    if (!g_hwndMATLAB || !g_hwndOverlay) return;
    if (!IsWindow(g_hwndMATLAB)) {
        Log(L"MATLAB window destroyed, exiting");
        DestroyWindow(g_hwndOverlay);
        return;
    }

    // Reposition overlay to cover MATLAB window
    RECT r;
    GetWindowRect(g_hwndMATLAB, &r);
    int x = r.left;
    int y = r.top;
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    // Update overall window opacity via LWA_ALPHA
    // (avoids UpdateLayeredWindow which can't stack above hardware-accelerated CEF)
    BYTE alpha = (BYTE)(g_cfg.opacity * 255 / 100);
    SetLayeredWindowAttributes(g_hwndOverlay, 0, alpha, LWA_ALPHA);

    // Move and resize the overlay
    SetWindowPos(g_hwndOverlay, nullptr, x, y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Bind the overlay as an OWNED window of the MATLAB main window.
    // Re-bind every time — MATLAB may recreate its window (e.g. theme change).
    SetWindowLongPtrW(g_hwndOverlay, GWLP_HWNDPARENT, (LONG_PTR)g_hwndMATLAB);

    // Trigger WM_PAINT to draw the frame
    InvalidateRect(g_hwndOverlay, nullptr, FALSE);
}

// ─── WinEvent Hook (track MATLAB window position/size) ────────────────

static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event,
                                  HWND hwnd, LONG idObject, LONG idChild,
                                  DWORD dwEventThread, DWORD dwmsEventTime)
{
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (hwnd != g_hwndMATLAB) return;

    switch (event) {
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_VALUECHANGE:
        UpdateOverlay();
        break;

    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_HIDE:
        Log(L"MATLAB window hidden/destroyed");
        break;
    }
}

// ─── Timer callback (fallback if WinEvent hook fails) ─────────────────

static void CALLBACK TimerProc(HWND, UINT, UINT_PTR idEvent, DWORD)
{
    if (!g_hwndMATLAB) {
        FindMATLABWindow();
        if (g_hwndMATLAB) {
            UpdateOverlay();
        }
    } else if (!IsWindow(g_hwndMATLAB)) {
        g_hwndMATLAB = nullptr;
    } else {
        UpdateOverlay();
    }
}

// ─── Main ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    InitLog();
    Log(L"=== MATLAB BG Patcher v1.0 ===");

    // Initialize GDI+
    GdiplusStartupInput gdiStartup;
    GdiplusStartup(&g_gdiToken, &gdiStartup, nullptr);

    // Load configuration
    LoadConfig();

    // Check that an image path is provided
    if (g_cfg.imagePath[0] == 0) {
        MessageBoxW(nullptr,
            L"MATLAB Background Image Patcher\n\n"
            L"No background image configured.\n\n"
            L"Usage:\n"
            L"  matlab_bg.exe C:\\path\\to\\image.png [opacity] [dimming] [scale]\n\n"
            L"Or create matlab_bg.ini alongside the exe:\n"
            L"  [Background]\n"
            L"  Image=C:\\path\\to\\bg.png\n"
            L"  Opacity=30\n"
            L"  Dimming=30\n"
            L"  ScaleMode=1",
            L"MATLAB BG Patcher - No Image", MB_ICONINFORMATION);
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    // Create overlay window (hidden until MATLAB is found)
    if (!CreateOverlay()) {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"Error", MB_ICONERROR);
        GdiplusShutdown(g_gdiToken);
        return 1;
    }

    // Try to find MATLAB immediately
    g_retryCount = 0;
    while (!FindMATLABWindow() && g_retryCount < 60) {
        Sleep(500);
        g_retryCount++;
    }

    if (!g_hwndMATLAB) {
        Log(L"MATLAB window not found after %d retries. Will keep polling.", g_retryCount);
    }

    // Set up WinEvent hook to track MATLAB window position/size changes
    if (g_hwndMATLAB) {
        g_hWinEventHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc,
            GetWindowThreadProcessId(g_hwndMATLAB, nullptr), 0,
            WINEVENT_OUTOFCONTEXT);
        Log(L"WinEvent hook installed for thread %lu",
            GetWindowThreadProcessId(g_hwndMATLAB, nullptr));
    }

    // Fallback timer — catches cases where WinEvent misses
    SetTimer(nullptr, g_timerId, 500, TimerProc);

    // Initial overlay
    if (g_hwndMATLAB) {
        UpdateOverlay();
    }

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    if (g_hWinEventHook) UnhookWinEvent(g_hWinEventHook);
    KillTimer(nullptr, g_timerId);
    GdiplusShutdown(g_gdiToken);

    Log(L"=== MATLAB BG Patcher exiting ===");
    return 0;
}