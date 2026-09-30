# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

**DevDesk** 是面向开发者的跨平台桌面工具集（C++20 GUI 应用），包含三大工具：

- **Markdown 笔记本**：本地文件存储 + SQLite FTS5 全文搜索
- **Todo 看板**：SQLite 存储，多列卡片管理
- **Claude 配置切换器**：多套 API profile 管理 + 本地 HTTP 代理（带流式转发）

另有首页仪表盘、设置页（模块插拔开关）与**桌宠**（module_pet，非 Router 页面模块，无边框置顶透明子窗，默认关闭、经设置页模块开关启用）。

目标平台：**macOS 27+**（当前开发平台，本机 arm64——macOS 也是本项目最初的开发平台，P0-P8 主线在此完成）和 **Windows 11**（移植目标，2026-09-21~28 曾在 Windows 上开发，P14 与 W/R 各批在 Windows 侧交付）。

> **平台补记（2026-09-28）**：项目已搬回本机 macOS（arm64）继续开发——macOS 是本项目最初的开发平台。本文件的 Windows 相关工具链章节（「常用命令」的 MSYS2 MinGW64 段、ccache 三件套、并行度封顶等）保留，作为移植目标的参考，不再是当前开发环境。

## 🚨 项目核心约束（最重要的几条）

1. **用户不熟悉 C++，纯 AI 开发** — 代码对用户不可见，所有反馈通过：文档 / 中文日志 / 截屏 / 测试结果 / mockup
2. **完整设计文档**：所有架构 / API / 设计 token 决策都写在 `docs/superpowers/specs/2026-09-20-devdesk-design.md`，**实现前先读 spec**
3. **每个 P 阶段完成后必须**：跑测试 + 启动应用 + 截屏 + 更新文档 + git commit（详见 spec §16.2）
4. **配色方案已定为 B（温暖专业 / Claude.ai 风格）**，完整 token 写在 spec §6.5.1 和 §6.5.2
5. **平台适配代码必须接口隔离**（2026-09-21 定）：头文件定义平台无关接口，实现按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake 按平台选择编译（与 EUI-NEO 后端选择同一模式）。**禁止在业务代码里散布 `#ifdef _WIN32` / `__APPLE__`**。虚接口仅在需要运行期注入/mock 时对单个点引入，不全局铺开

## 🛠️ 常用命令

> **🔴 构建与测试一律走 `bash scripts/build-test-run.sh`，禁止手敲 `cmake -B …` / `cmake --build …`。**
>
> 这条规则 2026-09-28 立，依据是当天实测出的事故，不是风格偏好：
>
> 1. **手敲会造出多份互相漂移的构建目录。** 当天机器上同时存在 `build/`、`build-release/`、`build-debug/` 三份手工构建目录，各自的 configure 参数互不相同；`build-debug/` 事后被删，`build-release/` 的产物与脚本构建出的不一致。结果是没人说得清"当前这个二进制到底是哪份配置编出来的"——这正是本项目要求"工艺参数可审计、可复现"的反面。
> 2. **手敲必然漏掉脚本里已经封装好的约定。** 脚本封装的每一项少掉都不会立刻报错，只会在之后某个时刻以"莫名其妙的结果"出现：`EUI_NEO_DIR` 的 worktree 自动定位（worktree 里没有同级 `../EUI-NEO`，漏了直接 configure 失败或链到错的框架源码）、ccache 的两个 launcher 参数（漏了编译缓存全 miss，白装）、`OPENSSL_ROOT_DIR` 的平台差异（macOS 用 Homebrew `openssl@3`、Windows 用 `C:/msys64/mingw64`）、发布形态守卫（Windows 单 exe / 零外部 DLL，漏了会在发布时才发现）、模块隔离守卫、以及并行度封顶 `DEVDESK_JOBS`（默认 3，多条 worktree 并发构建时防止互相拖死）。
> 3. **手敲的命令不会被任何人复现。** 脚本是唯一被记录、被反复执行的构建路径；只有它跑出来的结果才能当成基线——手敲的命令出问题时，无法区分是环境问题还是代码问题，事后也无法一次复跑定位。
>
> 脚本已覆盖两种构建类型：默认 Debug（构建目录 `build/`），加 `--release` 即 Release（自动切到 `build-release/`，两类构建互不覆盖）。需要非常规目录时用 `DEVDESK_BUILD_DIR=<dir>`，不要绕回手敲 cmake。
>
> **例外（有意保留，不是开口子）**：`scripts/` 内部的实现；以及**明确的诊断 / 验证场景**——例如临时用独立 build 目录做差分验证、或需要非标准 flag 做一次性探针——允许直接用 cmake，但**必须在报告 / commit message 里写明"为什么脚本不够用"**。这条例外的目的是不让规则变成"遇到脚本覆盖不到的情况就只能违规"，而不是给人留后门。
>
> 紧随其后的 macOS / Windows 手敲命令块只作**工具链参考**（记录编译器、参数形态、平台差异），日常构建与测试不要照着敲。

**macOS / arm64（当前开发平台，AppleClang + Ninja）**：

```bash
export PATH="/opt/homebrew/bin:$PATH"

cd DevDesk
rm -rf build   # 换平台后必须（缓存里锁着旧平台的产物）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
cd build && ctest --output-on-failure
```

本机工具链：cmake 4.4.3、ninja 1.13.2、AppleClang（`/usr/bin/clang++`）、ccache 4.14.1（均在 `/opt/homebrew/bin`，已在 PATH）。ccache 在 macOS 的持久配置读 **`~/Library/Preferences/ccache/ccache.conf`**——**不是** XDG 的 `~/.config/ccache/ccache.conf`（实测写 XDG 路径 ccache 完全无视）。**当前 macOS 构建状态（是否已通过、测试基线）见 `TODO.md`「macOS 构建环境」段**，本文不写死。

**Windows 11（移植目标，MSYS2 MinGW64 + Ninja；以下命令保留作移植参考）**：

```bash
export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"

cd DevDesk
rm -rf build   # 换平台后必须（缓存里锁着旧平台的产物）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build
cd build && ctest --output-on-failure
```

**一体化脚本（推荐）**：`bash scripts/build-test-run.sh` —— 增量构建 + 全量测试 + 杀旧进程 + 启动 app，默认全增量（不删 build/）。参数：`--clean` 全量重建、`--no-test` 跳过测试、`--no-run` 不启动、`--filter X` 只跑名字含 X 的用例。工具链 PATH 脚本内部处理，app 以重定向后台方式启动不阻塞终端。**worktree 里直接可用**：脚本按 `EUI_NEO_DIR` 环境变量 → 同级 `../EUI-NEO` → git 主工作树同级 自动定位 EUI-NEO 并传给 configure（`-DEUI_NEO_DIR`）；`build/` 未初始化时自动 configure —— worktree 首建无需手敲 `-D`（见 HANDOFF §0.4）。

**模块隔离静态守卫（TIN-01，已从单测迁到脚本）**：`bash scripts/check-module-isolation.sh` —— 断言 `src/module_settings/settings_module.cpp` 不反向依赖 `module_pet`（零 include / 零触碰 pet 盘 store / 零 `namespace module::pet`）、`module_pet_bridge.h` 恰好 include 一次且四条桥函数都真被调用，并断言 `src/module_settings/CMakeLists.txt` 的非注释行不 link 兄弟模块库。退出码 `0` 通过 / `1` 违规（逐条打印 `文件:行:命中行`）/ `2` 环境错（**不静默绿**）；`--root DIR` 可指向仓库外拷贝树做反证。`scripts/build-test-run.sh` 在测试段之后自动串跑它（`--no-test` 也跑），失败即整体 `exit 1`。*为什么独立成脚本*：原守卫编在 `test_settings.cpp` 内靠 `__FILE__` 上溯仓库根，ccache 相对化 `__FILE__` 后 cwd=仓库根 必误报 → 全量必红；脚本全程绝对路径、不读 `$PWD`、脚本内不 cd。

**编译缓存 ccache（worktree 并行必备，已接进脚本）**：三件套**已落 ccache 持久配置**（`%LOCALAPPDATA%\ccache\ccache.conf` 写 cache_dir/base_dir/hash_dir/max_size，缓存目录 `D:/software/ccache-devdesk/ccache.conf` 再写一份），**不需要任何 `CCACHE_*` 环境变量**；手敲 cmake **只需带两个 launcher 参数**：
```
-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```
`scripts/build-test-run.sh` 已自动带这两个参数；`scripts/ccache-env.sh` 只在要**临时覆盖**缓存盘/基准目录时才 source（平时不需要）。未装 ccache → 自动降级不传 launcher，行为与从前一致。三件套的作用：
- `CCACHE_DIR=D:/software/ccache-devdesk` —— 缓存放持久盘（放 `build/` 里会被 worktree 清理一起删掉）
- `CCACHE_BASEDIR=D:/project/gui` —— 把 `<worktree>` 绝对路径改写成「相对当前目录」再哈希。**不设 = 白装**：每个 worktree 绝对路径都不同 → 同一份源码哈希不同 → 永远 miss（实测：不设三件套，A/B 两棵树 2 miss；设了第二棵 `direct_cache_hit`）
- `CCACHE_NOHASHDIR=true` —— 命令哈希不含工作目录；`-g` 调试信息里也有绝对路径，靠 BASEDIR 一起改写

launcher 是 **configure 期属性**：已存在的 `build/` 目录不会自动改，要么 `--clean`、要么手敲一次带 launcher 的 configure，之后才走缓存。**并行度封顶**：脚本默认 `DEVDESK_JOBS=3`（→ `CMAKE_BUILD_PARALLEL_LEVEL`），12 核机器上多条 worktree 并发构建时避免互拖；单条独占机器可 `DEVDESK_JOBS=8 bash scripts/build-test-run.sh`。
**实测（2026-09-27，12 核 / `DEVDESK_JOBS=3` / 两个全新 worktree 首建）**：冷缓存 **647 s**（configure 52 s + ninja 284 边 595 s）→ 热缓存 **35 s**（239 次查找 237 命中 = **99.2%**，且 237 全为 direct 命中、零预处理），即 worktree 首建从 ~11 分钟降到 **~35 秒**；缓存产物 `devdesk_tests.exe` 全量全绿（当时快照 589/589；**用例总数以运行输出 `Total:` 为准，不在文档写死**）、`DevDesk.exe` 依赖 DLL 集合与冷编译逐项一致（17 个系统 DLL、零外部 DLL）。
**"裸 `cmake -B build`（完全不带 -D）也能吃缓存"这条路走不通，别再试**：① 本仓库 configure 本身就需要 `-DOPENSSL_ROOT_DIR=C:/msys64/mingw64`（否则 `Could NOT find OpenSSL` 直接失败，CLAUDE.md 上面那段是最简可用形态）；② 试过 ccache 官方 masquerade（MSYS2 自带 `/mingw64/lib/ccache/bin` 是 bash 脚本、写死 `/mingw64/bin/g++`，在 Git Bash 里指向 Git 自带目录 → 报错）与自建 shim 目录（ccache.exe 复制成 `g++.exe`，实测能让 `g++ --version` 走 ccache），但 shim 会顶替编译器路径 → 本仓库 `DEVDESK_MINGW_ROOT` 从编译器路径反推 MinGW 根（`cmake/Dependencies.cmake`），根变成 `D:/software` 后静态 curl 依赖链解析失败（`set_property can not be used on an ALIAS target`）→ 不采用。
**实测（2026-09-27，12 核 / `DEVDESK_JOBS=3` / 两个全新 worktree 首建）**：冷缓存 **647 s**（configure 52 s + ninja 284 边 595 s）→ 热缓存 **35 s**（239 次查找 237 命中 = **99.2%**，且 237 全为 direct 命中、零预处理），即 worktree 首建从 ~11 分钟降到 **~35 秒**；缓存产物 `devdesk_tests.exe` 全量全绿（当时快照 589/589；**用例总数以运行输出 `Total:` 为准，不在文档写死**）、`DevDesk.exe` 依赖 DLL 集合与冷编译逐项一致（17 个系统 DLL、零外部 DLL）。

**macOS 侧的脚本（2026-09-28 起为跨平台）**：`scripts/build-test-run.sh` / `scripts/ccache-env.sh` / `scripts/check-consistency.sh` 由提交 `577e0d0` 加上 macOS/Linux 分支——Darwin 走 `pkill -f` 杀进程、`otool -L` 依赖清单、`screencapture -x` 截屏、`stat -f%z`、`-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`，ccache 配置读 `~/Library/Preferences/ccache/ccache.conf`；原先写死的 Windows 工具链 PATH 与 `C:/msys64/mingw64` openssl 路径改为按 `DEVDESK_OS` 分支取（Windows 分支行为不变，波 1 TIN-13 的 Playwright + `CopyFromScreen` + Pillow 回退仍在）。**该移植只做过语法检查与定点验证，端到端从未跑过**——macOS 上「构建 + 测试 + 启动」未完整跑过一遍；`check-consistency.sh` 的 `screencapture` 分支所需的终端「屏幕录制」TCC 权限**已于 2026-09-28 晚授予，`screencapture -x` 实测可用（2940×1912 PNG）**，现存限制仅为「对 OpenGL 窗口可能抓到导航前的旧帧」（已登记 `docs/KNOWN_LIMITATIONS.md` §20）。本文档旧版提到的 `./scripts/dev.sh`（构建+运行）、`./scripts/test.sh`、`./scripts/screenshot.sh <page>`、`./scripts/reset-db.sh` **在本仓库中不存在**（`git ls-files scripts/` 可核），不要再照抄执行。

构建入口：`CMakeLists.txt` 顶层 → `src/main.cpp`。
EUI-NEO 依赖：`../EUI-NEO/`（同级目录，通过 `EUI_NEO_DIR` cache 变量 `add_subdirectory` 集成，**不是 git submodule**）。**当前依赖 rev**：`72a5187`（2026-09-30，与 `DEVDESK_EUI_NEO_MIN_REV` 一致；EUI-NEO 本地 main 领先 origin 93 个提交（2026-09-30 实测），**已决策永不 push 到 origin**（2026-09-28 用户决策）——origin/main 停在 v0.6.0，本地领先是长期状态、提交数只会继续增长）。**EUI-NEO 需保持本地 main 最新**（含 onWindowDestroyed/requestFocus/trayMenu/windowEffect/lineStyles/getMonitorWorkAreas/setSystemPowerHandler/styledMenu/onTrayMenu/windowIsFocused/input onCancel 等新 API），顶层 CMake configure 时打印 `EUI-NEO git rev` 并在缺失最低依赖 rev（`DEVDESK_EUI_NEO_MIN_REV`，与本行保持一致、消费 EUI 新 API 后两处同步更新）时软警告不 fail；原计划「EUI-NEO push 后启用硬版本检查」的前置条件因「永不 push」决策不再到来，硬版本检查保持未启用（审计 [11]）。

## 🏗️ 架构要点（4 层模块化）

```
main → 6 个 module_* (home / markdown / todo / claude_proxy / settings / pet)
            │  pet 非 Router 页面模块：main 直接 startPetWindow 接线独立子窗
            ↓
       core (Result / paths / sqlite_db / logging / theme / router)
            ↓
       platform (http_client / http_server / keychain / json)
            ↓
         EUI-NEO
```

**关键依赖规则**（违反会导致循环依赖）：
- `core` ❌ 不能依赖 `platform`；❌ 不可调用 EUI-NEO 运行时行为（仅 **header-only 类型桥接**例外——eui::Color 值类型 / Ui/Screen 签名透传 / component_theme.h 纯映射，边界见 spec §4.2.1）
- `module_*` 之间 ❌ 不可互相依赖
- `platform` ✅ 可以依赖 `core`（用 `Result`）
- `module_*` ✅ 可以依赖 `core` 和 `platform`

## 🔧 EUI-NEO 上游贡献政策（重要）

**遇到 EUI-NEO 的 ❌ 限制时**，优先考虑**直接修复 EUI-NEO 仓库**（`../EUI-NEO/`，与 DevDesk 同级目录），反哺社区并提交 PR，而不是 DevDesk 内自造 workaround。

### 工作流

1. **在 DevDesk 中遇到问题** → 不要立即写 DevDesk 专属 hack
2. **判断是否通用问题** → 如果是（拖拽 / 文件拖入 / macOS 27 / 字体等），属于 EUI-NEO 上游应修
3. **修复 EUI-NEO** → 在 `../EUI-NEO/` 写 patch → 编译 DevDesk 验证
4. **提交 PR** → 推送到用户 GitHub EUI-NEO fork 或发起到上游
5. **DevDesk 受益** → 移除自己的 hack，复用 EUI-NEO 新能力

### 哪些场景适合修上游

- ✅ **通用 UI 能力缺失**（drop target / file drop / tree 组件）
- ✅ **macOS 27+ 适配**（Liquid Glass / Metal 后端 / OpenGL fallback）
- ✅ **跨平台一致性问题**（IME / 字体 fallback / DPI）
- ✅ **API 设计改进**（fontWeight 真切换 / 主题切换便捷性）
- ✅ **性能 / 编译时间 / 二进制大小**

### 哪些场景 DevDesk 自己解决

- ✅ **业务逻辑**（Markdown 解析、Todo CRUD、Claude 切换）
- ✅ **应用级拖拽**（Todo 卡片跨列拖拽是 DevDesk 业务，EUI-NEO 提供基础 onDrag 就够）
- ✅ **设计 token / 配色方案**（DevDesk 自己的事）
- ✅ **第三方集成**（Keychain / SQLite / libcurl）

### PR 准备清单

修复 EUI-NEO 后提交 PR 前要确保：
1. 在 EUI-NEO 的 `examples/` 或 `apps/` 加 demo 用例
2. 不破坏现有 API（向后兼容）
3. 更新 `docs/` 文档
4. 通过 EUI-NEO 自身的编译（DevDesk 不需要参与）
5. commit message 写明需求来源（"DevDesk 需要 X 功能"）

### EUI-NEO 仓库约定

- 主分支：`main`
- 提交前缀：`feat:` `fix:` `refactor:` `docs:`（与 DevDesk 一致）
- 大改动先开 issue 讨论
- 提交后附带 example + 测试

**详见**：[[docs/EUI-NEO-CAPABILITIES.md]] 中每个 ❌ 项的"应对" —— 现在"应对"应优先考虑"提交 PR"而非"自造 hack"。

---

## ⚠️ EUI-NEO 真实能力盘点（已审计源码，2026-09-20）

**📄 完整调研报告**：[`docs/EUI-NEO-CAPABILITIES.md`](docs/EUI-NEO-CAPABILITIES.md) — 后续开发遇到 UI 问题**先查这里**。

实现时**只使用以下能力**，不要假设有未列出的 API：

| ✅ 支持 | ❌ 不支持（必须 hack 或放弃） |
|---------|------------------------------|
| button / input (多行+IME+选区) / checkbox / switch / dropdown / datepicker / slider / progress / tabs / navbar / sidebar / dialog模态 / toast / contextMenu / tooltip / dataTable / charts / markdown / virtualList / scrollView | **OS 文件拖入** — 没有 onFileDrop |
| 任意 hex 颜色 + 透明度 + 圆角 + 阴影 + 渐变（仅2色+水平/垂直）+ transform 2D/3D | **drop target API** — 必须自实现 DragController |
| FreeType 字体加载 + 系统字体 fallback + HiDPI 自动 | **tree 组件** — 必须自实现 MarkdownTreeView |
| FontAwesome 图标 + Unicode Emoji + SVG（栅格化） | **macOS 27 Liquid Glass / vibrancy** — 用实色 + 阴影 |
| onTap / onPress / onDrag / onKeyEvent / onScroll / onContextMenu / clipboard / IME / 撤销重做 | **全局 OS hotkey**（窗口失焦时收不到键盘事件）|
| `transition + animate` (8 种 easing + 位掩码属性) | **fontWeight runtime 切换**（只打包 Bold 一份字重，SemiBold/Medium 无效）|
| 多窗口（`app::openWindow`） | **自定义托盘菜单**（只硬编码 Show/Exit）|
| | **Toast 队列**（单实例）|
| | **Collapse / Accordion 组件** |
| | **skeleton loader**（loading 占位需自画）|
| | **Tab 焦点循环**（InputKey::Tab 不自动调度）|
| | **onResize / onMove 回调**（靠 `screen` 参数变化触发自适应）|

### 后续开发遇到 UI 问题时的查询顺序

1. 查 [`docs/EUI-NEO-CAPABILITIES.md`](docs/EUI-NEO-CAPABILITIES.md) 的组件清单和功能核对表
2. grep `../EUI-NEO/components/` 看是否有现成组件
3. grep `../EUI-NEO/core/dsl.h` 看 DSL API
4. 看 `../EUI-NEO/examples/` 和 `../EUI-NEO/apps/` 的实际用法
5. 如果还是不清楚，**不要假设**，明确写"待验证"留 TODO

## 🎨 视觉规范（"大气"原则）

- **默认窗口**：1600 × 1000，最小 1200 × 720
- **HiDPI**：默认开启（GLFW_SCALE_TO_MONITOR）
- **主品牌色**：Claude Clay `#D97757`
- **配色**：方案 B（warm professional，暖墨底 + Claude Clay 橘）
- **正文**：15 px（不是 14）
- **页面标题**：32 px
- **统计数字**：48 px
- **主按钮高度**：40-44 px（不是 32）
- **间距**：基于 8 px 网格，主区 padding 40 px
- **圆角**：按钮 10 px / 卡片 12-14 px / chip 999 px（pill）

完整 token 见 spec §6.5.1 和 §6.5.2。

## 🚦 当前阶段（2026-09-28 更新，项目已从 Windows 搬回 macOS 本机开发）

按 spec §16 实施，进度：

- ✅ **P0-P8 完成**（macOS 上 66 测试通过 + .app bundle，截图见 `docs/screenshots/`）
- ✅ **P9（代理非流式）+ P10a（SSE 流式）+ P10b（流式压测）完成** —— W1 收编 macOS 中断现场并修全部基线问题（T1-T5），Windows 全量测试 109 例全绿（W2 验收 2026-09-22：10MB e2e/soak/断连恢复/内存采样全过）
- ✅ **P14（Windows）必选达成** —— spec §16 已更新：P14 升必选（当时以 Windows 11 为开发环境；2026-09-28 起当前开发平台是本机 macOS）+ 发布形态约束"单 exe 零外部 DLL"（T3 达成，W2 复查通过，均为 Windows 侧验收）；平台适配接口隔离完成（T6，业务代码零平台宏）
- ✅ **W3 剩余需求收官（T9–T12 + P16）** —— 启动顺序理顺零 WARN、P12 Home 真实数据、P13 设置页、platform::json 实装、P15 文档（124/124 全绿），恢复计划关闭
- ✅ **W5 遗留项清零（2026-09-22）** —— Claude 按钮全量接线、侧边栏导航（删浮动齿轮）、全局字号缩放、UTF-8 argv、clearColor 随主题、跨模块跳转负载（Home 快捷按钮弹窗 + 最近编辑打开指定笔记）。最终基线 **137/137 全绿**；详见 CHANGELOG [W5] 与 KNOWN_LIMITATIONS「已解决」小节
- ✅ **W6-W9 + 磨砂全链（2026-09-22/23）** —— 窗口效果三档、供应商配置（settings.json 自动接管）、插件化门控、桌宠 MVP、Markdown 键盘流/标签/目录树、Todo 搜索/逾期/tags/拖拽反馈、模块插拔（09-23 基线 330 全绿）
- ✅ **R8 装配层收口 + R10 ClaudeService 拆分（2026-09-24~28）** —— Module::shortName 虚函数 + app_registry 合表 + buildGatedEntries/dispatchTrayNavigation 下沉可测 + THS-08 表驱动移除 claudeOn；R10 A1-A4 把 CallLogStore / ProfileRepository / ProxyRuntime / SettingsTakeOver 抽离，门面 `claude_service.cpp` 2005→780 行、**43 签名零变化**
- ✅ **共享配置 + failover 批 1-3（2026-09-24~28）** —— 服务层 + 片段编辑器 UI + `shouldFailoverTo` / `resolveFallbackProfile` / `fallback_profile_id` migration v10 + 注入守卫，共享全链闭环
- ✅ **MD-01 重载块映射修复（2026-09-24~28）** —— `(kind,hash,stableId)` 三元组优先 + `(kind,hash)` 回退 + 空文档 guard
- ✅ **setScrollOffset 上游补齐（2026-09-24~28）** —— EUI-NEO `Ui::setScrollOffset` pending 挂起模式 + DevDesk M6 删 id 轮转 hack
- ✅ **TIN-06 测试 scaffold 迁移（2026-09-24~28）** —— HeadlessFrame 三合一 + InMemoryDb + ScopedAppDatabase + DataRootEnv
- ✅ **UI 综合 audit + 修（2026-09-24~28）** —— home/markdown/todo/settings/pet 41 findings（home hover 闪烁 = onTimer 整页重绘、markdown 双栏滚动失效 = init-only scrollState、todo 拖动插入、settings 下拉 z 碰撞回归）；lineChart chromeless 化、datepicker 中文化、窗口失焦全局重绘（EUI-NEO `ca40ff2`）、theme 去纯白米色基调（surface `#F5F2EC`）、卡片 panelShadow、侧边栏删 tooltip
- ✅ **macOS 三特性 + UI 现代化 M1-M3（2026-09-28）** —— specs：`2026-09-28-macos-three-features-design.md` / `2026-09-28-ui-modernization-design.md`
- ⏳ **DevDesk probes spec（2026-09-28，dev 未开始）** —— EUI-NEO probe 模式 + dropdown z MVP，spec `docs/superpowers/specs/2026-09-28-devdesk-probes-design.md`（e149132）

### Windows 迁移缺口（P14 提前激活；2026-09-22 W2 验收后更新为状态表）

| 问题 | 位置 | 状态 |
|------|------|------|
| DPAPI keychain 曾是 stub（三函数全返回 NotImplemented，阻断 Claude 模块） | `src/platform/keychain_win.cpp` | ✅ T7 已实现（a534f5d）：CryptProtectData/CryptUnprotectData + 文件存储，19 个连锁用例全转绿 |
| 缺 `ws2_32` + `crypt32` 链接（Winsock2 + TLS 依赖链） | `src/platform/CMakeLists.txt` | ✅ T2 已修：`if(WIN32)` 内 PUBLIC 链接，不再需要 `CMAKE_CXX_STANDARD_LIBRARIES` 绕过 |
| FTS5 检测假阴性（MSYS2 sqlite 实际带 FTS5） | `cmake/Dependencies.cmake` | ✅ T2 已修：`check_cxx_source_compiles` 补 SQLite3 库链接，CJK 搜索测试全过 |
| 5 处干净编译修复（json.h 缺 cstdint / migrations designated initializers / proxy_router 绑临时 / -Werror 泄漏进 EUI-NEO） | `src/platform/json.h` 等 | ✅ T2 已修：删 build/ 全新 configure 零 workaround flag 可构建 |
| 测试基建 Windows 崩溃（`filesystem::remove` 抛异常 + sqlite 句柄未关 + test_main 无容错） | `tests/` | ✅ T1-B 已修：套件跑到底，单测异常只挂本用例不炸套件 |
| 启动顺序 WARN：模块 `onStart` 时 `ClaudeService::global()` 为 null，main 兜底启动代理 | `main.cpp` / `claude_module.cpp` | ✅ W3 已修（`50ff663`）：`startClaudeProxy()` 挪到 `getRouter()` 之前，模块 onStart 时 service 就绪，启动零 WARN |
| 发布形态：单 exe 零外部 DLL | `cmake/Dependencies.cmake` | ✅ T3 已达成（95c59d5）：sqlite3/curl/ssl/crypto/C++ 运行时全静态，W2 验收复查 `objdump` 仅 16 个 Windows 系统 DLL |
| `captureOpenFileFromArgv()` 曾仅 `__APPLE__` 有实现 | `src/platform/file_open_win.cpp` | ✅ T6 已按平台拆文件（接口隔离）；✅ W5 已修（`90d5792`）：改 `GetCommandLineW + CommandLineToArgvW` 宽字符 argv 转 UTF-8，非 ASCII 路径不再乱码 |
| `check-consistency.sh` 用 macOS `screencapture`/`open -a` | `scripts/` | ✅ 波 1 TIN-13 已 Windows 化（Playwright + `CopyFromScreen` + Pillow 回退）；残留限制（锁屏/RDP 黑图、python 解释器错配）见 `docs/KNOWN_LIMITATIONS.md` 一.3 |
| 旧 `build/` 是 Darwin arm64 产物 | `build/` | ✅ W1 已删除并在 Windows 重建 |

已正确处理的跨平台点（T6 接口隔离后）：**keychain / paths / time / file_open 四族平台 API 全部按平台拆文件**（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`，CMake `target_sources` 按平台选择编译，与 EUI-NEO 后端选择同一模式），业务代码零 `#ifdef _WIN32` / `__APPLE__`（2026-09-22 W2 验收 grep `src/` *.cpp/*.h 零命中）；HTTP 层（libcurl/cpp-httplib）本身跨平台；`.app` target（`if(APPLE)` 守卫）、Security.framework 链接（有守卫）。time 统一走 `core::time::localTime`（`src/core/time.h` + `time_win/posix.cpp`）。

**测试框架注意**：`tests/doctest.h` 是自制 87 行迷你框架（`REGISTER_TEST` 宏静态自注册 + `DOCTEST_CHECK/REQUIRE` 断言，fail-fast），**不是真 doctest**——grep `TEST_CASE` 找不到用例、`--list-test-cases` 之类参数会被当作过滤子串属正常。**用例总数以运行输出 `Total:` 为准，不在文档写死**——2026-09-28 波 1 合并后**（Windows 侧）**实测 `Total: 646 / Passed: 645 / Failed: 0`（Total 比 Passed 多的那 1 条是 `DEVDESK_SOAK=1` 门控的 soak，常规套件打印 `[SKIP]` 直接跳过）；波 1 之前的基线是 `Total: 621 / Passed: 619 / Failed: 1`，那条 Failed 是模块隔离守卫的 cwd 缺陷（ccache 把 `__FILE__` 相对化后、从仓库根跑必误报「找不到仓库根」），已由波 1 迁到 `scripts/check-module-isolation.sh` 修复。以下数字均为**历史**快照，只作沿革记录、不再作为当前值引用：530（2026-09-27）→ 514（同日更早）→ 481（2026-09-24；= 09-23 基线 330 + W6-W9/磨砂全链/供应商配置/桌宠 MVP/Markdown 键盘流等 backlog 波 + 词法器加 JS/Go/Rust/Shell +4 + 挂机穿透功能移除 -1）。test_main 支持子串过滤：`./devdesk_tests.exe stress` 只跑名字含 "stress" 的用例，`--log` 打开 Debug 级控制台日志。

## 🐾 桌宠（module_pet）皮肤资源

内置皮肤包：`assets/pets/`（git 追踪），现内置 **6 个 GitHub 皮肤包**（目录名 = 皮肤名，括号为 `pet.json` frameSize）：

| 皮肤 | frameSize | 皮肤 | frameSize |
|------|-----------|------|-----------|
| copilot | 136×168 | kirari | 360×480 |
| ileina | 192×208 | whale-girl | 328×212 |
| sakura-chan | 512×512 | whale-maid | 512×512 |

- CMake POST_BUILD `copy_directory`（`CMakeLists.txt:184-188`）把 `assets/pets` 拷到 `build/assets/pets`，从 build 目录启动 DevDesk 即可发现
- **动态发现，零硬编码注册**：`discoverSkins()`（`src/module_pet/pet_skin.cpp:723`）扫 `skinRootCandidates()`（:702）两根——`petsDir()`（appData）+ `exeDir/assets/pets`，每个含 `pet.json` 的子目录自动入皮肤清单
- renderer 帧尺寸校验 clamp [16,512]（pet_skin.cpp:559）；程序生成的内置皮肤走 `petsDir()`，不放在 `assets/pets`（CMakeLists 注释）

**皮肤格式**：目录 = `pet.json` + PNG 序列帧；`pet.json` = `{name, frameSize:[w,h], frameRate, license, source, phrases, states:{idle,poked,sleep,typing}}`。

## 📦 安装程序（2026-09-28）

- **Windows**：Inno Setup `installer/windows/devdesk.iss` —— 用户级安装（`%LOCALAPPDATA%\Programs\DevDesk`，`PrivilegesRequired=lowest`，免管理员）。`:65` `Source: "{#AssetsDir}\*"; DestDir: "{app}\assets"; Flags: recursesubdirs createallsubdirs` 递归拷 assets（含 `assets/pets/`）到安装目录；`.exe` 单独一条（:59，loose 文件非 embed）；`:60-64` 注释：Excludes 排除 17MB `NotoSansSC-VF.ttf`（变量字体 FreeType 加载不稳、当前未启用，运行时字体走 EUI-NEO fallback 链）
- **打包脚本**：`scripts/package-windows.sh`（Release 构建 + ISCC 三段式探测 + 出 `DevDesk-<ver>-setup.exe`）；macOS：`installer/macos/`（DMG 背景 + 首次运行去隔离 `.command`）+ `scripts/package-macos.sh`
- spec：`docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md`

## 📂 关键文件

| 文件 | 作用 |
|------|------|
| `docs/superpowers/specs/2026-09-20-devdesk-design.md` | **唯一权威 spec**，所有架构 / API / token 决策都在这 |
| `docs/mockups/palette-comparison.html` | 配色对比 demo（已确定方案 B）|
| `docs/mockups/devdesk-prototype.html` | HTML 交互原型（待生成）|
| `../EUI-NEO/` | 上游 GUI 框架（同级目录源码，非 submodule），通过 `EUI_NEO_DIR` 以 `add_subdirectory` 集成；**当前依赖 rev `72a5187`（本地 main 领先 origin 93 个提交（2026-09-30 实测）且已决策永不 push，需保持最新，见「常用命令」段与审计 [11]）** |
| `docs/superpowers/specs/2026-09-27-*.md` | 09-27 批次 specs：wave1-fix / god-class-refactor / shared-config-and-failover |
| `docs/superpowers/specs/2026-09-28-*.md` | 09-28 批次 specs：installer-and-dmg / devdesk-probes / r8-assembly-layer / tin06-test-scaffold-migration / setscrolloffset-upstream-m6-refactor / md01-reload-block-mapping / macos-three-features / ui-modernization / profile-run |

## 🧪 验证原则

- **未运行测试 = 未完成**
- **未截屏 = 未完成**
- **未读 spec = 未开始**
- **未 commit = 未持久化**
- **不要假设 EUI-NEO 能力** — 不确定时去 `../EUI-NEO/` 源码 grep

---

## 🤝 AI 协作与交付纪律（团队通用）

> 本节是**团队 / 跨会话**通用纪律：谁改文件、怎么派单、怎么收尾、审计口径、已踩过的环境坑。工具链细节见同文件「常用命令」段，架构约束见「项目核心约束」段。

### 派单与并发

- **主会话禁改文件**（2026-09-27 定）：源码 / 测试 / 配置 / 文档一律**派子任务**改；主会话只做需求裁决、任务拆分、派单、只读核查（Read/Grep/Glob）、产物审计、合并与提交决策。**边界**：`commit` / `merge` 不算改文件（写的是 `.git` 记账、不动工作区），主会话可直接执行——别拿这条当不提交的借口；构建 / 测试 / 只读 shell 同样可在主会话跑。*为什么*：既写又审会失去第二方视角（当时主会话连续直接改源码和配置被纠）。
- **粒度看复杂度，不看文件数**：引用点已查清、无需独立判断的机械改动（3-6 文件也算）**合并成 1 个子任务**做完，不拆成 N 个；需独立探索 / 设计判断 / 影响面广 / 要第二视角才拆多个并行。*为什么*：拆细后子任务启动 + 产物验证 + 等通知的成本高于收益（6 文件机械删除被派了独立子代理，用户纠"不复杂时成本反而更高"）。与上一条合起来 = **"少派、但必须派"**（"主会话自己 inline 做"的旧做法已作废）。
- **并发上限不限，按机器饱和度自调**（2026-09-28 用户指令取消 3 的封顶）。**仍保留的约束**：① 同一文件同一时刻只有一个写者（见下条）；② 多 worktree 并发构建互拖时主会话主动收束（I/O / 内存带宽饱和、ccache 热命中率被挤压 → 串行排队或降 `DEVDESK_JOBS`），不必等用户干预。**沿革**：2026-09-27 先定 7、当晚上调 10、09-28 下调 3，同日再取消封顶；不再沿用「并发 ≤ 2」的保守默认。构建并行度是另一件事，见「常用命令」段的 `DEVDESK_JOBS=3`。
- **同一文件同一时刻只有一个写者**：派单前先列在飞白名单做交叠检查。*反例*：漏检让 R0 与 H2 同写 `settings_module.cpp`，只能 `TaskStop` 一条。
- **禁止长时间 sleep（上限 60s）**：脚本/命令里不许 `sleep > 60`（如 `sleep 120` 禁用）；派单 prompt 明确禁子任务用长 sleep 等待（构建超时用 `timeout` 包，测试异步用有界轮询/事件信号而非固定长睡）。*为什么*：2026-09-28 用户指令，长 sleep 拖垮节奏且无信号价值。
- **子代理不吃会话风格**：caveman / 极简是**每会话**设置、子代理不继承，不写规则就吐长报告——长报告是主会话 token 的主要开销。派单 prompt 末尾固定加：「报告用极简风格——删铺垫 / 客套 / 复述 / 过程叙述，只留结论、证据（file:符号名 + 最短决定性输出）、sha、数字、待办；每条结论一行；总长 ≤ 原上限一半；技术精度不变，不许为省字含糊」。适合的场景改用 `cavecrew-*` agent（`cavecrew-investigator` 定位代码 / `cavecrew-builder` 一到两文件小改 / `cavecrew-reviewer` diff 审查），其输出本身即压缩形态。**边界**：写进仓库的文档 / commit message / memory 仍是正常文字（极简只针对给主会话的报告）。
- **意外终止先唤醒原 agent，不重派**：子任务崩溃 / 卡死被清理 / 静默失联（无 sha、无报告）时，用 `SendMessage` 唤醒**同一个** agent 从 transcript 续跑，写明"你上次停在哪、接着做别从头再来"；**不是**主会话接手，**不是**派新子任务重做（换新 agent 丢上下文 = 从零烧 token + 重跑全量编译）。有回单但结论存疑 = 走审计流程，不属本条。唤醒失败（agent 已被彻底回收）→ **升级给用户**定夺，不自行改派。派单时即记录 agent id（无人值守靠 watchdog 先探活、再唤醒）。

### 交付流水线（四律，按序叠加）

1. **只跑定向测试**：只编译受影响目标 + 只跑相关用例（`./build/tests/devdesk_tests.exe <功能子串>`，如 `markdown` / `proxy` / `settings`）。**禁全量重建、禁全量套件**（用户原话"进度太慢了"）；全量只在里程碑收尾 / 提交前由主会话跑一次。
2. **涉及编译 / 测试的子任务必须 worktree 隔离**（`isolation: "worktree"` 或手动 `git worktree add`），**主工作树禁跑构建**；worktree 首建用 `scripts/build-test-run.sh` 自动 configure（`-DEUI_NEO_DIR=D:/project/gui/EUI-NEO` + 既有 OPENSSL / ZLIB 参数）。
3. **完成即提交并合 master**：agent 在自己的分支原子 commit（**显式 pathspec，禁 `git add -A`**）并报 sha → 主会话审计（读 diff + 复跑验收）→ **串行**合并进 master（多 agent 同时 merge 会撞 `index.lock`）。**不攒批**。
4. **worktree 用完即清**：合并后立即 `git worktree remove <path>` + `git worktree prune`；有未跟踪文件先看内容、必要时先备份到仓库外再 `--force`。**禁止残留**（曾一次清出 12 个 EUI-NEO 残留 worktree）。

*为什么*：全量测试每轮 ~2 分钟且互相抢 build；主工作树脏 + 多 agent 同写导致"任务完成也没法干净提交"。**注意**：worktree 是从**提交**切出的 → **WIP 不落地，新 worktree 任务就起不来**（规则 3 是规则 2 的前提）。
**卡死多半是系统休眠，不是 agent 出问题**：本机休眠会让后台 agent 被判"600 s 无进展"而中断（环境事件、不是子任务失败）。唤醒机器后：① 看哪些 agent 断了 → `SendMessage` 唤醒续跑；② 空槽按"空槽即补"填满。用户输入【继续】= 唤醒 + 拉活，不用再问。

### 功能开发生命周期（六阶段，2026-09-28 定）

> **为什么立这一节**：2026-09-28 一天内集中暴露出一批缺陷——回归、平台不对称、静默失败、假绿测试、文档失实、交付没合并 / 没装对——它们的共同点是：改动"实现完了"就被当成"做完了"。从今天起，每个功能 / 修复 / 开发都按下面六个阶段走，每个阶段写明谁做、做什么、通过判据、不过怎么办；**判据不满足就停在原地，不得进入下一阶段**。本节与上面「交付流水线（四律）」叠加使用：四律管派单、worktree 与提交纪律，本节管一个改动从实现到验收的完整流转。

**阶段 1：实现**

- 谁做：派子任务实现（粒度按「派单与并发」的原则）；主会话不写文件。
- 做什么：在 worktree 隔离分支上完成改动，原子 commit（显式 pathspec，禁 `git add -A`）。
- 通过判据：实现子任务回单**含分支上的 commit sha + 改动文件清单 + "改了什么、为什么这么改"的说明**；三者缺一不算完成。
- 不过怎么办：缺 sha 或缺说明 → 用 `SendMessage` 唤醒原 agent 补齐（沿用「意外终止先唤醒原 agent」），不重派、不回滚重做。

**阶段 2：独立审计（强制关卡，2026-09-28 新增）**

- 谁做：**必须另派一个独立的只读子任务**做审计；**审计者不得是实现者本人**，也不得参与本轮改动。审计者只读代码与产物，唯一允许的写动作是落盘审计文件。（「交付流水线」规则 3 里主会话"读 diff + 复跑验收"是合并前核查，不能替代本阶段的独立审计；「审计与决策口径」段的四轴 / 分片是**全仓**审计口径，本阶段是**每个改动**的最小审计清单，两者并行、互不豁免。）
- 做什么：按下面六个维度逐条核，产出**结构化 findings**（每条 = `file:line` + 问题 + 影响 + 建议），写入 `docs/superpowers/plans/2026-09-28-<主题>-audit.md`（或 `.json`，与既有 `*-audit-findings.json` 同风格）留痕。六个维度：
  1. **正确性**：改动是否真的解决了目标问题——对照用户原始诉求逐条核对，而不是对照实现者自己的复述。
  2. **回归面**：改动触及的调用点 / 依赖点是否全部同步更新。（实证：把「最近调用」从一个容器搬到另一个容器时漏搬锚点，用户看到 60% 空白；插一个函数时顺带删掉两段无关代码。）
  3. **平台对称性**：改动是否只在单一平台上验证过；另一平台的代码路径会不会因此变化。（实证：Windows 期间写的 macOS 代码从未编译过，回 macOS 后首次构建 115 / 117 条用例失败。）
  4. **静默失败面**：新增或触及的错误路径是否"不崩但也不报"——吞掉异常、兜底 clamp、返回默认值而不留任何日志。（实证：`@catch { return false; }` 让材质从未挂上却一行日志都没有；`clamp(..., 3, 10)` 把"高度不足"变成"少几行不报错"。）
  5. **测试有效性**：新增断言是否真能在改动回退时变红（变异验证），还是恒真的假绿。
  6. **文档口径**：文档里是否有与新行为矛盾的陈述。（实证：文档写「0 未提交改动」而实际有 3 个未跟踪文件；文档写了不存在的字段名。）
- 通过判据：审计文件已落盘，且**六个维度逐条给出结论**（"未发现问题"也要写明"已核"），每条 finding 带 `file:line`。任一维度未作答、findings 缺 `file:line`、或审计者与实现者是同一 agent，审计即无效。
- 不过怎么办：有任何 finding → 进入阶段 3；审计文件缺失 / 维度未答全 → 审计不算通过，唤醒原审计 agent 补齐，不得直接进阶段 4。

**阶段 3：按审计修**

- 谁做：派子任务在功能分支上修（沿用阶段 1 的 worktree 与提交纪律）。
- 做什么：逐条处理 findings——修复，或明确判定"不修"并写明理由。
- 通过判据：findings 文件里每条都有处置结论（已修并给出新 sha / 不修并写明理由），且相关维度已由审计方（或另一个独立子任务）复查通过。
- 不过怎么办：复查未过 → 继续修，不得进入阶段 4。

**阶段 4：测试（审计通过后才准跑）**

- 谁做：在功能分支的 worktree 内执行；构建与单测一律走 `scripts/build-test-run.sh`（见「常用命令」段，禁止手敲 cmake）。
- 做什么：
  - **单测**：跑 `devdesk_tests` 的定向用例（`bash scripts/build-test-run.sh --filter <相关子串>`，如 `markdown` / `proxy` / `settings`）；改动涉及共享层（`core` / `platform` / 公共头）时跑全量。
  - **UI 测试**：跑 `tests/probes/` 探针（`(cd build && ctest -L probe)`，或直接跑单个 probe 可执行文件）。`build-test-run.sh` 不跑探针，必须单独执行。probe 能力：`tests/probes/probe_harness.h` 提供真实窗口后端（GLFW + OpenGL）+ 真实指针 / 键盘 / 滚轮 / 文本 / 拖拽注入 + 几何与 z 序断言。**凡改动涉及可见 UI 的，必须有 probe 或等价的无头几何断言覆盖——光有单测不算。**
  - **覆盖率口径（当前无工具支撑）**：仓库里**没有任何覆盖率工具在跑**——spec §12.3 规划过 `llvm-cov` / `gcovr` 与分层行覆盖目标（核心工具类 ≥ 90% / Service ≥ 80% / Platform ≥ 70%），但从未落地：CMake 里没有 `--coverage` 插桩配置，`scripts/` 里没有覆盖率步骤，也没有 CI 在收集。**引入覆盖率工具是一个独立任务，尚未启动**；在那之前不要引用任何覆盖率数字、不要假装已有覆盖率。工具就位前，用三条可检查的口径替代：① 新增 / 修改的行为必须有断言，没有断言的改动视为未完成；② 修复类改动必须有"用例在修复前会红、修复后会绿"的证据——回退一次修复跑出红，或用变异验证等价地证明断言非恒真；恒真断言即假绿，须重写；③ 涉及几何 / 布局的改动必须有数值断言，不接受"看起来对"。
- 通过判据：定向用例全绿（报告贴出 `Total / Passed / Failed` 行且 `Failed: 0`）；涉及可见 UI 的改动有 probe 覆盖且 `ctest -L probe` 全绿；修复类改动有"先红后绿"（或变异验证）证据；几何 / 布局改动有数值断言；**凡改动涉及可见 UI 的，实现者必须交付前自己启动 app、打开窗口、截图看效果**（断言能证明"几何数值对"，证明不了"看起来对"）——可复制做法：① 用**独立的 `DEVDESK_DATA_ROOT`** 启动自己 worktree 的构建产物（**不得用用户真实数据根、不得碰 `/Applications/DevDesk.app`**）；② `screencapture -x <file>` 截图（屏幕录制权限已授，实测可用）；③ 需要点击 / 移动鼠标驱动界面时用 `cliclick`（`/opt/homebrew/bin/cliclick`，坐标是**点** = 物理像素 ÷ 2；**AppleScript `click at` 对本 app 无效，别用**）；④ **已知坑**：`screencapture` 对 OpenGL 窗口**可能抓到导航前的旧帧**——移动鼠标触发重绘后再拍第二张、取后一张，报告里要说明是否遇到；⑤ **⚠️ 严禁 `pkill -f DevDesk`**——多个 agent 可能同时在跑 app（还有用户自己在用的实例），按名杀会互相杀并杀掉用户的 app，**只按自己启动的 PID 杀**。*为什么*：2026-09-28 的真实事故——标题栏无底色、详情面板 60% 空白、卡片缺边距、侧边栏边距改错层次，**全都是用户肉眼先发现、我们事后才验证的**。
- 不过怎么办：任何一项红 → 回阶段 1 / 阶段 3 修；发现恒真断言 → 重写断言（不许把断言删掉了事）。

**阶段 5：合并与发布**

- 谁做：主会话执行合并，**串行**（一次只合一条；多 agent 同时 merge 会撞 `index.lock`）；合并后的重建与安装同样在 master 上做。
- 做什么：功能分支完成审计 + 测试后合并进 master；合并后在 master 上重新构建 + 安装——用户的标准是「编译 = 编译 + 复制到 `/Applications`」。
- 通过判据：master 上有本次改动的 merge commit（报告给出 merge sha），且合并后重建通过、已覆盖安装到 `/Applications/DevDesk.app`。**没合并 = 没交付。**
- 不过怎么办：合并后构建 / 测试红 → 不得交付（回退或修复后重走本阶段）。
- *为什么（今天的血教训）*：实现了、测试也绿了，但没合进 master，用户在新功能上验证的还是旧代码，得出"还是没效果"；另一次是安装的还是修复前的版本，用户同样在旧代码上验证新功能。

**阶段 6：真机验收**

- 谁做：用户在真机上点验。
- 做什么：涉及视觉 / 交互的改动，交付后由用户在自己的机器上点验。本机 `screencapture` 已可用（屏幕录制权限已授），但**对 OpenGL 窗口可能抓到导航前的旧帧**——截图只作"观感"补刀，几何正确性以阶段 4 的数值断言为准。
- 通过判据：用户在真机上点验确认通过（或提出问题 → 修复 → 复验通过）。
- 不过怎么办：用户点出的问题按新 bug 重走完整六阶段，不因"小"而跳过审计与测试。

### 交付物与测试环境隔离（2026-09-28 定）

- **`/Applications/DevDesk.app` 是交付物，不是测试对象**：agent 不得把它当工作区——不得为验证 / 调试直接改动 bundle 内容（塞文件、改 Info.plist、覆盖内层二进制、拿它当运行目标反复覆盖）；唯一允许的写操作是阶段 5 的整包覆盖安装（交付动作，不是调试手段）。测试一律用独立 `DEVDESK_DATA_ROOT`（覆盖优先级见 `src/core/paths.h`：`DEVDESK_CONFIG_PATH` > `DEVDESK_DATA_ROOT` > 平台默认根）+ 独立构建目录，不碰用户的真实数据与安装产物。
- *为什么*：今天出过两次——「装的是修复前版本 → 用户在新功能上验证，实际跑的是旧代码」，以及「bundle 里根本没有可执行文件」；两次都是把交付物当成可随手改动的测试对象造成的。

### 审计与决策口径

- **全量代码审计必须分片 + 专业角色**（2026-09-27 定）：按片（core+platform / claude_proxy / markdown / todo+home+settings / pet+app 层+构建 / tests+基建）各派一个**只读**子任务，产结构化 JSON（`docs/superpowers/plans/2026-09-27-full-audit-<area>.json`），最后合并 + 主会话抽验。**不是**一条线扫全仓——单线全仓既漏片、又没人对"设计质量"发表意见。
- **审计四轴**：主线是**架构 / 设计模式 / 面向对象 / 可维护性**；语言级缺陷（UB / 并发 / 泄漏）降为次要维度。两条角色线各自完整过一遍：**C++20 系统开发专家 + 软件架构师**，findings 加 `lens` 字段。
- **findings 口径**：`impact` 写**维护成本 / 演进代价**（如"加一个新 provider 字段要改几处""改一处的波及面"），severity 按"多难维护 / 多难演进"定，**不按"会不会崩"**。去重时对照 `docs/superpowers/plans/2026-09-27-expert-audit.md` 标 `already_registered`；只修确认项，且走"只修需求交付物缺陷"的范围纪律。
- **先找两全解，再谈取舍**：遇"需求 vs 既有约束"冲突，先穷举能同时满足的落点（换位置 / 换层次 / 换时机 / 拆两步），自己判断哪条更省；只有确实互斥（满足 A 必破 B）才升级给用户，并写明"为什么无法两全"。**禁止**把可两全的事摆成 A / B / C 非此即彼——推荐项放第一，若有"两全"选项它必须是我的推荐而非第三选项；也别照搬文档里的对立建议当二选一。两全方案若动了既有守卫 / 测试，仍按范围纪律显式授权白名单。*反例*：extern 桥收口撞模块隔离守卫，被摆成「撤销 / 破守卫 / 中立层」三选一，用户回"整合啊""为什么非要二选一"——把桥头放到中立层即可两全。

### 环境与工具链

- **构建 / 测试**：工具链 PATH 三件套（cmake 4.3.3 / ninja / msys64 mingw64，见「常用命令」）；测试二进制 `build/tests/devdesk_tests.exe` **支持子串过滤**。测试框架是**自制迷你 doctest**（`REGISTER_TEST` + fail-fast，**不是**真 doctest）——grep `TEST_CASE` 找不到用例、`--list-test-cases` 被当过滤子串属正常。
- **并行度封顶**：`CMAKE_BUILD_PARALLEL_LEVEL` = `DEVDESK_JOBS`（**默认 3**，`scripts/build-test-run.sh`）——12 核上多条 worktree 并发构建时避免互拖；单条独占机器才调高（如 `DEVDESK_JOBS=8`）。
- **ccache 三件套已落持久配置**：`%LOCALAPPDATA%\ccache\ccache.conf` + `D:/software/ccache-devdesk/ccache.conf`（`cache_dir=D:/software/ccache-devdesk` / `base_dir=D:/project/gui` / `hash_dir=false` / `max_size=20G`），**无需任何 `CCACHE_*` 环境变量**；手敲 cmake 只需带两个 `-DCMAKE_*_COMPILER_LAUNCHER=ccache`。**不设 BASEDIR = 白装**：每棵 worktree 绝对路径不同 → 同一份源码哈希不同 → 永远 miss。
- **codegraph 索引排除 worktree / build**：`D:/project/gui/codegraph.json` 的 `exclude` = `.claude/worktrees/**`、`DevDesk/.claude/worktrees/**`、`wt-*/**`、`**/build/**`（新增临时目录时同步维护，别把产物灌进索引）。
- **`DevDesk.exe` 会锁 build 目录**：运行中链接报 `cannot open output file DevDesk.exe: Permission denied` → `taskkill -F -IM DevDesk.exe` 解锁（**别杀 `claude.exe` / `cc-switch.exe`**）。
- **代理直测脚本**：`doc/scripts/proxy_smoke_test.py`（`proxy` / `upstream` / `all`；环境变量 `DEVDESK_PROXY` / `DEVDESK_MODEL` / `DEVDESK_TOKEN`）。
- **EUI-NEO 最低依赖 rev `72a5187`**：`CMakeLists.txt` 的 `DEVDESK_EUI_NEO_MIN_REV`，需与本文件「常用命令」段的记录保持一致，消费 EUI 新 API 后两处同步更新；EUI-NEO 要跑在本地 main 最新（本地领先 origin 93 个提交（2026-09-30 实测）；2026-09-28 用户决策永不 push，见「常用命令」段）。
- **已踩过的坑（写码前先想一遍）**：
  - **单位错配（系统性）**：`windowSize` / `glfwCreateWindow` 是**客户区物理像素**，compose 侧按 `framebuffer/(dpiScale×uiScale)` 折算逻辑像素——菜单窗、启动窗、桌宠锚点都栽过。本机 125%：工作区 1920×1020 物理、任务栏 60 px，窗口客户区底部 52 物理 px 会被任务栏吃掉（贴底 UI 不可见）。
    - **上面这条只是 Windows 口径**；macOS 口径（2026-09-28 实机回填，三行实测）：① `windowSize` / `glfwCreateWindow` / `glfwSetWindowSizeLimits` 的单位是**点**——`core::window::windowScaleForPoint` 在 macOS **恒返 1.0**（Retina 的 2.0 是 contentScale/backing scale，**不是**窗口单位换算，乘上去会双倍放大）；② 本机实测 `framebuffer 2940×1602 / contentScale 2.0 → 逻辑 1470×801`，即 `screen.height` **等于客户区点高**（两平台都只除一次，无多除少除——"Retina 没折算"的假设已被此数据证伪）；③ 工作区 1470×833 点、标题栏 28 点 → 客户区 805 点，而设计最小窗高 845 点 > 833 点：最小窗尺寸必须经 `core::window_layout::minWindowSizeForWorkArea` 做**同一条换算**再与工作区取 min（只做其中一步就是把病换个平台复制）。非客户区外框同样按平台注入：macOS `{0,28}`、Windows `{14.4,37.6}@96dpi`（`window_layout_macos.cpp` / `_win.cpp`）。
  - **三套坐标系**：窗口坐标（`onContextMenu` 给的是它）/ 页面坐标（`app.page` 子树，原点 x = 侧边栏宽 **208/72**）/ 控件局部坐标，跨层必须换算（`Router::pageOriginX` 为此而加）。
  - **上游方言 `upstream_dialect`**：UA 不能透传客户端（Cloudflare 1010 按 UA 封）、`opencode.ai/zen/go` 需 `x-opencode-session`、模型名带 `[1M]` 要剥。
  - **自定义 check 必须 raise**（只 print 的"全绿"是假象）。
  - **负载敏感 flake**：`md_preview_highlight` / `md_highlight` 的 `<8ms` 墙钟断言、`test_claude_ui` 的固定端口 28888。
- **`~/.claude/settings.json` 归属（2026-09-27 定）**：**DevDesk 是 settings.json 接管功能的终局 owner**，cc-switch（本地代理 15721 + `sessionAutoSyncEnabled` 反向同步）只在过渡期并存，**不为它做任何兼容 / 协调处理**（用户原话"不需要管 cc-switch，本软件开发完善后就不使用了"）。并存期两边都会整体重写 settings.json；cc-switch 库里存有 DevDesk 代理时代的旧快照（`ANTHROPIC_BASE_URL=http://127.0.0.1:8888` + 老 token），切 provider 时可能吐回 → 用户感知为"配置一直被还原"。**识别写者看键名**：DevDesk 写 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED`，cc-switch 写 `ANTHROPIC_API_KEY=PROXY_MANAGED`。DevDesk 三个写入时机（`claude_service.cpp`）：起代理 takeOver / 运行中切 profile 再同步 / 停代理或退出按键还原；启动只在有残留 sidecar 时补还原（`main.cpp` 启动序列）。再遇"配置被还原"类反馈：先按键名判写者，是 cc-switch 写的一句话说明即止——**不派任务、不做兼容**。

### 反事故红线

- **禁批量清理"在飞"的 worktree**：清理前先 `ListAgents` 判活，在飞的绝不碰。判定式必须含"**有提交**"这条：`tip != base && git merge-base --is-ancestor <branch> HEAD`（base 取该 worktree 创建时的 master tip，可从分支 reflog 的 `branch: Created from` 读）。批量删除**逐条确认**，别一条命令梭哈——`git worktree remove` 在文件被占用时会**半删**（`.git` 没了、目录还在），恢复成本更高。*事故*：只用 `--is-ancestor` 判定时，**在飞 agent 的分支 tip 就是基线提交（尚未提交）也为真** → 三条在干活的 worktree 连同未提交改动一起被删、另两条半残，三条线只能重做（成果默认只在工作区，"无提交"与"已合并"在分支指针上长得一样）。误删后：`ls` 看目录是否残留 → `git worktree repair`；`.git` 已丢则重建同路径同名分支的干净 worktree，再 `SendMessage` 唤醒原 agent 凭其报告重做（transcript 里的清单能省掉重新分析）。
- **跨仓库改动必须"产补丁 + 非隔离线应用"**：带 `isolation: "worktree"` 的 agent **无法对任何其它仓库执行 git 操作**（`cd <别的仓库> && git …` 与 `git -C <任意路径> …` 一律被隔离守卫拒）→ 建不了别的仓库的 worktree、也提交不了。正确形态：**隔离 agent 产 patch + 验证**（可用文件副本模拟依赖来真跑构建 / 测试）→ **另起一条"非隔离" agent**（不设 `isolation`）把 patch `git apply` 进目标仓库、跑编译、原子 commit、清理副本；非隔离线的 prompt 必须写死"只许改 `<目标仓库>`，禁止碰 DevDesk 文件"。反过来**别让非隔离 agent 碰 DevDesk**（它没有隔离保护，工作目录就是主树）。主会话**不要**替 agent 落文件（含 `git apply`）。*为什么*：DevDesk 与 EUI-NEO 是两个仓库（同级目录、非 submodule），上游能力改造天然跨仓库。
- **`git merge` 不与 Agent 派单放在同一条消息里并行跑**：主会话 git 操作与派单并行会因 git 锁竞争导致隔离校验失败（实测两条派单被拒、留下两个孤儿 worktree）。