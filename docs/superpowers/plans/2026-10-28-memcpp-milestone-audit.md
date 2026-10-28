# memcpp 里程碑收口审计（并 dev 前唯一审计）

- 日期：2026-10-28。审计者：独立审计子代理（只读，未参与本轮任何改动）。
- 对象：feat/memcpp @ `9ee92079`（D:/wt-memcpp）。
- 独立性自检：派单书写明实现者分支 feat/memcpp；本审计为独立会话，未参与实现/测试/e2e，非延续会话——审计有效。
- 参考基准：`docs/superpowers/specs/2026-10-28-memcpp-overview.md`、`2026-10-27-memcpp-integration-spec.md`、`plans/2026-10-28-memcpp-e2e-verify.md`、`plans/2026-10-28-memcpp-s2-live-verify.md`。
- severity 口径：按维护成本/演进代价定，不按"会不会崩"。

## 一、六维度逐条结论

### 1. 正确性 —— 已核，2 项 finding（F3/F4/F6）

- **脱敏闸 fail-closed（主线①）**：`sanitize/sanitize_gate.cpp:26-63` 严格 UTF-8 解码拒截断/过长/代理区；`scanText:726-733` 非法输入返回 Error（fail-closed 入口）；`SanitizeGate::process:802-826` 把 Error 原样上抛、审计行记 `sanitize:rejected`。契约头 `sanitize_gate.h:8-9` 明文"禁止把 Error 当无命中"。单测 `test_memory_sanitize.cpp:114-124` 钉死三类非法输入必 Error。闸本体合格；但产品侧无出机调用点（见 F1），闸当前无守卫对象。正则变异验证见维度 5。
- **hook 注入契约（主线②）**：`inject.cpp:50-58` additionalContext 只在 `hookSpecificOutput` 内、`hookEventName` 同层（单测 `test_memory_hook.cpp:54-69` 钉位置，顶层无键）；预算 `assembleContext:32-48` ≤5 条（kMaxItems）≤500 字（kMaxChars）双界；`hook_command.cpp:24-71` 五条错误路径（空 stdin/解析失败/非目标事件/检索失败/空结果）全部空输出+exit 0；`main.cpp:642-665` 子命令分派先于单实例锁、`_Exit` 跳 atexit、恒 `return outcome.exitCode`（恒 0），全链无 exit 2 出口。已核：通过。
- **proposals 状态机（主线③）**：undoOf 唯一 = partial unique index（`proposal_store.cpp:42-43`）+ 复用刷新路径（:541-583）+ 单测复用断言（`test_memory_proposals.cpp:211-214`）；执行失败保持 pending、原因进审计（:438-447 + `panel_state.cpp:334-340` UI 可见）；e2e 修的 5 缺陷（会话缝/UUID 回填/融合段解析/delete 映射）在 `gateway/telemem_client.cpp:93-209,329-418` 的形状与 e2e 报告 §7 一致，且 2 条钉版单测在位（`test_memory_hook.cpp:208-232`、`test_memory_proposals.cpp:298-412`）。遗留形状错位见 F3；baseVersion live 空转见 F4；撤销死卡见 F6。

### 2. 回归面 —— 已核，1 项 finding（F7）

- main.cpp 双入口：`runMemoryHookSubcommand`（:642-665）仅 `args[1]=="memory" && args[2]=="hook"` 命中，其余参数组合返回 -1 走原启动序列；分派先于 `acquireInstanceLock()`（:677）——锁语义未被绕过（hook 是 Claude Code 拉起的短命子进程，设计如此并有注释），普通 GUI 二开仍走锁+唤起。已核：无回归。
- settings 开关对称性：memory 与 wallpaper/agent 同构四件套——`settings_config.cpp:44-48`（kKnownModuleIds）、:56-60（defaultEnabledModules，memory 默认开）、`settings_ui_cards.cpp:414-416`（开关卡）、`app_registry.h:41`（注册行）。memory 默认开且无自启（引擎只在按钮/向导触发 startAsync，`memory_module.cpp:378-380,785,2173`），"零后台成本"文案属实。已核：对称。
- CMake/tests：`src/module_memory/CMakeLists.txt` 零兄弟模块依赖（core/platform/ui_kit 三库）；tests/CMakeLists :74-82 仅追加 memory 测试文件，:230/311/413 为探针目标链接——不波及他模块。已核。

### 3. 平台对称性 —— 已核，1 项 finding（F2）

- _win/_posix 成对完整性：python_locator / telemem_process / llama_host / process_host 四组 _win/_posix 全部真实现（非空壳），CMake 按平台选择编译（`src/module_memory/CMakeLists.txt:55-68`）；`python_locator_posix.cpp:1-3` 显式 TODO(mac 实测) 标注，无 Windows 代码误用 posix 空实现路径。已核。
- 路径分隔符：`telemem_config.cpp:18-21` YAML 路径统一 forward-slash；hook 命令串 `"exe" memory hook` 经 JSON stringify 转义反斜杠（`hooks_settings.cpp:46-48`，单测用 `C:/Program Files/...` 形态验证）。已核：契约钉版警告已消化。
- **F2（唯一平台缺陷）**：posix 侧 spawnDetached 在父进程 setenv 注入 key——见 findings。

### 4. 静默失败面 —— 已核，2 项 finding（F5/F7）

- 有可见出口的路径（已逐一核）：Python 不合格/venv 失败/pip 失败/拉起失败/健康超时 → `telemem_engine.cpp:174-301` setState(Failed, 可读原因)，面板显 detail + 一键重启；引擎运行中被外杀 → `snapshot():126-138` 转 Failed 提示；面板库打开失败 → `panel_state.cpp:118-137` storesError → `memory_module.cpp:791-799` 红字可见；模型下载失败 → llamaMissing 引导态（`telemem_engine.cpp:230-266`）。无 except-吞错式兜底（全仓 Result 贯穿）。
- 不可达：hook 通道 fail-open 空 stdout exit 0（e2e 真跑 hook-down-out.json 0 字节实证）；评测不可达 → `recall_eval.cpp:154-157` 全败才报错、单查询失败按未命中不虚增。但评测失败的 UI 出口缺失（F5）、注入账并发丢失（F7）。

### 5. 测试有效性（变异视角）—— 已核，通过（1 项遗留见 F3）

- **变异 1（正则）**：将 `sanitize_gate.cpp:620` 手机号二位边界 `t[j+1].cp > '9'` 改 `>=`（拒 13X/19X 尾段）→ 红队夹具 positives 的 `expect_categories` 类别未命中 → `memory_sanitize_redteam_zero_leak_zero_misfire` 红；同变异波及 `preview_full_mask`（phone+email 双命中断言）与 `detector_spot_checks`（全角手机号）——三处红，夹具 110 条实存（`tools/memory-lab/sanitize/datasets/redteam.json` 计数=110）、样本下限 + positives/negatives>0 防夹具静默跳过。非恒真。
- **变异 2（预算常量）**：kMaxChars 500→5000 → `test_memory_hook.cpp:96-99`（600 字符首条必空）红（上界钉住）；500→50 → :86-87（第 6 条起裁剪）红（下界钉住）。双向可红。
- 探针断言数据源：`memory_e2e_probe.cpp:90-100` 会话根由 `--projects` 外部注入（e2e 报告 §1 伪造 3 会话，含假手机号/假密钥），"3 段/2 pending/1 拦截"（:115,164,173）全部基于独立造数，非自证恒真。"3/5 条"类读数（S5 :253-256）由真 hook 函数 + 真 TelememHttpRetriever 产出。
- 修复类"先红后绿"：5 缺陷各有钉版回归用例（live 信封解析 :208-232、UUID 回填+遗忘撤销重录入链 `test_memory_proposals.cpp:298-412`），旧实现行为（信封原文当正文、整型 id 假设）即红面，成立。
- 遗留：遗忘线钉版用例用 FakeEngine 原样回显整型 "7"，未钉 live UUID 形态（并入 F3）。

### 6. 文档口径 —— 已核，2 项 finding（F1/F9，另 F4 的文档面）

- 需求映射抽验 3 条验证物路径：`plans/2026-10-28-memcpp-s2-live-verify.md` 实存、`plans/2026-10-27-memcpp-embed-bakeoff.md` 实存、`research/2026-10-27-memcpp-hook-contract-facts.md` 实存；另核 redteam.json（110 条=文中"红队 110 条"）、`.verify-screenshots/memory-{1,2,3}-*.png` 实存、mockup 实存。无死链虚记。
- 已知限制清单与代码现状比对：限制 1（引擎无列举/全清缝）与 `telemem_client.h:21-38` 三方法一致；限制 2（token 估算口径）与 `usage_ledger.h` 契约注释一致；限制 3（窄窗标签）UI 瑕疵口head一致；限制 4（macOS/Linux 未实测）与 posix TODO 标注一致；限制 6（评测未出终数）一致。**限制 5 不完整**：实际是"产品侧零调用点"而非仅"触发形态待定"（F1）；且 F4（baseVersion live 空转）未入清单。
- overview 现状行滞后于同日 e2e 结论（F9）。

## 二、Findings 汇总（9 项：High 2 / Medium 3 / Low 4）

| # | 级别 | 位置 | 问题 | 影响 | 建议 |
|---|---|---|---|---|---|
| F1 | High | `collector/collector.cpp:262`、`proposals/curator.cpp:11`、`sanitize/sanitize_gate.cpp:802` | 采集/整理器/脱敏闸在 src/ 全树零产品调用点，唯一调用方是 `tests/probes/memory_e2e_probe.cpp` | 三条主线以库+探针形态存在；面板脱敏开关与"原样出机"警示守着不存在的出机路径；接线人需从零拼管线且易漏 fail-closed 传播约定 | overview 限制清单写实"产品侧无采集/出机调用点"；接线时收敛单一管线入口（gate.process→proposeAdd），fail-closed 中断外发约定随之钉死 |
| F2 | High | `platform/process_host_posix.cpp:52-58` | spawnDetached 在 fork 前对**父进程** setenv（含 ANTHROPIC_API_KEY、TELEMEM_CONFIG），从不清除；Windows 侧 `process_host_win.cpp:151-186` 是 per-child 环境块 | POSIX 交付时 key 常驻 DevDesk 主进程环境，被后续所有子进程（终端/git/sidecar）继承，撞 telemem_config.h"密钥唯一落点=子进程环境"红线 | 改为 fork 后子进程内 setenv（execvp 前）或 execve 传 envp；mac 交付前必修（若本轮含 mac 则升 blocker） |
| F3 | Medium | `proposals/curator.cpp:46-54` + `curator.h:27`；测试固化 `test_memory_proposals.cpp:318-337` | proposeForget 以 `std::to_string(int64)` 构造 targetIds，与 live 定案 UUID 串形态错位；"live 钉版"用例用 FakeEngine 回显整型 "7"，把错误形态钉进了测试 | 遗忘扫描接真引擎（补 MemoryListFn 缝）时 forget 卡执行必挂，必然返工点；测试名暗示 live 已钉、实为内回声 | MemorySummary.id 改 string（或加 idText）；钉版测试断言执行器收到的 id 必须来自引擎回填形态 |
| F4 | Medium | `panel/panel_state.cpp:32-36,327-329,377-379` | accept/createUndo 均以 trustSnapshotVersion 放行 → `proposal_store.cpp:419-436` baseVersion 防覆盖校验 live 恒过（空转）；overview §1.5 记为已落地保障未注明 | 防覆盖是名义防线；读者按 overview 会误信覆盖保护在生效；引擎补版本缝时此处是隐藏改点 | overview 限制清单补"baseVersion 校验 live 空转"；引擎版本缝就绪后删除 trustSnapshotVersion 并补对照测试 |
| F5 | Medium | `panel/panel_state.cpp:410-424`；`panel/recall_eval.cpp:159-166` | 评测失败仅 LOG_WARN，snapshot 无错误字段，UI 零反馈；评测 doc 清理仅覆盖 id 回填成功者（get_memories 200 条上限/正文失配即漏），残留 doc 滞留真引擎检索池、可被 hook 注入 | 用户点评测无果不知因；评测数据污染注入通道且无自愈出口 | snapshot 加 lastEvalError 面板显示；清理失败升级为可见提示；清理对齐 addDirect 的回填口径（含超 200 条场景） |
| F6 | Low | `proposal_store.cpp:450-468` + `curator.cpp:71-76`；`gateway/telemem_client.cpp:329-364` | add 类 id 回填失败（正文不匹配/超 200 条）时 targetIds 恒空，其撤销卡接受必 ValidationError，"可再点接受重试"永不成立 | 用户面对只能拒绝的死循环待审卡，错误文案误导（实为不可撤销） | undoOf 置位且 targetIds 空的卡在 accept 前给出"不可撤销（引擎条目 id 缺失）"明确终态/提示 |
| F7 | Low | `main.cpp:651-659`；core/sqlite_db 无 busy_timeout | hook 子进程与 GUI 并发写 usage_ledger.db，BUSY 即静默丢行（`(void)` 丢弃） | 注入账偶发少记 → 预算环读数偏低；约定"尽力而为"已注释但零观测 | SqliteDb::open 设 busy_timeout；recordInjection 失败至少 stderr/日志一行 |
| F8 | Low | `collector/collector.cpp:220-224` | CRLF 行 pop '\r' 后按 size()+1 推水位，每 CRLF 行少计 1 字节，下轮从残留字节续读、逐轮收敛（坏行 WARN/空轮询噪声） | Claude Code jsonl 实为 LF，现实触发率低；一旦出现 CRLF 源，水位抖动难排查 | 水位按流实际偏移（tellg）或保留 '\r' 计入 consumedFrom |
| F9 | Low | `docs/superpowers/specs/2026-10-28-memcpp-overview.md:5,150-151` | 现状行/§6 S3/S4 停在"验收中、结论未回"，同日 e2e-verify 已"可交付"并修 5 缺陷；§1.4 验证物可补 hook 真跑证据 | 入口文档滞后，新读者按旧状态判断里程碑 | 现状行更新至 e2e 结论 + 本审计；S3/S4 状态改"已验收（e2e-verify）+ 未接线项见 F1" |

## 三、总裁决

**有 finding，无 blocker——放行并入 dev。** 条件：F1/F4/F9 的文档口径修正随并 dev 一并落（改 overview 限制清单与现状行，零代码风险）；F2 在 macOS 交付排期内必修（当前 Windows 交付平台口径下可带 F2 放行，但必须随 F1/F4 一起登记进已知限制清单）；F3 在"补 MemoryListFn 缝/接遗忘扫描"任务开工前必修；F5-F8 进 backlog 按序消化。

—— 审计完。六维度全答，findings 9（High 2 / Medium 3 / Low 4），无 blocker。

## 四、处置回填（实现线 2026-10-28，分支 feat/memcpp）

| # | 处置 | 依据 commit |
|---|---|---|
| F1 | **已修**：采集/整理管线接入产品生命周期——新增 pipeline/collect_round 单一管线入口（collector→SanitizeGate.process→curator.proposeAdd，段全部处理完才 commit）；PanelState 背景循环随引擎 Running 起、引擎停自停（kPollIntervalMs=30s 轮询，stop 分片 250ms 保 UI join 上界）；拦截段（含 fail-closed Error）只出审计行（core::log 通道）+ 动态流 Blocked 可见行（零正文零原文），不入提案；decay 6h 周期触发，生产清单缝因引擎无列举能力如实报 NotImplemented（fail-closed 零假卡，缝补齐后只换闭包）；集成单测 test_memory_collect_wiring.cpp 两例（临时会话目录直驱：干净段出 pending 卡、敏感段拦截审计行+note、二轮幂等；UUID 清单缝真形态 + 缺席 fail-closed） | `092f0aee` |
| F2 | **已修**：posix spawnDetached 改 fork 后子进程内 setenv（execvp 前），父进程环境零注入；注释钉死 key 红线（严禁挪回 fork 前）。Windows 侧本就 per-child 环境块，语义对齐；posix 文件真机验证仍随限制#4 待 mac 实测 | `d7b4de41` |
| F3 | **已修**：MemorySummary.id 改 std::string（UUID 串）；proposeForget targetIds/去重比对原样使用；live 钉版测试改 UUID 形状断言（FakeEngine 回显 kLiveUuid，targetVersion 按 UUID 校验，卡面 targetIds 断言引擎串形态）；memory_probe 演示数据同步改 UUID 串（整型字面量在 vector<string> 上变 size_type 构造，编译期即拒） | `f14ce96e` |
| F4 | **文档口径已落**：overview 限制清单新增 #7「baseVersion 防覆盖校验 live 空转」（明示 trustSnapshotVersion 旁路、勿误信已生效、引擎版本缝就绪后删除并补对照测试）；代码不动（契约位置已注释） | `1d7e567c` |
| F5 | **已修**：评测失败原因进 PanelSnapshot.lastEvalError、评测卡 note 显示（旧仅 LOG_WARN）；清理缺口（id 未回填 + 删除失败）计数上浮 RecallResult.cleanupFailures 并面板可见；清理口径边界（get_memories ≤200 条）如实入 overview 限制#8。引擎侧无法定向清理的根治依赖列举缝，残留以「重跑评测覆盖清理」为出口 | `092f0aee` / `1d7e567c` |
| F6 | **已修**：createUndo 对「invalidate 且原卡 targetIds 空」建卡前即拒（不给死循环卡）；accept 对「undoOf 置位且 targetIds 空」防御性复检快速失败，文案明示「不可撤销：引擎条目 id 缺失，此卡只能拒绝」（留 apply_rejected 审计行） | `33513448` |
| F7 | **已修**：SqliteDb::open 统一 sqlite3_busy_timeout(5000)（共享库并发写 BUSY 先等再败）；hook 注入记账失败改 stderr 一行留痕（stdout 是注入通道禁污染，此刻日志通道未必已建立） | `33513448` |
| F8 | **已修**：采集水位按流实际消费偏移推进（'\r' 剥离只影响解析文本，rawLen 计入 consumedFrom），CRLF 源不再逐轮少计 1 字节 | `33513448` |
| F9 | **已修**：overview 现状行更新至 e2e 可交付 + 本审计处置结论；S3/S4 状态改「已达成（e2e-verify + 审计处置）」并列验证物；限制5 重写为「已接线 + decay 边界」 | `1d7e567c` |

- 批外增量：e2e 探针补显式标准头（`2c0e101c`，std include 守卫红面为 9ee92079 既有，非本批引入）。
- 定向门禁：`bash scripts/build-test-run.sh --filter memory` 全绿（构建成功；43/43 用例过，含 2 条新增接线用例；模块隔离/交叉编译 280 文件/std include/探针登记/玻璃体系五守卫过）。
