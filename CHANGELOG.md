# CHANGELOG

本文件记录 **a1a106a（macOS 中断现场收编）起** 的条目式变更，按恢复计划的三波（W1 止血收编 / W2 平台架构 / W3 剩余需求）组织，波内按 feat / fix / test / docs 分组。更早的 macOS 主线历史（P0–P8，66 测试通过）见 git log 与 spec §16。

> **发布物依赖版本（2026-09-27）**：OpenSSL **3.6.4** + libcurl **8.22.0**（单 exe 静态链入，两者都是安全相关组件；configure 时 `message(STATUS)` 打印，见 `cmake/Dependencies.cmake` —— 出 CVE 时据此定位）。

> **当前基线（2026-09-24，master @ 817af80）**：用例 **407**（`grep REGISTER_TEST` 口径，本会话 324 → 407；全绿与否以主会话终验为准）。各波次条目内的测试数为该波时点事实，不代表当前总数；波次按合并时间倒序排列。

## [未发布]

- feat(agent)：Scheduler 支持 once 一次性触发（错过不补）——到点窗口内派发一次即停用，已响过以 `agent_runs` 判定（重启不重响），窗口外只记 INFO 不补跑；坏配置/缺 `once_at` 点名 task id WARN。
- feat(agent)：`trigger_config` 支持 `once_at` 一次性触发载荷（Unix 秒，与 `cron`/`events` 互斥；仅编解码与校验，Scheduler / UI 后续接入）。
- feat(agent)：**新增「每天」触发**——不指定时刻，本地当天首次调度跑一次，当天不重复、跨天恢复；判定以 `agent_runs` 为唯一事实源（重启不重复），不消费 `trigger_config`。
- feat(agent)：**system prompt 注入当前本地时间**（`当前本地时间：YYYY-MM-DD HH:MM`）——模型没有时钟，日期类任务由宿主给日期，不让模型自行推算/猜测。
- feat(agent)：智能体 LLM 直连解耦——run 经激活 profile 直连端点，代理未开也能跑（详见下方 `[AgentDirectLlm]` 段）。
- merge：master 并入 dev——桌宠摘除定案、migration 折为单份 DDL（审计放行 `47b31e0`）。
- feat(github-sync)：真机 15 场景验收 15/15 通过，验收抓出并修 4 处引擎缺陷（报告 `docs/superpowers/plans/2026-10-16-ghsync-live-acceptance.md`，随矩阵票合入）。
- fix(github-sync)：推送分支参数修复——`putFile`/`deleteFile` 增 `branch` 形参，database.sql / 笔记 / 删除三路推送全传配置分支（`ebf815c`）。
- feat(agent)：**任务「只跑一次」次数上限**（`trigger_config.max_runs`，跑满自动停用 `acdfa11`）+ **限次额度闸移到 `cronMatches` 之前**（跑满即停每分钟可见，不再等下次匹配 `6e58716`）+ **配置异常任务在列表挂可见标记**（消灭 Scheduler 静默跳过 `bb92b7d`）。
- fix(agent)：**页内错误行不再粘滞回弹**——用户清除后同一份旧错误不被轮询顶回来；实现为「已展示 runId 集合 + 按任务数剪枝」，多任务各错时逐个展示、不兜圈（`7165f6e` + `c8f5989`）；`message_blocks` text 块缺 `text` 字段不再渲染字面量 `"null"`（`f83b26e`）；已通知 runId 集合随窗口收敛，消灭模块内唯一无界容器（`b48f70c`）。
- feat(todo)：`todo.create` 一次调用可带 description/priority/dueAt（消灭建任务二跳 `b398e3c`）。
- feat(todo)：`todo.list` 支持 `overdue` 逾期过滤——直接复用 `TodoService::listOverdue()`（与 Todo 页逾期弹窗同口径）；`completed=true` 与之互斥，返回工具错误而非空列表。
- fix(github-sync)：**嵌套笔记（子目录）三处口径对齐**——收集递归 + 相对名（Windows 不漂反斜杠）/ 落盘前建父目录 / 备份保留子目录结构（`548b0e4`）；**拉取摘要点名库备份名** + 409 覆盖清单 50 行闸（`5a803be`）；**导入失败摘要点名回滚点**（`fb31ae2`，口径修订见 spec F3）；公开仓警告行与披露文案同口径（`8230fcb`）。
- fix(settings)：macOS git 探测每圈都查 deadline（持续输出的子进程不再绕过 3s 上界 `31dd9fe`）。
- fix(markdown)：**冲突未决时显式保存兑现选择**——Ctrl+S / 切笔记 / 退出模块落盘成功即等价于「保留本地」并收掉冲突 toast（否则其「重载磁盘版本」会把刚保存的内容再丢掉）；自动链路仍不越权落盘，退出时未决冲突补 WARN 留痕（`03505df`）。
- fix(core)：**Windows 原子替换改一步 `MoveFileExW`**（消除「目标短暂不存在」窗口，`1c5fbb7`；真机行为待 CI，本机验证 = 交叉编译守卫；POSIX `rename` 语义本来就无此窗口）。
- test：`>1MB` 笔记端到端往返（`6b10975`）；todo.create 三态分支（`0280bde`）；cron「命中分钟」边界与预览配色通道断言（`4fa83dd` + `75a796e`）；库导入收割当前笔记三分支（`c27dd16`）；markdown 轮询周期可注入测试缝 + 缝自证（`3b372cc`，根治 db.imported 用例的负载假红）。
- fix(scripts)：**bash 3.2 兼容** —— 「变量紧跟全角字符」补花括号（`"$V（"` 会把全角括号首字节并进变量名：开了 `set -u` 的脚本直接 `unbound variable` 崩、没开的**静默丢值**）。本次门禁实测抓到 `check-consistency.sh` 自 `6365a68` 起就因此假红（exit 1 = 前置缺失，实际是崩在打印行），装机输出 `✅ 版本号: ??已写入打包产物 Info.plist）` 也是同根；全仓 9 个脚本 27 处一并修（`a6ed265` + `008f9c2`）。
- fix(claude)：**macOS 启动不再反复弹「请输入钥匙串密码」**——旧凭据回填改为一次性（`app_config` 键 `claude.keychain_backfill_done`，失败也置位；成功即清空 legacy `keychain_account` 列）；`keychain_macos` 只有 `errSecItemNotFound` 报 NotFound，授权被拒/用户取消改报 Internal 并带 OSStatus（此前一律伪装成「条目不存在」→ 每次启动重试、每次再弹一次）。判据 `claude_module: 旧凭据回填只尝试一次（二次启动零钥匙串触达）`（`74110de`）。
- docs：github-sync spec F3 回滚口径修订（单事务 ⇒ 导入期失败已整体回滚；备份留盘 + 失败摘要点名回滚点；不做自动恢复及其理由）；M3 规划 Task 1 通知形态结论（**应用级 toast**，不推上游补系统通知 API）；backlog 四点十二/四点十三两批登记与二十余条销账。

## [AgentDirectLlm] 2026-10-16 —— 智能体与代理解耦：直连端点

- feat(agent)：**智能体跑 run 不再经本地代理**——新增 `DirectLlmClient`（`src/module_agent/direct_llm_client.{h,cpp}`，实现 `core::LlmEndpoint`）：按**激活 profile**（baseUrl / API key / authField / models_config / 上游方言）直发 `POST {baseUrl}/v1/messages`；槽位用 `model_slots.h` + profile 的 models_config 自解析，**代理没开也能跑**。删除旧 `LlmClient`（经代理实现）与其单测——代理路径不再保留，需要时从 git 历史找回。
- feat(core)：新增 `src/core/llm_profile_source.h`（`LlmDirectProfile` 快照 + `LlmProfileSource` 抽象）：core 只见纯数据与接口，代理运行态不进 agent 的依赖面；装配层 `main.cpp` 注册 `ClaudeDirectProfileSource` 适配器（激活 profile + `loadApiKey` 读 DB 加密列，明文 key 只活在内存，不落盘 / 不进日志）。
- fix(agent,ui)：页内错误行 / 详情弹窗的「去 Claude 页」引导改为按「LLM 配置不可用：」前缀匹配（无激活 profile / 未配 Base URL / 未配 API Key / 槽位未配置四类；旧「代理未启」错误随解耦不再存在）。
- test(agent)：新增 `test_direct_llm_client.cpp` 七条契约（请求组装 / 槽位解析 / authField 三态 / 四类配置错误不发包 / 401+网络错分类 / opencode 方言 / **解耦硬证据**：无 ClaudeService 单例且无 IClaudeProxyService 注册时仍能组请求走到 HTTP 发送缝）；agent 既有用例的 fake 代理全量换成 `test_support::FakeProfileSource`。
- 已知边界：① 直连暂无 failover（代理路径的备用 profile 单跳重试未随迁）；② opencode 方言判定在 `upstream_dialect.h`（代理转发）与 `direct_llm_client.cpp`（agent 直连）两处各持一份最小复刻（模块隔离下不可互 include），新增方言需两处同步。

## [M2-Agent] 2026-10-02 —— 智能体二期：资源上限 / 单一闸 / Scheduler / 8 新工具 / 任务与运行 UI

- feat(agent)：**资源上限落地**——单次运行墙钟预算 `min(迭代 × 60s, 30min)` + 200K token 上限，由 **AgentLoop 检查点**强制（Scheduler 等触发路径经 loop 组合传递，全覆盖）；超限当轮的 assistant 消息**仍然入库**，只是不再发下一次请求。
- feat(agent)：**AgentStore 补齐**——`createTask`（触发配置）/ `getTask` / `updateTask` / `setTaskEnabled`，以及 **`startRunExclusive` 单一闸**：手动触发与 cron 共用同一入口，锁内「查在跑 + 插 run」原子完成，**同一任务不并发**。
- feat(scheduler)：**cron 调度**——vixie 语义解析（含溢出防护）+ tick 派发 + 常驻任务队列 + `stopAll` 收口顺序（**先停常驻、再停运行中 run**，避免常驻线程在停机窗口里新起 run）。
- feat(agent,tools)：**新增 8 个工具**——`md.create` / `md.list_tags` / `md.delete`（破坏性 + 路径逃逸防护）/ `claude.list_profiles` / `claude.get_active_profile` / `claude.switch_profile`（破坏性）/ `claude.test_connection` / `app.read_settings`（**9 键白名单**，其余配置键零泄密）。
- fix(agent,claude)：`setActiveProfile` 加服务内锁（`switchMutex_`），并发切换不再竞态。
- feat(ui,agent)：**任务与运行管理界面**——任务列表 / 新建 / 编辑（cron 表达式保存期校验）/ 启停 / 常驻入队；模型槽位抽到中立头 `src/model_slots.h`；run 列表（状态 / 耗时 / 迭代 / token / 错误），选中 run 驱动消息区；补**真窗 probe**。
- fix(claude)：**代理服务惰性解析**——会话内开启 claude 模块后，已经在跑的 agent 无须再 off→on 一轮。

## [LazyAssemble] 2026-10-01 —— claude 服务惰性装配 + auto-start 会话内恢复 + 侧边栏表序插入（`adeb3b8`）

- feat(assembly,claude)：**claude 服务惰性装配**——启动期 claude 模块关闭时不再创建 `ClaudeService`；会话内在设置页开启该模块即可**即时装配**服务，不再需要重启。落点：`module_toggle_bridge` 增第二桥函数 `ensureClaudeServiceAssembled`，`main.cpp` 侧 `assembleClaudeServiceNow` 幂等拆分（重复调用安全）。
- feat(settings)：**auto-start 会话内恢复**——会话内开启 claude 模块时读取 `claudeProxyAutoStart` 配置，为真则自动启动代理。
- fix(router)：**侧边栏表序插入**——`Router::registerModule` 改为按 `PageId` 表序插入，off→on 重注册回原位，不再 append 到末尾。
- fix(assembly,claude)：**退出时序 UAF**——惰性装配使 `ClaudeService` 构造晚于 exit hook 注册，`atexit` LIFO 反转导致退出路径踩悬空指针；两层修法：`~ClaudeService` 清 global 指针做防御 + `getClaudeService` 改**永不析构堆持有**，保证退出时 `stopProxy` 正常还原 `settings.json`。
- **已知边界更新**：`[LiveToggle]` 遗留的「claude 启动期被关闭后、会话内重新开启需重启才能完成装配接线」与「模块重新启用后回侧边栏末位」两条边界，本批已消除。

## [LiveToggle] 2026-10-01 —— 模块开关即时生效（路线 A 注册表活门控）（`39b44f5`，13 提交）

- feat(router,assembly,settings)：设置页「模块」开关从**重启生效**改为**即时生效**——路线 A（注册表活门控）。三层落点：`Router` 活门控基座（`fc68ee0`，新增 `unregisterModule` + register/unregister 幂等）→ 装配层 `dispatchModuleToggle`（`615ca70`，on/off 驱动注册与模块生命周期 + 当前页停在该模块时重定向）→ `main.cpp` 装配段注入活门控 handler（`149b279`）+ settings 侧经中立层桥头接 `app::toggle`（`8f4cd65`，可注入缝）。
- fix(modules)：四页面模块 onStart/onStop 重入幂等（`f955fb7`，double-onStart no-op 守卫）；todo/markdown service_ 进程级存活收口 off→on UAF 窗口（`6002c50`，onStart 复用存活服务，不重建悬空指针）。
- test：侧边栏实时反映门控状态（`eb3d0d6`，无头 + probe 双层；probe 增真点击路径 + redirect-in-dispatch 断言 `2a2e2cc`）；装配层锁 D-1 off 路径 onStop 先于 unregisterModule 的时序（`83ab3ed`）。
- fix(ui)：模块开关与托盘文案改即时生效口径 + claude 模块关闭后进入其页面给死页（无服务）toast 缓解（`3d20fc6`）。
- **已知边界**：claude 在启动期被关闭的，会话内重新开启需要重启才能完成装配服务接线；模块重新启用后回侧边栏末位（不保留原顺序位）。

## [Schema] 2026-09-30 —— 删版本化迁移，改单份当前 schema 建表（无历史包袱）

- refactor(core)：`src/core/migrations.h`（11 条版本化迁移 v1–v11）→ `src/core/schema.h`，一份 `core::schema::kSql`（9 表 + 5 索引 + FTS5 虚表 + 默认 Todo 列种子）。`SqliteDb::runMigrations(vector<Migration>)` + `schemaVersion()` → `applySchema(const std::string&)`：整份脚本一个事务幂等执行。`Migration` 结构体、`schema_version` 建表/查询/插入、「数据库比二进制新」降级守卫**全部删除**。
- **为什么**（用户决策）：程序尚未开发完毕，不存在需要升级的真实旧数据。版本化迁移是为「已发布产品要保住存量行」准备的，这份包袱在本项目没有对象；而 DROP COLUMN（曾要求 SQLite ≥3.35）之类约束因此自动消失——新库直接建最终形态。
- **已知后果（既定决策，不是 bug）**：**旧 DB 文件不再被升级**。schema 改动后旧库缺列就是缺列，本程序不检测、不迁移、不告警、不自动重建——删掉旧 db 文件重来即可。当前形态的旧库不受影响（全部 `CREATE ... IF NOT EXISTS`，打开即命中，不改结构）。
- 幂等：DDL 靠 `IF NOT EXISTS`，唯一的种子 INSERT 靠 `WHERE NOT EXISTS (SELECT 1 FROM todo_columns)`（`todo_columns.title` 无 UNIQUE，写不了 ON CONFLICT）——连跑多次种子仍是 4 行，且不覆盖用户改过/删过/加过的行。
- test：新增 `tests/test_core/test_schema.cpp`（原 test_migrations.cpp 改名重写）——逐表逐列断言最终形态（`PRAGMA table_info` 的 name:type:notnull:default 逐条比对）、种子行含 kind、连跑两次 sqlite_master 逐条相同且不踩用户数据、断言**无** `schema_version` 表 / 无 trigger / 无 view。`test_provider_config` 的「旧库升级」用例改为「新列默认值 + 空 JSON 读侧回落」；`test_markdown_service` 原内联复制的 markdown DDL 改为共用 `core::schema::kSql`。变异验证：删 `claude_profiles.fallback_profile_id` 列 / 改 `auth_field` 默认值 → 新断言红，恢复即绿。
- 实测（macOS/arm64 Debug）：定向 `--filter schema` **Total 8 / Passed 8 / Failed 0**；全量 **Total 894 / Passed 888 / Failed 0**。另用一次性程序对全新空 DB 连跑两次 `applySchema` 核验：两次均无错、25 个 sqlite_master 对象逐条相同、9 张表列清单与 schema 逐列一致。

## [DataRoot] 2026-09-29 —— 应用数据根搬到用户主目录 `<home>/DevDesk`（`03c896c`）

- fix(paths)：三个平台的 `platformDefaultAppDataDir()` 从系统惯例位置改为用户主目录下一级——macOS `~/DevDesk`（原 `~/Library/Application Support/DevDesk`）、Windows `%USERPROFILE%\DevDesk`（原 `%APPDATA%\DevDesk`）、POSIX `~/DevDesk`（原 `~/.local/share/DevDesk`）。主目录来源按平台取（macOS/POSIX `HOME`、Windows `USERPROFILE`，与同文件 `claudeSettingsFile` 同源；不硬编码、不用 cwd）。**为什么**：数据根要默认可见（`~/Library/Application Support` 在 Finder 里是隐藏的），且笔记目录后续要做 git 仓库、用户会自己去 commit / 推远端。单一数据根设计（`appDataDir()` 唯一入口 / `DEVDESK_DATA_ROOT` 优先 / `executableDir` + `claudeSettingsFile` 两个刻意例外）不变。
- **不做迁移**（用户决策）：新根从零开始，旧位置数据原样留存——不读、不删、不搬；要保留旧配置 / 笔记 / 看板只能手动拷贝（`docs/USER_GUIDE.md` §5 末「从旧位置升级」一段有命令级说明）。
- test(paths)：新增 `paths_default_root_home_devdesk`（默认根恰好 = `<home>/DevDesk`，parent == 主目录，防回落路径残留 `Library/Application Support` 中间层）与 `paths_default_root_no_legacy_touch`（临时主目录隔离：新根不存在时 `appDataDir()` 即返回新路径且不创建任何目录；旧位置种 config + 笔记后，路径解析与 `ensureDirectoriesExist()` 都不读不删它）。`test_support::AppDataEnv` 在 Windows 补重定向 `USERPROFILE`（数据根不再经 `APPDATA` 派生后，只重定向 `APPDATA` 隔离不住真实用户目录）。变异验证：默认根改回旧位置 → 两条新用例 FAIL（`test_paths.cpp:287` / `:320`），改回即绿。
- docs：`README` / `USER_GUIDE`（§5「数据都在哪」表 + 3 处路径）/ `KNOWN_LIMITATIONS` / `UI-UX-DESIGN` / 权威 spec（§6.2 `paths.h` 样例、§9.1 平台差异表、§10.1 启动流、笔记默认根）/ `installer/macos/安装说明.txt` 同步新根。
- 实测（macOS/arm64 Debug，一次性独立目录 `build-paths-verify`）：定向 `devdesk_tests paths` **Total 12 / Passed 12 / Failed 0**；全量 **Total 819 / Passed 741–742 / Failed 69–70**（三次运行区间）——失败集合与 HEAD 既有基线同类（`f5be21f` 记录 HEAD 65 条、markdown 子集 51/73 本机逐条一致），集中在 `mod->onKeyEvent` 键盘事件与其它 agent 在飞改动（todo / about / UI modern），与数据根无关。**Windows / POSIX 路径未实机与未编译验证**。

## [Installer-M2.1] 2026-09-28 —— 安装向导中文语言：简体中文默认 + 英文可切（`5da21a0`）

- feat(installer)：`installer/windows/devdesk.iss` 新增 `[Languages]` —— `chinesesimp` 排首 = 默认向导语言（契合中文用户环境，与 `[Run]` 段既有"启动 DevDesk"中文文案一致）；`english` 次之，向导首屏语言下拉可切英文。
- 中文 ISL 自托管：本机 Inno 6.7.3 安装的 `Languages` 目录缺失全部中文文件（`compiler:` 前缀引用会编译失败），故把官方 issrc `Files/Languages/ChineseSimplified.isl`（Inno 6.5.0+ 简体中文消息，与 6.7.3 兼容）随仓库放 `installer/windows/Languages/`，经 `{#SourcePath}` 引用；english 复用编译器自带 `Default.isl`（官方英文基准，本机同样缺 `English.isl`）。
- 官方默认中文文案已覆盖全部向导按钮与提示（"下一步"/"安装"/"完成"/"浏览" 等均现代友好），不做 `[CustomMessages]` 覆盖。ISCC 实编译通过、零 language warning；安装包体积 +3,463 字节（12,403,967 vs 12,400,504，语言消息表）。

## [Batch-0927-D] 2026-09-27 深夜 —— 第三批：审计分片 A–F 与汇总 + U8/U1 上游采纳 + 写路径收口 + ccache 闭环（`c48546e` → `9dd9ee2`）

master `c48546e`（`[Batch-0927-B]` 收口）→ `9dd9ee2`：**36 个 `--no-ff` 合并批次 + 3 个直接提交** / 96 文件（+8840 −805）。内容 = 六片全量审计（A–F）+ 汇总（79 条 → 去重 77 行 / Top10 / 三波计划 / 8 条待拍板）+ 审计修复批（CP-01 / CP-02 / CPX-02 / CPX-03 / MD-01 / PAB-01 等）+ EUI-NEO 上游 U8 消费侧与 U1 采纳 + 写路径与值语义收口（原子写两批 / JsonValue 去镜像 / SqliteDb 非法态不可表达）+ 共享配置片段批 1（migration v6）+ Markdown Phase3 M1·M2 + ccache 编译缓存闭环。按合并时间倒序（`fe0b990` 反向同步 M5 收口的内容已见 `[Batch-0927-C]`，本节只列时间线）：

### [Profile-Run] `a93cf58` —— Profile「运行」：用该 profile 在新终端打开 Claude Code
- 详情页第 6 键「运行」：确保本地代理在跑（用 `configuredProxyPort()`）→ 目标 profile 非激活则先切激活 → 在新终端里用该 profile 的托管配置打开 `claude` 会话。密钥零落盘、零进命令行/进程环境（会话走本地代理，真实密钥留 Keychain 由代理转发注入）。
- 新增平台层 `platform::terminal`（`src/platform/terminal_launch*.{h,cpp}`）：Windows = Windows Terminal 优先（`%LOCALAPPDATA%\Microsoft\WindowsApps\wt.exe` 探测），缺失退化传统控制台并提示；macOS = 写 `appDataDir()/run-scripts/run-<id>-<ts>.command` → `open`（跟随默认终端）；其他平台 `NotImplemented`。命令文本拼装为跨平台纯函数，单测逐字比对（含 `-EncodedCommand` 的 UTF-16LE+base64 载荷解码回读）。
- 已知边界（spec §10）：起始目录默认用户主目录（首次进入新目录会弹 workspace trust）；运行中的会话不受 DevDesk 管理（停代理/退出只提示）；macOS 侧本机无 Mac，未实机验证。

### [Installer-M4] `bb71195` —— 安装器 M4（macOS 侧）：executableDir 三平台 + 皮肤根改 exe 目录 + bundle assets（`e2a848c`，集成 `597c0eb`）
- fix(paths,pet,macos)：新增 `executableDir`（三平台）→ 皮肤根与打包脚本改「exe 旁资源」；macOS 打包补 assets；与 master 集成时 test_paths 双方用例保留。

### [Installer-M2] `69bbfdf` —— 安装器 M2：Inno .iss + 两平台打包脚本 + DMG 去隔离脚本/说明 + .gitignore（`51f12c4`）
- feat(installer)：`scripts/package-windows.sh` + `scripts/package-macos.sh` + `installer/**` 全部文本文件（.iss / .command / 说明）+ `bash -n` 校验；缺 ISCC 时清晰报错 + 给安装命令；`.gitignore` 补录入。

### [Installer-M1] `de8183c` —— 安装器 M1：DMG 背景图生成脚本 + 素材入库（`5cf517e`、`cdb3d02`）
- feat(installer)：新增 `doc/scripts/gen_dmg_background.py`（Windows 侧 Pillow 预生成 700×440 / 1400×880 两张背景图入库，附尺寸断言）。

### [Installer-M1.2] `7748006` —— 安装器 M1 补：DMG `.icns` 生成脚本（macOS 专用 `iconutil`/`sips`，Windows 侧入库为载体）
- feat(installer)：新增 `doc/scripts/gen_app_icns.sh`（macOS 侧用 `iconutil`/`sips` 合成 `.icns`；Windows 侧脚本入库、以 git 为载体）+ `doc/scripts/index.md` 登记 + `installer/macos/.gitignore` 补 `.icns` 产物。

### [Audit-P2] `db0093c` —— 审计 P2：probe_winfx.ps1 输出路径去 worktree 硬编码（`198517b`）
- fix(scripts)：probe 截图输出改为脚本所属仓库 `build/probe`（原路径指向已删 worktree）。

### [Platform-P1] `326ee39` —— 平台适配修复批 P1（5 commits）：AppleClang 判据 / POSIX 原子写 / 测试隔离 / file_open 契约（`a4d55b9`、`34e4087`、`3819a11`、`8a20054`、`e1c702d`）
- fix(cmake)：`devdesk_apply_warnings` 补 AppleClang 分支（macOS 上警告开关整段静默失效）。
- refactor(core)：原子写覆盖式替换按平台拆文件（POSIX 走单次 rename 原子替换）。
- fix(tests)：AppDataEnv 一并隔离 HOME、keychain 用例 macOS 整例跳过（测试隔离）。
- refactor(platform)：`wideToUtf8ForTest` 声明移出平台无关头（新增 `file_open_win.h`，file_open 契约补齐）。

### [AuthField] `77c7f6e` —— Profile 认证字段（鉴权头三态、migration v7）
- 新增 profile 级「认证字段」：`both`（默认，双发 x-api-key + Authorization: Bearer，= 原行为）/ `bearer` / `api_key`；由代理转发按 profile 决定注入哪个鉴权头（流式 / 非流式 / 连通测试三处同源）。
- 接管 settings.json 的占位符仍固定 `ANTHROPIC_AUTH_TOKEN=PROXY_MANAGED`（不随该字段变）；粘贴导入按键名证据自动选（只带 ANTHROPIC_API_KEY → 只发 x-api-key）。

### [Ccache] `dc727cd` —— CMake 自动探测 ccache 成为仓库默认 + worktree 预设（`649bea3`，直接提交 `3e7a600`）
- `scripts/build-test-run.sh` 默认带 `-DCMAKE_{C,CXX}_COMPILER_LAUNCHER=ccache`（未装 ccache 自动降级、行为同从前）；新增 CMakePresets worktree 预设 + `-DDEVDESK_CCACHE=OFF` 开关；并行度封顶 `DEVDESK_JOBS=3`。
- 三件套落 ccache **持久配置**（`%LOCALAPPDATA%\ccache\ccache.conf`：`cache_dir D:/software/ccache-devdesk` / `base_dir D:/project/gui` / `hash_dir=false` / `max_size=20G`），**不需要任何 `CCACHE_*` 环境变量**；实测冷 647 s → 热 35 s（239 次查找 237 命中 = 99.2%，全 direct 命中）。

### [Docs-Eui-Rev] `9dd9ee2` —— EUI 依赖 rev 记录三处失真修正（`d5e302c` → `269a8c4`，`be9b44f`）
- `CLAUDE.md` 两处 + `docs/superpowers/plans/2026-09-27-HANDOFF.md` 的 EUI rev 行按实测对齐到 `269a8c4`；标注 EUI-NEO 本地 main 领先 `origin/main` 70 个提交未 push。

### [Md-Anchor] `1784a37` —— 实时态活动块锚跟随真实光标（审计 MD-01，`e09feb8`）
- 修 M2 引入的真缺陷：回车/标题/列表造块后输入框换前块（锚只初始化不推进 → 块边界一变就换文本 + 光标归零）；改为选活动块前用活动块起点回填锚（`setLiveCursor` 最小版）。

### [Settings-NoIO] `af3f4d3` —— 设置页 compose 里 4 组 fs 调用移出渲染路径（审计 THS-01，`c72c060`）
- 快照 + 显式刷新点，渲染路径零文件系统查询。

### [Cpx-Ui] `4c74e69` —— 接管/覆盖警告接到 Claude 详情页（审计 CPX-02 UI 侧，`fab039e`）
- 去掉被吞的展示门，`lastSyncWarnings` 在 Claude 页可见；服务侧上报已随 `71b13a9` 落地（`syncRestore` 仍 void，剩余半见审计汇总表）。

### [Eui-Rev-U1] `de63bba` —— EUI 最低依赖 rev 抬到 `269a8c4` + todo 视图状态注释对齐（`1687cbf`、`2a972b9`）

### [Audit-Consolidated] `5215b94` —— 六片审计汇总（`807deff`）
- 79 条 → 去重 **77 行**（已修 10 / 部分已修 3 / 待排 66）；含 Top10、三波修复计划（波内按线、线内串行、线间文件不冲突）、8 条待拍板；产出 `docs/superpowers/plans/2026-09-27-full-audit-consolidated.md`。

### [U8-Consume] `6dc03a9` —— U8 消费侧清理（`1b67687`、`481cf11`、`4e74966`、`bfd641f`）
- 摘掉 EUI-NEO `-Os` workaround（上游 U8 已不向消费者 app target 强加）；`json.cpp` 手写序列化器换 yyjson writer，并补**逐字节快照用例**锁新格式；`json.cpp` 注释对齐上游 writer 放开现状。

### [U1-Consume] `29d459f` —— todo 拖拽改吃框架 `contentScale` + `windowToCompose` 入参改源（`13e31fb`、`459a5be`）
- dpi 探针降级为断言；坐标换算唯一入口 `core::windowToCompose` 的入参语义改指框架尺度（U1 契约）。

### [Shared-Fragment-1] `2d2ebfa` —— 共享配置片段批 1（`a6cfac9`、`d6419bc`、`ee3505a`）
- migration v6：共享配置片段表 + profile 绑定列；服务层片段 CRUD、绑定与生命周期（合并 + G10 接线）；合并优先级与 G10 防污染守卫（纯函数层 + 接管入口）。

### [Audit-Shard-D] `4ad16e3` —— 全量审计分片 D（markdown）8 条（高 1，`81e8e40`）
- 高 1 = MD-01 实时态锚冻结（已由 `1784a37` 修复）；其余 3 中 / 4 低见汇总表。

### [Sqlitedb-Ca] `5ffa2a2` —— SqliteDb 非法态不可表达 + 平台宏收进 CA 拆分点（审计 CP-07 / CP-03，`f06da47`、`0b60c49`）
- `SqliteDb` 默认构造私有化、`open()` 成为唯一入口；CA 策略按平台拆文件，`http_client.cpp` 回到平台无关。

### [Md-Phase3-M2] `ad82fbc` —— Markdown Phase3 M2 活动块单输入 + 内容贴合高度（`8c2359c`、`8cf37df`）
- 纯函数层（活动块推导 / 块局部偏移 / 内容贴合高度）+ UI 接线与无头用例；`[live] input over=0` 达标。

### [God-Class-Design] `d483f5b` —— spec：上帝类治理设计（CPX-01 / CPX-05 / THS-02，`1f5e15b`）
- 产出 `docs/superpowers/specs/2026-09-27-god-class-refactor-design.md`（Q1–Q3 待拍板）。

### [Atomic-Write-2] `63a6d64` —— 截断写收口第二批（`e1f173b`）
- markdown 视图状态 / pet 配置与皮肤三个写点改用 `core::atomicWriteFile`。

### [Test-Runner] `e7eab8c` —— 运行器统计口径（审计 TIN-03，`fc8eda1`）
- 0 用例命中不再假绿、`[SKIP]` 单列 Skipped。

### [Core-No-Draw] `3515e28` —— core 不再直接绘制（审计 CP-01 分层违规，`61ecab6`）
- 空态兜底文案改为 app 层注入回调，`core` 零 `ui.` 调用（spec §4.2.1 边界恢复）。

### [Script-Worktree] `27d9a82` —— 构建脚本自带 EUI-NEO 路径解析 + 首建自动 configure（审计 PAB-01，`0efa088`）
- worktree 里直接 `bash scripts/build-test-run.sh` 即可（`EUI_NEO_DIR` → 同级 `../EUI-NEO` → git 主工作树同级，三段式探测）。

### [Usage-Log] `cc7d4ca` —— 用量落库（cc-switch ref §3-4 第一批，`1d8cbf2`、`ba929d4`）
- migration v5：`claude_call_logs` 加 token 四类 + 首事件毫秒；流末回填本次流的用量。

### [Ccswitch-Ref-1] `71b13a9` —— cc-switch 可抄项批次 1（`2f82c7c`、`2a5bb77`、`a8c950e`、`e33c244`）
- takeOver 写 env 前先删上一轮托管、本轮不再生成的键；接管快照污染守卫 + 无 sidecar 时清除残留代理形态键；接管写 live 失败从静默升级为可上报（`lastSyncWarnings`）；反向同步守卫表 G1–G6 拆成每守卫一条专测。

### [Json-Value] `d443b8c` —— JsonValue 去镜像字段（审计 CP-02，`fb92d51`）
- 类型与取值统一从 `JsonImpl` 派生，修左值拷贝/移动静默错值。

### [Atomic-Write-1] `cf5edf7` —— 原子写收口（审计 THS-03 + 审计漏项，`a9d3116`、`8401b5a`）
- 新增 `core::atomicWriteFile` 原子写唯一实现；todo 视图状态 / 设置配置落盘迁移。

### [Audit-Shard-F] `8f6cd72` —— 全量审计分片 F（tests + 基建）13 条（高 2，`c9fcb4f`）

### [Audit-Shard-E] `3c9b60b` —— 全量审计分片 E（pet + app + 构建）13 条（1 高，`d627c4e`）

### [Shared-Fragment-Design] `a588085` —— 共享配置片段 + Failover 交互设计（295 行，`7012b98`）
- 产出 `docs/superpowers/specs/2026-09-27-shared-config-and-failover-design.md`；含流程图与 5 条待拍板。

### [Md-Phase3-M1] `534cd57` —— Markdown Phase3 M1 第四态骨架（只读）+ C1 聚焦 `.hit` 修复（`1624655`、`ec578c6`）
- 编程式聚焦统一改发 input 的 `.hit` id（M0 spike C1 结论落地）。

### [Audit-Shard-B] `a02f05b` —— 全量审计分片 B（todo + home + settings）18 条（0 高 / 6 中，`5b4cfb2`）

### [Audit-Shard-A] `c0374e6` —— 全量审计分片 A（core + platform）12 条（高 2，`7b6f64b`）

### [Audit-Shard-C] `f154618` —— 全量审计分片 C（claude_proxy）15 条（架构 / OOP / 可维护性为主轴，`3e68183`）

### [Eui-Candidates] `7e86964` —— EUI-NEO 上游候选清单（13 条：S6 / M6 / L1 + 建议顺序，`2c663a2`）
- 产出 `docs/superpowers/plans/2026-09-27-eui-upstream-candidates.md`（含不建议上游清单）。

### [Timeline] `fe0b990` —— 反向同步 M5 收口：详情页第 5 键「复制」+ `doDuplicate` + 无头 UI 用例
- 内容见 `[Batch-0927-C]`（本节只列时间线以免重复）。

### [Ccswitch-Ref-Docs] `a3d13f4` / `c073db7`（直接提交）—— cc-switch 参考两份
- 代理侧（`c073db7`）与配置 / 反向同步侧（`a3d13f4`）：机制速览 + 逐项对照 + 可抄项（6 + 8）+ 不该抄（7 + 8 类）+ 各 5 条待拍板；落 `docs/superpowers/plans/2026-09-27-cc-switch-ref-proxy.md` 与 `...-provider-sync.md`。

### [Compose-IO-1] `f47766b` —— compose 里 3 处 IO/DB 移出渲染路径（审计 H2 第一批，`f50e1ac`）
- 只读快照 + 后台 tick。

### [Eui-Capability-M0] `03b56b0` —— EUI-NEO 能力盘点补 M0 spike 实测口径（`621eee7`）
- 焦点 / pin 命中 id / 流式 input / 换文本残留。

### [Coord-Unify] `2518545` —— 坐标 / DPI 收成唯一换算 `windowToCompose`（审计 M4，`cc5ed26`、`9916f3e`）
- core 新增唯一坐标换算；markdown / todo 调用点改走它，dpi 探针降级为断言。

### [Docs-Closeout] `a901489` —— 文档收口（`b025864`）
- CHANGELOG 第二批 12 merge 条目 + 用例数 514 → 530 + HANDOFF 刷新 + 审计存疑 V1 标 REFUTED。

### [Flake-Fix] `9f35eb7` —— 测试去 flake（`1fd57e8`）
- 墙钟红线改记录式路径断言 + 实测值记录；`test_claude_ui.cpp` 固定端口 28888 改动态端口。

## [Batch-0927-C] 2026-09-27 —— 反向同步 M5 收口：Profile 复制的 UI（`f47766b` → `82577ed`）

### [Reverse-Sync-M5] `82577ed` —— 详情页第 5 键「复制」+ `doDuplicate` + 无头 UI 用例
- 详情页操作按钮组 4 键 → 5 键：`actW` 由 `(w0 - actGap * 3) / 4` 改 `(w0 - actGap * 4) / 5`（最小窗 1200 → 120px、默认窗 1600 → 200px，容纳 4 字按钮无虞）；新增「复制」`claude.act.dup`（点击节点 `.bg`，与同区其他键同款），复制源 = 详情面板当前显示的 profile（`shown`，取法与测试连通/编辑一致）；无选中 → toast「请先选择 Profile」。
- 新增 `doDuplicate(int64_t)`：标脏 `dataDirty_` → `ClaudeService::duplicateProfile(id)`（M4 已落地，**服务层零改动**）→ 失败按既有 toast 口径报错，成功 toast「已创建副本「X 副本」（未激活）」；副本不激活 ⇒ 选中态/激活态不变。
- 无头 UI 用例 +2（`test_claude_ui.cpp`）：① 复制键建副本——DB 对账（key 同源 / `is_active` 未变 / 源行未动）+ toast 文案 + 选中态不变 + 新卡片出现（有界轮询）② 选中卡点启用 → settings.json 的值收编进原激活 profile（M3 反向同步链路的 UI 侧证据）。「compose 无服务」用例补 `claude.act.dup` 存在 + 点击不崩。
- 定向实测 `devdesk_tests.exe claude` **60/60 全绿**（含本批新增 2 条）；全量套件由主会话终验。
- **未截屏**：Windows 开发机无可用无头截屏手段（仓库内唯一截屏脚本 `scripts/screenshot.sh` 走 macOS `screencapture`），本批施工口径禁启动 app。替代物 = spec「附：记录（实施期回填）」里的 5 步用户点验清单。

## [Batch-0927-B] 2026-09-27 晚 —— 第二批 12 个 merge（`e99ee21` → `c48546e`，master 实测 530/530 全绿）

master `e99ee21`（第一批收口 docs）→ `c48546e`：12 个 `--no-ff` 合并批次 / 16 个子提交 / 46 文件（+4269 −314）。内容 = 专家审计修复批（M1/M2/M3 残项/M5/M6/M7 + L1/L2/L4/L5/L6/L7）+ 反向同步 M1–M4 + Markdown Phase3 M0 门禁 spike + 装配层与 extern 桥两笔重构。按合并时间倒序：

### [Json-Bridge] `c48546e` —— JSON 深拷贝语义注释 + 粘贴导入 1MB 上限 + argv 直测（审计 L6 / F 残留）
- `platform/json.h` 深拷贝语义显式注释 + 粘贴导入整体文本上限 1MB（含用例，审计 L6，`25e6327`）。
- `app_assembly` 的 `captureOpenFileFromArgv` 加可测接缝 + argv 捕获直测（F 残留 ②，`2cd781a`）。

### [Md-Phase3-M0] `cdb481a` —— Markdown Phase3 M0 门禁 spike（V1–V4 结论）
- 结论：**四项全部有解，可进入 M1**，带 3 条接线硬约束（C1 编程式聚焦只发 `<input id>.hit`；C2 单输入 id 不得落在 `releaseStateScope` 释放的前缀下；C5 焦点帧末消费 ⇒ caret 次帧出现 ≈16ms）。
- 产出 `docs/superpowers/plans/2026-09-27-md-phase3-m0-spike.md`（结论 + 13 项用户点验清单）+ `tests/test_modules/test_md_live_spike.cpp` 9 条无头证据用例（`7a794e8`）。

### [Reverse-Sync-M3M4] `4a34eb0` —— 反向同步接线 + Profile 复制
- M3 `syncCaptureActiveProfile`（`d1d880f`）：插入点 = `setActiveProfile` 的 `getProfile` 校验之后、清零/置位事务之前（先于既有 `syncTakeOver`）→ 读侧 → 纯函数 → 逐字段比较 → 仅真变化时 `updateProfile`；G0 无激活不读文件；守卫键不写也不清、非守卫托管字段缺席即清空；坏 JSON/读不了 → fail 中止（S-2，profile 一字未改）；key 清空走现成 `deleteSecret` + 最小 UPDATE；写序 Keychain → DB、DB 失败 best-effort 回写；新增 service 用例 ①-⑧（+711 行）。
- M4 `duplicateProfile`（`db0f15f`）：含 key 全复制、副本不激活（⇒ 不触发 M3，零耦合）、`name + " 副本"` 重名递增避让（100 次上限）、二段 `updateProfile` 失败用 `deleteProfile` 回滚（回滚失败文案注明残留 id）。

### [Ui-Esc-Header] `0f09f1d` —— header 对偶宽度常量 + 三模块 Esc 关最上层弹窗（审计 M6/M5）
- header 对偶宽度字面量抽 `kHeaderActionsW`（M6，`b148a2c`）；claude/markdown/todo 三模块 Esc 关最上层弹窗兜底（M5，`86e3be1`，三份 UI 用例 +238 行）。

### [Doc-Align] `4e297cc` —— doctest 语义注释 + 用例数对齐 + 陈旧注释（审计 L2/L4/V6）
- `tests/doctest.h` 注释注明 `DOCTEST_REQUIRE` == `DOCTEST_CHECK`（fail-fast：断言失败即 `return false`，同用例后续断言不再执行）；用例总数 481 → 514 对齐（`CLAUDE.md` + 反向同步 spec §6 验收基线）；`settings_sync.cpp` sidecar 过期注释修正（`e1469d7`）。

### [Audit-Doc-Reconcile] `122d40a` —— 审计文档对账 + HANDOFF 刷新
- 消 M1↔C 类裁决矛盾 + 编号列状态标注（`d9c940c`）；HANDOFF 第二批收口刷新。

### [App-Assembly] `0b04d29` —— 装配层抽到 `app_assembly` + 4 条 headless 护栏（审计 M2）
- `src/main.cpp` 装配段抽成 `src/app_assembly.{h,cpp}`（+191 / +106 行），main.cpp 净减 ~110 行（+93 / −202）；新增 `tests/test_modules/test_app_assembly.cpp` 4 条 headless 护栏用例（`9351c28`）。

### [Pet-Bridge] `85f6144` —— extern 桥收口到中立头 `module_pet_bridge.h`（审计 M1 短期项）
- settings↔pet 的 extern 自由函数声明收进 `src/module_pet_bridge.h`，双方各自 include 同一份契约头（链接期解耦裁决保留、模块隔离实质不变）；`test_settings.cpp` 模块隔离守卫按新形状重写（`805772e`）。

### [Core-Hardening] `4a41f4b` —— 日志轮转 + 日志开关原子化 + `SqliteDb` 关闭/加锁对齐（审计 M7/L7/L5）
- 日志按大小轮转（M7）；`g_level` / `g_console` 改 `std::atomic`（L7）；`SqliteDb` 析构改 `sqlite3_close_v2`、`bind()` / `prepare()` 加锁（L5）；`test_logging.cpp` / `test_sqlite.cpp` 补用例（`538abcf`）。

### [Dead-Code] `f57e8b8` —— 删死代码 `sendWithRetry` + `Result` 默认态 `errorMessage()` 不抛
- `http_client` 删 `sendWithRetry`；`Result` 默认态（未真正报错时）的 `errorMessage()` 返回文案不再抛异常；+用例（`b6efb37`）。**H1（取消即中止）经核实早在 `797b3a8` 已落地**，与本 merge 无关。

### [Config-Bak] `9e9e354` —— `config.json` 解析失败保留 `.bak` + configure 打印依赖版本
- settings `config.json` 解析失败保留 `.bak` + WARN 出路径（审计 M3 残项，`6e4b32f`）；configure 打印 OpenSSL / libcurl 版本并记 CHANGELOG 当前组合（审计 L1，`6ec6c8d`）。

### [Reverse-Sync-M1M2] `0b68bc9` —— 反向同步 M1 读侧 + M2 纯函数（+11 用例）
- M1 `settings_sync::readSettingsEnvForSync`：只读、复用既有 IO 助手；六种文件形态分别处置（缺失 / 0 字节空文件 / 无 env → no-op 成功；坏 JSON / 顶层非对象 / env 非对象 → fail 待中止）、sidecar 损坏只 WARN 不失败。
- M2 `provider_config` 四个纯函数（全部追加在文件末尾，不改既有函数行为）：`isLoopbackUrl`（127/8 全段 + localhost + ::1，不做前缀匹配）、`parseEnvForReverseSync`（容错映射，守卫 G1–G6 登记 `skippedKeys`）、`mergeReverseSyncEnv`（出现覆盖 / 缺席清空 / 守卫保持）、`makeCopyName`（「副本 N」避让，100 次上限）；用例 +11（子串 `reverse` / `loopback` / `copyname`）。

## [Session-0923] 2026-09-23 晚 → 09-24 —— 代码/产品审计修复波 + 特性波（f602599 → 817af80，23+ 批，测试 324 → 407）

master `f602599`（侧边栏折叠 merge 点）→ `817af80`：100 commits，22 个 `--no-ff` 合并批次 + 直接提交（审计批、docs 流程批、桌宠气泡①②、HEAD todo 搜索收尾）。文中 `[N]` 编号全部指向两份审计总账：代码质量 49 条（`25e1e67`，2H/20M/27L）+ 产品视角 52 条（`4a81135`，4H/30M/18L），两总账文档批随欠登批一并补登（见文末「欠登补登」节）。按合并时间倒序：

### [Todo-Search] `817af80` —— Todo 搜索 Enter 无副作用
- Enter 改无副作用与 Markdown 搜索一致（产品审计 [10]），清除只留 Esc（[21] onCancel 路径）。

### [Tech-Debt] `cbc9d4a` —— 技术债四项（A openUp / B 拖拽快照 / C md.tree 语义 / D keychain_posix）
- A：settings 日志卡下拉 openUp 判定魔数抽命名常量，注明与 EUI-NEO dropdown 默认 metrics 的同步耦合（a5d2a36）。
- B：todo 拖拽命中 pressY/scrollOffset 对齐到按下同帧快照，消除拖动中列滚动导致的板顶错位 + 纯函数边缘场景锁测试（656496a）。
- C：md.tree 单 stack 列布局语义规范注释（stack 居中/绝对定位语义 + bg 尺寸耦合点）+ layout 锁测试防回退（e966494）。
- D：keychain_posix deleteSecret 吞错改失败返回 Internal，语义对齐 keychain_win（不存在=幂等 Ok）；POSIX-only 文件，Windows 不编译未跑 Win 测试（a789bb2）。

### [Pet-Bubble-Proxy] `1e9bf08` —— 桌宠气泡③：接代理事件联动（M2 §4.5 ③，产品审计 [18]）（5b5ad19）

### [View-State-Persist] `5f2bda9` —— 视图状态跨会话持久化（审计 [23]）
- markdown：选中笔记/三态视图/目录折叠跨会话持久化（282a250）；todo：列滚动按列 id 记忆并跨会话持久化（07c4160）。

### [Keyboard-Shortcuts] `fc3e6eb` —— 快捷键（审计 [22]）
- claude：Ctrl+N 新建 Profile / Ctrl+S 保存表单 / Enter 启停代理（4bdf3f6）；settings：Ctrl+S 提交待决草稿显式落盘（dbdfeed）。

### [User-Guide-Flows] `885870d` —— USER_GUIDE/README 全局流程对齐（审计 [33]，doc-drift 收尾）
- 启动序列代理默认关 / Ctrl+B 侧边栏折叠 / 托盘左键唤窗+动态收缩菜单 / 桌宠退出二段确认+逾期气泡 / 数据表接管文件 / FAQ 重写（ee745b2）；README 功能清单与 KNOWN_LIMITATIONS 过期项对齐（b7a5b37）；§4.3 去掉不存在的 KNOWN_LIMITATIONS 引用、数据表补笔记标签与修锚点（3a228f2）。

### [Esc-OnCancel] `d2d373a` —— Esc=取消不提交全量接线（审计 [21]）
- 创建/编辑弹窗与内联输入全接 EUI-NEO onCancel（38c1d73），MIN_REV → d5e302c（input onCancel 上游依赖，5a203d5）；Claude 代理流程对齐文档（默认不启动/接管-还原语义/删激活停代理/token 脱敏显形/端口可配 + 设置页六卡全量，c695a38，审计 [17][33]）随批。

### [Tray-Dynamic + 随行直接提交] `2a42f39` 前后 —— 托盘菜单动态收缩 + Todo/Markdown 流程文档 + 桌宠气泡①②
- 托盘菜单按 Router 启用模块动态收缩 + 失效入口给提示（bdce869，审计 [7]）。
- 交错直接提交：Todo 流程对齐现状（981b2d3）+ Markdown 流程对齐（fc4b59e）（审计 [8]）；桌宠气泡①②——接 Home 逾期任务数 + 静态语料（98d6eb0，M2 §4.5 ①②，产品审计 [18]）。

### [Feedback-Consistency] `3f9e218` —— 反馈统一内嵌红字 formError（审计 [19]）
- todo 创建/校验失败 → 内嵌红字（对齐 claude 范本，633b080）；settings 端口校验红字随下一次输入清除（cca0cf5）。

### [Menu-Beautify] `c5e0ede` —— styled 菜单窗：桌宠右键 + 托盘（含原生降级）
- 桌宠右键菜单换 styled 菜单窗（圆角/主题/FA 图标，根治 128px 裁剪，001f013）；托盘菜单换 styled + 原生降级（f7db662）；MIN_REV → b92abb7（styledMenu + onTrayMenu + windowIsFocused，548c0b4）；styled 菜单结构测试 +10 例（e26ed66）；todo 测试悬垂 Element 指针修复（dbf0e37）。

### [Md-Formerr-Table] `93900d4` —— 弹窗错误内嵌红字 + 表格编辑 MVP
- 新建笔记/文件夹弹窗错误改内嵌红字 formError（1536518，审计 [30]）；表格编辑 MVP：右键插入表格级联 + Tab 跳格（f45b431，审计 [12]）。

### [Pet-M2] `b3488e9` —— 桌宠 M2：多显示器钳制 + Sleep 零渲染
- 按位置取对应 monitor work area 钳制（47ccfe2，EUI-NEO getMonitorWorkAreas）；系统挂起停渲染、resume 恢复（32164fd，onSystemPower）；MIN_REV → 12c14ed（8d3dfcf）。

### [Todo-Ux] `1a89a48` —— 列重命名显性化 + drain 防卡死（产品审计 [24]）
- 「+ 列」改弹窗命名 + 列头 ✎ 重命名入口显性化（6adb99d）；createColumn/createTask RETURNING 取行后 drain 到 DONE 防 COMMIT 卡死（9452a85）。

### [Audit-Arch] `5e13cf1` —— 架构 findings（审计 [6][11]）
- spec §4.2 + ARCHITECTURE 承认 core EUI-NEO header-only 类型桥接例外（99fbc19，[6]）；EUI-NEO 集成加 git rev 诊断 + 软版本警告（6d2fc1e，[11]，push 后转硬检查）。

### [Ux-Polish] `4ee112e` —— UX 三项（审计 [25][27][30]）
- 浅色辅助灰/主色文字加深至 WCAG AA 4.5:1（5b166b2，[27]）；桌宠退出二段确认防误触（85da0a0，[25]）；markdown 副标题改静态操作说明、动态状态走 toast（eb81d2f，[30]）。

### [Settings-Spec] `e10af1c` —— 设置/代理规格项四项（审计 [6][14][15][16]）
- claude 首个 Profile 自动激活 + 503 文案指引 + 未激活徽章（bcb59eb，[6]）；代理端口可配 + 改端口自动重启（e3665c0，[14]）；目录卡「打开目录」按钮 + 登记目录不可配限制（fcf8a99，[15]）；代理 token 默认脱敏 + 眼睛按钮临时显形不持久化（a455b5a，[16]）。

### [Todo-Keychain] `9f14dbb` —— rowid 竞态 + JSON 迁移 + POSIX 权限（审计 [7]残留[26][29]）
- INSERT...RETURNING id 取代 last_insert_rowid 竞态（8e29151，[7] 残留）；serializeTags 迁 platform::json 删 yyjson 直引（6562fdc，[26]）；POSIX keychain 落盘后设 0600 防本机他用户读（7479f9c，[29]）。

### [Claude-Ui] `0078fca` —— 日志面板检索 + Profile 防溢出（审计 [11][26]）
- 日志面板加搜索/状态码过滤/翻页（e4ac090，[11]）；Profile 列表加 scrollView 防超长破坏布局（4bb0416，[26]）。

### [Md-Lifecycle] `536f5be` —— 笔记生命周期闭环（审计 [4][5][13][20][31]）
- 笔记删除/重命名 UI 接线（ceb87ad，[4][20]）；文件打开分发带 open: 负载 + 路径解析、内容真实加载（fab8348，[5]）；新建笔记自动聚焦编辑器 + 默认名秒建（f34f997，[13][31]，EUI-NEO requestFocus）。

### [Small-Hardening] `44d9b20` —— 小硬化簇（审计 [22][30][32][33][35][47][48]）
- 搜索 LIKE 转义防误命中（915138e，[22]）；Bearer token 鉴权改常数时间比较（29380f8，[30]）；迁移/schema 注释对齐 + 删 FTS5 死宏（11a3fc2，[32][33][35]）；PageId 真顺序断言 + core::time::localTime 用例（23c3a4e，[47][48]）。

### [Pet-Uaf] `1ece55a` —— 桌宠 UAF 根治 + 产品审计 HIGH（审计 [0][1][12][28]，产品审计 [1][2]）
- 窗口销毁回调清空句柄杜绝桌宠窗 UAF（d578e3f，[0][1]）；坐标落盘前钳进工作区，消除负坐标与 -1 哨兵冲突（7bce2c9，[12]）；皮肤帧名校验只放行相对文件名防路径穿越（4114e70，[28]）；删激活 Profile 时停代理并还原配置（5740d09，产品审计 HIGH [1]）；todo 详情动作按钮先存表单防丢编辑 + 删除/背景守卫（5ecfcd0，产品审计 HIGH [2]）。

### [Winfx-Pet] `296b9c8` —— 窗口效果五修 + 穿透恢复⑥ + 真机探针
- 主窗主体半透/磨砂生效：删五页全屏不透明 pagebg + 侧边栏自铺底（d923f0f）；桌宠灰底——窗口效果档位显式 None（2321dd9）；桌宠右键菜单改原生弹出不被 128px 窗口裁剪（fb0dd16）；托盘菜单全中文化 keepDefault=false + 显示主窗口/退出显式项（7e00096）；设置页鼠标穿透 toggle=穿透锁死态恢复路径（任务⑥，3b9e0bb）；品红底衬真机像素探针脚本（9b1a4cb，d923f0f 配套）。段错误守卫随 EUI-NEO 同名分支上游（requestWindowClose 空句柄守卫 + requestExit/consumeExitRequest 单测）。

### [Audit-Batch1] ff 进 master（分支头 `2d3e50f`）—— 数据完整性簇（审计 [2][3][7][8][9][10]）
- SqliteDb::transaction 持锁贯穿 fn()——并发写不再穿插进开放事务（ad89193）；markdown 笔记保存改原子写 + openNote 切换前守未落盘编辑（2c31871）；claude 停止代理先取消在途流 + createProfile 原子取 rowid + token 满熵 128 位（72f1db8）；runMigrations「数据库比二进制新」守卫 + 回归用例（37bea22）+ 迁移记录三个 bind 返回值补检查（2a3905d）；Keychain 失败补偿回滚 DELETE 补错误检查与日志（b363822）；curl_global_init 改 std::call_once 消首并发数据竞争（ded3c57）；Result::value/expect 失败路径先落 LOG_FATAL 再 throw（5cafb98）；theme_hierarchy 测试对齐 pagebg 删除形态修合并语义冲突（87e017c）。

### [Docs-Drift] `b3eb66a` + `56142c4` —— 文档漂移修正批 + 桌宠节/CHANGELOG 重排（审计 doc-drift [18-21][34][41-46]）
- 测试数 330/330、C++20、架构图补 module_pet（5→6 模块）删 drag_controller、浮动齿轮→常驻侧边栏、Home ⚙ 核实已接线（b3eb66a）；USER_GUIDE 补桌宠节 §3.6 + 六模块/数据表/FAQ 同步 + CHANGELOG 顶部按合并时间重排（56142c4）。

### [欠登补登] f602599 及之前 —— 侧边栏折叠 / Claude 一键导入 / 审计总账两文档
- 侧边栏折叠：72px 图标栏 + Ctrl+B + 持久化 + 内容区自适应（e3de9a5，合入点即 f602599）；导航项加 12px 内边距（32a1027）。
- Claude 页「导入当前配置」一键读 settings.json 建 Profile（9a4a3be，aad617b）。
- 审计总账：代码质量 49 条（25e1e67）+ 产品视角 52 条（4a81135）文档批落 master——本大节 `[N]` 编号的权威出处。
- spec 缩水 A：settings 跟随系统主题（themeMode auto + 平台 prefersDarkTheme，`c0fc536`）——本条目建档时仍在并行分支未合 master，合入与否以主会话为准。

### EUI-NEO 上游增量（随本会话）
- EUI-NEO main 本地领先 origin **62 commit（未 push**，origin/main 停在 v0.6.0，push 与否为用户决策），其中本会话新增上游 9 项：onWindowDestroyed（b488f11）、Ui::requestFocus（8c103b1）、styledMenu 组件（9f21b97）、onTrayMenu（b92abb7）、onSystemPower（12c14ed）、getMonitorWorkAreas（162e3d3）、input onCancel（d5e302c）、Runtime::compose elementStructure_ move 化（aa4d5f7，代码审计 [38]）、原生菜单 showContextMenu + 子菜单路径表悬空重构（ffd859e/dcd2a15）。DevDesk MIN_REV 随批推进 12c14ed → b92abb7 → d5e302c。

## [Pet-Fix] 2026-09-23 —— 桌宠 Windows 真机四症状修复

分支 `fix/pet-window-bugs`（EUI-NEO 同名分支已合上游 main）。首次真机验证暴露
「状态对但行为错」四症状，全部先根因后修；**DevDesk 312/312 全绿**（+1 拖拽
锚点用例），EUI-NEO 46/46 全绿。根因链详见各 commit message。

### fix（EUI-NEO，D:/project/gui/EUI-NEO main）

- **症状 2 黑背景**：`GLFW_TRANSPARENT_FRAMEBUFFER` hint 只由 app 级窗口效果
  档位驱动（`windowEffect != None` 才开），设置页「关闭」档（用户实测即此档）
  → `setWindowEffect(None)` → 桌宠 sprite 窗不透明创建，全透 clearColor 落在
  不透明表面 = 黑底。新增 per-window `transparentFramebuffer`（DslWindowConfig
  → DslWindowRequest → WindowCreateRequest 三层透传），与全局档位解耦。
- **症状 3 任务栏图标**：双重根因——GLFW 的 Win32 窗口自带 `WS_EX_APPWINDOW`
  （与 `WS_EX_TOOLWINDOW` 互斥，只加不清不消失）且可见创建先注册任务栏按钮。
  修法：window_style_win 对称清/补 APPWINDOW + `visible` 创建期属性
  （hideFromTaskbar 窗隐藏创建、样式就位后 glfwShowWindow 首秀）。
- **症状 4 主窗点 X 无反应**：托盘可用时 close callback 撤回关闭并请求隐藏到
  托盘，但主循环「有子窗即取消隐藏」守卫把请求吞掉——桌宠存活期间点 X 什么都
  不发生。删守卫（模态保护在 callback 层）+ hiddenToTray 分支补子窗泵（桌宠
  动画/输入/开窗关窗照常，设计 R5「宠物独立于主窗存活」）。
- **症状 1 拖拽抖动（上游示例同款）**：examples/pet_window 拖拽改锚点跟随。

### fix（DevDesk）

- **症状 1 拖动抖动非常厉害**：onDrag 按 `DragEvent.delta` 移动窗口——delta 是
  窗口内相对坐标，窗口自身移动后参考系漂移（+10/-5/+10 振荡），且单位是帧缓冲
  像素（DPI≠100% 时差一个 contentScale）。改 `PetDragAnchor` 锚点跟随
  （getWindowPos + getCursorPosition 光标屏幕位 + 锚点，1:1、DPI 无关），
  `petDragMove/petDragEnd` 纯函数化。
- **症状 2（DevDesk 侧）**：开窗请求声明 `.transparentFramebuffer(true)`，
  `pet_start_window_request` 补透传断言。

### test

- `pet: 拖拽锚点跟随——1:1 跟光标，无反馈抖动`（旧方案必现负增量的轨迹回归）
- `pet: 开窗请求逐字段断言（无头）` +transparentFramebuffer
- EUI-NEO `pet_window_config` +transparentFramebuffer 透传/默认值断言

### 冒烟（真机，2026-09-23）

窗口样式枚举（TOOLWIN=1/APPWIN=0）、主窗 WM_CLOSE → 隐藏到托盘且进程/桌宠
存活、托盘驻留期间桌宠持续动画（截屏差分 135px）、桌宠窗口角落像素非黑
（255,255,25x，桌面透出）、桌宠 WM_CLOSE 仅关桌宠。任务栏按钮不出现、拖拽
手感、磨砂视觉三项登记人工点验。

## [Theme-Hierarchy] 2026-09-23 —— 浅色主题层级全量审计与修正（结构性面板错用卡片白）

分支 `fix/theme-hierarchy`（worktree `wt-themehier`，基线 e0b0a70 侧边栏修复后续）。**315/315 全绿**（+4 层级回归用例）。

**根因**（承接侧边栏修复）：结构性面板（页面结构的一部分）错用卡片白 `surface`（浅色 #FFFFFF），浅色模式下「页面米色、面板纯白」刺眼。层级口诀（spec §6.5 方案 B）：页面结构 → `background`（#FAF9F5 / 暖墨 #1A1816）；浮动卡片/对话框/弹层/按钮 → `surface`/`surfaceElevated`（白 / #232120 / #2C2A28）；行 hover / 分组底 → `hover`（#F5F2EC，既有 surfaceMuted 语义由 hover token 承担）。

### fix（结构性面板 → background）

- **markdown**：左栏树面板 `md.tree.bg`、编辑区面板 `md.editor.bg`、预览区面板 `md.preview.bg`、编辑器底部状态栏 `md.status.bg`、树行底（文件行/文件夹行）——全部 `surface` → `background`（行底随面板，hover 仍 #F5F2EC 显色）。
- **todo**：看板列容器 `todo.col.N.bg` `surface` → `background`；看板卡片 `surfaceElevated` → `surface`（深色回落 #232120 卡片层级，曾被无谓抬到浮层色；浅色两值同为白无视觉变化）。
- **claude**：左栏 Profile 列表 `claude.profiles.bg`、右栏详情面板 `claude.detail.bg`、最近调用「查看全部」命中区——`surface` → `background`。
- **markdown 大纲浮层** `md.outline.panel.bg` + 大纲行底：`surface` → `surfaceElevated`（zIndex 浮层语义，深色下正确抬到 #2C2A28 浮层色）。

### 审计确认无需改（防过度修正）

- **home**：统计卡 / 最近编辑卡是**浮动卡片**，留 `surface` 白（浅色下卡片白浮在米色页面上正是 Claude.ai 风格）；行底随卡。**settings**：卡片浮动该白；卡内嵌字段框（路径/Token/字段底）是控件层 `surfaceElevated`，层级正确。四模块页面底 `pagebg` 均已是 `background`。
- **EUI-NEO componentTokens 映射**（`core/component_theme.h`）：dialog 面板/dropdown 弹层 → `surface`（浮层 ✓）；dropdown 字段/input 底/segmented 轨道 → `surface`（控件层白，Claude.ai 象牙底+白输入框同款）；scrollView 不铺底（透出结构性面板底 ✓）。上游零改动。
- **深色同规则核对**：修正后层次 = 页面/面板 #1A1816 < 卡片 #232120 < 弹层/按钮/字段 #2C2A28 < hover #353231，符合 spec §6.5.1。

### test

- `tests/test_modules/test_theme_hierarchy.cpp`（+4 用例）：无头 compose 色值断言（防再漂移）——todo（列容器=background、卡片/dialog=surface、交互打开 dialog 后复验）、markdown（左栏/编辑/预览/状态栏=background，Ctrl+Shift+P 切 Split 保三栏全渲染）、claude（左栏/详情=background）、home（统计卡=surface 不得跟风变米色）；明暗两主题各验一遍。

### 人工点验项（无头测试覆盖不到的）

- 侧边栏（`main.cpp app.sidebar.bg`，e0b0a70 已修，无头 compose 不含 main.cpp）浅色米色/深色暖墨；
- 浅色下逐页目测：markdown 三栏、todo 看板列、claude 双栏、home 卡片浮起感、settings 卡片；深色下看板卡片 #232120 与列底 #1A1816 的层次。

## [PhaseC-MD-Highlight] 2026-09-23 —— 编辑器 Markdown 语法高亮（styled-runs Phase C）

分支 `feat/md-highlight`（worktree `wt-mdhl`）。上游 Phase A/B 已在 EUI-NEO main @ b7e0e9f。基线 303/303 → **310/310 全绿**（+7 高亮用例）。

### feat

- **md_line_scanner 扩展**：`scanFences(text) -> vector<MdFenceBlock>`（开栏行/内容区间/闭栏行字节偏移，``` 与 ~~~ 各自配对，未闭合到文末）；`scanHeadings` 改为消费 scanFences——大纲与高亮共用一套围栏状态机（一鱼三吃），原围栏内标题语义不变（既有用例全绿）。
- **行级高亮器** `src/module_markdown/md_highlight.{h,cpp}`：`MdHighlighter::runsForLine(fullText, lineText, lineStartByte) -> vector<TextRun>`（全文档坐标）。块级：标题 `#` 前缀、引用 `>`（正文 textSecondary + 行内 token 后写覆盖叠加）、列表 `-`/`*`/`+`/`N.`、表格管道（仅 `|` 开头行）；行内：`` `code` ``（同长度反引号配对，含双反引号）、`**bold**`（标记弱化，内容等 Phase D weight run）、`*italic*`（无 italic 字形用 info 色替代）、`[text](url)` / `![alt](url)`。围栏行整行弱化、围栏内容整行 success 色；围栏状态由高亮器内部按 display 文本重建（文本变化首回调一次 O(n)，其后的可视行回调走等值快路径）——与框架回调同源（IME preedit 快照），字节坐标不会错位。块级规则仅硬行首触发（软换行段中恰以 `# ` 开头不误判）。
- **色表** `mdHighlightColors(Tokens)`：§6.2 全部映射现有语义 token（标记 textTertiary / 代码 success / 链接+斜体 info / 列表 primary / 引用正文 textSecondary），明暗两套自动适配，未新增 token。
- **接线**：markdown_module 编辑器 input 挂 `.lineStyles(回调)`（从 InputState 解析当前 display 文本喂高亮器）；主题名变化 = 换色表 = bump `InputState.styleRevision` 触发全部可视行重高亮（compose 每帧检查，无模块间耦合）。

### test

- `test_md_highlight.cpp` +7 用例：scanFences 区间（配对/未闭合/空内容/CRLF）、块级前缀与行内 token 字节区间断言（CJK 边界、未闭合标记不误标、粗体星不误判斜体）、围栏跨行、软换行段防误判、色表映射、无头接线（行元素 runs 落色 + 主题切换 styleRevision++ 且换色）、万行性能冒烟。
- 性能实测（万行 / 2500 围栏块 / 可视窗 27 行）：首帧 1.13ms，击键轮（围栏重建 + 27 行重高亮）avg 0.98ms / max 1.53ms——预算 8ms 内 5 倍余量。

### docs

- 设计文档 §8 Phase C 状态回填；预览侧 `renderInlineRuns` 收敛为 `.runs()` 评估结论：**不做**（segment 间 fontFamily/fontSize/fontWeight 真实不同——代码段是 monospace 小字号，颜色 run 无法合并异字体段，重构手工 inline 布局 > 半天，见设计文档登记）。

## [Pet-Anime] 2026-09-23 —— 二次元桌宠皮肤（真实美术素材包）
分支 `feat/pet-anime`（基线 ad5c94d，含 merge master f6d0ac3）。**304/304 全绿**（+1 内置美术皮肤断言）。

### feat

- **内置美术皮肤 `anime-girl`**（用户调研指名的「二次元/女性角色」）：OpenGameArt
  "Anime Girl"（Michona）by Alicahordana，**CC0** 可再分发（满足素材纪律 R7，调研报告
  §3 渠道红线）。素材为 221px sprite strip（blink/walk/jump 三组），帧提取后按全局统一
  缩放规整到 128×128（脚踩底边、内容居中，保纵横比），四态映射：blink(5)→idle、
  jump(13)→poked、walk(11)→typing、闭眼帧(1)→sleep，共 30 帧 + pet.json 落
  `assets/pets/anime-girl/`（~600KB）。
- **assets 分发机制**：顶层 CMake POST_BUILD 拷 `assets/pets/` → build 目录（沿用
  fonts 同款机制）；运行期 `skinRootCandidates()` 的 cwd/assets/pets 候选零改动即发现，
  皮肤下拉/右键换肤菜单自动出现 anime-girl。

### docs

- `assets/pets/README.md`（新）：皮肤包结构 + 手动安装第三方素材指南（含 itch.io
  "Desktop Pet Sprite"（tatsu-san）的下载/解包/四态映射表——其页面为口头许可无正式
  开源协议，按「下载后放置」模式不入仓库）。
- 桌宠设计文档 §4.3 补记内置美术皮肤与分发机制。

### test

- `pet: 内置美术皮肤 anime-girl（CC0 四态断言）`：从源码树真实加载（__FILE__ 相对
  定位），断言四态帧数/128×128/全帧 PNG 结构/license 含 CC0/皮肤名合法。

### 调研结论（候选素材实测）

- itch.io "Desktop Pet Sprite"（tatsu-san，免费）——34MB PyInstaller exe，帧已提取
  （walk/run/eat/sleep/grab，128×128），许可为口头声明 → 不入仓库，走手动安装
- itch.io "Lacey Animated Pixel Doll"（kyoii011，调研报告 §3 候选）——**$5 付费**，放弃
- OpenGameArt "Anime Girl"（Michona，CC0）——**采纳内置**
- itch.io "School Girl Sprite Pack"（chibi785）——CC-BY-NC-ND 4.0，触碰红线，放弃

## [Final-Verify] 2026-09-23 —— 终验发现问题收尾批（12 项）

分支 `fix/final-verify`。基线 290/290 → **291/291 全绿**（+1 设置页布局断言；EUI-NEO 侧另加 1 个单测）。

### fix

- **模块隔离**：module_pet 直 include module_settings 的 SettingsStore（违反 module_* 互依禁令）——petSkin/petX/petY/petClickThrough 四字段从 config.json 迁到 `petsDir()/pet_config.json`，module_pet 自有 `PetConfigStore` 读写（存量旧 pet 坐标一次性丢失，桌宠回默认位置）。
- **逾期弹窗溢出**：高度封顶 640 但行全量渲染，≥13 条画到窗外——行循环按 (640-96)/44=12 条截断，提示行标注「仅显示前 N 条」。
- **设置页效果下拉溢出**：固定 x=680 在 1200 最小窗下超出外观卡 60px——字号/效果两列改右缘自适应（宽窗原位不变），补 1200 宽断言。
- **键盘流 spec 对齐（§5.2）**：Ctrl+B/I 再按去包裹（选区两端恰为 **/* 时移除）；Tab 缩进 2→4 空格（反缩进每行去 1 Tab 或 ≤4 前导空格）；顺带修 applyTab 偏移修正的坐标混用（多行反缩进选区端点偏 1）。
- **overdueDays 改本地日历日差**：昨天到期今天看 = 1 天（原秒差向上取整在 23:59 到期/次日 00:01 查看时虚报 2 天）。
- **桌宠 petX/petY 单边 -1 哨兵**：原 x/y 任一边未设置就两边一起重置——改单边独立；删除死代码 `discoverSkins()`。
- **桌宠菜单文案**：「隐藏桌宠（设置页可重新开启）」→「本次会话；重启后恢复」（设置页开关本就需重启生效）。
- **md 切笔记清空 tagInput_**；空态文案「+ 新建」→「+ 笔记」。
- **FA 残留清扫**：设置页降级提示 ⚠、Todo 弹窗标题 ⚠/卡片勾 ✓/详情按钮 ✓↩ 全部换 FA icon 通道。

### test

- Todo 拖拽防误触断言去空洞化：直接调回调时 onClick 从不派发、旧断言恒真——改驱动 EUI-NEO `InteractionState` 真实时序（press→超阈值 drag→release 回卡内 = clicked=false；对照组原地 release = clicked=true）。
- EUI-NEO `tests/unit/exit_request.cpp`（上游 main 本地 commit）：requestExit/consumeExitRequest 恰好一次语义 + requestWindowClose(nullptr) 守卫。

### docs

- KNOWN_LIMITATIONS 重复 #12 重新编号（12-16 连续）；CHANGELOG 补登波次 3+4 各批（见下节）；CLAUDE.md 测试数 137→291。

## [Wave-3+4] 2026-09-22/23 —— 波次 3+4 补登（自主时段批次汇总）

对照 `docs/superpowers/plans/2026-09-22-backlog.md` §二补记此前漏登的批次（Module-Gating / Provider-Config 已有专节，不重复）。EUI-NEO 侧上游 commit 均在 `../EUI-NEO` main（本地领先 origin，未 push）。

### 磨砂全链 Phase A–D（spec `2026-09-22-eui-window-frost-design.md`）

- Phase A：标题栏深浅主题联动（见 [W8] 条目）。
- Phase B/C（EUI-NEO 上游）：透明管线 + 运行时 `app::setClearColor`（覆盖主窗口 + 全部子窗口 + requestFullPaint）；DWM backdrop `setWindowEffect/activeWindowEffect` 运行时 API。
- Phase D（`4f6fbac`）：设置页「外观」卡窗口效果三档（off/translucent/frosted）——档位→参数纯函数（clearColor alpha + backdrop 请求 + 降级提示矩阵）、`applyWindowVisuals` 三处共用接线、透明 hint 常开三档运行时切换不重建窗口；顺手根治 KNOWN_LIMITATIONS「切主题底色需重启」（244/244）。

### EUI-NEO 上游批（DevDesk 消费）

- 拖拽取消 click（`InteractionState.dragMoved`：超阈值拖动后松手不派发 click）——DevDesk 删 Todo 卡片本地抑制逻辑（`3dba229`）。
- toast action 按钮（单按钮 + 右上 ✕）——Markdown 外部修改二选一从 dialog 改 toast action（`97db502`：重载磁盘 / 保留本地，duration(0) 强制显式决定）。
- `input.onKeyEvent` passthrough——Markdown 键盘流的通道 A（见下）。
- 按钮 disabled 自动置灰、托盘 UTF-16 乱码修复。

### Markdown 布局修复（`8cbc2ba`）

- Split 双栏 `midW` 公式笔误 + 三栏「column 铺底 + overlay stack」双占位结构导致双栏各窄 128px、预览溢出屏——改单 stack 铺底 + 内容绝对定位，补 headless layout 回归（235/235）。
- 默认视图改编辑态（左树 + 右编辑两栏，三态循环 Edit→Split→Preview）；目录树根组标题常显置顶 + 空目录空态同显；新建文件夹入口 + 新建笔记子路径提示（240/240）。

### Todo 功能补完（`a6f3742`，W7 计划 P2 三件套）

- 搜索框（真实 `service->search`，命中列照常渲染只显示命中卡，Enter/Esc 清空）；逾期视图（badge=listOverdue 数量、0 置灰，弹窗四列数据、行点击跳详情）；tags 编辑（详情弹窗回车添加/× 删除，卡片前 2 个 chip + 溢出 +N）；列色点 + 右键改色（6 预设，`setColumnColor` 落库）。纯函数 `columnDotColor/overdueDays/filterBySearch` 落 `todo_ui_logic.h`（241/241）。

### Markdown 标签系统（`d58379d`，spec §8.1）

- 服务层 `markdown_tags/markdown_note_tags` 表首次接线：`setTags` 全量替换（事务 + 孤儿清理）、`tagsForNote/listAllTags/notesWithTag`；UI 手动标签最简形态（编辑器顶栏 chip 行 + 左栏筛选，点击过滤树）+5 用例（245/245）。

### Markdown 键盘流（`9b7558f`，spec §5.2 通道 A）

- `md_keyboard` 纯函数（EditBuffer 字节偏移语义）+ `handleEditorKey` 接线（IME 预编辑防御 → 纯函数 → 写回 InputState 含 undo/textRevision/onChange）：Tab 缩进、Enter 列表续行（前缀继承/空项退出）、Ctrl+B/I 包裹、Ctrl+K 链接骨架，+15 单测；顺带修反缩进误删非前导空格。
- FA 收尾（`97db502`）：Markdown/Todo 全量 emoji→FontAwesome（码点对 FA7 Free-Solid cmap 离线核对）；`#FFFFFF` 硬编码 → `t.colors.onPrimary`。

### 桌宠 MVP（`bbac916`，spec `2026-09-22-desktop-pet-design.md` M1）

- module_pet（非 Router 页面模块）：pet.json 皮肤解析 + 内置占位皮肤 clay-cat 程序生成（PNG 未压缩 deflate 写出器零第三方依赖）；帧状态机（Idle 循环/poke 播一遍）；`app::openWindow` 子窗口（无边框/置顶/128×128/不抢焦点/不进任务栏/全透，EUI-NEO G1-G5 扩展）+ 拖拽 setWindowPos + 右键菜单（穿透/隐藏/退出）+ 持久化 + 工作区钳制；`kModuleIdPet` 门控（默认关）+ `core::paths::petsDir()`，+10 用例（285/285）。子窗口视觉呈现因企业水印层无法自动截图，人工点验。

### 打磨批（`426584a`）

- 调用时间列跨天显示（`formatCallTime`：今日 HH:MM/昨天/更早 MM-DD）；Home/Claude/设置页 emoji→FA；设置页卡片区套 scrollView（4 卡总高 580 > 小窗口可视 552，第 4 卡被裁）；`onPrimary` token；CLAUDE.md 架构图 4 模块改 5（237/237）。

### 托盘图标（`049a4f4`，波次 2 批次补记）

- `.tray` 托盘 + 四模块快速跳转菜单（EUI-NEO tray-menu PR 已合 main）。

## [Module-Gating] 2026-09-22 —— 模块插拔方案 A（编译期内置 + 运行时注册门控）

设计文档 `docs/superpowers/specs/2026-09-22-module-plugin-architecture-design.md`（推荐方案 A 为权威）；分支 `feat/plugin-gating`。最终基线 **275/275 全绿**（259 基线 + 16 新增）。

### feat

- **enabledModules 配置**：`config.json` 新增 `enabledModules` 字符串数组（缺省全启用，老配置零迁移）；`home`/`settings` 恒启用（落地页 + 开关 UI 宿主，sanitized 强制补齐），`markdown`/`todo`/`claude_proxy` 可关；未知 ID 忽略 + WARN。为此给 `platform::json` 补了最小数组读写能力（`size()/operator[]` 实装 + `makeStringArray`，原先是返回 0 的桩）。
- **注册门控**：`main.cpp getRouter()` 按 `isModuleEnabled()` 决定三个工具模块是否 `registerModule`——不注册 = onStart/compose/onKeyEvent/侧边栏入口全部不可达（结构性零执行，不靠模块自觉）；Claude 模块关闭时代理整体下线（不创建 ClaudeService、8888 不监听、`proxy_token.txt` 启动时清理陈旧文件），与 `claudeProxyAutoStart` 正交（模块开关优先）。
- **Router 加固**：新增 `isRegistered()`；`navigateTo()` 目标页未注册 → WARN + 回退 Home（不进"无页面"空白页；Home 也未注册的极端防御分支保持当前页）。Home 卡片、托盘菜单、文件打开分发（`.md`→Markdown 等）的残留跳转入口全部经此兜底。
- **Home 仪表盘降级**：统计卡/快捷按钮/最近编辑按 Router 注册状态隐藏（宽度按剩余数量现算，n=1..5；三模块全关出「所有工具模块已关闭」空态文案）；不改 HomeService SQL，隐藏纯在渲染层。
- **设置页「模块」卡**：三个工具模块各一行开关（名称 + 描述 + switch），描述注明「关闭模块后需重启应用生效」（注册是启动期行为，重启生效是设计 §5.4 的一期决策）；claude 关 → 代理卡整卡隐藏，markdown 关 → 目录卡整卡隐藏（不显示故障态）。

### test（16 新增）

- config 语义 6 用例：默认全启用/恒启用项/往返/未知 ID 过滤/空数组/损坏回退（`test_module_gating.cpp` + `test_settings.cpp`）。
- Router 4 用例：`isRegistered`/未注册导航回退 Home/负载作废/全未注册防御（既有 payload 用例补注册桩适配新语义）。
- 注册门控 1 用例：按 config 注册后关闭模块不在 `modules()`。
- Home 降级 3 用例：三模块全关/部分关/全开等价（scoped appRouter 注入注册状态的无头 compose）。
- 设置页 3 用例（含既有 openUp 用例适配 5 卡新布局改窗口高度）。

### 已知边界

- Claude 模块关闭期间不执行崩溃补还原（`recoverPendingSettingsRestore` 随 service 一起下线）：若上次退出时处于接管状态且本次以关闭 claude 模块启动，`settings.json` 保持托管值直至重新启用。见 KNOWN_LIMITATIONS。

## [Provider-Config] 2026-09-22 —— Claude 供应商完整配置（Phase A–E 全量落地）

设计文档 `docs/superpowers/specs/2026-09-22-claude-provider-config-design.md`；分支 `feat/provider-config-bc`（A/B/C，已合 master）+ `feat/provider-config-de`（D/E）。最终基线 **233/233 全绿**（226 基线 + 7 新增）。

### feat（Phase A/B/C，已随 master 合入，此处补记条目）

- **数据模型 v3**：`claude_profiles` 增 `models_config`（6 模型槽位 × 实际名/显示名）+ `extra_env`（自定义键值）两个 JSON 文本列，存量数据零迁移；上游 key 仍全走 Keychain 不落数据库。
- **代理默认不启动**（硬性需求 1）：`claudeProxyAutoStart` 配置默认 false，设置页「随应用启动代理」开关；`startProxy` 收敛为手动入口（Claude 页按钮 / 设置页重生成 token）。
- **settings.json 接管通道**（硬性需求 2+3）：代理启动 / 运行中切 profile → `buildProxyEnv()` 生成的托管 env 块合并写入 `~/.claude/settings.json`（BASE_URL→本地代理、AUTH_TOKEN→代理 token、模型映射、行为开关）；非 env 字段与用户自有 env 键原样保留；首份快照存 sidecar，停止代理 / 应用退出按键级还原；原子写 + 崩溃启动补还原（备份轮换不实现，降级取舍见 KNOWN_LIMITATIONS）；managed/project 层同名 env 键检测 → 警告提示不代改。

### feat（Phase D，本批）

- **Profile 编辑弹窗升级 tabs 三页**（760×600）：基础（名称/Base URL/API Key）+ 模型映射（6 槽位 × 上游模型名/显示名，显示名留空自动剥 `[1M]` 等标记，placeholder 实时渲染派生预览）+ 高级 env（键值行编辑器 + cc-switch/settings.json 粘贴导入）。
- **粘贴导入**：`parseImportedEnv()` 纯函数解析（整段 settings.json 或裸 env 对象两种形状；BASE_URL/AUTH_TOKEN 归基础页、模型键归槽位、其余进 env 表；值必须字符串、键名白名单、4KB/64 键上限）；解析后先预览（识别槽位数 / 别名映射 / 未识别键警告角标），确认才应用到表单；`AUTO_COMPACT_ENABLED` 等非官方别名自动映射官方变量。
- **接管状态可视**：设置页 + Claude 页详情面板显示「● 已接管 <settings.json 路径> / ○ 未接管」+ 上层覆盖警告行；Claude 页头部代理未启动时一次性引导条。
- **模型映射摘要**：详情面板「模型映射」字段改一行式摘要（默认模型 + 已配 N/6），列表卡片 model 行显示 default 槽位（未配回落旧 model 列）。

### test

- `parseImportedEnv` 纯函数 4 用例（用户真实模板全量分布 / 裸对象 / 非法输入整体拒绝 / API_KEY+AUTH_TOKEN 同现）。
- 无头 compose 3 用例：表单 tabs 槽位/env 填写→保存→DB 往返→重开回显（经 input.hit 的 `onTextInput` 驱动，等价真实键盘路径）；粘贴导入解析→预览→应用→保存全链路；设置页接管状态行随 startProxy/stopProxy 切换 + 覆盖警告命中。
- 回归：全量 233/233 全绿（226 基线 + 7 新增）。

### 已知边界（详见 KNOWN_LIMITATIONS）

- `CLAUDE_CONFIG_DIR` 自定义 `.claude` 目录一期不识别，只接管默认 `~/.claude`。
- 接管期间用户手工改托管键（BASE_URL/AUTH_TOKEN/模型键）会在下次同步被覆盖（托管键语义归 DevDesk）。
- env 热切换（运行中的 Claude Code 会话保存即生效）已有官方文档背书，真机验证留给用户人工点验。

## [W8] 2026-09-22 —— 设置页三项修复（下拉裁剪 / 日志保留即时生效 / token 居中）

分支 `fix/settings-ux`（`0efd83e`），链内基线 180/180（177 + 3 新增）。

### fix

- **下拉框被窗体裁剪**：诊断为 EUI-NEO dropdown 组件只向下展开——「调用日志保留」是设置页最后一张卡，1200×720 最小窗下 6 项弹层（≈216px）超出窗体下缘。EUI-NEO 侧新增 `openUp(true)` 构建器选项（负 y 偏移 + transformOrigin 锚底边，默认 false 完全向后兼容，已合入 EUI-NEO main `2d818eb`）；DevDesk 侧按字段绝对 Y + 弹层高度估算动态决定展开方向，大窗口行为不变。
- **日志保留切档即时生效**：`Database::pruneCallLogs` 逻辑提为自由函数 `core::pruneCallLogs(SqliteDb&, int)`（成员版委托，`main.cpp` 启动清理零改动）；设置页切档立即经 `core::appDatabase()` 共享连接清理，副标题反馈「已清理 N 条历史日志」，0=永久跳过并提示。另加 `DEVDESK_CONFIG_PATH` 环境变量覆盖供测试隔离真实 config.json。
- **token 文本框不居中**：代理认证 token 与笔记目录路径两处框内文本补 `verticalAlign(Center)`（W7 侧边栏同款缺陷）。

### test

- 720/1000 两窗口尺寸下拉弹层无头 layout 出界断言；切档 onClick 无头触发后 DB 行数 + 副标题断言；verticalAlign 断言。

## [W8] 2026-09-22 —— 代理安全加固（W8 期间并行任务）

分支 `fix/proxy-hardening`（`67aa88a` / `30fddc8` / `8dd4d3d`），链内基线 182/182（177 + 5 新增）。

### fix

- **错误 JSON 未转义（6 处）**：`proxy_router.cpp` 手工拼接的错误响应在 message 含引号/反斜杠/控制字符时产出非法 JSON；抽 `jsonEscape()`（RFC 8259）+ `writeErrorJson()` 统一出口，缓冲 4 处 + 流式 2 处全换。
- **请求体无上限（DoS 面）**：`Server` 构造设置 `set_payload_max_length(kMaxBodyBytes=10MB)`，httplib 读包阶段单点拦截回 413（spec §10.4 承诺），所有路由含 SSE 生效。
- **deleteProfile 孤儿密钥**：调序为先清 Keychain（失败中止、SQLite 行保留可重试）再删行；keychain 新增 `deleteSecretsNotIn(service, keep)`，`ClaudeService::cleanupOrphanKeychainEntries()` 启动时清理孤儿密钥（`claude_module` onStart 接线）。

### test

- 错误 JSON 转义往返解析（缓冲+流式）；10MB 边界通过 / 10MB+1 回 413；Keychain 删除失败中止+行保留+重试恢复；孤儿清理只删无主密钥+幂等。

## [W8] 2026-09-22 —— Todo 卡片拖拽真 drop 语义 + 拖动视觉反馈

基线：**178/178 测试全绿**（177 基线 + 1 新增纯函数用例；1 个既有拖拽用例改写为新语义）。

### feat

- **Todo 卡片拖拽重构为真 drop 语义**：旧实现 onDrag 阈值（横向拖过 60% 列宽）即落库，一次拖拽可连环触发多列移动且无中间态。新实现：`onPress` 记锚点（按下时卡片 bounds + 源列/卡序）→ `onDrag` 只更新悬停列（视觉反馈）→ `onRelease` 按指针落点一次性落库（`computeAppendPosition` 追加到目标列尾）。落点换算是纯函数 `ui::dragDropTarget`（todo_ui_logic.h）：以按下卡片 bounds 为锚，dpi 用 `bounds.width / compose 卡宽` 探测、板面原点用卡片左缘回推，同时消掉指针像素空间与 compose 空间的缩放和偏移差；列内滚动偏移一并还原（命中框跟随滚动 transform）。落点在列间隙 / 板外 / Cancel / 源列 → 取消，卡片回原位。顺带消灭旧缺陷：落库只依赖稳定 taskId，不再使用 reloadAll 后会过期的列/卡索引。
- **拖动视觉中间态**：被拖卡片整体半透明（opacity 0.45，含优先级条 / 标题 / chip / 打勾）；指针悬停列边框高亮（2px primary）。拖拽松手回落在本卡上不误开详情弹窗（onClick 抑制，运行时 onClick 先于 onRelease 派发）。
- **删除死代码 `core::drag::DragController`**：审计确认无任何调用方；其"平行拖拽框架"需要应用层指针接线，而 EUI-NEO 元素级 `onPress`/`onDrag`/`onRelease` 回调已直接提供同等数据，无法复用，删除 .h/.cpp 与 CMake 条目。

### test

- **`todo_ui_drag_hit_pure`（新增）**：`dragDropTarget` 纯函数——命中/列间隙/板外/板上下方/HiDPI（dpi=2 全坐标翻倍）/列内滚动偏移/退化输入。
- **`todo_ui_move_appends_to_end`（改写）**：真实 layout 帧驱动完整拖拽序列（press → drag → release），断言中间态（被拖卡 opacity 0.45 + 目标列 border 2px + 源列不亮）、松手落库追加列尾、拖出板外取消、落回源列取消、拖拽松手不开详情弹窗（dialog 常驻 compose，断言 panel opacity）。

## [W8] 2026-09-22 —— Markdown 左栏重构（二级目录树 + 大纲浮动面板）

最终基线：**184/184 测试全绿**（W5 后 177 基线 + 7 新增）。分支 `feat/md-tree-outline`（worktree `wt-w8-md`）。

### feat

- **`4019b85` 左栏二级目录树**：新增 `md_tree.{h,cpp}` 纯函数 `buildTreeRows`——顶层 = 一级子目录（字母序），二级 = 组内文件（mtime 倒序），根目录散文件单独一组排最后，更深层文件挂在一级目录下显示剩余子路径（二级树不递归）。文件夹可展开/收起（内存态，不持久化）；搜索时树展平为命中文件列表。用户原话：基本 UI 就是左侧目录树（二级，文件夹-文件）、右侧展示编辑区域，勾选大纲后浮动展示大纲可点击。
- **`4019b85` 大纲浮动面板**：移除左栏「文件/大纲」页签，改为顶栏 switch 开关；面板 264px、`positioned + zIndex(10)` 浮在编辑/预览区右侧，不挤压布局；沿用 Phase 1 大纲逻辑（md_line_scanner 扫标题、当前节高亮、点击跳转 InputState.cursor + followCaret）；✕ 可关闭。
- **`4019b85` 文件变化自动感知**（spec §8.1 承诺的 5 秒轮询）：`MarkdownService::pollChanged()`（轻量 mtime/size 快照比对，不读内容不写库不打日志）+ `fileMtime()`；模块层 5 秒节流轮询，有差异才 `rescan()`——新文件自动出现在树里、删除自动消失，「↻ 重扫」按钮随之移除。当前笔记被外部修改：无本地编辑 → 静默重载；有未保存编辑 → dialog 二选一（重载磁盘版本 / 保留本地；EUI-NEO toast 无按钮能力，选择类交互用 dialog）；外部删除 → toast 提示（`lastKnownMtime=-1` 标记防 5 秒重复弹，dirty 时保存会重建文件）。
- **`4019b85` 新建笔记路径校验**：`isSafeNewNotePath` 拒绝绝对路径、盘符（`X:`）、`..` 逃逸、反斜杠；模块层前置校验给友好提示，`MarkdownService::create` 同规则兜底（trust boundary 在服务层）。

### fix

- **`4019b85` 左栏/大纲行渲染 position 静默失效（W7 已知坑实锤）**：文件行/大纲行的 rect 原是叶子元素，行内 icon/name 的 `position()` 在 scrollView column 流式布局中被忽略，图标和名字会被流式排成独立行。改为整行包 `ui.stack`（行内元素 position 定位）——与 W7 Todo 列同一修法，无头 layout 级测试断言缩进与行内坐标。

### test

- md_tree 纯函数 2 例（分组/收起/深层文件）；`isSafeNewNotePath`/`create` 兜底/`pollChanged+fileMtime` 3 例；无头 compose+layout 2 例（目录树展开收起与缩进 `frame.x` 断言、大纲面板 zIndex/右侧浮层/编辑区不挤压）；既有 Phase 1 / W7 用例同步到新 UI（页签→开关、树行 id）。
- 踩坑登记：`REGISTER_TEST` 宏按逗号切参，测试体里的 vector 初始化列表会破坏宏展开（报 "passed N arguments"），需用 `push_back`。

## [W8] 2026-09-22 —— 标题栏深浅主题联动（磨砂 Phase A 先行）

最终基线：**178/178 测试全绿**（177 基线 + 1 主题联动）。

### feat

- **标题栏深浅主题联动（EUI-NEO 上游 + DevDesk 接线）**：EUI-NEO 新增 `core/platform/window_effect.{h,_win,_stub.cpp}`（`TitleBarAppearance` + `applyTitleBarAppearance`，Windows 走 `DWMWA_USE_IMMERSIVE_DARK_MODE`，Win11 22000+，老系统静默降级返回 false）、`DslAppConfig::darkTitleBar(bool)` 启动快照、`app::setTitleBarAppearance/currentTitleBarAppearance` 运行时 API（即时生效，覆盖主窗口 + 全部存活子窗口 + 之后新开子窗口）+ `examples/window_effect.cpp` 演示 + 单测，已合回 EUI-NEO main（feat 34b21f2，本地未 push）。DevDesk 侧 `core::theme` 增加变更回调（core 不依赖 EUI-NEO 的架构铁律下由 main.cpp 注入 `app::setTitleBarAppearance`），启动主题恢复与设置页运行时切换走同一路径；`dslAppConfig` 补 `.darkTitleBar(theme::current().isDark)` 快照。测试 +1（theme 变更回调 mock 断言）。设计文档：`docs/superpowers/specs/2026-09-22-eui-window-frost-design.md` Phase A。

## [W5] 2026-09-22 —— 遗留项清零（三链并行 + 集成）

最终基线：**137/137 测试全绿**（124 基线 + 6 Router payload + 2 Claude UI 冒烟 + 5 字号缩放/Router 集成）。

### feat

- **`c3901a8` 跨模块跳转负载（w5-payload 链）**：`Router::navigateTo` 增加可选 payload（默认空，既有调用零改动兼容），`consumePendingPayload(PageId)` 一次性消费语义（取出即清空、页面隔离、纯导航不清挂起负载）；Home「+ 新建笔记 / + 新建任务」带 `action:*` 负载在目标页触发既有新建弹窗，「最近编辑」行点击带 `open:<路径>` 负载直接打开指定笔记。测试 +6。
- **`843cb2c` Claude 模块操作按钮全量接线（w5-claude-ui 链）**：claude_module.cpp 从 0 个 onClick 到全量真接 `ClaudeService`——新建/编辑（dialog 表单，编辑预填、Key 留空=不换）、启用（setActiveProfile）、删除（确认 dialog + Keychain 清理）、测试连通（testConnection + toast）、代理开关（startProxy/stopProxy）。测试 +2（无头 compose 冒烟）。
- **`90d5792` 侧边栏导航 + 全局字号缩放 + UTF-8 argv + clearColor 跟随主题（w5-nav-global 链）**：main.cpp 自绘常驻侧边栏（260px，5 模块入口，当前页高亮），移除 T11 浮动齿轮桥接；`core::theme` 增加 `fontScale`（钳制 [0.5,2.0]，token 读取层统一施加、重建后重放不叠加），设置页字号档位改写全局缩放；`file_open_win.cpp` 改 `GetCommandLineW + CommandLineToArgvW` 宽字符 argv 转 UTF-8（非 ASCII 路径不再乱码，链接补 shell32）；`clearColor` 构造时从持久化主题快照（运行中切换主题窗口底色仍需重启，EUI-NEO 无运行时 API，见已知限制）。测试 +5。
- **`317bcf6` / `68182cc` / `0a20a65` 三链合并集成**：w5-claude-ui、w5-nav-global、w5-payload 依次合并回 master，合并后全量 137/137 全绿。

### docs

- **（本波收尾提交）**：KNOWN_LIMITATIONS 已修条目移入「已解决」存档小节（含修复 commit）；USER_GUIDE 导航改为侧边栏描述、代理配置流程改为全 UI 可完成；恢复计划追加 W5 段；CLAUDE.md 缺口表同步。

## [W3] 2026-09-22 —— 剩余需求（T9–T12 + P16）

最终基线：**124/124 测试全绿**（109 基线 + 2 Home + 10 Settings + 3 JSON）。

### feat

- **`883d681` P12 Home 真实数据仪表盘**：新增 `HomeService`（今日笔记 / 待办 / 逾期 / 今日 API 调用 / 最近编辑前 5，只读聚合 markdown_index / todo_tasks / claude_call_logs）；4 张统计卡 + 最近编辑列表接真数据并可点击跳转；快捷按钮接线（新建笔记 / 新建任务 / 切换 Profile，跨模块禁依赖故降级为导航）；`Database::pruneCallLogs` 启动时滚动清理调用日志；`core::Router` 增加 `registerAppRouter/appRouter()`。测试 109 → 111。
- **`a18c8a8` P13 设置页**：新增 `module_settings`——主题切换（下一帧全页生效）、字号缩放档位（85–130%，当前仅作用于设置页自身，见已知限制）、代理 token 只读显示 + 重新生成（同端口重启代理并重写 `proxy_token.txt`）、Markdown 目录只读展示、日志保留天数（0–3650，0=永久）；`SettingsStore` 读写 `%APPDATA%\DevDesk\config.json`，文件缺失 / 损坏 / 越界一律回退默认或钳制。设置入口为 main.cpp 顶层浮动齿轮按钮（应用无侧边栏）。测试 +10。
- **`750b7fa` W3 三链集成接线**：调用日志清理天数由硬编码 30 改为读 `config.json` 的 `logRetentionDays`（默认 30，0=永久时跳过清理——`pruneCallLogs(0)` 会删光日志，必须短路）。隔离 APPDATA 种子验证：retention=7 清掉 40 天前日志、retention=0 旧日志完好、Home 五项统计与种子吻合、objdump 零外部 DLL、启动冒烟五模块齐启。

### fix

- **`50ff663` 启动顺序理顺**：`startClaudeProxy()` 挪到 `getRouter()` 之前，模块 `onStart` 时 `ClaudeService::global()` 不再为 null，消除启动 WARN；代理只由 main 启动一次；删除 `proxy_router.cpp` 的 `#if 0` getHeader 死代码（全仓库零引用）。
- **`1f273f9` platform::json 实装**：`parse()` 此前是无条件返回 null 的桩、`stringify()` 一律输出 `{}`；实装 yyjson 递归解析（失败返回 JsonParseError）+ 自写 RFC 8259 序列化（yyjson 共享库被 EUI-NEO 以 `YYJSON_DISABLE_WRITER=1` 编译，writer 不可用）。测试 +3。

### docs

- **T12（本提交）**：USER_GUIDE + KNOWN_LIMITATIONS + README + CHANGELOG + P16 维护评估；恢复计划收官。

## [W2] 2026-09-21/22 —— 平台架构 + P14（T6–T8）

### feat

- **`a534f5d` Windows DPAPI keychain**：`CryptProtectData/CryptUnprotectData`（用户级，禁弹 UI）+ 密文落盘 `%APPDATA%\DevDesk\keychain\<service>\<account>.key`；T4 记录的 19 个连锁失败（keychain 2 + claude_service 10 + proxy_router 7）**全部转绿**，103 例基线全绿。

### refactor

- **`14a2373` 平台适配接口隔离**：keychain / paths / time / file_open 四族平台 API 按平台拆文件（`*_win.cpp` / `*_macos.cpp` / `*_posix.cpp`），CMake `target_sources` 按平台选择；`core::time::localTime` 统一 3 处散布的 `#ifdef`；业务代码零 `#ifdef _WIN32` / `__APPLE__`（grep `src/` *.cpp/*.h 零命中）。纯重构零行为变化（103 例 84/19 与 T4 基线一致）。

### test

- **`458cf50` P10b 流式压测**（6 例，103 → 109）：
  - 10MB 流式 e2e：44ms、1440 次 receiver 调用，全串比对 + FNV-1a 哈希 + 160 块块号三重校验一致（**吞吐实测约 240–320 MB/s**）；
  - 10MB 缓冲路径：48ms 字节一致；
  - soak 60s：1200/1200 chunk（**0% 丢失**，验收口径 < 5%），seq 严格递增；
  - soak 300s：`DEVDESK_SOAK=1` 门控，常规套件打印 `[SKIP]`；
  - 断连恢复：上游中途 `closesocket`，流以 End 干净收尾，已收字节与上游已发完全一致（直接层 + 端到端两场景）；
  - 内存采样：20 轮 × 10MB 流式，增量 12,660 KB（阈值 102,400 KB），无泄漏趋势。

### docs

- **`0a76935` W2 验收**：spec §16 P14 升必选 + 发布形态约束（单 exe 零外部 DLL）；计划文件 T7 / T8 附实测数字；CLAUDE.md 缺口表更新。

## [W1] 2026-09-21 —— 止血收编 + 发布形态（T1–T5）

### feat

- **`a1a106a` 收编 macOS 中断现场 P9/P10a**：流式代理（SSE chunk 透传）全部修改 + `streaming_proxy.*` + 3 个新测试文件 + Windows 迁移现场快照（含平台迁移现状文档与本恢复计划）。

### fix

- **`90f2d77` / `642c257` Windows 测试基建**：`filesystem::remove` 抛异常 + sqlite 句柄未关导致的套件崩溃修复，test_main 容错（单测异常只挂本用例不炸套件）；补提 claude_service / proxy_router / streaming_proxy 共 32 例。
- **`5460487` Windows 干净编译**（T2）：`json.h` 补 `<cstdint>`；platform 链接 `ws2_32/crypt32`；`CMAKE_CXX_STANDARD` 升 20（designated initializers）；`proxy_router.cpp` const 引用绑临时修复；`-Werror` 收窄到 DevDesk 自己的 target（不再压 EUI-NEO，曾产生 517 个第三方假错）；FTS5 检测补 SQLite3 库链接（修假阴性，CJK 搜索测试全过）。验收：删 `build/` 全新 configure **零 workaround flag**。
- **`ff3ce3d` / `fefc6ab` / `e4c2cb3` P10a 流式修复**（T5）：缓冲转发路径也走注入的 profileProvider；上游截断（`CURLE_PARTIAL_FILE`）按流干净结束而非报错；背压改为 write callback 内阻塞（取消不再跨线程 `curl_easy_pause`）。14 个 streaming 测试全绿。
- **`11d474c` mock 上游 SSE 服务器**：补 releaser 兜底 + `is_connection_closed` 双保险，client disconnect 用例真实可观测。

### build

- **`95c59d5` 单 exe 零外部 DLL**（T3）：`DEVDESK_STATIC`（默认 ON，仅 MinGW）——SQLite3 / CURL / OpenSSL / zlib / C++ 运行时全静态，libcurl 传递依赖闭包按 GNU ld 单遍序补齐（ssh2/nghttp2/nghttp3/brotli/zstd/…）；Release 摘除 EUI-NEO 强加的 `-Os` 规避 GCC 16.2 constexpr std::string C4 mangle bug。验收：`objdump` 仅 16 个 Windows 系统 DLL（Debug+Release 双验证）、无 MSYS2 PATH 干净目录可跑、**Release 15.4 MiB（16,152,064 字节）**。

### test

- **`8dfc8a9` 测试入口增强**：devdesk_tests 支持按子串过滤（`./devdesk_tests.exe stress`）与 `--log` Debug 级日志。

### docs

- **`3700ce1` W1 验收**：T1–T5 完成标记与实测数字归档。
