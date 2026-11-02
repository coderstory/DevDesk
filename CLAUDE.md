# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

**DevDesk** 是面向开发者的跨平台桌面工具集（C++20 GUI 应用）：**Markdown 笔记本**（本地文件 + SQLite FTS5 全文搜索）、**Todo 看板**（SQLite 多列卡片）、**Claude 配置切换器**（多套 API profile + 本地 HTTP 代理带流式转发），另有**首页仪表盘**、**设置页**、**关于页**、**智能体页**（module_agent：任务 / 运行 / 工具）。

> **模块口径以代码为准**：`src/app_registry.h` 的 `kModuleRegistry` 现有 **14 行**，Router 实注册 13 页（上线组父项工厂返回 nullptr 不导航；枚举序见 `src/core/module_interface.h` 的 `PageId`）。可被模块开关插拔的有 9 个：笔记 / 待办 / 定时 / Claude / 智能体 / 记忆 / 壁纸（默认关）/ MCP / 上线；Home / Settings / About 的 `gatingKey` 是 `nullptr`，不可禁用。`src/` 下有 12 个 `module_*` 目录。

目标平台：**macOS**（arm64）和 **Windows 11**——两平台均为原生支持。CI 双平台构建 + 测试守卫。


## 🧭 智能中枢定位（2026-10-21 定，贯穿所有开发会话的设计基准）

**DevDesk 的战略方向是智能中枢**：形态 = Claude Code / OpenCode 已验证的「智能体 + harness」骨架（工具循环、触发调度、权限闸、审计、技能扩展），**特化在三条轴上——智能体相关设计取舍以这三轴为准**：

1. **无人值守**：触发驱动（定时 / 事件 / 自唤醒）+ 失败重试 + 验收自评 + 失败升级通知；可靠性优先于交互性——对话窗、流式响应、交互式 REPL 刻意不做（路线图「明确不做」清单）。
2. **C++ 原生桌面常驻**：托盘、可视化审计、桌面感知（剪贴板 / 窗口 / 文件状态）。
3. **业务绑定**：自有模块工具面 + 公司内部 GitLab / 敏捷平台对接。

**路线方针**：每个取舍先问「前人有没有已验证的答案」——有就照搬机制模式（skills、权限闸、角色体系、质量闸分层）；没有答案或不落在三轴上才自研。流水线终点永远是 MR 留人审（ADR 0001）。
**轻资产原则**：核心只保留「机制与接口」（智能体循环、工具注册、事件总线、触发调度、权限闸、审计、预算护栏），业务能力一律可插拔接入。接口质量硬判据 = **新能力以「注册 / 放文件」接入的步骤数**（工具 4 步 / 事件 2 步，超出即接口退化——先修接口、不加能力）。协议可插拔（`core::LlmEndpoint` / `ToolRegistry` 即接口缝）。**插件状态必须单一注册源**（反例 dsh 散落五处）——插件面设计红线。
**参考源码**：`../opencode2/`、`../smolagents/`、`../anthropic-skills/`；吸收成果沉淀在 `docs/strategy/07-OpenCode源码吸收报告.md`。
**权威底稿**：`docs/strategy/00-总览索引.md`——各期实现 spec 从路线图 05 派生，冲突裁决顺序 ADR > 路线图 > spec；术语见 `GLOSSARY.md`。活文档只留规则与防错理由，历史包袱归 git 历史（本文档亦受此约束）。

## 🚨 项目核心约束（最重要的几条）

1. **用户不熟悉 C++，纯 AI 开发** — 代码对用户不可见，反馈走：文档 / 中文日志 / 截屏 / 测试结果 / mockup；面向用户拍板的文档与报告**零黑话**（术语首次出现就地一句人话解释，判据 = 不查资料一遍读懂）
2. **完整设计文档**：所有架构 / API / 设计 token 决策都在 `docs/superpowers/specs/2026-09-20-devdesk-design.md`，**实现前先读 spec**
3. **每个 P 阶段完成后必须**：跑测试 + 启动应用 + 截屏 + 更新文档 + git commit（spec §16.2）
4. **配色为绿色体系（现行，2026-11-02 用户拍板）**：旧橙「方案 B / Claude Clay `#D97757`」已废弃、列为禁止项；现行 token 以 `src/core/theme.h` 为准，详见「视觉规范」段
5. **平台适配代码必须接口隔离**：头文件定义平台无关接口，实现按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake 按平台选择编译（与 EUI-NEO 后端选择同一模式）。**禁止在业务代码里散布 `#ifdef _WIN32` / `__APPLE__`**。虚接口仅在需要运行期注入/mock 时对单个点引入，不全局铺开

## 🛠️ 常用命令

> **🔴 构建与测试一律走 `bash scripts/build-test-run.sh`，禁止手敲 `cmake -B …` / `cmake --build …`。** 手敲造出互相漂移的构建目录，且漏掉脚本封装的约定（EUI_NEO_DIR、ccache、OPENSSL、发布形态守卫）。默认 Debug（`build/`），`--release` 切 `build-release/`；非常规目录用 `DEVDESK_BUILD_DIR=<dir>`。**例外**：`scripts/` 内部实现与诊断 / 验证场景（差分验证、一次性探针）可直接用 cmake，但须写明"为什么脚本不够用"。下方手敲命令块只作**工具链参考**（换平台必须 `rm -rf build`），日常不要照着敲。

**发布形态**：**不强制单 exe**，**允许第三方开源依赖**；优先静态链接，只能动态链接的（DLL/dylib）由安装程序携带并保证加载路径（旧「单 exe 零外部 DLL」已废弃）。

**macOS / arm64（当前开发平台，AppleClang + Ninja）**：

```bash
export PATH="/opt/homebrew/bin:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build && cd build && ctest --output-on-failure
```

本机工具链：cmake 4.4.3、ninja 1.13.2、AppleClang、ccache 4.14.1（`/opt/homebrew/bin`）。ccache macOS 持久配置读 **`~/Library/Preferences/ccache/ccache.conf`**——**不是** XDG 的 `~/.config/ccache/`（写了完全无视）。用例总数以运行输出 `Total:` 为准。

**Windows 11（MSYS2 MinGW64 + Ninja，本机与 CI 同一套）**：

```bash
export PATH="/d/software/msys64/mingw64/bin:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build && cd build && ctest --output-on-failure
```

**一体化脚本（推荐）**：`bash scripts/build-test-run.sh` —— 增量构建 + 测试 + 杀旧进程 + 启动 app，默认全增量。参数：`--clean` 全量重建、`--no-test` 跳过测试、`--no-run` 不启动、`--filter X` 只跑名字含 X 的用例。工具链 PATH 与 EUI-NEO 定位（`EUI_NEO_DIR` → 仓内 `./EUI-NEO` submodule，worktree 内同样可见）脚本内部处理；`build/` 未初始化时自动 configure——无需手敲 `-D`。

**模块隔离静态守卫**：`bash scripts/check-module-isolation.sh` —— 断言 `src/module_agent/` 零兄弟模块头 include、todo/markdown/claude 工具与模块注册文件不回依赖 `module_agent`、`module_settings/CMakeLists.txt` 非注释行不 link 兄弟模块库。退出码 `0` 通过 / `1` 违规（打印 `文件:行:命中行`）/ `2` 环境错（**不静默绿**）；`--root DIR` 反证用。`build-test-run.sh` 测试段后自动串跑（`--no-test` 也跑）。

**编译缓存 ccache（已接进脚本）**：持久 conf 各平台已配（Windows `%LOCALAPPDATA%\ccache\ccache.conf` + `D:/software/ccache-devdesk/ccache.conf`；macOS `~/Library/Preferences/ccache/ccache.conf`），**不需要 `CCACHE_*` 环境变量**；手敲 cmake 只需带 `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`（脚本已自动带；未装自动降级）。要点：① **BASEDIR 必设**（conf 已配）——不设每棵 worktree 哈希不同 → 永远 miss；② launcher 是 **configure 期属性**，已有 `build/` 不自动改，要 `--clean` 或重跑带 launcher 的 configure；③ `scripts/ccache-env.sh` 只在临时覆盖缓存时 source。**并行度封顶** `DEVDESK_JOBS=3`，单机独占可调 8。
**"裸 `cmake -B build` 也能吃缓存"走不通**：OpenSSL/MinGW 根由 `cmake/Dependencies.cmake` 从编译器路径推导；masquerade / shim 顶替编译器路径 → MinGW 根反推错误 → 静态 curl 依赖链解析失败，已实证不采用。

**macOS 侧脚本（跨平台）**：`scripts/build-test-run.sh` / `ccache-env.sh` / `check-consistency.sh` 已支持 macOS/Linux（PATH 与 openssl 按 `DEVDESK_OS` 分支，Windows 行为不变）。`check-consistency.sh` 的 `screencapture` 需「屏幕录制」TCC（已授予），但 OpenGL 窗口可能抓旧帧；**agent 会话拿不到该 TCC 授权**（launchd 拉起）——出图用探针 `--screenshot`（见六阶段·阶段 4）。旧文档的 `./scripts/dev.sh` / `test.sh` / `screenshot.sh` / `reset-db.sh` **本仓库不存在**，别照抄。

构建入口：`CMakeLists.txt` 顶层 → `src/main.cpp`。
EUI-NEO 依赖：`EUI-NEO/`（**仓内 git submodule**，`.gitmodules` 指 coderstory fork；`EUI_NEO_DIR` 默认仓内路径、`add_subdirectory` 集成，可覆盖）。消费政策（**ADR 0006**）：版本基线由 gitlink 记录、推进 = 升 submodule 指针，无最低 rev、configure 期零校验；版本匹配唯一权威判据 = `scripts/check-eui-neo-version.sh` 的**内容级 API 守卫**（全部消费编译单元 `-fsyntax-only`）。EUI-NEO 内 `origin` = 上游 sudoevolve，**永不 push**；用了新 API 先推 fork、再升 submodule 指针推 DevDesk。

## 🏗️ 架构要点（4 层模块化）

```
main → 13 个 Router 页面（枚举见「项目概况」）
            ↓
       core (Result / paths / sqlite_db / logging / theme / router)
            ↓
       platform (http_client / http_server / keychain / credential_protection / json)
            ↓
         EUI-NEO
```

**关键依赖规则**（违反会循环依赖）：
- `core` ❌ 不能依赖 `platform`；❌ 不可调用 EUI-NEO 运行时行为（仅 **header-only 类型桥接**例外——eui::Color 值类型 / Ui/Screen 签名透传 / component_theme.h 纯映射，边界见 spec §4.2.1）
- `module_*` 之间 ❌ 不可互相依赖
- `platform` ✅ 可依赖 `core`（用 `Result`）；`module_*` ✅ 可依赖 `core` 和 `platform`

**注释纪律（硬规则，写代码时同步遵守）**——唯一读者是 AI，它会自信地"改进"冗余代码。**唯一判据**：这句话帮 AI 少犯一条错，还是浪费注意力？少犯错就留，浪费就删。总纲：**契约与不变量优先，叙事历史进 git/docs**（演变叙事查 `git log`，决策背景进 `docs/`；**存量注释不回退**）。
- **必留 · 反重构警告（最高优先级）**：「看起来该清理」但不能动的，必须写明为什么（顺序不可换／刻意不用某 API／看似冗余但必需／缺省值陷阱）。例：`// canChooseFiles = false 不能省：省了用户能选到单个 .mp4，扫描器枚举后返回空`
- **必留 · 契约**：参数单位 / 返回语义含三态与哨兵值 / 前置条件、并发与线程约束、错误码语义、非显然的坑（只陈述规则）
- **必留 · 跨文件不变量**：单文件推不出的规则。例：「`core` 不能依赖 `platform`」「四个 Watcher 都必须强持有，否则回调永久泄漏」
- **必删**：复述代码行为／复述命名／演变叙事／出处与证据（`spec §`、日期、commit hash）／需求原话引用／验证体系自解说／设计推演／告警符号（⚠️🔴）／装饰分隔线
- **篇幅不设上限**，信息密度才是判据；默认一行写完，折行只在句子边界。现状限制可留一句话

**测试纪律（硬规则，写测试时同步遵守）**：权威全文见 `docs/TESTING-DISCIPLINE.md`。核心：**禁止形式主义/为覆盖率写测试，每条用例必须防「特定功能×特定场景×特定边界」的回归**；不看行覆盖率，看功能×场景×边界覆盖矩阵（未钉格子=明账欠债，显式记录）。新增用例过三问（防什么功能回归/什么场景/卡哪条边界），答不出不写；会红性铁律（删被测代码必红）。

## 🔧 EUI-NEO 上游贡献政策（重要）

**遇到 EUI-NEO 的 ❌ 限制时**，优先考虑**直接修复 EUI-NEO 仓库**（`EUI-NEO/`，仓内 submodule）并提交 PR，而不是 DevDesk 内自造 workaround。

### 工作流

遇到问题 → 判断是否通用（拖拽 / 文件拖入 / macOS 27 / 字体 / IME / DPI 等），通用 = 上游应修 → submodule 内写 patch、编译 DevDesk 验证 → 推 fork 或发起到上游 → 移除 hack，复用新能力。

- **适合修上游**：通用 UI 能力缺失（drop target / file drop / tree）、macOS 27+ 适配（Liquid Glass / Metal / OpenGL fallback）、跨平台一致性（IME / 字体 fallback / DPI）、API 设计改进、性能 / 编译时间 / 二进制大小。
- **DevDesk 自己解决**：业务逻辑（Markdown 解析、Todo CRUD、Claude 切换）、应用级拖拽（卡片跨列是业务，EUI-NEO 给基础 onDrag 就够）、设计 token / 配色、第三方集成（Keychain / SQLite / libcurl）。
- **PR 准备**：`examples/` 或 `apps/` 加 demo；不破坏现有 API；更新 `docs/`；过 EUI-NEO 自身编译；commit message 写明需求来源（"DevDesk 需要 X 功能"）；大改动先开 issue。仓库约定：主分支 `main`，前缀 `feat:` `fix:` `refactor:` `docs:`。

**详见**：[[docs/EUI-NEO-CAPABILITIES.md]] 各 ❌ 项的"应对"——优先"提交 PR"而非"自造 hack"。

---

## ⚠️ EUI-NEO 真实能力盘点

实现时**只使用以下能力**，不要假设有未列出的 API。完整清单以 [`docs/EUI-NEO-CAPABILITIES.md`](docs/EUI-NEO-CAPABILITIES.md) 为准（遇 UI 问题先查它 → grep `EUI-NEO/components/`、`core/dsl.h` → 看 `examples/`、`apps/`；还不清楚就写"待验证"，不要假设）：

| ✅ 支持 | ❌ 不支持（自行实现或放弃） |
|---------|------------------------------|
| button / input（多行+IME+选区）/ checkbox / switch / dropdown / datepicker / slider / progress / tabs / navbar / sidebar / dialog模态 / toast / contextMenu / tooltip / dataTable / charts / markdown / virtualList / scrollView | **OS 文件拖入 / drop target**——自实现 DragController（无 onFileDrop） |
| 任意 hex 颜色 + 透明度 + 圆角 + 阴影 + 渐变（仅2色+水平/垂直）+ transform 2D/3D | **tree 组件**——自实现 MarkdownTreeView |
| FreeType 字体 + 系统字体 fallback + HiDPI 自动；FontAwesome + Unicode Emoji + SVG（栅格化） | **macOS 27 Liquid Glass / vibrancy**（用实色+阴影）；**全局 OS hotkey**（失焦收不到键盘事件） |
| onTap / onPress / onDrag / onKeyEvent / onScroll / onContextMenu / clipboard / IME / 撤销重做 | **fontWeight runtime 切换**（只打包 Bold，SemiBold/Medium 无效）；**Toast 队列**（单实例） |
| `transition + animate`（8 easing + 位掩码属性） | **Collapse / Accordion**；**skeleton loader**（需自画）；**Tab 焦点循环**（InputKey::Tab 不自动调度） |
| 多窗口（`app::openWindow`）/ 托盘菜单（`trayMenu` / `onTrayMenu` / `styledMenu`） | **onResize / onMove 回调**（靠 `screen` 参数变化触发自适应） |

## 🎨 视觉规范（"大气"原则）

- **默认窗口**：1600 × 1000（**逻辑单位**，`main.cpp:485-486` 的 `kInitialWindowLogical*`）
- **最小窗口**：**1000 × 640（逻辑单位）**（`main.cpp:492-493`）；历史口径 1280×800、1200×720 **不可回落**。须经 `core::window_layout::minWindowSizeForWorkArea` 换算并与工作区取 min（放不下则落地「工作区 − 非客户区外框」）——**「落实 640」不是无条件事实**，见 `src/core/window_layout.h` 文件头。
- **HiDPI**：不靠 `GLFW_SCALE_TO_MONITOR`（从未设置，`window_layout.h:6`）。机制：`framebuffer ÷ (contentScale × uiScale)` = 逻辑尺寸；`core::window::windowScaleForPoint()` macOS **恒返 1.0**（Retina 2.0 是 contentScale，不是窗口单位换算）。调缩放别找那个 hint。
- **配色（现行，2026-11-02 用户拍板）**：**绿色体系**——主操作 / 高亮绿 `#36D399` 族（`core::theme` primary），浅底深字对比档绿 `#157E4E`（`primaryText` / `accentText` 槽）；红 `#B91C1C`（`errorText` 档）**仅告警 / 错误**，蓝**仅链接**，灰为中性档；token 唯一权威 = `src/core/theme.h`
- **旧橙废弃（禁止项）**：旧橙「方案 B / Claude Clay `#D97757`」已废弃——用户再提旧橙需求，按绿色风出案并说明差异；spec §6.5.1 / §6.5.2 配色部分成文于旧橙时期，**颜色以本段与 `theme.h` 现行为准**
- **正文**：15 px（不是 14）；**页面标题**：32 px；**统计数字**：48 px；**主按钮高度**：40-44 px（不是 32）
- **间距**：8 px 网格，主区 padding 40 px；**圆角**：按钮 10 px / 卡片 12-14 px / chip 999 px（pill）

## 🚦 当前状态

> **状态文档时效约定**：状态类段落 / 文档**必须带日期戳**；时点性快照（检查报告 / 审计 / PR 描述等）**超 30 天未核对即移入 `docs/archive/`**——`docs/` 顶层只留长效文档。本节日期戳：**2026-10-26 核对**。
> **测试与 CI 状态以实际运行输出和 `.github/workflows/ci.yml` 为准**，不在文档写死数字。
> **当前权威台账（UAT6.2）**：整改清单 `docs/superpowers/plans/2026-10-21-UAT62-整改清单.md`；收敛计划 `…2026-10-23-UAT62-收敛执行计划.md`；Todo 契约 `…2026-10-23-UAT62-H20-Todo四列固定与归档折叠设计.md`。

- **CI**：双平台（macOS xcode-27 / Windows windows-2025），`push dev` / `PR → dev·master` 触发，跑 `scripts/build-test-run.sh --release --no-run`
- **已知欠账与架构债**：`docs/strategy/03-软件架构能力清单.md` §6；**近期交付**见 `git log`
- **跨仓库耦合**：DevDesk 硬依赖 EUI-NEO 最新源码（不锁版本，ADR 0006）；新 API 先推 fork 再推 DevDesk；`scripts/check-eui-neo-version.sh` 守卫把关

## 📦 安装程序（2026-09-28）

- **Windows**：Inno Setup `installer/windows/devdesk.iss` —— 用户级安装（`%LOCALAPPDATA%\Programs\DevDesk`，`PrivilegesRequired=lowest`）；assets 递归拷入，`.exe` 单独一条（loose 非 embed）；排除 17MB `NotoSansSC-VF.ttf`（变量字体 FreeType 不稳、未启用，运行时走 EUI-NEO fallback）
- **打包脚本**：`scripts/package-windows.sh`（Release + ISCC 三段式探测 + `DevDesk-<ver>-setup.exe`）；macOS：`installer/macos/`（DMG 背景 + 去隔离 `.command`）+ `scripts/package-macos.sh`
- spec：`docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md`

## 📂 关键文件

| 文件 | 作用 |
|------|------|
| `docs/superpowers/specs/2026-09-20-devdesk-design.md` | **唯一权威 spec**，所有架构 / API / token 决策都在这 |
| `docs/mockups/palette-comparison.html` | 配色对比 demo（历史存档，方案 B 已废弃）|
| `docs/mockups/devdesk-prototype.html` | HTML 交互原型（待生成）|
| `EUI-NEO/` | 上游 GUI 框架（**仓内 submodule**，`EUI_NEO_DIR` + `add_subdirectory` 集成）；消费政策见 ADR 0006 与「常用命令」段；submodule url = coderstory fork（CI 经 checkout submodules 检出） |
| `docs/superpowers/specs/2026-09-27-*.md` | 09-27 批次 specs：wave1-fix / god-class-refactor / shared-config-and-failover |
| `docs/superpowers/specs/2026-09-28-*.md` | 09-28 批次 specs：installer-and-dmg / devdesk-probes / r8-assembly-layer / tin06-test-scaffold-migration / setscrolloffset-upstream-m6-refactor / md01-reload-block-mapping / macos-three-features / ui-modernization / profile-run |

## 🧪 验证原则

- **未运行测试 = 未完成；未截屏 = 未完成；未读 spec = 未开始；未 commit = 未持久化**
- **不要假设 EUI-NEO 能力** — 不确定就去 `EUI-NEO/` 源码 grep
- **按场景写测试**：用例对应具体业务场景、注明防的事故（改动回退时会红）；「覆盖 XX 函数」不是测试目的
- **智能体类测试两段制**：先 mock（假端点、假工具）验证理想路径与分支，不发真实网络——但 **mock 只证理想情况，收口必须真模型实测**（草稿 / 任务运行 / 工具循环每类至少一次真实端到端；真网关方言、token 计费、超时行为只有实操才暴露）
- **业务场景必须实际跑起来**：验证金字塔 = 纯逻辑单测 < 无头几何 < **真窗口 probe（真点击 / 键盘 / 拖拽 / 渲染）** < 真网 live < 真机 UAT；交付主流程至少到 probe 层、涉网关到 live 层
- **测试绿 ≠ 可用，数字不报给用户**：交付时报三行——跑了哪些场景 / 结果 / 哪些没跑（明写"未验证"）

---

## 🤝 AI 协作与交付纪律（团队通用）

### 派单与并发

- **主会话零分析/开发（硬标准）**：错误定位、方案设计、代码分析、修复实现、文档撰写一律**不进主会话**，全部派子任务；主会话只留：需求裁决、拆分派单、构建 / 测试产物验收、只读核查、合并与提交决策、环境准备。子任务一律用 **devflow 插件七角色**：`developer` / `qa-tester` / `researcher` / `bug-fixer` / `code-reviewer` / `product-manager` / `ui-ux-designer`，按工种对号入座；不得用其它类型创建开发 / 分析子任务；实现者不得自审。旧「六类 devdesk-* 子智能体」口径已作废。
- **主会话禁改文件**：源码 / 测试 / 配置 / 文档一律派子任务改；主会话只做裁决、拆分、派单、只读核查、产物审计、合并与提交决策。**边界**：`commit` / `merge` 写 `.git` 记账不算改文件，主会话可直接执行；构建 / 测试 / 只读 shell 同理（既写又审会失去第二方视角）。
- **粒度看复杂度，不看文件数**：引用点已查清的机械改动（3-6 文件也算）合并 1 个子任务；需独立探索 / 设计判断 / 影响面广 / 第二视角才拆并行。合起来 = **"少派、但必须派"**（"主会话 inline 做"已作废）。
- **并发上限 8 个在飞子任务**。约束：① **同时只允许 1 个执行编译/测试**（worktree 构建互抢 IO/ccache，派构建前先确认构建槽空闲）；② 同一文件同时只有一个写者（见下条）；③ 多 worktree 构建互拖时主动收束（串行或降 `DEVDESK_JOBS`）。
- **同一文件同一时刻只有一个写者**：派单前列在飞白名单做交叠检查。
- **禁止长 sleep（上限 60s）**：命令不许 `sleep > 60`；派单 prompt 禁长 sleep 等待（构建超时用 `timeout` 包，异步用有界轮询）。
- **Bash 超时上限 3 分钟**：长构建 / 打包走后台（`run_in_background`）。只有工作**天生就慢**（冷编译首次 configure、--clean、大包打包）才上调，且须写明「为什么慢是正常的、不是卡死」——超长 timeout 掩盖卡死。
- **子代理不吃会话风格**：极简是**每会话**设置、子代理不继承。派单 prompt 末尾固定加：「报告用极简风格——删铺垫 / 客套 / 复述 / 过程叙述，只留结论、证据（file:符号名 + 最短决定性输出）、sha、数字、待办；每条结论一行；总长 ≤ 原上限一半；技术精度不变」。**边界**：写进仓库的文档 / commit message 仍是正常文字。
- **意外终止先唤醒原 agent，不重派**：崩溃 / 静默失联时 `SendMessage` 唤醒**同一个** agent 从 transcript 续跑（写明"上次停在哪"）；换新 agent = 从零烧 token。唤醒失败 → **升级给用户**。派单时即记录 agent id。
- **项目级 hooks 已内置**：`.claude/settings.json` 四条——①基线文件（docs/strategy、docs/adr、docs/superpowers、GLOSSARY.md、CLAUDE.md）改后跑累积核对清单，丢历史标记即拦截（`.claude/hooks/strategy-guard.sh`，唯一可阻塞）；②merge/pull/rebase 后「分支绿≠合并绿」提醒；③commit 前「先红后绿」提醒；④改 `src/` 前「plan 先行」提醒。提醒不阻塞、守卫才拦截；改 hook 须子任务 + 管道实测。

### 交付流水线（四律，按序叠加）

> 四律本体（只跑定向测试禁全量 / 编译测试子任务 worktree 隔离、主工作树禁跑构建 / 完成即原子提交（显式 pathspec 禁 `git add -A`）并合并不攒批 / worktree 用完即清禁止残留）**权威在 devflow 插件 `ask` 收口节**。本项目特有事实：
- **集成分支是 `dev` 不是 `master`**：`master` 只是发布分支（`.github/workflows/release.yml` 只在 push master 出制品），本机无本地 `master`（引用写 `origin/master`）。**分支绿 ≠ 合并绿**：merge 后必须对合并后的 dev 树跑一次完整门禁——worktree 审计闸不覆盖合并结果。
- 合 `dev` 由主会话**串行**执行（并发 merge 撞 `index.lock`）；worktree 首建用 `build-test-run.sh` 自动 configure（自动定位 EUI-NEO 与平台参数，无需手敲 `-D`）；worktree 从**提交**切出 → WIP 不落地，新任务起不来。
- **卡死多半是系统休眠**：休眠让后台 agent 被判"600 s 无进展"中断。唤醒机器后：① `SendMessage` 唤醒断掉的 agent；② 空槽即补。用户输入【继续】= 唤醒 + 拉活，不用再问。
### 功能开发生命周期（六阶段）

> 实现 → 独立审计 → 按审计修 → 测试 → 合并发布 → 真机验收的**流转权威在 devflow 插件**（`ask` 定级链与 `developer` / `qa-tester` / `code-reviewer` / `audit` 角色 skill）；「需求产物与技术方案卡各过一次独立审计」见 AGENTS.md §十。判据不满足就停在原地。本项目特有事实：
- **审计者不得是实现者**（主会话合并前核查不能替代）；结构化 findings（每条 = `file:line` + 问题 + 影响 + 建议）落盘 `docs/superpowers/plans/<日期>-<主题>-audit.md`，逐维度给结论（"未发现问题"也写明"已核"）。
- **凡涉及可见 UI，必须有 probe 或等价几何断言——光有单测不算**（`ctest -L probe`，脚本不跑探针；用法 / 坑位 / 参考探针见 `.zcode/cheatsheets/ui-probe-harness.md`）。**探针勿单独 `#include <GLFW/glfw3.h>`**——harness 已统一给 GLFW + glad，混用撞 `OpenGL header already included` #error。
- **实现者交付前必须自己启动 app 看效果**（断言证明"几何对"，证明不了"看起来对"）：① 独立 `DEVDESK_DATA_ROOT` 启动自己 worktree 产物，**不得用用户真实数据根、不得碰 `/Applications/DevDesk.app`**（它是交付物不是测试对象，唯一写操作 = 阶段 5 整包覆盖安装）；② 截图走探针 `--screenshot`（agent 拿不到 `screencapture` TCC 授权），图为 framebuffer 像素非逻辑值；③ 点击用 `cliclick`（坐标 = 物理像素 ÷ 2；AppleScript `click at` 无效）；④ **严禁 `pkill -f DevDesk`**（多 agent 与用户实例同跑），只按自己的 PID 杀。
- 覆盖率只认 `bash scripts/coverage.sh` 全流程输出（`DEVDESK_COVERAGE` 默认 OFF；目标 spec §12.3：工具 ≥90% / Service ≥80% / Platform ≥70%）；修复必须有"先红后绿"证据，恒真断言重写、不许删断言了事；涉及共享层（`core` / `platform` / 公共头）的改动跑全量单测。
- 合并 = 审计 + 测试过后主会话**串行**合入 `dev`（并发 merge 撞 `index.lock`）并重建 + 覆盖安装 `/Applications/DevDesk.app`（用户标准：编译 = 编译 + 复制）；**没合并 = 没交付；修复合并 ≠ 交付**——新包放用户桌面 + 用户真机 UAT 通过才算完。
- **交付前三问**（发包 / 宣布完成前必答，答不上写进"未验证"栏、不许沉默）：① 存量数据升级路径验过吗（老库用例 / 真实库副本实测）② 真实外网 / 网关验过吗（方言断言 + live）③ 目标平台真机验过吗（probe 真窗口 + 用户 UAT）。
### 交付物与测试环境隔离

- **`/Applications/DevDesk.app` 是交付物，不是测试对象**：不得为验证 / 调试改 bundle 内容（塞文件、改 Info.plist、覆盖内层二进制）；唯一允许的写操作是阶段 5 整包覆盖安装。测试一律独立 `DEVDESK_DATA_ROOT`（优先级 `DEVDESK_CONFIG_PATH` > `DEVDESK_DATA_ROOT` > 平台默认根，见 `src/core/paths.h`）+ 独立构建目录。

### 审计与决策口径

> 审计方法论权威在 devflow 插件（`audit` / `code-reviewer` skill；主线 = 架构 / 设计 / 可维护性，语言级缺陷次要，findings 加 `lens` 字段）。本项目特有事实：
- **调研 / 分析成果归档与索引**：落盘文档必须登记进 [`docs/RESEARCH-INDEX.md`](docs/RESEARCH-INDEX.md)（单一登记源：日期 | 类型 | 主题 | 一句话结论 | 路径）；调研派单前先查该索引（查重先行）。`docs/strategy/` 级报告由 `00-总览索引.md` 管，两册分工不重复登记。
- **全量代码审计必须分片**：按片（core+platform / claude_proxy / markdown / todo+home+settings / pet+app 层+构建 / tests+基建）各派**只读**子任务产结构化 JSON，最后合并 + 主会话抽验。
- findings 口径：`impact` 写**维护成本 / 演进代价**，severity 按"多难维护 / 多难演进"定，不按"会不会崩"；去重对照 `docs/superpowers/plans/2026-09-27-expert-audit.md` 标 `already_registered`，只修确认项。
- **先找两全解，再谈取舍**：先穷举能同时满足的落点（换位置 / 换层次 / 换时机 / 拆两步）；确实互斥才升级给用户并写明"为什么无法两全"，**禁止**把可两全的事摆成非此即彼。动了既有守卫 / 测试按范围纪律显式授权白名单。
### 环境与工具链

- **构建 / 测试**：脚本内部处理工具链 PATH（Windows cmake 4.4.4 / ninja 1.13.2 / `D:/software/msys64/mingw64`）；测试二进制 macOS `build/tests/devdesk_tests`、Windows `…devdesk_tests.exe`，均支持子串过滤。测试框架是**自制迷你 doctest**（`REGISTER_TEST` + fail-fast，**不是**真 doctest）——grep `TEST_CASE` 找不到用例属正常。
- **codegraph 索引排除产物**：`codegraph.json`（仓库根）`exclude` = `**/build/**`、`**/build-*/**`、`Testing/**`、`wt-*/**`（新增临时目录同步维护）。
- **`DevDesk.exe` 会锁 build 目录**：链接报 `Permission denied` → `taskkill -F -IM DevDesk.exe` 解锁（**别杀 `claude.exe`**）。
- **代理直测脚本**：`doc/scripts/proxy_smoke_test.py`（`proxy` / `upstream` / `all`；环境变量 `DEVDESK_PROXY` / `DEVDESK_MODEL` / `DEVDESK_TOKEN`）。
- **EUI-NEO 版本匹配**：判据 = `bash scripts/check-eui-neo-version.sh --root . --eui-dir EUI-NEO` 绿（见「常用命令」段）。
- **已踩过的坑**：
  - **单位错配（系统性，两端口径不同）**：Windows：`windowSize` / `glfwCreateWindow` 是**客户区物理像素**，compose 侧按 `framebuffer/(dpiScale×uiScale)` 折算（本机 125%：任务栏吃掉客户区底部 52 物理 px，贴底 UI 不可见）；macOS：同组 API 单位是**点**，`windowScaleForPoint` **恒返 1.0**（Retina 2.0 是 contentScale，乘上去双倍放大），`screen.height` = 客户区点高（工作区 1470×833 点、标题栏 28 点 → 客户区 805 点）。最小窗必须经 `minWindowSizeForWorkArea` 做**同一条换算**再与工作区取 min（只做一步 = 把病换平台复制）。非客户区外框：macOS `{0,28}`、Windows `{14.4,37.6}@96dpi`（`window_layout_macos.cpp` / `_win.cpp`）。
  - **三套坐标系**：窗口坐标（`onContextMenu` 给的）/ 页面坐标（`app.page` 子树，原点 x = 侧边栏宽 **208/72**）/ 控件局部坐标，跨层必须换算（`Router::pageOriginX`）。
  - **上游方言 `upstream_dialect`**：UA 不能透传客户端（Cloudflare 1010 按 UA 封）、`opencode.ai/zen/go` 需 `x-opencode-session`、模型名带 `[1M]` 要剥。
  - **自定义 check 必须 raise**（只 print 的"全绿"是假象）。
  - **负载敏感 flake**：`md_preview_highlight` / `md_highlight` 的 `<8ms` 墙钟断言、`test_claude_ui` 固定端口 28888。
- **`~/.claude/settings.json` 归属**：**DevDesk 是接管功能的终局 owner**，独占写者，**不为第三方工具做兼容 / 协调**。写者识别看键名：DevDesk 写 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED`（`ANTHROPIC_API_KEY=PROXY_MANAGED` 出自别的工具）。写入时机（`claude_service.cpp`）：起代理 takeOver / 切 profile 再同步 / 停代理或退出按键还原；启动只在有残留 sidecar 时补还原。遇"配置被还原"反馈：先查还有没有第三方工具在写——**不做兼容**。

### 反事故红线

- **禁批量清理"在飞"的 worktree**：清理前 `ListAgents` 判活，在飞的绝不碰。判定式必须含"**有提交**"：`tip != base && git merge-base --is-ancestor <branch> HEAD`（base = 创建时**集成分支（`dev`）** tip，从 reflog `branch: Created from` 读）——只用 `--is-ancestor` 时，在飞 agent 的分支 tip 尚等于基线提交也为真，误判"已合并"。逐条确认；`git worktree remove` 文件被占用时会**半删**（`.git` 没了、目录还在）。误删后：`git worktree repair`；`.git` 已丢则重建同路径同名分支干净 worktree，`SendMessage` 唤醒原 agent 凭报告重做。
- **跨仓库改动必须"产补丁 + 非隔离线应用"**：`isolation: "worktree"` 的 agent **无法对其它仓库执行 git 操作**（隔离守卫拒 `cd <别的仓库> && git …` 与 `git -C …`）。正确形态：隔离 agent 产 patch + 验证（文件副本模拟依赖真跑构建）→ **非隔离 agent** `git apply` 进目标仓库、编译、原子 commit；非隔离 prompt 写死"只许改 `<目标仓库>`"，也**别让非隔离 agent 碰 DevDesk**。主会话**不要**替 agent 落文件（含 `git apply`）。
- **`git merge` 不与派单同一条消息并行跑**：git 锁竞争导致隔离校验失败（实测留下孤儿 worktree）。

## Agent skills

### Issue tracker

Issues 与待办统一走 `docs/pool/` 池库（pool MCP 工具，见 devflow 插件 `pool-usage` skill；`.agents/skills/` 仅保留项目特有 `devdesk-discipline`）。角色词汇以 devflow 插件七角色为准。已退役机制（历史备注）：TODO.md、`.scratch/` 本地 markdown 机制、`docs/agents/` 英文模板目录均已于 2026-10-31 清理。