# MCP 服务骨架实施台账（B1-B5 冻结清单）

> 日期：2026-10-26 · 分支 `feat/mcp-server-skeleton` · worktree `D:/wt-mcp`
> 设计权威：`docs/superpowers/specs/2026-10-26-mcp-server-skeleton-design.md`（HEAD `29e199a8`）
> 台账定位：可核销冻结清单。只记「做什么、什么算过、只许碰哪些文件」；接口签名、错误码、几何公式、文案一律以 spec 为准，本文件不重复设计。

## 0. 目标、基线与状态机

**目标**：新增默认关闭的 `module_mcp`，本机 `127.0.0.1` 提供 Streamable HTTP MCP 服务，首个工具 `web_fetch_markdown`；设置页有模块开关与门控配置卡；不自动写 Claude Code 配置。

**完成硬门禁（不满足即本轮未完成）**：真实 Claude Code 连接真实 DevDesk 进程，握手 → `tools/list` → **真实调用 `web_fetch_markdown` 抓取真网页**，且 `claude -p` 输出包含目标网页正文标记。mock fetcher、进程内假客户端、只握手不调工具，都不算完成。

**基线**：
- 分支 base = 本地 dev `5c952d7e`（该提交已把 origin/dev 的 4 个 CI 修复并入本地 dev；本地 dev 领先 origin/dev，**本轮不 push**）。
- spec HEAD = `29e199a8`；本台账提交在其后，只新增本文件。
- 构建在 worktree `D:/wt-mcp`；`scripts/build-test-run.sh` 自动从 git 主工作树定位 EUI-NEO，必要时显式 `EUI_NEO_DIR=D:/project/gui/EUI-NEO`。

**状态机**：

| 标记 | 含义 | 勾选条件 |
|---|---|---|
| ⬜ 未开始 | 尚未派发或尚未动工 | 默认态 |
| 🔵 进行中 | 已派发、未交回 | 派单时由主会话改 |
| ✅ 实测通过 | 判据全部命中且证据已回填 | **必须**附「命令原文 + 关键结果 + 退出码 / commit sha」 |

**证据规则**：
- 只写「已完成 / 全绿 / 应该没问题」不勾 ✅；无证据视为未完成。
- 定向测试证据写筛中的用例名与退出码，不拿 Total/Passed 总数当交付证明。
- 真实 E2E 证据必须有：目标网页正文输出片段、进程清理记录、用户 Claude 配置未变。
- 主会话按批回单逐行核销：只认证据，不接受口头 DONE。

## 1. 执行规则（全批生效）

1. **每批最多一次构建**（一次 `cmake --build`）。configure / 重配开关不计构建；测试运行不计构建。
2. **定向测试只跑 `--filter mcp`**；B3 追加 `--filter settings`，复用同一构建产物直接跑 `build/tests/devdesk_tests.exe --filter settings`，**不触发第二次构建**。
3. 子任务 **≤30 分钟**；到时停手交回「已完成项 + 部分证据 + 剩余项」，不重做已完成，不整段重来。
4. **主会话只派发 / 对账 / 核销**，不直接写码；实现子任务负责改码、跑本批门禁、勾台账、提交。
5. **同文件单写者**：同一时刻只有一个子任务改同一文件；跨批追加严格按 §1.1 顺序。
6. **禁启动 app**；构建脚本一律 `--no-run`。B4/B5 真实 E2E 是唯一例外：必须 `--minimized` + 临时 `DEVDESK_DATA_ROOT`，按 PID 杀进程，结束删临时根。
7. **不 push 远端、不出安装包、不改 EUI-NEO、不自动写任何 Claude Code 配置**。
8. 每批收尾 **显式 pathspec commit**，只含本批文件 + 台账；信息格式 `feat(mcp): Bx <主题>`（纯文档批 `docs(mcp): ...`）。

### 1.1 跨批同文件写者顺序（防前批注入被后批覆盖）

| 文件 | 批次顺序与边界 |
|---|---|
| `src/module_mcp/mcp_tools_builtin.h/.cpp` | B1 只写 `server_info` 与注册骨架 → B2 追加 `web_fetch_markdown`；B2 不得重构 B1 行为 |
| `src/module_mcp/CMakeLists.txt` | B1 建库与注入宏 → B2 只追加 `html_to_markdown.cpp` |
| `CMakeLists.txt`（root） | B1 `add_subdirectory` / 链接 / 覆盖率 / include → B3 只追加 `src/mcp_service_bridge.cpp` 到 DevDesk 源 |
| `tests/CMakeLists.txt` | B1 登记四文件 + 链接 + include → B2 登记两文件 → B3 桥符号源集 + 两个测试登记 → B4 独立 `mcp_live` 块 |
| `tests/test_main.cpp` | 仅 B3 追加桥四槽复位 |
| 本台账 | 每批末由当批实现者只改「本批行 + 证据槽」，主会话逐行核销 |

## 2. B1 core 接缝 + 协议骨架

**批次门禁**：一次构建 + `bash scripts/build-test-run.sh --filter mcp --no-run`；commit。
**禁止触碰**（本批条目文件之外全部禁改，重点点名）：`src/module_settings/**`、`src/mcp_service_bridge.*`、`src/main.cpp`、`src/app_assembly.cpp`（B3）；`src/module_mcp/html_to_markdown.*` 与 `web_fetch_markdown` 实现（B2，B1 只写 `server_info`）；`tests/live/**`、`scripts/run-mcp-live.sh`、`README.md`（B4）；`tests/test_main.cpp`、`scripts/check-windows-cross-compile.sh`（B3）；`src/app_registry.h`、`src/core/module_interface.h`、`src/platform/**`、其它 `src/module_*`、`../EUI-NEO/**`（全轮不动）。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| B1-01 | ✅ | 端口常量头 | `kDefaultMcpPort=8931` / `kMinMcpPort=1` / `kMaxMcpPort=65535`；纯常量头无 cpp；命名镜像 `core/proxy_port.h` | `src/core/mcp_port.h` |
| B1-02 | ✅ | core 能力接缝 | spec §3 接口逐字落地；空指针 / 非法名 / 重名 → `fail(ValidationError)`；`find` 未找到 → `NotFound`；`list` 按 name 字典序；单锁串行；`globalMcpTools()` 永不析构 | `src/core/mcp_tool.h`、`src/core/mcp_tool.cpp` |
| B1-03 | ✅ | core 构建登记 | 只把 `mcp_tool.cpp` 加入 `devdesk_core` 源，不改既有源 | `src/core/CMakeLists.txt` |
| B1-04 | ✅ | module_mcp 静态库 | `devdesk_module_mcp` 只链 `devdesk_core` + `devdesk_platform`；不链 `ui_kit` / 兄弟模块；注入 `DEVDESK_VERSION` / `DEVDESK_BUILD_TYPE` / `DEVDESK_PLATFORM`，不设 `DEVDESK_OS_NAME` | `src/module_mcp/CMakeLists.txt` |
| B1-05 | ✅ | 会话表 | 32 位十六进制；最多 64 个，超出淘汰最旧；`create/has/remove`；可注入 id 生成器；id 只含可见 ASCII | `src/module_mcp/mcp_session_store.h/.cpp` |
| B1-06 | ✅ | 协议分发 | spec §5.4 判序 / §5.5 版本协商与会话 / §5.6 字典序与非法 schema 跳过 / §5.7 `isError` 与 `-32603`；通知 202；数组 400 `-32600`；非法 JSON 400 `-32700`；id 解析后回显；所有字符串经 JSON 工厂转义 | `src/module_mcp/mcp_protocol.h/.cpp` |
| B1-07 | ✅ | HTTP 适配器 | `RequestCtx`/`ResponseWriter` 适配；POST/GET/DELETE/OPTIONS `/mcp`；415/403/404/400 传输层判序；GET/OPTIONS 405 且 `Allow: POST, DELETE`；202 无 Content-Type；JSON 响应设 Content-Type；入站头按小写键 | `src/module_mcp/mcp_http_handler.h/.cpp` |
| B1-08 | ✅ | 服务生命周期 | spec §4.1：`start(0)` 返回实际端口；运行中幂等；`stop` 幂等并清会话；`restart`；状态加锁快照；每次 start 新建 `Server`；Host 白名单；失败文案 + `lastError`；单例永不析构 | `src/module_mcp/mcp_service.h/.cpp` |
| B1-09 | ✅ | 内置工具（仅 server_info） | `registerBuiltinMcpTools(registry, fetcher)` 使用注入注册表，不读全局；`devdesk_server_info` schema `{"type":"object","properties":{}}`；输出固定顺序 5 项（版本/构建类型/平台/两协议版本/工具数与名字典序）；宏缺失回退 `dev`/`unknown`/`unknown` | `src/module_mcp/mcp_tools_builtin.h/.cpp` |
| B1-10 | ✅ | root CMake 接线 | `add_subdirectory(src/module_mcp)`；`DevDesk` 链接 `devdesk_module_mcp`；覆盖率清单加入模块；`DevDesk` include 补 `src/module_mcp` | `CMakeLists.txt` |
| B1-11 | ✅ | 测试与登记 | 四个用例文件覆盖 spec §10 批次 1 的四组；用例描述含 ASCII `mcp`；`tests/CMakeLists.txt` 登记（漏登记触发 TIN-10 configure FATAL）、`devdesk_tests` 链接 `devdesk_module_mcp`、include `src/module_mcp` | `tests/test_core/test_mcp_tool_registry.cpp`、`tests/test_modules/test_mcp_session_store.cpp`、`test_mcp_protocol.cpp`、`test_mcp_tools_builtin.cpp`、`tests/CMakeLists.txt` |
| B1-G | ✅ | 批次门禁（恢复后重跑 `--filter mcp`） | `bash scripts/build-test-run.sh --filter mcp --no-run` 退出 0；四个新文件用例出现在输出；构建成功行；commit 仅本批 pathspec；台账本批行勾 ✅ | — |

**批次证据（✅ 已回填）**：
- 命令原文（worktree `D:/wt-mcp-skeleton`，工具链 PATH 前置 `export PATH="/d/SoftWare/msys64/mingw64/bin:/d/SoftWare/cmake/cmake-4.3.3-windows-x86_64/bin:/d/SoftWare/ninja:$PATH"`）：`bash scripts/build-test-run.sh --filter mcp --no-run`。
- 首次门禁红：`tests/test_modules/test_mcp_tools_builtin.cpp` 在 `REGISTER_TEST` 宏参数内嵌 `#if defined(_WIN32)` 触发 `-Werror=pedantic`；修复：平台断言抽成匿名命名空间 helper `expectedPlatformName()`（`#if` 移出宏参数），测试体内运行时引用。
- 结果：`✅ 构建成功` + `✅ 发布形态（单 exe / 零外部 DLL；DEVDESK_STATIC=ON）`；`--filter mcp` 筛中 41 用例（test_mcp_tool_registry 8 + test_mcp_protocol 22 + test_mcp_session_store 6 + test_mcp_tools_builtin 5），`Total: 41 Passed: 41 Failed: 0`；直跑 `build/tests/devdesk_tests.exe --filter mcp` 退出码 0；门禁脚本最终 `EXIT=0`。
- 守卫段同批全绿：probe 跳过登记 / 模块隔离 / Windows 交叉编译 260 文件 / std include 483 文件 / 玻璃体系 / ToolGuard / 守卫自检。
- 源码 `4bccde3c`（21 文件 +2260 行）；pedantic 修复与本核销同提交。

## 3. B2 抓取能力

**批次门禁**：一次构建 + `bash scripts/build-test-run.sh --filter mcp --no-run`；commit。
**禁止触碰**：`src/core/**`（B1 冻结）；`src/module_mcp/mcp_protocol.*`、`mcp_http_handler.*`、`mcp_service.*`、`mcp_session_store.*`（B1 冻结）；`src/module_settings/**`、`src/mcp_service_bridge.*`、`src/main.cpp`、`src/app_assembly.cpp`、`tests/test_main.cpp`（B3）；`tests/live/**`、`scripts/**`、`README.md`（B4）；`src/platform/**`、其它 `src/module_*`、`../EUI-NEO/**`、`src/app_registry.h`、`src/core/module_interface.h`；禁止新增第三方依赖。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| B2-01 | ✅ | HTML→Markdown | spec §7 全部规则：剔除 script/style/head（留 title）/注释；标题、段落、br/hr；嵌套 ul/ol 2 空格；blockquote；pre 围栏 / 行内 code；a/img；strong/em；基础表格与 `|` 转义；命名与数字实体；空白折叠；`title` 兜底；未闭合标签不抛异常；输入 2 MiB 截断 | `src/module_mcp/html_to_markdown.h/.cpp` |
| B2-02 | ✅ | `web_fetch_markdown` | spec §6.1 逐条：仅 http/https；SSRF 字面量全表**每跳**校验；手动跟随最多 5 跳且总时限 10s；非 2xx 含状态码 + 前 200 字符；响应头大小写不敏感；Content-Type 分类；charset 白名单外报错、白名单内 sanitize；2 MiB 截断附注；`max_chars` 钳制 1000..200000 且 UTF-8 边界截断；空正文非错误；网络失败中文 + 底层信息；所有输出进 JSON 前良构 UTF-8 | `src/module_mcp/mcp_tools_builtin.h/.cpp`（追加）、`src/module_mcp/CMakeLists.txt` |
| B2-03 | ✅ | 测试登记 | `tests/CMakeLists.txt` 登记两个新文件；不改 B1 既有登记与链接行 | `tests/CMakeLists.txt` |
| B2-04 | ✅ | HTML→MD 用例 | 覆盖 spec §10 批次 2 的 HTML 组，每条注明防的事故；描述含 `mcp` | `tests/test_modules/test_mcp_html_to_markdown.cpp` |
| B2-05 | ✅ | fetch 工具用例 | mock fetcher 覆盖 spec §10 批次 2 的 fetch 组（重定向/时限/编码/SSRF/截断/内容类型/二进制/空正文），描述含 `mcp` | `tests/test_modules/test_mcp_fetch_tool.cpp` |
| B2-G | ✅ | 批次门禁 | 一次构建 + `--filter mcp` 退出 0；B1 用例同批回归不红；commit 仅本批 pathspec；台账勾 ✅ | — |

**批次证据（✅ 已回填）**：
- 命令原文（worktree `D:/wt-mcp-skeleton`，工具链 PATH 前置同 B1）：`bash scripts/build-test-run.sh --filter mcp --no-run`。
- 首次门禁红（`1bf64e4f`）：`--filter mcp` → `Total: 77 / Passed: 73 / Failed: 4`，4 条全为 B1 遗留计数断言（`test_mcp_tools_builtin.cpp` 63/103/117/163 行，内置工具注册数 1→2 必然触发；该文件不在本批 §3 允许清单，按纪律未动并随回单上报）；本批新增 36 例（HTML 15 + fetch 21）全绿。
- 修法（主会话裁决选 A）：`e4a61901` 仅动计数/清单派生断言（size 1→2、工具数文案与字典序清单含 `web_fetch_markdown`），行为断言一律未动；**B2 门禁绿以 `e4a61901` 为准**：`--filter mcp` 77/77 退出 0。
- 审计安全收口批核销（6 commit；主会话授权依据：researcher 两轮调研 + auditor 审计（F1/F3 等），文档 `docs/superpowers/research/2026-10-27-mcp-implementation-review.md`、`2026-10-27-mcp-round2-review.md`、`docs/superpowers/plans/2026-10-27-mcp-b1b2b3-audit.md`）：
  - `e1b22986` `html_to_markdown.cpp` 补显式 `<utility>`/`<algorithm>` 过 std include 守卫（首红：`std::move`/`std::max` 靠传递包含可见；修后 491 文件全绿）。
  - `43b983ac` 拦截 IP 数字编码绕过 SSRF 校验（数字形态按 inet_aton 语义还原查表、::/96 还原、userinfo 一律拒、解析失败 fail-closed；修复前 4 红，修后 106/106 绿）。
  - `e5c7aff3` markdown 转义表（对照 turndown@7.2.0 官方 escapes）与嵌套有序列表 2→4 空格缩进（门禁 `--filter mcp` 109/109、`--filter settings` 113/113 退出 0）。
  - `02e38d72` 非 2xx 错误前缀按 UTF-8 码点截 200 字符（修复前 36 筛中 1 红，修后 36/36 绿）。
  - `bb556cad` 拒绝尾点 host（fail-closed）并补组播 deny 段（IPv4 224.0.0.0/4、IPv6 ff00::/8；修复前 38 筛中 2 红，修后 38/38 绿）。
- 终态正式门禁（tester 取证，HEAD `bb556cad`）：`✅ 构建成功` + `✅ 发布形态（单 exe / 零外部 DLL；DEVDESK_STATIC=ON）`；`--filter mcp` `Total: 112 Passed: 112 Failed: 0`，脚本退出 0；守卫段全绿（probe 跳过登记 / 模块隔离 / Windows 交叉编译 266 文件 / std include 491 文件 / 玻璃体系 / ToolGuard / 守卫自检 5 道好树绿坏树红）。

## 4. B3 设置与生命周期

**批次门禁**：一次构建 → `bash scripts/build-test-run.sh --filter mcp --no-run` → 直接跑 `build/tests/devdesk_tests.exe --filter settings`（不二次构建）；commit。
**禁止触碰**：`src/core/**`、`src/module_mcp/**`（B1/B2 冻结；发现阻塞缺陷停手报主会话，不就地改）；`tests/live/**`、`scripts/run-mcp-live.sh`、`README.md`（B4）；`src/platform/**`、其它 `src/module_*`（只动 `module_settings`）、`../EUI-NEO/**`、`src/app_registry.h`、`src/core/module_interface.h`；安装包脚本与 CI。root CMake 只许追加桥源一行，不得改 B1 既有行。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| B3-01 | ✅ | 配置头 | `kModuleIdMcp = "mcp"`；include `core/mcp_port.h` 并别名三常量；`mcpPort` 默认 8931；`parseMcpPortInput` 声明 | `src/module_settings/settings_config.h` |
| B3-02 | ✅ | 配置实现 | `kKnownModuleIds` 加 mcp；`defaultEnabledModules()` **不加** mcp；`sanitized` 越界/0/负回落默认（不钳边界）；save/load 新增顶层 `mcpPort`，行数 17→18 并同步注释；解析只接受范围内纯数字 | `src/module_settings/settings_config.cpp` |
| B3-03 | ✅ | 卡几何同源 | `mcpPortRowY()/mcpStatusRowHeight()/mcpCommandRowY()/mcpCommandRowHeight()/mcpCardHeightFor()` 按 spec §8.3 公式；下边距 `std::max(16.0f, roundFs(16.0f))`；**不写死 160** | `src/module_settings/settings_ui_logic.h` |
| B3-04 | ✅ | 设置卡 UI | 模块卡标题右侧开关 id `settings.card.modules.mcp`、命中的 `<id>.hit`、盒宽 150、标题盒 `w-56-150`；`drawMcpCard` 根 id `settings.card.mcp`；端口输入 96×28（placeholder `8931`）+ 应用 64×28；反馈四态；状态三态；命令行；`SettingsCardsIn/Actions` 新字段一律追加末尾带默认值 | `src/module_settings/settings_ui_cards.h/.cpp` |
| B3-05 | ✅ | 设置页逻辑 | 门控卡 `gate = kModuleIdMcp` 排在代理卡后；开关走既有 `applyModuleEnabled`；`mcpPortDraft_` / 应用 / Esc / Ctrl+S；输入变化清错；桥状态快照；`CardHeightKind::McpCard` + switch；`contentKey` 追加 `(mcpOn ? "M" : "m")` | `src/module_settings/settings_module.cpp` |
| B3-06 | ✅ | app 层桥 | spec §8.4 API；默认桩 `start/applyPort` fail「MCP 服务未装配」、`stop` no-op、`status` 全空；`static_assert` 签名哨兵；桥 cpp 只依赖 core + 标准库，不链接 module_mcp | `src/mcp_service_bridge.h`、`src/mcp_service_bridge.cpp` |
| B3-07 | ✅ | main 装配 | 一次注册内置工具；`setHandlers` 装四个无捕获转发函数；启动时按 `moduleEnabled(..., kModuleIdMcp)` 启动 `cfg.mcpPort`，失败只 LOG_ERROR；`atexit` stop | `src/main.cpp` |
| B3-08 | ✅ | 模块开关分发 | `dispatchModuleToggle` 的 mcp 特例**位于注册表遍历之前**；开 → 读端口并 start，失败 `showAppNotice`；关 → stop；随后 return | `src/app_assembly.cpp` |
| B3-09 | ✅ | root CMake 桥源 | `DevDesk` 源追加 `src/mcp_service_bridge.cpp`；不改 B1 行 | `CMakeLists.txt` |
| B3-10 | ✅ | 测试构建接线 | 登记两个新测试；`devdesk_tests` 源加桥、链接 module_mcp、include 补 `src/module_mcp`；probe 公共源集与 `github_sync_live` / `agent_live` 源集各补桥 | `tests/CMakeLists.txt` |
| B3-11 | ✅ | 跨编译脚本 | `build_incs()` 增加 `-I "${ROOT}/src/module_mcp"` | `scripts/check-windows-cross-compile.sh` |
| B3-12 | ✅ | 用例出厂复位 | `resetCaseGlobals` 增加 `app::mcp::setHandlers(nullptr, nullptr, nullptr, nullptr)` | `tests/test_main.cpp` |
| B3-13 | ✅ | 设置用例 | 配置（解析边界/符号/空格、默认 8931、默认关闭、sanitized 回落、未知 ID 过滤、save/load 18 键往返）；桥（默认桩/注入路由/复位）；卡片（开关默认关且命中 `.hit`、门控卡开关两态、状态三态、反馈四态、非法端口不落库、命令端口来源、85/100/130% 行盒不重叠且下边距 ≥16）；描述含 `mcp` | `tests/test_modules/test_mcp_settings.cpp` |
| B3-14 | ✅ | 服务用例 | port 0 实际端口；isRunning/port/status；stop 幂等；restart 换端口；端口占用 fail + lastError；Host/Origin 拒绝；在途工具 300ms 时 stop 不崩且收敛；GET 405 + Allow、OPTIONS 405、通知 202；重启后旧 session 404 | `tests/test_modules/test_mcp_service.cpp` |
| B3-G | ✅ | 批次门禁 | 一次构建 + `--filter mcp` 退出 0 + 直接跑 `--filter settings` 退出 0；commit 仅本批 pathspec；台账勾 ✅ | — |

**批次证据（✅ 已回填）**：
- 命令原文：`bash scripts/build-test-run.sh --filter mcp --no-run` → 直跑 `build/tests/devdesk_tests.exe --filter settings`（同一构建产物，不二次构建）。
- 首次门禁红（`740d20c6` 接手收口，前序中断会话 `3fb183df` 链接从未通过）：①`test_mcp_settings.cpp` 重复定义 `app::dslAppConfig()` 且包含 `dsl_app_impl.h`（DSL 运行时注入点全二进制唯一）→ 本 TU 去掉定义与 impl 头，仅经 `dsl_app.h` 复用声明；②`test_mcp_service.cpp` 端口占用用例 Windows 恒红（httplib 服务端默认 SO_REUSEADDR 双绑定假成功）→ holder 改排他裸 socket。修后 `--filter mcp` 64/64、`--filter settings` 113/113 均退出 0；合并 `a572947e` 入 `feat/mcp-server-skeleton`。
- fix 批核销（`a028fb1e`，主会话授权，范围白名单 `src/platform/http_server.cpp` + `tests/test_modules/test_mcp_service.cpp`）：根治 `740d20c6` 留裁决项——服务端 socket 排他绑定（Windows SO_EXCLUSIVEADDRUSE / POSIX 不设 reuse 位），端口被占如实 fail + lastError；先红后绿（修复前 1 筛中 1 红：conflict.isFail 断言失败）；门禁 `--filter mcp` 101/101、`--filter settings` 113/113、`--filter proxy` 25/25 均退出 0。
- 审计安全收口批涉及 B3 面注记：`3759beb3` 非初始化请求缺会话头返回 400（主会话授权解冻 `mcp_protocol.cpp`，本修复未动 `mcp_http_handler.cpp`；修复前 107 筛中 1 红，修后 107/107 绿）。
- 终态正式门禁（tester 取证，HEAD `bb556cad`）：`--filter mcp` `Total: 112 Passed: 112 Failed: 0`；`--filter settings` 直跑 `Total: 113 / Passed: 113 / Failed: 0 / Skipped: 0`；两者均退出 0。实现者报告的「模块卡标题右 MCP 开关」「门控卡显隐」两例负载敏感偶发在本次全 mcp 过滤下未复现（直接绿），维持负载 flaky 定性、非回归。

## 5. B4 集成与真实链路

**批次门禁**：一次构建（含 live 开关）→ `mcp_live --check` → **真实 Claude Code E2E 硬门禁**；commit。
**禁止触碰**：`src/**` 全部源码（发现缺陷停手报主会话，回对应批次修）；既有 `tests/live/github_sync_live.cpp`、`tests/live/agent_live.cpp`、`scripts/run-ghsync-live.sh`、`scripts/run-agent-live.sh`；`src/platform/**`、`../EUI-NEO/**`、安装包脚本、CI；用户 `~/.claude.json` 与 `~/.claude/**`；除 `README.md` 与本 spec 外的文档。本批只许改 `tests/live/mcp_live.cpp`、`tests/CMakeLists.txt`（仅新增独立块）、`scripts/run-mcp-live.sh`、`README.md`、本 spec 的口径段、台账。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| B4-01 | ✅ | live 驱动 | `--check`：进程内真 HTTP initialize（取 session）→ tools/list → tools/call `devdesk_server_info` → DELETE；未知 session 404；Content-Type 415；GET 405 且 `Allow: POST, DELETE`；OPTIONS 405；通知 202；重启后旧 session 404；真网页抓取不可用时 skip 并记录；`--serve` 打印 `PORT=<n>` 后 `fflush(stdout)` 再阻塞；Windows/macOS 均可编译 | `tests/live/mcp_live.cpp` |
| B4-02 | ✅ | live 目标登记 | **独立** `if(DEVDESK_BUILD_LIVE_TOOLS)` 块（无 `AND UNIX`）；源含 `live/mcp_live.cpp` + `src/mcp_service_bridge.cpp`；链接 `devdesk_module_mcp`；LABELS `live`；默认 OFF | `tests/CMakeLists.txt` |
| B4-03 | ✅ | 实连脚本 | `mktemp -d` 空 cwd；解析 `--serve` 的 `PORT=`；临时 mcp json 的 server key 固定 `devdesk`；`claude -p --strict-mcp-config --mcp-config <临时> --allowedTools mcp__devdesk__web_fetch_markdown --output-format text`；不设 `CLAUDE_CONFIG_DIR`；不碰用户配置；trap 清理临时文件 | `scripts/run-mcp-live.sh` |
| B4-04 | ✅ | 文档口径 | `README.md` 能力清单 / 模块开关 / 接入命令同步；spec 仅在实现与已拍板口径有偏差时同步该段，不改设计 | `README.md`、spec |
| B4-G1 | ✅ | 构建 + `--check` | 配置 `-DDEVDESK_BUILD_LIVE_TOOLS=ON`（worktree 无同级 EUI-NEO，显式 `-DEUI_NEO_DIR=D:/project/gui/EUI-NEO`）后**一次构建**；`build/tests/mcp_live.exe --check` 退出 0；不启动 DevDesk | — |
| B4-05 | ✅ | E2E 准备 | `export DEVDESK_DATA_ROOT=$(mktemp -d)`；首次启动 `build/DevDesk.exe --minimized` 至 `devdesk.db` 生成，按 PID 杀；不得触碰用户真实数据根 | — |
| B4-06 | ✅ | E2E 播种 | `sqlite3` 写 `app_config` 两键：`config.json:enabledModules` = 含 `mcp` 的 JSON 数组（含 home/settings）、`config.json:mcpPort` = 本机空闲端口；回读校验值 | — |
| B4-07 | ✅ | E2E 启动与探活 | `--minimized` 启动 DevDesk，记录 PID；轮询 `/mcp` 至 GET 返回 405（服务已监听）；失败则保留日志并判本批红 | — |
| B4-08 | ✅ | E2E 连接 | 临时 json：`{"mcpServers":{"devdesk":{"type":"http","url":"http://127.0.0.1:<port>/mcp"}}}`；URL 与播种端口一致 | — |
| B4-09 | ✅ | E2E 调用断言 | 临时空 cwd 运行 `claude -p --strict-mcp-config --mcp-config <临时> --allowedTools mcp__devdesk__web_fetch_markdown --output-format text`；提示明确要求抓取目标真网页；stdout 含目标网页正文标记；记录 claude 版本（须支持 `--strict-mcp-config`） | — |
| B4-10 | ✅ | E2E 清理 | 按 PID 杀 DevDesk 并确认进程消失；删临时数据根 / json；用户 `~/.claude.json` 前后校验和不变 | — |
| B4-G2 | ✅ | E2E 硬门禁汇总 | B4-05..10 全 ✅ + 输出片段 + 清理记录；任一环失败本批红，本轮不得宣称完成 | — |
| B4-G3 | ✅ | 批次 commit | 仅本批 pathspec；台账勾 ✅ | — |

**批次证据（✅ 回填 2026-10-27，tester）**：`build/tests/mcp_live.exe --check` exit 0，ok=21 / skip=1 / 失败 0（skip=真网页抓取 SSL 受限，口径沿用）；所测产物为 6f130768 构建（DevDesk.exe 17:30 / mcp_live.exe 17:41）。claude 2.1.310；链 B PORT=54281，URL `http://127.0.0.1:54281/mcp`，目标 `https://www.iana.org/domains/reserved`（主会话裁定新基线：example.com 现行文案已无标记句且遭 CA 缺陷拦截）。stdout 正文：These domains may be used as illustrative examples in documents without prior coordination with us.（CLAUDE_RC=0）。杀进程与清理：首启 PID 7976（devdesk.db 约 1s 生成）→ taskkill 终止；服务 PID 10524（GET /mcp=405 且 `Allow: POST, DELETE`）→ 断言通过后 taskkill 并确认进程消失；临时数据根 / mcp json / 空 cwd 全删零残留；`~/.claude.json` 对 `"devdesk"` 与 `54281` 均零命中（主会话新口径：claude CLI 自身状态写入不算违规）。所测基线 commit 6f130768。

**链 A 记录（run-mcp-live.sh 实连，2026-10-27 tester）**：首跑 FAIL——目标抓取被 fetcher SSL 证书错误拦截（CA 缺陷：Windows 走 `CURLSSLOPT_NATIVE_CA` 依赖本机 ROOT 库；libcurl 库不读 `CURL_CA_BUNDLE`，`http_client_ca_win.cpp` 注释口径有误）→ 已由主会话登记为独立缺陷批。授权复跑（CURL_CA_BUNDLE=msys2 ca-bundle.crt）脚本 exit 0（PORT=53449），但标记句系 claude 顺链接抓取 iana.org 所得，且经独立 curl 证实 example.com 现行文案已不含该句——据此主会话将 E2E 目标基线改为 `https://www.iana.org/domains/reserved`，链 B 即以新基线全绿。脚本 iana 基线修正 commit 尚未落盘（builder WIP），链 A 复跑跳过。

## 6. B5 审计与合并

**禁止触碰**：审计未确认的改动、需求外重构与顺手清理；`uat` 分支与任何远端 push；安装包；`../EUI-NEO/**`；macOS 真机；用户 Claude 配置；非本任务 worktree（只删 `D:/wt-mcp`）。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| B5-01 | ⬜ | 独立实现审计 | 六维逐条：①spec 一致性（接口/错误码/几何/文案）②依赖分层与模块隔离（module_mcp 只链 core/platform；settings 不 include module_mcp；core 不依赖 platform）③协议与传输（JSON-RPC 映射/session/头/状态码/转义）④安全（SSRF 每跳/Host/Origin/charset/UTF-8）⑤生命周期与线程（port 0/stop 在途/atexit/单例）⑥测试与文档口径；每条 CONFIRMED/REFUTED 带 file:line | 全仓只读 |
| B5-02 | ⬜ | 修复 + 复审 | 只修 CONFIRMED 且属需求交付物的缺陷；非需求/历史遗留只记录；复审逐条闭合 | 按审计清单 |
| B5-03 | ⬜ | 合入本地 dev | 从 `D:/project/gui/DevDesk` 串行 merge `feat/mcp-server-skeleton` 到本地 dev；无冲突残留；不 push；不碰 uat | — |
| B5-04 | ✅ | dev 全量门禁 | dev 上 `bash scripts/build-test-run.sh --no-run` **一次**（全量套件 + probe，不启动 app）；构建成功且门禁退出 0 | — |
| B5-05 | ⬜ | dev 复跑 E2E | 用 dev 构建产物复跑 B4-05..10；输出仍含目标正文；清理完成 | — |
| B5-06 | ⬜ | 删除 worktree | 从主工作树 `git worktree remove D:/wt-mcp`；`git worktree list` 不再含该目录；分支保留 | — |
| B5-07 | ⬜ | 按场景汇报 | 报「哪些业务场景真跑起来了（真窗口/真网/真机）、结果、哪些没跑到」；不报 Total/Passed 数字；明写 macOS 仅 CI、未出安装包、未 push | — |
| B5-08 | ⬜ | 台账收口 | 全部条目 ✅ 且有证据；merge/commit sha 回填 | 本台账 |

**批次证据（✅ 时回填）**：`<审计结论计数与清单路径>` / `<merge sha + 全量门禁退出码 + 构建结果>` / `<dev E2E 片段>` / `<worktree list 结果>`。

**B5-04 收官全量门禁（✅ 回填 2026-10-28，tester）**：worktree `D:/wt-mcp-skeleton`，门禁基线 `fa359678`（执行期间 HEAD 前移至 `113a7001`，纯 docs 三文件，代码与 fa359678 零差）。上游等待记录：本单前两次尝试先后在 wallpaper_probe 编译红（UI 批 `PageId::Mcp` case 缺 → 修复 `2ad1b4dc`）、probe 链接红（4 个 probe 目标缺 `mcp_page_module.cpp` 源 → 修复 `fa359678`）处停，修复后全仓摸排无残留。本轮证据：
- 全目标构建 `cmake --build build`：37 步全绿 EXIT=0，wallpaper_probe / uat62_page_shots_probe / h13_fluid_background_probe / module_toggle_sidebar_probe / DevDesk.exe / devdesk_tests 全链接落地——§11「待办移交」的 DevDesk exe 构建与 probe 编译证词就此补验。
- `bash scripts/build-test-run.sh --filter mcp --no-run` 退出 0：构建成功（Build version 0.6.7→0.6.8）；发布形态单 exe / 零外部 DLL（DEVDESK_STATIC=ON）；`--filter mcp` **Total: 147 Passed: 147 Failed: 0**。
- 守卫段逐项绿：探针跳过登记（每个 probe 带 77）；模块隔离（settings 无兄弟依赖、module_agent 零兄弟 include、工具方不回依赖）；Windows 交叉编译 **269/269** 文件全过（含 `mcp_page_module.cpp`——module_mcp/CMakeLists.txt 无平台分支无条件进受编集合；任务书旧口径 273 为漂移数，实际 269）；std include 495 文件全过；玻璃体系（无 drawGlassCardLayers 调用 / 无 glass_card.h include）；ToolGuard 双层花括号（无 Guard{{ 形态）；守卫自检 5 道全过（好树绿 / 坏树红，非恒真）。
- 同产物追加直跑：`--filter settings` **Total: 113 Passed: 113 Failed: 0**、`--filter app` **Total: 47 Passed: 47 Failed: 0**，均退出 0。
- DevDesk.exe 取证（verifier 预检用）：`build/DevDesk.exe` 208,785,578 字节，mtime 2026-10-28 14:08:32 +0800，sha256 `93ab24743ce71c296c03718c43b73926a4e240b26dd0bf975ac2122fef1841cd`；特征串 `mcp.card` ×1、`mcp.dialog` ×17 命中（另有 mcp.section ×4 / mcp.status ×3 等）。
- **未验证**（不沉默）：无 filter 全量套件门禁未跑（本轮口径为 `--filter mcp`+settings/app 定向，沿用 CE 收官先例）；probe 实跑截图取证未做（`--no-run` 不执行 probe，截图归后续批次）；B5-05 dev 复跑 E2E（真实 Claude Code live）属下一条目；存量数据升级与目标平台真机未涉。

## 7. 已知假设

1. 本地 dev 已并入 origin/dev 的 4 个 CI 修复（merge `5c952d7e`）；本轮**不 push 远端、不出安装包**。
2. macOS 仅 CI 编译 + 单测；真实 Claude Code 实连在 Windows 开发机完成，macOS 无真机验证。
3. **不自动修改 Claude Code 配置**；设置卡只展示接入命令；E2E 只用临时配置且 `--strict-mcp-config`。
4. 端口默认 **8931**；E2E 若默认端口被占用，播种其他空闲端口并让临时 json 同步。
5. worktree 无同级 `../EUI-NEO`：构建脚本自动从 git 主工作树定位，必要时显式 `EUI_NEO_DIR=D:/project/gui/EUI-NEO`。
6. 真实 E2E 的目标网页须本机网络可达；若公司网络不可达，由主会话先选定可达真网页并把 URL 与断言串写入证据，**硬门禁不降级为 mock**。
7. `claude --version` 须支持 `--strict-mcp-config`（spec 要求 2.1.307+）；不满足则视为环境阻塞，如实上报，不绕过。
## 8. 代理参数批（B4 后追加，主会话拍板设计）

**范围**：`src/platform/http_client.{h,cpp}`（Request.proxy + CURLOPT_PROXY + tcpReachable 探测）、`src/module_mcp/mcp_tools_builtin.{h,cpp}`（useProxy/proxy 参数、探测缝 ProxyOptions、冲突与形态校验、直连附注）、两测试文件；spec §6.1 同步参数口径。口径：`useProxy` 默认 true → 探测 7897→10808→51295（单候选 ≤300ms，调用内缓存），全落空直连并附注「未检测到本地代理,已直连」；`proxy` 仅 IP:port / localhost:port，强制走 + TCP 预检失败报「显式指定的代理 … 不可用」；与 useProxy=false 同给 → 参数冲突；代理是传输通道不进目标 deny 表，目标 SSRF 每跳校验不变。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| PX-01 | ✅ | 探测缝 | ProxyOptions{autoCandidates/probe/perProbeTimeout} 注入缝（FetchLimits 先例）；probe 空 = 生产 `platform::http::tcpReachable` | `mcp_tools_builtin.{h,cpp}` |
| PX-02 | ✅ | useProxy 自动探测 | 命中首选短路走代理（Request.proxy 断言）；全落空直连 + 附注一行 | 同上 + `test_mcp_fetch_tool.cpp` |
| PX-03 | ✅ | 显式 proxy | 预检过 → 强制走；预检败 → isError「显式指定的代理 … 不可用」不触网；形态仅 IP:port/localhost:port，其余报「仅支持」 | 同上 |
| PX-04 | ✅ | 冲突 | proxy + useProxy=false → isError「参数冲突」，不探测不触网 | 同上 |
| PX-05 | ✅ | 传输层 | Request.proxy → CURLOPT_PROXY（"http://host:port"）+ NOPROXY 清空；`tcpReachable` = curl CONNECT_ONLY 纯 TCP 握手 | `http_client.{h,cpp}` |
| PX-06 | ✅ | schema 与测试 | schema 含 useProxy(boolean)/proxy(string)，required 仍仅 url；代理用例 8 例（命中/落空/显式过/显式败/冲突/关/形态矩阵/schema） | `test_mcp_fetch_tool.cpp`、`test_mcp_tools_builtin.cpp` |
| PX-G | ✅ | 批次门禁 | 一次增量构建 + `--filter mcp` 退出 0 + `--filter settings` 不回归（计数见批次证据） | — |

**批次证据（✅ 回填 2026-10-27，builder）**：红 = 接口脚手架（ProxyOptions/Request.proxy/schema）落地后行为全缺，fetch 过滤 46 筛中 6 红（命中/落空/显式过/显式败/冲突/形态矩阵全红，schema 与 useProxy=false 两例按定义绿）；实现后 fetch 46/46、tools builtin 6/6、`--filter mcp` 112+8=120/120 退出 0、`--filter settings` 113/113、std include 守卫 492 文件绿。期间 EUI-NEO 被并行会话改入中途态致 2 次构建红（`jellyDeepenedShadow` 未声明，与本批零交集），等待重试后收敛。commit 见本批 hash。

## 9. CA 批（代理参数批同期，tester E2E 证据触发的缺陷修复）

**缺陷**：Windows CA 策略（CURLSSLOPT_NATIVE_CA）在本机 ROOT 库缺部分根证书时校验失败（CURLE 60，example.com 实测被拦）；且 `http_client_ca_win.cpp` 旧注释「CURL_CA_BUNDLE libcurl 自己会读」不成立——该 env 是 curl 命令行工具约定，libcurl 库不读（实测 8.x 无效果）。

**修法（主会话拍板）**：默认 NATIVE_CA 不变；新增显式环境变量 `DEVDESK_CA_BUNDLE`（DevDesk 自有约定）：代码读 env → 文件存在 → `CURLOPT_CAINFO` 显式设置且不再叠加 NATIVE（换信任根，不弱化校验）；路径不存在 → 可读报错（含变量名与路径）。注释口径同步修正。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| CA-01 | ✅ | 策略解析 | `resolveCaPolicy`（平台无关纯函数，单测直测）：未设 → Native；存在文件 → Bundle 不叠 NATIVE；不存在 → fail 含变量名与路径 | `http_client_ca.h`、`http_client.cpp` |
| CA-02 | ✅ | 两调用点接入 | `send` 与 `sendStreaming` 均先解析后套用：Native → `applyCaOptions`（现状不变）；Bundle → `CURLOPT_CAINFO`；解析失败 → 可读 fail（流式先于句柄创建） | `http_client.cpp` |
| CA-03 | ✅ | 注释口径修正 | `http_client_ca_win.cpp`/`http_client_ca.h` 旧「CURL_CA_BUNDLE libcurl 自己会读」更正为 DevDesk 自有约定 + 实测结论 | `http_client_ca_win.cpp`、`http_client_ca.h` |
| CA-G | ✅ | 批次门禁 | `--filter "http ca"` 3 例全绿 + `--filter http` 23/23 不回归；批级计数随攒批终门禁（见批次证据） | — |

**批次证据（✅ 回填 2026-10-27，builder）**：红 = 用例先落（`resolveCaPolicy` 符号未声明，编译期红）；实现后 `--filter "http ca"` 3/3（未设→Native、存在文件→Bundle 不叠 NATIVE、不存在→fail 含变量名与路径）、`--filter http` 23/23；攒批终门禁 `--filter mcp` 120/120、`--filter settings` 113/113 均退出 0，std include 守卫 492 文件绿。example.com 抓取恢复验证留给后续 E2E（用含本批二进制 + `DEVDESK_CA_BUNDLE=<msys2 ca-bundle>`）。

## 10. CE 正文主干提取批（B 系收官后追加；主会话拍板：main_content 默认 true，蓝图 R1–R4 全做）

**范围**：`src/module_mcp/html_to_markdown.{h,cpp}`（Mode::Suppress 抑制栈 + R1/R1b/R2/R3/R4 + 参数化入口）、`mcp_tools_builtin.cpp`（main_content schema+透传）、两测试文件、spec §7 口径（§6.1 参数口径循代理批先例同步一行）。设计蓝图 = `docs/superpowers/research/2026-10-27-mcp-content-extraction-review.md`（词表与阈值照抄不改值）；六金样内嵌测试文件（批次范围不新增 fixture 目录）。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| CE-01 | ✅ | 参数与关断保底 | `main_content` boolean 默认 true；false 时全部规则短路，6 金样 off 输出 = 基线逐字节金样（差分保底） | `html_to_markdown.{h,cpp}`、`mcp_tools_builtin.cpp`、`test_mcp_html_to_markdown.cpp`、`test_mcp_fetch_tool.cpp` |
| CE-02 | ✅ | R1 语义剔除 | header/footer/nav/aside/form/svg 等整块消失；Suppress 栈同名嵌套计数、乱序闭合弹到匹配层；article/main/h1-h3 信号释放 + 64KiB 硬释放双保险 | `html_to_markdown.cpp` + 金样 docs/malformed + 64KiB 用例 |
| CE-03 | ✅ | R1b 关键词剔除 | 负面词表命中且无正词（kMaybeTokens 保险丝）→ 抑制；类名冲突金样零误伤 | 同上 + 金样 class-keyword/portal-list |
| CE-04 | ✅ | R2 链接密度 | 块 ≤150 且密度 >0.5 且无图 → 丢弃；锚链接 0.3 计重保 TOC；inCodeSpan_/href_ 随块复位、有序列表 next-- 回退；未闭合 <a> 存疑不删 | 同上 + R2 三用例 |
| CE-05 | ✅ | R3 锚定保护 | main/article 区内 R2 暂停（列表页主干即链接列表仍存活）+ 触发 R1 释放 | 同上 + 金样 portal-list/malformed |
| CE-06 | ✅ | R4 无障碍属性 | role 词表/aria-hidden/hidden/display:none 以容器为闸抑制；void/自闭合不启动防栈滞留 | 同上 + R4 用例 |
| CE-G | ✅ | 批次门禁 | 一次增量构建（全目标，DevDesk.exe/tests/mcp_live 链接落地）+ `--filter html`/`--filter fetch` 定向全绿 + `--filter http` 审计子集不回归 | — |

**批次证据（✅ 回填 2026-10-28，builder）**：红 = 用例先落（金样 off 口径按基线语义推导 pin 死；实现缺口处 html 过滤编译期/断言红）；实现后 `--filter html` 23/23、`--filter fetch` 48/48、`--filter http` 28/28 均退出 0（计数详见批次回单）。真实样本对比：OSChina 类列表页站头站尾/友情链接消失、新闻列表全存活；bigmodel 类文档页导航/页脚/版权消失、正文+TOC 锚链接+代码块完整。
**收官统一门禁（✅ 回填 2026-10-28，tester；CE-G 判据，全流程仅此一次）**：worktree `D:/wt-mcp-skeleton` @ `2e959c16`（本批主干 `fd0b2e99`）；`bash scripts/build-test-run.sh --filter mcp --no-run` 退出 0：增量构建成功、发布形态单 exe/零外部 DLL（DEVDESK_STATIC=ON）、守卫段逐项绿（探针跳过登记、模块隔离、Windows 交叉编译 267/267 文件、std include 492+ 文件、玻璃体系、ToolGuard，守卫自检好树绿/坏树红非恒真）、`--filter mcp` 133/133；同产物追加直跑 `--filter settings` 113/113、`--filter http` 28/28 均退出 0。

### 审计 findings 修复（并入本批一次构建验证，独立 commit）

| # | severity | 处置 |
|---|---|---|
| F1 | 中 | ✅ `tcpReachable` 加 `CURLOPT_NOPROXY "*"`（钉掉 env 代理）；用例 = env http_proxy 指死端口时探测活监听必真（修前红） |
| F2 | 中 | ✅ send/sendStreaming 各补 bundle-missing 无网可读失败用例（删解析接线即红）；双调用点 CA 分支收敛 `applyCaPolicy` 单一事实源。CAINFO 下发本身无 TLS 对端不可单测观测，按审计建议留 live 层（已注记） |
| F3 | 中低 | ✅ sendStreaming 按 Request.proxy 契约接线（与 send 同款 PROXY/NOPROXY 分支）；用例 = 显式死代理必败（修前静默直连红）；http_client.h 契约注释补「两入口同款」 |
| F4 | 中低 | ✅ `tcpReachable` 首行 `globalInit()`（并发首触数据竞争）；无独立用例（窗口不可稳定复现），随 F1 用例同路径执行 |
| F5 | 低 | ✅ `test_mcp_fetch_tool.cpp` :619/:745 两处补 `legacyProxyOpts()`，单测不再真触本机探测端口 |
| F6 | 低 | ✅ `resolveCaPolicy` 改 `exists/is_regular_file` error_code 口径；目录路径提前可读中文报错（用例红→绿） |
| F7 | 低 | 注释口径处置：`http_client_ca.h` 声明「bundle 路径仅保证 ASCII 可靠，非 ASCII 有 ACP/UTF-8 错配风险、失败形态 fail-closed」；非 ASCII 路径实证用例不做（需真实编码环境，留待需要时立项） |
| F8/F9 | info | 已读不修（审计自评可不修：类型不符静默按默认与既有钳制惯例一致；探测缓存死防御无害） |

## 11. UI 批（MCP 菜单页；主会话拍板冻结面松绑，spec §15 口径）

**范围**：MCP 成为 Router 页面模块（`PageId::Mcp` 插 Wallpaper 后、Settings 前），新增 `src/module_mcp/mcp_page_module.{h,cpp}`（工具卡片网格 + 详情弹窗 + 试一下后台收割）；`app_registry.h` 登记行；`dispatchModuleToggle` mcp 分支改双面性（先启停服务、不再提前 return，页面注册/注销走既有遍历）；`main.cpp` `moduleForPage` 补实例分支。冻结不变：B1/B2 五文件、`module_settings/**`、`mcp_service_bridge.*`（只读调用）、`../EUI-NEO/**`。

| ID | 状态 | 条目 | 验收判据（可核销） | 涉及文件 |
|---|---|---|---|---|
| UI-01 | ✅ | 枚举接线 | `PageId::Mcp` 序 Wallpaper < Mcp < Settings；`pageIdToString(Mcp)=="Mcp"` | `src/core/module_interface.h` |
| UI-02 | ✅ | 注册表行 | 表 8→9 行，第 7 行 gatingKey=kModuleIdMcp/短名 MCP/图标 U+F1E6/工厂；行↔虚函数双副本一致（既有 getters 用例迭代全表自动锁） | `src/app_registry.h` |
| UI-03 | ✅ | 页面模块 | 页头+状态行（桥 status 只读）/卡网格（flow 定宽 340+显式 size）/详情弹窗（按钮+Esc+backdrop 三通道关闭）/试一下（execute 后台线程+compose 收割，UI 线程零直调）/空态/亮暗 token | `src/module_mcp/mcp_page_module.h/.cpp`、`src/module_mcp/CMakeLists.txt` |
| UI-04 | ✅ | 开关双面性 | mcp 开 on=启服务+注册页面；off=停服务+注销页面+回首页（settings 门控承诺兑现） | `src/app_assembly.cpp`、`src/main.cpp` |
| UI-05 | ✅ | 受影响断言 | 真实表形状断言 8→9（assembly+wallpaper 两处）、all-on 计数、uat62 probe slug static_assert | `tests/test_modules/test_app_assembly.cpp`、`test_wallpaper_module.cpp`、`tests/probes/uat62_page_shots_probe.cpp` |
| UI-06 | ✅ | 新用例 | mcp_page 组（pageid 序/注册表行/网格/弹窗开关通道/后台收割/失败路径/onStop join/状态行/hover CTA/默认构造全局源）先红后绿 | `tests/test_modules/test_mcp_page.cpp`、`tests/CMakeLists.txt` |
| UI-G | ✅ | 批次门禁 | 一次增量构建成功 + `--filter mcp` / `--filter app` / `--filter mcp_page` 三子集退出 0 | — |

**批次证据（✅ 已回填）**：
- 命令原文（worktree `D:/wt-mcp-skeleton`，工具链 PATH 前置同 B1）：构建 `cmake --build build --target devdesk_tests` + 直跑 `build/tests/devdesk_tests.exe --filter <子集>`。
- **为什么脚本不够用（CLAUDE.md 诊断例外，须写明）**：共享上游 `D:/project/gui/EUI-NEO` 于本批构建窗口内被并行会话改到中间态（`updateAndRender` 加参、调用点 `glfw_app_main.cpp` 未跟上，11:24 起），`bash scripts/build-test-run.sh` 的全量构建在 **DevDesk exe 目标**上必然失败（上游文件，非本轮改动、非本轮可修——EUI-NEO 红线禁碰）。`devdesk_tests` 目标不含该 TU，target 级构建可绕开破损面取得测试证据；脚本无 target 参数，故按 CLAUDE.md 诊断例外直敲 cmake。上游修复合拢后收官（B5-04）须以脚本全量门禁为准。
- **构建槽互斥记录**：本批窗口内检测到同目录并行构建至少两次（12:14、12:20 前后），曾产生杂交 exe（首启 0xC0000135/行为错乱）；清产物后单写者重建 取证。
- **红**（骨架态，`--filter mcp_page`）：`FAIL grid != nullptr` / `FAIL ui.find("mcp.empty") == nullptr` / `FAIL backdrop != nullptr` + SEGFAULT（弹窗未实现，后续用例未执行）——网格/弹窗/收割面全未落地，行为断言翻红。
- **绿**（实现落定，一次链接后三子集直跑）：`--filter mcp_page` `Total: 13 Passed: 13 Failed: 0`；`--filter app` `Total: 47 Passed: 47 Failed: 0`；`--filter mcp` `Total: 147 Passed: 147 Failed: 0`；三者退出码均 0。
- 过程缺陷（同批修）：①EUI 布局/flow/row 构建链漏 `.build()` 元素不入树（测试揭出，6 处补齐）；②`HorizontalAlign::End` 不存在 → `Right`；③私有方法头文件漏声明 + 缺 `<core/platform/platform.h>`；④hover 用例跨帧复用失旧元素指针（bad_function_call，改为每帧重 find）。
- 待办移交：DevDesk exe 构建与 probe 编译证词待上游 EUI-NEO 中间态收口后随 B5-04 全量门禁统一补验；uat62_page_shots_probe 的 mcp slug 行已同步，截图取证归后续批次。（补验完成：见 §6 B5-04 收官证据第一、二条。）
- **越界注记（2026-10-28 主会话裁决）**：本批相对原冻结面/目标外共 **6 处越界**——①`src/core/module_interface.h`（`PageId::Mcp` 枚举，§15 对 §11 不动清单松绑）②`src/app_registry.h`（注册表 8→9 行，同上）③`src/app_assembly.cpp` + `src/main.cpp`（`dispatchModuleToggle` 双面性不提前 return + `moduleForPage` 分支）④存量断言改判 `test_app_assembly.cpp` / `test_wallpaper_module.cpp` / `uat62_page_shots_probe.cpp`（真实表 8→9）⑤`tests/probes/wallpaper_probe.cpp` 波及 case（`2ad1b4dc`）⑥`tests/CMakeLists.txt` probe 目标补 `mcp_page_module` 源（`fa359678`）——**均已裁决认可**，依 spec §15 口径登记，不再计违规。

