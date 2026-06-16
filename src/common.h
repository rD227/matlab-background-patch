/*
 * MATLAB Background Image Patcher v2.0 — Common header
 */
#ifndef MATLAB_BG_COMMON_H
#define MATLAB_BG_COMMON_H

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

// ─── Configuration ───────────────────────────────────────────────────

struct Config {
    WCHAR imagePath[MAX_PATH] = {};
    int   scope     = 2;    // 1=Command Window HWND, 2=Entire window, 3=CDP CSS inject
    int   opacity   = 30;   // 0-100
    int   dimming   = 30;   // 0-100
    int   scaleMode = 1;    // 0=fit, 1=fill, 2=stretch, 3=center, 4=tile
    int   speed     = 150;  // timer interval ms (50-2000)
    int   cdpPort   = 0;    // CDP debug port (0=auto-detect 9222-9229)
    WCHAR cdpTarget[128] = L"#commandWindowWrapper";
    int   httpPort  = 9221; // HTTP image server port for Scope=3
    int   debugLog  = 1;    // Write log file? 0=no, 1=yes
};

// ─── Globals ──────────────────────────────────────────────────────────

extern Config        g_cfg;
extern HWND          g_hwndMATLAB;
extern HWND          g_hwndTarget;
extern HWND          g_hwndOverlay;
extern HWINEVENTHOOK g_hWinEventHook;
extern UINT_PTR      g_timerId;
extern ULONG_PTR     g_gdiToken;
extern int           g_retryCount;
extern WCHAR         g_targetClass[256];
extern WCHAR         g_logPath[MAX_PATH];

// ─── Function declarations ────────────────────────────────────────────

// log.cpp
void InitLog();
void Log(const WCHAR *fmt, ...);

// config.cpp
std::wstring GetExeDir();
void LoadConfig();

// process.cpp
bool IsMATLABProcess(HWND hwnd, WCHAR *outExeName, size_t outLen);
bool FindMATLABTarget();

// overlay.cpp
Gdiplus::Bitmap* LoadBgImage(const WCHAR *path);
bool CreateOverlay();
void UpdateOverlay();
void RepaintOverlay(HDC hdcScreen = nullptr);

// cdp.cpp
bool CdpInjectCSS();
int  CdpAutoDetectPort();
bool EnsureHttpServerRunning();

// HTTP server state (defined in cdp.cpp)
extern HANDLE           g_hHttpThread;
extern CRITICAL_SECTION g_httpLock;
extern bool             g_httpRunning;

#endif // MATLAB_BG_COMMON_H
