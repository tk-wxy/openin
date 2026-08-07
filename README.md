# vscode-launcher 通用安装器

在**资源管理器地址栏**或任意终端里输入 `vscode`（或 `vscode.cmd`），用 VS Code 打开当前文件夹。

## 文件

| 文件 | 作用 |
|---|---|
| `vscode-installer.exe` | 安装器（双击运行） |
| `vscode-installer.c` | 安装器源码 |

## 使用

### 交互模式（推荐）
双击 `vscode-installer.exe` → 弹出文件夹选择框 → 选中 VS Code 安装目录（含 `Code.exe`）
→ 自动生成、编译启动器 → 自动处理 PATH → 完成。

### 命令行模式
```bat
vscode-installer.exe -d "D:\Microsoft VS Code"              :: 指定 VS Code 目录
vscode-installer.exe -d "D:\Microsoft VS Code" code         :: 自定义命令名
vscode-installer.exe -p C:\Users\you\bin -d "D:\VS Code"    :: 指定安装目录
vscode-installer.exe -q -d "D:\..."                         :: 静默模式，结果写入日志
```

## 安装位置（自动选择通用目录）

启动器**不放进专属文件夹**，而是自动挑选一个通用、常用的用户 bin 目录（这些目录通常已在
PATH 中，因此多数情况**无需修改 PATH**）：

| 优先级 | 目录 | 说明 |
|---|---|---|
| 1 | `-p <目录>` | 显式指定，完全自定义 |
| 2 | `%USERPROFILE%\.local\bin` | GitHub 风格用户 bin（claude 等工具在此），**已在 PATH 则跳过修改** |
| 3 | `%APPDATA%\npm` | Node 全局工具目录（codex 等在此），已在 PATH 则跳过修改 |
| 4 | 创建 `%USERPROFILE%\.local\bin` | 目录不存在时自动创建 |
| 5 | 回退 `%LOCALAPPDATA%\Programs\vscode-launcher` | 上述都不可用时才使用专属目录 |

- 所选目录**已在 PATH** → 不改动任何环境变量，仅放入文件
- 所选目录**不在 PATH**（如自定义 `-p`）→ 自动追加到**用户** PATH（HKCU，无需管理员权限）

生成的产物直接落在该目录：`<name>.exe`（有 gcc 时）与 `<name>.cmd`（始终）。
源码与编译日志走临时目录、用完即删，**目标目录零残留**。

## 双保险（冗余）机制

每次安装**始终生成** `<name>.cmd` 批处理备用启动器（无需编译器）；若检测到 `gcc`，
**额外编译** `<name>.exe`（优先使用）。

| 环境 | 生成物 | 终端输入 | 地址栏输入 |
|---|---|---|---|
| 有 gcc | `.exe` + `.cmd` | `vscode`（PATHEXT 优先 exe） | `vscode` 或 `vscode.cmd` |
| 无 gcc | 仅 `.cmd` | `vscode` | `vscode.cmd`（地址栏不解析 PATHEXT，需带扩展名） |

> `.cmd` 按系统 ANSI 代码页（中文系统为 GBK）写入，含中文的路径也能被 cmd 正确解析。
> 若 VS Code 路径含 `% ^ &` 字符，批处理内会自动转义。

## 卸载

1. 删除安装目录里的 `<name>.exe`、`<name>.cmd`
2. 仅当安装器当时**自行加入过 PATH**（目录原本不在 PATH，通常是自定义 `-p` 或回退场景），
   再在「设置 → 系统 → 系统信息 → 高级系统设置 → 环境变量 → 用户变量 → Path」里删除该目录

## 特性

- 命令名默认 `vscode`，可自定义，与 `code`（Cursor 等）互不冲突
- 安装在通用用户 bin 目录，已在 PATH 则完全不动环境变量，卸载只删几个文件
- 冗余：有 gcc 用 exe，无 gcc 退化为 cmd，`vscode.cmd` 永远可用
- 幂等：重复安装会覆盖同名文件，不产生重复 PATH 条目
- 冲突检测：同时检查 PATH 中是否已有同名 `.exe` / `.cmd`，有则提示优先解析到的位置
- 兼容选中 `bin` 子目录：自动向上找到上级的 `Code.exe`
- 支持参数透传：`vscode . -r`（复用窗口）、`vscode D:\some\file.txt`

## 故障排查

- **无 gcc**：自动退化为 `.cmd` 模式，功能不受影响；想用 exe 可装 MinGW-w64
  （`winget install BrechtSanders.WinLibs.POSIX.UCRT`）后重新运行安装器
- **编译失败**：不影响使用，会保留 `.cmd` 备用并给出 gcc 输出
- **换个 VS Code 位置**：重新运行安装器选择新目录即可（会重新生成文件，PATH 通常不用动）
- **新窗口不生效**：已打开的终端/资源管理器需重启；Explorer 会自动刷新（WM_SETTINGCHANGE）

## 依赖

- **gcc 可选**：仅在需要编译 `.exe` 时使用（MinGW-w64）；无 gcc 也能工作（.cmd 模式）
- 安装器本身是预编译 exe，运行它不需要任何编译器
