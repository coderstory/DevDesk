# 审计⑥ Memory 熵治理 — code-reviewer 面（约束符合 / 跨文件一致性 / 结构冲突）

- 日期：2026-11-02 ｜ 对象：`/Users/coderstory/.zcode/cli/memories/projects/devdesk-7acb05266951a89c/memory/` 全部 19 个 .md + MEMORY.md 索引
- 只读声明：本轮零写入 memory 目录，未删未改任何 memory 文件；本报告为唯一落盘物。
- 尺子：`/Users/coderstory/dev/DevDesk/docs/research/2026-11-01-skill-quality-baseline.md`（引用编号 F.1-F.5 / C.9 / C.7 / D.3）；已验证事实沿用不重查（devflow 0.33.0 固化清单、池 73 拒绝终态、索引 19 行）。

---

## 轴① 逐条分类（19 文件，每文件判一类）

| 文件 | 类别 | 理由 | 若 B：插件/真相源指针 |
|---|---|---|---|
| demand-full-pipeline-discipline | B | 正文自认「均已固化 devflow ask 0.24–0.27」；0.33.0 ask §0/§1/§3 全覆盖（多需求点拆分/存量产物合规核/完备方案免重复审计/审计选面/小任务批次/编排权/跳过权，已验证） | `/Users/coderstory/dev/zcode-plugins/devflow/skills/ask/SKILL.md` §0/§1/§3 |
| devflow-plugin-firmware | B（原则留一行） | 0.18–0.31 版本史流水账已全部固化进 0.33.0（已验证清单逐条对上）；git log 是版本史唯一真相源；其自身 0.29.0 立规「出处归 commit message」——本文件即违例现场；安装段与 zcode-plugin-management 全文重复。原则「流程问题改插件不写 memory」保留为 MEMORY.md 一行 | devflow 仓 git log + 同目录 zcode-plugin-management（承接安装坑） |
| main-session-dispatch-only | B | 「批准≠跳流程授权」已固化为 0.31.0 派发前置确认 + ask 分诊 + agent-gate（已验证：流程同意原则） | devflow `skills/ask/SKILL.md` + `hooks/session-start.sh` + `gates/agent-gate.sh` |
| test-purpose-discipline | B | 维度分工/全量阶段档/角色边界已固化（0.21/0.22/0.25 → 0.33.0 agents 定义，已验证）；「三问/会红性」两句并入指针行防丢失 | devflow `agents/developer.md`、`agents/qa-tester.md` + DevDesk AGENTS.md §五 |
| zcode-plugin-platform-facts | B（指针化） | 池 81 v2 基线报告的摘要复述=第二真相源，基线更新必腐烂（F.4/C.9）；文件自身已写「判据全表见 baseline.md」，指针化顺理成章 | `/Users/coderstory/dev/DevDesk/docs/research/2026-11-01-skill-quality-baseline.md` |
| devdesk-pool-tool-pitfalls | D | 「`拒绝→在池` 可逆」与 pool_mcp.py 状态机矛盾（`"拒绝": set()` 终态无出边，池 73 日志从未有拒绝跳——已验证）；其余条目为工具行为抄写（C.9 cache，已开始腐烂），应回流 pool_mcp.py 工具描述后整文件删除 | 回流目标：devflow `tools/pool_mcp.py` 工具 description |
| devflow-audit-plan | C | 纯审计任务进度快照（池 75-81 台账即真相源）；「81 待验收」已过时——现状 81 已验收、下游 75-80 已派发（本报告即审计⑥产物） | 池 75-81 台账 |
| ui-batch-iv-plan | C | 纯项目编排进度 + git 未推状态快照；方案文档（docs/design/2026-11-01-UI整改批次IV-*.md）与池 64-73 是真相源；「推进口径」段为 test-purpose/demand 内容复述（C.7 散落）；含「73 曾被误拒已恢复」错误事实须纠错后迁 | 方案卡 §9 + 池 64-73 |
| bridge-fork-abandoned | A（浓缩） | 跨会话方针哨兵（bridge 类默认不做、复活须独立路径），插件无此内容；池条目是逐条状态，此条是汇总方针，有召回价值 | — |
| current-env-is-zcode | A（浓缩） | 老板语境事实（ZCode/DSH 双生态默认指 ZCode），查不到、可复用 | — |
| dsh-runtime-paths | A | DSH 侧运行时路径参考，发现成本高、可复用（双生态未消失） | — |
| github-push-via-system-proxy | A | 本机网络环境事实（git 不读系统代理+双通道），可复用 | — |
| subagent-dispatch-channel | A（拆分删杂） | 拆三堆：条 5/7 已固化（随 B 删）；条 6+条 8 上半=已恢复的配额故障叙事（过时状态，删）；条 1/3/4/9=可复用平台实测事实（留，与 token 纪律合并浓缩） | 条 5→`gates/agent-gate.sh`；条 7→`skills/ask`（0.31.0） |
| subagent-token-discipline | A（合并） | 派发预算纪律未进插件（0.33.0 已验证清单不含）、可复用；与 channel 条 3/9 重叠（续跑代重派、打回式简报），合并为一 | — |
| open-research-no-preset-frame | A（待固化转 B） | 本会话新增；0.33.0 已验证固化清单不含此规则，正文自认「后续应提炼进 ask/researcher」——先留，固化后转一行指针 | 固化后→ask/researcher 派发口径 |
| zcode-cli-headless-usage | A | CLI 调用方式（15MB bundle 内部行为），查不到、可复用 | — |
| zcode-mcp-registration | A | MCP 注册点平台实测事实，可复用；「9 工具」数字过时须修（P2） | — |
| zcode-permission-wall-headless | A | 无审批客户端会话的放行面事实，可复用 | — |
| zcode-plugin-management | A | CLI 插件更新流程+坑，可复用；firmware 去重后升为安装坑唯一真相源 | — |

**统计：A=11（其中 3 个需浓缩/拆分/合并）｜B=5｜C=2｜D=1。**

---

## 轴② 索引一致性

- 死链：未发现——MEMORY.md 19 行链接与 19 个文件一一对应，全部可解析。
- 漏索引：未发现——无文件游离于索引外（open-research-no-preset-frame 本会话新增且已入第 19 行）。
- 索引行数：19 行，与已验证口径一致。
- 发现的问题（hook 文字 vs description/正文 vs 现状）：
- [P0] MEMORY.md:13 — 索引行携带错误事实「拒绝→在池可逆」 — 证据：行 13「…关闭须过待验收且带 evidence；拒绝→在池可逆；…」与 pool_mcp.py 终态语义矛盾（已验证），错误随索引每次召回扩散 — 建议：改写为「拒绝=终态不可逆，误操作走 soft_delete 前 先老板确认」或整行随文件 D 化删除。
- [P1] subagent-dispatch-channel.md:3 — description 停留旧状态、与正文矛盾 — 证据：description「配额耗尽三连败（2026-11-01 深夜…）」（通道暂停语义）vs 正文条 8「当晚已确认恢复：池 81 researcher 派发成功」；MEMORY.md:6 索引行却是「已恢复」——三处两个版本（F.5 新鲜度不同步） — 建议：合并浓缩时以「已恢复；恢复以探针通过为准，失效条件=探针失败；model 字段勿动」一句收口，删故障过程。
- [P1] devflow-plugin-firmware.md + MEMORY.md:14 — 版本状态过时 — 证据：正文「cache 已装 0.31.0」「0.20.0–0.31.0 未推」，索引「已至 0.31.0…0.20-0.31 未推」；实际插件已 0.33.0（已验证口径） — 建议：随文件 B 化删除版本状态；今后 git 状态一律 `git log origin/main..HEAD` 现查（C.9）。
- [P1] devflow-audit-plan.md + MEMORY.md:15 — 「待验收」状态过时 — 证据：正文/索引「81 基线 v2 已交付**待验收**…老板验收 81 → 派 75/76」；现状下游六审计已派发（本报告即证） — 建议：整文件迁池台账，索引只留一行「审计任务见池 75-81」。
- [P2] MEMORY.md:6,14,15 — 索引行内嵌状态快照（易腐形态） — 证据：以上三行均把「当时状态值」写进索引，均已过时或含错误 — 建议：索引行只写「何时查什么」（主题+触发词+日期），状态值永不入索引。
- [P2] demand-full-pipeline-discipline.md:21 — 死 wiki 链接 — 证据：「相关：[[devdesk-pool-usage-skill]]」全库无此文件且未入索引 — 建议：随文件 B 化删除；若留指针体系统一走 MEMORY.md 行链接。

---

## 轴③ 膨胀度量

- 体量：文件 19（不含索引）/20（含）；总字符 26,720（含索引），正文净 24,203；平均 ≈1,274 字符/文件。最大：devflow-plugin-firmware 3,121；次大 subagent-dispatch-channel 2,633——两大文件恰是流水账/状态混杂重灾区。
- frontmatter type 分布：project×6、feedback×6、reference×7（合计 19 核对无误）。误标三处：
- [P2] zcode-permission-wall-headless.md:7 — type 误标 — 证据：`type: project`，内容实为无审批会话放行面的平台参考 — 建议：改 reference。
- [P2] devdesk-pool-tool-pitfalls.md:7 + subagent-dispatch-channel.md:7 — 单文件混装多型 — 证据：前者 reference 含「老板 2026-11-01 令」（feedback 语义）与错误事实；后者 project 内混平台事实+纪律+状态快照三型 — 建议：按「平台事实=reference／纪律=feedback／项目状态=不入 memory」重立，一文件一型。
- 语义重叠对（逐对给重叠段落与合并方案）：
1. **demand-full-pipeline-discipline ↔ devflow-plugin-firmware ↔ devflow-audit-plan**：demand「三次扩展」四条 bullet（:16-19）与 firmware 0.20.0/0.24.0/0.26.0/0.27.0 行几乎同文；「完备方案免重复审计（教训：31 万 token…）」三处重复（demand:19、firmware 0.20.0 行、audit-plan How to apply）；「审计选面」两处重复。合并方案：三者全 B/C 化（指针见轴①），重叠即消失。
2. **ui-batch-iv-plan ↔ test-purpose-discipline**：ui-batch「推进口径」段（:18）复述「一条一条/完备方案免重复审计/全量只在阶段节点」并挂指针——复述+指针双写。合并方案：ui-batch 整体 C 迁后此段直接删。
3. **subagent-dispatch-channel ↔ subagent-token-discipline ↔ main-session-dispatch-only**：channel 条 3「SendMessage 续跑而非重派」↔ token ④ 同义；channel 条 9「极简打回式」↔ token ⑦「打回=甩产物路径+缺陷清单」同义；channel 条 7（派发前置确认 0.31.0）↔ main-session-dispatch-only（批准≠跳流程）↔ firmware 0.31.0 行——**同一条规则三处记录**。合并方案：channel+token 合并为「派发平台事实」+「派发预算纪律」两条（见轴④）；main-session 与 firmware 0.31.0 行删除留指针。
4. **devflow-plugin-firmware ↔ zcode-plugin-management**：安装两步走、@marketplace、「新会话生效」、daemon.sock 坑在 firmware「插件安装已走通 CLI」段全文重复。合并方案：以 zcode-plugin-management 为唯一真相源，firmware 段删。
5. **devflow-audit-plan ↔ zcode-plugin-platform-facts**：后者是基线报告摘要（第二真相源），前者引同一路径。合并方案：都指针化指向池 75-81 与基线文档。

---

## 轴④ 治理方案（可直接执行）

**删减清单（主会话执行；删前先做回流动作）**：
1. devdesk-pool-tool-pitfalls — 先把「关闭链两跳+evidence 前置/bug_resolved 传行 id/快照随 commit/署名 ZCode主会话」回流 pool_mcp.py 工具描述与池 usage 文档，然后整文件删（P0 矛盾条不迁移、就地销毁）。
2. demand-full-pipeline-discipline / main-session-dispatch-only / test-purpose-discipline / zcode-plugin-platform-facts / devflow-plugin-firmware — 删，MEMORY.md 各留一行指针（见轴① B 列）；firmware 额外保留原则行「流程问题改 devflow 插件固化，不写 memory/AGENTS.md（版本史见插件仓 git log）」；test-purpose 指针行内保留「三问（防什么回归/什么场景/哪条边界）+会红性」两句。
3. devflow-audit-plan / ui-batch-iv-plan — 迁池台账/方案卡后删；ui-batch 迁移时必须纠错「73 曾被误拒已恢复」表述（ui-batch-iv-plan.md:21，P0 同源）。

**浓缩合并方案**：
1. subagent-dispatch-channel + subagent-token-discipline → 两条新档：
   - `subagent-dispatch-facts`（reference）— description：子代理派发平台实测——并发≤2 稳定/第 3 个必拦；续跑（SendMessage）优先于重派；随会话打断被杀的后台代理不可续跑只能重派；检查点 commit 会改在跑代理 diff 基线须补发通知；agent model 字段勿动、通道恢复以探针通过为准（失效条件=探针失败）。
   - `subagent-dispatch-budget`（feedback）— description：主会话喂事实+禁读清单；一切任务（含实测/实现类）给轮次硬上限与中途检查点；审计只审新增改动面；产物「只写增量不复述上游」；打回=产物路径+缺陷清单不复写任务书。
2. bridge-fork-abandoned、current-env-is-zcode 各压至 2-3 句（保留 Why/How to apply）。
3. open-research-no-preset-frame 保留原样，尽快固化进 ask/researcher 派发口径（开放性 vs 固定路线两类简报模板）后转一行指针。

**今后写入规范建议**：
1. 写入前先问「能否抽通用进插件」；插件已有 → memory 只留一行指针（firmware 原则）。
2. 固化工序补一步「固化完成后删源 memory 条目」——本次 5 个 B 类全是固化完成后未回头清理的滞后物，是扩增失控的直接机制。
3. 状态/进度不入 memory：池台账是唯一状态真相源，git log 是版本史唯一真相源（C.9/F.4）。
4. 平台/环境事实必须带实测日期+失效条件（F.5），如「通道恢复以探针通过为准」。
5. 引用工具行为不抄写：状态机/参数语义以 pool_mcp.py 工具描述为事实源，坑清单回流工具描述（A6.7）。
6. 每条必须带 Why/How to apply 且可复用；禁日期流水账与一次性状态（现有 19 条中 17 条已合规，保持）。

---

## 约束轴核对

P0 六约束轴（架构分层方向/平台宏进业务代码/错误处理约定/只改一头平台/写只读目录/UI 无证据物）：不适用于纯 memory 文档面；本面零代码改动、memory 目录零写入（只读铁律遵守），唯一落盘为本报告（任务书指定路径）。工程标准轴：四轴任务书即核销清单，①②③④全核销完毕，无 F-ID 遗留。

## 汇总

| 级别 | 计数 |
|---|---|
| P0 | 1（错误事实「拒绝→在池可逆」及其两处扩散） |
| P1 | 4（进度入 memory×2、description 状态不同步×1、版本史流水账自违例×1） |
| P2 | 5（type 误标/混装×3、过时数字×1、死 wiki 链接×1；索引快照形态并入） |

治理后预计：文件 19 → 10（A 类 11 中 channel+token 合并减 1，其余 8 B/C/D 全部退化为 MEMORY.md 指针行或迁出）；总字符（含索引）26,720 → 约 10,000（压缩 ≈63%）。

**总判定：可合入**——清单可直接交付主会话执行；P0 一条三处（pool-pitfalls:17、MEMORY.md:13、ui-batch-iv-plan:21）建议最先纠错，防「误拒可逆」诱导不可恢复的破坏性操作。

（自报：实际工具调用 5 次，预算 30，未超。）
