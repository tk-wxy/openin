/*
 * openin.c — 入口: 无参数进 GUI,有参数走 CLI。
 * 引擎逻辑已拆分: utils.c / pathenv.c / detect.c / core.c / gui.c,接口见 openin.h。
 *
 * 运行方式:
 *   openin.exe                    弹出 GUI 主窗口
 *   openin.exe -d "D:\VS Code"    静默指定目录(命令行安装,默认命令名 vscode)
 *   openin.exe -d "D:\VS Code" code   同时指定目录和命令名
 *   openin.exe -p C:\Users\you\bin    指定安装目录
 *   openin.exe -u vscode          卸载指定命令
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openin.h"

static int g_quiet = 0;


/* ---------- 输出(静默模式下不弹窗) ---------- */
static void show(const wchar_t *title, const wchar_t *text, UINT flags)
{
    if (!g_quiet)
        MessageBoxW(NULL, text, title, flags);
}

/* CLI 文本输出: 有 stdout(重定向)走 WriteFile;否则挂父控制台 WriteConsoleW;都没有则回退弹窗 */
static void cli_print(const wchar_t *title, const wchar_t *text, UINT flags)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    BOOL attached = FALSE;
    DWORD written = 0;

    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            attached = TRUE;
            hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }
    if (!hOut || hOut == INVALID_HANDLE_VALUE)
        hOut = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);

    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        /* 控制台走 WriteConsoleW(UTF-16 中文无损);重定向管道/文件则 WriteFile */
        if (!WriteConsoleW(hOut, text, (DWORD)wcslen(text), &written, NULL))
            WriteFile(hOut, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &written, NULL);
        WriteConsoleW(hOut, L"\r\n", 2, &written, NULL);
        if (attached) FreeConsole();
        return;
    }
    if (attached) FreeConsole();
    show(title, text, flags);
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
    int quiet = 0, i, rc, uninstallMode = 0, listMode = 0, installAllMode = 0;

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
        if (_wcsicmp(argv[i], L"-l") == 0) { listMode = 1; continue; }
        if (_wcsicmp(argv[i], L"-a") == 0) { installAllMode = 1; continue; }
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
        int urc = uninstall_target(name, udir, ubuf, 1024, NULL);
        if (urc == 0)
            remove_target_entry(name);
        show(L"openin", ubuf, urc == 0 ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
        return urc;
    }

    /* 列出已安装模式 */
    if (listMode) {
        wchar_t dir[MAX_PATH], list[4096], msg[4300];
        pick_target_dir(targetOverride, dir, MAX_PATH);
        int n = list_installed(dir, list, 4096);
        if (n > 0)
            _snwprintf_s(msg, 4300, 4299, L"已安装命令(%d 个,目录: %s):\n\n%s", n, dir, list);
        else
            _snwprintf_s(msg, 4300, 4299, L"未安装任何命令。(安装目录: %s)", dir);
        cli_print(L"openin", msg, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    /* 全部安装模式: 遍历预设,检出即装、已装跳过 */
    if (installAllMode) {
        wchar_t dir[MAX_PATH], found[MAX_PATH], buf[4096], msg[512];
        int added = 0, skipped = 0, notFound = 0, failed = 0;
        pick_target_dir(targetOverride, dir, MAX_PATH);
        for (i = 0; i < preset_count(); i++) {
            if (target_installed(PRESETS[i].name, dir)) { skipped++; continue; }
            found[0] = L'\0';
            if (!detect_app(PRESETS[i].exeName, found, MAX_PATH)) { notFound++; continue; }
            if (install_target(PRESETS[i].name, found, PRESETS[i].args, PRESETS[i].url, PRESETS[i].cli, dir, buf, 4096, NULL, NULL) == 0)
                added++;
            else
                failed++;
        }
        _snwprintf_s(msg, 512, 511,
                     L"全部安装完成:\n  新增: %d\n  已存在跳过: %d\n  未检出: %d\n  失败: %d\n\n目录: %s",
                     added, skipped, notFound, failed, dir);
        cli_print(L"openin", msg, MB_OK | MB_ICONINFORMATION);
        return failed ? 1 : 0;
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
        const wchar_t *args = L"";
        const wchar_t *url = NULL;
        int cli = 0;
        for (i = 0; i < preset_count(); i++)
            if (_wcsicmp(PRESETS[i].name, name) == 0) { cli = PRESETS[i].cli; args = PRESETS[i].args; url = PRESETS[i].url; break; }
        rc = install_target(name, codeExe, args, url, cli, installDir, buf, 4096, NULL, NULL);
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
