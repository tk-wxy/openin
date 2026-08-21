/*
 * openin.h — 跨模块接口
 *   utils.c   纯工具(路径/校验/对话框)
 *   pathenv.c 用户 PATH 与安装目录选择
 *   detect.c  应用自动检索(App Paths 注册表/PATH/磁盘)
 *   core.c    安装/卸载引擎、launcher 生成、预设模板
 *   gui.c     Win32 GUI(只暴露 gui_main)
 *   openin.c  入口:无参数进 GUI,有参数走 CLI
 * 依赖方向(单向):utils ← pathenv ← core;utils ← detect;core/gui/openin ← ...
 */
#ifndef OPENIN_H
#define OPENIN_H

#include <windows.h>

/* ---------- 版本信息(单一事实源) ----------
 * ⚠️ 改版本号必须三处同步: ①本宏 ②src/openin.rc 的 FILEVERSION/PRODUCTVERSION 与
 * StringFileInfo ③src/openin.manifest 的 assemblyIdentity version。 */
#define OPENIN_VERSION_MAJOR  2
#define OPENIN_VERSION_MINOR  1
#define OPENIN_VERSION_PATCH  0
#define OPENIN_VERSION_STR    "2.1.0"

/* ---------- 共享类型与全局(core.c 持有) ---------- */
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
    int native;              /* 1 = 原生地址栏 CLI(如 claude): openin 不创建/不卸载其命令,安装按钮改为「修复」 */
    const wchar_t *args;     /* 固定参数(NULL = 无): 插在目标与用户参数之间,如 dsh → npm exec ... --call "dsh web" */
    const wchar_t *url;      /* Web 工具 URL(NULL = 无): launcher 静默后台启动 server 后自动用默认浏览器打开该 URL */
    int appPaths;            /* 1 = 需写 App Paths 注册表(解决 npm 裸文件拦截,如 codex);0 = 纯 PATH(如 dsh/vscode) */
} Preset;

extern const Preset PRESETS[];
extern wchar_t g_installDir[MAX_PATH];   /* 每次运行由 pick_target_dir 计算,不落盘 */
extern Target g_targets[MAX_TARGETS];    /* 自定义目标,仅会话内存 */
extern int g_targetCount;

/* ---------- utils.c ---------- */
BOOL browse_for_folder(wchar_t *out, size_t out_sz);
BOOL path_exists(const wchar_t *p);
BOOL locate_main_exe(const wchar_t *folder, const wchar_t *exeName,
                     wchar_t *out, size_t out_sz);
int  valid_name(const wchar_t *s);

/* ---------- pathenv.c ---------- */
BOOL add_to_user_path(const wchar_t *dir);
BOOL remove_from_user_path(const wchar_t *dir);
BOOL path_in_environment(const wchar_t *dir);
void pick_target_dir(const wchar_t *override, wchar_t *out, size_t out_sz);
BOOL find_in_path(const wchar_t *file, wchar_t *out, size_t out_sz);

/* ---------- detect.c ---------- */
BOOL detect_app(const wchar_t *exeName, wchar_t *out, size_t out_sz);
int  deep_scan_presets(wchar_t found[][MAX_PATH], volatile LONG *stop);

/* ---------- core.c ---------- */
int  preset_count(void);
int  find_target(const wchar_t *name);
void remove_target_entry(const wchar_t *name);
BOOL target_installed(const wchar_t *name, const wchar_t *installDir);
int  list_installed(const wchar_t *installDir, wchar_t *out, size_t outSz);
typedef void (*StepCb)(const wchar_t *msg);   /* 操作步骤回调: 安装/卸载时逐条通报,可为 NULL */
int  install_target(const wchar_t *name, const wchar_t *codeExe, const wchar_t *args,
                    const wchar_t *url, int cli, const wchar_t *installDir,
                    wchar_t *outSummary, size_t sumSz, int *outAddedPath, StepCb onStep);
int  uninstall_target(const wchar_t *name, const wchar_t *installDir,
                      wchar_t *outSummary, size_t sumSz, StepCb onStep);

/* ---------- gui.c ---------- */
int  gui_main(void);

#endif /* OPENIN_H */
