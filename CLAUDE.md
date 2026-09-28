# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

**DevDesk** 是面向开发者的跨平台桌面工具集（C++20 GUI 应用），包含三大工具：

- **Markdown 笔记本**：本地文件存储 + SQLite FTS5 全文搜索
- **Todo 看板**：SQLite 存储，多列卡片管理
- **Claude 配置切换器**：多套 API profile 管理 + 本地 HTTP 代理（带流式转发）

另有首页仪表盘、设置页（模块插拔开关）与**桌宠**（module_pet，非 Router 页面模块，无边框置顶透明子窗，默认关闭、经设置页模块开关启用）。

目标平台：**Windows 11**（当前开发环境，2026-09 从 macOS 迁移而来）和 **macOS 27+**（已完成主线，平台分支代码保留可用）。

## 🚨 项目核心约束（最重要的几条）

1. **用户不熟悉 C++，纯 AI 开发** — 代码对用户不可见，所有反馈通过：文档 / 中文日志 / 截屏 / 测试结果 / mockup
2. **完整设计文档**：所有架构 / API / 设计 token 决策都写在 `docs/superpowers/specs/2026-09-20-devdesk-design.md`，**实现前先读 spec**
3. **每个 P 阶段完成后必须**：跑测试 + 启动应用 + 截屏 + 更新文档 + git commit（详见 spec §16.2）
4. **配色方案已定为 B（温暖专业 / Claude.ai 风格）**，完整 token 写在 spec §6.5.1 和 §6.5.2
5. **平台适配代码必须接口隔离**（2026-09-21 定）：头文件定义平台无关接口，实现按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake 按平台选择编译（与 EUI-NEO 后端选择同一模式）。**禁止在业务代码里散布 `#ifdef _WIN32` / `__APPLE__`**。虚接口仅在需要运行期注入/mock 时对单个点引入，不全局铺开

## 🛠️ 常用命令

**Windows 11（当前开发平台，MSYS2 MinGW64 + Ninja）**：

```bash
export PATH="/d/software/cmake/cmake-4.3.3-windows-x86_64/bin:/d/software/ninja:/c/msys64/mingw64/bin:$PATH"

cd DevDesk
rm -rf build   # 换平台后必须（旧 build/ 是 macOS Darwin arm64 产物）
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

**macOS（历史平台，仅打包脚本保留）**：macOS 主线（P0-P8）在 2026-09 迁 Windows 前完成，迁后仓库内只剩 `platform/macos/build-app.sh` 与 `scripts/package-macos.sh` 两个打包辅助脚本。本文档旧版提到的 `./scripts/dev.sh`（构建+运行）、`./scripts/test.sh`、`./scripts/screenshot.sh <page>`、`./scripts/reset-db.sh` **在本仓库中不存在**（`git ls-files scripts/` 可核），不要再照抄执行。`check-consistency.sh` 已 Windows 化（波 1 TIN-13）：Playwright 截 HTML demo + PowerShell `CopyFromScreen` 截主窗口 + Pillow 合成回退，二进制名按 `uname` 分平台取 `build/DevDesk.exe`，不再是 macOS 专用脚本。

构建入口：`CMakeLists.txt` 顶层 → `src/main.cpp`。
EUI-NEO 依赖：`../EUI-NEO/`（同级目录，通过 `EUI_NEO_DIR` cache 变量 `add_subdirectory` 集成，**不是 git submodule**）。**当前依赖 rev**：`269a8c4`（2026-09-27，U1 尺度契约合入点；EUI-NEO 本地 main 领先 origin 70 个提交；这批提交尚未 push 到 origin——origin/main 停在 v0.6.0，push 与否是用户决策门控）。**EUI-NEO 需保持本地 main 最新**（含 onWindowDestroyed/requestFocus/trayMenu/windowEffect/lineStyles/getMonitorWorkAreas/setSystemPowerHandler/styledMenu/onTrayMenu/windowIsFocused/input onCancel 等新 API），顶层 CMake configure 时打印 `EUI-NEO git rev` 并在缺失最低依赖 rev（`DEVDESK_EUI_NEO_MIN_REV`，与本行保持一致、消费 EUI 新 API 后两处同步更新）时软警告不 fail；EUI-NEO push 后将启用硬版本检查（审计 [11]）。

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

## 🚦 当前阶段（2026-09-22 更新，macOS → Windows 11 迁移后）

按 spec §16 实施，进度：

- ✅ **P0-P8 完成**（macOS 上 66 测试通过 + .app bundle，截图见 `docs/screenshots/`）
- ✅ **P9（代理非流式）+ P10a（SSE 流式）+ P10b（流式压测）完成** —— W1 收编 macOS 中断现场并修全部基线问题（T1-T5），Windows 全量测试 109 例全绿（W2 验收 2026-09-22：10MB e2e/soak/断连恢复/内存采样全过）
- ✅ **P14（Windows）必选达成** —— spec §16 已更新：P14 升必选（Windows 11 为当前开发环境）+ 发布形态约束"单 exe 零外部 DLL"（T3 达成，W2 复查通过）；平台适配接口隔离完成（T6，业务代码零平台宏）
- ✅ **W3 剩余需求收官（T9–T12 + P16）** —— 启动顺序理顺零 WARN、P12 Home 真实数据、P13 设置页、platform::json 实装、P15 文档（124/124 全绿），恢复计划关闭
- ✅ **W5 遗留项清零（2026-09-22）** —— Claude 按钮全量接线、侧边栏导航（删浮动齿轮）、全局字号缩放、UTF-8 argv、clearColor 随主题、跨模块跳转负载（Home 快捷按钮弹窗 + 最近编辑打开指定笔记）。最终基线 **137/137 全绿**；详见 CHANGELOG [W5] 与 KNOWN_LIMITATIONS「已解决」小节

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

**测试框架注意**：`tests/doctest.h` 是自制 87 行迷你框架（`REGISTER_TEST` 宏静态自注册 + `DOCTEST_CHECK/REQUIRE` 断言，fail-fast），**不是真 doctest**——grep `TEST_CASE` 找不到用例、`--list-test-cases` 之类参数会被当作过滤子串属正常。**用例总数以运行输出 `Total:` 为准，不在文档写死**——2026-09-28 波 1 合并后实测 `Total: 646 / Passed: 645 / Failed: 0`（Total 比 Passed 多的那 1 条是 `DEVDESK_SOAK=1` 门控的 soak，常规套件打印 `[SKIP]` 直接跳过）；波 1 之前的基线是 `Total: 621 / Passed: 619 / Failed: 1`，那条 Failed 是模块隔离守卫的 cwd 缺陷（ccache 把 `__FILE__` 相对化后、从仓库根跑必误报「找不到仓库根」），已由波 1 迁到 `scripts/check-module-isolation.sh` 修复。以下数字均为**历史**快照，只作沿革记录、不再作为当前值引用：530（2026-09-27）→ 514（同日更早）→ 481（2026-09-24；= 09-23 基线 330 + W6-W9/磨砂全链/供应商配置/桌宠 MVP/Markdown 键盘流等 backlog 波 + 词法器加 JS/Go/Rust/Shell +4 + 挂机穿透功能移除 -1）。test_main 支持子串过滤：`./devdesk_tests.exe stress` 只跑名字含 "stress" 的用例，`--log` 打开 Debug 级控制台日志。

## 📂 关键文件

| 文件 | 作用 |
|------|------|
| `docs/superpowers/specs/2026-09-20-devdesk-design.md` | **唯一权威 spec**，所有架构 / API / token 决策都在这 |
| `docs/mockups/palette-comparison.html` | 配色对比 demo（已确定方案 B）|
| `docs/mockups/devdesk-prototype.html` | HTML 交互原型（待生成）|
| `../EUI-NEO/` | 上游 GUI 框架（同级目录源码，非 submodule），通过 `EUI_NEO_DIR` 以 `add_subdirectory` 集成；**当前依赖 rev `269a8c4`（本地 main 未 push，需保持最新，见「常用命令」段与审计 [11]）** |

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
- **子代理不吃会话风格**：caveman / 极简是**每会话**设置、子代理不继承，不写规则就吐长报告——长报告是主会话 token 的主要开销。派单 prompt 末尾固定加：「报告用极简风格——删铺垫 / 客套 / 复述 / 过程叙述，只留结论、证据（file:符号名 + 最短决定性输出）、sha、数字、待办；每条结论一行；总长 ≤ 原上限一半；技术精度不变，不许为省字含糊」。适合的场景改用 `cavecrew-*` agent（`cavecrew-investigator` 定位代码 / `cavecrew-builder` 一到两文件小改 / `cavecrew-reviewer` diff 审查），其输出本身即压缩形态。**边界**：写进仓库的文档 / commit message / memory 仍是正常文字（极简只针对给主会话的报告）。
- **意外终止先唤醒原 agent，不重派**：子任务崩溃 / 卡死被清理 / 静默失联（无 sha、无报告）时，用 `SendMessage` 唤醒**同一个** agent 从 transcript 续跑，写明"你上次停在哪、接着做别从头再来"；**不是**主会话接手，**不是**派新子任务重做（换新 agent 丢上下文 = 从零烧 token + 重跑全量编译）。有回单但结论存疑 = 走审计流程，不属本条。唤醒失败（agent 已被彻底回收）→ **升级给用户**定夺，不自行改派。派单时即记录 agent id（无人值守靠 watchdog 先探活、再唤醒）。

### 交付流水线（四律，按序叠加）

1. **只跑定向测试**：只编译受影响目标 + 只跑相关用例（`./build/tests/devdesk_tests.exe <功能子串>`，如 `markdown` / `proxy` / `settings`）。**禁全量重建、禁全量套件**（用户原话"进度太慢了"）；全量只在里程碑收尾 / 提交前由主会话跑一次。
2. **涉及编译 / 测试的子任务必须 worktree 隔离**（`isolation: "worktree"` 或手动 `git worktree add`），**主工作树禁跑构建**；worktree 首建用 `scripts/build-test-run.sh` 自动 configure（`-DEUI_NEO_DIR=D:/project/gui/EUI-NEO` + 既有 OPENSSL / ZLIB 参数）。
3. **完成即提交并合 master**：agent 在自己的分支原子 commit（**显式 pathspec，禁 `git add -A`**）并报 sha → 主会话审计（读 diff + 复跑验收）→ **串行**合并进 master（多 agent 同时 merge 会撞 `index.lock`）。**不攒批**。
4. **worktree 用完即清**：合并后立即 `git worktree remove <path>` + `git worktree prune`；有未跟踪文件先看内容、必要时先备份到仓库外再 `--force`。**禁止残留**（曾一次清出 12 个 EUI-NEO 残留 worktree）。

*为什么*：全量测试每轮 ~2 分钟且互相抢 build；主工作树脏 + 多 agent 同写导致"任务完成也没法干净提交"。**注意**：worktree 是从**提交**切出的 → **WIP 不落地，新 worktree 任务就起不来**（规则 3 是规则 2 的前提）。
**卡死多半是系统休眠，不是 agent 出问题**：本机休眠会让后台 agent 被判"600 s 无进展"而中断（环境事件、不是子任务失败）。唤醒机器后：① 看哪些 agent 断了 → `SendMessage` 唤醒续跑；② 空槽按"空槽即补"填满。用户输入【继续】= 唤醒 + 拉活，不用再问。

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
- **EUI-NEO 最低依赖 rev `269a8c4`**：`CMakeLists.txt` 的 `DEVDESK_EUI_NEO_MIN_REV`，需与本文件「常用命令」段的记录保持一致，消费 EUI 新 API 后两处同步更新；EUI-NEO 要跑在本地 main 最新（本地领先 origin、尚未 push）。
- **已踩过的坑（写码前先想一遍）**：
  - **单位错配（系统性）**：`windowSize` / `glfwCreateWindow` 是**客户区物理像素**，compose 侧按 `framebuffer/(dpiScale×uiScale)` 折算逻辑像素——菜单窗、启动窗、桌宠锚点都栽过。本机 125%：工作区 1920×1020 物理、任务栏 60 px，窗口客户区底部 52 物理 px 会被任务栏吃掉（贴底 UI 不可见）。
  - **三套坐标系**：窗口坐标（`onContextMenu` 给的是它）/ 页面坐标（`app.page` 子树，原点 x = 侧边栏宽 **208/72**）/ 控件局部坐标，跨层必须换算（`Router::pageOriginX` 为此而加）。
  - **上游方言 `upstream_dialect`**：UA 不能透传客户端（Cloudflare 1010 按 UA 封）、`opencode.ai/zen/go` 需 `x-opencode-session`、模型名带 `[1M]` 要剥。
  - **自定义 check 必须 raise**（只 print 的"全绿"是假象）。
  - **负载敏感 flake**：`md_preview_highlight` / `md_highlight` 的 `<8ms` 墙钟断言、`test_claude_ui` 的固定端口 28888。
- **`~/.claude/settings.json` 归属（2026-09-27 定）**：**DevDesk 是 settings.json 接管功能的终局 owner**，cc-switch（本地代理 15721 + `sessionAutoSyncEnabled` 反向同步）只在过渡期并存，**不为它做任何兼容 / 协调处理**（用户原话"不需要管 cc-switch，本软件开发完善后就不使用了"）。并存期两边都会整体重写 settings.json；cc-switch 库里存有 DevDesk 代理时代的旧快照（`ANTHROPIC_BASE_URL=http://127.0.0.1:8888` + 老 token），切 provider 时可能吐回 → 用户感知为"配置一直被还原"。**识别写者看键名**：DevDesk 写 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED`，cc-switch 写 `ANTHROPIC_API_KEY=PROXY_MANAGED`。DevDesk 三个写入时机（`claude_service.cpp`）：起代理 takeOver / 运行中切 profile 再同步 / 停代理或退出按键还原；启动只在有残留 sidecar 时补还原（`main.cpp` 启动序列）。再遇"配置被还原"类反馈：先按键名判写者，是 cc-switch 写的一句话说明即止——**不派任务、不做兼容**。

### 反事故红线

- **禁批量清理"在飞"的 worktree**：清理前先 `ListAgents` 判活，在飞的绝不碰。判定式必须含"**有提交**"这条：`tip != base && git merge-base --is-ancestor <branch> HEAD`（base 取该 worktree 创建时的 master tip，可从分支 reflog 的 `branch: Created from` 读）。批量删除**逐条确认**，别一条命令梭哈——`git worktree remove` 在文件被占用时会**半删**（`.git` 没了、目录还在），恢复成本更高。*事故*：只用 `--is-ancestor` 判定时，**在飞 agent 的分支 tip 就是基线提交（尚未提交）也为真** → 三条在干活的 worktree 连同未提交改动一起被删、另两条半残，三条线只能重做（成果默认只在工作区，"无提交"与"已合并"在分支指针上长得一样）。误删后：`ls` 看目录是否残留 → `git worktree repair`；`.git` 已丢则重建同路径同名分支的干净 worktree，再 `SendMessage` 唤醒原 agent 凭其报告重做（transcript 里的清单能省掉重新分析）。
- **跨仓库改动必须"产补丁 + 非隔离线应用"**：带 `isolation: "worktree"` 的 agent **无法对任何其它仓库执行 git 操作**（`cd <别的仓库> && git …` 与 `git -C <任意路径> …` 一律被隔离守卫拒）→ 建不了别的仓库的 worktree、也提交不了。正确形态：**隔离 agent 产 patch + 验证**（可用文件副本模拟依赖来真跑构建 / 测试）→ **另起一条"非隔离" agent**（不设 `isolation`）把 patch `git apply` 进目标仓库、跑编译、原子 commit、清理副本；非隔离线的 prompt 必须写死"只许改 `<目标仓库>`，禁止碰 DevDesk 文件"。反过来**别让非隔离 agent 碰 DevDesk**（它没有隔离保护，工作目录就是主树）。主会话**不要**替 agent 落文件（含 `git apply`）。*为什么*：DevDesk 与 EUI-NEO 是两个仓库（同级目录、非 submodule），上游能力改造天然跨仓库。
- **`git merge` 不与 Agent 派单放在同一条消息里并行跑**：主会话 git 操作与派单并行会因 git 锁竞争导致隔离校验失败（实测两条派单被拒、留下两个孤儿 worktree）。