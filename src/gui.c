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
#include <uxtheme.h>      /* SetWindowTheme: 编辑框现代扁平边框 */
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
    IDC_DEEPSCAN = 105,
    IDC_ROW_EDIT = 500,
    IDC_ROW_BROWSE = 600,
    IDC_ROW_INSTALL = 700,
    IDC_ROW_REMOVE = 800,
    IDC_ROW_TEST = 900,
    IDC_ROW_UNINSTALL = 1000,
    IDM_ADD = 401,
    IDM_ABOUT = 405,
    IDC_AD_NAME = 200,
    IDC_AD_PATH = 201,
    IDC_AD_BROWSE = 202,
    IDC_AD_CLI = 203
};

static HWND g_hMain, g_hHeader, g_hRedetect, g_hAdv, g_hDeepScan, g_hStatus, g_hList;
static HWND *g_name, *g_edit, *g_browse, *g_install, *g_remove, *g_test, *g_uninstall, *g_rowStatus;
static int g_rowCount = 0;
static const wchar_t *g_winClass = L"openin_main";
static const wchar_t *g_listClass = L"openin_list";
static int g_addResult = 0;

#define WM_APP_DETECT (WM_APP + 1)      /* 后台检测线程回填单行路径 */
#define WM_APP_DEEPSCAN (WM_APP + 2)    /* 深度扫描线程回填全部预设路径 */
static HANDLE g_detectThread = NULL;    /* 后台自动检测线程(仅主线程访问) */
static volatile LONG g_detectStop = 0;  /* 通知后台线程停止 */
static HANDLE g_deepThread = NULL;      /* 深度扫描线程 */
static volatile LONG g_deepStop = 0;    /* 通知深度扫描线程停止 */

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

/* 该预设是否为「原生地址栏 CLI」(openin 不创建/不卸载其命令,安装按钮改为「修复」) */
static BOOL is_native_row(int row)
{
    return row >= 0 && row < preset_count() && PRESETS[row].native != 0;
}

/* 该命令的 launcher 文件是否已安装(见 core.c target_installed) */
static BOOL is_installed(const wchar_t *name)
{
    return target_installed(name, g_installDir);
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
        if (g_test[i]) DestroyWindow(g_test[i]);
        if (g_uninstall[i]) DestroyWindow(g_uninstall[i]);
        if (g_rowStatus[i]) DestroyWindow(g_rowStatus[i]);
    }
    if (g_name) {
        free(g_name); free(g_edit); free(g_browse);
        free(g_install); free(g_remove); free(g_test); free(g_uninstall); free(g_rowStatus);
    }
    g_name = g_edit = g_browse = g_install = g_remove = g_test = g_uninstall = g_rowStatus = NULL;
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
    g_test = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_uninstall = (HWND *)calloc((size_t)n, sizeof(HWND));
    g_rowStatus = (HWND *)calloc((size_t)n, sizeof(HWND));
    if (!g_name || !g_edit || !g_browse || !g_install || !g_remove || !g_test || !g_uninstall || !g_rowStatus)
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
        g_edit[i] = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 0, 0, 100, 22, h,
            (HMENU)(INT_PTR)(IDC_ROW_EDIT + i), hi, NULL);
        if (g_edit[i]) SetWindowTheme(g_edit[i], L"Explorer", NULL);  /* 现代扁平边框(替代经典 3D 凹陷) */
        g_browse[i] = CreateWindowExW(0, L"BUTTON", L"浏览",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 60, 24, h,
            (HMENU)(INT_PTR)(IDC_ROW_BROWSE + i), hi, NULL);
        g_install[i] = CreateWindowExW(0, L"BUTTON", L"安装",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 90, 26, h,
            (HMENU)(INT_PTR)(IDC_ROW_INSTALL + i), hi, NULL);
        g_test[i] = CreateWindowExW(0, L"BUTTON", L"测试",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 56, 26, h,
            (HMENU)(INT_PTR)(IDC_ROW_TEST + i), hi, NULL);
        g_uninstall[i] = CreateWindowExW(0, L"BUTTON", L"卸载",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 56, 26, h,
            (HMENU)(INT_PTR)(IDC_ROW_UNINSTALL + i), hi, NULL);
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
    if (is_native_row(row)) {
        /* 原生 CLI: 命令二进制在安装目录才可用;状态与按钮反映「修复」而非 openin 安装 */
        wchar_t dest[MAX_PATH];
        _snwprintf_s(dest, MAX_PATH, MAX_PATH - 1, L"%s\\%s", g_installDir, PRESETS[row].exeName);
        wcscpy_s(st, 64, path_exists(dest) ? L"原生命令" : L"原生命令(缺失)");
        SetWindowTextW(g_rowStatus[row], st);
        SetWindowTextW(g_install[row], L"修复");
        if (g_uninstall[row]) EnableWindow(g_uninstall[row], FALSE);   /* 原生不可卸载 */
        return;
    }
    target_status(name, st, 64);
    SetWindowTextW(g_rowStatus[row], st);
    SetWindowTextW(g_install[row], is_installed(name) ? L"更新" : L"安装");
    if (g_uninstall[row]) EnableWindow(g_uninstall[row], TRUE);
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
    GetScrollInfo(h, SB_VERT, &si);          /* 读回系统按范围钳制后的位置 */
    g_scrollY = (int)si.nPos;
}

static void layout_controls(HWND h)
{
    RECT rc;
    int m = SCALE(16), rowH = SCALE(64), gap = SCALE(8);
    int nameW = SCALE(150);
    int browseW = SCALE(64), installW = SCALE(96), testW = SCALE(56), removeW = SCALE(56), uninstallW = SCALE(56);
    int btnH = SCALE(28), lineH = SCALE(28);
    int listTop = SCALE(68);
    int w, clientH, viewportH, yBottom, i;
    int rightEdge, removeX, uninstallX, testX, installX, browseX, editX, editW;
    int first, last;
    HDWP hdwp;

    update_scrollbar(h);   /* 先按范围钳制 g_scrollY, 供本次布局使用 */

    GetClientRect(h, &rc);
    w = rc.right - rc.left;
    clientH = rc.bottom - rc.top;
    rightEdge = w - m;
    uninstallX = rightEdge - uninstallW;
    removeX = uninstallX - gap - removeW;
    testX = removeX - gap - testW;
    installX = testX - gap - installW;
    browseX = installX - gap - browseW;
    editX = m + nameW + gap;
    editW = browseX - gap - editX;
    yBottom = clientH - m - btnH;
    viewportH = (yBottom - gap) - listTop;   /* 滚动面板可视高度 */

    /* 可见行范围(含上/下一行余量,跨界行也参与重排) */
    first = (g_scrollY - rowH) / rowH;
    if (first > 0) first--;
    if (first < 0) first = 0;
    last = (g_scrollY + viewportH + rowH) / rowH;
    if (last >= g_rowCount) last = g_rowCount - 1;
    if (first > last) first = last;   /* 内容不足一屏时 */

    /* 批量移动一次成型: 避免每行 MoveWindow 各自触发重绘,滚动大幅提速。
     * 注意: 同一批次不能同时移动父窗口和它的子控件(EndDeferWindowPos 会静默不生效),
     * 因此主窗口子级与列表面板内的行控件分两个批次。 */
    hdwp = BeginDeferWindowPos(8);
    if (hdwp) {
        const UINT df = SWP_NOZORDER | SWP_NOACTIVATE;
        /* 三段式:顶部菜单栏行(重新检测/高级)+ 标题固定,中部滚动面板裁剪行控件,底部状态固定 */
        if (g_hRedetect)
            DeferWindowPos(hdwp, g_hRedetect, NULL, m, SCALE(6), SCALE(96), btnH, df);
        if (g_hDeepScan)
            DeferWindowPos(hdwp, g_hDeepScan, NULL, m + SCALE(96) + gap, SCALE(6), SCALE(96), btnH, df);
        if (g_hAdv)
            DeferWindowPos(hdwp, g_hAdv, NULL, m + (SCALE(96) + gap) * 2, SCALE(6), SCALE(80), btnH, df);
        if (g_hHeader)
            DeferWindowPos(hdwp, g_hHeader, NULL, m, SCALE(40), w - 2 * m, SCALE(20), df);
        if (g_hList)
            DeferWindowPos(hdwp, g_hList, NULL, 0, listTop, w, (yBottom - gap) - listTop, df);
        if (g_hStatus)
            DeferWindowPos(hdwp, g_hStatus, NULL, m, yBottom + (btnH - SCALE(20)) / 2, rightEdge - m, SCALE(20), df);
        EndDeferWindowPos(hdwp);
    }

    hdwp = BeginDeferWindowPos(g_rowCount * 8 + 8);
    if (hdwp) {
        const UINT df = SWP_NOZORDER | SWP_NOACTIVATE;
        for (i = 0; i < g_rowCount; i++) {
            /* 可视行按滚动定位; 屏外行统一移到固定屏外位置——否则大幅跳转(如拖到底)
             * 后, 未重排的行会残留旧位置落入可视区。屏外行位置不变, Defer 为廉价空操作 */
            int y = (i >= first && i <= last) ? SCALE(4) + i * rowH - g_scrollY : -4 * rowH;
            if (g_name[i])
                DeferWindowPos(hdwp, g_name[i], NULL, m, y + (lineH - SCALE(20)) / 2, nameW, SCALE(20), df);
            if (g_edit[i])
                DeferWindowPos(hdwp, g_edit[i], NULL, editX, y, editW, lineH, df);
            if (g_browse[i])
                DeferWindowPos(hdwp, g_browse[i], NULL, browseX, y, browseW, btnH, df);
            if (g_install[i])
                DeferWindowPos(hdwp, g_install[i], NULL, installX, y, installW, btnH, df);
            if (g_test[i])
                DeferWindowPos(hdwp, g_test[i], NULL, testX, y, testW, btnH, df);
            if (g_uninstall[i])
                DeferWindowPos(hdwp, g_uninstall[i], NULL, uninstallX, y, uninstallW, btnH, df);
            if (g_remove[i])
                DeferWindowPos(hdwp, g_remove[i], NULL, removeX, y, removeW, btnH, df);
            if (g_rowStatus[i])
                DeferWindowPos(hdwp, g_rowStatus[i], NULL, editX, y + lineH + SCALE(4), editW, SCALE(18), df);
        }
        EndDeferWindowPos(hdwp);
    }
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

/* ---------- 深度扫描(按钮触发,单遍遍历,剪枝控时) ---------- */
static DWORD WINAPI deep_scan_thread_fn(LPVOID param)
{
    HWND h = (HWND)param;
    int n = preset_count();
    wchar_t(*found)[MAX_PATH] = (wchar_t(*)[MAX_PATH])
        malloc((size_t)n * MAX_PATH * sizeof(wchar_t));
    int count;

    if (!found) return 0;
    count = deep_scan_presets(found, &g_deepStop);
    if (!PostMessageW(h, WM_APP_DEEPSCAN, (WPARAM)count, (LPARAM)found))
        free(found);   /* 窗口已销毁,消息未入队 */
    return 0;
}

/* 点击「深度扫描」: 起后台线程跑全量深扫,防重入 */
static void start_deep_scan(HWND h)
{
    if (g_deepThread) {
        if (WaitForSingleObject(g_deepThread, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_deepThread);
            g_deepThread = NULL;   /* 上一个已结束,可复用 */
        } else {
            SetWindowTextW(g_hStatus, L"深度扫描正在进行,请稍候…");
            return;
        }
    }
    InterlockedExchange(&g_deepStop, 0);
    g_deepThread = CreateThread(NULL, 0, deep_scan_thread_fn, h, 0, NULL);
    if (!g_deepThread) g_deepThread = NULL;
    SetWindowTextW(g_hStatus, L"正在深度扫描应用路径…");
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

/* 安装/卸载步骤闪显(左下角状态栏): 同步重绘不泵消息,无重入风险;100ms 短暂停留肉眼可读 */
static void install_step_cb(const wchar_t *msg)
{
    SetWindowTextW(g_hStatus, msg);
    UpdateWindow(g_hStatus);
    Sleep(100);
}

/* 原生 CLI 修复: 确保 安装目录\<exeName> 存在真实二进制(复制自核心程序位置),不覆盖非 openin 内容 */
static void repair_row(int row)
{
    wchar_t name[64], src[MAX_PATH], dst[MAX_PATH], st[300];

    if (!row_to_name(row, name, 64)) return;
    GetWindowTextW(g_edit[row], src, MAX_PATH);
    _snwprintf_s(dst, MAX_PATH, MAX_PATH - 1, L"%s\\%s", g_installDir, PRESETS[row].exeName);

    if (!src[0]) {
        SetWindowTextW(g_hStatus, L"核心程序路径为空,请先浏览选择或深度扫描检出。");
        return;
    }
    if (_wcsicmp(src, dst) == 0) {
        SetWindowTextW(g_hStatus, L"命令已在正确位置,无需修复。");
        return;
    }
    if (path_exists(dst)) {
        _snwprintf_s(st, 300, 299, L"目标已存在,已跳过(不覆盖非 openin 内容):\n%s", dst);
        SetWindowTextW(g_hStatus, st);
        return;
    }
    if (CopyFileW(src, dst, TRUE)) {
        _snwprintf_s(st, 300, 299, L"已修复: 已将 %s 恢复到\n%s", PRESETS[row].exeName, dst);
        SetWindowTextW(g_edit[row], dst);
    } else {
        _snwprintf_s(st, 300, 299, L"修复失败: 复制 %s 到 %s 出错。", PRESETS[row].exeName, dst);
    }
    SetWindowTextW(g_hStatus, st);
    update_status(row);
}

static void install_row(int row)
{
    wchar_t name[64], path[MAX_PATH], codeExe[MAX_PATH], buf[4096];
    int tidx;
    DWORD attrs;

    if (!row_to_name(row, name, 64)) return;
    if (is_native_row(row)) { repair_row(row); return; }   /* 原生 CLI: 修复而非安装 */
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
        if (install_target(name, codeExe, (row < preset_count()) ? PRESETS[row].args : L"",
                           (row < preset_count()) ? PRESETS[row].url : NULL,
                           cli, g_installDir, buf, 4096, NULL, install_step_cb) == 0) {
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

    if (!row_to_name(row, name, 64)) return;
    if (is_native_row(row)) {
        SetWindowTextW(g_hStatus, L"原生实现,openin 无法卸载。");
        return;
    }
    if (uninstall_target(name, g_installDir, buf, 1024, install_step_cb) == 0)
        remove_target_entry(name);
    SetWindowTextW(g_hStatus, buf);
    update_status(row);
}

/* 测试启动: 已安装则拉起 launcher 实测(等价于地址栏/终端敲命令) */
static void test_row(int row)
{
    wchar_t name[64], exePath[MAX_PATH], cmdPath[MAX_PATH];
    wchar_t cmdline[2 * MAX_PATH + 16], st[600];
    int cli;
    DWORD flags;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!row_to_name(row, name, 64)) return;
    if (!target_installed(name, g_installDir)) {
        SetWindowTextW(g_hStatus, L"未安装,请先点「安装」再测试。");
        return;
    }

    cli = (row < preset_count()) ? PRESETS[row].cli : 0;
    if (row >= preset_count()) {
        int tidx = find_target(name);
        if (tidx >= 0) cli = g_targets[tidx].cli;
    }

    _snwprintf_s(exePath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.exe", g_installDir, name);
    _snwprintf_s(cmdPath, MAX_PATH, MAX_PATH - 1, L"%s\\%s.cmd", g_installDir, name);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    /* GUI 目标无需控制台;CLI 目标新建控制台,子进程继承显示 TUI(等价于终端敲命令) */
    flags = cli ? CREATE_NEW_CONSOLE : 0;

    if (path_exists(exePath)) {
        _snwprintf_s(cmdline, 2 * MAX_PATH + 16, 2 * MAX_PATH + 15, L"\"%s\"", exePath);
    } else if (path_exists(cmdPath)) {
        _snwprintf_s(cmdline, 2 * MAX_PATH + 16, 2 * MAX_PATH + 15,
                     L"cmd.exe /c \"\"%s\"\"", cmdPath);
    } else {
        SetWindowTextW(g_hStatus, L"未找到 launcher 文件。");
        return;
    }

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, flags,
                        NULL, NULL, &si, &pi)) {
        _snwprintf_s(st, 600, 599, L"启动失败(%lu)。", (unsigned long)GetLastError());
        SetWindowTextW(g_hStatus, st);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    _snwprintf_s(st, 600, 599, L"已启动 \"%s\",请确认目标应用是否打开。", name);
    SetWindowTextW(g_hStatus, st);
}

static void remove_custom_row(int row)
{
    wchar_t name[64], buf[256];

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
        HWND ed;
        CreateWindowExW(0, L"STATIC", L"命令名:", WS_CHILD | WS_VISIBLE,
            SCALE(12), SCALE(14), SCALE(70), SCALE(20), h, NULL, hi, NULL);
        ed = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, SCALE(86), SCALE(12), SCALE(210), SCALE(22),
            h, (HMENU)IDC_AD_NAME, hi, NULL);
        if (ed) SetWindowTheme(ed, L"Explorer", NULL);
        CreateWindowExW(0, L"STATIC", L"主程序:", WS_CHILD | WS_VISIBLE,
            SCALE(12), SCALE(44), SCALE(70), SCALE(20), h, NULL, hi, NULL);
        ed = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, SCALE(86), SCALE(42), SCALE(210), SCALE(22),
            h, (HMENU)IDC_AD_PATH, hi, NULL);
        if (ed) SetWindowTheme(ed, L"Explorer", NULL);
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
    RECT rc;

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
        g_hDeepScan = CreateWindowExW(0, L"BUTTON", L"深度扫描",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 90, 26, h, (HMENU)IDC_DEEPSCAN, hi, NULL);
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
    case WM_APP_DEEPSCAN: {
        int count = (int)w, filled = 0, i;
        wchar_t(*found)[MAX_PATH] = (wchar_t(*)[MAX_PATH])l;
        wchar_t st[200];
        for (i = 0; i < preset_count() && i < g_rowCount; i++) {
            if (found[i][0] && g_edit && g_edit[i]
                && GetWindowTextLengthW(g_edit[i]) == 0) {
                SetWindowTextW(g_edit[i], found[i]);
                filled++;
            }
        }
        _snwprintf_s(st, 200, 199, L"深度扫描完成: 检出 %d 个预设路径,新填充 %d 行。", count, filled);
        SetWindowTextW(g_hStatus, st);
        free(found);
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
        if (LOWORD(w) >= IDC_ROW_TEST && LOWORD(w) < IDC_ROW_TEST + g_rowCount) {
            test_row(LOWORD(w) - IDC_ROW_TEST);
            return 0;
        }
        if (LOWORD(w) >= IDC_ROW_REMOVE && LOWORD(w) < IDC_ROW_REMOVE + g_rowCount) {
            remove_custom_row(LOWORD(w) - IDC_ROW_REMOVE);
            return 0;
        }
        if (LOWORD(w) >= IDC_ROW_UNINSTALL && LOWORD(w) < IDC_ROW_UNINSTALL + g_rowCount) {
            uninstall_row(LOWORD(w) - IDC_ROW_UNINSTALL);
            return 0;
        }
        switch (LOWORD(w)) {
        case IDC_REDETECT: re_detect_all(); return 0;
        case IDC_DEEPSCAN: start_deep_scan(h); return 0;
        case IDC_ADV: show_adv_menu(h); return 0;
        case IDM_ADD:
            if (add_custom_dialog(h)) refresh_rows(h);
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
        InterlockedExchange(&g_deepStop, 1);
        if (g_deepThread) {
            WaitForSingleObject(g_deepThread, 5000);
            CloseHandle(g_deepThread);
            g_deepThread = NULL;
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

