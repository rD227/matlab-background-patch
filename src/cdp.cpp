/*
 * MATLAB Background Image Patcher — CDP (Chrome DevTools Protocol) + HTTP server
 */
#include "common.h"

// ─── HTTP Image Server state ─────────────────────────────────────────
static std::vector<BYTE>  g_httpImgData;
static const char*        g_httpImgMime = "image/jpeg";

HANDLE           g_hHttpThread = nullptr;
CRITICAL_SECTION g_httpLock;
bool             g_httpRunning = false;

// ─── Base64 encoder ──────────────────────────────────────────────────

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

// ─── CDP /json page discovery ────────────────────────────────────────

static bool CdpGetPageUrl(int port, char *hostOut, int *portOut, char *pathOut, int pathCap)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { Log(L"CDP: socket() failed: %d", WSAGetLastError()); return false; }

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

    const char *wsKey = "\"webSocketDebuggerUrl\": \"";
    const char *urlKey = "\"url\": \"";
    const char *idxSig = "index-jsd.html";
    const char *bestWs = nullptr;
    const char *searchPos = buf;

    while ((searchPos = strstr(searchPos, wsKey)) != nullptr) {
        searchPos += strlen(wsKey);

        const char *scan = searchPos;
        const char *prevUrl = nullptr;
        while (scan > buf) {
            scan--;
            if (strncmp(scan, urlKey, strlen(urlKey)) == 0) {
                prevUrl = scan + strlen(urlKey);
                break;
            }
        }

        bool isMain = false;
        if (prevUrl) {
            const char *urlEnd = prevUrl;
            while (*urlEnd && *urlEnd != '"') urlEnd++;
            int urlLen = (int)(urlEnd - prevUrl);
            if (urlLen > 0 && urlLen < 512) {
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

        if (!bestWs) {
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
    if (strncmp(p, "ws://", 5) != 0) { Log(L"CDP: unexpected ws URL"); return false; }
    p += 5;
    const char *hostStart = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hostLen = (int)(p - hostStart);
    if (hostLen > 63) hostLen = 63;
    memcpy(hostOut, hostStart, hostLen); hostOut[hostLen] = '\0';

    *portOut = (*p == ':') ? atoi(p + 1) : port;
    while (*p && *p != '/') p++;
    const char *pathStart = p;
    while (*p && *p != '"') p++;
    int pathLen = (int)(p - pathStart);
    if (pathLen > pathCap - 1) pathLen = pathCap - 1;
    memcpy(pathOut, pathStart, pathLen); pathOut[pathLen] = '\0';

    if (pathLen > 0 && pathOut[pathLen - 1] == '"') pathOut[pathLen - 1] = '\0';

    Log(L"CDP: using page ws://%hs:%d%hs", hostOut, *portOut, pathOut);
    return true;
}

// ─── WebSocket connect + handshake ───────────────────────────────────

static SOCKET CdpWsConnect(const char *host, int port, const char *path)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    SOCKADDR_IN addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    if (connect(s, (SOCKADDR*)&addr, sizeof(addr)) != 0) {
        Log(L"CDP: ws connect() failed: %d", WSAGetLastError());
        closesocket(s); return INVALID_SOCKET;
    }

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

// ─── WebSocket send/recv ─────────────────────────────────────────────

static bool CdpWsSend(SOCKET s, const char *json)
{
    int len = (int)strlen(json);
    BYTE frame[10];
    int hdrLen;

    frame[0] = 0x81;
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

    BYTE mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (BYTE)(rand() & 0xFF);

    std::vector<BYTE> packet(hdrLen + 4 + len);
    memcpy(packet.data(), frame, hdrLen);
    memcpy(packet.data() + hdrLen, mask, 4);
    for (int i = 0; i < len; i++)
        packet[hdrLen + 4 + i] = (BYTE)json[i] ^ mask[i % 4];

    return send(s, (char*)packet.data(), (int)packet.size(), 0) == (int)packet.size();
}

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

// ─── Auto-detect CDP port ────────────────────────────────────────────

int CdpAutoDetectPort()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (int port = 9222; port <= 9229; port++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
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
/* PLACEHOLDER_CDP_HTTP */

// ─── HTTP Image Server ───────────────────────────────────────────────

static DWORD WINAPI HttpServerThread(LPVOID)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { WSACleanup(); return 1; }

    int reuse = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    SOCKADDR_IN addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((u_short)g_cfg.httpPort);

    if (bind(ls, (SOCKADDR*)&addr, sizeof(addr)) != 0) {
        Log(L"HTTP: bind port %d failed: %d", g_cfg.httpPort, WSAGetLastError());
        closesocket(ls); WSACleanup(); return 1;
    }
    listen(ls, 3);
    Log(L"HTTP: image server started on 127.0.0.1:%d", g_cfg.httpPort);

    while (g_httpRunning) {
        int timeout = 1000;
        setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        SOCKET client = accept(ls, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!g_httpRunning) break;
            continue;
        }

        char req[512] = {};
        recv(client, req, sizeof(req) - 1, 0);

        EnterCriticalSection(&g_httpLock);
        std::vector<BYTE> data = g_httpImgData;
        const char *mime = g_httpImgMime;
        LeaveCriticalSection(&g_httpLock);

        if (!data.empty()) {
            char hdr[512];
            int hdrLen = _snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %d\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Cache-Control: max-age=3600\r\n"
                "Connection: close\r\n"
                "\r\n",
                mime, (int)data.size());
            send(client, hdr, hdrLen, 0);
            send(client, (const char*)data.data(), (int)data.size(), 0);
        } else {
            const char *nf = "HTTP/1.1 404 Not Found\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: close\r\n\r\n";
            send(client, nf, (int)strlen(nf), 0);
        }
        closesocket(client);
    }

    closesocket(ls);
    WSACleanup();
    Log(L"HTTP: server stopped");
    return 0;
}

bool EnsureHttpServerRunning()
{
    static WCHAR s_lastPath[MAX_PATH] = {};

    if (wcscmp(g_cfg.imagePath, s_lastPath) != 0) {
        wcscpy(s_lastPath, g_cfg.imagePath);

        HANDLE hFile = CreateFileW(g_cfg.imagePath, GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            Log(L"HTTP: cannot open image: %s (err=%d)", g_cfg.imagePath, GetLastError());
            return false;
        }
        DWORD fileSize = GetFileSize(hFile, nullptr);
        if (fileSize == 0 || fileSize > 10 * 1024 * 1024) {
            Log(L"HTTP: bad image size: %d", fileSize);
            CloseHandle(hFile); return false;
        }

        EnterCriticalSection(&g_httpLock);
        g_httpImgData.resize(fileSize);
        DWORD bytesRead = 0;
        ReadFile(hFile, g_httpImgData.data(), fileSize, &bytesRead, nullptr);
        CloseHandle(hFile);

        if (bytesRead != fileSize) {
            LeaveCriticalSection(&g_httpLock);
            Log(L"HTTP: short read on image file");
            return false;
        }

        const BYTE *d = g_httpImgData.data();
        if (fileSize >= 3 && d[0] == 0xFF && d[1] == 0xD8)
            g_httpImgMime = "image/jpeg";
        else if (fileSize >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
            g_httpImgMime = "image/png";
        else if (fileSize >= 2 && d[0] == 'B' && d[1] == 'M')
            g_httpImgMime = "image/bmp";
        else if (fileSize >= 6 && (memcmp(d, "GIF89a", 6) == 0 || memcmp(d, "GIF87a", 6) == 0))
            g_httpImgMime = "image/gif";
        LeaveCriticalSection(&g_httpLock);

        Log(L"HTTP: loaded image %hs (%d bytes)", g_httpImgMime, fileSize);
    }

    if (!g_httpRunning) {
        g_httpRunning = true;
        g_hHttpThread = CreateThread(nullptr, 0, HttpServerThread, nullptr, 0, nullptr);
        if (!g_hHttpThread) {
            g_httpRunning = false;
            Log(L"HTTP: failed to create server thread");
            return false;
        }
        Sleep(100);
    }
    return true;
}
/* PLACEHOLDER_CDP_INJECT */

// ─── CDP CSS Injection ───────────────────────────────────────────────

bool CdpInjectCSS()
{
    int port = g_cfg.cdpPort;
    if (port == 0) port = CdpAutoDetectPort();
    if (port == 0) {
        Log(L"CDP: no debug port found. Enable CEF debugging or set CdpPort in ini.");
        return false;
    }

    Log(L"CDP: CdpInjectCSS starting, port=%d", port);

    if (!EnsureHttpServerRunning()) {
        Log(L"CDP: HTTP image server not available");
        return false;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    char host[64] = "127.0.0.1";
    int  wsPort   = 0;
    char path[256] = {};
    if (!CdpGetPageUrl(port, host, &wsPort, path, sizeof(path))) {
        WSACleanup(); return false;
    }

    SOCKET ws = CdpWsConnect(host, wsPort, path);
    if (ws == INVALID_SOCKET) { WSACleanup(); return false; }

    char cssSelector[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_cfg.cdpTarget, -1, cssSelector, sizeof(cssSelector), nullptr, nullptr);

    char imgUrl[128];
    _snprintf(imgUrl, sizeof(imgUrl), "http://127.0.0.1:%d/bg", g_cfg.httpPort);

    const char *objFit;
    switch (g_cfg.scaleMode) {
        case 2: objFit = "fill";      break;
        case 0: objFit = "contain";   break;
        case 3: objFit = "none";      break;
        default:objFit = "cover";     break;
    }

    char opacityStr[16], dimmingStr[16];
    _snprintf(opacityStr, sizeof(opacityStr), "%.2f", g_cfg.opacity / 100.0);
    _snprintf(dimmingStr, sizeof(dimmingStr), "%.2f", g_cfg.dimming / 100.0);

    std::string jsLayer;
    jsLayer += "(function(){";
    jsLayer += "var els=document.querySelectorAll('"; jsLayer += cssSelector; jsLayer += "');";
    jsLayer += "for(var i=0;i<els.length;i++){";
    jsLayer += "var w=els[i];";
    jsLayer += "if(getComputedStyle(w).position==='static')w.style.position='relative';";
    jsLayer += "if(!w.querySelector('.matlab-bg-dim')){";
    jsLayer += "var dim=document.createElement('div');dim.className='matlab-bg-dim';";
    jsLayer += "dim.style.cssText='position:absolute;top:0;left:0;width:100%;height:100%;"
               "background:black;opacity:"; jsLayer += dimmingStr; jsLayer += ";"
               "z-index:1;pointer-events:none';";
    jsLayer += "w.appendChild(dim);}";
    jsLayer += "if(!w.querySelector('.matlab-bg-img')){";
    jsLayer += "var img=document.createElement('img');img.className='matlab-bg-img';";
    jsLayer += "img.style.cssText='position:absolute;top:0;left:0;width:100%;height:100%;"
               "object-fit:"; jsLayer += objFit; jsLayer += ";"
               "object-position:center;opacity:"; jsLayer += opacityStr; jsLayer += ";"
               "z-index:0;pointer-events:none';";
    jsLayer += "w.appendChild(img);}";
    jsLayer += "}";
    jsLayer += "})()";

    std::string cmd1;
    cmd1 += "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"";
    for (size_t i = 0; i < jsLayer.size(); i++) {
        char c = jsLayer[i];
        if      (c == '\\')      cmd1 += "\\\\";
        else if (c == '"')       cmd1 += "\\\"";
        else if (c == '\n' || c == '\r') cmd1 += " ";
        else if (c < 0x20)      { char hex[8]; _snprintf(hex, 8, "\\u%04X", (unsigned char)c); cmd1 += hex; }
        else                     cmd1 += c;
    }
    cmd1 += "\"}}";

    if (!CdpWsSend(ws, cmd1.c_str())) {
        Log(L"CDP: layer creation send failed");
        closesocket(ws); WSACleanup(); return false;
    }
    CdpWsRecv(ws);
    Log(L"CDP: dimming layer created");
    /* PLACEHOLDER_CDP_INJECT_PART2 */

    // Get base64 image data
    EnterCriticalSection(&g_httpLock);
    std::vector<BYTE> imgData = g_httpImgData;
    const char *imgMime = g_httpImgMime;
    LeaveCriticalSection(&g_httpLock);

    int imgB64Len = ((imgData.size() + 2) / 3) * 4;
    char *imgB64Buf = new char[imgB64Len + 1];
    b64encode(imgData.data(), (int)imgData.size(), imgB64Buf);
    std::string imgB64(imgB64Buf);
    delete[] imgB64Buf;

    Log(L"CDP: image %d bytes, base64 %d bytes, sending in chunks...",
        (int)imgData.size(), (int)imgB64.size());

    // Send base64 data in 6 KB chunks
    const int CHUNK = 6000;
    int chunkCount = 0;
    int msgId = 2;

    cmd1 = "{\"id\":" + std::to_string(msgId) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"window._mbgC=[]\"}}";
    if (!CdpWsSend(ws, cmd1.c_str())) { closesocket(ws); WSACleanup(); return false; }
    CdpWsRecv(ws);
    msgId++;

    for (int pos = 0; pos < (int)imgB64.size(); pos += CHUNK) {
        int chunkLen = std::min(CHUNK, (int)imgB64.size() - pos);
        std::string chunkData = imgB64.substr(pos, chunkLen);

        std::string chunkEsc;
        for (int i = 0; i < (int)chunkData.size(); i++) {
            char c = chunkData[i];
            if      (c == '\\')      chunkEsc += "\\\\";
            else if (c == '"')       chunkEsc += "\\\"";
            else if (c == '\n' || c == '\r') chunkEsc += " ";
            else if (c < 0x20)      { char hex[8]; _snprintf(hex, 8, "\\u%04X", (unsigned char)c); chunkEsc += hex; }
            else                     chunkEsc += c;
        }

        std::string chunkCmd;
        chunkCmd += "{\"id\":" + std::to_string(msgId) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"window._mbgC.push('";
        chunkCmd += chunkEsc;
        chunkCmd += "')\"}}";

        if (!CdpWsSend(ws, chunkCmd.c_str())) {
            Log(L"CDP: chunk %d send failed at pos %d", chunkCount, pos);
            closesocket(ws); WSACleanup(); return false;
        }
        CdpWsRecv(ws);
        msgId++;
        chunkCount++;
    }

    Log(L"CDP: sent %d chunks (%d bytes total)", chunkCount, (int)imgB64.size());

    // Final command: assemble blob URL and set img.src
    std::string jsFinal;
    jsFinal += "(function(){";
    jsFinal += "var b64=window._mbgC.join('');";
    jsFinal += "var bin=atob(b64);";
    jsFinal += "var bytes=new Uint8Array(bin.length);";
    jsFinal += "for(var i=0;i<bin.length;i++)bytes[i]=bin.charCodeAt(i);";
    jsFinal += "var blob=new Blob([bytes],{type:'"; jsFinal += imgMime; jsFinal += "'});";
    jsFinal += "var url=URL.createObjectURL(blob);";
    jsFinal += "var imgs=document.querySelectorAll('.matlab-bg-img');";
    jsFinal += "for(var j=0;j<imgs.length;j++){";
    jsFinal += "var oldUrl=imgs[j].src;";
    jsFinal += "imgs[j].src=url;";
    jsFinal += "if(oldUrl&&oldUrl.indexOf('blob:')===0)URL.revokeObjectURL(oldUrl);";
    jsFinal += "}";
    jsFinal += "window._mbgC=null;";
    jsFinal += "})()";

    std::string finalCmd;
    finalCmd += "{\"id\":" + std::to_string(msgId) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"";
    for (size_t i = 0; i < jsFinal.size(); i++) {
        char c = jsFinal[i];
        if      (c == '\\')      finalCmd += "\\\\";
        else if (c == '"')       finalCmd += "\\\"";
        else if (c == '\n' || c == '\r') finalCmd += " ";
        else if (c < 0x20)      { char hex[8]; _snprintf(hex, 8, "\\u%04X", (unsigned char)c); finalCmd += hex; }
        else                     finalCmd += c;
    }
    finalCmd += "\"}}";

    if (!CdpWsSend(ws, finalCmd.c_str())) {
        Log(L"CDP: final assemble command failed");
        closesocket(ws); WSACleanup(); return false;
    }
    CdpWsRecv(ws);

    closesocket(ws);
    WSACleanup();

    Log(L"CDP: image set via blob URL (%d chunks)", chunkCount);
    return true;
}
