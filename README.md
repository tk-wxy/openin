# openin — 通用「打开到应用」启动器安装器

在**资源管理器地址栏**或任意终端里输入一个命令（如 `vscode`），用指定应用打开当前文件夹。

openin 的核心思路：**把任意工具注入地址栏** —— 选择一个应用（VS Code、Cursor、Claude、
Codex……），安装后即可用一行命令以「当前文件夹」为参数打开它，哪怕该应用本身没有实现
地址栏打开。

## 文件

| 文件 | 作用 |
|---|---|
| `openin.exe` | 安装器（双击运行） |
| `openin.c` | 安装器源码 |

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
gcc -O2 -s -municode -mwindows -o openin.exe openin.c -lshell32 -lole32 -lcomdlg32
```

编译参数说明：

| 参数 | 作用 |
|---|---|
| `-municode` | 使用 `wmain` 宽字符入口，正确处理中文 / Unicode 路径 |
| `-mwindows` | GUI 子系统，运行时不弹出黑框 |
| `-O2 -s` | 优化并去符号瘦身（产物约 51 KB） |
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

生成的产物直接落在该目录：`<name>.exe`（二进制 Patch，无需 gcc）与 `<name>.cmd`（始终）。
源码与编译日志走临时目录、用完即删，**目标目录零残留**。

## 双保险（冗余）机制

每次安装**始终生成** `<name>.cmd` 批处理备用启动器（无需编译器）；`<name>.exe` 默认通过
**二进制 Patch** 预编译模板生成（毫秒级、无需 gcc），仅当 Patch 失败且系统有 gcc 时才降级编译。

| 情况 | 生成物 | 终端输入 | 地址栏输入 |
|---|---|---|---|
| 默认（二进制 Patch） | `.exe` + `.cmd` | `vscode`（PATHEXT 优先 exe） | `vscode` 或 `vscode.cmd` |
| Patch 失败且无 gcc | 仅 `.cmd` | `vscode` | `vscode.cmd`（地址栏不解析 PATHEXT，需带扩展名） |

> `.cmd` 按系统 ANSI 代码页（中文系统为 GBK）写入，含中文的路径也能被 cmd 正确解析。
> 若应用路径含 `% ^ &` 字符，批处理内会自动转义。

## 技术实现

**原理**：地址栏 / 终端按 PATH 查找命令，并以「当前文件夹」作为子进程工作目录（CWD）启动。
openin 装进去的不是应用本身，而是一个**跳板**（launcher）：读取 CWD 后作为参数启动真正的应用，
于是「站在哪个文件夹就 `vscode`，哪个文件夹被打开」。

**Launcher 生成（双冗余）**：

- `<name>.exe`：默认**二进制 Patch** —— 内置预编译模板（源见 `scratch/launcher_gui.c`、
  `scratch/launcher_cli.c`，用 `scratch/bin2h.c` 转成字节数组写进 `launcher_templates.h`），
  把模板 `.data` 段里的 `__OPENIN_TARGET_EXE_MAGIC` 占位符原地替换为目标 exe 路径，毫秒级、免 gcc；
  仅当 Patch 失败且系统有 gcc 时降级为源码编译
- `<name>.cmd`：始终生成（GBK/ANSI 写出，自动转义 `% ^ &`），免编译器兜底

**两类模板**（`cli` 标志区分）：

- **GUI 应用**（vscode / cursor…）：把 CWD 或命令行参数作为「目录参数」传给应用（`DETACHED_PROCESS`）
- **命令行工具**（claude / codex…）：不传目录参数，原地打开可见终端并继承 CWD
  （`cmd /c start "" wt.exe -d <CWD> cmd /k <shim>.cmd`，回退 `cmd /c` + `CREATE_NEW_CONSOLE`）

**PATH**：所选安装目录已在 PATH 则完全不动；否则追加到用户 PATH（HKCU，免管理员）并广播
`WM_SETTINGCHANGE` 让 Explorer 刷新。

**无状态**：只创建 launcher 文件，不写任何配置 / 日志；临时编译源码与日志用完即删；自定义目标仅存
会话内存，关闭即失。

**GUI 后台自动检索**：窗口先秒开，18 个预设路径由后台线程逐个扫描（PATH → `%LOCALAPPDATA%\Programs`
等根目录深 2 层 → 各盘符根一层 → `.exe`⇄`.cmd` 回退一层），经 `WM_APP_DETECT` 消息回填，
仅在编辑框为空时写入，不覆盖用户输入。

**已知问题**：命令行工具从地址栏启动时，前台焦点不可靠（Windows 前台锁 + Windows Terminal 不抢焦点），
详见 `CLAUDE.md`。

## 卸载

1. 删除安装目录里的 `<name>.exe`、`<name>.cmd`
2. 仅当 openin 当时**自行加入过 PATH**（目录原本不在 PATH，通常是自定义 `-p` 或回退场景），
   再在「设置 → 系统 → 系统信息 → 高级系统设置 → 环境变量 → 用户变量 → Path」里删除该目录

## 特性

- 一个工具注入任意应用，每个应用一个独立命令，互不冲突
- 安装在通用用户 bin 目录，已在 PATH 则完全不动环境变量，卸载只删几个文件
- 冗余：exe 默认二进制 Patch 生成（无需 gcc），`<name>.cmd` 永远可用
- 幂等：重复安装会覆盖同名文件，不产生重复 PATH 条目
- 冲突检测：同时检查 PATH 中是否已有同名 `.exe` / `.cmd`，有则提示优先解析到的位置
- 兼容选中 `bin` 子目录：自动向上找到上级的主程序（如 `Code.exe`）
- 支持参数透传：`vscode . -r`（复用窗口）、`vscode D:\some\file.txt`
- 两类预设：**GUI 应用**（vscode，传目录参数）与**命令行工具**（codex，继承 cwd、不传目录参数、经 `cmd /c start` 在 Windows Terminal 中打开）

## 路线图

- [x] VS Code 支持（首个应用）
- [x] 预设应用模板（18 个：vscode、cursor、JetBrains 全家桶、zed、sublime、wt、claude、codex…）
- [x] 图形界面管理：多目标列表、安装/更新、卸载、添加自定义、移除、刷新、后台自动检索
- [x] 无 gcc 环境默认分发预编译 launcher（二进制 Patch）
- [ ] 系统托盘常驻 / 开机自启动

## 故障排查

- **无 gcc**：无需安装——`<name>.exe` 通过内置预编译模板二进制 Patch 生成，不依赖 gcc
- **Patch / 编译失败**：不影响使用，会保留 `.cmd` 备用并给出错误信息
- **换个应用/路径**：重新运行选择新目录即可（会重新生成文件，PATH 通常不用动）
- **新窗口不生效**：已打开的终端/资源管理器需重启；Explorer 会自动刷新（WM_SETTINGCHANGE）

## 依赖

- **gcc 可选**：仅在二进制 Patch 生成 `.exe` 失败时才降级使用（MinGW-w64）；默认无 gcc 也能生成 `.exe`
- openin 本身是预编译 exe，运行它不需要任何编译器
