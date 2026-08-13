/*
 * gui.c — openin 的 Win32 GUI 全部逻辑(无 .rc 资源)。
 * 依赖引擎(openin.c)导出接口,见 openin.h;对外只暴露 gui_main()。
 * 拆分自原单文件 openin.c(2026-08-12,方案 B:先拆 GUI)。
 */
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00   /* GetDpiForWindow / GetDpiForSystem / WM_DPICHANGED */

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "openin.h"

static int g_scrollY = 0;

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
    IDM_ABOUT = 405,
    IDC_AD_NAME = 200,
    IDC_AD_PATH = 201,
    IDC_AD_BROWSE = 202,
    IDC_AD_CLI = 203
};

static HWND g_hMain, g_hHeader, g_hRedetect, g_hAdv, g_hStatus, g_hList;
static HWND *g_name, *g_edit, *g_browse, *g_install, *g_remove, *g_rowStatus;
static int g_rowCount = 0;
static int g_activeRow = -1;
static const wchar_t *g_winClass = L"openin_main";
static const wchar_t *g_listClass = L"openin_list";
static int g_addResult = 0;

#define WM_APP_DETECT (WM_APP + 1)      /* 后台检测线程回填单行路径 */
static HANDLE g_detectThread = NULL;    /* 后台自动检测线程(仅主线程访问) */
static volatile LONG g_detectStop = 0;  /* 通知后台线程停止 */

/* ---------- UI 字体与 DPI(现代原生观感,根治高缩放下的字体扭曲) ---------- */
static HFONT g_font = NULL;             /* 雅黑 UI 字体,随 DPI 重建 */
static int  g_dpi = 96;                 /* 当前窗口 DPI(manifest 声明 PerMonitorV2) */

#define SCALE(x) MulDiv((x), g_dpi, 96) /* 96 DPI 基准像素 → 当前 DPI 物理像素 */

/* 依窗口当前 DPI 重建雅黑 UI 字体;旧字体先释放防泄漏 */
static void create_ui_font(HWND h)
{
    UINT dpi = GetDpiForWindow(h);
    int height;
    if (dpi == 0) dpi = 96;
    g_dpi = (int)dpi;
    if (g_font) { DeleteObject(g_font); g_font = NULL; }
    height = -MulDiv(9, g_dpi, 72);     /* 9pt,逻辑像素随 DPI 缩放 */
    g_font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");   /* 缺失时 GDI 自动替换 */
}

static BOOL CALLBACK set_font_cb(HWND child, LPARAM l)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)l, TRUE);
    return TRUE;
}

/* 把 UI 字体应用到 root 下全部子控件(含后续重建的行控件) */
static void apply_font(HWND root)
{
    if (g_font) EnumChildWindows(root, set_font_cb, (LPARAM)g_font);
}

/* 滚动列表面板:裁剪行控件、隔离固定底栏;按钮/编辑框通知转交主窗口统一处理 */
static LRESULT CALLBACK list_wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_COMMAND)
        return SendMessageW(GetParent(h), msg, w, l);
    return DefWindowProcW(h, msg, w, l);
}


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
            int k = non_preset_index(i - preset_count());
            row_to_name(i, nm, 64);
            _snwprintf_s(label, 160, 159, L"%s (自定义%s)",
                         nm, (k >= 0 && g_targets[k].cli) ? L"·CLI" : L"");
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

static void update_scrollbar(HWND h)
{
    RECT rc;
    int m = SCALE(16), btnH = SCALE(28), gap = SCALE(8), listTop = SCALE(68);
    int clientH, viewportH, contentH;
    SCROLLINFO si;

    GetClientRect(h, &rc);
    clientH = rc.bottom - rc.top;
    viewportH = (clientH - m - btnH - gap) - listTop;   /* 可视列表高度(滚动面板高) */
    contentH = g_rowCount * SCALE(64) + SCALE(8);       /* 全部行内容高度 */

    ZeroMemory(&si, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = contentH;
    si.nPage = (UINT)(viewportH > 0 ? viewportH : 1);
    si.nPos = g_scrollY;
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

static void layout_controls(HWND h)
{
    RECT rc;
    int m = SCALE(16), rowH = SCALE(64), gap = SCALE(8);
    int nameW = SCALE(150);
    int browseW = SCALE(64), installW = SCALE(96), removeW = SCALE(56);
    int btnH = SCALE(28), lineH = SCALE(28);
    int listTop = SCALE(68);
    int w, clientH, yBottom, i;
    int rightEdge, removeX, installX, browseX, editX, editW;

    GetClientRect(h, &rc);
    w = rc.right - rc.left;
    clientH = rc.bottom - rc.top;
    rightEdge = w - m;
    removeX = rightEdge - removeW;
    installX = removeX - gap - installW;
    browseX = installX - gap - browseW;
    editX = m + nameW + gap;
    editW = browseX - gap - editX;
    yBottom = clientH - m - btnH;

    /* 三段式:顶部菜单栏行(重新检测/高级)+ 标题固定,中部滚动面板裁剪行控件,底部状态固定 */
    if (g_hRedetect) MoveWindow(g_hRedetect, m, SCALE(6), SCALE(96), btnH, TRUE);
    if (g_hAdv) MoveWindow(g_hAdv, m + SCALE(96) + gap, SCALE(6), SCALE(80), btnH, TRUE);
    if (g_hHeader)
        MoveWindow(g_hHeader, m, SCALE(40), w - 2 * m, SCALE(20), TRUE);
    if (g_hList)
        MoveWindow(g_hList, 0, listTop, w, (yBottom - gap) - listTop, TRUE);

    for (i = 0; i < g_rowCount; i++) {
        int y = SCALE(4) + i * rowH - g_scrollY;   /* 相对列表面板,越界由面板裁剪 */
        if (g_name[i]) MoveWindow(g_name[i], m, y + (lineH - SCALE(20)) / 2, nameW, SCALE(20), TRUE);
        if (g_edit[i]) MoveWindow(g_edit[i], editX, y, editW, lineH, TRUE);
        if (g_browse[i]) MoveWindow(g_browse[i], browseX, y, browseW, btnH, TRUE);
        if (g_install[i]) MoveWindow(g_install[i], installX, y, installW, btnH, TRUE);
        if (g_remove[i]) MoveWindow(g_remove[i], removeX, y, removeW, btnH, TRUE);
        if (g_rowStatus[i]) MoveWindow(g_rowStatus[i], editX, y + lineH + SCALE(4), editW, SCALE(18), TRUE);
    }

    if (g_hStatus) MoveWindow(g_hStatus, m, yBottom + (btnH - SCALE(20)) / 2,
                              rightEdge - m, SCALE(20), TRUE);

    update_scrollbar(h);
}

/* ---------- 后台自动检测 ---------- */
/*
 * 扫描全部预设主程序路径是较重的磁盘遍历(约 1~2s),放后台线程逐行回填,
 * 让窗口立即出现;回填只在编辑框仍为空时生效,不覆盖用户已填的内容。
 */
static DWORD WINAPI detect_thread_fn(LPVOID param)
{
    HWND h = (HWND)param;
    int n = preset_count();

    for (int row = 0; row < n && !g_detectStop; row++) {
        wchar_t *path = (wchar_t *)malloc(MAX_PATH * sizeof(wchar_t));
        if (!path) break;
        if (detect_app(PRESETS[row].exeName, path, MAX_PATH)) {
            if (!PostMessageW(h, WM_APP_DETECT, (WPARAM)row, (LPARAM)path))
                free(path);   /* 窗口已销毁,消息未入队 */
        } else {
            free(path);
        }
    }
    return 0;
}

/* 确保有一个后台检测线程在跑;上一个已结束则重新启动 */
static void start_auto_detect(HWND h)
{
    if (g_detectThread) {
        if (WaitForSingleObject(g_detectThread, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_detectThread);
            g_detectThread = NULL;   /* 上一个线程已结束,可复用 */
        } else {
            return;                  /* 仍在跑,复用 */
        }
    }
    InterlockedExchange(&g_detectStop, 0);
    g_detectThread = CreateThread(NULL, 0, detect_thread_fn, h, 0, NULL);
    if (!g_detectThread) g_detectThread = NULL;
}

static void refresh_rows(HWND h)
{
    int i;
    build_rows(g_hList);   /* 行控件挂在滚动面板内,由面板裁剪、隔离底栏 */
    layout_controls(h);
    for (i = 0; i < g_rowCount; i++)
        update_status(i);
    for (i = preset_count(); i < g_rowCount; i++)
        fill_path(i);   /* 自定义行: 从会话记录恢复路径,免二次填写 */
    apply_font(h);      /* 重建的行控件统一应用雅黑字体 */
    start_auto_detect(h);   /* 后台逐行回填预设路径,窗口无需等待 */
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
    int tidx;
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
    {
        int cli = (row < preset_count()) ? PRESETS[row].cli : 0;
        if (row >= preset_count()) {
            tidx = find_target(name);
            if (tidx >= 0) cli = g_targets[tidx].cli;
        }
        if (install_target(name, codeExe, cli, g_installDir, buf, 4096, NULL) == 0) {
            tidx = find_target(name);
            if (tidx >= 0) {
                wcscpy_s(g_targets[tidx].exePath, MAX_PATH, codeExe);
                g_targets[tidx].cli = cli;
            } else if (g_targetCount < MAX_TARGETS) {
                wcscpy_s(g_targets[g_targetCount].name, 64, name);
                wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, codeExe);
                g_targets[g_targetCount].cli = cli;
                g_targetCount++;
            }
            SetWindowTextW(g_edit[row], codeExe);
        }
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
    if (uninstall_target(name, g_installDir, buf, 1024) == 0)
        remove_target_entry(name);
    SetWindowTextW(g_hStatus, buf);
    update_status(row);
}

static void remove_custom_row(int row)
{
    wchar_t name[64], buf[256];

    g_activeRow = row;
    if (!row_to_name(row, name, 64)) return;
    remove_target_entry(name);
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

/* ---------- 添加自定义对话框(非核心) ---------- */
static LRESULT CALLBACK add_wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(NULL);
        CreateWindowExW(0, L"STATIC", L"命令名:", WS_CHILD | WS_VISIBLE,
            SCALE(12), SCALE(14), SCALE(70), SCALE(20), h, NULL, hi, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, SCALE(86), SCALE(12), SCALE(210), SCALE(22),
            h, (HMENU)IDC_AD_NAME, hi, NULL);
        CreateWindowExW(0, L"STATIC", L"主程序:", WS_CHILD | WS_VISIBLE,
            SCALE(12), SCALE(44), SCALE(70), SCALE(20), h, NULL, hi, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, SCALE(86), SCALE(42), SCALE(210), SCALE(22),
            h, (HMENU)IDC_AD_PATH, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            SCALE(302), SCALE(42), SCALE(80), SCALE(22), h, (HMENU)IDC_AD_BROWSE, hi, NULL);
        CreateWindowExW(0, L"BUTTON",
            L"命令行工具 (CLI: 继承 cwd, 经 Windows Terminal 打开可见终端)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            SCALE(12), SCALE(74), SCALE(390), SCALE(22), h, (HMENU)IDC_AD_CLI, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            SCALE(170), SCALE(120), SCALE(80), SCALE(28), h, (HMENU)IDOK, hi, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            SCALE(270), SCALE(120), SCALE(80), SCALE(28), h, (HMENU)IDCANCEL, hi, NULL);
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
            ofn.lpstrFilter = L"应用程序 (*.exe;*.cmd)\0*.exe;*.cmd\0所有文件 (*.*)\0*.*\0";
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
                int cli = IsDlgButtonChecked(h, IDC_AD_CLI);
                int idx = find_target(nm);
                if (idx >= 0) {
                    wcscpy_s(g_targets[idx].exePath, MAX_PATH, path);
                    g_targets[idx].cli = cli;
                } else if (g_targetCount < MAX_TARGETS) {
                    wcscpy_s(g_targets[g_targetCount].name, 64, nm);
                    wcscpy_s(g_targets[g_targetCount].exePath, MAX_PATH, path);
                    g_targets[g_targetCount].cli = cli;
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
                        CW_USEDEFAULT, CW_USEDEFAULT, SCALE(430), SCALE(220),
                        parent, NULL, wc.hInstance, NULL);
    if (!h) return 0;
    g_addResult = 0;
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    apply_font(h);   /* 添加对话框各控件应用雅黑字体 */

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
        create_ui_font(h);   /* 先确定 g_dpi,供布局 SCALE 与字体使用 */
        g_hHeader = CreateWindowExW(0, L"STATIC",
            L"可用应用（路径自动检测，确认后点「安装」）：",
            WS_CHILD | WS_VISIBLE, 0, 0, 400, 20, h, NULL, hi, NULL);
        g_hRedetect = CreateWindowExW(0, L"BUTTON", L"重新检测",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 90, 26, h, (HMENU)IDC_REDETECT, hi, NULL);
        g_hAdv = CreateWindowExW(0, L"BUTTON", L"高级▾",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 70, 26, h, (HMENU)IDC_ADV, hi, NULL);
        g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 400, 26, h, (HMENU)IDC_STATUS, hi, NULL);
        g_hList = CreateWindowExW(0, g_listClass, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100, h, NULL, hi, NULL);
        refresh_rows(h);
        return 0;
    }
    case WM_SIZE:
        layout_controls(h);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(h, SB_VERT, &si);
        int oldPos = si.nPos;
        switch (LOWORD(w)) {
        case SB_LINEUP: si.nPos -= 20; break;
        case SB_LINEDOWN: si.nPos += 20; break;
        case SB_PAGEUP: si.nPos -= si.nPage; break;
        case SB_PAGEDOWN: si.nPos += si.nPage; break;
        case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        GetScrollInfo(h, SB_VERT, &si);
        if (si.nPos != oldPos) {
            g_scrollY = si.nPos;
            layout_controls(h);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int zDelta = GET_WHEEL_DELTA_WPARAM(w);
        SCROLLINFO si;
        ZeroMemory(&si, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        GetScrollInfo(h, SB_VERT, &si);
        int oldPos = si.nPos;
        si.nPos -= (zDelta / WHEEL_DELTA) * 40;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
        GetScrollInfo(h, SB_VERT, &si);
        if (si.nPos != oldPos) {
            g_scrollY = si.nPos;
            layout_controls(h);
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *prc = (RECT *)l;   /* 系统建议的新窗口矩形 */
        create_ui_font(h);       /* 按新 DPI 重建字体并更新 g_dpi */
        apply_font(h);
        if (prc)
            SetWindowPos(h, NULL, prc->left, prc->top,
                         prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        layout_controls(h);
        return 0;
    }
    case WM_APP_DETECT: {
        int row = (int)w;
        wchar_t *path = (wchar_t *)l;
        if (row >= 0 && row < g_rowCount && row < preset_count()
            && g_edit && g_edit[row]
            && GetWindowTextLengthW(g_edit[row]) == 0)
            SetWindowTextW(g_edit[row], path);
        free(path);
        return 0;
    }
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
            if (add_custom_dialog(h)) refresh_rows(h);
            return 0;
        case IDM_UNINSTALL:
            if (g_activeRow >= 0) uninstall_row(g_activeRow);
            else SetWindowTextW(g_hStatus, L"请先在某一行点击浏览或安装。");
            return 0;
        case IDM_ABOUT:
            MessageBoxW(h, L"openin — 通用「打开到应用」启动器安装器\n\n"
                          L"把应用注入地址栏: 输入命令,以当前文件夹为参数打开。",
                        L"关于 openin", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_detectStop, 1);
        if (g_detectThread) {
            WaitForSingleObject(g_detectThread, 3000);
            CloseHandle(g_detectThread);
            g_detectThread = NULL;
        }
        destroy_rows();
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int gui_main(void)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;

    pick_target_dir(NULL, g_installDir, MAX_PATH);   /* 无状态: 每次自动选择安装目录 */

    InitCommonControls();                 /* 配合 v6 manifest 启用现代主题控件 */
    g_dpi = (int)GetDpiForSystem();       /* 先用系统 DPI 估算初始窗口尺寸 */
    if (g_dpi <= 0) g_dpi = 96;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = g_winClass;
    RegisterClassW(&wc);

    ZeroMemory(&wc, sizeof(wc));          /* 滚动列表面板:裁剪行控件、隔离底栏 */
    wc.lpfnWndProc = list_wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = g_listClass;
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(0, g_winClass, L"openin — 打开到应用",
                           WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, SCALE(760), SCALE(584),
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

