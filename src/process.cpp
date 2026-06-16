/*
 * MATLAB Background Image Patcher — Process detection and window finding
 */
#include "common.h"

bool IsMATLABProcess(HWND hwnd, WCHAR *outExeName, size_t outLen)
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

// ─── Find Target Window ──────────────────────────────────────────────

struct EnumData {
    HWND   bestMain;
    HWND   bestCmdWnd;
    int    bestMainArea;
    int    bestCmdArea;
    bool   firstPass;
};

struct ChildCtx {
    EnumData *data;
};

static void PickMainWindow(HWND hwnd, EnumData *data, const WCHAR * /*exeName*/)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    int area = (r.right - r.left) * (r.bottom - r.top);
    if (area > data->bestMainArea) {
        data->bestMain     = hwnd;
        data->bestMainArea = area;
    }
}

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

    EnumChildWindows(child, EnumChildProc, lp);
    return TRUE;
}

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

bool FindMATLABTarget()
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
        FindCommandWindowChild(data.bestMain, &data);

        if (data.bestCmdWnd && IsWindow(data.bestCmdWnd)) {
            g_hwndTarget = data.bestCmdWnd;
            GetClassNameW(g_hwndTarget, g_targetClass, 256);

            RECT r; GetWindowRect(g_hwndTarget, &r);
            Log(L"Scope=1: Found Command Window: hwnd=0x%p class='%s' area=%d rect=(%d,%d,%d,%d)",
                (void*)g_hwndTarget, g_targetClass, data.bestCmdArea,
                r.left, r.top, r.right, r.bottom);
        } else {
            Log(L"Scope=1: No Command Window child found (Chrome_RenderWidgetHostHWND / SunAwtCanvas).");
            Log(L"         Falling back to entire MATLAB window.");
            g_hwndTarget = data.bestMain;
            GetClassNameW(g_hwndTarget, g_targetClass, 256);
        }
    } else {
        g_hwndTarget = data.bestMain;
        GetClassNameW(g_hwndTarget, g_targetClass, 256);
    }

    RECT r; GetWindowRect(g_hwndTarget, &r);
    Log(L"Target: hwnd=0x%p class='%s' title='%s' rect=(%d,%d,%d,%d) scope=%d",
        (void*)g_hwndTarget, g_targetClass, title,
        r.left, r.top, r.right, r.bottom, g_cfg.scope);

    return true;
}
