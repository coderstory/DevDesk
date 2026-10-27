# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

**DevDesk** 是面向开发者的跨平台桌面工具集（C++20 GUI 应用），包含三大工具：

- **Markdown 笔记本**：本地文件存储 + SQLite FTS5 全文搜索
- **Todo 看板**：SQLite 存储，多列卡片管理
- **Claude 配置切换器**：多套 API profile 管理 + 本地 HTTP 代理（带流式转发）

另有**首页仪表盘**、**设置页**、**关于页**、**智能体页**（module_agent：任务 / 运行 / 工具）。

> **模块口径以代码为准**：`src/app_registry.h` 的 `kModuleRegistry` 现有 **8 个 Router 页面**（首页 / 笔记 / 待办 / Claude / **智能体** / **壁纸** / 设置 / **关于**，枚举序见 `src/core/module_interface.h` 的 `PageId`），**可被模块开关插拔的有 笔记 / 待办 / Claude / 智能体 / 壁纸 五者**（壁纸默认关闭；Home / Settings / About 的 `ModuleEntry` 里模块 id 是 `nullptr`，不可禁用）。`src/` 下有 8 个 `module_*` 目录。

目标平台：**macOS**（arm64）和 **Windows 11**——两平台均为原生支持。CI 双平台构建 + 测试守卫。


## 🧭 智能中枢定位（2026-10-21 定，贯穿所有开发会话的设计基准）

**DevDesk 的战略方向是智能中枢**：形态 = Claude Code / OpenCode 已验证的「智能体 + harness」骨架（工具循环、触发调度、权限闸、审计、技能扩展），**特化在三条轴上——一切智能体相关的设计取舍以这三轴为准**：

1. **无人值守**：触发驱动（定时 / 事件 / 自唤醒）+ 失败重试 + 验收自评 + 失败升级通知。人不在环，可靠性优先于交互性——对话窗、流式响应、交互式 REPL 刻意不做（见路线图「明确不做」清单）。
2. **C++ 原生桌面常驻**：托盘、可视化审计、桌面感知（剪贴板 / 窗口 / 文件状态）——web / Electron 方案做不到的差异化。
3. **业务绑定**：自有模块工具面 + 公司内部 GitLab / 敏捷平台对接——外部产品不会为你做的事。

**路线方针——「走前人铺好的路，继而发展出自己的路」**：每个设计取舍先问「前人（Claude Code / opencode）有没有已验证的答案」——有就照搬机制模式（skills、权限闸、角色体系、质量闸分层），不走它们验证过的岔路（对话窗、流式、交互式 REPL 在路线图「明确不做」清单）；没有答案、或答案不落在三条特化轴上，才自研。前人的路管「骨架怎么搭」，自己的路在「什么是非我们不可」。流水线终点永远是 MR 留人审（ADR 0001）。
**轻资产原则（2026-10-21 用户拍板）**：核心只保留「机制与接口」——智能体循环、工具注册、事件总线、触发调度、权限闸、审计、预算护栏；一切具体业务能力（文件工具、GitLab、搜索、技能包、角色定义…）一律以可插拔形态接入，不进核心。接口质量的硬判据：**新能力以「注册 / 放文件」形态接入的步骤数**——超出标准（工具 4 步 / 事件 2 步）即接口退化，先修接口、不加能力。协议同样可插拔：模型端点与工具协议都是可替换层（`core::LlmEndpoint` / `ToolRegistry` 即接口缝）。参考与反例：dsh（本机 DeepSeek CLI）已验证「万物皆可插件」可行，但其插件状态散落五处（依赖表 / 状态文件 / 包目录 / 两个补丁清单），坏一个皮肤要五处同步、少一处启动就崩——**插件状态必须单一注册源**（一份清单为唯一真相，激活/注销只动这一处），这是 DevDesk 插件面的设计红线。
**参考源码**：`../opencode2/`（opencode v2.1.1）、`../smolagents/`（约千行的极简 agent 骨架）、`../anthropic-skills/`（官方技能格式规范），均 2026-10-21 浅克隆——供反复阅读的本地参考库；吸收成果单独沉淀在 `docs/strategy/07-OpenCode源码吸收报告.md`，持续更新不合并进其他文档。
**权威底稿**：`docs/strategy/00-总览索引.md`（功能/技术/架构/场景四清单 + 路线图 05 + 趋势 06 + 源码吸收 07）——各期实现 spec 从 05 派生，冲突裁决顺序 ADR > 路线图 > spec；术语见仓库根 `GLOSSARY.md`。活文档只保留规则与防错理由，历史包袱与元信息一律归 git 历史（2026-10-21 卫生整顿定，本文档亦受此约束）。

## 🚨 项目核心约束（最重要的几条）

1. **用户不熟悉 C++，纯 AI 开发** — 代码对用户不可见，所有反馈通过：文档 / 中文日志 / 截屏 / 测试结果 / mockup。所有面向用户拍板阅读的文档与报告一律**零黑话**：术语首次出现就地一句人话解释（2026-10-21 用户定；判据 = 不查任何资料一遍读懂）
2. **完整设计文档**：所有架构 / API / 设计 token 决策都写在 `docs/superpowers/specs/2026-09-20-devdesk-design.md`，**实现前先读 spec**
3. **每个 P 阶段完成后必须**：跑测试 + 启动应用 + 截屏 + 更新文档 + git commit（详见 spec §16.2）
4. **配色方案已定为 B（温暖专业 / Claude.ai 风格）**，完整 token 写在 spec §6.5.1 和 §6.5.2
5. **平台适配代码必须接口隔离**（2026-09-21 定）：头文件定义平台无关接口，实现按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake 按平台选择编译（与 EUI-NEO 后端选择同一模式）。**禁止在业务代码里散布 `#ifdef _WIN32` / `__APPLE__`**。虚接口仅在需要运行期注入/mock 时对单个点引入，不全局铺开

## 🛠️ 常用命令

> **🔴 构建与测试一律走 `bash scripts/build-test-run.sh`，禁止手敲 `cmake -B …` / `cmake --build …`。**
>
> 这条规则 2026-09-28 立，依据是当天实测出的事故，不是风格偏好：
>
> 1. **手敲会造出多份互相漂移的构建目录**（当天机器上同时存在 build/build-release/build-debug 三份手工构建目录，互相不一致）。
> 2. **手敲必然漏掉脚本里已经封装好的约定**（EUI_NEO_DIR 自动定位、ccache 参数、OPENSSL_ROOT_DIR 平台差异、发布形态守卫——少了不立刻报错，只在某个时刻以莫名其妙的结果出现）。
> 3. **手敲的命令不会被任何人复现**（脚本是唯一被记录并反复执行的构建路径）。
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

本机工具链：cmake 4.4.3、ninja 1.13.2、AppleClang（`/usr/bin/clang++`）、ccache 4.14.1（均在 `/opt/homebrew/bin`，已在 PATH）。ccache 在 macOS 的持久配置读 **`~/Library/Preferences/ccache/ccache.conf`**——**不是** XDG 的 `~/.config/ccache/ccache.conf`（实测写 XDG 路径 ccache 完全无视）。**当前构建与测试状态见本文「🚦 当前状态」段**（用例总数以运行输出 `Total:` 为准，不在文档写死）。

**Windows 11（原生支持平台，MSYS2 MinGW64 + Ninja；本机当前为 Windows 11，以下命令可直接使用；CI 的 Windows job 走同一套）**：

```bash
export PATH="/d/software/msys64/mingw64/bin:$PATH"

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
**"裸 `cmake -B build`（完全不带 -D）也能吃缓存"这条路走不通，别再试**：① 本仓库 configure 的 OpenSSL/MinGW 根由 `cmake/Dependencies.cmake` 从编译器路径自动推导（2026-10 起无需手传 `-DOPENSSL_ROOT_DIR`）；② 试过 ccache 官方 masquerade（MSYS2 自带 `/mingw64/lib/ccache/bin` 是 bash 脚本、写死 `/mingw64/bin/g++`，在 Git Bash 里指向 Git 自带目录 → 报错）与自建 shim 目录（ccache.exe 复制成 `g++.exe`，实测能让 `g++ --version` 走 ccache），但 shim 会顶替编译器路径 → 本仓库 `DEVDESK_MINGW_ROOT` 从编译器路径反推 MinGW 根（`cmake/Dependencies.cmake`），根变成 `D:/software` 后静态 curl 依赖链解析失败（`set_property can not be used on an ALIAS target`）→ 不采用。

**macOS 侧的脚本（跨平台）**：`scripts/build-test-run.sh` / `scripts/ccache-env.sh` / `scripts/check-consistency.sh` 已支持 macOS/Linux 分支——Darwin 走 `pkill -f` 杀进程、`otool -L` 依赖清单、`screencapture -x` 截屏、`stat -f%z`、`-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`，ccache 配置读 `~/Library/Preferences/ccache/ccache.conf`；原先写死的 Windows 工具链 PATH 与 `C:/msys64/mingw64` openssl 路径改为按 `DEVDESK_OS` 分支取（Windows 分支行为不变，波 1 TIN-13 的 Playwright + `CopyFromScreen` + Pillow 回退仍在）。**该移植当年只做过语法检查，此后 macOS 已成为日常开发平台、脚本在 macOS 上逐日真跑**；`check-consistency.sh` 的 `screencapture` 分支所需的终端「屏幕录制」TCC 权限已授予，但对 OpenGL 窗口可能抓到导航前的旧帧。**agent 会话里不要指望 `screencapture`**：会话由 launchd 拉起、不在终端进程树下，拿不到该 TCC 授权——要出图请用探针的进程内帧捕获（`./build/tests/<probe> --screenshot <路径>`，见「功能开发生命周期」阶段 4）。本文档旧版提到的 `./scripts/dev.sh`（构建+运行）、`./scripts/test.sh`、`./scripts/screenshot.sh <page>`、`./scripts/reset-db.sh` **在本仓库中不存在**（`git ls-files scripts/` 可核），不要再照抄执行。（旧版此处引用的 `docs/KNOWN_LIMITATIONS.md` 已按用户指示整档删除。）

构建入口：`CMakeLists.txt` 顶层 → `src/main.cpp`。
EUI-NEO 依赖：`../EUI-NEO/`（同级目录，通过 `EUI_NEO_DIR` cache 变量 `add_subdirectory` 集成，**不是 git submodule**）。消费政策（2026-10-25 用户拍板，**ADR `docs/adr/0006-eui-neo-consume-policy-latest-source.md`**）：**源码同级目录引入、不锁版本、每次用最新源码**——无最低依赖 rev、无 pin，configure 期不做任何 rev 校验（旧 rev 软校验已删，其基准 rev 在 EUI-NEO 历史重写后已蒸发）。版本匹配的唯一权威判据 = `scripts/check-eui-neo-version.sh` 的**内容级 API 守卫**（src/ 下全部 EUI-NEO 消费编译单元逐个 `-fsyntax-only`）。远端形态：EUI-NEO 的 `origin` = coderstory/EUI-NEO（fork，**已与本地 main 同步**），上游 sudoevolve/EUI-NEO 仅作参考；CI 的 `EUI_NEO_REPO` 克隆 fork、`--depth 1` 拉 HEAD = 最新源码（**政策，非缺陷**）。**EUI-NEO 需保持本地 main 最新**（含 onWindowDestroyed/requestFocus/trayMenu/windowEffect/lineStyles/getMonitorWorkAreas/setSystemPowerHandler/styledMenu/onTrayMenu/windowIsFocused/input onCancel 等新 API），用了新 API 先推 fork 再推 DevDesk。

## 🏗️ 架构要点（4 层模块化）

```
main → 8 个 Router 页面 (home / markdown / todo / claude_proxy / agent / wallpaper / settings / about)
            ↓
       core (Result / paths / sqlite_db / logging / theme / router)
            ↓
       platform (http_client / http_server / keychain / credential_protection / json)
            ↓
         EUI-NEO
```

**关键依赖规则**（违反会导致循环依赖）：
- `core` ❌ 不能依赖 `platform`；❌ 不可调用 EUI-NEO 运行时行为（仅 **header-only 类型桥接**例外——eui::Color 值类型 / Ui/Screen 签名透传 / component_theme.h 纯映射，边界见 spec §4.2.1）
- `module_*` 之间 ❌ 不可互相依赖
- `platform` ✅ 可以依赖 `core`（用 `Result`）
- `module_*` ✅ 可以依赖 `core` 和 `platform`

**注释纪律（硬规则，写代码时同步遵守）**：

> **出发点：代码的唯一读者是 AI，不是人。** 这一条决定其余全部——AI 解析代码零成本、
> 记得住文件里的每条约束，**但会自作主张地"改进"它认为冗余的代码**。
> 注释的唯一任务是帮 AI 在「该做什么、该不做什么」上不犯错。

**唯一判据**——对每条注释问一句：*这句话是在帮 AI 少犯一条错，还是在浪费它的注意力？*
少犯错就留，浪费就删。没有第三档，没有「以防万一」。

**总纲——新增注释「契约与不变量优先，叙事历史进 git/docs」**：新写的注释先回答「读代码的人必须知道什么才不会用错」（接口契约、并发与线程约束、错误码语义、跨文件不变量、反重构警告）；「此前如何 / 后来改成什么 / 为什么改」等演变叙事一律不进注释——演进史查 `git log`，决策与背景沉淀进 `docs/`（ADR / spec / plans）。**存量注释不回退**：不要求回溯改写，新增与触碰到的注释按本纪律执行。
- **必留 · 反重构警告（最高优先级）**：凡是「看起来该被清理」但其实不能动的，必须写明为什么。
  人类不会擅自重构，AI 会，而且很自信。典型：顺序不可换（反了会怎样）／刻意不用某 API（用了会怎样）／
  看似冗余但必需（省了会怎样）／缺省值陷阱（漏了会怎样）／不许顺手清理的怪异写法。
  例：`// canChooseFiles = false 不能省：省了用户能选到单个 .mp4，扫描器枚举后返回空`
- **必留 · 契约**：① 接口契约（参数单位 / 返回语义含三态与哨兵值 / 前置条件）
  ② 并发与线程约束（哪个函数必须在主线程、哪个回调的投递线程、哪把锁）
  ③ 错误码语义（枚举值到底代表什么）④ 非显然的坑（只陈述规则，不写发现过程与调试故事）
- **必留 · 跨文件不变量**：单文件里推不出来的规则。例：「`core` 不能依赖 `platform`」
  「`module_*` 之间不可互相依赖」「四个 Watcher 都必须强持有，否则回调永久泄漏」。
- **必删**：复述代码行为／复述命名／演变叙事（「此前」「改成」「旧实现」——AI 会自己 `git log`）／
  出处与证据（`spec §4.2.1`、`2026-10-02`、commit hash）／需求原话引用／
  验证体系自解说（「守卫：探针组 K」「判据锁着」，AI 不参与该体系）／
  设计推演（「为什么不抽出来」，结构本身已表达答案）／告警符号（⚠️🔴 与「这行最容易被顺手删掉」——
  AI 不会因恐吓而更小心，只会很困惑到底哪里危险，写事实即可）／纯装饰分隔线。
- **篇幅不设上限**。行数、长度都不是判据，信息密度才是。
  仅要求排版：默认一行写完，超宽自然折行，不要为了「看起来整齐」主动折；
  折行只在句子边界，参数说明与枚举项各自独立成行。
- 现状限制可以留（如「暂不支持 X」），一句话即可。

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

- **默认窗口**：1600 × 1000（**逻辑单位**，`main.cpp:485-486` 的 `kInitialWindowLogical*`）
- **最小窗口**：**1000 × 640（逻辑单位）**（`main.cpp:492-493`；2026-10-20 UAT3 D-R3-2 从 1280×800 收窄，用户拍板）。两平台均不再被工作区钳制：macOS 640 ≤ 805 可用、Windows@125% 640×1.25=800 物理 ≤ 973 可用——**有效下限 = 设计下限**（1280×800 时代 Windows 高曾被钳 973 物理 = 778 逻辑）。历史口径：1280×800（UAT2 从 1330×845 收窄）、1200 × 720（spec 早期废弃值，**不可回落**）。落地时仍经 `core::window_layout::minWindowSizeForWorkArea` 换算成窗口单位、**并与工作区取 min**：工作区放得下才落实该下限，否则落地为「工作区 − 非客户区外框」。细节见 `src/core/window_layout.h` 文件头，**别把「落实 640」当无条件事实**。
- **HiDPI**：**不是**靠 `GLFW_SCALE_TO_MONITOR`——该 hint **从未被设置**（全仓只有 `glfwGetWindowAttrib` 读它用于诊断日志，DevDesk 与 EUI-NEO 皆然；`src/core/window_layout.h:6` 明写「Windows GLFW 路径不设 GLFW_SCALE_TO_MONITOR」）。真实机制：`framebuffer ÷ (contentScale × uiScale)` 得逻辑尺寸，`core::window::windowScaleForPoint()` 在 macOS **恒返 1.0**（Retina 的 2.0 是 contentScale，不是窗口单位换算）。**要调缩放别去找那个 hint**，细节见「已踩过的坑 → 单位错配」。
- **主品牌色**：Claude Clay `#D97757`
- **配色**：方案 B（warm professional，暖墨底 + Claude Clay 橘）
- **正文**：15 px（不是 14）
- **页面标题**：32 px
- **统计数字**：48 px
- **主按钮高度**：40-44 px（不是 32）
- **间距**：基于 8 px 网格，主区 padding 40 px
- **圆角**：按钮 10 px / 卡片 12-14 px / chip 999 px（pill）

完整 token 见 spec §6.5.1 和 §6.5.2。

## 🚦 当前状态

> **状态文档时效约定（2026-10-26 立）**：任何「当前状态」类段落 / 文档**必须带日期戳**（写明最后一次核对日期）；**时点性的状态快照**（检查报告 / 评估 / 审计 / PR 描述等）**超过 30 天未核对即移入 `docs/archive/` 归档**——`docs/` 顶层只留长效文档（架构 / 指南 / 规格 / 能力盘点），过期快照留在顶层会被误当现状引用。本节日期戳：**2026-10-26 核对**。

> **当前权威台账与规则（UAT6.2）**：
> - 权威台账 = `docs/superpowers/plans/2026-10-21-UAT62-整改清单.md`
> - 波次/时限规则 = `docs/superpowers/plans/2026-10-23-UAT62-收敛执行计划.md`
> - Todo 契约 = `docs/superpowers/plans/2026-10-23-UAT62-H20-Todo四列固定与归档折叠设计.md`

> **测试与 CI 状态以实际运行输出和 `.github/workflows/ci.yml` 为准**，不在文档写死数字。

- **CI**：双平台（macOS xcode-27 / Windows windows-2025），`push dev` / `PR → dev·master` 触发，跑 `scripts/build-test-run.sh --release --no-run`
- **已知欠账与架构债**：已归档到 `docs/strategy/03-软件架构能力清单.md` 第 6 节
- **近期交付**：见 `git log`
- **跨仓库耦合**：DevDesk 硬依赖 EUI-NEO 最新源码（**不锁版本**，消费政策见 ADR 0006）；推 DevDesk 前若用了 EUI-NEO 新 API 必须先推 EUI-NEO 的 `origin`（= coderstory fork，CI 克隆它）；版本匹配由 `scripts/check-eui-neo-version.sh` 内容级守卫把关，CI `--depth 1` 拉 fork HEAD = 最新源码

## 📦 安装程序（2026-09-28）

- **Windows**：Inno Setup `installer/windows/devdesk.iss` —— 用户级安装（`%LOCALAPPDATA%\Programs\DevDesk`，`PrivilegesRequired=lowest`，免管理员）。`:65` `Source: "{#AssetsDir}\*"; DestDir: "{app}\assets"; Flags: recursesubdirs createallsubdirs` 递归拷 assets 到安装目录；`.exe` 单独一条（:59，loose 文件非 embed）；`:60-64` 注释：Excludes 排除 17MB `NotoSansSC-VF.ttf`（变量字体 FreeType 加载不稳、当前未启用，运行时字体走 EUI-NEO fallback 链）
- **打包脚本**：`scripts/package-windows.sh`（Release 构建 + ISCC 三段式探测 + 出 `DevDesk-<ver>-setup.exe`）；macOS：`installer/macos/`（DMG 背景 + 首次运行去隔离 `.command`）+ `scripts/package-macos.sh`
- spec：`docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md`

## 📂 关键文件

| 文件 | 作用 |
|------|------|
| `docs/superpowers/specs/2026-09-20-devdesk-design.md` | **唯一权威 spec**，所有架构 / API / token 决策都在这 |
| `docs/mockups/palette-comparison.html` | 配色对比 demo（已确定方案 B）|
| `docs/mockups/devdesk-prototype.html` | HTML 交互原型（待生成）|
| `../EUI-NEO/` | 上游 GUI 框架（同级目录源码，非 submodule），通过 `EUI_NEO_DIR` 以 `add_subdirectory` 集成；**消费政策：不锁版本、每次用最新源码（ADR 0006），版本匹配由 `scripts/check-eui-neo-version.sh` 内容级判据保证**；EUI-NEO 的 `origin` = coderstory fork（CI 克隆它，已与本地同步），见「常用命令」段 |
| `docs/superpowers/specs/2026-09-27-*.md` | 09-27 批次 specs：wave1-fix / god-class-refactor / shared-config-and-failover |
| `docs/superpowers/specs/2026-09-28-*.md` | 09-28 批次 specs：installer-and-dmg / devdesk-probes / r8-assembly-layer / tin06-test-scaffold-migration / setscrolloffset-upstream-m6-refactor / md01-reload-block-mapping / macos-three-features / ui-modernization / profile-run |

## 🧪 验证原则

- **未运行测试 = 未完成**
- **未截屏 = 未完成**
- **未读 spec = 未开始**
- **未 commit = 未持久化**
- **不要假设 EUI-NEO 能力** — 不确定时去 `../EUI-NEO/` 源码 grep
- **按场景写测试**：每条用例对应一个具体业务场景、注明防的具体事故（场景 = 用户从什么状态来、在什么环境做什么；防 = 改动回退或场景条件变化时会红）。「覆盖 XX 函数」不是测试目的。
- **智能体类测试两段制（2026-10-21 定）**：先用注入 mock（假模型端点、假工具）验证理想路径与分支逻辑，不发真实网络——但 **mock 只证明理想情况，收口验证必须真模型实测**：草稿生成、任务运行、工具循环每类至少一次真实端到端调用（真网关方言、真 token 计费、真超时行为只有实操才暴露；反例实证：UAT3 星锐网关拒 `[1M]` 模型名，mock 全绿真网关当场爆）。测试目的 = 验证用户场景与边界，不为代码覆盖率。
- **所有业务场景必须实际跑起来**：验证金字塔 = 纯逻辑单测（守重构）< 无头几何 < **真窗口 probe（真点击 / 键盘 / 拖拽 / 渲染）** < 真网 live < 真机 UAT。交付主流程至少到 probe 层、涉网关到 live 层。无头单测绿不算「实际跑过」。
- **测试绿 ≠ 可用，数字不报给用户**：向用户交付时报三行——哪些场景实际跑了 / 结果 / 哪些没跑到（明写"未验证"）。

---

## 🤝 AI 协作与交付纪律（团队通用）

> 本节是**团队 / 跨会话**通用纪律：谁改文件、怎么派单、怎么收尾、审计口径、已踩过的环境坑。工具链细节见同文件「常用命令」段，架构约束见「项目核心约束」段。

### 派单与并发

- **主会话零分析/开发（2026-10-27 用户定，硬标准）**：错误定位、方案设计、代码分析、修复实现、文档撰写一律**不进主会话**——全部创建子任务执行；主会话只保留：需求裁决、任务拆分派单、构建 / 测试产物验收、只读核查、合并与提交决策、环境准备（依赖下载等非开发杂务）——构建 / 测试的执行本身可派 `devdesk-tester`。子任务一律用工作区根定义的六类子智能体创建（位于 `../.zcode/agents/`，ZCode 会话级配置，DevDesk 仓库外），按工种对号入座：`devdesk-builder`（实现类：修码 / 写码 / 重构 / 构建修复 / 文档撰写）、`devdesk-auditor`（只读审计类，实现者不得自审）、`devdesk-investigator`（只读侦察类：错误定位 / 影响面 / 方案比选 / 派单前侦察）、`devdesk-researcher`（外部调研类：联网查最新动态 / 技术方案 / 开源实现，选型与可行性核对，结论必带一手来源引用，防闭门造车与重复造轮子）、`devdesk-tester`（构建与测试执行类：定向测试 / 探针 / 截屏 / 先红后绿取证）、`devdesk-upstream`（EUI-NEO 跨仓库联络类，非隔离线，只碰 `../EUI-NEO/`），不得用其它类型创建开发 / 分析子任务。*为什么*：既分析又实现失去第二方视角；主会话 token 昂贵，子任务可钉低成本模型（六类 agent 的 frontmatter 均钉 GLM-5.3-Flash）。下一条「主会话禁改文件」（2026-09-27 定）是本条在「改文件」维度的既有细则，继续有效。
- **主会话禁改文件**（2026-09-27 定）：源码 / 测试 / 配置 / 文档一律**派子任务**改；主会话只做需求裁决、任务拆分、派单、只读核查（Read/Grep/Glob）、产物审计、合并与提交决策。**边界**：`commit` / `merge` 不算改文件（写的是 `.git` 记账、不动工作区），主会话可直接执行——别拿这条当不提交的借口；构建 / 测试 / 只读 shell 同样可在主会话跑。*为什么*：既写又审会失去第二方视角（当时主会话连续直接改源码和配置被纠）。
- **粒度看复杂度，不看文件数**：引用点已查清、无需独立判断的机械改动（3-6 文件也算）**合并成 1 个子任务**做完，不拆成 N 个；需独立探索 / 设计判断 / 影响面广 / 要第二视角才拆多个并行。*为什么*：拆细后子任务启动 + 产物验证 + 等通知的成本高于收益（6 文件机械删除被派了独立子代理，用户纠"不复杂时成本反而更高"）。与上一条合起来 = **"少派、但必须派"**（"主会话自己 inline 做"的旧做法已作废）。
- **并发上限 8 个在飞子任务**（2026-10-21 用户上调）。**仍保留的约束**：① 其中**同时只允许 1 个执行编译/测试**（多 worktree 并发构建互抢 IO/ccache 实测互拖，派构建类任务前先确认构建槽空闲）；② 同一文件同一时刻只有一个写者（见下条）；③ 多 worktree 并发构建互拖时主会话主动收束（I/O / 内存带宽饱和、ccache 热命中率被挤压 → 串行排队或降 `DEVDESK_JOBS`），不必等用户干预。构建并行度是另一件事，见「常用命令」段的 `DEVDESK_JOBS=3`。
- **同一文件同一时刻只有一个写者**：派单前先列在飞白名单做交叠检查。*反例*：漏检让 R0 与 H2 同写 `settings_module.cpp`，只能 `TaskStop` 一条。
- **禁止长时间 sleep（上限 60s）**：脚本/命令里不许 `sleep > 60`（如 `sleep 120` 禁用）；派单 prompt 明确禁子任务用长 sleep 等待（构建超时用 `timeout` 包，测试异步用有界轮询/事件信号而非固定长睡）。*为什么*：2026-09-28 用户指令，长 sleep 拖垮节奏且无信号价值。
- **Bash 超时上限 3 分钟**：主会话与子任务的任何 Bash 调用，`timeout` 参数默认最多 3 分钟（180000 ms）。长构建 / 全量测试 / 打包走后台跑（`run_in_background`）+ 完成通知，不靠前台长等。**只有当超时本身不是脚本或代码的问题、而是这项工作天生就慢**（例：全新 worktree 冷编译首次 configure、--clean 全量重建、大型安装包打包）才允许往上调，且必须在报告里写明「为什么这个慢是正常的、不是卡死」，并在可行时改后台 + Monitor/通知而不是把 timeout 顶满。*为什么*：2026-10-02 用户指令——超长 timeout 掩盖卡死（本会话多次出现 agent 卡住数十分钟才发现），而真正需要长时间的构建类任务本就该后台跑；把 10 分钟顶满当默认等于放弃卡死检测。
- **子代理不吃会话风格**：caveman / 极简是**每会话**设置、子代理不继承，不写规则就吐长报告——长报告是主会话 token 的主要开销。派单 prompt 末尾固定加：「报告用极简风格——删铺垫 / 客套 / 复述 / 过程叙述，只留结论、证据（file:符号名 + 最短决定性输出）、sha、数字、待办；每条结论一行；总长 ≤ 原上限一半；技术精度不变，不许为省字含糊」。适合的场景改用 `cavecrew-*` agent（`cavecrew-investigator` 定位代码 / `cavecrew-builder` 一到两文件小改 / `cavecrew-reviewer` diff 审查），其输出本身即压缩形态。**边界**：写进仓库的文档 / commit message / memory 仍是正常文字（极简只针对给主会话的报告）。
- **意外终止先唤醒原 agent，不重派**：子任务崩溃 / 卡死被清理 / 静默失联（无 sha、无报告）时，用 `SendMessage` 唤醒**同一个** agent 从 transcript 续跑，写明"你上次停在哪、接着做别从头再来"；**不是**主会话接手，**不是**派新子任务重做（换新 agent 丢上下文 = 从零烧 token + 重跑全量编译）。有回单但结论存疑 = 走审计流程，不属本条。唤醒失败（agent 已被彻底回收）→ **升级给用户**定夺，不自行改派。派单时即记录 agent id（无人值守靠 watchdog 先探活、再唤醒）。

- **项目级 hooks 已内置（强制层，2026-10-21）**：`.claude/settings.json` 四条——①基线文件（docs/strategy、docs/adr、docs/superpowers、GLOSSARY.md、CLAUDE.md）被改后自动跑累积核对清单，丢历史标记即拦截（`.claude/hooks/strategy-guard.sh`，唯一可阻塞的一条）；②merge/pull/rebase 后「分支绿≠合并绿」门禁提醒；③commit 前「先红后绿」提醒；④改 `src/` 前「plan 先行」提醒。提醒不阻塞、守卫才拦截；改 hook 配置须走子任务 + 管道实测（合成 stdin 喂脚本验证两条路径），不许只改不测。

### 交付流水线（四律，按序叠加）

1. **只跑定向测试**：只编译受影响目标 + 只跑相关用例（macOS：`./build/tests/devdesk_tests <功能子串>`；Windows：`./build/tests/devdesk_tests.exe <功能子串>`，如 `markdown` / `proxy` / `settings`）。**禁全量重建、禁全量套件**（用户原话"进度太慢了"）；全量只在里程碑收尾 / 提交前由主会话跑一次。
2. **涉及编译 / 测试的子任务必须 worktree 隔离**（`isolation: "worktree"` 或手动 `git worktree add`），**主工作树禁跑构建**；worktree 首建用 `scripts/build-test-run.sh` 自动 configure——它按 `EUI_NEO_DIR` 环境变量 → 同级 `../EUI-NEO` → **git 主工作树同级**依次定位框架源码，并自动补 OPENSSL（macOS `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`；Windows 由 `cmake/Dependencies.cmake` 从编译器路径自动推导）等平台参数，**worktree 里首建不需要手敲任何 `-D`**。
3. **完成即提交并合 `dev`**：agent 在自己的分支原子 commit（**显式 pathspec，禁 `git add -A`**）并报 sha → 主会话审计（读 diff + 复跑验收）→ **串行**合并进 **`dev`**（多 agent 同时 merge 会撞 `index.lock`）。**不攒批**。⚠ **集成分支是 `dev` 不是 `master`**：近期的合并全是 `Merge branch 'worktree-agent-*' into dev`；`master` 只是**发布分支**（`.github/workflows/release.yml` 只在 `push master` 时出 Release 制品），本机甚至没有 `master` 分支（要引用得写 `origin/master`）。别把改动合到一个不存在的本地 `master` 上。**分支绿 ≠ 合并绿**：任何 merge 提交后必须对合并后的 dev 树跑一次完整门禁——worktree 上的审计闸不覆盖合并结果（f274f74 事故：审计在 worktree 跑闸、合并进 dev 后从未构建，dev 当场无法 configure，直到 1ad2ac9 补救）。
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
  - **UI 测试**：跑 `tests/probes/` 探针（`(cd build && ctest -L probe)`，或直接跑单个 probe 可执行文件）。`build-test-run.sh` 不跑探针，必须单独执行。probe 能力：`tests/probes/probe_harness.h` 提供真实窗口后端（GLFW + OpenGL）+ 真实指针 / 键盘 / 滚轮 / 文本 / 拖拽注入 + 几何与 z 序断言 + **进程内帧捕获（`--screenshot <路径>`）**。**凡改动涉及可见 UI 的，必须有 probe 或等价的无头几何断言覆盖——光有单测不算。**
  - **看图：`--screenshot <路径>`（2026-10-03）**——`screencapture` 在 agent 会话里拿不到「屏幕录制」TCC 授权（会话由 launchd 拉起、不在终端进程树下），所以探针改成**进程内 `glReadPixels` 读自己的 GL 后备缓冲**再编码成 PNG，**不需要任何系统授权**。任何探针都能拍：probe 退出前调 `probe::captureIfRequested(args, window, runtime, &passed)`（`home_probes` 已接）。用法：
    ```bash
    ./build/tests/home_probes --screenshot /tmp/home.png     # 跑一次就拿到当前状态图（图为 framebuffer 尺寸，如 2400x2000 @2x）
    ```
    （2026-10-03 轻量化：原 12 个 probe 里 8 个与无头单测逐条重复、且 CI 从不执行，已删；剩 `home_probes` / `screenshot_probe` / `settings_dropdown_z_probe` / `module_toggle_sidebar_probe` 这 4 个各有独占判据——首页矮窗滚动与几何、帧捕获自测、真窗口 z 序、真实点击派发栈。）
    图尺寸 = **framebuffer 像素**（逻辑 × contentScale），不是 `--size` 的逻辑值。落图失败 → stderr 明确原因 + 退出码非零（不静默）。**不传该开关时零开销**（不建渲染后端、不渲染、不写文件、不改退出码）——这条由 `screenshot_probe` 的断言④守着。要多张/自定义构图用 `probe::FrameCapture capture(window)`（`capturePng` / `readFrame`）。细节见 `tests/probes/probe_harness.h` 文件头「帧捕获」段；`tests/probes/screenshot_probe.cpp` 是这条能力的自测（PNG 合法性 + 翻轴 + 非全黑）。**给探针补 GL 调用时不要再单独 `#include <GLFW/glfw3.h>`**——harness 已统一给 GLFW + glad，两者混用会撞 glad.h 的 `OpenGL header already included` #error。
  - **覆盖率口径（工具已就位：`scripts/coverage.sh`，2026-10-03 落地 `1a3de0b`）**：`bash scripts/coverage.sh` 全流程跑出分层行覆盖表——独立 `build-coverage/` 目录，插桩只覆盖 DevDesk 库层（core / platform / ui_kit / module_*，含 agent），EUI-NEO 与测试/探针/app 源码不插桩（CMake 开关 `DEVDESK_COVERAGE`，默认 OFF、正常构建零变化；测试 Failed 非 0 即中止，不出假基线）。**引用覆盖率数字只认该脚本全流程输出**（spec §12.3 目标：核心工具类 ≥90% / Service ≥80% / Platform ≥70%），不手算、不拿部分跑当基线；首批基线（2026-10-03，dev `99a9aef`）：DevDesk 合计 87.1%，core 79.5%（未达标）/ platform 81.2% / pet 40.0%（各模块最低）——数字会漂移，以脚本最新输出为准。Windows（MinGW GCC）的 gcovr 路线与 CI 接入仍是欠账。工具之外三条口径继续有效：① 新增 / 修改的行为必须有断言，没有断言的改动视为未完成；② 修复类改动必须有"用例在修复前会红、修复后会绿"的证据——回退一次修复跑出红，或用变异验证等价地证明断言非恒真；恒真断言即假绿，须重写；③ 涉及几何 / 布局的改动必须有数值断言，不接受"看起来对"。
- 通过判据：定向用例全绿（报告贴出 `Total / Passed / Failed` 行且 `Failed: 0`）；涉及可见 UI 的改动有 probe 覆盖且 `ctest -L probe` 全绿；修复类改动有"先红后绿"（或变异验证）证据；几何 / 布局改动有数值断言；**凡改动涉及可见 UI 的，实现者必须交付前自己启动 app、打开窗口、截图看效果**（断言能证明"几何数值对"，证明不了"看起来对"）——可复制做法：① 用**独立的 `DEVDESK_DATA_ROOT`** 启动自己 worktree 的构建产物（**不得用用户真实数据根、不得碰 `/Applications/DevDesk.app`**）；② `screencapture -x <file>` 截图（屏幕录制权限已授，实测可用）；③ 需要点击 / 移动鼠标驱动界面时用 `cliclick`（`/opt/homebrew/bin/cliclick`，坐标是**点** = 物理像素 ÷ 2；**AppleScript `click at` 对本 app 无效，别用**）；④ **已知坑**：`screencapture` 对 OpenGL 窗口**可能抓到导航前的旧帧**——移动鼠标触发重绘后再拍第二张、取后一张，报告里要说明是否遇到；⑤ **⚠️ 严禁 `pkill -f DevDesk`**——多个 agent 可能同时在跑 app（还有用户自己在用的实例），按名杀会互相杀并杀掉用户的 app，**只按自己启动的 PID 杀**。*为什么*：2026-09-28 的真实事故——标题栏无底色、详情面板 60% 空白、卡片缺边距、侧边栏边距改错层次，**全都是用户肉眼先发现、我们事后才验证的**。
- 不过怎么办：任何一项红 → 回阶段 1 / 阶段 3 修；发现恒真断言 → 重写断言（不许把断言删掉了事）。
  - **交付前三问**（2026-10-19 定，真机 UAT 三连爆教训——全量 1632 条测试全绿的当天，用户真机 UAT 连爆：老库缺 `context_note` 列建不了任务、星锐网关拒 `[1M]` 模型名草稿失败、Windows 字体度量致弹窗叠字）：发任何包 / 宣布完成前必答——① 存量数据升级路径验过吗（老库场景用例 / 真实库副本实测）② 真实外网 / 网关验过吗（方言断言 + live）③ 目标平台真机验过吗（probe 真窗口 + 用户 UAT）。答不上写进"未验证"栏，不许沉默。

**阶段 5：合并与发布**

- 谁做：主会话执行合并，**串行**（一次只合一条；多 agent 同时 merge 会撞 `index.lock`）；合并后的重建与安装同样在集成分支上做。
- 做什么：功能分支完成审计 + 测试后合并进 **`dev`**；合并后在 `dev` 上重新构建 + 安装——用户的标准是「编译 = 编译 + 复制到 `/Applications`」。**发布另有一步**：把 `dev` 推到 `master` 才会触发 `release.yml` 出双平台制品。
- 通过判据：`dev` 上有本次改动的 merge commit（报告给出 merge sha），且合并后重建通过、已覆盖安装到 `/Applications/DevDesk.app`。**没合并 = 没交付。**
- 不过怎么办：合并后构建 / 测试红 → 不得交付（回退或修复后重走本阶段）。
- *为什么（今天的血教训）*：实现了、测试也绿了，但没合进 master，用户在新功能上验证的还是旧代码，得出"还是没效果"；另一次是安装的还是修复前的版本，用户同样在旧代码上验证新功能。
- **修复合并 ≠ 交付**（2026-10-19 定）：新包打出来放用户桌面 + 用户真机 UAT 通过才算完。用户手里的包未更新时，对他来说修复不存在（0.4.3 事故实证：修复已在 dev 且已 push，用户装的旧包照样炸）。

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

- **构建 / 测试**：`bash scripts/build-test-run.sh` 内部处理工具链 PATH（macOS 前置 `/opt/homebrew/bin`；Windows 三件套 cmake 4.4.4 / ninja 1.13.2 / `D:/software/msys64/mingw64`）；测试二进制 macOS `build/tests/devdesk_tests`、Windows `build/tests/devdesk_tests.exe`，两者都**支持子串过滤**。测试框架是**自制迷你 doctest**（`REGISTER_TEST` + fail-fast，**不是**真 doctest）——grep `TEST_CASE` 找不到用例、`--list-test-cases` 被当过滤子串属正常。用例总数与 CI 现状见本文「🚦 当前状态」。
- **并行度封顶**：`CMAKE_BUILD_PARALLEL_LEVEL` = `DEVDESK_JOBS`（**默认 3**，`scripts/build-test-run.sh:90`）——多条 worktree 并发构建时避免互拖；单条独占机器才调高（如 `DEVDESK_JOBS=8`）。
- **ccache 持久配置按平台各一份**（`.sh` 里已接，**平时不用管**）：Windows `%LOCALAPPDATA%\ccache\ccache.conf` + `D:/software/ccache-devdesk/ccache.conf`；macOS **`~/Library/Preferences/ccache/ccache.conf`**（`cache_dir=~/Library/Caches/ccache-devdesk` / `base_dir=~/dev/gui` / `hash_dir=false` / `max_size=5G`）——**不是** XDG 的 `~/.config/ccache/ccache.conf`，写那儿 ccache 完全无视。**不设 BASEDIR = 白装**：每棵 worktree 绝对路径不同 → 同一份源码哈希不同 → 永远 miss。要**临时覆盖**再 `source scripts/ccache-env.sh`。
- **codegraph 索引排除 worktree / build**：`/Users/coderstory/dev/gui/codegraph.json`（Windows 侧同文件在 `D:/project/gui/`）的 `exclude` = `.claude/worktrees/**`、`**/.claude/worktrees/**`、`DevDesk/.claude/worktrees/**`、`wt-*/**`、`**/build/**`（新增临时目录时同步维护，别把产物灌进索引）。
- **`DevDesk.exe` 会锁 build 目录**：运行中链接报 `cannot open output file DevDesk.exe: Permission denied` → `taskkill -F -IM DevDesk.exe` 解锁（**别杀 `claude.exe` / `cc-switch.exe`**）。
- **代理直测脚本**：`doc/scripts/proxy_smoke_test.py`（`proxy` / `upstream` / `all`；环境变量 `DEVDESK_PROXY` / `DEVDESK_MODEL` / `DEVDESK_TOKEN`）。
- **EUI-NEO 版本匹配**：消费政策 = **不锁版本、每次用最新源码**（ADR 0006）；无最低依赖 rev / 无 pin，configure 期零 rev 校验。唯一权威判据 = `bash scripts/check-eui-neo-version.sh --root . --eui-dir ../EUI-NEO`（内容级 API 守卫）绿。EUI-NEO 要保持本地 main 最新并与 `origin`（coderstory fork，CI 克隆它）同步，见「常用命令」段与「跨仓库耦合」段。
- **已踩过的坑（写码前先想一遍）**：
  - **单位错配（系统性）**：`windowSize` / `glfwCreateWindow` 是**客户区物理像素**，compose 侧按 `framebuffer/(dpiScale×uiScale)` 折算逻辑像素——菜单窗、启动窗、桌宠锚点都栽过。本机 125%：工作区 1920×1020 物理、任务栏 60 px，窗口客户区底部 52 物理 px 会被任务栏吃掉（贴底 UI 不可见）。
    - **上面这条只是 Windows 口径**；macOS 口径（2026-09-28 实机回填，三行实测）：① `windowSize` / `glfwCreateWindow` / `glfwSetWindowSizeLimits` 的单位是**点**——`core::window::windowScaleForPoint` 在 macOS **恒返 1.0**（Retina 的 2.0 是 contentScale/backing scale，**不是**窗口单位换算，乘上去会双倍放大）；② 本机实测 `framebuffer 2940×1602 / contentScale 2.0 → 逻辑 1470×801`，即 `screen.height` **等于客户区点高**（两平台都只除一次，无多除少除——"Retina 没折算"的假设已被此数据证伪）；③ 工作区 1470×833 点、标题栏 28 点 → 客户区 805 点；设计最小窗高现为 800 点 < 833（2026-10-20 UAT2 从 845 收窄——旧 845 点 > 833 点曾致「最小值 > 可用面积」，这正是最小窗必须经 `core::window_layout::minWindowSizeForWorkArea` 做**同一条换算**再与工作区取 min 的原因，只做其中一步就是把病换个平台复制）。非客户区外框同样按平台注入：macOS `{0,28}`、Windows `{14.4,37.6}@96dpi`（`window_layout_macos.cpp` / `_win.cpp`）。
  - **三套坐标系**：窗口坐标（`onContextMenu` 给的是它）/ 页面坐标（`app.page` 子树，原点 x = 侧边栏宽 **208/72**）/ 控件局部坐标，跨层必须换算（`Router::pageOriginX` 为此而加）。
  - **上游方言 `upstream_dialect`**：UA 不能透传客户端（Cloudflare 1010 按 UA 封）、`opencode.ai/zen/go` 需 `x-opencode-session`、模型名带 `[1M]` 要剥。
  - **自定义 check 必须 raise**（只 print 的"全绿"是假象）。
  - **负载敏感 flake**：`md_preview_highlight` / `md_highlight` 的 `<8ms` 墙钟断言、`test_claude_ui` 的固定端口 28888。
- **`~/.claude/settings.json` 归属（2026-09-27 定）**：**DevDesk 是 settings.json 接管功能的终局 owner**，cc-switch（本地代理 15721 + `sessionAutoSyncEnabled` 反向同步）只在过渡期并存，**不为它做任何兼容 / 协调处理**（用户原话"不需要管 cc-switch，本软件开发完善后就不使用了"）。并存期两边都会整体重写 settings.json；cc-switch 库里存有 DevDesk 代理时代的旧快照（`ANTHROPIC_BASE_URL=http://127.0.0.1:8888` + 老 token），切 provider 时可能吐回 → 用户感知为"配置一直被还原"。**识别写者看键名**：DevDesk 写 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED`，cc-switch 写 `ANTHROPIC_API_KEY=PROXY_MANAGED`。DevDesk 三个写入时机（`claude_service.cpp`）：起代理 takeOver / 运行中切 profile 再同步 / 停代理或退出按键还原；启动只在有残留 sidecar 时补还原（`main.cpp` 启动序列）。再遇"配置被还原"类反馈：先按键名判写者，是 cc-switch 写的一句话说明即止——**不派任务、不做兼容**。

### 反事故红线

- **禁批量清理"在飞"的 worktree**：清理前先 `ListAgents` 判活，在飞的绝不碰。判定式必须含"**有提交**"这条：`tip != base && git merge-base --is-ancestor <branch> HEAD`（base 取该 worktree 创建时的**集成分支（`dev`）** tip，可从分支 reflog 的 `branch: Created from` 读）。批量删除**逐条确认**，别一条命令梭哈——`git worktree remove` 在文件被占用时会**半删**（`.git` 没了、目录还在），恢复成本更高。*事故*：只用 `--is-ancestor` 判定时，**在飞 agent 的分支 tip 就是基线提交（尚未提交）也为真** → 三条在干活的 worktree 连同未提交改动一起被删、另两条半残，三条线只能重做（成果默认只在工作区，"无提交"与"已合并"在分支指针上长得一样）。误删后：`ls` 看目录是否残留 → `git worktree repair`；`.git` 已丢则重建同路径同名分支的干净 worktree，再 `SendMessage` 唤醒原 agent 凭其报告重做（transcript 里的清单能省掉重新分析）。
- **跨仓库改动必须"产补丁 + 非隔离线应用"**：带 `isolation: "worktree"` 的 agent **无法对任何其它仓库执行 git 操作**（`cd <别的仓库> && git …` 与 `git -C <任意路径> …` 一律被隔离守卫拒）→ 建不了别的仓库的 worktree、也提交不了。正确形态：**隔离 agent 产 patch + 验证**（可用文件副本模拟依赖来真跑构建 / 测试）→ **另起一条"非隔离" agent**（不设 `isolation`）把 patch `git apply` 进目标仓库、跑编译、原子 commit、清理副本；非隔离线的 prompt 必须写死"只许改 `<目标仓库>`，禁止碰 DevDesk 文件"。反过来**别让非隔离 agent 碰 DevDesk**（它没有隔离保护，工作目录就是主树）。主会话**不要**替 agent 落文件（含 `git apply`）。*为什么*：DevDesk 与 EUI-NEO 是两个仓库（同级目录、非 submodule），上游能力改造天然跨仓库。
- **`git merge` 不与 Agent 派单放在同一条消息里并行跑**：主会话 git 操作与派单并行会因 git 锁竞争导致隔离校验失败（实测两条派单被拒、留下两个孤儿 worktree）。

## Agent skills

### Issue tracker

Issues live as local markdown under `.scratch/<feature>/`. See `docs/agents/issue-tracker.md`.

### Triage labels

Default five-role vocabulary. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context. See `docs/agents/domain.md`.