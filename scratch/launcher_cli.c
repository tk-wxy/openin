#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

__attribute__((section(".data")))
wchar_t g_target_path[1024] = L"__OPENIN_TARGET_EXE_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

int wmain(int argc, wchar_t *argv[])
{
    wchar_t cmdline[2048];
    wchar_t *ext;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int i;

    /* 检测目标是否为 .cmd/.bat 脚本 */
    ext = wcsrchr(g_target_path, L'.');
    if (ext && (_wcsicmp(ext, L".cmd") == 0 || _wcsicmp(ext, L".bat") == 0)) {
        /* .cmd/.bat: 用 cmd /c 执行,继承当前控制台 */
        cmdline[0] = L'\0';
        wcscat_s(cmdline, 2048, L"cmd.exe /c \"\"");
        wcscat_s(cmdline, 2048, g_target_path);
        wcscat_s(cmdline, 2048, L"\"");

        for (i = 1; i < argc; i++) {
            wcscat_s(cmdline, 2048, L" \"");
            wcscat_s(cmdline, 2048, argv[i]);
            wcscat_s(cmdline, 2048, L"\"");
        }
        wcscat_s(cmdline, 2048, L"\"");
    } else {
        /* .exe: 直接启动 */
        cmdline[0] = L'\0';
        wcscat_s(cmdline, 2048, L"\"");
        wcscat_s(cmdline, 2048, g_target_path);
        wcscat_s(cmdline, 2048, L"\"");

        for (i = 1; i < argc; i++) {
            wcscat_s(cmdline, 2048, L" \"");
            wcscat_s(cmdline, 2048, argv[i]);
            wcscat_s(cmdline, 2048, L"\"");
        }
    }

    /* 启动目标进程,继承当前控制台 */
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* 不使用 CREATE_NEW_CONSOLE,让子进程继承父控制台
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
