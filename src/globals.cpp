/*
 * MATLAB Background Image Patcher — Global variable definitions
 */
#include "common.h"

Config        g_cfg;
HWND          g_hwndMATLAB      = nullptr;
HWND          g_hwndTarget      = nullptr;
HWND          g_hwndOverlay     = nullptr;
HWINEVENTHOOK g_hWinEventHook   = nullptr;
UINT_PTR      g_timerId         = 1;
ULONG_PTR     g_gdiToken        = 0;
int           g_retryCount      = 0;
WCHAR         g_targetClass[256] = {};
WCHAR         g_logPath[MAX_PATH] = {};
