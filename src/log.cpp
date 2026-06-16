/*
 * MATLAB Background Image Patcher — Logging
 */
#include "common.h"

void InitLog()
{
    GetModuleFileNameW(nullptr, g_logPath, MAX_PATH);
    WCHAR *slash = wcsrchr(g_logPath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat(g_logPath, L"matlab_bg.log");
    HANDLE hFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
}

void Log(const WCHAR *fmt, ...)
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

    if (g_cfg.debugLog) {
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
