/*
 * MATLAB Background Image Patcher v2.0 — Entry point
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
 *   g++ -O2 -static -mwindows -municode src/*.cpp -o matlab_bg.exe
 *     -luser32 -lgdi32 -lgdiplus -lole32 -lcomctl32 -ldwmapi -lws2_32
 *
 * Usage:
 *   matlab_bg.exe [image_path] [opacity] [dimming] [scale_mode] [speed] [scope] [cdp_port] [cdp_target]
 */
#include "common.h"

// ─── WinEvent Hook + Timer ────────────────────────────────────────────

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event,
                                  HWND hwnd, LONG idObject, LONG idChild,
                                  DWORD, DWORD)
{
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
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
        if (g_cfg.scope == 1 && (!g_hwndTarget || !IsWindow(g_hwndTarget))) {
            FindMATLABTarget();
        }
        UpdateOverlay();
    }
}

// ─── Main ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    InitLog();
    Log(L"=== MATLAB BG Patcher v2.0 ===");
    InitializeCriticalSection(&g_httpLock);

    Gdiplus::GdiplusStartupInput gdiStartup;
    Gdiplus::GdiplusStartup(&g_gdiToken, &gdiStartup, nullptr);

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
        Gdiplus::GdiplusShutdown(g_gdiToken);
        return 1;
    }

    // ── Scope=3: Auto-launch MATLAB if needed ──────────────────────
    if (g_cfg.scope == 3) {
        HANDLE hMatlabProc = nullptr;

        auto IsPortOpen = [](int pt) -> bool {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) { WSACleanup(); return false; }
            int to = 500;
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof(to));
            SOCKADDR_IN addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons((u_short)pt);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            int ok = connect(s, (SOCKADDR*)&addr, sizeof(addr));
            closesocket(s);
            WSACleanup();
            return ok == 0;
        };

        int checkPort = (g_cfg.cdpPort > 0) ? g_cfg.cdpPort : 9222;
        bool matlabRunning = IsPortOpen(checkPort);
        /* PLACEHOLDER_MAIN_SCOPE3 */

        if (!matlabRunning) {
            Log(L"MATLAB not running, searching for matlab.exe...");

            WCHAR matlabPath[MAX_PATH] = {};
            {
                WCHAR searchDir[MAX_PATH];
                GetModuleFileNameW(nullptr, searchDir, MAX_PATH);
                WCHAR *lastSlash = wcsrchr(searchDir, L'\\');
                if (lastSlash) *lastSlash = L'\0';

                for (int level = 0; level < 5 && !matlabPath[0]; level++) {
                    WCHAR test[MAX_PATH];
                    wsprintfW(test, L"%s\\matlab.exe", searchDir);
                    if (GetFileAttributesW(test) != INVALID_FILE_ATTRIBUTES)
                        wcscpy(matlabPath, test);
                    else {
                        wsprintfW(test, L"%s\\bin\\matlab.exe", searchDir);
                        if (GetFileAttributesW(test) != INVALID_FILE_ATTRIBUTES)
                            wcscpy(matlabPath, test);
                    }
                    if (matlabPath[0]) break;
                    WCHAR *ps = wcsrchr(searchDir, L'\\');
                    if (!ps) break;
                    *ps = L'\0';
                }
            }

            if (matlabPath[0]) {
                Log(L"Found: %s", matlabPath);

                int dbgPort = (g_cfg.cdpPort > 0) ? g_cfg.cdpPort : 9222;

                std::wstring envBlock;
                {
                    LPCWCH orig = GetEnvironmentStringsW();
                    LPCWCH p = orig;
                    while (*p) {
                        size_t len = wcslen(p);
                        if (wcsncmp(p, L"MW_CEF_STARTUP_OPTIONS=", 24) != 0) {
                            envBlock.append(p, len);
                            envBlock += L'\0';
                        }
                        p += len + 1;
                    }
                    FreeEnvironmentStringsW((LPWCH)orig);
                    WCHAR envOpt[128];
                    wsprintfW(envOpt, L"MW_CEF_STARTUP_OPTIONS=--remote-debugging-port=%d", dbgPort);
                    envBlock += envOpt;
                    envBlock += L'\0';
                    envBlock += L'\0';
                }

                WCHAR cmdLine[MAX_PATH + 128];
                wsprintfW(cmdLine, L"\"%s\" --remote-debugging-port=%d", matlabPath, dbgPort);

                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_SHOW;

                Log(L"Launching MATLAB (with CDP env var)...");
                if (CreateProcessW(nullptr, cmdLine,
                        nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT,
                        (LPVOID)envBlock.c_str(),
                        nullptr, &si, &pi)) {
                    hMatlabProc = pi.hProcess;
                    CloseHandle(pi.hThread);
                    for (int i = 0; i < 180 && !matlabRunning; i++) {
                        Sleep(500);
                        matlabRunning = IsPortOpen(checkPort);
                    }
                    if (matlabRunning)
                        Log(L"MATLAB CDP port %d is ready", checkPort);
                    else
                        Log(L"CDP port never opened (env var may not have worked)");
                } else {
                    Log(L"Failed to launch MATLAB: %d", GetLastError());
                }
            } else {
                Log(L"matlab.exe not found. Start MATLAB manually.");
            }
        } else {
            Log(L"MATLAB already running (port %d)", checkPort);
        }

        Log(L"Entering Scope=3 CDP mode");
        bool ok = CdpInjectCSS();
        Log(L"CDP: first injection %s", ok ? L"succeeded" : L"FAILED");

        WNDCLASSW wc = {};
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MATLAB_BG_CdpHelper";
        if (!GetClassInfoW(wc.hInstance, wc.lpszClassName, &wc))
            RegisterClassW(&wc);

        HWND hwndHelper = CreateWindowExW(
            0, L"MATLAB_BG_CdpHelper", L"", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        SetWindowLongPtrW(hwndHelper, GWLP_USERDATA, (LONG_PTR)hMatlabProc);

        SetTimer(hwndHelper, g_timerId, 5000,
            [](HWND hwnd, UINT, UINT_PTR, DWORD) {
                HANDLE hProc = (HANDLE)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (hProc && WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0) {
                    Log(L"MATLAB process exited, shutting down");
                    PostQuitMessage(0);
                    return;
                }
                CdpInjectCSS();
            });

        Log(L"CDP mode active: re-injecting CSS every 5s");

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        KillTimer(hwndHelper, g_timerId);
        DestroyWindow(hwndHelper);
        if (hMatlabProc) CloseHandle(hMatlabProc);
        if (g_httpRunning) {
            g_httpRunning = false;
            WaitForSingleObject(g_hHttpThread, 3000);
        }
        DeleteCriticalSection(&g_httpLock);
        Gdiplus::GdiplusShutdown(g_gdiToken);

        Log(L"=== MATLAB BG Patcher exiting ===");
        return 0;
    }

    // ── Scope=1 / Scope=2: Win32 overlay ────────────────────────────
    if (!CreateOverlay()) {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"Error", MB_ICONERROR);
        Gdiplus::GdiplusShutdown(g_gdiToken);
        return 1;
    }

    g_retryCount = 0;
    while (!FindMATLABTarget() && g_retryCount < 60) {
        Sleep(500);
        g_retryCount++;
    }
    if (!g_hwndMATLAB) {
        Log(L"MATLAB not found after %d retries. Will keep polling.", g_retryCount);
    }

    if (g_hwndMATLAB) {
        DWORD tid = GetWindowThreadProcessId(g_hwndMATLAB, nullptr);
        g_hWinEventHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, tid, 0, WINEVENT_OUTOFCONTEXT);
        Log(L"WinEvent hook installed for thread %lu", tid);
    }

    SetTimer(nullptr, g_timerId, g_cfg.speed, TimerProc);

    if (g_hwndMATLAB) UpdateOverlay();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hWinEventHook) UnhookWinEvent(g_hWinEventHook);
    KillTimer(nullptr, g_timerId);
    if (g_httpRunning) {
        g_httpRunning = false;
        WaitForSingleObject(g_hHttpThread, 3000);
    }
    DeleteCriticalSection(&g_httpLock);
    Gdiplus::GdiplusShutdown(g_gdiToken);

    Log(L"=== MATLAB BG Patcher exiting ===");
    return 0;
}
