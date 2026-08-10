#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

__attribute__((section(".data")))
wchar_t g_target_path[1024] = L"__OPENIN_TARGET_EXE_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

int wmain(int argc, wchar_t *argv[])
{
    wchar_t cmdline[2 * MAX_PATH + 64];
    wchar_t cwd[MAX_PATH];
    wchar_t *target;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (argc > 1) {
        int len = (int)wcslen(argv[1]);
        if (len >= 2 && argv[1][0] == L'"' && argv[1][len - 1] == L'"') {
            argv[1][len - 1] = L'\0';
            target = argv[1] + 1;
        } else {
            target = argv[1];
        }
    } else {
        if (GetCurrentDirectoryW(MAX_PATH, cwd) == 0) return 1;
        target = cwd;
    }

    cmdline[0] = L'\0';
    wcscat_s(cmdline, 2 * MAX_PATH + 64, L"\"");
    wcscat_s(cmdline, 2 * MAX_PATH + 64, g_target_path);
    wcscat_s(cmdline, 2 * MAX_PATH + 64, L"\" \"");
    wcscat_s(cmdline, 2 * MAX_PATH + 64, target);
    wcscat_s(cmdline, 2 * MAX_PATH + 64, L"\"");
    for (int a = 2; a < argc; a++) {
        wcscat_s(cmdline, 2 * MAX_PATH + 64, L" ");
        wcscat_s(cmdline, 2 * MAX_PATH + 64, argv[a]);
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(g_target_path, cmdline, NULL, NULL, FALSE,
                        DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
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
