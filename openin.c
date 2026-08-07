/*
 * openin.c — 通用「打开到应用」启动器安装器
 *
 * 把任意工具注入地址栏: 选择一个应用(如 VS Code),安装后即可在资源管理器
 * 地址栏或终端输入 <命令名> 以当前文件夹为参数打开它。
 *
 * 运行方式:
 *   openin.exe                    弹出 GUI 主窗口
 *   openin.exe -d "D:\VS Code"    静默指定目录(命令行安装,默认命令名 vscode)
 *   openin.exe -d "D:\VS Code" code   同时指定目录和命令名
 *   openin.exe -p C:\Users\you\bin    指定安装目录
 *   openin.exe -u vscode          卸载指定命令
 *
 * 流程:
 *   选择应用目录 → 定位主程序(如 Code.exe) → 生成 launcher 源码(内嵌路径)
 *   → 用 gcc 编译成 <name>.exe → 写入通用用户 bin 目录(如 ~/.local\bin)
 *   → 目录已在 PATH 则不动环境变量,否则自动加入用户 PATH(幂等) → 输出汇总
 *
 * 说明:
 *   - 只改 HKCU 的 Path,不需要管理员权限
 *   - -q 静默模式: 不弹 MessageBox,结果写入 %LOCALAPPDATA%\openin.log
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
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
    bi.lpszTitle = L"请选择应用安装目录 (包含主程序 exe 的文件夹)";
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

/* 在 folder 中找主程序 exeName;找不到则向上一级再找(兼容选中 bin 子目录的情况) */
static BOOL locate_main_exe(const wchar_t *folder, const wchar_t *exeName,
                            wchar_t *out, size_t out_sz)
{
    wchar_t cand[MAX_PATH];
    wchar_t parent[MAX_PATH];
    wchar_t *slash;

    _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\%s", folder, exeName);
    if (path_exists(cand)) {
        wcscpy_s(out, out_sz, cand);
        return TRUE;
    }
    wcscpy_s(parent, MAX_PATH, folder);
    slash = wcsrchr(parent, L'\\');
    if (slash) *slash = L'\0';
    if (slash && parent[0]) {
        _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\%s", parent, exeName);
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
 *   5. 回退: 创建 %LOCALAPPDATA%\Programs\openin
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
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\openin", p);
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
 * 生成的 launcher: 启动主程序打开「当前工作目录」或命令行参数指定路径。
 * 模板按行存放,@@PATH@@ 处替换为转义后的主程序完整路径,@@NAME@@ 为命令名。
 * 使用 wcscat 拼接命令行以避免引号嵌套带来的格式转义问题。
 */
static BOOL write_launcher_source(const wchar_t *codeExe, const wchar_t *name,
                                  const wchar_t *srcPath)
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
        const wchar_t *pmark = wcsstr(line, L"@@PATH@@");
        const wchar_t *nmark = wcsstr(line, L"@@NAME@@");
        const wchar_t *mark = pmark;
        const wchar_t *subst = codeExe;
        size_t mlen = 8;
        size_t l = wcslen(line);

        if (nmark && (!pmark || nmark < pmark)) {
            mark = nmark;
            subst = name;
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
        L"rem openin launcher - generated by openin\r\n"
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

/* ---------- 从用户 PATH 移除目录 ---------- */
static BOOL remove_from_user_path(const wchar_t *dir)
{
    DWORD type = REG_EXPAND_SZ, size = 0;
    wchar_t *val = NULL, *newval = NULL;
    BOOL ok = FALSE;
    LONG r;
    HKEY hk;

    r = RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, NULL, &size);
    if (r != ERROR_SUCCESS || size == 0) return TRUE;   /* 没有 PATH 可改 */
    val = (wchar_t *)malloc(size + sizeof(wchar_t));
    if (!val) return FALSE;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     &type, val, &size) != ERROR_SUCCESS) {
        free(val);
        return FALSE;
    }
    val[size / sizeof(wchar_t)] = L'\0';

    if (!path_value_contains(val, dir)) { free(val); return TRUE; }

    {
        const wchar_t *p = val;
        size_t cap = wcslen(val) + 2;
        newval = (wchar_t *)calloc(cap, sizeof(wchar_t));
        if (!newval) { free(val); return FALSE; }
        while (*p) {
            const wchar_t *end = wcschr(p, L';');
            size_t len = end ? (size_t)(end - p) : wcslen(p);
            wchar_t entry[MAX_PATH], dirmod[MAX_PATH];
            size_t i, elen;

            if (len >= MAX_PATH) len = MAX_PATH - 1;
            for (i = 0; i < len; i++) entry[i] = p[i];
            entry[len] = L'\0';
            elen = wcslen(entry);
            while (elen > 0 && entry[elen - 1] == L'\\') entry[--elen] = L'\0';

            wcscpy_s(dirmod, MAX_PATH, dir);
            elen = wcslen(dirmod);
            while (elen > 0 && dirmod[elen - 1] == L'\\') dirmod[--elen] = L'\0';

            if (!(entry[0] && _wcsicmp(entry, dirmod) == 0)) {
                if (newval[0]) wcscat_s(newval, cap, L";");
                wcscat_s(newval, cap, entry);
            }
            if (!end) break;
            p = end + 1;
        }
    }
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

    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    return ok;
}

/* ---------- 配置 (targets.ini: install_dir + added_path + 目标 map) ---------- */
#define MAX_TARGETS 64
static wchar_t g_installDir[MAX_PATH];
static int g_addedPath = 0;
static int g_createdDir = 0;   /* 安装目录是否为 openin 新建 */
typedef struct { wchar_t name[64]; wchar_t exePath[MAX_PATH]; } Target;
static Target g_targets[MAX_TARGETS];
static int g_targetCount = 0;

static void get_config_dir(wchar_t *out, size_t sz)
{
    wchar_t la[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL,
                         SHGFP_TYPE_CURRENT, la) == S_OK)
        _snwprintf_s(out, sz, sz - 1, L"%s\\openin", la);
    else
        wcscpy_s(out, sz, L"C:\\");
}

static void load_config(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    FILE *f;
    char buf[1024];

    g_targetCount = 0;
    g_installDir[0] = L'\0';
    g_addedPath = 0;
    g_createdDir = 0;

    get_config_dir(dir, MAX_PATH);
    _snwprintf_s(path, MAX_PATH, MAX_PATH - 1, L"%s\\targets.ini", dir);
    f = _wfopen(path, L"rb");
    if (!f) return;
    {
        int section = 0;
        while (fgets(buf, sizeof(buf), f)) {
            wchar_t line[1024];
            int n = MultiByteToWideChar(CP_UTF8, 0, buf, -1, line, 1024);
            if (n <= 0) continue;
            {
                size_t l = wcslen(line);
                while (l > 0 && (line[l-1] == L'\n' || line[l-1] == L'\r')) line[--l] = L'\0';
            }
            if (line[0] == L'[') {
                section = (_wcsicmp(line, L"[openin]") == 0) ? 1 :
                          (_wcsicmp(line, L"[targets]") == 0) ? 2 : 0;
                continue;
            }
            if (section == 1) {
                if (_wcsnicmp(line, L"install_dir=", 12) == 0)
                    wcscpy_s(g_installDir, MAX_PATH, line + 12);
                else if (_wcsnicmp(line, L"added_path=", 11) == 0)
                    g_addedPath = _wtoi(line + 11);
                else if (_wcsnicmp(line, L"created_dir=", 12) == 0)
                    g_createdDir = _wtoi(line + 12);
            } else if (section == 2 && g_targetCount < MAX_TARGETS) {
                wchar_t *eq = wcschr(line, L'=');
                if (eq && eq > line) {
                    *eq = L'\0';
                    wcscpy_s(g_targets[g_targetCount].name, 64, line);
                    wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, eq + 1);
                    g_targetCount++;
                }
            }
        }
        fclose(f);
    }
}

static void save_config(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    wchar_t *buf;
    char *u8;
    int n, pos = 0;
    FILE *f;
    enum { CAP = 40000 };

    get_config_dir(dir, MAX_PATH);
    CreateDirectoryW(dir, NULL);
    _snwprintf_s(path, MAX_PATH, MAX_PATH - 1, L"%s\\targets.ini", dir);

    buf = (wchar_t *)malloc(CAP * sizeof(wchar_t));
    if (!buf) return;
    pos += _snwprintf_s(buf + pos, CAP - pos, CAP - pos - 1,
                        L"[openin]\r\ninstall_dir=%s\r\nadded_path=%d\r\ncreated_dir=%d\r\n",
                        g_installDir[0] ? g_installDir : L"", g_addedPath, g_createdDir);
    if (g_targetCount > 0) {
        pos += _snwprintf_s(buf + pos, CAP - pos, CAP - pos - 1, L"[targets]\r\n");
        for (int i = 0; i < g_targetCount && pos < CAP - 600; i++)
            pos += _snwprintf_s(buf + pos, CAP - pos, CAP - pos - 1,
                                L"%s=%s\r\n", g_targets[i].name, g_targets[i].exePath);
    }

    n = WideCharToMultiByte(CP_UTF8, 0, buf, pos, NULL, 0, NULL, NULL);
    if (n > 0) {
        u8 = (char *)malloc((size_t)n + 1);
        if (u8) {
            WideCharToMultiByte(CP_UTF8, 0, buf, pos, u8, n, NULL, NULL);
            u8[n] = '\0';
            f = _wfopen(path, L"wb");
            if (f) { fwrite(u8, 1, (size_t)n, f); fclose(f); }
            free(u8);
        }
    }
    free(buf);
}

static int find_target(const wchar_t *name)
{
    for (int i = 0; i < g_targetCount; i++)
        if (_wcsicmp(g_targets[i].name, name) == 0) return i;
    return -1;
}

static void remove_target_entry(const wchar_t *name)
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
 */
static int install_target(const wchar_t *name, const wchar_t *codeExe,
                          const wchar_t *installDir,
                          wchar_t *outSummary, size_t sumSz, int *outAddedPath)
{
    wchar_t cmdPath[MAX_PATH], exePath[MAX_PATH], srcPath[MAX_PATH], logPath[MAX_PATH];
    wchar_t tempDir[MAX_PATH];
    wchar_t cmdline[1024], logBuf[4096];
    wchar_t conflictPath[MAX_PATH], gccPath[MAX_PATH];
    int exeBuilt = 0, pathAlready = 0, conflict = 0, compileFailed = 0;

    if (outAddedPath) *outAddedPath = 0;

    /* 确保安装目录存在(-p 可能指向尚未创建的目录) */
    {
        DWORD a = GetFileAttributesW(installDir);
        if (a == INVALID_FILE_ATTRIBUTES)
            g_createdDir = CreateDirectoryW(installDir, NULL) ? 1 : 0;
        else
            g_createdDir = 0;   /* 目录原本已存在 */
    }

    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);
    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    GetTempPathW(MAX_PATH, tempDir);
    _snwprintf_s(srcPath, MAX_PATH, MAX_PATH - 1, L"%s%s.c", tempDir, name);
    _snwprintf_s(logPath, MAX_PATH, MAX_PATH - 1, L"%s%s-build.log", tempDir, name);

    /* .cmd 备用启动器(无需编译器,始终生成) */
    if (!write_launcher_cmd(codeExe, cmdPath)) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"生成 .cmd 备用启动器失败:\n%s", cmdPath);
        return 1;
    }

    /* 有 gcc 则编译 .exe(可选) */
    if (find_in_path(L"gcc.exe", gccPath, MAX_PATH)) {
        if (write_launcher_source(codeExe, name, srcPath)) {
            _snwprintf_s(cmdline, 1024, 1023,
                         L"gcc -O2 -s -municode -mwindows -o \"%s\" \"%s\"", exePath, srcPath);
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

    /* PATH: 目录已在 PATH 中则无需修改 */
    pathAlready = path_in_environment(installDir);
    if (!pathAlready) {
        if (!add_to_user_path(installDir)) {
            _snwprintf_s(outSummary, sumSz, sumSz - 1,
                         L"已生成,但自动加入 PATH 失败。\n请手动把该目录加入用户 PATH:\n%s",
                         installDir);
            return 1;
        }
        if (outAddedPath) *outAddedPath = 1;
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
    if (exeBuilt) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"安装完成 ✓\n命令:   %s (.exe)\n备用:   %s.cmd\n"
                     L"目录:   %s (%s)\n应用:   %s\nlauncher: %s\n\n"
                     L"地址栏/终端输入 \"%s\" 或 \"%s.cmd\" 即可打开。\n"
                     L"注意: 已打开的终端/资源管理器需重启生效。%s",
                     name, name, installDir,
                     pathAlready ? L"已在 PATH" : L"已加入 PATH",
                     codeExe, exePath, name, name,
                     conflict ? L"\n\n⚠ 检测到 PATH 中另有同名命令:\n" : L"");
    } else {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"安装完成 ✓ (无 gcc,仅 .cmd)\n命令:   %s.cmd\n"
                     L"目录:   %s (%s)\n应用:   %s\n\n"
                     L"终端输入 \"%s\"、地址栏输入 \"%s.cmd\" 即可打开。\n"
                     L"注意: 已打开的终端/资源管理器需重启生效。%s",
                     name, installDir,
                     pathAlready ? L"已在 PATH" : L"已加入 PATH",
                     codeExe, name, name,
                     conflict ? L"\n\n⚠ 检测到 PATH 中另有同名命令:\n" : L"");
    }
    if (conflict) wcscat_s(outSummary, sumSz, conflictPath);
    if (compileFailed && !exeBuilt)
        wcscat_s(outSummary, sumSz, L"\n\n⚠ .exe 编译失败,当前仅 .cmd 生效。");
    return 0;
}

/* 卸载目标: 删除 launcher 文件;若 openin 曾加入 PATH 且目录已空,移除 PATH 条目 */
static int uninstall_target(const wchar_t *name, const wchar_t *installDir,
                            wchar_t *outSummary, size_t sumSz)
{
    wchar_t exePath[MAX_PATH], cmdPath[MAX_PATH];
    BOOL any = FALSE;

    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);
    if (DeleteFileW(exePath)) any = TRUE;
    if (DeleteFileW(cmdPath)) any = TRUE;

    if (!any) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1, L"\"%s\" 尚未安装,无需卸载。", name);
        return 1;
    }

    /* PATH / 目录清理: openin 曾加入 或 曾创建 且 目录已空 → 还原 */
    {
        BOOL empty = TRUE;
        if (g_addedPath || g_createdDir) {
            wchar_t pat[MAX_PATH];
            WIN32_FIND_DATAW fd;
            HANDLE hf;
            _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s\\*", installDir);
            hf = FindFirstFileW(pat, &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                        empty = FALSE;
                        break;
                    }
                } while (FindNextFileW(hf, &fd));
                FindClose(hf);
            }
        }
        if (empty) {
            if (g_addedPath) {
                remove_from_user_path(installDir);
                g_addedPath = 0;
            }
            if (g_createdDir) {
                RemoveDirectoryW(installDir);
                g_createdDir = 0;
            }
        }
    }

    _snwprintf_s(outSummary, sumSz, sumSz - 1, L"已卸载 \"%s\"。", name);
    return 0;
}

/* ---------- 预设模板 ---------- */
typedef struct {
    const wchar_t *name;     /* 命令名 */
    const wchar_t *exeName;  /* 主程序文件名,用于目录定位 */
    const wchar_t *display;  /* 界面显示名 */
} Preset;
static const Preset PRESETS[] = {
    { L"vscode", L"Code.exe", L"VS Code" },
    /* 后续在此追加: cursor/Cursor.exe, claude/claude.exe, codex/codex.exe ... */
};

/* ---------- GUI ---------- */
/* 自动检索: 轻量扫描常用根 + 盘符根,首个命中即停 */
static BOOL search_tree(const wchar_t *root, const wchar_t *exeName,
                        int depth, int maxDepth, wchar_t *out)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hf;

    if (out[0]) return TRUE;
    if (depth > maxDepth) return FALSE;
    _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s\\*", root);
    hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (out[0]) break;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t sub[MAX_PATH];
            _snwprintf_s(sub, MAX_PATH, MAX_PATH - 1, L"%s\\%s", root, fd.cFileName);
            search_tree(sub, exeName, depth + 1, maxDepth, out);
        } else {
            if (_wcsicmp(fd.cFileName, exeName) == 0)
                _snwprintf_s(out, MAX_PATH, MAX_PATH - 1, L"%s\\%s", root, fd.cFileName);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return out[0] != L'\0';
}

/* 盘符根一层: 枚举根下子目录,直接探测 exe,不深入 */
static BOOL scan_root_shallow(const wchar_t *root, const wchar_t *exeName, wchar_t *out)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hf;

    _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s*", root);
    hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t cand[MAX_PATH];
            _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s%s\\%s", root, fd.cFileName, exeName);
            if (path_exists(cand)) {
                wcscpy_s(out, MAX_PATH, cand);
                FindClose(hf);
                return TRUE;
            }
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return FALSE;
}

/* 自动检索主程序完整路径;成功返回 TRUE */
static BOOL detect_app(const wchar_t *exeName, wchar_t *out, size_t out_sz)
{
    wchar_t root[MAX_PATH], buf[MAX_PATH];
    DWORD len, drives;
    int i;

    out[0] = L'\0';

    /* 1. PATH 目录 */
    len = GetEnvironmentVariableW(L"Path", NULL, 0);
    if (len && len < 32768) {
        wchar_t *path = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        if (path) {
            const wchar_t *p;
            GetEnvironmentVariableW(L"Path", path, len + 1);
            p = path;
            while (*p && !out[0]) {
                const wchar_t *end = wcschr(p, L';');
                size_t dlen = end ? (size_t)(end - p) : wcslen(p);
                if (dlen < MAX_PATH) {
                    memcpy(root, p, dlen * sizeof(wchar_t));
                    root[dlen] = L'\0';
                    _snwprintf_s(buf, MAX_PATH, MAX_PATH - 1, L"%s\\%s", root, exeName);
                    if (path_exists(buf)) wcscpy_s(out, out_sz, buf);
                }
                if (!end) break;
                p = end + 1;
            }
            free(path);
        }
    }
    if (out[0]) return TRUE;

    /* 2. Programs 常用根(深度 2) */
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH)) {
        _snwprintf_s(buf, MAX_PATH, MAX_PATH - 1, L"%s\\Programs", root);
        search_tree(buf, exeName, 0, 2, out);
    }
    if (!out[0] && GetEnvironmentVariableW(L"ProgramFiles", root, MAX_PATH))
        search_tree(root, exeName, 0, 2, out);
    if (!out[0] && GetEnvironmentVariableW(L"ProgramFiles(x86)", root, MAX_PATH))
        search_tree(root, exeName, 0, 2, out);

    /* 3. 各固定盘根(一层) */
    if (!out[0]) {
        drives = GetLogicalDrives();
        for (i = 0; i < 26; i++) {
            if (drives & (1 << i)) {
                wchar_t drv[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
                if (GetDriveTypeW(drv) == DRIVE_FIXED && scan_root_shallow(drv, exeName, buf))
                    wcscpy_s(out, out_sz, buf);
            }
        }
    }
    return out[0] != L'\0';
}

enum {
    IDC_REDETECT = 102,
    IDC_ADV = 103,
    IDC_STATUS = 104,
    IDC_ROW_EDIT = 500,
    IDC_ROW_BROWSE = 600,
    IDC_ROW_INSTALL = 700,
    IDC_ROW_REMOVE = 800,
    IDM_ADD = 401,
    IDM_UNINSTALL = 402,
    IDM_CONFIG = 404,
    IDM_ABOUT = 405,
    IDC_AD_NAME = 200,
    IDC_AD_PATH = 201,
    IDC_AD_BROWSE = 202
};

static HWND g_hMain, g_hHeader, g_hRedetect, g_hAdv, g_hStatus;
static HWND *g_name, *g_edit, *g_browse, *g_install, *g_remove, *g_rowStatus;
static int g_rowCount = 0;
static int g_activeRow = -1;
static const wchar_t *g_winClass = L"openin_main";
static int g_addResult = 0;

static int preset_count(void) { return (int)(sizeof(PRESETS) / sizeof(PRESETS[0])); }

/* 目标总数 = 预设 + 非预设自定义 */
static int total_rows(void)
{
    int n = preset_count();
    for (int i = 0; i < g_targetCount; i++) {
        int isP = 0;
        for (int p = 0; p < preset_count(); p++)
            if (_wcsicmp(PRESETS[p].name, g_targets[i].name) == 0) { isP = 1; break; }
        if (!isP) n++;
    }
    return n;
}

/* g_targets 中第 k 个「非预设」项的索引 */
static int non_preset_index(int k)
{
    int seen = 0;
    for (int i = 0; i < g_targetCount; i++) {
        int isP = 0;
        for (int p = 0; p < preset_count(); p++)
            if (_wcsicmp(PRESETS[p].name, g_targets[i].name) == 0) { isP = 1; break; }
        if (!isP) {
            if (seen == k) return i;
            seen++;
        }
    }
    return -1;
}

/* 列表行 → 命令名(返回 0 失败) */
static int row_to_name(int row, wchar_t *name, size_t sz)
{
    if (row < preset_count()) {
        wcscpy_s(name, sz, PRESETS[row].name);
        return 1;
    }
    {
        int idx = non_preset_index(row - preset_count());
        if (idx >= 0) {
            wcscpy_s(name, sz, g_targets[idx].name);
            return 1;
        }
    }
    return 0;
}

/* 行 → 主程序文件名(预设);自定义返回 NULL */
static const wchar_t *row_exe_name(int row)
{
    if (row < preset_count()) return PRESETS[row].exeName;
    return NULL;
}

/* 该命令的 launcher 文件是否已安装 */
static BOOL is_installed(const wchar_t *name)
{
    wchar_t exePath[MAX_PATH], cmdPath[MAX_PATH];
    if (!g_installDir[0]) return FALSE;
    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", g_installDir, name);
    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", g_installDir, name);
    return path_exists(exePath) || path_exists(cmdPath);
}

/* 计算某命令的安装状态 */
static void target_status(const wchar_t *name, wchar_t *out, size_t sz)
{
    if (!is_installed(name)) {
        wcscpy_s(out, sz, L"未安装");
    } else {
        wcscpy_s(out, sz, path_in_environment(g_installDir) ? L"已安装" : L"已安装(PATH 缺失)");
    }
}

static void destroy_rows(void)
{
    int i;
    for (i = 0; i < g_rowCount; i++) {
        if (g_name[i]) DestroyWindow(g_name[i]);
        if (g_edit[i]) DestroyWindow(g_edit[i]);
        if (g_browse[i]) DestroyWindow(g_browse[i]);
        if (g_install[i]) DestroyWindow(g_install[i]);
        if (g_remove[i]) DestroyWindow(g_remove[i]);
        if (g_rowStatus[i]) DestroyWindow(g_rowStatus[i]);
    }
    if (g_name) {
        free(g_name); free(g_edit); free(g_browse);
        free(g_install); free(g_remove); free(g_rowStatus);
    }
    g_name = g_edit = g_browse = g_install = g_remove = g_rowStatus = NULL;
    g_rowCount = 0;
}

static void build_rows(HWND h)
{
    HINSTANCE hi = GetModuleHandleW(NULL);
    int n, i;

    destroy_rows();
    n = total_rows();
    g_rowCount = n;
    if (n <= 0) return;
    g_name = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_edit = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_browse = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_install = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_remove = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_rowStatus = (HWND *)calloc((size_t)n, sizeof(HWND));
    if (!g_name || !g_edit || !g_browse || !g_install || !g_remove || !g_rowStatus)
        return;

    for (i = 0; i < n; i++) {
        wchar_t label[160];
        if (i < preset_count()) {
            _snwprintf_s(label, 160, 159, L"%s  (%s)", PRESETS[i].display, PRESETS[i].name);
        } else {
            wchar_t nm[64];
            row_to_name(i, nm, 64);
            _snwprintf_s(label, 160, 159, L"%s (自定义)", nm);
        }
        g_name[i] = CreateWindowExW(0, L"STATIC", label, WS_CHILD | WS_VISIBLE,
            0, 0, 120, 20, h, NULL, hi, NULL);
        g_edit[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 22, h,
            (HMENU)(INT_PTR)(IDC_ROW_EDIT + i), hi, NULL);
        g_browse[i] = CreateWindowExW(0, L"BUTTON", L"浏览",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 60, 24, h,
            (HMENU)(INT_PTR)(IDC_ROW_BROWSE + i), hi, NULL);
        g_install[i] = CreateWindowExW(0, L"BUTTON", L"安装",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 90, 26, h,
            (HMENU)(INT_PTR)(IDC_ROW_INSTALL + i), hi, NULL);
        if (i >= preset_count())   /* 自定义行带「移除」 */
            g_remove[i] = CreateWindowExW(0, L"BUTTON", L"移除",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 50, 26, h,
                (HMENU)(INT_PTR)(IDC_ROW_REMOVE + i), hi, NULL);
        g_rowStatus[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            0, 0, 200, 18, h, NULL, hi, NULL);
    }
}

/* 填充一行路径: 先取已记录路径,否则自动检索 */
static void fill_path(int row)
{
    wchar_t name[64], found[MAX_PATH] = L"";
    int tidx;

    if (!row_to_name(row, name, 64)) return;
    tidx = find_target(name);
    if (tidx >= 0 && g_targets[tidx].exePath[0] && path_exists(g_targets[tidx].exePath))
        wcscpy_s(found, MAX_PATH, g_targets[tidx].exePath);
    if (!found[0] && row < preset_count())
        detect_app(PRESETS[row].exeName, found, MAX_PATH);
    SetWindowTextW(g_edit[row], found);
}

/* 刷新一行状态文本与安装按钮标签 */
static void update_status(int row)
{
    wchar_t name[64], st[64];

    if (!row_to_name(row, name, 64)) return;
    target_status(name, st, 64);
    SetWindowTextW(g_rowStatus[row], st);
    SetWindowTextW(g_install[row], is_installed(name) ? L"更新" : L"安装");
}

static void layout_controls(HWND h)
{
    RECT rc;
    int m = 10, rowH = 62, top = 36;
    int w, yBottom, i;

    GetClientRect(h, &rc);
    w = rc.right - rc.left;
    yBottom = rc.bottom - m - 26;

    if (g_hHeader)
        MoveWindow(g_hHeader, m, 8, w - 2 * m, 20, TRUE);

    for (i = 0; i < g_rowCount; i++) {
        int y = top + i * rowH;
        if (g_name[i]) MoveWindow(g_name[i], m, y + 2, 120, 20, TRUE);
        if (g_edit[i]) MoveWindow(g_edit[i], 135, y, w - 135 - 225, 22, TRUE);
        if (g_browse[i]) MoveWindow(g_browse[i], w - 225, y - 2, 60, 24, TRUE);
        if (g_install[i]) MoveWindow(g_install[i], w - 160, y - 3, 90, 26, TRUE);
        if (g_remove[i]) MoveWindow(g_remove[i], w - 62, y - 2, 50, 24, TRUE);
        if (g_rowStatus[i]) MoveWindow(g_rowStatus[i], 135, y + 26, w - 135 - 20, 18, TRUE);
    }

    if (g_hRedetect) MoveWindow(g_hRedetect, m, yBottom, 90, 26, TRUE);
    if (g_hAdv) MoveWindow(g_hAdv, 108, yBottom, 70, 26, TRUE);
    if (g_hStatus) MoveWindow(g_hStatus, 188, yBottom, w - 198, 26, TRUE);
}

static void refresh_rows(HWND h)
{
    int i;
    build_rows(h);
    layout_controls(h);
    for (i = 0; i < g_rowCount; i++) {
        fill_path(i);
        update_status(i);
    }
}

/* 预设行浏览: 选目录并定位主程序;自定义行: 直接选 exe 文件 */
static void browse_row(int row)
{
    g_activeRow = row;
    if (row >= preset_count()) {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hMain;
        ofn.lpstrFilter = L"应用程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrTitle = L"选择主程序 exe";
        if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_edit[row], file);
        return;
    }
    {
        wchar_t folder[MAX_PATH], codeExe[MAX_PATH];
        if (!browse_for_folder(folder, MAX_PATH)) return;
        if (!locate_main_exe(folder, PRESETS[row].exeName, codeExe, MAX_PATH)) {
            SetWindowTextW(g_hStatus, L"所选目录中未找到主程序,仅填入目录。");
            SetWindowTextW(g_edit[row], folder);
            return;
        }
        SetWindowTextW(g_edit[row], codeExe);
    }
}

static void install_row(int row)
{
    wchar_t name[64], path[MAX_PATH], codeExe[MAX_PATH], buf[4096];
    int addedPath = 0, tidx;
    DWORD attrs;

    g_activeRow = row;
    if (!row_to_name(row, name, 64)) return;
    GetWindowTextW(g_edit[row], path, MAX_PATH);
    if (!path[0] && row < preset_count())
        detect_app(PRESETS[row].exeName, path, MAX_PATH);
    if (!path[0]) { SetWindowTextW(g_hStatus, L"请选择或输入主程序路径。"); return; }

    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) { SetWindowTextW(g_hStatus, L"路径不存在,请重新选择。"); return; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        const wchar_t *exeName = row_exe_name(row);
        if (!exeName) { SetWindowTextW(g_hStatus, L"自定义目标需填主程序 exe 文件路径。"); return; }
        if (!locate_main_exe(path, exeName, codeExe, MAX_PATH)) {
            SetWindowTextW(g_hStatus, L"所选目录中未找到主程序。");
            return;
        }
    } else {
        wcscpy_s(codeExe, MAX_PATH, path);
    }

    SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (install_target(name, codeExe, g_installDir, buf, 4096, &addedPath) == 0) {
        g_addedPath = addedPath;
        tidx = find_target(name);
        if (tidx >= 0)
            wcscpy_s(g_targets[tidx].exePath, MAX_PATH, codeExe);
        else if (g_targetCount < MAX_TARGETS) {
            wcscpy_s(g_targets[g_targetCount].name, 64, name);
            wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, codeExe);
            g_targetCount++;
        }
        save_config();
        SetWindowTextW(g_edit[row], codeExe);
    }
    SetWindowTextW(g_hStatus, buf);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    update_status(row);
}

static void uninstall_row(int row)
{
    wchar_t name[64], buf[1024];

    g_activeRow = row;
    if (!row_to_name(row, name, 64)) return;
    if (uninstall_target(name, g_installDir, buf, 1024) == 0) {
        remove_target_entry(name);
        save_config();
    }
    SetWindowTextW(g_hStatus, buf);
    update_status(row);
}

static void remove_custom_row(int row)
{
    wchar_t name[64], buf[256];

    g_activeRow = row;
    if (!row_to_name(row, name, 64)) return;
    remove_target_entry(name);
    save_config();
    _snwprintf_s(buf, 256, 255, L"已移除自定义目标 \"%s\"(已安装的命令不受影响)。", name);
    SetWindowTextW(g_hStatus, buf);
    refresh_rows(g_hMain);
}

static void re_detect_all(void)
{
    int i;
    for (i = 0; i < g_rowCount; i++)
        if (i < preset_count()) fill_path(i);
    SetWindowTextW(g_hStatus, L"已重新自动检索路径。");
}

static void open_config_dir(void)
{
    wchar_t dir[MAX_PATH];
    get_config_dir(dir, MAX_PATH);
    CreateDirectoryW(dir, NULL);
    ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------- 添加自定义对话框(非核心) ---------- */
static LRESULT CALLBACK add_wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(NULL);
        CreateWindowExW(0, L"STATIC", L"命令名:", WS_CHILD | WS_VISIBLE, 12, 14, 70, 20, h, NULL, hi, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 86, 12, 210, 22,
            h, (HMENU)IDC_AD_NAME, hi, NULL);
        CreateWindowExW(0, L"STATIC", L"主程序:", WS_CHILD | WS_VISIBLE, 12, 44, 70, 20, h, NULL, hi, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 86, 42, 210, 22,
            h, (HMENU)IDC_AD_PATH, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            302, 42, 80, 22, h, (HMENU)IDC_AD_BROWSE, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            170, 96, 80, 28, h, (HMENU)IDOK, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            270, 96, 80, 28, h, (HMENU)IDCANCEL, hi, NULL);
        SetFocus(GetDlgItem(h, IDC_AD_NAME));
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_AD_BROWSE: {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn;
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h;
            ofn.lpstrFilter = L"应用程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrTitle = L"选择主程序 exe";
            if (GetOpenFileNameW(&ofn))
                SetWindowTextW(GetDlgItem(h, IDC_AD_PATH), file);
            return 0;
        }
        case IDOK: {
            wchar_t nm[64], path[MAX_PATH];
            GetWindowTextW(GetDlgItem(h, IDC_AD_NAME), nm, 64);
            GetWindowTextW(GetDlgItem(h, IDC_AD_PATH), path, MAX_PATH);
            if (!valid_name(nm)) {
                MessageBoxW(h, L"命令名只能包含字母、数字、下划线和短横线。",
                            L"openin", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!path[0] || !path_exists(path)) {
                MessageBoxW(h, L"请选择有效的主程序 exe 文件。",
                            L"openin", MB_OK | MB_ICONWARNING);
                return 0;
            }
            {
                int idx = find_target(nm);
                if (idx >= 0)
                    wcscpy_s(g_targets[idx].exePath, MAX_PATH, path);
                else if (g_targetCount < MAX_TARGETS) {
                    wcscpy_s(g_targets[g_targetCount].name, 64, nm);
                    wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, path);
                    g_targetCount++;
                }
            }
            g_addResult = 1;
            DestroyWindow(h);
            return 0;
        }
        case IDCANCEL:
            g_addResult = 0;
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        g_addResult = 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* 模态式添加自定义窗口(嵌套消息循环),返回是否新增 */
static int add_custom_dialog(HWND parent)
{
    const wchar_t *cls = L"openin_add";
    WNDCLASSW wc;
    HWND h;
    MSG msg;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = add_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    h = CreateWindowExW(0, cls, L"添加自定义目标",
                        WS_CAPTION | WS_SYSMENU | WS_POPUP,
                        CW_USEDEFAULT, CW_USEDEFAULT, 400, 170,
                        parent, NULL, wc.hInstance, NULL);
    if (!h) return 0;
    g_addResult = 0;
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    EnableWindow(parent, FALSE);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(h, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return g_addResult;
}

/* 高级菜单 */
static void show_adv_menu(HWND h)
{
    HMENU m = CreatePopupMenu();
    wchar_t label[128];
    RECT rc;

    if (g_activeRow >= 0 && g_activeRow < g_rowCount) {
        wchar_t nm[64];
        if (row_to_name(g_activeRow, nm, 64))
            _snwprintf_s(label, 128, 127, L"卸载 %s", nm);
        else
            wcscpy_s(label, 128, L"卸载");
    } else {
        wcscpy_s(label, 128, L"卸载(未选择)");
    }
    AppendMenuW(m, MF_STRING | (g_activeRow >= 0 ? MF_ENABLED : MF_GRAYED), IDM_UNINSTALL, label);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_ADD, L"添加自定义…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_CONFIG, L"打开配置目录");
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"关于");

    GetWindowRect(g_hAdv, &rc);
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_RIGHTBUTTON, rc.left, rc.bottom, 0, h, NULL);
    DestroyMenu(m);
}

/* ---------- 主窗口 ---------- */
static LRESULT CALLBACK main_wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(NULL);
        g_hMain = h;
        g_hHeader = CreateWindowExW(0, L"STATIC",
            L"可用应用（路径自动检测，确认后点「安装」）：",
            WS_CHILD | WS_VISIBLE, 0, 0, 400, 20, h, NULL, hi, NULL);
        g_hRedetect = CreateWindowExW(0, L"BUTTON", L"重新检测",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 90, 26, h, (HMENU)IDC_REDETECT, hi, NULL);
        g_hAdv = CreateWindowExW(0, L"BUTTON", L"高级▾",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 70, 26, h, (HMENU)IDC_ADV, hi, NULL);
        g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 400, 26, h, (HMENU)IDC_STATUS, hi, NULL);
        refresh_rows(h);
        return 0;
    }
    case WM_SIZE:
        layout_controls(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= IDC_ROW_BROWSE && LOWORD(w) < IDC_ROW_BROWSE + g_rowCount) {
            browse_row(LOWORD(w) - IDC_ROW_BROWSE);
            return 0;
        }
        if (LOWORD(w) >= IDC_ROW_INSTALL && LOWORD(w) < IDC_ROW_INSTALL + g_rowCount) {
            install_row(LOWORD(w) - IDC_ROW_INSTALL);
            return 0;
        }
        if (LOWORD(w) >= IDC_ROW_REMOVE && LOWORD(w) < IDC_ROW_REMOVE + g_rowCount) {
            remove_custom_row(LOWORD(w) - IDC_ROW_REMOVE);
            return 0;
        }
        switch (LOWORD(w)) {
        case IDC_REDETECT: re_detect_all(); return 0;
        case IDC_ADV: show_adv_menu(h); return 0;
        case IDM_ADD:
            if (add_custom_dialog(h)) { save_config(); refresh_rows(h); }
            return 0;
        case IDM_UNINSTALL:
            if (g_activeRow >= 0) uninstall_row(g_activeRow);
            else SetWindowTextW(g_hStatus, L"请先在某一行点击浏览或安装。");
            return 0;
        case IDM_CONFIG: open_config_dir(); return 0;
        case IDM_ABOUT:
            MessageBoxW(h, L"openin — 通用「打开到应用」启动器安装器\n\n"
                          L"把应用注入地址栏: 输入命令,以当前文件夹为参数打开。",
                        L"关于 openin", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        destroy_rows();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static int gui_main(void)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;

    load_config();
    if (!g_installDir[0])
        pick_target_dir(NULL, g_installDir, MAX_PATH);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = g_winClass;
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(0, g_winClass, L"openin — 打开到应用",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 700, 420,
                           NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

/* ---------- 主流程: 无参数进 GUI,有参数走 CLI ---------- */
int wmain(int argc, wchar_t *argv[])
{
    wchar_t name[64];
    wchar_t folder[MAX_PATH];
    wchar_t codeExe[MAX_PATH];
    wchar_t installDir[MAX_PATH];
    wchar_t buf[4096];
    wchar_t targetOverride[MAX_PATH];
    wchar_t localApp[MAX_PATH];
    int quiet = 0, i, addedPath = 0, rc, uninstallMode = 0;

    if (argc <= 1) {
        return gui_main();                       /* 双击/无参数 → GUI */
    }

    load_config();                               /* 与 GUI 共享配置 */

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
        if (_wcsicmp(argv[i], L"-u") == 0 && i + 1 < argc) {
            wcscpy_s(name, 64, argv[++i]);
            uninstallMode = 1;
            continue;
        }
        /* 其它参数视为命令名 */
        if (valid_name(argv[i]))
            wcscpy_s(name, 64, argv[i]);
        else
            show(L"openin",
                 L"命令名只能包含字母、数字、下划线和短横线,已回退为默认名 \"vscode\"。",
                 MB_OK | MB_ICONWARNING);
    }
    g_quiet = quiet;
    if (quiet) {
        if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL,
                             SHGFP_TYPE_CURRENT, localApp) == S_OK) {
            _snwprintf_s(g_logpath, MAX_PATH, MAX_PATH - 1,
                         L"%s\\openin.log", localApp);
            DeleteFileW(g_logpath);
            g_logfile = g_logpath;
        }
    }

    /* 卸载模式 */
    if (uninstallMode) {
        wchar_t ubuf[1024];
        int urc = uninstall_target(name, g_installDir, ubuf, 1024);
        if (urc == 0) {
            remove_target_entry(name);
            save_config();
        }
        show(L"openin", ubuf, urc == 0 ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
        return urc;
    }

    /* 选择应用目录 */
    if (!folder[0]) {
        if (!browse_for_folder(folder, MAX_PATH)) return 0;
    }

    /* 定位主程序(默认预设 vscode/Code.exe) */
    {
        const wchar_t *exeName = L"Code.exe";
        for (i = 0; i < preset_count(); i++)
            if (_wcsicmp(PRESETS[i].name, name) == 0) { exeName = PRESETS[i].exeName; break; }
        if (!locate_main_exe(folder, exeName, codeExe, MAX_PATH)) {
            _snwprintf_s(buf, 4096, 4095,
                         L"在所选目录中未找到 %s:\n%s\n\n请确认选择了正确的应用目录。",
                         exeName, folder);
            show(L"openin", buf, MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    /* 选择安装目录(自动: ~\.local\bin > %APPDATA%\npm > 创建 > 回退) */
    pick_target_dir(targetOverride, installDir, MAX_PATH);

    rc = install_target(name, codeExe, installDir, buf, 4096, &addedPath);
    if (rc == 0) {
        /* 持久化,便于 GUI 识别状态 */
        wcscpy_s(g_installDir, MAX_PATH, installDir);
        g_addedPath = addedPath;
        {
            int tidx = find_target(name);
            if (tidx >= 0)
                wcscpy_s(g_targets[tidx].exePath, MAX_PATH, codeExe);
            else if (g_targetCount < MAX_TARGETS) {
                wcscpy_s(g_targets[g_targetCount].name, 64, name);
                wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, codeExe);
                g_targetCount++;
            }
        }
        save_config();
        show(L"openin", buf, MB_OK | MB_ICONINFORMATION);
    } else {
        show(L"openin", buf, MB_OK | MB_ICONERROR);
    }
    return rc;
}
