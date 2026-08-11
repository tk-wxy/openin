#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

__attribute__((section(".data")))
wchar_t g_target_path[1024] = L"__OPENIN_TARGET_EXE_MAGIC_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD_PAD__";

/* ---------- 新控制台窗口识别与强制激活(前台锁绕过) ---------- */
typedef struct { HWND hwnd[64]; int count; } ConsoleList;

static BOOL CALLBACK enum_consoles(HWND h, LPARAM lp)
{
    wchar_t cls[64];
    ConsoleList *list = (ConsoleList *)lp;

    if (GetClassNameW(h, cls, 64) == 0) return TRUE;
    if (wcscmp(cls, L"ConsoleWindowClass") != 0) return TRUE;
    if (list->count < 64) list->hwnd[list->count++] = h;
    return TRUE;
}

static BOOL in_list(const ConsoleList *l, HWND h)
{
    int i;
    for (i = 0; i < l->count; i++)
        if (l->hwnd[i] == h) return TRUE;
    return FALSE;
}

/* 轮询找「启动前不存在的」控制台窗口(即本次新建的),强制激活。
 * 手段: AttachThreadInput 合并输入队列 + ALT 键注入(经典前台锁绕过) +
 * SetForegroundWindow + TOPMOST 置顶兜底 */
static void activate_new_console(const ConsoleList *before)
{
    int tries;
    for (tries = 0; tries < 30; tries++) {
        ConsoleList after;
        int i;
        ZeroMemory(&after, sizeof(after));
        EnumWindows(enum_consoles, (LPARAM)&after);
        for (i = 0; i < after.count; i++) {
            if (!in_list(before, after.hwnd[i])) {
                HWND h = after.hwnd[i];
                DWORD thread = GetWindowThreadProcessId(h, NULL);
                DWORD self = GetCurrentThreadId();
                if (thread && thread != self) {
                    AttachThreadInput(self, thread, TRUE);
                    ShowWindow(h, SW_RESTORE);
                    keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY, 0);
                    keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
                    SetForegroundWindow(h);
                    SetForegroundWindow(h);
                    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW);
                    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
                    AttachThreadInput(self, thread, FALSE);
                }
                return;
            }
        }
        Sleep(50);
    }
}

int wmain(int argc, wchar_t *argv[])
{
    wchar_t cmdline[2 * MAX_PATH + 512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ConsoleList before;
    MSG msg;
    int a;

    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);   /* 确保消息队列存在(AttachThreadInput 需要) */

    ZeroMemory(&before, sizeof(before));
    EnumWindows(enum_consoles, (LPARAM)&before);

    /* 主路径: cmd /k 新建可见控制台运行 CLI(继承 cwd,不传目录参数) */
    cmdline[0] = L'\0';
    wcscat_s(cmdline, 2 * MAX_PATH + 512, L"/k \"");
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

    activate_new_console(&before);   /* 找到新控制台窗口并强制激活 */
    return 0;
}
