# openin

在 Windows 资源管理器地址栏输入一个词，用你想要的应用打开当前文件夹。

<p align="center">
  <img src="https://img.shields.io/badge/Windows%2010+-blue?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/C%20(Win32)-00599C?logo=c&logoColor=white" alt="Language" />
  <img src="https://img.shields.io/badge/version-2.1.0-blue" alt="Version" />
  <img src="https://img.shields.io/badge/~150%20KB-green" alt="Size" />
  <img src="https://img.shields.io/badge/零依赖-brightgreen" alt="Zero dependencies" />
  <img src="https://img.shields.io/github/license/tk-wxy/openin" alt="License" />
</p>

---

openin 是一个**一次性注入工具**：它为指定应用生成独立的启动器（如 `vscode.exe`），
放进 PATH 目录里，然后就完事了。生成的启动器是完全独立的程序，
运行时与 openin 没有任何关系——**openin 用完即可删除**，已注入的命令照常工作。

需要撤销？再跑一次 openin 卸载即可，所有改动完整还原。

```
               openin.exe（注入工具，用完可删）
                        │
                 一次性生成启动器
                        │
                        ▼
  地址栏输入 vscode ──► vscode.exe（独立跳板）──► VS Code 打开当前目录
```

## 下载

前往 [Releases](https://github.com/tk-wxy/openin/releases) 下载 `openin.exe`（当前版本 **v2.1.0**）。

单文件便携程序，双击即用。不需要安装，不需要常驻运行，不需要编译器。
在 Windows 资源管理器中右键 exe →「属性」→「详细信息」可查看版本信息。

## 使用

### GUI（推荐）

双击 `openin.exe` 打开管理窗口：

1. 程序自动检测已安装的应用并填入路径
2. 点「安装」生成启动命令，点「测试」验证效果
3. 需要时点「深度扫描」查找安装在非常规位置的应用
4. 点「卸载」可完整清除启动器并还原所有环境变更

### CLI

```bat
openin.exe -a                                :: 一键安装所有检测到的预设
openin.exe -l                                :: 列出已安装的命令
openin.exe -d "D:\Microsoft VS Code"         :: 安装指定应用（自动推导命令名）
openin.exe -d "D:\Microsoft VS Code" code    :: 安装并指定自定义命令名
openin.exe -u vscode                         :: 卸载
openin.exe -q -d "D:\path\to\app" name       :: 静默模式（无弹窗，适合脚本）
openin.exe -p "C:\my\bin" -d "D:\app"        :: 指定启动器安装目录
```

| 参数 | 说明 |
|------|------|
| `-d <路径> [名称]` | 指定应用目录或可执行文件，安装启动命令 |
| `-u <名称>` | 卸载指定命令 |
| `-a` | 自动安装所有检测到的预设 |
| `-l` | 列出已安装的启动命令 |
| `-p <目录>` | 指定启动器存放目录 |
| `-q` | 静默模式 |

## 内置预设

开箱即用 19 个常见开发工具，打开窗口自动检测路径：

**编辑器**　`vscode` · `cursor` · `windsurf` · `zed` · `sublime` · `neovide`

**JetBrains**　`idea` · `pycharm` · `webstorm` · `goland` · `clion` · `rider` · `datagrip` · `rustrover` · `fleet`

**CLI / TUI**　`claude` · `codex` · `opencode`

**Web 服务**　`dsh`（DeepSeek Harness —— 后台启动服务，自动打开浏览器）

> 点击「高级▾ → 添加自定义…」可为任意应用创建启动命令。

## 工作方式

openin 本身只是一个注入工具，不参与命令的实际运行。它做的事情是在用户 bin 目录
（如 `~\.local\bin`）中放置独立的小型**启动器**——启动器读取当前工作目录，
作为参数启动目标应用。启动器生成后就是独立程序，openin 可以删除。

**安装（注入）时做了什么：**

- 生成 `<name>.exe`（内置模板二进制打补丁，免编译器，毫秒完成）
- 同时生成 `<name>.cmd`（纯批处理兜底，保证可用）
- 如果目录不在 PATH 中，追加到用户 PATH（无需管理员权限）

**卸载（回退）时做了什么：**

- 删除启动器文件、清理注册表项
- 若安装时修改过 PATH，自动撤销该修改
- 若安装目录由 openin 创建且已清空，自动删除

不产生任何配置文件、日志或缓存。卸载后系统与安装前完全一致。

<details>
<summary>启动器安装目录选择逻辑</summary>

| 优先级 | 目录 | 条件 |
|--------|------|------|
| 1 | `-p` 指定的目录 | 用户显式指定 |
| 2 | `%USERPROFILE%\.local\bin` | 已存在且可写 |
| 3 | `%APPDATA%\npm` | 已存在且可写 |
| 4 | 创建 `%USERPROFILE%\.local\bin` | 上述不可用时 |
| 5 | `%LOCALAPPDATA%\Programs\openin` | 最终回退 |

已在 PATH 中的目录不会重复添加。

</details>

<details>
<summary>CLI 工具的地址栏与终端隔离</summary>

npm 全局工具（如 `codex`）在 `%APPDATA%\npm` 下有无扩展名的 shell 脚本，
会拦截地址栏的无后缀搜索。openin 通过写入 App Paths 注册表（HKCU）解决：

- 地址栏输入 `codex` → 系统优先查 App Paths → 走 openin 启动器
- 终端输入 `codex` → 按 PATH 搜索 → 走 npm 原生命令

两者互不干扰，卸载时自动清理注册表项。

</details>

<details>
<summary>安全机制</summary>

- **覆盖保护**：只操作 openin 自己生成的文件（通过文件内签名识别），拒绝覆盖或删除非 openin 的同名文件
- **原生命令保护**：对 `claude` 等已有官方二进制的命令，只提供「修复」功能，不覆盖、不卸载
- **可逆标记**：修改 PATH 或创建目录前写入 `.openin-undo` 标记，卸载时据此精准还原

</details>

## 从源码构建

需要 MinGW-w64（gcc + windres）：

```bat
winget install BrechtSanders.WinLibs.POSIX.UCRT

windres -Isrc src\openin.rc -O coff -o openin_res.o
gcc -O2 -s -municode -mwindows -o openin.exe ^
    src\openin.c src\core.c src\detect.c src\pathenv.c src\utils.c src\gui.c ^
    openin_res.o -Isrc -lshell32 -lole32 -lcomdlg32 -lcomctl32 -luxtheme
```

<details>
<summary>项目结构</summary>

```
src/
  openin.c             主入口与 CLI 参数解析
  core.c               安装/卸载引擎、启动器生成、预设表
  detect.c             应用路径检测与深度扫描
  pathenv.c            PATH 环境变量与目录操作
  utils.c              路径校验等工具函数
  gui.c                Win32 原生 GUI
  openin.h             模块间接口
  launcher_templates.h 预编译启动器模板
  openin.manifest      DPI 感知与视觉样式声明
  openin.rc            资源定义
scratch/
  launcher_gui.c       GUI 启动器模板源码
  launcher_cli.c       CLI 启动器模板源码
  bin2h.c              二进制转 C 数组工具
  gui_shot.ps1         GUI 自动化测试脚本
```

</details>

## 常见问题

**地址栏输入命令后没反应？**
刚安装且修改了 PATH 时，已打开的资源管理器窗口需要关闭重开。新开的窗口立即生效。

**没有 GCC 能用吗？**
能。启动器由内置模板直接生成，不需要编译器。GCC 仅在极端情况下作为回退方案。

**卸载干净吗？**
干净。启动器文件、注册表项、PATH 修改全部还原，不留任何痕迹。

## License

[MIT](LICENSE)
