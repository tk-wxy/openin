#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <shellapi.h>

__attribute__((section(".data")))
wchar_t g_target_path[1024] = L"__OPENIN_TARGET_EXE_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

int wmain(int argc, wchar_t *argv[])
{
    wchar_t cwd[MAX_PATH];
    wchar_t wtPath[MAX_PATH];
    wchar_t cmdline[2 * MAX_PATH + 512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    int a;

    if (GetCurrentDirectoryW(MAX_PATH, cwd) == 0) cwd[0] = L'\0';

    /* 优先: Windows Terminal 打开可见终端运行 CLI */
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", wtPath, MAX_PATH)) {
        wcscat_s(wtPath, MAX_PATH, L"\\Microsoft\\WindowsApps\\wt.exe");
        if (GetFileAttributesW(wtPath) != INVALID_FILE_ATTRIBUTES) {
            cmdline[0] = L'\0';
            wcscat_s(cmdline, 2 * MAX_PATH + 512, L"/c start \"\" \"");
            wcscat_s(cmdline, 2 * MAX_PATH + 512, wtPath);
            wcscat_s(cmdline, 2 * MAX_PATH + 512, L"\" -d \"");
            wcscat_s(cmdline, 2 * MAX_PATH + 512, cwd);
            wcscat_s(cmdline, 2 * MAX_PATH + 512, L"\" cmd /k \"");
            wcscat_s(cmdline, 2 * MAX_PATH + 512, g_target_path);
            wcscat_s(cmdline, 2 * MAX_PATH + 512, L"\"");
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdline,
                                NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return 0;
            }
        }
    }

    /* 回退: cmd /c 新建控制台 */
    cmdline[0] = L'\0';
    wcscat_s(cmdline, 2 * MAX_PATH + 512, L"/c \"\"");
    wcscat_s(cmdline, 2 * MAX_PATH + 512, g_target_path);
    for (a = 1; a < argc; a++) {
        wcscat_s(cmdline, 2 * MAX_PATH + 512, L" \"");
        wcscat_s(cmdline, 2 * MAX_PATH + 512, argv[a]);
        wcscat_s(cmdline, 2 * MAX_PATH + 512, L"\"");
    }
    wcscat_s(cmdline, 2 * MAX_PATH + 512, L"\"");

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmdline,
                        NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
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
