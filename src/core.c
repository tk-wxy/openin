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
    { L"opencode", L"opencode.cmd",      L"OpenCode",        1 },
};

int preset_count(void) { return (int)(sizeof(PRESETS) / sizeof(PRESETS[0])); }
