/*
 * MATLAB Background Image Patcher — Configuration loading
 */
#include "common.h"

std::wstring GetExeDir()
{
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.rfind(L'\\');
    if (pos != std::wstring::npos) s.resize(pos);
    return s;
}

void LoadConfig()
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
    g_cfg.httpPort  = GetPrivateProfileIntW(L"Background", L"HttpPort",  9221, iniPath.c_str());
    g_cfg.debugLog  = GetPrivateProfileIntW(L"Background", L"DebugLog",  1,    iniPath.c_str());
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

    Log(L"Config: scope=%d image='%s' opacity=%d dimming=%d scaleMode=%d speed=%d"
        L" cdpPort=%d cdpTarget='%s' debugLog=%d httpPort=%d",
        g_cfg.scope, g_cfg.imagePath, g_cfg.opacity, g_cfg.dimming, g_cfg.scaleMode, g_cfg.speed,
        g_cfg.cdpPort, g_cfg.cdpTarget, g_cfg.debugLog, g_cfg.httpPort);
}
