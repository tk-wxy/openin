#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((section(".data")))
wchar_t g_target_path[1024] = L"__OPENIN_TARGET_EXE_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

/* 固定参数槽: 由 openin 安装时二进制 Patch 覆写(零填充),空串 = 无固定参数 */
__attribute__((section(".data")))
wchar_t g_fixed_args[512] = L"__OPENIN_TARGET_ARGS_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

/* Web URL 槽: 非空 = Web 工具——静默后台启动 server 后自动用默认浏览器打开该 URL
   (如 dsh → http://127.0.0.1:3080);空串 = 普通 CLI 工具 */
__attribute__((section(".data")))
wchar_t g_web_url[512] = L"__OPENIN_TARGET_URL_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

/* 追加固定参数(非空才插入): 插在目标与用户参数之间,如 dsh → npm.cmd exec ... --call "dsh web" */
static void append_fixed_args(wchar_t *cmdline, size_t cap)
{
    if (g_fixed_args[0] != L'\0') {
        wcscat_s(cmdline, cap, L" ");
        wcscat_s(cmdline, cap, g_fixed_args);
    }
}

/* 构造目标命令行(.cmd/.bat 用 cmd /c 包装,其余直接启动): 目标 + 固定参数 + 用户参数 */
static void build_cmdline(int argc, wchar_t *argv[], wchar_t *cmdline, size_t cap)
{
    wchar_t *ext = wcsrchr(g_target_path, L'.');
    int i;

    if (ext && (_wcsicmp(ext, L".cmd") == 0 || _wcsicmp(ext, L".bat") == 0)) {
        cmdline[0] = L'\0';
        wcscat_s(cmdline, cap, L"cmd.exe /c \"\"");
        wcscat_s(cmdline, cap, g_target_path);
        wcscat_s(cmdline, cap, L"\"");
        append_fixed_args(cmdline, cap);
        for (i = 1; i < argc; i++) {
            wcscat_s(cmdline, cap, L" \"");
            wcscat_s(cmdline, cap, argv[i]);
            wcscat_s(cmdline, cap, L"\"");
        }
        wcscat_s(cmdline, cap, L"\"");
    } else {
        cmdline[0] = L'\0';
        wcscat_s(cmdline, cap, L"\"");
        wcscat_s(cmdline, cap, g_target_path);
        wcscat_s(cmdline, cap, L"\"");
        append_fixed_args(cmdline, cap);
        for (i = 1; i < argc; i++) {
            wcscat_s(cmdline, cap, L" \"");
            wcscat_s(cmdline, cap, argv[i]);
            wcscat_s(cmdline, cap, L"\"");
        }
    }
}

/* 从 URL 提取端口(host:port 取最后一个冒号后的数字);无法解析返回 -1 */
static int url_port(const wchar_t *url)
{
    const wchar_t *colon = wcsrchr(url, L':');
    int port;
    if (!colon || colon[1] == L'\0') return -1;
    port = _wtoi(colon + 1);
    return (port > 0 && port <= 65535) ? port : -1;
}

/* 单次探测 TCP 端口是否可连接(server 是否已就绪) */
static int port_ready(int port, WSADATA *wsa)
{
    SOCKET s;
    SOCKADDR_IN addr;
    int ok = 0;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)port);
    if (connect(s, (SOCKADDR *)&addr, sizeof(addr)) == 0) ok = 1;
    closesocket(s);
    return ok;
}

/* 轮询端口就绪(最多 timeout_ms),供 web 模式等 server 起来后再开浏览器 */
static void wait_port_ready(int port, int timeout_ms)
{
    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < timeout_ms) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            int ok = port_ready(port, &wsa);
            WSACleanup();
            if (ok) return;
        }
        Sleep(200);
    }
}

/* HTTP JSON-RPC: 向本地 Web 工具 server 发 POST /api/<method>(winsock 手写,免额外库)。
   仅对提供该 API 的 Web 工具有效(如 dsh 的 workspace.create/session.create);
   返回动态分配宽字符响应全文(调用者 free),失败返回 NULL。 */
static wchar_t *dsh_rpc(const wchar_t *method, const wchar_t *payloadJson)
{
    int port = url_port(g_web_url);
    WSADATA wsa;
    SOCKET s;
    SOCKADDR_IN addr;
    char methA[64], bodyA[2048], reqA[2300], respA[16384];
    wchar_t bodyW[1024], rpcId[48];
    int n, total;
    wchar_t *out;

    if (port <= 0) return NULL;
    _snwprintf_s(rpcId, 48, 47, L"openin-%lu", (unsigned long)GetTickCount());
    _snwprintf_s(bodyW, 1024, 1023,
                 L"{\"type\":\"client-request\",\"rpcId\":\"%s\",\"method\":\"%s\",\"payload\":%s}",
                 rpcId, method, payloadJson);
    if (WideCharToMultiByte(CP_UTF8, 0, bodyW, -1, bodyA, sizeof(bodyA), NULL, NULL) <= 0) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, method, -1, methA, sizeof(methA), NULL, NULL) <= 0) return NULL;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { WSACleanup(); return NULL; }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)port);
    if (connect(s, (SOCKADDR *)&addr, sizeof(addr)) != 0) { closesocket(s); WSACleanup(); return NULL; }

    /* 接收超时 4s: server 若对某个请求长时间不响应(如网络异常时 agent 初始化挂起),
       不让 launcher 无限等待,直接按失败降级(仍会打开浏览器) */
    {
        int tv = 4000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    }

    n = wsprintfA(reqA, "POST /api/%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                        "Content-Type: application/json\r\nContent-Length: %d\r\n"
                        "Connection: close\r\n\r\n%s",
                  methA, port, (int)strlen(bodyA), bodyA);
    if (send(s, reqA, n, 0) == SOCKET_ERROR) { closesocket(s); WSACleanup(); return NULL; }

    total = 0;
    while (total < (int)sizeof(respA) - 1) {
        int r = recv(s, respA + total, (int)sizeof(respA) - 1 - total, 0);
        if (r <= 0) break;
        total += r;
    }
    closesocket(s);
    WSACleanup();
    respA[total] = '\0';

    out = (wchar_t *)malloc((size_t)(total + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, respA, -1, out, total + 1);
    return out;
}

/* The TCP listener can come up before dsh has finished mounting its API. */
static int dsh_rpc_succeeded(const wchar_t *response)
{
    return response && wcsstr(response, L"\"ok\":true") != NULL;
}

/* Retry bounded RPCs during dsh startup instead of silently losing registration. */
static wchar_t *dsh_rpc_retry(const wchar_t *method, const wchar_t *payloadJson)
{
    DWORD start = GetTickCount();
    for (;;) {
        wchar_t *response = dsh_rpc(method, payloadJson);
        if (dsh_rpc_succeeded(response)) return response;
        free(response);
        if ((DWORD)(GetTickCount() - start) >= 12000) return NULL;
        Sleep(250);
    }
}

/* 从 npm.cmd 路径推导 node.exe(同目录) */
static void derive_node_exe(const wchar_t *npmCmd, wchar_t *out, size_t cap)
{
    wcscpy_s(out, cap, npmCmd);
    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash) { slash[1] = L'\0'; wcscat_s(out, cap, L"node.exe"); }
    else wcscpy_s(out, cap, L"node.exe");
}

/* 在 npm 缓存里找 dsh 的 bin.js(_npx\<hash>\node_modules\@deepseek-ai\dsh\lib\bin.js)。
   找到返回 1 并写 out;找不到返回 0。 */
static int find_dsh_binjs(wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH], pattern[MAX_PATH], probe[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) return 0;
    _snwprintf_s(pattern, MAX_PATH, MAX_PATH - 1, L"%s/npm-cache/_npx/*", base);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        _snwprintf_s(probe, MAX_PATH, MAX_PATH - 1,
                     L"%s/npm-cache/_npx/%s/node_modules/@deepseek-ai/dsh/lib/bin.js",
                     base, fd.cFileName);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(out, cap, probe);
            FindClose(h);
            return 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

int wmain(int argc, wchar_t *argv[])
{
    wchar_t cmdline[2048];
    wchar_t open_url[1024];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int webMode = (g_web_url[0] != L'\0');

    build_cmdline(argc, argv, cmdline, 2048);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (webMode) {
        wcscpy_s(open_url, 1024, g_web_url);
        /* Web 工具: 首次启动时用可见 cmd 窗口跑 server——地址栏场景 explorer 会为 cmd 分配
         * 新控制台窗口(可看 npm/dsh 运行日志,Ctrl+C 或关窗即停止),终端场景则继承当前控制台;
         * 端口就绪后自动开浏览器;server 已在跑则跳过启动,只开浏览器 */
        int port = url_port(g_web_url);
        int already = 0;
        if (port > 0) {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
                already = port_ready(port, &wsa);
                WSACleanup();
            }
        }
        if (!already) {
            wchar_t launch[2400];
            /* 优先 node 直跑 dsh 的 bin.js(绕开 npm exec——其在本机环境卡死,且 cmd /c 对
               含引号固定参数有解析问题);找不到 bin.js 或非 npm.cmd 目标则回退原命令 */
            if (wcsstr(g_target_path, L"npm.cmd")) {
                wchar_t binjs[MAX_PATH], nodeExe[MAX_PATH];
                if (find_dsh_binjs(binjs, MAX_PATH)) {
                    derive_node_exe(g_target_path, nodeExe, MAX_PATH);
                    _snwprintf_s(launch, 2400, 2399, L"\"%s\" \"%s\" web", nodeExe, binjs);
                } else {
                    wcscpy_s(launch, 2400, cmdline);
                }
            } else {
                wcscpy_s(launch, 2400, cmdline);
            }
            if (!CreateProcessW(NULL, launch, NULL, NULL, TRUE,
                                0, NULL, NULL, &si, &pi)) {
                wchar_t msg[600];
                _snwprintf_s(msg, 600, 599, L"Failed to start:\n%s\n\nError: %lu",
                             g_target_path, (unsigned long)GetLastError());
                MessageBoxW(NULL, msg, L"openin", MB_OK | MB_ICONERROR);
                return 1;
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (port > 0)
                wait_port_ready(port, 8000);
        }
        /* 按文件夹打开: 把当前目录登记为工作区并建一个最新会话;
         * dsh-session-link Client 插件通过 URL deep-link 选中该 session。
         * 仅对提供 workspace API 的 Web 工具(如 dsh)有效;任一调用失败都降级为直接开浏览器。 */
        {
            wchar_t cwd[MAX_PATH], esc[2 * MAX_PATH + 8], payload[2 * MAX_PATH + 64];
            wchar_t *resp = NULL, *wsId;
            int i, j;
            if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {
                /* JSON 转义路径中的反斜杠与引号 */
                for (i = 0, j = 0; cwd[i] && j < (int)(2 * MAX_PATH) - 2; i++) {
                    if (cwd[i] == L'\\' || cwd[i] == L'"') esc[j++] = L'\\';
                    esc[j++] = cwd[i];
                }
                esc[j] = L'\0';
                _snwprintf_s(payload, 2 * MAX_PATH + 64, 2 * MAX_PATH + 63,
                             L"{\"path\":\"%s\"}", esc);
                resp = dsh_rpc_retry(L"workspace.create", payload);
                wsId = resp ? wcsstr(resp, L"\"workspaceId\":\"") : NULL;
                if (wsId) {
                    wchar_t id[64];
                    wchar_t sessionId[64];
                    int k;
                    wsId += 15;   /* 跳过 "workspaceId":" */
                    for (k = 0; k < 63 && wsId[k] && wsId[k] != L'"'; k++) id[k] = wsId[k];
                    id[k] = L'\0';
                    if (k > 0) {
                        _snwprintf_s(sessionId, 64, 63, L"openin-%lu-%lu",
                                     (unsigned long)GetTickCount(),
                                     (unsigned long)GetCurrentProcessId());
                        _snwprintf_s(payload, 2 * MAX_PATH + 64, 2 * MAX_PATH + 63,
                                     L"{\"workspaceId\":\"%s\",\"sessionId\":\"%s\"}",
                                     id, sessionId);
                        free(resp);
                        resp = dsh_rpc_retry(L"session.create", payload);
                        if (resp) {
                            const wchar_t *sep = wcschr(g_web_url, L'?') ? L"&" : L"?";
                            _snwprintf_s(open_url, 1024, 1023,
                                         L"%s%sopeninSession=%s", g_web_url, sep, sessionId);
                        }
                    }
                }
                free(resp);
            }
        }
        ShellExecuteW(NULL, L"open", open_url, NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }

    /* 普通 CLI 工具: 继承当前控制台
     * 终端场景: 直接继承,输出显示在当前终端
     * 地址栏场景: explorer 会为 launcher 分配控制台 */
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0,
                        NULL, NULL, &si, &pi)) {
        wchar_t msg[600];
        _snwprintf_s(msg, 600, 599, L"Failed to start:\n%s\n\nError: %lu",
                     g_target_path, (unsigned long)GetLastError());
        MessageBoxW(NULL, msg, L"openin", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
