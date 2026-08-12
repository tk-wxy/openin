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
 *   - 无状态: 只创建启动文件(<name>.exe/.cmd),不产生任何配置、日志或外部文件夹;
 *     临时编译产物用完即删
 *   - -q 静默模式: 不弹 MessageBox,无任何输出
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

#include "launcher_templates.h"
#include "openin.h"

static int g_quiet = 0;


/* ---------- 输出(静默模式下不弹窗、不产生任何文件) ---------- */
static void show(const wchar_t *title, const wchar_t *text, UINT flags)
{
    if (!g_quiet)
        MessageBoxW(NULL, text, title, flags);
}

/* ---------- 文件夹选择 ---------- */
BOOL browse_for_folder(wchar_t *out, size_t out_sz)
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

BOOL path_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 在 folder 中找主程序 exeName;找不到则向上一级再找(兼容选中 bin 子目录的情况) */
BOOL locate_main_exe(const wchar_t *folder, const wchar_t *exeName,
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
BOOL path_in_environment(const wchar_t *dir)
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
void pick_target_dir(const wchar_t *override, wchar_t *out, size_t out_sz)
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

/* ---------- 预编译二进制 Patch Launcher (免 GCC 模式) ---------- */
static BOOL write_launcher_binary(const wchar_t *codeExe, int cli, const wchar_t *outExePath)
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

/* ---------- 生成 launcher 源码 ---------- */
/*
 * 生成的 launcher: 启动主程序打开「当前工作目录」或命令行参数指定路径。
 * 模板按行存放,@@PATH@@ 处替换为转义后的主程序完整路径,@@NAME@@ 为命令名。
 * 使用 wcscat 拼接命令行以避免引号嵌套带来的格式转义问题。
 */
static BOOL write_launcher_source(const wchar_t *codeExe, const wchar_t *name,
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
    /* CLI 模板: 智能判断 .cmd/.bat 用 cmd /c 执行,其他直接启动,继承控制台 */
    static const wchar_t *CLI_TPL[] = {
        L"#define UNICODE",
        L"#define _UNICODE",
        L"#include <windows.h>",
        L"#include <stdio.h>",
        L"",
        L"#define CODE_EXE L\"@@PATH@@\"",
        L"",
        L"int wmain(int argc, wchar_t *argv[])",
        L"{",
        L"    wchar_t cmdline[2048];",
        L"    wchar_t *ext;",
        L"    STARTUPINFOW si;",
        L"    PROCESS_INFORMATION pi;",
        L"    int i;",
        L"",
        L"    /* 检测目标是否为 .cmd/.bat 脚本 */",
        L"    ext = wcsrchr(CODE_EXE, L'.');",
        L"    if (ext && (_wcsicmp(ext, L\".cmd\") == 0 || _wcsicmp(ext, L\".bat\") == 0)) {",
        L"        /* .cmd/.bat: 用 cmd /c 执行,继承当前控制台 */",
        L"        cmdline[0] = L'\\0';",
        L"        wcscat_s(cmdline, 2048, L\"cmd.exe /c \\\"\\\"\");",
        L"        wcscat_s(cmdline, 2048, CODE_EXE);",
        L"        wcscat_s(cmdline, 2048, L\"\\\"\");",
        L"",
        L"        for (i = 1; i < argc; i++) {",
        L"            wcscat_s(cmdline, 2048, L\" \\\"\");",
        L"            wcscat_s(cmdline, 2048, argv[i]);",
        L"            wcscat_s(cmdline, 2048, L\"\\\"\");",
        L"        }",
        L"        wcscat_s(cmdline, 2048, L\"\\\"\");",
        L"    } else {",
        L"        /* .exe: 直接启动 */",
        L"        cmdline[0] = L'\\0';",
        L"        wcscat_s(cmdline, 2048, L\"\\\"\");",
        L"        wcscat_s(cmdline, 2048, CODE_EXE);",
        L"        wcscat_s(cmdline, 2048, L\"\\\"\");",
        L"",
        L"        for (i = 1; i < argc; i++) {",
        L"            wcscat_s(cmdline, 2048, L\" \\\"\");",
        L"            wcscat_s(cmdline, 2048, argv[i]);",
        L"            wcscat_s(cmdline, 2048, L\"\\\"\");",
        L"        }",
        L"    }",
        L"",
        L"    /* 启动目标进程,继承当前控制台 */",
        L"    ZeroMemory(&si, sizeof(si));",
        L"    si.cb = sizeof(si);",
        L"    ZeroMemory(&pi, sizeof(pi));",
        L"",
        L"    /* 不使用 CREATE_NEW_CONSOLE,让子进程继承父控制台",
        L"     * 终端场景: 直接继承,输出显示在当前终端",
        L"     * 地址栏场景: explorer 会为 launcher 分配控制台 */",
        L"    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0,",
        L"                        NULL, NULL, &si, &pi)) {",
        L"        wchar_t msg[600];",
        L"        _snwprintf_s(msg, 600, 599, L\"Failed to start:\\n%s\\n\\nError: %lu\",",
        L"                     CODE_EXE, (unsigned long)GetLastError());",
        L"        MessageBoxW(NULL, msg, L\"openin\", MB_OK | MB_ICONERROR);",
        L"        return 1;",
        L"    }",
        L"",
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

static BOOL write_launcher_cmd(const wchar_t *codeExe, int cli, const wchar_t *cmdPath)
{
    wchar_t esc[2 * MAX_PATH + 16];
    wchar_t buf[3 * MAX_PATH + 512];
    char *ansi;
    int alen;
    HANDLE h;
    DWORD written;

    cmd_escape(codeExe, esc, 2 * MAX_PATH + 16);

    if (cli) {
        /* CLI: 继承 cwd,透传参数,新窗口运行 */
        _snwprintf_s(buf, 3 * MAX_PATH + 512, 3 * MAX_PATH + 512 - 1,
            L"@echo off\r\n"
            L"rem openin launcher - generated by openin\r\n"
            L"start \"\" \"%s\" %%*\r\n",
            esc);
    } else {
        _snwprintf_s(buf, 3 * MAX_PATH + 512, 3 * MAX_PATH + 512 - 1,
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

/* ---------- 命令名校验 ---------- */
int valid_name(const wchar_t *s)
{
    const wchar_t *c;
    if (!s || !*s) return 0;
    for (c = s; *c; c++)
        if (!((*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') ||
              (*c >= L'0' && *c <= L'9') || *c == L'_' || *c == L'-'))
            return 0;
    return 1;
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
 */
int install_target(const wchar_t *name, const wchar_t *codeExe, int cli,
                   const wchar_t *installDir,
                   wchar_t *outSummary, size_t sumSz, int *outAddedPath)
{
    wchar_t cmdPath[MAX_PATH], exePath[MAX_PATH], srcPath[MAX_PATH], logPath[MAX_PATH];
    wchar_t tempDir[MAX_PATH];
    wchar_t cmdline[1024], logBuf[4096];
    wchar_t conflictPath[MAX_PATH], gccPath[MAX_PATH];
    int exeBuilt = 0, pathAlready = 0, conflict = 0, compileFailed = 0;

    /* CLI 工具: 写入 App Paths 注册表(HKCU),实现地址栏/终端分离
       - 地址栏 'codex' → ShellExecuteEx 查 App Paths → 启动 openin launcher
       - 终端 'codex' → SearchPath 仅查 PATH → 运行原生 CLI(不受影响)
       - 卸载时清理注册表键,零残留 */

    /* 确保安装目录存在(-p 可能指向尚未创建的目录) */
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

    /* .cmd 备用启动器(无需编译器,始终生成) */
    if (!write_launcher_cmd(codeExe, cli, cmdPath)) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"生成 .cmd 备用启动器失败:\n%s", cmdPath);
        return 1;
    }

    /* 1. 优先使用预编译二进制 Patch 模板生成 .exe (免 GCC 模式, 毫秒级完成, 无环境依赖) */
    if (write_launcher_binary(codeExe, cli, exePath)) {
        exeBuilt = 1;
    }
    /* 2. 备用: 若二进制 Patch 失败且系统存在 gcc, 则降级使用 gcc 动态编译 */
    else if (find_in_path(L"gcc.exe", gccPath, MAX_PATH)) {
        if (write_launcher_source(codeExe, name, cli, srcPath)) {
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

    /* CLI 工具: 写入 App Paths 注册表(HKCU),地址栏可用无后缀命令 */
    if (cli && exeBuilt) {
        wchar_t subKey[300];
        HKEY hKey;
        LONG res;

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
    if (exeBuilt) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1,
                     L"安装完成 ✓\n命令:   %s (.exe%s)\n备用:   %s.cmd\n"
                     L"目录:   %s (%s)\n应用:   %s\nlauncher: %s\n\n"
                     L"地址栏/终端输入 \"%s\" 或 \"%s.cmd\" 即可打开。\n"
                     L"注意: 已打开的终端/资源管理器需重启生效。%s",
                     name, cli ? L" + 地址栏注册" : L"", name, installDir,
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

/* 卸载目标: 删除 launcher 文件 + 清理 App Paths 注册表(CLI) */
int uninstall_target(const wchar_t *name, const wchar_t *installDir,
                     wchar_t *outSummary, size_t sumSz)
{
    wchar_t exePath[MAX_PATH], cmdPath[MAX_PATH], subKey[300];
    BOOL any = FALSE;

    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", installDir, name);
    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", installDir, name);
    if (DeleteFileW(exePath)) any = TRUE;
    if (DeleteFileW(cmdPath)) any = TRUE;

    /* 清理 App Paths 注册表(CLI 目标,失败忽略) */
    _snwprintf_s(subKey, 300, 299,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s.exe", name);
    RegDeleteKeyW(HKEY_CURRENT_USER, subKey);

    if (!any) {
        _snwprintf_s(outSummary, sumSz, sumSz - 1, L"\"%s\" 尚未安装,无需卸载。", name);
        return 1;
    }

    _snwprintf_s(outSummary, sumSz, sumSz - 1, L"已卸载 \"%s\"。", name);
    return 0;
}

/* ---------- 预设模板 ---------- */
const Preset PRESETS[] = {
    { L"vscode",   L"Code.exe",          L"VS Code",         0 },
    { L"cursor",   L"Cursor.exe",        L"Cursor",          0 },
    { L"windsurf", L"Windsurf.exe",      L"Windsurf",        0 },
    { L"zed",      L"zed.exe",           L"Zed Editor",      0 },
    { L"sublime",  L"sublime_text.exe",  L"Sublime Text",    0 },
    { L"idea",     L"idea64.exe",        L"IntelliJ IDEA",   0 },
    { L"pycharm",  L"pycharm64.exe",     L"PyCharm",         0 },
    { L"webstorm", L"webstorm64.exe",    L"WebStorm",        0 },
    { L"clion",    L"clion64.exe",       L"CLion",           0 },
    { L"goland",   L"goland64.exe",      L"GoLand",          0 },
    { L"rider",    L"rider64.exe",       L"Rider",           0 },
    { L"datagrip", L"datagrip64.exe",    L"DataGrip",        0 },
    { L"rustrover",L"rustrover64.exe",   L"RustRover",       0 },
    { L"fleet",    L"Fleet.exe",         L"JetBrains Fleet", 0 },
    { L"neovide",  L"neovide.exe",       L"Neovide",         0 },
    { L"wt",       L"wt.exe",            L"Windows Terminal",0 },
    { L"claude",   L"claude.exe",        L"Claude Code",     1 },
    { L"codex",    L"codex.cmd",         L"Codex",           1 },
};

int preset_count(void) { return (int)(sizeof(PRESETS) / sizeof(PRESETS[0])); }

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

/* 去掉注册表值两端的空白与引号(部分安装程序登记时带引号/尾随空格) */
static void trim_reg_path(wchar_t *s)
{
    size_t len = wcslen(s);
    while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = L'\0';
    if (len >= 2 && s[0] == L'"' && s[len - 1] == L'"') {
        s[len - 1] = L'\0';
        memmove(s, s + 1, (len - 1) * sizeof(wchar_t));
        len = wcslen(s);
        while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = L'\0';
    }
}

/* 读某个注册表键的默认值(REG_SZ/EXPAND_SZ)到 out,成功返回 TRUE */
static BOOL reg_default_value(HKEY root, const wchar_t *subKey, wchar_t *out, size_t out_sz)
{
    DWORD size = (DWORD)(out_sz * sizeof(wchar_t));
    if (RegGetValueW(root, subKey, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     NULL, out, &size) != ERROR_SUCCESS)
        return FALSE;
    out[out_sz - 1] = L'\0';
    trim_reg_path(out);
    return out[0] != L'\0';
}

/* App Paths 注册表检测(只读): 安装程序登记的权威位置,零磁盘成本 */
static BOOL detect_app_registry(const wchar_t *exeName, wchar_t *out, size_t out_sz)
{
    static const wchar_t *AP = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    static const wchar_t *AP32 = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    wchar_t subKey[300];

    out[0] = L'\0';
    _snwprintf_s(subKey, 300, 299, L"%s\\%s", AP, exeName);
    if (reg_default_value(HKEY_CURRENT_USER, subKey, out, out_sz)) return TRUE;
    if (reg_default_value(HKEY_LOCAL_MACHINE, subKey, out, out_sz)) return TRUE;
    _snwprintf_s(subKey, 300, 299, L"%s\\%s", AP32, exeName);
    if (reg_default_value(HKEY_LOCAL_MACHINE, subKey, out, out_sz)) return TRUE;
    out[0] = L'\0';
    return FALSE;
}

/* 自动检索主程序完整路径;成功返回 TRUE */
/* fromFallback=1 时不再做 .exe<->.cmd 扩展名回退,避免两两互检导致无限递归 */
static BOOL detect_app_impl(const wchar_t *exeName, wchar_t *out, size_t out_sz, int fromFallback)
{
    wchar_t root[MAX_PATH], buf[MAX_PATH];
    DWORD len, drives;
    int i;

    out[0] = L'\0';

    /* 1. App Paths 注册表(HKCU/HKLM,安装程序登记的权威位置) */
    if (detect_app_registry(exeName, out, out_sz) && path_exists(out)) return TRUE;

    /* 2. PATH 目录 */
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

    /* 3. Programs 常用根(深度 2) */
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH)) {
        _snwprintf_s(buf, MAX_PATH, MAX_PATH - 1, L"%s\\Programs", root);
        search_tree(buf, exeName, 0, 2, out);
    }
    if (!out[0] && GetEnvironmentVariableW(L"ProgramFiles", root, MAX_PATH))
        search_tree(root, exeName, 0, 2, out);
    if (!out[0] && GetEnvironmentVariableW(L"ProgramFiles(x86)", root, MAX_PATH))
        search_tree(root, exeName, 0, 2, out);

    /* 4. 各固定盘根(一层) */
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
    /* 5. 扩展名回退策略 (.exe <-> .cmd): 只允许回退一层,防止无限递归 */
    if (!out[0] && !fromFallback) {
        wchar_t altName[MAX_PATH];
        wcscpy_s(altName, MAX_PATH, exeName);
        wchar_t *dot = wcsrchr(altName, L'.');
        if (dot) {
            if (_wcsicmp(dot, L".exe") == 0) {
                wcscpy_s(dot, 5, L".cmd");
                detect_app_impl(altName, out, out_sz, 1);
            } else if (_wcsicmp(dot, L".cmd") == 0) {
                wcscpy_s(dot, 5, L".exe");
                detect_app_impl(altName, out, out_sz, 1);
            }
        }
    }
    return out[0] != L'\0';
}

/* 对外接口: 以 exeName 检索,必要时回退到另一扩展名(仅一层) */
BOOL detect_app(const wchar_t *exeName, wchar_t *out, size_t out_sz)
{
    return detect_app_impl(exeName, out, out_sz, 0);
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
    int quiet = 0, i, rc, uninstallMode = 0;

    if (argc <= 1) {
        return gui_main();                       /* 双击/无参数 → GUI */
    }

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

    /* 卸载模式 */
    if (uninstallMode) {
        wchar_t ubuf[1024];
        wchar_t udir[MAX_PATH];
        pick_target_dir(targetOverride, udir, MAX_PATH);   /* 启动器在自动选择的目录 */
        int urc = uninstall_target(name, udir, ubuf, 1024);
        if (urc == 0)
            remove_target_entry(name);
        show(L"openin", ubuf, urc == 0 ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
        return urc;
    }

    /* 选择应用目录 */
    if (!folder[0]) {
        if (!browse_for_folder(folder, MAX_PATH)) return 0;
    }

    /* 定位主程序: 若 -d 直接传入 exe/cmd 文件则直接使用, 否则在目录中定位 */
    DWORD attrs = GetFileAttributesW(folder);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        wcscpy_s(codeExe, MAX_PATH, folder);
    } else {
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

    {
        int cli = 0;
        for (i = 0; i < preset_count(); i++)
            if (_wcsicmp(PRESETS[i].name, name) == 0) { cli = PRESETS[i].cli; break; }
        rc = install_target(name, codeExe, cli, installDir, buf, 4096, NULL);
    }
    if (rc == 0) {
        /* 仅在会话内存记录目标,不落盘 */
        {
            int cli = 0;
            for (i = 0; i < preset_count(); i++)
                if (_wcsicmp(PRESETS[i].name, name) == 0) { cli = PRESETS[i].cli; break; }
            int tidx = find_target(name);
            if (tidx >= 0) {
                wcscpy_s(g_targets[tidx].exePath, MAX_PATH, codeExe);
                g_targets[tidx].cli = cli;
            } else if (g_targetCount < MAX_TARGETS) {
                wcscpy_s(g_targets[g_targetCount].name, 64, name);
                wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, codeExe);
                g_targets[g_targetCount].cli = cli;
                g_targetCount++;
            }
        }
        show(L"openin", buf, MB_OK | MB_ICONINFORMATION);
    } else {
        show(L"openin", buf, MB_OK | MB_ICONERROR);
    }
    return rc;
}
