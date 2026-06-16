/*
 * MATLAB Background Image Patcher — Overlay window and painting
 */
#include "common.h"

using namespace Gdiplus;

// ─── Image Loading (GDI+) ─────────────────────────────────────────────

Bitmap* LoadBgImage(const WCHAR *path)
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

bool CreateOverlay()
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

void RepaintOverlay(HDC hdcScreen)
{
    static Bitmap *s_lastBmp = nullptr;
    static WCHAR   s_lastPath[MAX_PATH] = {};

    if (wcscmp(g_cfg.imagePath, s_lastPath) != 0) {
        if (s_lastBmp) { delete s_lastBmp; s_lastBmp = nullptr; }
        s_lastBmp = LoadBgImage(g_cfg.imagePath);
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
    /* PLACEHOLDER_REPAINT_CONT */

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

void UpdateOverlay()
{
    if (!g_hwndMATLAB || !g_hwndTarget || !g_hwndOverlay) return;

    if (!IsWindow(g_hwndMATLAB)) {
        Log(L"MATLAB window destroyed, exiting");
        DestroyWindow(g_hwndOverlay);
        return;
    }

    if (g_cfg.scope == 1) {
        if (!IsWindow(g_hwndTarget)) {
            Log(L"Target child window (Command Window) destroyed. Re-scanning...");
            FindMATLABTarget();
            if (!g_hwndTarget) return;
        }
    }

    RECT r;
    GetWindowRect(g_hwndTarget, &r);
    int x = r.left;
    int y = r.top;
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    BYTE alpha = (BYTE)(g_cfg.opacity * 255 / 100);
    SetLayeredWindowAttributes(g_hwndOverlay, 0, alpha, LWA_ALPHA);

    SetWindowPos(g_hwndOverlay, nullptr, x, y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetWindowLongPtrW(g_hwndOverlay, GWLP_HWNDPARENT, (LONG_PTR)g_hwndMATLAB);

    InvalidateRect(g_hwndOverlay, nullptr, FALSE);
}
