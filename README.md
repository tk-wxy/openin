# openin — 通用「打开到应用」启动器安装器

在**资源管理器地址栏**或任意终端里输入一个命令（如 `vscode`），用指定应用打开当前文件夹。

openin 的核心思路：**把任意工具注入地址栏** —— 选择一个应用（VS Code、Cursor、Claude、
Codex……），安装后即可用一行命令以「当前文件夹」为参数打开它，哪怕该应用本身没有实现
地址栏打开。

## 文件

| 文件 | 作用 |
|---|---|
| `openin.exe` | 安装器（双击运行） |
| `openin.c` | 入口（无参数进 GUI，有参数走 CLI） |
| `core.c` | 引擎：安装/卸载、launcher 生成 |
| `detect.c` | 应用自动检索 |
| `pathenv.c` | PATH / 安装目录操作 |
| `utils.c` | 路径/校验工具 |
| `gui.c` | GUI 窗口逻辑 |
| `openin.h` | 跨模块接口 |

## 使用

### GUI 界面（推荐）
双击 `openin.exe`（或无参数运行）打开**管理主窗口**：每个应用一行（名称 + 路径框 + 浏览 + 安装）。
窗口打开即**自动检索**并填入主程序路径，确认无误后点「安装」即可。
- 底部「重新检测」重新扫描路径；「高级▾」菜单含卸载、添加自定义
- **无状态**：只在安装目录创建启动文件（`vscode.exe`/`.cmd`），**不产生任何配置文件、日志或外部文件夹**；
  自定义目标仅存于会话内存，关闭即失

### 命令行模式
```bat
openin.exe                                  :: 打开 GUI 主窗口
openin.exe -d "D:\Microsoft VS Code"        :: 指定应用目录安装（默认命令名 vscode）
openin.exe -d "D:\Microsoft VS Code" code   :: 自定义命令名
openin.exe -p C:\Users\you\bin -d "D:\VS Code"   :: 指定安装目录
openin.exe -u vscode                        :: 卸载指定命令
openin.exe -q -d "D:\..."                   :: 静默模式（不弹窗）
```

> 每个应用装一次，得到一个独立命令：`openin.exe -d "D:\...\Code.exe的目录" vscode`、
> `openin.exe -d "D:\...\cursor" cursor`、`openin.exe -d "D:\...\claude" claude`……

## 从源码构建

需要 **MinGW-w64（gcc）**，并确保 `gcc` 在 PATH 中：

```bat
:: 安装 gcc（已装则跳过）
winget install BrechtSanders.WinLibs.POSIX.UCRT

:: 编译
gcc -O2 -s -municode -mwindows -o openin.exe openin.c core.c detect.c pathenv.c utils.c gui.c -lshell32 -lole32 -lcomdlg32
```

编译参数说明：

| 参数 | 作用 |
|---|---|
| `-municode` | 使用 `wmain` 宽字符入口，正确处理中文 / Unicode 路径 |
| `-mwindows` | GUI 子系统，运行时不弹出黑框 |
| `-O2 -s` | 优化并去符号瘦身（产物约 101 KB） |
| `-lshell32 -lole32` | 文件夹选择对话框与 COM 所需系统库 |
| `-lcomdlg32` | 文件选择对话框（GetOpenFileName） |

产物 `openin.exe` 是**单文件便携程序**，可拷到任何 Windows 机器直接双击运行，
目标机器无需安装编译器。

## 安装位置（自动选择通用目录）

启动器**不放进专属文件夹**，而是自动挑选一个通用、常用的用户 bin 目录（这些目录通常已在
PATH 中，因此多数情况**无需修改 PATH**）：

| 优先级 | 目录 | 说明 |
|---|---|---|
| 1 | `-p <目录>` | 显式指定，完全自定义 |
| 2 | `%USERPROFILE%\.local\bin` | GitHub 风格用户 bin（claude 等工具在此），**已在 PATH 则跳过修改** |
| 3 | `%APPDATA%\npm` | Node 全局工具目录（codex 等在此），已在 PATH 则跳过修改 |
| 4 | 创建 `%USERPROFILE%\.local\bin` | 目录不存在时自动创建 |
| 5 | 回退 `%LOCALAPPDATA%\Programs\openin` | 上述都不可用时才使用专属目录 |

- 所选目录**已在 PATH** → 不改动任何环境变量，仅放入文件
- 所选目录**不在 PATH**（如自定义 `-p`）→ 自动追加到**用户** PATH（HKCU，无需管理员权限）

生成的产物直接落在该目录：`<name>.exe`（始终，见下节）与 `<name>.cmd`（始终）。
源码与编译日志走临时目录、用完即删，**目标目录零残留**。

## 双保险（冗余）机制

每次安装**始终生成** `<name>.cmd` 批处理备用启动器（无需编译器），
并**始终生成** `<name>.exe`：核心是**免编译器**的「二进制 Patch」——程序内置预编译好的
launcher 模板（约 22 KB），把其中预留的路径占位符原地替换为目标应用路径，毫秒级完成，
任何机器都能用。仅当 Patch 意外失败且系统装有 `gcc` 时，才降级为源码编译生成 `.exe`。

| 生成物 | 生成方式 | 终端输入 | 地址栏输入 |
|---|---|---|---|
| `<name>.exe` | 二进制 Patch（免 gcc）；失败且有 gcc 时源码编译 | `vscode`（PATHEXT 优先 exe） | `vscode` 或 `vscode.cmd` |
| `<name>.cmd` | 始终生成（纯批处理兜底） | `vscode` | `vscode.cmd`（地址栏不解析 PATHEXT，需带扩展名） |

> `.cmd` 按系统 ANSI 代码页（中文系统为 GBK）写入，含中文的路径也能被 cmd 正确解析。
> 若应用路径含 `% ^ &` 字符，批处理内会自动转义。

## 卸载

1. 删除安装目录里的 `<name>.exe`、`<name>.cmd`
2. 仅当 openin 当时**自行加入过 PATH**（目录原本不在 PATH，通常是自定义 `-p` 或回退场景），
   再在「设置 → 系统 → 系统信息 → 高级系统设置 → 环境变量 → 用户变量 → Path」里删除该目录

## 特性

- 一个工具注入任意应用，每个应用一个独立命令，互不冲突
- 安装在通用用户 bin 目录，已在 PATH 则完全不动环境变量，卸载只删几个文件
- 冗余：`.exe` 免编译器生成（二进制 Patch），`.cmd` 永远兜底
- 幂等：重复安装会覆盖同名文件，不产生重复 PATH 条目
- 冲突检测：同时检查 PATH 中是否已有同名 `.exe` / `.cmd`，有则提示优先解析到的位置
- 兼容选中 `bin` 子目录：自动向上找到上级的主程序（如 `Code.exe`）
- 支持参数透传：`vscode . -r`（复用窗口）、`vscode D:\some\file.txt`
- 两类预设：**GUI 应用**（vscode，传目录参数）与**命令行工具**（codex，继承 cwd、不传目录参数、经 Windows Terminal 打开可见终端运行）

## 路线图

- [x] VS Code 支持（首个应用）
- [x] 预设应用模板（18 个：16 GUI + 2 CLI）
- [x] 图形界面管理：多目标列表、安装/更新、卸载、添加自定义、移除、刷新
- [x] 无 gcc 环境默认分发预编译 launcher（二进制 Patch）
- [ ] 系统托盘常驻 / 开机自启动

## 故障排查

- **无 gcc**：`<name>.exe` 由内置模板二进制 Patch 生成，无需编译器；仅 Patch 失败且无 gcc 时才退化为纯 `.cmd` 模式
- **编译失败**：不影响使用，会保留 `.cmd` 备用并给出 gcc 输出
- **换个应用/路径**：重新运行选择新目录即可（会重新生成文件，PATH 通常不用动）
- **新窗口不生效**：已打开的终端/资源管理器需重启；Explorer 会自动刷新（WM_SETTINGCHANGE）

## 依赖

- **gcc 可选**：仅作为二进制 Patch 失败时的兜底编译手段（MinGW-w64）；无 gcc 也能正常工作
- openin 本身是预编译 exe，运行它不需要任何编译器
