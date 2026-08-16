# openin — Windows 资源管理器地址栏 / 终端应用启动命令生成器

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2010%2B-blue?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/badge/Language-C%20(Win32)-00599C?logo=c" alt="Language" />
  <img src="https://img.shields.io/badge/Size-~150%20KB-success" alt="Size" />
  <img src="https://img.shields.io/badge/Dependencies-Zero-brightgreen" alt="Dependencies" />
  <img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License" />
</p>

**openin** 是一个极致小巧（~150 KB）、纯 C 编写的 Win32 原生工具。它能为你的任意开发工具、编辑器、命令行或 Web 服务生成一条**专属启动命令**（如 `vscode`、`idea`、`cursor`、`claude`、`dsh`）。

安装后，在 Windows 资源管理器**地址栏**（或任何终端）中直接输入该命令，即可**以当前文件夹为工作区**秒级拉起对应应用。无需应用自身支持地址栏调用，也不受复杂环境变量配置困扰。

```
┌───────────────────────────────────────────────────────────────┐
│ 📁 D:\projects\my-app                                         │
│ ┌───────────────────────────────────────────────────────────┐ │
│ │ vscode                                                 ↵  │ │  ◄── 在任意文件夹地址栏输入命令
│ └───────────────────────────────────────────────────────────┘ │
└──────────────────────────────┬────────────────────────────────┘
                               │
               openin Launcher (毫秒级跳板)
                               │
                               ▼
        🚀 VS Code 立即打开当前目录 (D:\projects\my-app)
```

---

## ⚡ 核心特性

- 🪶 **极致轻量 & 零依赖**：单个便携 `.exe`，体积仅约 150 KB；纯 Win32 原生开发，无需 .NET / VC++ Runtime 或其他运行库。
- ⚡ **免编译器（二进制 Patch）**：内置预编译的跳板模板，安装时毫秒级原地打补丁生成启动器，**目标机器完全无需安装 GCC / 编译环境**。
- 🔍 **智能两级路径检索**：
  - **启动快搜**：后台秒级扫描注册表 App Paths、系统 PATH 及常用应用目录。
  - **单遍剪枝深度扫描（Deep Scan）**：一键彻底穿透多层复杂安装目录（如 JetBrains 全家桶、多盘符安装），自动剪枝避开噪点目录，耗时仅约 1 秒。
- 🛡️ **严格可逆 & 零污染**：
  - **无状态设计**：不产生任何多余配置文件、运行缓存或外部日志。
  - **精准还原**：独创 `.openin-undo` 撤销机制，卸载时精准逐字节还原用户 PATH 与注册表，绝不残留多余环境变量。
  - **原生保护**：自动识别并保护已存在的同名原生工具（如官方 `claude.exe`），拒绝误删或误覆盖。
- 🖥️ **现代原生 Win32 GUI**：
  - 完美支持 **PerMonitorV2 DPI 高清自适应** 与 Windows 原生视觉样式，高分屏文字与控件清晰细腻。
  - **自研可滚动画布**：优化触控板 60~125Hz 高频消息合并与画布单窗口平移，滚动丝滑不掉帧。
  - **操作透明化**：安装与卸载时的每一步操作（写跳板、改注册表、撤销标记）在状态栏实时动态反馈。
- 🌐 **多场景启动模式**：
  - **GUI 应用**：自动透传当前文件夹或指定路径参数。
  - **CLI / TUI 命令行工具**：智能继承控制台与工作目录，通过 HKCU App Paths 完美隔离地址栏与终端。
  - **Web 服务工具（如 dsh）**：后台静默拉起服务，探测端口就绪后自动在默认浏览器中打开对应工作区。

---

## 📦 支持的应用预设（开箱即用）

内置 19 款常用开发工具预设（支持一键自动检索与自定义扩展）：

| 类别 | 命令名 | 目标工具 | 启动模式与特性 |
|---|---|---|---|
| **现代编辑器** | `vscode` | Visual Studio Code | GUI 模式，以当前目录打开 |
| | `cursor` | Cursor IDE | GUI 模式，以当前目录打开 |
| | `windsurf` | Windsurf Editor | GUI 模式，以当前目录打开 |
| | `zed` | Zed Editor | GUI 模式，以当前目录打开 |
| | `sublime` | Sublime Text | GUI 模式，以当前目录打开 |
| | `neovide` | Neovide (Neovim GUI) | GUI 模式，以当前目录打开 |
| **JetBrains 家族** | `idea` | IntelliJ IDEA | GUI 模式，支持深度扫描自动匹配 |
| | `pycharm` | PyCharm | GUI 模式，支持专业版 / 社区版 |
| | `webstorm` | WebStorm | GUI 模式，以当前目录打开 |
| | `goland` | GoLand | GUI 模式，以当前目录打开 |
| | `clion` | CLion | GUI 模式，以当前目录打开 |
| | `rider` | JetBrains Rider | GUI 模式，以当前目录打开 |
| | `datagrip` | DataGrip | GUI 模式，以当前目录打开 |
| | `rustrover` | RustRover | GUI 模式，以当前目录打开 |
| | `fleet` | JetBrains Fleet | GUI 模式，以当前目录打开 |
| **CLI / TUI 工具** | `claude` | Claude Code CLI | 原生 CLI 识别与一键状态修复 |
| | `codex` | Codex CLI | App Paths 架构级地址栏/终端隔离 |
| | `opencode` | OpenCode CLI | TUI 模式，继承终端控制台 |
| **Web 模式工具** | `dsh` | DeepSeek Harness | 内置固定参数，后台静默启动服务 + 自动登记工作区 + 唤起浏览器 |

> 💡 **添加自定义应用**：点击主界面「高级▾」→「添加自定义…」，即可为任意第三方软件配置专属启动命令。

---

## 🚀 快速上手

### 1. 图形界面管理（推荐）

直接双击运行 `openin.exe` 即可进入管理窗口：

1. **自动识别**：打开窗口后，程序会自动探测已安装应用的路径并回填。
2. **深度扫描**：若有应用未被命中（如安装在自定义深层目录），点击顶栏 **「深度扫描」** 进行全面穿透扫描。
3. **一键安装**：点击目标行右侧的 **「安装」** 按钮，毫秒级生成启动命令。
4. **即时验证**：安装完成后，点击 **「测试」** 按钮即可一键拉起目标验证效果。
5. **干净卸载**：点击 **「卸载」** 即可完整清除启动器并还原所有环境变更。

---

### 2. 命令行模式（CLI / 脚本自动化）

openin 支持纯命令行参数调用，方便自动化部署或无界面环境使用：

```bat
:: 打开 GUI 管理界面
openin.exe

:: 一键安装全部已自动检测到的预设应用
openin.exe -a

:: 查看当前已安装的所有 openin 命令
openin.exe -l

:: 为指定目录/程序安装命令（默认命令名自动推导）
openin.exe -d "D:\Microsoft VS Code"

:: 为指定程序安装自定义名称命令（如下述命令生成 `code.exe`）
openin.exe -d "D:\Microsoft VS Code" code

:: 指定安装目录（默认优先使用 ~/.local/bin 等通用目录）
openin.exe -p "C:\Users\YourName\bin" -d "D:\VS Code"

:: 卸载指定命令（自动还原环境变量与注册表）
openin.exe -u vscode

:: 静默安装模式（无弹窗提示，适合脚本批量执行）
openin.exe -q -d "D:\Program Files\JetBrains\IntelliJ IDEA\bin\idea64.exe" idea
```

#### CLI 参数速查

| 参数 | 描述 |
|---|---|
| `-d <路径> [命令名]` | 指定应用目录或可执行文件路径进行安装 |
| `-p <目录>` | 指定启动器生成的安装目标目录 |
| `-u <命令名>` | 卸载指定的启动命令并还原环境 |
| `-l` | 列出当前系统中已安装的所有 openin 启动命令 |
| `-a` | 批量自动安装所有已检测到的预设应用 |
| `-q` | 静默模式（不显示任何消息提示弹窗） |

---

## 🛠️ 工作原理与架构设计

### 1. 三层冗余 Launcher 生成体系
每次安装时，openin 采用多重保险机制保证启动器 100% 可用：
1. **`<name>.exe`（内置二进制 Patch，首选）**：基于预编译的模板二进制（约 22 KB），将内置魔法占位槽（目标路径、固定参数、Web URL）原地替换，免编译器秒级生成。
2. **`<name>.cmd`（ANSI/GBK 批处理，兜底）**：始终同步生成纯脚本跳板，确保在无 `.exe` 关联或特定环境下依然可用，支持特殊字符自动转义与中文路径解析。
3. **源码编译（降级备用）**：若二进制 Patch 发生极端异常且系统安装有 GCC，自动回退到临时源码编译。

### 2. 智能目录与 PATH 策略
openin 遵循**外部最小影响**原则，避免盲目新建专有目录：
1. 优先使用已存在且通常已在 PATH 中的通用用户目录（如 `%USERPROFILE%\.local\bin`、`%APPDATA%\npm`）。
2. 若目录已在 PATH 中，**完全不修改系统环境变量**。
3. 若必须修改 PATH，仅追加至当前用户的 `HKCU` 环境变量（无需管理员权限），并在卸载时依据 `.openin-undo` 标记精准逆向清除。

### 3. CLI 工具与地址栏的架构隔离
针对 Node.js / npm 全局工具（如 `codex`、`opencode`）常因裸 shell 脚本拦截地址栏调用的问题，openin 在安装 CLI 目标时写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths` 注册表：
- **地址栏输入**：Windows `ShellExecuteEx` 优先匹配 App Paths 注册表 → 命中 openin launcher，成功以当前目录拉起。
- **终端输入**：终端通过 `SearchPath` 查找 PATH 环境变量 → 优先执行 npm 原生 CLI，互不干扰。

---

## 🔨 从源码构建

本项目采用标准模块化 C99 开发，零第三方库依赖。

### 环境准备
安装 MinGW-w64 工具链（确保 `gcc` 与 `windres` 在系统 PATH 中）：
```bat
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

### 编译步骤
```bat
:: 1. 编译 Manifest 与 Windows 原生资源
windres -Isrc src\openin.rc -O coff -o openin_res.o

:: 2. 编译并链接生成单文件可执行程序
gcc -O2 -s -municode -mwindows -o openin.exe ^
    src\openin.c src\core.c src\detect.c src\pathenv.c src\utils.c src\gui.c openin_res.o ^
    -Isrc -lshell32 -lole32 -lcomdlg32 -lcomctl32 -luxtheme
```

#### 关键编译参数解析
- `-municode`：使用 `wmain` 宽字符入口，原生支持 Unicode 与中文路径。
- `-mwindows`：指定 GUI 子系统，运行与启动目标时不产生控制台黑框闪烁。
- `-O2 -s`：全量代码优化并去除符号表，极致压缩体积。
- `-luxtheme -lcomctl32`：启用现代 Windows 控件样式库与主题平滑渲染。

---

## 📂 项目结构

```
openin/
├── src/
│   ├── openin.c             # 程序主入口与 CLI 参数解析
│   ├── core.c               # 核心引擎：安装/卸载、二进制 Patch 生成器、预设表
│   ├── detect.c             # 应用探测引擎与单遍剪枝深度扫描算法
│   ├── pathenv.c            # 用户 PATH 环境变量安全操作与目录优选
│   ├── utils.c              # 路径校验、文件夹选择等公共工具函数
│   ├── gui.c                # Win32 原生 GUI、自研可滚动画布与消息循环
│   ├── openin.h             # 跨模块接口与数据结构定义
│   ├── openin.manifest      # DPI 感知与 comctl32 v6 视觉样式声明
│   ├── openin.rc            # Windows 资源定义文件
│   └── launcher_templates.h # 预编译 Launcher 模板字节数组
├── scratch/                 # 模板工具链与自动化测试脚本
│   ├── bin2h.c              # 模板二进制转 C 数组工具
│   ├── launcher_gui.c       # GUI 应用启动器源码模板
│   ├── launcher_cli.c       # CLI/Web 工具启动器源码模板
│   └── gui_shot.ps1         # GUI 冒烟测试与可见性采样脚本
└── README.md
```

---

## ❓ 常见问题 (FAQ)

<details>
<summary><b>Q: 在资源管理器地址栏输入命令后提示找不到文件？</b></summary>
若刚完成首次安装且 openin 自动为你配置了 PATH 环境变量，部分已开启的资源管理器窗口或第三方终端可能需要重启以重新加载最新环境变量；新打开的资源管理器窗口均可立即生效。
</details>

<details>
<summary><b>Q: 目标机器没有装 GCC 能正常安装命令吗？</b></summary>
完全可以。openin 采用内置的「二进制 Patch」技术直接生成启动器，运行与安装过程 100% 独立，不需要机器上存在任何 C 语言编译器或开发工具。
</details>

<details>
<summary><b>Q: 为什么对 Claude Code 等原生命令只显示「修复」而不是「安装」？</b></summary>
Claude Code 等官方工具本身已具备原生的地址栏命令体系（如数百兆的大型二进制）。openin 遵循原生保护原则，绝不破坏或覆盖原生安装。当检测到官方二进制意外丢失时，可通过「修复」按钮一键从核心版本目录恢复。
</details>

<details>
<summary><b>Q: 卸载之后会残留垃圾文件或修改我的系统设置吗？</b></summary>
不会。openin 严格执行无状态与可逆卸载：不仅会删除安装目录下的启动器，还会一并清除 App Paths 注册表项；若安装时由 openin 追加过 PATH，卸载最后一个命令时会自动将其从 PATH 中撤销并删除空目录。
</details>

---

## 📄 开源协议

本项目基于 [MIT License](LICENSE) 开源发布。
