# openin — 开发备忘

通用「打开到应用」启动器安装器：选择一个应用目录，安装后即可在资源管理器地址栏 / 终端输入命令，
以**当前文件夹**为参数打开该应用。多工具路线：vscode / cursor / claude / codex……

## 构建

```bat
gcc -O2 -s -municode -mwindows -o openin.exe openin.c -lshell32 -lole32 -lcomdlg32
```

- `-municode`：`wmain` 宽字符入口，正确处理中文/Unicode 路径
- `-mwindows`：GUI 子系统，运行时不弹黑框
- `-lshell32 -lole32`：文件夹选择对话框 + COM
- `-lcomdlg32`：GetOpenFileName 文件选择对话框

## 核心机制

- launcher = 取 `GetCurrentDirectoryW()`（地址栏/终端运行时的当前文件夹）→ `CreateProcessW()` 启动应用并传入
- 每装一个应用 = 一个独立命令（默认 `vscode`，可用参数自定义）
- launcher 模板在 `write_launcher_source` 内嵌，`@@PATH@@` 主程序路径、`@@NAME@@` 命令名

## GUI（纯 Win32，无 .rc 资源）

- `openin.exe` 无参数 → `gui_main()` 主窗口；带参数 → CLI
- 主窗口 = **预设表单行**：每行 [名称静态] [路径 Edit] [浏览] [安装/更新]，自定义行另加 [移除]
- 底部仅两个按钮：`重新检测`、`高级▾`（菜单：卸载选中、添加自定义…、打开配置目录、关于）
- **自动检索** `detect_app(exeName)`：PATH 目录 → Programs 常用根(深度2) → 各固定盘根(一层)，首个命中即停
- `refresh_rows()`：销毁重建所有行控件并填充（路径取已记录或自动检索）
- 添加自定义：`add_custom_dialog()` 嵌套消息循环模态窗（命令名 + 主程序 exe，GetOpenFileName 浏览）
- 预设模板 `PRESETS[]`：目前 vscode/Code.exe，追加即可支持更多（cursor/codex…）
- 状态计算 `is_installed(name)`：`install_dir\<name>.exe|.cmd` 存在 → 已安装（按钮变「更新」）

## 配置（`%LOCALAPPDATA%\openin\targets.ini`，UTF-8）

```
[openin]
install_dir=C:\Users\...\.local\bin
added_path=0      # openin 是否往用户 PATH 追加过该目录
created_dir=0     # 该目录是否为 openin 新建
[targets]
vscode=D:\Microsoft VS Code\Code.exe   # 命令名=主程序路径
```

## 安装行为

- 目标目录自动选择：`-p` > `~/.local/bin` > `%APPDATA%\npm` > 创建 `~/.local/bin` > 回退 `%LOCALAPPDATA%\Programs\openin`
- 目录**已在 PATH** → 不动环境变量；不在 → 追加到**用户** PATH（HKCU，无需管理员）
- 冗余：始终生成 `<name>.cmd`（ANSI/GBK 编码，转义 `% ^ &`）；检测到 gcc 才额外编译 `.exe`
- 零残留：源码 `.c` 与 `build.log` 走 `%TEMP%`，用完即删；目标目录只留 `.exe` + `.cmd`
- 卸载（`uninstall_target`）：删文件；若 `added_path=1` 且目录空 → 移 PATH 条目；若 `created_dir=1` 且目录空 → 删目录
- 冲突检测：扫 PATH 中同名 `.exe` / `.cmd`
- `locate_main_exe`：选中 `bin` 子目录时自动向上找主程序

## CLI

```
openin.exe                             # GUI 主窗口
openin.exe -d <应用目录> [命令名]       # 命令行安装
openin.exe -p <安装目录>               # 指定安装目录
openin.exe -u <命令名>                 # 卸载
openin.exe -q ...                      # 静默，结果写 %LOCALAPPDATA%\openin.log
```

## 约定

- 提交消息：英文，**不加** `Co-Authored-By: Claude` 署名（用户明确要求）
- `.exe` 已 gitignore，不入库
- 发布：编译 → `git tag` → `gh release create`（见 README）
- 关键文件：`openin.c`（引擎 + GUI 单文件）、`README.md`、`agent.md`
