# DevDesk 未完成任务清单

> **2026-10-28 全文清理（本机核对）**：本机此前落后 `origin/dev` 179 个提交（主开发在另一台机器），已同步至 `40bd1d7e` 并以其为准逐条核对重写。EUI-NEO 同步至 `fork/main @ 3ee91da`（上游 `origin`=sudoevolve 只读不推，决策不变；fork 领先上游 132 提交为长期状态）。
> 权威台账：`docs/superpowers/plans/2026-10-21-UAT62-整改清单.md`；当前状态以 `CLAUDE.md`「🚦 当前状态」段为准。
> **规矩（沿用）**：用户口述的需求当次就写进本文件，否则必丢。
> 清理前的旧版全文（2026-09-28 历史快照、§13 需求与决策记录、各会话收尾清单原文）：`git show 40bd1d7e:TODO.md`。

## 🔴 活跃待办

### A. memcpp 线（2026-10-28 收线）

> 已交付部分见 `docs/superpowers/specs/2026-10-28-memcpp-overview.md`（需求→实现→验证映射）；本段只记**未完成项**，按优先级排。

**P0 — 用户已明确要求，下一步先做**

- [x] **MCP 集成（用户 2026-10-28 需求，纯增量接入）**（2026-10-29 交付：PRD `docs/superpowers/specs/2026-10-29-memcpp-mcp-integration.md`，`memory_search` 工具 + MCP 页记忆行开关 + 运行时下线桥，全量 1927/1921/0，审查复核通过；F-08 真连通道后置与下条 UAT 同场）——Claude Code 经统一入口查记忆（recall/search 工具走既有 `module_memory/gateway/telemem_client`），MCP 页面展示记忆模块行+可开关（关=工具不注册/下线）。**红线：既有功能（hook 注入/面板/采集/卡片）一行不动**。前置：读 module_mcp 的模块注册模式与开关持久化；其线 B4 真连 E2E 是他们自己的欠账，不阻塞注册接线。（2026-10-28 核：`mcp_tools_builtin.cpp` 尚无 memory 工具，未开工）
- [ ] **打包+真机 UAT（S5 收官）**：`bash scripts/package-windows.sh` 出安装包 → 用户安装 → 首跑向导（Python 3.10~3.12 检测/venv/pip 约 97s/引擎拉起）→ 真机点验：自动采集（30s 轮询）、脱敏拦截（动态流可见）、提案卡审核+撤销、Claude Code 提问时 hook 注入（≤5 条 ≤500 字）、消耗页、触发线仪表（冻结 15 查询评测）。

**P1 — 已知缺口（不影响 Windows 日常使用）**

- [ ] **infer=true 云端提炼真调**：当前全部直存（infer=False，零 LLM 调用）。智能提炼需真调智谱 anthropic 端点 glm-5.3-flash（key 运行时读 `~/.claude/settings.json`，TELEMEM_CONFIG 已配 anthropic provider）。注意：套餐 key 用于后台常驻提炼偏离智谱使用条款，用户 2026-10-27 已知悉自担；控耗五条（节流/月度预算软硬档）已实现，实测验证待做。
- [ ] **月度预算硬档实测**：预算耗尽 → 暂停提炼只留检索的面板行为真机验证。
- [ ] **decay 遗忘清单真实现**：待 telemem 引擎暴露记忆列举缝（MemoryListFn）；当前 fail-closed 零假卡（扫描触发在，清单为空）。
- [ ] **baseVersion live 空转**：telemem 无版本查询缝，接受时以快照放行（契约位已留）；引擎升级后接真值。
- [x] **memory_e2e_probe 门禁注入通道**（2026-10-29 处置：改走 live 标签——`tests/CMakeLists.txt` LABELS "live" 挪出 probe 门禁，与 github_sync_live/agent_live 同款；验收走 `docs/superpowers/plans/2026-10-28-memcpp-e2e-verify.md` 手工 runbook，替代原计划的脚本注入开关方案）：探针需外部注入 DEVDESK_DATA_ROOT+外部编排 llama-server/telemem，当前在门禁属环境性失败；给 build-test-run.sh 加注入开关。

**P2 — 平台与打磨**

- [x] **macOS 平台面**（2026-10-29 销账：`memory_process_host_spawn_and_reap`（tests/test_modules/test_memory_engine.cpp:339，非 Windows 真拉起进程）本机 mac 全量多轮真跑绿——今天 3 轮全量含此用例 0 失败；agent 域 process_runner macos/posix 另由 3d59d076 mac 实测 133/133。QA 复核销）：`process_host_posix`（含 F2 修复：per-child 环境不污染父进程）仅编译验证，待 mac 实机；引擎托管 posix 侧同。
- [x] **批量模式标签窄窗重叠**（2026-10-29 销账：切图仔 commit e088aaef 合 dev 602feb61——1000 窄窗 chips 折两行（工具条 56→88 高）、1200+ 单行、批量开关恒右锚；根因=真渲染字宽 > 无头估宽，原契约「三档全单行」按证据改。QA 独立复核：增量构建 602feb61 后工具条 3 条全绿、chips 5 条全绿、memory 定向 0 FAIL）。
- [ ] **向量模型升级档**：记忆量增大（数百条+）后可升 Qwen3-Embedding-0.6B（610MB/约 1.4GB RSS，难集满分）——**必须显式 `-c 8192`**（默认槽位会吃 4GB）；改 `kEmbedModel` 常量+重启引擎即可，见 `docs/superpowers/plans/2026-10-27-memcpp-embed-bakeoff.md`。
- [ ] **触发线升级 C**：冻结集转测 Recall@5<85% 或 2 字词命中低于 5/3/11 → 按备用图纸（`docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md`）补自研全文通道；当前实测 100%（15/15），余量充足。

**环境事实（接线时会用到）**

- 模型文件收线时已删（曾下载于 spike/.bakeoff/）：bge-small-zh Q8_0 25MB（HF: CompendiumLabs/bge-small-zh-v1.5-gguf）+ Qwen3/bge-m3 各 640MB；`tools/memcpp-integration/spike/run_bakeoff.py` 内有下载 URL，重跑即取。
- spike 全套脚本在 `tools/memcpp-integration/spike/`（run_spike.py 五项实测可复跑）。
- 端口约定：llama-server 回环 11435、telemem 回环 8421；本机 http_proxy 会截胡云端调用（脚本已做 NO_PROXY 幂等旁路）。
- hook 注入通道：`devdesk.exe memory hook` 子命令（先于单实例锁分派）；hooks 配置键级装拆走 DevDesk settings 管理通道。

### B. 调度线收口（2026-10-29 批次 1/2 + 审计 A 后重写；审计报告：`docs/superpowers/plans/2026-10-29-schedule-audit-A-report.md`）

**已销账（2026-10-29 批次 1/2，未提交）**：引擎启动接线 ✓（onStart 挂 1Hz 泵 + 全局令牌桶 18/min 两级串联 + flushAllToMainThread 补调用）；运行历史抽屉接线 ✓（openHistory + 常驻 drawHistoryDrawer）；wecom 渠道注册 ✓（内置五渠道；**v1 裁决：单 wecom 渠道行**，per-row 装配 + keychain 写入 UI 记 backlog 等编辑弹窗波）；设置页「定时」开关卡 ✓（gatingKey + 迁移 v2 存量保持开启）；toast 反向依赖 ✓（toast_notice_bridge 函数指针缝）；DST 用例真跑 ✓（ScopedTz 切纽约，2026 真实跳变日钉死；Windows 侧 IANA 名不识别恒 SKIP 诚实兜底，真覆盖记债）；审计 A ✓（重定范围完成，5 P1 + 14 P2）；审计 B 4 条（链接重复行已修，余 3 入下）；附带真修：parseDigits 从不写出、fullscreen 开窗刷新死路、specStringArray 数字分支、flush 早退。

**剩余项（按审计 A 批次规划）**：

- [x] **R3·编辑弹窗接线（spec §4.2 欠账，最大项）**（2026-10-29 销账：已交付 + QA 全绿，commit 29a61928）：`drawEditDialog`/`saveRule` 已挂载生产调用 + actionJson 三字段输入（remind_open 的 deeplink / run_task 的 key，`schedule_ui_edit.h:77` 键名与 `action_runner.cpp:60` 对齐）+ P2-6 摘要键名（nth_weekday/month_day）。
- [x] **R2·双真源与断链**（2026-10-29 销账：全量收口 + 交叉审通过，commit 1b97ab4a、1d3e342c）：internal_task_registry 死真源删除 + run_task 任务源现状核实；暂停分支旧到期点对齐；completeAsync 死代码；slotBusy 不计次；memory_probe `--size` 口径。
- [x] **R4·小修与文档**（2026-10-29 销账：两波收口，commit 2a2f2e5f，合 dev 80a4446a，先红后绿证据齐，调度域 88/88 绿）：老库（无 schedule 表）首次升级用例补齐；calendar 400 年契约收敛 ok(nullopt)；handoff §0/§11 快照刷新；P2-12 边界登记（skipped 无人写 / 崩溃遗留 running 悬挂 / 90 天保留无清理 / max_runs 双不消费）。
- [ ] **Windows DST 真覆盖**：ucrt 不识别 IANA 时区名 → 恒 SKIP；需 Windows tzdata 分发或引擎时间源注入（需 Windows 机）。

**历史销账（2026-10-28 核）**：channel_registry 源登记 ✓；CMake `devdesk_platform`「重复行」✓；13→14 页终态序重钉 ✓（27807adb）；调度引擎对齐 ✓（40bd1d7e）；S7a 装配+列表 ✓（993e2c5b）。

### C. 上线（release）线收口

- [x] **整批零构建/零测试实测**（2026-10-29 销账：波次 A/B/C 全量回归 1915→1927 条全绿 + Debug/Release 双形态构建零错误 + 5 道静态守卫全过，见 `docs/superpowers/plans/` 波次交接与 `evidence/`）：合并进 dev 的调度 S1–S7a + H27–H31 + release 线代码从未构建、未跑测试（当时 CI 验证环节被用户取消，且等 EUI fork HEAD 修复——**EUI-NEO 现已同步 3ee91da，此前置已解除**）。**首个实测点 = 本机（Mac）下次构建**，红了按失败项修。macOS 侧此前仅静态审计，无实机编译。
- [ ] **安装程序 + 真机 UAT**（卡点：老板 commit/push 与装机授权未放，git 与覆盖安装由 coderstory/老板操作；代码侧测试闸已过——2026-10-29 全量 1954/0 + probe 22/22 + 守卫全绿，QA 复核）：0.5.0 出包（Inno Setup / DMG）+ 用户真机 UAT（与 memcpp P0-2 是同一件事，一处收口）。
- [x] **A-02 已核实销账（2026-10-29，产品经理核）**：列表与详情同源 `taskProgress()`，无口径冲突——列表卡 `release_list_ui.cpp:838` 与详情页头 `release_detail_ui.cpp:2615` 调用同一函数，分母 = 每服务 3 固定步骤 + 后端每服务 5 配置槽位（`release_logic.cpp:292` `expectedConfigs(Backend)=5`）+ 任务级前置 2~5 项；详情页环境卡为环境维度分层展示，非冲突。无需拍板。
- [x] **置顶泄漏 BUG（临时插单，2026-10-29 销账）**：老板真机现象「主窗无法最小化 + 窗体置顶」，根因 = `release_reminder.cpp` 全屏提醒窗 `alwaysOnTop(true)` 泄漏到主窗（`09d8aa62` 修复）；设置新增「上线提醒弹窗」开关默认关。QA 定向回归全绿（gate 守卫 4/4、release 60/60、schedule 86/86），清单卡 `docs/plans/release-popup-gate-tests.md`。生效待下次 0.5.0 出包。
- [ ] **S8 系统定时只读源**：ScheduleProvider 接口文件已在（`schedule_provider.cpp/h`），release 线作为首个来源的接线待做。
- 缓置（有意不做/后置）：F-3 父行存在性预检（导入/脚本入口出现时再补）；提醒窗句柄耦合块不抽纯函数（openReminder/closeForEmpty/collectReminderView/buildDemoView/绘制，硬抽会改行为）；cron 编辑回显为 v1 有意取舍（`schedule_ui_edit.cpp:371` 注释）。
- **已销账（2026-10-28 核）**：release 三页已并入 dev（ReleaseGroup/Tasks/Config，`app_registry.h:61-63`）；`feature/20261027-release-module` 文档分支已并入 dev。

### D. 壁纸线遗留（壁纸本体已交付 4ad41e52，verifier 8/8，用户确认）

- [ ] **退出 terminate 尾巴**（2026-10-29 老板拍板：不专项追——不倒腾机器，随那台 Windows 机下次使用顺手上行提交）：根因 = `src/platform/single_instance_win.cpp` 单实例监听线程 joinable 活到静态析构（std::terminate），非壁纸问题、属既有债。修复代码已写在**另一台 Windows 机** `wt-dev-gate` 工作树（`shutdownListener()` + `std::atexit`，增量构建绿）——**未上行未验证，本机没有这份代码**。Windows 侧收尾口径：验证托盘退出路径（X 关闭是隐藏不是退出；退出码 0 + stdout 无 terminate 行，双次确保确定性）→ 原子提交 `fix(exit): 单实例监听线程 atexit join 收尾——消 terminate 尾巴` → 推 dev。（2026-10-28 核：origin/dev 无此修复，`shutdownListener` 零命中）
- [x] **agent_e2e_probe 环境红**（2026-10-29 销账：本机生产库已有激活 Claude Profile，probe 层 22/22 全绿含 agent_e2e_probe Passed 85.75s（QA 复跑 job `bash-3a9a8f35d5ec4b3e` 实证），前置不再缺失。不碰代码）：本机生产库无激活 Claude Profile（探针设计「前置缺失=FAIL 防假绿」）——激活任一 Claude Profile 后重跑 `ctest -L probe` 即 19/19。不碰代码。
- [x] **首页顶部 5 张统计卡偏大**（2026-10-29 销账：用户拍板改小+字放大，方案已验收、改完合并 dev）：改动入口 `src/module_home/home_module.cpp` 统计卡区，卡高 144→96、数字 28→44px 加粗、说明行并入标签行；H18 弹性双档断言同步（tests/test_modules/test_home.cpp）。
- deferred（按需求书，非欠账）：图片壁纸四铺法（WIC 平铺）后续增强；macOS 平台层规格 P8，代码未写，勿宣称可用。永久不做：多显示器/转码降帧/维护删除当前壁纸/玻璃半透/独立应用。

### E. 历史 §13「profile 高级配置」终态记录（已闭环——勿再当开放需求）

- 2026-09-28 用户提出需求并逐条拍板 8 项决策（高级配置整块接管、写入三档、非 env 快照还原、片段赢浅合并等）→ 2026-09-30 按 `docs/superpowers/specs/2026-09-30-advanced-json-editor-design.md` **实施交付**（merge a1451e9，独立审计 10 findings 全处置；sidecar 记账扩 `nonEnvKeys`/`nonEnvSnapshot`——原「🔴 高风险点：快照只含 env」即由此解决）。
- **后被产品决策整体删除**（UAT5 批）：b232398c 删高级设置（高级配置页/全屏 JSON/接管合并）、026cbd71 删测试适配、2e354824 删 schema 遗留列。现行口径：**接管不写非 env 顶层键，仅保留旧 sidecar 的 nonEnv 记账用于还原**（`settings_sync.cpp:309`）。
- 用户 2026-10-28 确认「高级设置是后续删除了」。若要恢复此能力 = **新需求**，当次重新记录，勿引用旧 §13 决策为现行约束。

## 🟣 AI 开发体系债（2026-10-28 agent 体系沙盘推演产生）

- [ ] **UI 交互链路自动化验证缺位**：qa/研发可自主实证启动/截屏/日志/数据/HTTP，但点击/输入类交互无法自主执行（当前按 AGENTS.md §八如实标「交互链路未验证」）。长期解法：给 DevDesk 建测试自动化驱动入口（CLI/IPC 触发页面与操作），让 qa 能走完整用户流程。
- [x] **AGENTS.md §二平台文件统计过时**（2026-10-29 销账：魔女已更正，commit 3a94749b）：平台文件实数与 module_memory / module_wallpaper 记载已补齐。
- [ ] **`build/DevDesk.app` 陈旧 bundle 陷阱**：构建脚本常规路径不重组 .app bundle（脚本注释自认），产物是 `build/DevDesk` 裸二进制——直接启动旧 bundle 会得出「新功能不存在」假象（2026-10-29 MCP 集成验收实际踩中）。修法候选：构建时清理陈旧 bundle，或验收/打包口径钉死 `--install`/裸二进制（待用户选）。
- [ ] **process_runner 三平台共同待办**（2026-10-29 交叉审 3d59d076 登记，P2 级不单独派工，下次触碰时修）：① 收尸后 exited=true 超时不再生效，管道被孙进程持有则 EOF 永不来——三平台统一补「收尸后排空加有界宽限（timeoutMs/2）」（process_runner_macos.cpp:268、posix:189）；② poll revents 只认 POLLIN|POLLHUP，POLLERR/POLLNVAL 不排水不关 fd（process_runner_macos.cpp:247-262）；③ posix waitpid 失败 break 静默吞 ECHILD，与 macos 显式 fail+留痕口径不齐（process_runner_posix.cpp:204）。
