/*
 * core.c — 安装/卸载引擎 + launcher 三层生成 + 预设模板 + 目标表。
 * 依赖 pathenv(加 PATH/查 PATH)与 launcher_templates.h;
 * 对外导出 install/uninstall/find/remove/preset_count 及全局目标表。
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "launcher_templates.h"
#include "openin.h"

static BOOL run_gcc(const wchar_t *cmdline, const wchar_t *logPath,
                    wchar_t *logBuf, size_t logSz)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE hLog;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok = FALSE;
    DWORD code = 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    hLog = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog == INVALID_HANDLE_VALUE) return FALSE;

    {
        HANDLE hNull = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                                   OPEN_EXISTING, 0, NULL);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLog;
        si.hStdError = hLog;
        /* GUI 程序 GetStdHandle(STD_INPUT_HANDLE) 可能无效,用 NUL 设备避免 gcc 卡在读输入 */
        si.hStdInput = (hNull != INVALID_HANDLE_VALUE) ? hNull : NULL;
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessW(NULL, (wchar_t *)cmdline, NULL, NULL, TRUE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
            ok = (code == 0);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (hNull != INVALID_HANDLE_VALUE) CloseHandle(hNull);
    }
    CloseHandle(hLog);

    logBuf[0] = L'\0';
    {
        HANDLE h = CreateFileW(logPath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, NULL);
            if (sz > 0) {
                char *bytes = (char *)malloc(sz + 1);
                if (bytes) {
                    DWORD rd = 0;
                    if (ReadFile(h, bytes, sz, &rd, NULL)) {
                        bytes[rd] = '\0';
                        MultiByteToWideChar(CP_UTF8, 0, bytes, (int)rd,
                                            logBuf, (int)(logSz - 1));
                        logBuf[logSz - 1] = L'\0';
                    }
                    free(bytes);
                }
            }
            CloseHandle(h);
        }
    }
    return ok;
}

/* ---------- 预编译二进制 Patch Launcher (免 GCC 模式) ---------- */
static BOOL write_launcher_binary(const wchar_t *codeExe, const wchar_t *args,
                                  const wchar_t *url, int cli, const wchar_t *outExePath)
{
    const unsigned char *tmpl = cli ? g_launcher_cli_exe : g_launcher_gui_exe;
    size_t tmpl_len = cli ? g_launcher_cli_exe_len : g_launcher_gui_exe_len;

    static const wchar_t magic[] = L"__OPENIN_TARGET_EXE_MAGIC";
    size_t magic_bytes_len = wcslen(magic) * sizeof(wchar_t);

    long found_offset = -1;
    for (size_t i = 0; i <= tmpl_len - magic_bytes_len; i++) {
        if (memcmp(tmpl + i, magic, magic_bytes_len) == 0) {
            found_offset = (long)i;
            break;
        }
    }

    if (found_offset < 0) return FALSE;

    unsigned char *buf = (unsigned char *)malloc(tmpl_len);
    if (!buf) return FALSE;
    memcpy(buf, tmpl, tmpl_len);

    wchar_t target_buf[1024];
    ZeroMemory(target_buf, sizeof(target_buf));
    wcscpy_s(target_buf, 1024, codeExe);

    memcpy(buf + found_offset, target_buf, sizeof(target_buf));

    /* 固定参数槽: 仅 CLI 模板才有。找到即覆写(空串也清零,防残留魔法串被当成参数);
       找不到: GUI 模板无此槽,args 必为空则跳过;若需要 args 却没有槽则失败(走 gcc 编译回退) */
    {
        static const wchar_t args_magic[] = L"__OPENIN_TARGET_ARGS_MAGIC";
        size_t ambl = wcslen(args_magic) * sizeof(wchar_t);
        long args_offset = -1;
        for (size_t i = 0; i <= tmpl_len - ambl; i++) {
            if (memcmp(tmpl + i, args_magic, ambl) == 0) { args_offset = (long)i; break; }
        }
        if (args_offset >= 0) {
            wchar_t args_buf[512];
            ZeroMemory(args_buf, sizeof(args_buf));
            if (args && args[0]) wcscpy_s(args_buf, 512, args);
            memcpy(buf + args_offset, args_buf, sizeof(args_buf));
        } else if (args && args[0]) {
            free(buf);
            return FALSE;
        }
    }

    /* Web URL 槽: 仅 CLI 模板才有。找到即覆写(空串也清零);找不到但 url 非空则失败(走 gcc 编译回退) */
    {
        static const wchar_t url_magic[] = L"__OPENIN_TARGET_URL_MAGIC";
        size_t umbl = wcslen(url_magic) * sizeof(wchar_t);
        long url_offset = -1;
        for (size_t i = 0; i <= tmpl_len - umbl; i++) {
            if (memcmp(tmpl + i, url_magic, umbl) == 0) { url_offset = (long)i; break; }
        }
        if (url_offset >= 0) {
            wchar_t url_buf[512];
            ZeroMemory(url_buf, sizeof(url_buf));
            if (url && url[0]) wcscpy_s(url_buf, 512, url);
            memcpy(buf + url_offset, url_buf, sizeof(url_buf));
        } else if (url && url[0]) {
            free(buf);
            return FALSE;
        }
    }

    HANDLE h = CreateFileW(outExePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(buf);
        return FALSE;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)tmpl_len, &written, NULL) && (written == (DWORD)tmpl_len);
    CloseHandle(h);
    free(buf);
    return ok;
}

/* ---------- openin 归属识别与撤销标记 ---------- */
#define UNDO_MARKER_NAME L".openin-undo"

/* 文件内容是否包含指定字节串(.cmd 按 ANSI/GBK 写出,ASCII 子串字节一致) */
static BOOL file_contains_bytes(const wchar_t *path, const char *needle, size_t needle_len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE;
    DWORD sz = GetFileSize(h, NULL);
    if (sz > 0 && sz < 8 * 1024 * 1024) {
        char *buf = (char *)malloc((size_t)sz);
        if (buf) {
            DWORD rd = 0;
            if (ReadFile(h, buf, sz, &rd, NULL) && rd == sz) {
                for (DWORD i = 0; i + needle_len <= rd; i++)
                    if (memcmp(buf + i, needle, needle_len) == 0) { found = TRUE; break; }
            }
            free(buf);
        }
    }
    CloseHandle(h);
    return found;
}

/* .cmd 是否为 openin 生成的 launcher(含 "openin launcher" 注释) */
static BOOL cmd_is_openin(const wchar_t *cmdPath)
{
    static const char sig[] = "openin launcher";
    return path_exists(cmdPath) &&
           file_contains_bytes(cmdPath, sig, sizeof(sig) - 1);
}

/* .exe 是否为 openin 生成的 launcher(含 L"Failed to start:" 的 UTF-16LE 字节,四变体皆有) */
static BOOL exe_is_openin(const wchar_t *exePath)
{
    static const unsigned char sig[] = {
        'F',0, 'a',0, 'i',0, 'l',0, 'e',0, 'd',0, ' ',0,
        't',0, 'o',0, ' ',0, 's',0, 't',0, 'a',0, 'r',0, 't',0, ':',0
    };
    return path_exists(exePath) &&
           file_contains_bytes(exePath, (const char *)sig, sizeof(sig));
}

/* 目标文件是否为 openin 生成的 launcher(按扩展名分派) */
static BOOL is_openin_launcher(const wchar_t *path)
{
    const wchar_t *ext = wcsrchr(path, L'.');
    if (ext && (_wcsicmp(ext, L".cmd") == 0 || _wcsicmp(ext, L".bat") == 0))
        return cmd_is_openin(path);
    return exe_is_openin(path);
}

/* 写撤销标记(path-added/dir-created 仅记录实际发生的事实);失败返回 FALSE */
static BOOL write_undo_marker(const wchar_t *dir, BOOL pathAdded, BOOL dirCreated)
{
    wchar_t path[MAX_PATH];
    char buf[64];
    int n;
    HANDLE h;
    DWORD written = 0;

    _snwprintf_s(path, MAX_PATH, MAX_PATH - 1, L"%s\\%s", dir, UNDO_MARKER_NAME);
    n = _snprintf_s(buf, 64, 63, "path-added=%d\ndir-created=%d\n",
                    pathAdded ? 1 : 0, dirCreated ? 1 : 0);
    if (n <= 0) return FALSE;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = WriteFile(h, buf, (DWORD)n, &written, NULL) && written == (DWORD)n;
    CloseHandle(h);
    return ok;
}

/* 读撤销标记;文件不存在或解析失败返回 FALSE(调用方按「无标记」保守处理) */
static BOOL read_undo_marker(const wchar_t *dir, BOOL *pathAdded, BOOL *dirCreated)
{
    wchar_t path[MAX_PATH];
    HANDLE h;
    char buf[64] = { 0 };
    DWORD rd = 0;

    *pathAdded = FALSE;
    *dirCreated = FALSE;
    _snwprintf_s(path, MAX_PATH, MAX_PATH - 1, L"%s\\%s", dir, UNDO_MARKER_NAME);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (!ReadFile(h, buf, 63, &rd, NULL)) { CloseHandle(h); return FALSE; }
    buf[rd] = '\0';
    CloseHandle(h);
    *pathAdded = (strstr(buf, "path-added=1") != NULL);
    *dirCreated = (strstr(buf, "dir-created=1") != NULL);
    return TRUE;
}

static void delete_undo_marker(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, MAX_PATH - 1, L"%s\\%s", dir, UNDO_MARKER_NAME);
    DeleteFileW(path);
}

/* 目录里是否还有 openin 生成的 launcher */
static BOOL dir_has_openin_launcher(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pat[MAX_PATH], fp[MAX_PATH];

    _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        const wchar_t *ext = wcsrchr(fd.cFileName, L'.');
        if (!ext) continue;
        if (_wcsicmp(ext, L".cmd") != 0 && _wcsicmp(ext, L".bat") != 0 &&
            _wcsicmp(ext, L".exe") != 0)
            continue;
        _snwprintf_s(fp, MAX_PATH, MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName);
        if (is_openin_launcher(fp)) { FindClose(h); return TRUE; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return FALSE;
}

/* 目录是否为空(忽略 ./.. 与撤销标记自身) */
static BOOL dir_effectively_empty(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pat[MAX_PATH];
    BOOL any = FALSE;

    _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (_wcsicmp(fd.cFileName, UNDO_MARKER_NAME) == 0)
            continue;
        any = TRUE;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return !any;
}

/* 该命令的 launcher 文件是否已安装 */
BOOL target_installed(const wchar_t *name, const wchar_t *installDir)
{
    wchar_t exePath[MAX_PATH], cmdPath[MAX_PATH];
    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);
    return path_exists(exePath) || path_exists(cmdPath);
}

static BOOL name_in_list(const wchar_t *nm, wchar_t (*names)[64], int count)
{
    int i;
    for (i = 0; i < count; i++)
        if (_wcsicmp(names[i], nm) == 0) return TRUE;
    return FALSE;
}

/* 列出安装目录里所有 openin 生成的命令名(去重、字母序、\n 连接);返回个数 */
int list_installed(const wchar_t *installDir, wchar_t *out, size_t outSz)
{
    enum { MAX_NAMES = 256 };
    wchar_t names[MAX_NAMES][64];
    int count = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    wchar_t pat[MAX_PATH], fp[MAX_PATH];

    out[0] = L'\0';

    /* 扫 .cmd(每个 openin launcher 必有)与 .exe,用归属签名识别并去重 */
    {
        const wchar_t *pats[] = { L"*.cmd", L"*.exe" };
        int pi;
        for (pi = 0; pi < 2; pi++) {
            _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s\\%s", installDir, pats[pi]);
            h = FindFirstFileW(pat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                _snwprintf_s(fp, MAX_PATH, MAX_PATH - 1, L"%s\\%s", installDir, fd.cFileName);
                BOOL owned = (pi == 0) ? cmd_is_openin(fp) : exe_is_openin(fp);
                if (owned) {
                    wchar_t nm[64];
                    wcsncpy_s(nm, 64, fd.cFileName, 63);
                    nm[63] = L'\0';
                    wchar_t *dot = wcsrchr(nm, L'.');
                    if (dot) *dot = L'\0';
                    if (count < MAX_NAMES && !name_in_list(nm, names, count))
                        wcscpy_s(names[count++], 64, nm);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    /* 字母序(插入排序) */
    {
        int i;
        for (i = 1; i < count; i++) {
            wchar_t key[64];
            int j = i - 1;
            wcscpy_s(key, 64, names[i]);
            while (j >= 0 && _wcsicmp(names[j], key) > 0) {
                wcscpy_s(names[j + 1], 64, names[j]);
                j--;
            }
            wcscpy_s(names[j + 1], 64, key);
        }
    }

    /* 拼接 */
    {
        size_t used = 0;
        int i;
        for (i = 0; i < count; i++) {
            size_t n = wcslen(names[i]);
            if (used + n + 2 > outSz) break;   /* 需 \n + 名称 + \0 的空间 */
            if (used) out[used++] = L'\n';
            wcscpy_s(out + used, outSz - used, names[i]);
            used += n;
        }
    }
    return count;
}

/* ---------- 生成 launcher 源码 ---------- */
/*
 * 生成的 launcher: 启动主程序打开「当前工作目录」或命令行参数指定路径。
 * 模板按行存放,@@PATH@@ 处替换为转义后的主程序完整路径,@@NAME@@ 为命令名。
 * 使用 wcscat 拼接命令行以避免引号嵌套带来的格式转义问题。
 */
static BOOL write_launcher_source(const wchar_t *codeExe, const wchar_t *name,
                                  const wchar_t *args, const wchar_t *url,
                                  int cli, const wchar_t *srcPath)
{
    static const wchar_t *TPL[] = {
        L"#define UNICODE",
        L"#define _UNICODE",
        L"#include <windows.h>",
        L"#include <stdio.h>",
        L"",
        L"#define CODE_EXE L\"@@PATH@@\"",
        L"",
        L"int wmain(int argc, wchar_t *argv[])",
        L"{",
        L"    wchar_t cmdline[2 * MAX_PATH + 64];",
        L"    wchar_t cwd[MAX_PATH];",
        L"    wchar_t *target;",
        L"    STARTUPINFOW si;",
        L"    PROCESS_INFORMATION pi;",
        L"",
        L"    if (argc > 1) {",
        L"        int len = (int)wcslen(argv[1]);",
        L"        if (len >= 2 && argv[1][0] == L'\"' && argv[1][len - 1] == L'\"') {",
        L"            argv[1][len - 1] = L'\\0';",
        L"            target = argv[1] + 1;",
        L"        } else {",
        L"            target = argv[1];",
        L"        }",
        L"    } else {",
        L"        if (GetCurrentDirectoryW(MAX_PATH, cwd) == 0) return 1;",
        L"        target = cwd;",
        L"    }",
        L"",
        L"    cmdline[0] = L'\\0';",
        L"    wcscat_s(cmdline, 2 * MAX_PATH + 64, L\"\\\"\");",
        L"    wcscat_s(cmdline, 2 * MAX_PATH + 64, CODE_EXE);",
        L"    wcscat_s(cmdline, 2 * MAX_PATH + 64, L\"\\\" \\\"\");",
        L"    wcscat_s(cmdline, 2 * MAX_PATH + 64, target);",
        L"    wcscat_s(cmdline, 2 * MAX_PATH + 64, L\"\\\"\");",
        L"    for (int a = 2; a < argc; a++) {",
        L"        wcscat_s(cmdline, 2 * MAX_PATH + 64, L\" \");",
        L"        wcscat_s(cmdline, 2 * MAX_PATH + 64, argv[a]);",
        L"    }",
        L"",
        L"    ZeroMemory(&si, sizeof(si));",
        L"    si.cb = sizeof(si);",
        L"    ZeroMemory(&pi, sizeof(pi));",
        L"",
        L"    if (!CreateProcessW(CODE_EXE, cmdline, NULL, NULL, FALSE,",
        L"                        DETACHED_PROCESS, NULL, NULL, &si, &pi)) {",
        L"        wchar_t msg[600];",
        L"        _snwprintf_s(msg, 600, 599, L\"Failed to start:\\n%s\\n\\nError: %lu\",",
        L"                     CODE_EXE, (unsigned long)GetLastError());",
        L"        MessageBoxW(NULL, msg, L\"@@NAME@@\", MB_OK | MB_ICONERROR);",
        L"        return 1;",
        L"    }",
        L"    CloseHandle(pi.hThread);",
        L"    CloseHandle(pi.hProcess);",
        L"    return 0;",
        L"}",
        L""
    };
    /* CLI 模板: 智能判断 .cmd/.bat 用 cmd /c 执行,其他直接启动,继承控制台;
       CODE_URL 非空 = Web 工具: 静默后台启动 server(CREATE_NO_WINDOW),端口就绪后自动开浏览器 */
    static const wchar_t *CLI_TPL[] = {
        L"#define UNICODE",
        L"#define _UNICODE",
        L"#include <winsock2.h>",
        L"#include <windows.h>",
        L"#include <stdio.h>",
        L"#include <stdlib.h>",
        L"#include <string.h>",
        L"",
        L"#define CODE_EXE L\"@@PATH@@\"",
        L"#define CODE_ARGS L\"@@ARGS@@\"",
        L"#define CODE_URL L\"@@URL@@\"",
        L"",
        L"/* 追加固定参数(非空才插入) */",
        L"static void append_args(wchar_t *cmdline, size_t cap)",
        L"{",
        L"    if (CODE_ARGS[0]) { wcscat_s(cmdline, cap, L\" \"); wcscat_s(cmdline, cap, CODE_ARGS); }",
        L"}",
        L"",
        L"/* 构造目标命令行: 目标(.cmd/.bat 用 cmd /c 包装) + 固定参数 + 用户参数 */",
        L"static void build_cmdline(int argc, wchar_t *argv[], wchar_t *cmdline, size_t cap)",
        L"{",
        L"    wchar_t *ext = wcsrchr(CODE_EXE, L'.');",
        L"    int i;",
        L"    if (ext && (_wcsicmp(ext, L\".cmd\") == 0 || _wcsicmp(ext, L\".bat\") == 0)) {",
        L"        cmdline[0] = L'\\0';",
        L"        wcscat_s(cmdline, cap, L\"cmd.exe /c \\\"\\\"\");",
        L"        wcscat_s(cmdline, cap, CODE_EXE);",
        L"        wcscat_s(cmdline, cap, L\"\\\"\");",
        L"        append_args(cmdline, cap);",
        L"        for (i = 1; i < argc; i++) {",
        L"            wcscat_s(cmdline, cap, L\" \\\"\");",
        L"            wcscat_s(cmdline, cap, argv[i]);",
        L"            wcscat_s(cmdline, cap, L\"\\\"\");",
        L"        }",
        L"        wcscat_s(cmdline, cap, L\"\\\"\");",
        L"    } else {",
        L"        cmdline[0] = L'\\0';",
        L"        wcscat_s(cmdline, cap, L\"\\\"\");",
        L"        wcscat_s(cmdline, cap, CODE_EXE);",
        L"        wcscat_s(cmdline, cap, L\"\\\"\");",
        L"        append_args(cmdline, cap);",
        L"        for (i = 1; i < argc; i++) {",
        L"            wcscat_s(cmdline, cap, L\" \\\"\");",
        L"            wcscat_s(cmdline, cap, argv[i]);",
        L"            wcscat_s(cmdline, cap, L\"\\\"\");",
        L"        }",
        L"    }",
        L"}",
        L"",
        L"/* URL 端口提取(host:port 取最后一个冒号后的数字) */",
        L"static int url_port(const wchar_t *url)",
        L"{",
        L"    const wchar_t *colon = wcsrchr(url, L':');",
        L"    int port;",
        L"    if (!colon || colon[1] == L'\\0') return -1;",
        L"    port = _wtoi(colon + 1);",
        L"    return (port > 0 && port <= 65535) ? port : -1;",
        L"}",
        L"",
        L"/* 单次探测 TCP 端口是否可连接 */",
        L"static int port_ready(int port)",
        L"{",
        L"    SOCKET s; SOCKADDR_IN addr; int ok = 0;",
        L"    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);",
        L"    if (s == INVALID_SOCKET) return 0;",
        L"    addr.sin_family = AF_INET;",
        L"    addr.sin_addr.s_addr = inet_addr(\"127.0.0.1\");",
        L"    addr.sin_port = htons((u_short)port);",
        L"    if (connect(s, (SOCKADDR *)&addr, sizeof(addr)) == 0) ok = 1;",
        L"    closesocket(s);",
        L"    return ok;",
        L"}",
        L"",
        L"/* 轮询端口就绪(最多 timeout_ms) */",
        L"static void wait_port_ready(int port, int timeout_ms)",
        L"{",
        L"    WSADATA wsa; DWORD start = GetTickCount();",
        L"    WSAStartup(MAKEWORD(2, 2), &wsa);",
        L"    while ((int)(GetTickCount() - start) < timeout_ms) {",
        L"        if (port_ready(port)) break;",
        L"        Sleep(200);",
        L"    }",
        L"    WSACleanup();",
        L"}",
        L"",
        L"/* HTTP JSON-RPC: 向本地 Web 工具 server 发 POST /api/<method>(winsock 手写,免额外库)。",
        L"   仅对提供该 API 的 Web 工具有效(如 dsh 的 workspace.create/session.create);",
        L"   返回动态分配宽字符响应全文(调用者 free),失败返回 NULL。 */",
        L"static wchar_t *dsh_rpc(const wchar_t *method, const wchar_t *payloadJson)",
        L"{",
        L"    int port = url_port(CODE_URL);",
        L"    WSADATA wsa;",
        L"    SOCKET s;",
        L"    SOCKADDR_IN addr;",
        L"    char methA[64], bodyA[2048], reqA[2300], respA[16384];",
        L"    wchar_t bodyW[1024], rpcId[48];",
        L"    int n, total;",
        L"    wchar_t *out;",
        L"",
        L"    if (port <= 0) return NULL;",
        L"    _snwprintf_s(rpcId, 48, 47, L\"openin-%lu\", (unsigned long)GetTickCount());",
        L"    _snwprintf_s(bodyW, 1024, 1023,",
        L"                 L\"{\\\"type\\\":\\\"client-request\\\",\\\"rpcId\\\":\\\"%s\\\",\\\"method\\\":\\\"%s\\\",\\\"payload\\\":%s}\",",
        L"                 rpcId, method, payloadJson);",
        L"    if (WideCharToMultiByte(CP_UTF8, 0, bodyW, -1, bodyA, sizeof(bodyA), NULL, NULL) <= 0) return NULL;",
        L"    if (WideCharToMultiByte(CP_UTF8, 0, method, -1, methA, sizeof(methA), NULL, NULL) <= 0) return NULL;",
        L"",
        L"    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;",
        L"    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);",
        L"    if (s == INVALID_SOCKET) { WSACleanup(); return NULL; }",
        L"    addr.sin_family = AF_INET;",
        L"    addr.sin_addr.s_addr = inet_addr(\"127.0.0.1\");",
        L"    addr.sin_port = htons((u_short)port);",
        L"    if (connect(s, (SOCKADDR *)&addr, sizeof(addr)) != 0) { closesocket(s); WSACleanup(); return NULL; }",
        L"",
        L"    /* 接收超时 4s: server 若长时间不响应(如网络异常时 agent 初始化挂起),",
        L"       不让 launcher 无限等待,按失败降级(仍会打开浏览器) */",
        L"    {",
        L"        int tv = 4000;",
        L"        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));",
        L"    }",
        L"",
        L"    n = wsprintfA(reqA, \"POST /api/%s HTTP/1.1\\r\\nHost: 127.0.0.1:%d\\r\\n\"",
        L"                        \"Content-Type: application/json\\r\\nContent-Length: %d\\r\\n\"",
        L"                        \"Connection: close\\r\\n\\r\\n%s\",",
        L"                  methA, port, (int)strlen(bodyA), bodyA);",
        L"    if (send(s, reqA, n, 0) == SOCKET_ERROR) { closesocket(s); WSACleanup(); return NULL; }",
        L"",
        L"    total = 0;",
        L"    while (total < (int)sizeof(respA) - 1) {",
        L"        int r = recv(s, respA + total, (int)sizeof(respA) - 1 - total, 0);",
        L"        if (r <= 0) break;",
        L"        total += r;",
        L"    }",
        L"    closesocket(s);",
        L"    WSACleanup();",
        L"    respA[total] = '\\0';",
        L"",
        L"    out = (wchar_t *)malloc((size_t)(total + 1) * sizeof(wchar_t));",
        L"    if (!out) return NULL;",
        L"    MultiByteToWideChar(CP_UTF8, 0, respA, -1, out, total + 1);",
        L"    return out;",
        L"}",
        L"",
        L"/* 从 npm.cmd 路径推导 node.exe(同目录) */",
        L"static void derive_node_exe(const wchar_t *npmCmd, wchar_t *out, size_t cap)",
        L"{",
        L"    wcscpy_s(out, cap, npmCmd);",
        L"    wchar_t *slash = wcsrchr(out, L'\\x5c');",
        L"    if (slash) { slash[1] = L'\\0'; wcscat_s(out, cap, L\"node.exe\"); }",
        L"    else wcscpy_s(out, cap, L\"node.exe\");",
        L"}",
        L"",
        L"/* 在 npm 缓存里找 dsh 的 bin.js(_npx/<hash>/node_modules/@deepseek-ai/dsh/lib/bin.js) */",
        L"static int find_dsh_binjs(wchar_t *out, size_t cap)",
        L"{",
        L"    wchar_t base[MAX_PATH], pattern[MAX_PATH], probe[MAX_PATH];",
        L"    WIN32_FIND_DATAW fd;",
        L"    HANDLE h;",
        L"    if (GetEnvironmentVariableW(L\"LOCALAPPDATA\", base, MAX_PATH) == 0) return 0;",
        L"    _snwprintf_s(pattern, MAX_PATH, MAX_PATH - 1, L\"%s/npm-cache/_npx/*\", base);",
        L"    h = FindFirstFileW(pattern, &fd);",
        L"    if (h == INVALID_HANDLE_VALUE) return 0;",
        L"    do {",
        L"        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;",
        L"        if (fd.cFileName[0] == L'.') continue;",
        L"        _snwprintf_s(probe, MAX_PATH, MAX_PATH - 1,",
        L"                     L\"%s/npm-cache/_npx/%s/node_modules/@deepseek-ai/dsh/lib/bin.js\",",
        L"                     base, fd.cFileName);",
        L"        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {",
        L"            wcscpy_s(out, cap, probe);",
        L"            FindClose(h);",
        L"            return 1;",
        L"        }",
        L"    } while (FindNextFileW(h, &fd));",
        L"    FindClose(h);",
        L"    return 0;",
        L"}",
        L"",
        L"int wmain(int argc, wchar_t *argv[])",
        L"{",
        L"    wchar_t cmdline[2048];",
        L"    STARTUPINFOW si;",
        L"    PROCESS_INFORMATION pi;",
        L"    int webMode = (CODE_URL[0] != L'\\0');",
        L"",
        L"    build_cmdline(argc, argv, cmdline, 2048);",
        L"",
        L"    ZeroMemory(&si, sizeof(si));",
        L"    si.cb = sizeof(si);",
        L"    ZeroMemory(&pi, sizeof(pi));",
        L"",
        L"    if (webMode) {",
        L"        /* Web 工具: 首次启动时用可见 cmd 窗口跑 server(地址栏场景分配新控制台,可看 npm/dsh",
        L"           运行日志、Ctrl+C 或关窗停止;终端场景继承当前控制台),端口就绪后自动打开浏览器;",
        L"           server 已在跑则跳过启动,只开浏览器 */",
        L"        int port = url_port(CODE_URL);",
        L"        int already = 0;",
        L"        WSADATA wsa;",
        L"        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {",
        L"            if (port > 0) already = port_ready(port);",
        L"            WSACleanup();",
        L"        }",
        L"        if (!already) {",
        L"            wchar_t launch[2400];",
        L"            /* 优先 node 直跑 dsh bin.js(绕开 npm exec 卡死与 cmd /c 引号问题) */",
        L"            if (wcsstr(CODE_EXE, L\"npm.cmd\")) {",
        L"                wchar_t binjs[MAX_PATH], nodeExe[MAX_PATH];",
        L"                if (find_dsh_binjs(binjs, MAX_PATH)) {",
        L"                    derive_node_exe(CODE_EXE, nodeExe, MAX_PATH);",
        L"                    _snwprintf_s(launch, 2400, 2399, L\"\\\"%s\\\" \\\"%s\\\" web\", nodeExe, binjs);",
        L"                } else {",
        L"                    wcscpy_s(launch, 2400, cmdline);",
        L"                }",
        L"            } else {",
        L"                wcscpy_s(launch, 2400, cmdline);",
        L"            }",
        L"            if (!CreateProcessW(NULL, launch, NULL, NULL, TRUE,",
        L"                                0, NULL, NULL, &si, &pi)) {",
        L"                wchar_t msg[600];",
        L"                _snwprintf_s(msg, 600, 599, L\"Failed to start:\\n%s\\n\\nError: %lu\",",
        L"                             CODE_EXE, (unsigned long)GetLastError());",
        L"                MessageBoxW(NULL, msg, L\"openin\", MB_OK | MB_ICONERROR);",
        L"                return 1;",
        L"            }",
        L"            CloseHandle(pi.hThread);",
        L"            CloseHandle(pi.hProcess);",
        L"            if (port > 0) wait_port_ready(port, 8000);",
        L"        }",
        L"        /* 按文件夹打开(2026-08-15): 把当前目录登记为工作区并建一个最新会话,UI 初始选择",
        L"           (最近会话所在的工作区)随之落到当前目录;任一调用失败都降级为直接开浏览器。 */",
        L"        {",
        L"            wchar_t cwd[MAX_PATH], esc[2 * MAX_PATH + 8], payload[2 * MAX_PATH + 64];",
        L"            wchar_t *resp = NULL, *wsId;",
        L"            int i, j;",
        L"            if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) {",
        L"                /* JSON 转义路径中的反斜杠与引号 */",
        L"                for (i = 0, j = 0; cwd[i] && j < (int)(2 * MAX_PATH) - 2; i++) {",
        L"                    if (cwd[i] == L'\\x5c' || cwd[i] == L'\"') esc[j++] = L'\\x5c';",
        L"                    esc[j++] = cwd[i];",
        L"                }",
        L"                esc[j] = L'\\0';",
        L"                _snwprintf_s(payload, 2 * MAX_PATH + 64, 2 * MAX_PATH + 63,",
        L"                             L\"{\\\"path\\\":\\\"%s\\\"}\", esc);",
        L"                resp = dsh_rpc(L\"workspace.create\", payload);",
        L"                wsId = resp ? wcsstr(resp, L\"\\\"workspaceId\\\":\\\"\") : NULL;",
        L"                if (wsId) {",
        L"                    wchar_t id[64];",
        L"                    int k;",
        L"                    wsId += 15;   /* 跳过 \"workspaceId\":\" */",
        L"                    for (k = 0; k < 63 && wsId[k] && wsId[k] != L'\"'; k++) id[k] = wsId[k];",
        L"                    id[k] = L'\\0';",
        L"                    if (k > 0) {",
        L"                        _snwprintf_s(payload, 2 * MAX_PATH + 64, 2 * MAX_PATH + 63,",
        L"                                     L\"{\\\"workspaceId\\\":\\\"%s\\\"}\", id);",
        L"                        free(resp);",
        L"                        resp = dsh_rpc(L\"session.create\", payload);",
        L"                    }",
        L"                }",
        L"                free(resp);",
        L"            }",
        L"        }",
        L"        ShellExecuteW(NULL, L\"open\", CODE_URL, NULL, NULL, SW_SHOWNORMAL);",
        L"        return 0;",
        L"    }",
        L"",
        L"    /* 普通 CLI 工具: 继承当前控制台 */",
        L"    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0,",
        L"                        NULL, NULL, &si, &pi)) {",
        L"        wchar_t msg[600];",
        L"        _snwprintf_s(msg, 600, 599, L\"Failed to start:\\n%s\\n\\nError: %lu\",",
        L"                     CODE_EXE, (unsigned long)GetLastError());",
        L"        MessageBoxW(NULL, msg, L\"openin\", MB_OK | MB_ICONERROR);",
        L"        return 1;",
        L"    }",
        L"    CloseHandle(pi.hThread);",
        L"    CloseHandle(pi.hProcess);",
        L"    return 0;",
        L"}",
        L""
    };
    const wchar_t **tpl = cli ? CLI_TPL : TPL;
    size_t tplCount = cli ? (sizeof(CLI_TPL) / sizeof(CLI_TPL[0]))
                          : (sizeof(TPL) / sizeof(TPL[0]));

    enum { CAP = 16384 };
    wchar_t *src;
    size_t pos = 0, t;
    int u8len;
    char *u8;
    HANDLE h;
    DWORD written;

    src = (wchar_t *)malloc(CAP * sizeof(wchar_t));
    if (!src) return FALSE;

    for (t = 0; t < tplCount; t++) {
        const wchar_t *line = tpl[t];
        const wchar_t *pmark = wcsstr(line, L"@@PATH@@");
        const wchar_t *nmark = wcsstr(line, L"@@NAME@@");
        const wchar_t *amark = wcsstr(line, L"@@ARGS@@");
        const wchar_t *umark = wcsstr(line, L"@@URL@@");
        const wchar_t *mark = pmark;
        const wchar_t *subst = codeExe;
        size_t mlen = 8;
        size_t l = wcslen(line);

        if (nmark && (!pmark || nmark < pmark)) {
            mark = nmark;
            subst = name;
        }
        if (amark && (!mark || amark < mark)) {
            mark = amark;
            subst = (args && args[0]) ? args : L"";
        }
        if (umark && (!mark || umark < mark)) {
            mark = umark;
            subst = (url && url[0]) ? url : L"";
            mlen = 7;   /* @@URL@@ = 7 字符(其余 @@PATH@@/@@ARGS@@/@@NAME@@ 均为 8) */
        }
        if (mark) {
            size_t pre = (size_t)(mark - line);
            if (pos + l + 2000 >= CAP) { free(src); return FALSE; }
            memcpy(src + pos, line, pre * sizeof(wchar_t));
            pos += pre;
            for (const wchar_t *cp = subst; *cp; cp++) {
                if (*cp == L'\\' || *cp == L'"') src[pos++] = L'\\';
                src[pos++] = *cp;
            }
            memcpy(src + pos, mark + mlen, (l - pre - mlen) * sizeof(wchar_t));
            pos += l - pre - mlen;
        } else {
            if (pos + l + 4 >= CAP) { free(src); return FALSE; }
            memcpy(src + pos, line, l * sizeof(wchar_t));
            pos += l;
        }
        src[pos++] = L'\n';
    }

    u8len = WideCharToMultiByte(CP_UTF8, 0, src, (int)pos, NULL, 0, NULL, NULL);
    if (u8len <= 0) { free(src); return FALSE; }
    u8 = (char *)malloc((size_t)u8len + 1);
    if (!u8) { free(src); return FALSE; }
    WideCharToMultiByte(CP_UTF8, 0, src, (int)pos, u8, u8len, NULL, NULL);
    u8[u8len] = '\0';

    h = CreateFileW(srcPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(u8); free(src); return FALSE; }
    WriteFile(h, u8, (DWORD)u8len, &written, NULL);
    CloseHandle(h);

    free(u8);
    free(src);
    return TRUE;
}

/* ---------- 生成 .cmd 备用启动器(无需编译器) ---------- */
/* 把路径中的 % ^ & 转义为 cmd 安全形式 */
static void cmd_escape(const wchar_t *src, wchar_t *dst, size_t dst_sz)
{
    size_t n = 0;
    for (const wchar_t *c = src; *c && n + 3 < dst_sz; c++) {
        if (*c == L'%') { dst[n++] = L'%'; dst[n++] = L'%'; }
        else if (*c == L'^') { dst[n++] = L'^'; dst[n++] = L'^'; }
        else if (*c == L'&') { dst[n++] = L'^'; dst[n++] = L'&'; }
        else dst[n++] = *c;
    }
    dst[n] = L'\0';
}

static BOOL write_launcher_cmd(const wchar_t *codeExe, const wchar_t *args,
                               const wchar_t *url, int cli, const wchar_t *cmdPath)
{
    wchar_t esc[2 * MAX_PATH + 16];
    wchar_t esc_args[1024];
    wchar_t esc_url[1024];
    wchar_t buf[4 * MAX_PATH + 1024];
    char *ansi;
    int alen;
    HANDLE h;
    DWORD written;

    cmd_escape(codeExe, esc, 2 * MAX_PATH + 16);
    cmd_escape(args && args[0] ? args : L"", esc_args, 1024);
    cmd_escape(url && url[0] ? url : L"", esc_url, 1024);

    if (cli && url && url[0]) {
        /* Web 工具: 后台静默启动 server,再打开浏览器 URL */
        _snwprintf_s(buf, 4 * MAX_PATH + 1024, 4 * MAX_PATH + 1024 - 1,
            L"@echo off\r\n"
            L"rem openin launcher - generated by openin\r\n"
            L"start \"\" /b cmd /c \"\"%s\"%s%s %%*\" 2>nul\r\n"
            L"start \"\" \"%s\"\r\n",
            esc, esc_args[0] ? L" " : L"", esc_args, esc_url);
    } else if (cli) {
        /* CLI: 继承 cwd,透传参数;args 非空则插在目标与 %* 之间,新窗口运行 */
        _snwprintf_s(buf, 4 * MAX_PATH + 1024, 4 * MAX_PATH + 1024 - 1,
            L"@echo off\r\n"
            L"rem openin launcher - generated by openin\r\n"
            L"start \"\" \"%s\"%s%s %%*\r\n",
            esc, esc_args[0] ? L" " : L"", esc_args);
    } else {
        _snwprintf_s(buf, 4 * MAX_PATH + 1024, 4 * MAX_PATH + 1024 - 1,
            L"@echo off\r\n"
            L"rem openin launcher - generated by openin\r\n"
            L"if \"%%~1\"==\"\" (\r\n"
            L"    start \"\" \"%s\" \"%%CD%%\" %%2 %%3 %%4 %%5 %%6 %%7 %%8 %%9\r\n"
            L") else (\r\n"
            L"    start \"\" \"%s\" \"%%~1\" %%2 %%3 %%4 %%5 %%6 %%7 %%8 %%9\r\n"
            L")\r\n",
            esc, esc);
    }

    /* 按系统 ANSI 代码页(中文系统为 GBK)写出,cmd 才能正确解析中文路径 */
    alen = WideCharToMultiByte(CP_ACP, 0, buf, -1, NULL, 0, NULL, NULL);
    if (alen <= 0) return FALSE;
    ansi = (char *)malloc((size_t)alen);
    if (!ansi) return FALSE;
    if (WideCharToMultiByte(CP_ACP, 0, buf, -1, ansi, alen, NULL, NULL) <= 0) {
        free(ansi);
        return FALSE;
    }

    h = CreateFileW(cmdPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(ansi); return FALSE; }
    WriteFile(h, ansi, (DWORD)(alen - 1), &written, NULL);   /* 去掉结尾 \0 */
    CloseHandle(h);
    free(ansi);
    return TRUE;
}
/* ---------- 目标(仅会话内存,不持久化、不产生任何外部文件) ---------- */
wchar_t g_installDir[MAX_PATH];   /* 每次运行由 pick_target_dir 计算,不落盘 */
Target g_targets[MAX_TARGETS];
int g_targetCount = 0;

int find_target(const wchar_t *name)
{
    for (int i = 0; i < g_targetCount; i++)
        if (_wcsicmp(g_targets[i].name, name) == 0) return i;
    return -1;
}

void remove_target_entry(const wchar_t *name)
{
    int idx = find_target(name);
    if (idx < 0) return;
    for (int j = idx; j < g_targetCount - 1; j++) g_targets[j] = g_targets[j + 1];
    g_targetCount--;
}

/* ---------- 安装 / 卸载单个目标 ---------- */
/*
 * 安装单个目标: 写 .cmd 备用启动器 → 有 gcc 则编译 .exe → 加入 PATH → 冲突检查。
 * 成功返回 0,失败返回 1;summary 写入 outSummary。
 * outAddedPath 记录本次是否真的往用户 PATH 追加了目录。
 * onStep 非空时逐条通报操作步骤(供 GUI 左下角闪显,信息公开)。
 */
static void emit_step(StepCb cb, const wchar_t *msg)
{
    if (cb) cb(msg);
}

int install_target(const wchar_t *name, const wchar_t *codeExe, const wchar_t *args,
                   const wchar_t *url, int cli, const wchar_t *installDir,
                   wchar_t *outSummary, size_t sumSz, int *outAddedPath, StepCb onStep)
{
    wchar_t cmdPath[MAX_PATH], exePath[MAX_PATH], srcPath[MAX_PATH], logPath[MAX_PATH];
    wchar_t tempDir[MAX_PATH];
    wchar_t cmdline[1024], logBuf[4096];
    wchar_t conflictPath[MAX_PATH], gccPath[MAX_PATH];
    int exeBuilt = 0, pathAlready = 0, conflict = 0, compileFailed = 0;
    BOOL dirCreated = FALSE;

    /* CLI 工具: 写入 App Paths 注册表(HKCU),实现地址栏/终端分离
       - 地址栏 'codex' → ShellExecuteEx 查 App Paths → 启动 openin launcher
       - 终端 'codex' → SearchPath 仅查 PATH → 运行原生 CLI(不受影响)
       - 卸载时清理注册表键,零残留 */

    /* 确保安装目录存在(-p 可能指向尚未创建的目录);记录是否由 openin 创建(供卸载还原) */
    dirCreated = !path_exists(installDir);
    CreateDirectoryW(installDir, NULL);

    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);
    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    /* 临时文件放 openin.exe 所在目录(项目目录),用完即删,不碰系统 %TEMP% */
    if (GetModuleFileNameW(NULL, tempDir, MAX_PATH) > 0) {
        wchar_t *slash = wcsrchr(tempDir, L'\\');
        if (slash) *slash = L'\0';
    } else {
        wcscpy_s(tempDir, MAX_PATH, L".");
    }
    _snwprintf_s(srcPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.c", tempDir, name);
    _snwprintf_s(logPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s-build.log", tempDir, name);

    /* 覆盖保护: 已存在同名文件且不是 openin 生成的,拒绝覆盖(防误毁真实工具) */
    emit_step(onStep, L"检查同名文件归属…");
    {
        BOOL cmdExists = path_exists(cmdPath);
        BOOL exeExists = path_exists(exePath);
        if (cmdExists && !cmd_is_openin(cmdPath)) {
            _snwprintf_s(outSummary, sumSz, sumSz - 1,
                         L"已存在同名文件,且不是 openin 生成的,拒绝覆盖:\n%s\n\n请先处理该文件再安装。",
                         cmdPath);
            return 1;
        }
        if (exeExists && !cmdExists) {
            _snwprintf_s(outSummary, sumSz, sumSz - 1,
                         L"已存在同名 .exe(可能是真实工具),且没有 openin 的 .cmd 配对,拒绝覆盖:\n%s",
                         exePath);
            return 1;
        }
        if (cmdExists && cmd_is_openin(cmdPath) && exeExists && !exe_is_openin(exePath)) {
            _snwprintf_s(outSummary, sumSz, sumSz - 1,
                         L"检测到 openin 的 .cmd 已存在,但同名 .exe 不是 openin 生成的(可能被替换),拒绝覆盖:\n%s",
                         exePath);
            return 1;
        }
    }

    /* .cmd 备用启动器(无需编译器,始终生成) */
    {
        wchar_t st[400];
        _snwprintf_s(st, 400, 399, L"写入 %s.cmd 备用启动器…", name);
        emit_step(onStep, st);
    }
    if (!write_launcher_cmd(codeExe, args, url, cli, cmdPath)) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"生成 .cmd 备用启动器失败:\n%s", cmdPath);
        return 1;
    }

    /* 1. 优先使用预编译二进制 Patch 模板生成 .exe (免 GCC 模式, 毫秒级完成, 无环境依赖) */
    {
        wchar_t st[400];
        _snwprintf_s(st, 400, 399, L"生成 %s.exe(二进制 Patch)…", name);
        emit_step(onStep, st);
    }
    if (write_launcher_binary(codeExe, args, url, cli, exePath)) {
        exeBuilt = 1;
    }
    /* 2. 备用: 若二进制 Patch 失败且系统存在 gcc, 则降级使用 gcc 动态编译 */
    else if (find_in_path(L"gcc.exe", gccPath, MAX_PATH)) {
        if (write_launcher_source(codeExe, name, args, url, cli, srcPath)) {
            {
                wchar_t st[400];
                _snwprintf_s(st, 400, 399, L"用 gcc 编译 %s.exe…", name);
                emit_step(onStep, st);
            }
            _snwprintf_s(cmdline, 1024, 1023,
                         L"gcc -O2 -s -municode -mwindows -o \"%s\" \"%s\" -lws2_32", exePath, srcPath);
            if (run_gcc(cmdline, logPath, logBuf, 4096))
                exeBuilt = 1;
            else
                compileFailed = 1;
        } else {
            compileFailed = 1;
        }
        /* 临时源码与编译日志用完即删,目标目录零残留 */
        DeleteFileW(srcPath);
        DeleteFileW(logPath);
    }

    /* 撤销标记: 先写标记、后改 PATH——「PATH 被改 ⟹ 标记存在」的不变量,标记写失败则跳过加 PATH */
    emit_step(onStep, L"写撤销标记 .openin-undo…");
    {
        BOOL pathWillAdd = !path_in_environment(installDir);
        BOOL oldPath = FALSE, oldDir = FALSE;
        if (read_undo_marker(installDir, &oldPath, &oldDir)) {
            pathWillAdd = pathWillAdd || oldPath;   /* 单调合并,防后续 install 覆盖丢 dir-created */
            dirCreated = dirCreated || oldDir;
        }
        if (pathWillAdd || dirCreated) {
            if (!write_undo_marker(installDir, pathWillAdd, dirCreated)) {
                if (pathWillAdd) {
                    _snwprintf_s(outSummary, sumSz, sumSz - 1,
                                 L"已生成,但写入撤销标记失败,为避免不可撤销的 PATH 改动,已跳过自动加 PATH。\n"
                                 L"请手动把该目录加入用户 PATH:\n%s",
                                 installDir);
                    return 1;
                }
                /* 仅 dir-created 而标记写失败: 不阻断安装(空目录残留可接受) */
            }
        }
    }

    /* PATH: 目录已在 PATH 中则无需修改 */
    pathAlready = path_in_environment(installDir);
    if (!pathAlready) {
        wchar_t st[400];
        _snwprintf_s(st, 400, 399, L"追加用户 PATH: %s…", installDir);
        emit_step(onStep, st);
        if (!add_to_user_path(installDir)) {
            _snwprintf_s(outSummary, sumSz, sumSz - 1,
                         L"已生成,但自动加入 PATH 失败。\n请手动把该目录加入用户 PATH:\n%s",
                         installDir);
            return 1;
        }
        if (outAddedPath) *outAddedPath = 1;
    }

    /* CLI 工具: 写入 App Paths 注册表(HKCU),地址栏可用无后缀命令 */
    if (cli && exeBuilt) {
        wchar_t subKey[300];
        HKEY hKey;
        LONG res;

        emit_step(onStep, L"写 App Paths 注册表(地址栏/终端隔离)…");
        _snwprintf_s(subKey, 300, 299,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s.exe", name);
        res = RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
        if (res == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"", 0, REG_SZ, (BYTE*)exePath,
                           (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    }

    /* 冲突检查(.exe 与 .cmd 都要查) */
    _snwprintf_s(cmdline, 1024, 1023, L"%s.exe", name);
    if (find_in_path(cmdline, conflictPath, MAX_PATH) && _wcsicmp(conflictPath, exePath) != 0)
        conflict = 1;
    if (!conflict) {
        _snwprintf_s(cmdline, 1024, 1023, L"%s.cmd", name);
        if (find_in_path(cmdline, conflictPath, MAX_PATH) && _wcsicmp(conflictPath, cmdPath) != 0)
            conflict = 1;
    }

    /* 汇总 */
    const wchar_t *argsHdr = (args && args[0]) ? L"\n固定参数: " : L"";
    const wchar_t *argsVal = (args && args[0]) ? args : L"";
    const wchar_t *urlHdr = (url && url[0]) ? L"\nWeb UI: " : L"";
    const wchar_t *urlVal = (url && url[0]) ? url : L"";
    if (exeBuilt) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"安装完成 ✓\n命令:   %s (.exe%s)\n备用:   %s.cmd\n"
                     L"目录:   %s (%s)\n应用:   %s%s%s%s%s\nlauncher: %s\n\n"
                     L"地址栏/终端输入 \"%s\" 或 \"%s.cmd\" 即可打开。\n"
                     L"注意: 已打开的终端/资源管理器需重启生效。%s",
                     name, cli ? L" + 地址栏注册" : L"", name, installDir,
                     pathAlready ? L"已在 PATH" : L"已加入 PATH",
                     codeExe, argsHdr, argsVal, urlHdr, urlVal, exePath, name, name,
                     conflict ? L"\n\n⚠ 检测到 PATH 中另有同名命令:\n" : L"");
    } else {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"安装完成 ✓ (无 gcc,仅 .cmd)\n命令:   %s.cmd\n"
                     L"目录:   %s (%s)\n应用:   %s%s%s%s%s\n\n"
                     L"终端输入 \"%s\"、地址栏输入 \"%s.cmd\" 即可打开。\n"
                     L"注意: 已打开的终端/资源管理器需重启生效。%s",
                     name, installDir,
                     pathAlready ? L"已在 PATH" : L"已加入 PATH",
                     codeExe, argsHdr, argsVal, urlHdr, urlVal, name, name,
                     conflict ? L"\n\n⚠ 检测到 PATH 中另有同名命令:\n" : L"");
    }
    if (conflict) wcscat_s(outSummary, sumSz, conflictPath);
    if (compileFailed && !exeBuilt)
        wcscat_s(outSummary, sumSz, L"\n\n⚠ .exe 编译失败,当前仅 .cmd 生效。");
    return 0;
}

/* 卸载目标: 按归属删 launcher + 清理 App Paths;最后一个 launcher 时撤销 PATH 追加与目录创建 */
int uninstall_target(const wchar_t *name, const wchar_t *installDir,
                     wchar_t *outSummary, size_t sumSz, StepCb onStep)
{
    wchar_t exePath[MAX_PATH], cmdPath[MAX_PATH], subKey[300];
    BOOL any = FALSE, cmdExists, exeExists, cmdOwned, exeOwned;

    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);

    /* 归属检查: 只动 openin 生成的文件,防误删真实工具 */
    emit_step(onStep, L"检查文件归属…");
    cmdExists = path_exists(cmdPath);
    exeExists = path_exists(exePath);
    cmdOwned = cmd_is_openin(cmdPath);
    exeOwned = exe_is_openin(exePath);

    if (cmdExists && !cmdOwned) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"\"%s\" 不是 openin 生成的(已存在同名 .cmd),拒绝删除:\n%s", name, cmdPath);
        return 1;
    }
    if (exeExists && !cmdExists && !exeOwned) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"\"%s\" 的 .exe 已存在但不是 openin 生成的,拒绝删除:\n%s", name, exePath);
        return 1;
    }

    {
        wchar_t st[400];
        _snwprintf_s(st, 400, 399, L"删除 %s 启动器…", name);
        emit_step(onStep, st);
    }
    if (cmdOwned) { if (DeleteFileW(cmdPath)) any = TRUE; }
    if (exeOwned) { if (DeleteFileW(exePath)) any = TRUE; }

    /* 清理 App Paths 注册表(CLI 目标,失败忽略) */
    emit_step(onStep, L"清理 App Paths 注册表…");
    _snwprintf_s(subKey, 300, 299,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s.exe", name);
    RegDeleteKeyW(HKEY_CURRENT_USER, subKey);

    if (!any) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1, L"\"%s\" 尚未安装,无需卸载。", name);
        return 1;
    }

    _snwprintf_s(outSummary, sumSz, sumSz - 1, L"已卸载 \"%s\"。", name);
    if (cmdOwned && exeExists && !exeOwned) {
        wchar_t warn[640];
        _snwprintf_s(warn, 640, 639, L"\n⚠ 同名 .exe 不是 openin 生成的,已保留:\n%s", exePath);
        wcscat_s(outSummary, sumSz, warn);
    }

    /* 撤销该目录的 PATH 追加与目录创建(仅当已无其他 openin launcher) */
    if (!dir_has_openin_launcher(installDir)) {
        BOOL pathAdded = FALSE, dirCreated = FALSE;
        if (read_undo_marker(installDir, &pathAdded, &dirCreated)) {
            if (dir_effectively_empty(installDir)) {
                if (pathAdded) {
                    wchar_t st[400];
                    _snwprintf_s(st, 400, 399, L"从用户 PATH 移除 %s…", installDir);
                    emit_step(onStep, st);
                    if (remove_from_user_path(installDir))
                        wcscat_s(outSummary, sumSz, L"\n已从用户 PATH 移除安装目录。");
                    else
                        wcscat_s(outSummary, sumSz, L"\n⚠ 从用户 PATH 移除安装目录失败。");
                }
                delete_undo_marker(installDir);
                if (dirCreated) {
                    emit_step(onStep, L"删除 openin 创建的空目录…");
                    if (RemoveDirectoryW(installDir))
                        wcscat_s(outSummary, sumSz, L"\n已删除 openin 创建的空目录。");
                    else
                        wcscat_s(outSummary, sumSz, L"\n⚠ 删除安装目录失败(可能被占用)。");
                }
            } else {
                delete_undo_marker(installDir);
                wcscat_s(outSummary, sumSz, L"\n安装目录非空(有其他文件),PATH 条目与目录已保留。");
            }
        } else {
            wcscat_s(outSummary, sumSz, L"\n未找到撤销标记,目录与 PATH 保留。");
        }
    }
    return 0;
}

/* ---------- 预设模板 ---------- */
const Preset PRESETS[] = {
    { L"vscode",   L"Code.exe",          L"VS Code",         0, 0, NULL, NULL },
    { L"idea",     L"idea64.exe",        L"IntelliJ IDEA",   0, 0, NULL, NULL },
    { L"cursor",   L"Cursor.exe",        L"Cursor",          0, 0, NULL, NULL },
    { L"claude",   L"claude.exe",        L"Claude Code",     1, 1, NULL, NULL },
    { L"codex",    L"codex.cmd",         L"Codex",           1, 0, NULL, NULL },
    { L"opencode", L"opencode.cmd",      L"OpenCode",        1, 0, NULL, NULL },
    /* dsh: 用 npm exec 的 --call 表单规避 npx 直接跑 bin 的 Windows 缺陷;
       固定参数让 launcher 以「当前文件夹」为 cwd 启动 Harness Web UI;
       url 让 launcher 静默后台启动 server 后自动打开浏览器(地址栏不闪窗口) */
    { L"dsh",      L"npm.cmd",           L"DeepSeek Harness", 1, 0,
      L"exec --yes --package=@deepseek-ai/dsh --call \"dsh web\"",
      L"http://127.0.0.1:3080" },
    { L"pycharm",  L"pycharm64.exe",     L"PyCharm",         0, 0, NULL, NULL },
    { L"webstorm", L"webstorm64.exe",    L"WebStorm",        0, 0, NULL, NULL },
    { L"goland",   L"goland64.exe",      L"GoLand",          0, 0, NULL, NULL },
    { L"rider",    L"rider64.exe",       L"Rider",           0, 0, NULL, NULL },
    { L"datagrip", L"datagrip64.exe",    L"DataGrip",        0, 0, NULL, NULL },
    { L"clion",    L"clion64.exe",       L"CLion",           0, 0, NULL, NULL },
    { L"rustrover",L"rustrover64.exe",   L"RustRover",       0, 0, NULL, NULL },
    { L"fleet",    L"Fleet.exe",         L"JetBrains Fleet", 0, 0, NULL, NULL },
    { L"sublime",  L"sublime_text.exe",  L"Sublime Text",    0, 0, NULL, NULL },
    { L"zed",      L"zed.exe",           L"Zed Editor",      0, 0, NULL, NULL },
    { L"neovide",  L"neovide.exe",       L"Neovide",         0, 0, NULL, NULL },
    { L"windsurf", L"Windsurf.exe",      L"Windsurf",        0, 0, NULL, NULL },
};

int preset_count(void) { return (int)(sizeof(PRESETS) / sizeof(PRESETS[0])); }
