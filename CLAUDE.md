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
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

**macOS（历史平台，脚本保留）**：`./scripts/dev.sh`（构建+运行）、`./scripts/test.sh`（测试）、`./scripts/screenshot.sh <page>`（截屏）、`./scripts/reset-db.sh`（重置数据库）。注意 `check-consistency.sh` 用了 macOS `screencapture`/`open -a`，Windows 上不可用。

构建入口：`CMakeLists.txt` 顶层 → `src/main.cpp`。
EUI-NEO 依赖：`../EUI-NEO/`（同级目录，通过 `EUI_NEO_DIR` cache 变量 `add_subdirectory` 集成，**不是 git submodule**）。**当前依赖 rev**：`d5e302c`（2026-09-24，EUI-NEO 本地 main；这批提交尚未 push 到 origin——origin/main 停在 v0.6.0，push 与否是用户决策门控）。**EUI-NEO 需保持本地 main 最新**（含 onWindowDestroyed/requestFocus/trayMenu/windowEffect/lineStyles/getMonitorWorkAreas/setSystemPowerHandler/styledMenu/onTrayMenu/windowIsFocused/input onCancel 等新 API），顶层 CMake configure 时打印 `EUI-NEO git rev` 并在缺失最低依赖 rev（`DEVDESK_EUI_NEO_MIN_REV`，与本行保持一致、消费 EUI 新 API 后两处同步更新）时软警告不 fail；EUI-NEO push 后将启用硬版本检查（审计 [11]）。

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
| `check-consistency.sh` 用 macOS `screencapture`/`open -a` | `scripts/` | 🟢 仍开放：开发工具，非运行时 |
| 旧 `build/` 是 Darwin arm64 产物 | `build/` | ✅ W1 已删除并在 Windows 重建 |

已正确处理的跨平台点（T6 接口隔离后）：**keychain / paths / time / file_open 四族平台 API 全部按平台拆文件**（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`，CMake `target_sources` 按平台选择编译，与 EUI-NEO 后端选择同一模式），业务代码零 `#ifdef _WIN32` / `__APPLE__`（2026-09-22 W2 验收 grep `src/` *.cpp/*.h 零命中）；HTTP 层（libcurl/cpp-httplib）本身跨平台；`.app` target（`if(APPLE)` 守卫）、Security.framework 链接（有守卫）。time 统一走 `core::time::localTime`（`src/core/time.h` + `time_win/posix.cpp`）。

**测试框架注意**：`tests/doctest.h` 是自制 78 行迷你框架（`REGISTER_TEST` 宏静态自注册 + `DOCTEST_CHECK/REQUIRE` 断言，fail-fast），**不是真 doctest**——grep `TEST_CASE` 找不到用例、`--list-test-cases` 之类参数会被当作过滤子串属正常。共 **330 个用例**（2026-09-23 master @ 296b9c8 实测全绿：291 终验基线 + pet-skins/anime-girl 波 +13 + md-highlight +7 + theme-hierarchy +4 + pet-fix +1 + audit-batch1 +13 + winfx-pet +1；soak 300s 由 `DEVDESK_SOAK=1` 门控、常规套件打印 `[SKIP]` 直接通过）。test_main 支持子串过滤：`./devdesk_tests.exe stress` 只跑名字含 "stress" 的用例，`--log` 打开 Debug 级控制台日志。

## 📂 关键文件

| 文件 | 作用 |
|------|------|
| `docs/superpowers/specs/2026-09-20-devdesk-design.md` | **唯一权威 spec**，所有架构 / API / token 决策都在这 |
| `docs/mockups/palette-comparison.html` | 配色对比 demo（已确定方案 B）|
| `docs/mockups/devdesk-prototype.html` | HTML 交互原型（待生成）|
| `../EUI-NEO/` | 上游 GUI 框架（同级目录源码，非 submodule），通过 `EUI_NEO_DIR` 以 `add_subdirectory` 集成；**当前依赖 rev `d5e302c`（本地 main 未 push，需保持最新，见「常用命令」段与审计 [11]）** |

## 🧪 验证原则

- **未运行测试 = 未完成**
- **未截屏 = 未完成**
- **未读 spec = 未开始**
- **未 commit = 未持久化**
- **不要假设 EUI-NEO 能力** — 不确定时去 `../EUI-NEO/` 源码 grep