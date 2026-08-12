/*
 * openin.h — 跨模块接口(引擎 openin.c ⇄ GUI gui.c)
 * 引擎(openin.c): 安装/卸载、launcher 生成、检测、PATH/路径操作,不碰 HWND。
 * GUI(gui.c): 全部 Win32 窗口逻辑,依赖引擎导出,只暴露 gui_main。
 */
#ifndef OPENIN_H
#define OPENIN_H

#include <windows.h>

/* ---------- 共享类型与全局(引擎持有,见 openin.c) ---------- */
#define MAX_TARGETS 64

typedef struct {
    wchar_t name[64];
    wchar_t exePath[MAX_PATH];
    int cli;
} Target;

typedef struct {
    const wchar_t *name;     /* 命令名 */
    const wchar_t *exeName;  /* 主程序文件名(用于定位);cli 时为主命令的 shim 名 */
    const wchar_t *display;  /* 界面显示名 */
    int cli;                 /* 1 = 命令行工具: 继承 cwd,不传目录参数 */
} Preset;

extern const Preset PRESETS[];
extern wchar_t g_installDir[MAX_PATH];   /* 每次运行由 pick_target_dir 计算,不落盘 */
extern Target g_targets[MAX_TARGETS];    /* 自定义目标,仅会话内存 */
extern int g_targetCount;

/* ---------- 引擎导出函数 ---------- */
int  preset_count(void);
BOOL browse_for_folder(wchar_t *out, size_t out_sz);
BOOL path_exists(const wchar_t *p);
BOOL locate_main_exe(const wchar_t *folder, const wchar_t *exeName,
                     wchar_t *out, size_t out_sz);
BOOL path_in_environment(const wchar_t *dir);
void pick_target_dir(const wchar_t *override, wchar_t *out, size_t out_sz);
int  valid_name(const wchar_t *s);
int  find_target(const wchar_t *name);
void remove_target_entry(const wchar_t *name);
int  install_target(const wchar_t *name, const wchar_t *codeExe, int cli,
                    const wchar_t *installDir,
                    wchar_t *outSummary, size_t sumSz, int *outAddedPath);
int  uninstall_target(const wchar_t *name, const wchar_t *installDir,
                      wchar_t *outSummary, size_t sumSz);
BOOL detect_app(const wchar_t *exeName, wchar_t *out, size_t out_sz);

/* ---------- GUI 导出 ---------- */
int  gui_main(void);

#endif /* OPENIN_H */
