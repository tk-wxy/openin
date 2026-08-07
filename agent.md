# openin — 开发备忘

通用「打开到应用」启动器安装器：选择一个应用目录，安装后即可在资源管理器地址栏 / 终端输入命令，
以**当前文件夹**为参数打开该应用。多工具路线：vscode / cursor / claude / codex……

## 构建

```bat
gcc -O2 -s -municode -mwindows -o openin.exe openin.c -lshell32 -lole32
```

- `-municode`：`wmain` 宽字符入口，正确处理中文/Unicode 路径
- `-mwindows`：GUI 子系统，运行时不弹黑框
- 链接 `-lshell32 -lole32`：文件夹选择对话框 + COM

## 核心机制

- launcher = 取 `GetCurrentDirectoryW()`（地址栏/终端运行时的当前文件夹）→ `CreateProcessW()` 启动应用并传入
- 每装一个应用 = 一个独立命令（默认 `vscode`，可用参数自定义）

## 安装行为

- 目标目录自动选择：`-p` > `~/.local/bin` > `%APPDATA%\npm` > 创建 `~/.local/bin` > 回退 `%LOCALAPPDATA%\Programs\openin`
- 目录**已在 PATH** → 不动环境变量；不在 → 追加到**用户** PATH（HKCU，无需管理员）
- 冗余：始终生成 `<name>.cmd`（ANSI/GBK 编码，转义 `% ^ &`）；检测到 gcc 才额外编译 `.exe`
- 零残留：源码 `.c` 与 `build.log` 走 `%TEMP%`，用完即删；目标目录只留 `.exe` + `.cmd`
- 冲突检测：扫 PATH 中同名 `.exe` / `.cmd`
- `locate_code_exe`：选中 `bin` 子目录时自动向上找主程序

## CLI

```
openin.exe [-p 安装目录] [-d 应用目录] [-q 静默] [命令名]
```

## 约定

- 提交消息：英文，**不加** `Co-Authored-By: Claude` 署名（用户明确要求）
- `.exe` 已 gitignore，不入库
- 发布：编译 → `git tag` → `gh release create`（见 README）
- 关键文件：`openin.c`（全部逻辑单文件）、`README.md`、`agent.md`
