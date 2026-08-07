/*
 * vscode-installer.c — 通用 VS Code 启动器安装器
 *
 * 运行方式:
 *   vscode-installer.exe                    弹出文件夹选择框,默认命令名 vscode
 *   vscode-installer.exe code               弹出文件夹选择框,命令名 code
 *   vscode-installer.exe -d "D:\VS Code"    静默指定目录(命令行模式)
 *   vscode-installer.exe -d "D:\VS Code" code   同时指定目录和命令名
 *
 * 流程:
 *   选择 VS Code 目录 → 定位 Code.exe → 生成 launcher 源码(内嵌路径)
 *   → 用 gcc 编译成 <name>.exe → 写入 %LOCALAPPDATA%\Programs\vscode-launcher\
 *   → 自动把该目录加入用户 PATH(幂等) → 输出汇总
 *
 * 说明:
 *   - 只改 HKCU 的 Path,不需要管理员权限
 *   - -q 静默模式: 不弹 MessageBox,结果写入 %LOCALAPPDATA%\vscode-installer.log
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

static int g_quiet = 0;
static wchar_t g_logpath[MAX_PATH];
static const wchar_t *g_logfile = NULL;

/* ---------- 输出(静默模式下写日志) ---------- */
static void show(const wchar_t *title, const wchar_t *text, UINT flags)
{
    if (!g_quiet) {
        MessageBoxW(NULL, text, title, flags);
        return;
    }
    HANDLE h = CreateFileW(g_logfile, FILE_APPEND_DATA, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    {
        wchar_t tmp[5200];
        int n = _snwprintf_s(tmp, 5200, 5199, L"[%s] %s\n", title, text);
        if (n > 0) {
            char *u8 = (char *)malloc(n * 3 + 8);
            if (u8) {
                int blen = WideCharToMultiByte(CP_UTF8, 0, tmp, n, u8, n * 3, NULL, NULL);
                DWORD w = 0;
                WriteFile(h, u8, (DWORD)blen, &w, NULL);
                free(u8);
            }
        }
        CloseHandle(h);
    }
}

/* ---------- 文件夹选择 ---------- */
static BOOL browse_for_folder(wchar_t *out, size_t out_sz)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = L"请选择 VS Code 安装目录 (包含 Code.exe 的文件夹)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return FALSE;
    BOOL ok = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return ok && out[0];
}

static BOOL path_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 在 folder 中找 Code.exe;找不到则向上一级再找(兼容选中 bin 子目录的情况) */
static BOOL locate_code_exe(const wchar_t *folder, wchar_t *out, size_t out_sz)
{
    wchar_t cand[MAX_PATH];
    wchar_t parent[MAX_PATH];
    wchar_t *slash;

    _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\Code.exe", folder);
    if (path_exists(cand)) {
        wcscpy_s(out, out_sz, cand);
        return TRUE;
    }
    wcscpy_s(parent, MAX_PATH, folder);
    slash = wcsrchr(parent, L'\\');
    if (slash) *slash = L'\0';
    if (slash && parent[0]) {
        _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\Code.exe", parent);
        if (path_exists(cand)) {
            wcscpy_s(out, out_sz, cand);
            return TRUE;
        }
    }
    out[0] = L'\0';
    return FALSE;
}

/* ---------- 用户 PATH 操作 ---------- */
static BOOL path_value_contains(const wchar_t *value, const wchar_t *dir)
{
    const wchar_t *p = value;
    while (*p) {
        const wchar_t *end = wcschr(p, L';');
        size_t len = end ? (size_t)(end - p) : wcslen(p);
        wchar_t entry[MAX_PATH];
        wchar_t dirmod[MAX_PATH];
        size_t i, elen;

        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (i = 0; i < len; i++) entry[i] = p[i];
        entry[len] = L'\0';

        elen = wcslen(entry);
        while (elen > 0 && entry[elen - 1] == L'\\') entry[--elen] = L'\0';

        wcscpy_s(dirmod, MAX_PATH, dir);
        elen = wcslen(dirmod);
        while (elen > 0 && dirmod[elen - 1] == L'\\') dirmod[--elen] = L'\0';

        if (entry[0] && _wcsicmp(entry, dirmod) == 0) return TRUE;

        if (!end) break;
        p = end + 1;
    }
    return FALSE;
}

static BOOL add_to_user_path(const wchar_t *dir)
{
    DWORD type = REG_EXPAND_SZ, size = 0;
    wchar_t *val = NULL, *newval = NULL;
    size_t oldlen, dirlen;
    BOOL ok = FALSE;
    LONG r;
    HKEY hk;

    r = RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, NULL, &size);
    if (r == ERROR_SUCCESS && size > 0) {
        val = (wchar_t *)malloc(size + sizeof(wchar_t));
        if (!val) return FALSE;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path",
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         &type, val, &size) != ERROR_SUCCESS) {
            free(val);
            return FALSE;
        }
        val[size / sizeof(wchar_t)] = L'\0';
    } else if (r != ERROR_FILE_NOT_FOUND) {
        return FALSE;
    }

    if (val && path_value_contains(val, dir)) {           /* 已在 PATH 中 */
        free(val);
        return TRUE;
    }

    oldlen = val ? wcslen(val) : 0;
    dirlen = wcslen(dir);
    newval = (wchar_t *)malloc((oldlen + dirlen + 2) * sizeof(wchar_t));
    if (!newval) { free(val); return FALSE; }

    if (val && oldlen) {
        memcpy(newval, val, oldlen * sizeof(wchar_t));
        newval[oldlen] = L';';
        oldlen++;
    }
    wcscpy_s(newval + oldlen, dirlen + 1, dir);           /* 尾部追加 dir */
    free(val);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                      KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        if (RegSetValueExW(hk, L"Path", 0, type,
                           (const BYTE *)newval,
                           (DWORD)((wcslen(newval) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
            ok = TRUE;
        RegCloseKey(hk);
    }
    free(newval);

    /* 广播环境变更,让 Explorer 和已打开的资源管理器刷新 */
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    return ok;
}

/* ---------- 安装目录选择 ---------- */
/* 目录存在且可写? */
static BOOL dir_is_writable(const wchar_t *dir)
{
    DWORD attrs = GetFileAttributesW(dir);
    wchar_t probe[MAX_PATH];
    HANDLE h;

    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return FALSE;
    _snwprintf_s(probe, MAX_PATH, MAX_PATH - 1, L"%s\\.wtest-%lu", dir,
                 (unsigned long)GetCurrentProcessId());
    h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    DeleteFileW(probe);
    return TRUE;
}

/* 目录是否已存在于当前环境 PATH(合并了用户+系统) */
static BOOL path_in_environment(const wchar_t *dir)
{
    DWORD len = GetEnvironmentVariableW(L"Path", NULL, 0);
    wchar_t *val;
    BOOL f;

    if (!len) return FALSE;
    val = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!val) return FALSE;
    GetEnvironmentVariableW(L"Path", val, len + 1);
    f = path_value_contains(val, dir);
    free(val);
    return f;
}

/*
 * 选择安装目录(写入 out),优先级:
 *   1. -p 覆盖参数
 *   2. %USERPROFILE%\.local\bin   (GitHub 风格用户 bin,claude 也在此,通常已在 PATH)
 *   3. %APPDATA%\npm              (Node 全局工具目录,codex 等在此)
 *   4. 创建 %USERPROFILE%\.local\bin
 *   5. 回退: 创建 %LOCALAPPDATA%\Programs\vscode-launcher
 */
static void pick_target_dir(const wchar_t *override, wchar_t *out, size_t out_sz)
{
    wchar_t tmp[MAX_PATH];
    wchar_t p[MAX_PATH];
    DWORD n;

    if (override && override[0]) {                    /* 1. 显式指定 */
        wcscpy_s(out, out_sz, override);
        return;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", tmp, MAX_PATH);  /* 2. ~\.local\bin */
    if (n && n < MAX_PATH) {
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\.local\\bin", tmp);
        if (dir_is_writable(out)) return;
    }
    n = GetEnvironmentVariableW(L"APPDATA", tmp, MAX_PATH);      /* 3. %APPDATA%\npm */
    if (n && n < MAX_PATH) {
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\npm", tmp);
        if (dir_is_writable(out)) return;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", tmp, MAX_PATH);  /* 4. 创建 ~\.local\bin */
    if (n && n < MAX_PATH) {
        _snwprintf_s(p, MAX_PATH, MAX_PATH - 1, L"%s\\.local", tmp);
        CreateDirectoryW(p, NULL);
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\bin", p);
        CreateDirectoryW(out, NULL);
        if (dir_is_writable(out)) return;
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", tmp, MAX_PATH); /* 5. 回退专属目录 */
    if (n && n < MAX_PATH) {
        _snwprintf_s(p, MAX_PATH, MAX_PATH - 1, L"%s\\Programs", tmp);
        CreateDirectoryW(p, NULL);
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\vscode-launcher", p);
        CreateDirectoryW(out, NULL);
        return;
    }
    wcscpy_s(out, out_sz, L"C:\\");
}

/* ---------- 编译 ---------- */
static BOOL find_in_path(const wchar_t *file, wchar_t *out, size_t out_sz)
{
    DWORD len = GetEnvironmentVariableW(L"Path", NULL, 0);
    wchar_t *path, *p;
    BOOL found = FALSE;

    if (!len) return FALSE;
    path = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!path) return FALSE;
    GetEnvironmentVariableW(L"Path", path, len + 1);

    p = path;
    while (*p) {
        const wchar_t *end = wcschr(p, L';');
        size_t dlen = end ? (size_t)(end - p) : wcslen(p);
        wchar_t dir[MAX_PATH], cand[MAX_PATH];
        DWORD attrs;

        if (dlen >= MAX_PATH) dlen = MAX_PATH - 1;
        memcpy(dir, p, dlen * sizeof(wchar_t));
        dir[dlen] = L'\0';

        _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\%s", dir, file);
        attrs = GetFileAttributesW(cand);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            wcscpy_s(out, out_sz, cand);
            found = TRUE;
            break;
        }
        if (!end) break;
        p = end + 1;
    }
    free(path);
    return found;
}

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

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, (wchar_t *)cmdline, NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        ok = (code == 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
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

/* ---------- 生成 launcher 源码 ---------- */
/*
 * 生成的 launcher: 启动 Code.exe 打开「当前工作目录」或命令行参数指定路径。
 * 模板按行存放,@@PATH@@ 处替换为转义后的 Code.exe 完整路径。
 * 使用 wcscat 拼接命令行以避免引号嵌套带来的格式转义问题。
 */
static BOOL write_launcher_source(const wchar_t *codeExe, const wchar_t *srcPath)
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
        L"        MessageBoxW(NULL, msg, L\"vscode\", MB_OK | MB_ICONERROR);",
        L"        return 1;",
        L"    }",
        L"    CloseHandle(pi.hThread);",
        L"    CloseHandle(pi.hProcess);",
        L"    return 0;",
        L"}",
        L""
    };
    enum { CAP = 16384 };
    wchar_t *src;
    size_t pos = 0, t;
    int u8len;
    char *u8;
    HANDLE h;
    DWORD written;

    src = (wchar_t *)malloc(CAP * sizeof(wchar_t));
    if (!src) return FALSE;

    for (t = 0; t < sizeof(TPL) / sizeof(TPL[0]); t++) {
        const wchar_t *line = TPL[t];
        const wchar_t *mark = wcsstr(line, L"@@PATH@@");
        size_t l = wcslen(line);

        if (mark) {
            size_t pre = (size_t)(mark - line);
            if (pos + l + 2000 >= CAP) { free(src); return FALSE; }
            memcpy(src + pos, line, pre * sizeof(wchar_t));
            pos += pre;
            for (const wchar_t *cp = codeExe; *cp; cp++) {
                if (*cp == L'\\' || *cp == L'"') src[pos++] = L'\\';
                src[pos++] = *cp;
            }
            memcpy(src + pos, mark + 8, (l - pre - 8) * sizeof(wchar_t));
            pos += l - pre - 8;
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

static BOOL write_launcher_cmd(const wchar_t *codeExe, const wchar_t *cmdPath)
{
    wchar_t esc[2 * MAX_PATH + 16];
    wchar_t buf[3 * MAX_PATH + 512];
    char *ansi;
    int alen;
    HANDLE h;
    DWORD written;

    cmd_escape(codeExe, esc, 2 * MAX_PATH + 16);

    _snwprintf_s(buf, 3 * MAX_PATH + 512, 3 * MAX_PATH + 512 - 1,
        L"@echo off\r\n"
        L"rem vscode launcher - generated by vscode-installer\r\n"
        L"if \"%%~1\"==\"\" (\r\n"
        L"    start \"\" \"%s\" \"%%CD%%\" %%2 %%3 %%4 %%5 %%6 %%7 %%8 %%9\r\n"
        L") else (\r\n"
        L"    start \"\" \"%s\" \"%%~1\" %%2 %%3 %%4 %%5 %%6 %%7 %%8 %%9\r\n"
        L")\r\n",
        esc, esc);

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

/* ---------- 命令名校验 ---------- */
static int valid_name(const wchar_t *s)
{
    const wchar_t *c;
    if (!s || !*s) return 0;
    for (c = s; *c; c++)
        if (!((*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') ||
              (*c >= L'0' && *c <= L'9') || *c == L'_' || *c == L'-'))
            return 0;
    return 1;
}

/* ---------- 主流程 ---------- */
int wmain(int argc, wchar_t *argv[])
{
    wchar_t name[64];
    wchar_t folder[MAX_PATH];
    wchar_t codeExe[MAX_PATH];
    wchar_t installDir[MAX_PATH];
    wchar_t srcPath[MAX_PATH], exePath[MAX_PATH], cmdPath[MAX_PATH], logPath[MAX_PATH];
    wchar_t cmdline[1024];
    wchar_t logBuf[4096];
    wchar_t buf[4096];
    wchar_t conflictPath[MAX_PATH], gccPath[MAX_PATH];
    wchar_t localApp[MAX_PATH];
    wchar_t targetOverride[MAX_PATH];
    int quiet = 0, i, conflict = 0, exeBuilt = 0, pathAlready = 0;

    /* 解析参数 */
    wcscpy_s(name, 64, L"vscode");
    folder[0] = L'\0';
    targetOverride[0] = L'\0';
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-q") == 0) { quiet = 1; continue; }
        if (_wcsicmp(argv[i], L"-d") == 0 && i + 1 < argc) {
            wcscpy_s(folder, MAX_PATH, argv[++i]);
            continue;
        }
        if (_wcsicmp(argv[i], L"-p") == 0 && i + 1 < argc) {
            wcscpy_s(targetOverride, MAX_PATH, argv[++i]);
            continue;
        }
        /* 其它参数视为命令名 */
        if (valid_name(argv[i]))
            wcscpy_s(name, 64, argv[i]);
        else
            show(L"vscode-installer",
                 L"命令名只能包含字母、数字、下划线和短横线,已回退为默认名 \"vscode\"。",
                 MB_OK | MB_ICONWARNING);
    }
    g_quiet = quiet;
    if (quiet) {
        if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL,
                             SHGFP_TYPE_CURRENT, localApp) == S_OK) {
            _snwprintf_s(g_logpath, MAX_PATH, MAX_PATH - 1,
                         L"%s\\vscode-installer.log", localApp);
            DeleteFileW(g_logpath);
            g_logfile = g_logpath;
        }
    }

    /* 选择 VS Code 目录 */
    if (!folder[0]) {
        if (!browse_for_folder(folder, MAX_PATH)) return 0;
    }

    /* 定位 Code.exe */
    if (!locate_code_exe(folder, codeExe, MAX_PATH)) {
        _snwprintf_s(buf, 4096, 4095,
                     L"在所选目录中未找到 Code.exe:\n%s\n\n请确认你选择的是 VS Code 的安装目录。",
                     folder);
        show(L"vscode-installer", buf, MB_OK | MB_ICONERROR);
        return 1;
    }

    /* 选择安装目录(自动: ~\.local\bin > %APPDATA%\npm > 创建 > 回退) */
    pick_target_dir(targetOverride, installDir, MAX_PATH);

    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);
    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    {
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);   /* .c 源码与编译日志都放临时目录,目标目录零残留 */
        _snwprintf_s(srcPath, MAX_PATH, MAX_PATH - 1, L"%s%s.c", tempDir, name);
        _snwprintf_s(logPath, MAX_PATH, MAX_PATH - 1, L"%s%s-build.log", tempDir, name);
    }

    /* 先写 .cmd 备用启动器(无需编译器,始终生成,保证冗余) */
    if (!write_launcher_cmd(codeExe, cmdPath)) {
        show(L"vscode-installer", L"生成 .cmd 备用启动器失败。", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* 尝试用 gcc 编译 .exe(可选;无 gcc 则仅保留 .cmd) */
    if (find_in_path(L"gcc.exe", gccPath, MAX_PATH)) {
        if (write_launcher_source(codeExe, srcPath)) {
            _snwprintf_s(cmdline, 1024, 1023,
                         L"gcc -O2 -s -municode -mwindows -o \"%s\" \"%s\"", exePath, srcPath);
            if (run_gcc(cmdline, logPath, logBuf, 4096)) {
                exeBuilt = 1;
            } else {
                _snwprintf_s(buf, 4096, 4095,
                             L"编译 .exe 失败(已保留 .cmd 备用)。\n\n%s",
                             logBuf[0] ? logBuf : L"(无输出)");
                show(L"vscode-installer", buf, MB_OK | MB_ICONWARNING);
            }
        } else {
            show(L"vscode-installer", L"生成 C 源码失败(已保留 .cmd 备用)。",
                 MB_OK | MB_ICONWARNING);
        }
        /* 临时源码与编译日志用完即删,目标目录零残留 */
        DeleteFileW(srcPath);
        DeleteFileW(logPath);
    } else {
        _snwprintf_s(buf, 4096, 4095,
                     L"未在 PATH 中找到 gcc.exe,已生成 .cmd 备用启动器(跳过 .exe 编译)。\n\n"
                     L"提示: 终端里输入 \"%s\"、资源管理器地址栏输入 \"%s.cmd\" 均可打开 VS Code。",
                     name, name);
        show(L"vscode-installer", buf, MB_OK | MB_ICONINFORMATION);
    }

    /* PATH: 目标目录已在 PATH 中则无需修改 */
    pathAlready = path_in_environment(installDir);
    if (!pathAlready && !add_to_user_path(installDir)) {
        _snwprintf_s(buf, 4096, 4095,
                     L"已生成,但自动加入 PATH 失败。\n\n"
                     L"请手动把以下目录加入「用户环境变量 → Path」:\n\n%s",
                     installDir);
        show(L"vscode-installer", buf, MB_OK | MB_ICONWARNING);
        return 1;
    }

    /* 冲突检查(.exe 与 .cmd 都要查) */
    _snwprintf_s(buf, 4096, 4095, L"%s.exe", name);
    if (find_in_path(buf, conflictPath, MAX_PATH) && _wcsicmp(conflictPath, exePath) != 0)
        conflict = 1;
    if (!conflict) {
        _snwprintf_s(buf, 4096, 4095, L"%s.cmd", name);
        if (find_in_path(buf, conflictPath, MAX_PATH) && _wcsicmp(conflictPath, cmdPath) != 0)
            conflict = 1;
    }

    /* 汇总 */
    if (exeBuilt) {
        _snwprintf_s(buf, 4096, 4095,
                     L"安装完成 ✓\n\n"
                     L"命令:   %s (.exe)\n"
                     L"备用:   %s.cmd\n"
                     L"目录:   %s (%s)\n"
                     L"VS Code: %s\n"
                     L"launcher: %s\n\n"
                     L"地址栏/终端输入 \"%s\" 或 \"%s.cmd\" 均可打开 VS Code。\n"
                     L"注意: 已打开的终端/资源管理器窗口需重启后才会生效。%s",
                     name, name, installDir,
                     pathAlready ? L"已在 PATH" : L"已加入 PATH",
                     codeExe, exePath, name, name,
                     conflict ? L"\n\n⚠ 检测到 PATH 中另有同名命令,当前仍会优先解析到:\n" : L"");
    } else {
        _snwprintf_s(buf, 4096, 4095,
                     L"安装完成 ✓ (无 gcc,仅 .cmd 模式)\n\n"
                     L"命令:   %s.cmd\n"
                     L"目录:   %s (%s)\n"
                     L"VS Code: %s\n\n"
                     L"终端里输入 \"%s\"、资源管理器地址栏输入 \"%s.cmd\" 均可打开 VS Code。\n"
                     L"注意: 已打开的终端/资源管理器窗口需重启后才会生效。%s",
                     name, installDir,
                     pathAlready ? L"已在 PATH" : L"已加入 PATH",
                     codeExe, name, name,
                     conflict ? L"\n\n⚠ 检测到 PATH 中另有同名命令,当前仍会优先解析到:\n" : L"");
    }
    if (conflict) wcscat_s(buf, 4096, conflictPath);
    show(L"vscode-installer", buf, MB_OK | MB_ICONINFORMATION);
    return 0;
}
