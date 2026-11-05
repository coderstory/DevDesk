# 线索台账生命周期模式调研（need#253）

- 作者：researcher · 2026-11-05 · 档位：全报告 · 实际工具调用 ≈72 次 / 预算 80（预算内）
- **不换方案声明（老板 2026-11-05 澄清）**：本文只校准现有「池库 + clues 线索表 + MCP 工具 + 共享事实台账」的**生命周期模式**（失效/合并/更新/当前视图），**不选型、不替换**。Mem0、Graphiti、XTDB、Splink 等一律**仅作模式参考**；不引入外部依赖、向量库、外部服务、后台守护进程。约束不变：Python stdlib + SQLite、MCP 单写者、工具面 16、token 增幅 ≤5%、离线可用、审计行不物理删除。
- 依据链：PRD doc#15（slug=2026-11-05-devflow-clue-lifecycle）+ 技术方案 doc#16（slug=…-tech）+ 审计 doc#1（简报已证事实直接采信，未重查）+ 本轮外部检索（来源见 §九）。
- 中段即时入库：clue#124–#131（domain=devflow, need_id=253, by=researcher）——下游凭台账复用，不重查。

---

## 一、结论先行（≤5 条）

1. **现方案五列模型在文献上最小完备**：current 读面派生 + `valid_until`（valid time 上界）+ `created`/`resolved`（transaction time）+ `superseded_by`（前向失效指针）恰好是 SQL:2011 双时态与 Zep/Graphiti bi-temporal 的最小子集。**无需 `valid_from`、无需区间列、无需第三存储态**。（已确认，两源交叉）
2. **单链 `superseded_by` 足够，DAG/merge 节点不必引入**：Graphiti 的事实失效同样是「新信息把旧边置 `t_invalid`」的单向前向指针；多对一合并 = 多旧行同指一新区，与 `supersedes=[多id]` 同构。（已确认，HTML 全文）
3. **「state 到期只退出读面、不自动改状态」有同构先例**：XTDB 的 delete 只抹可见性、eviction 才物理删——过期可逆 + 审计行不删（I5）与主流时态库语义一致。90 天复核阈值**无文献公认值**（如实），应作「初值 + doctor 告警率实测回调」。（机制已确认；阈值=无公认值）
4. **最需要警惕的外部反例：纯 append-only 无失效传播有害**——TEPA 实证（50 seeds）：append-only 与 last-write-wins 在知识完全反转时 0.210，低于无记忆基线 0.309。我们的 supersedes + subject 唯一索引恰好补上失效传播，方案方向被外部实证支持；但「新信息优先」有错杀风险（MAB 冲突切片 1/3 gold 对 last-write 规则不可达），误取代恢复链会增长——属审计特性非缺陷。（已确认）
5. **不建议项集中且都有更小替代**：`valid_from` 列、recorded_at 列、自动实体解析、结构化 reason_code 列、DAG、`as_of` 参数、TTL 自动写状态、复核守护进程——全部进校准表「不建议」栏并给理由。（分析）

---

## 二、术语对齐

| 文献术语 | 定义 | 我们方案中的对应 |
|---|---|---|
| valid time（有效时间，Snodgrass 提出、SQL:2011 采纳） | 事实在现实世界成立的时间 | `valid_until`（上界单点简化；下界=created，「写入即生效」） |
| transaction time（事务时间） | 数据进库时点，不可回写，审计用 | `created` / `resolved`(+`resolved_by`) |
| verification time（复核时间，非标准术语） | 事实最后一次被确认仍真的时点 | `last_verified` |
| TTL（time-to-live） | 声明式到期时点，到期降级不删除 | `valid_until`（仅 kind=state） |
| invalidation（显式失效） | 标记事实停止为真（真伪变化，人/agent 判） | `clue_resolve` / supersedes 置 stale + reason 前缀 |
| supersession（取代） | 新事实取代旧事实，前向指针可追 | `supersedes` / `superseded_by` 单链 |
| merge（合并） | 多旧事实归并为一条新事实 | `supersedes=[id1,id2,…]`（多旧行同指一新区） |
| current view（当前视图） | 「现在为真且是该主题最新版本」的读面 | current 派生式：open ∧ 未过期 ∧ 未被取代 |
| entity resolution（实体解析） | 判定两条记录指同一实体 | subject=`<归一域名>:<topic>` 规范化键（手工 canonical key 路线） |
| expansion / contraction / revision（AGM） | 信念集三操作：加不删 / 删不加 / 删¬p 再加 p | clue_add / clue_resolve(refuted:) / clue_add(supersedes=[旧]) |
| justification（TMS，Doyle） | 信念的结构化依赖记载 | supersedes 链 + resolved/resolved_by/reason（人类审计文本版） |

---

## 三、模式目录（= 八个调研问题逐条答案）

> 每问结构：**答案** → 文献/实践依据（标已确认/待验证/推测） → 直接可用的落点。

### Q1 当前视图：`current` 定义是否足够？三分法最小落哪几个字段？

**答案：足够。三分法最小落点 = 现方案五列，一个不加。** valid time → `valid_until`；transaction time → `created`+`resolved`（已有）；verification time → `last_verified`。`valid_from` 不必加：我们的事实「写入即生效」，不存在回溯声明 valid 区间的业务（SQL:2011 application time 需要下界是因为业务数据可追溯声明生效日；线索表没有）。`recorded_at` 不必加：`created` 已承担。
- SQL:2011：transaction time 由 `SYSTEM VERSIONING` 系统版本表自动维护，valid time 由 `APPLICATION TIME PERIOD` 表承载；约束只作用于 current rows（Kulkarni & Köhler 2012, DOI 10.1145/2380776.2380786；PostgreSQL Wiki SQL2011Temporal）。**已确认**（搜索+官方页，两源）。
- XTDB（官方文档）：tx-time 不可回写（"You cannot write a new transaction with a transaction-time that is in the past"），(valid-time, tx-time) 对做 point-in-time 查询，未指定时 valid=tx 同值。**已确认**（全文）。
- Datomic 的 datom/history 模型：**待验证**（本轮未核验，与 XTDB 同族，启示以 XTDB 为准即可）。
- 最小启示（不引库）：系统版本表的本质=「历史行自动保留 + 约束只作用 current」，我们用 I5（行不删）+ current 派生式手工等价实现，规模（104 行级）下零性能顾虑。

### Q2 失效：TTL / 显式 invalidation / 定期复核各适用什么？90 天有无依据？到期只退读面是否合理？

**答案：三种机制对应三种时间性，分工正确。**
- `valid_until`（TTL）→ **kind=state**：版本/时刻限定的环境快照（「0.38.15 实测」「同树 WIP 三红」）——这类事实的失效是**机械可判**的，系统只做时效判定不判真伪，与 PRD「不自动判定事实真伪」一致。
- 显式 invalidation → **kind=fact 的真伪变化**（被推翻/被修正）：只有人/agent 能判，走 `clue_resolve(refuted:)` 或 supersedes。
- `last_verified` + 90 天复核 → **fact 的第三种时间性**：未被推翻但「可能已过时」——LongMemEval 把 temporal reasoning 单列为核心能力，恰说明「没有验证时间戳的事实会被高估信任」。
- **90 天阈值：无文献/工业公认值**（如实：本轮未检索到权威依据；工业 TTL 值域极散，无可引用锚点）。建议作「初值 + doctor 告警率实测回调」处理（§六 拍板③）。
- 「state 到期只退出读面、不自动改状态」**合理**：XTDB 同构先例——delete 只从当前视角抹可见性，eviction 才物理删；过期行保留审计面且可逆（改 valid_until 即恢复）。**已确认**。

### Q3 更新/合并：单列 superseded_by 单链是否足够？要不要 invalid_at 区间/DAG/merge 节点？

**答案：单链足够，全部不必引入。**
- Zep/Graphiti（HTML 全文）：每条 edge 四时间戳——`t_valid`/`t_invalid`（事实轴）+ `t'_created`/`t'_expired`（事务轴）；失效由「LLM 对比新边与语义相关旧边检测矛盾」触发，"invalidates the affected edges by setting their t_invalid to the t_valid of the invalidating edge"，且 "consistently prioritizes new information"。**没有 DAG、没有 merge 节点**；多对一合并=多旧行同 `t_invalid` 指向同一取代者。我们的 `superseded_by`（前向单链）+ status 是 `t_invalid` 的行级离散化实现。**已确认**（一手全文）。
- Mem0（HTML 全文）：update phase 由 LLM 经 tool-call 从 ADD/UPDATE/DELETE/NOOP 四选一（top-s 相似记忆共同判定）；其 UPDATE=**原位改写**、DELETE=**物理删**。我们不做原位改写、不物理删（I5），以 supersedes 保留完整审计链——比 Mem0 更保守但可回放；其 ADD/NOOP 对应我们的查重铁律 + subject 冲突拦截。**已确认**。
- 「同 subject 至多一条 current」部分唯一索引 = **正确的收敛不变量**：等价于把 Kimball SCD Type 2 的 current 唯一性从纪律升级为 DB 约束（Type 2 规范三列：row effective date + row expiration date + current row flag，kimballgroup 官方，**已确认**，两源）；并发兜底（预检 + IntegrityError 双通道 + BEGIN IMMEDIATE 串行化，SQLite 官方文档确认 IMMEDIATE 立即取写锁、BUSY 可重试）足够。
- Wikidata statement ranks（preferred/normal/deprecated，值级「最新优先、旧值降档不删」思想）：**待验证**（wikidata.org 本轮网络不可达），仅作模式线索，不影响结论。

### Q4 主题身份：手工规范化键 vs 自动实体解析？

**答案：手工规范化键正确，自动 ER 不引入。**
- Splink（2.6k★, MIT, 活跃）与 Dedupe（4.5k★, MIT, 2025-07 后放缓）解决的是「**无先验键**的海量记录概率链接」（Fellegi–Sunter 两源比较模型；Splink 模型文档细节**待验证**，仓库元数据**已确认**）。
- 我们的场景有先验：**写入者本人知道主题**，subject 必填 = canonical key 路线，天然绕开概率链接的全部复杂度（特征比较、阻塞、阈值调参、训练标注）——这些与 stdlib/单写者/零依赖约束直接冲突。
- 最小可行 = 现方案（subject 必填 + 形态校验 + pool-usage 命名规范 + doctor 兜底）。可选增强见校准表（strip+lower 归一）。

### Q5 状态与理由：open/stale 二元 + reason 前缀够不够？AGM/TMS 能借什么？要不要 reason_code 列？

**答案：够，不拆列。**
- AGM（SEP 一手确认："On the Logic of Theory Change", JSL 50:510–530, 1985）：expansion（加不删）/ contraction（删不加）/ revision（Levi identity `K*p=(K÷¬p)+p`，先收缩 ¬p 再扩张 p）。映射到我们：**clue_add=expansion、clue_resolve(refuted:)=contraction、clue_add(supersedes=[旧])=revision**——状态机映射完备，证明现有 open/stale 二元 + 写动作组合无需扩第三存储态。**已确认**。
- Doyle TMS（AI 12(3):231–272, 1979, DOI 10.1016/0004-3702(79)90008-0，Crossref 确认）：justification 是结构化依赖记载——我们的等价物是 supersedes 链 + resolved/resolved_by/reason 审计文本；TMS 服务于推理机回溯，我们服务于人类审计，结构化到 reason 前缀粒度已够。**已确认**（题录）。
- de Kleer ATMS：**题录部分待验证**（Semantic Scholar 确认 title+"An assumption-based truth maintenance system"+1986 年，venue 字段空；卷页二手说法不一）。ATMS 是多环境假设枚举，我们是单一现实世界台账，无直接可借点——不影响结论。
- reason 前缀 = 「层1 枚举（status）+ 层2 结构化前缀 + 自由文本细节」的双层设计；Wikidata 的「rank 枚举 + qualifier 自由文本」双层共存是同构参照（**待验证**，仅线索）。拆 reason_code 列是过度固化：前缀集合尚在演化（refuted/expired/promoted/superseded/merged/other 六种），文本前缀零迁移成本可增删，等集合稳定再固化不迟（tech doc R6 已留升级路径）。

### Q6 读面语义：current 缺省 + 降序 + is_current 够吗？有无更好的 as-of/history 模式（不引库）？

**答案：够。as-of 有零新列的派生式，不建议加参数。**
- XTDB point-in-time 语义（官方文档，已确认）：「在 T 时刻的当前视图」= 按 (valid-time, tx-time) 双轴过滤。我们的等价派生式（全部用现有列）：
  `created <= T AND (resolved IS NULL OR resolved > T) AND (valid_until IS NULL OR _parse_ts(valid_until) > T) AND（superseded 链按 resolved > T 判定仍可见）`
  ——104 行规模下作为 pool-usage 文本约定/收口批手动查询即可，**不建议**加 `as_of` 参数（工具面 16 冻结；SQL 粗滤+Python 精滤口径与 tech doc §四一致，跨时区字符串比较不可靠的防御已有）。
- current 面降序（最新优先）+ all 面升序（审计序）：消费语义正确，无文献反例；Fowler bitemporal 的 record-history 读法（"on Mar 25th, we thought Sally's salary on Feb 25th was $6500"）说明「时点视角查询」是双轴读，我们的 all 面+created 排序是其单轴退化版，够用。**已确认**（Fowler 全文）。

### Q7 多代理共享：blackboard / tuple space 经典模式 + SQLite 并发建议？

**答案：我们是 blackboard 的强串行化特例，BEGIN IMMEDIATE + 唯一索引兜底足够。**
- HEARSAY-II blackboard（Erman, Hayes-Roth, Lesser, Reddy, ACM Computing Surveys 12(2):213–253, 1980, DOI 10.1145/356810.356816，Crossref 题录**已确认**；机制细节**待验证**——本机未能读原文）：共享数据结构 + 多知识源机会主义协作 + 调度器协调。
- Linda tuple space（Gelernter, ACM TOPLAS 7(1):80–112, 1985, DOI 10.1145/2363.2433，Crossref 题录**已确认**；机制细节**待验证**）：生成式通信、共享元组空间、时间/空间解耦。
- 两者的共同点：**以共享数据本身为协调媒介**。我们的差异点：MCP 单写者把「调度器/并发控制」从运行时搬到「会话串行 + DB 约束」——blackboard 的调度器职责由状态机+唯一索引承担。在 104 行/单机/单写者规模下这是正确简化；CRDT/LWW 分布式机制全部不需要（老板 clue#41 各机各账裁决进一步消除了跨机并发面）。
- SQLite（官方文档，**已确认**）：`BEGIN IMMEDIATE` 立即取写锁、避免 DEFERRED 读升级写的 BUSY 死锁；BUSY 可重试（busy_timeout 配合）；WAL 下 IMMEDIATE≡EXCLUSIVE。tech doc 的 IMMEDIATE 事务 + 条件 UPDATE（debt232 模式）+ IntegrityError 转译 + 部分唯一索引 = 并发面完备，**不建议** WAL 调优/守护进程（audit doc#1 已取舍）。

### Q8 评估：哪些评测任务类型可借来设计 F-06/QA 验收？

**答案：四个基准的四种任务构造法可直接借（只借任务设计，不引产品/依赖）。**
- **LongMemEval**（arXiv:2410.10813, ICLR 2025, 500 题）：五能力=信息提取 / 多会话推理 / **时间推理** / **知识更新** / **弃权**。knowledge-update 任务=「旧值在后续会话被新值覆盖，考能否取新」——与我们 supersedes 后 current 面只出新值同构。**已确认**（arXiv 摘要页）。
- **MemoryAgentBench**（arXiv:2507.05257, Hu/Wang/McAuley）：四能力=准确检索 / 测试时学习 / 长程理解 / **选择性遗忘**（含 conflict-resolution 切片）。**已确认**（arXiv API）。
- **MemBench**（arXiv:2506.21605, ACL 2025 Findings, Tan et al.）：事实记忆+反思记忆 × 参与/观察场景 × 有效性/效率/容量。**已确认**（arXiv API）。
- **LoCoMo**（arXiv:2402.17753, Maharana et al.）：QA + 事件摘要 + 多模态对话生成（300 轮 / 9K tokens / 最多 35 会话）。**已确认**（arXiv 摘要页）。
- 具体可借构造见 §八 评估任务建议。

---

## 四、开源实现对照表

> **全部仅模式参考，不替换现有池/线索表；引入价值=可借鉴的生命周期模式，功能利用率=若照搬其全部功能我们实际会用到的比例，集成可行性=按硬约束（stdlib+SQLite+单写者+零外部服务）评估。**

| 实现 | 活跃度 | 许可 | 引入价值（可借鉴模式） | 功能利用率 | 集成可行性 | 链接 |
|---|---|---|---|---|---|---|
| Graphiti（Zep） | 32,555★ · pushed 2026-11-03 · 活跃 | Apache-2.0 | bi-temporal 四时间戳、LLM 矛盾检测置 `t_invalid`、当前视图过滤 | ≈5%（只借失效模式，KG/LLM 管线全不用） | 不可行（需 KG 存储+LLM 调用管线） | github.com/getzep/graphiti · arxiv.org/abs/2501.13956 |
| Mem0 | 68,458★ · pushed 2026-11-04 · 活跃 | Apache-2.0 | 更新操作四分类（ADD/UPDATE/DELETE/NOOP）的冲突处理语义 | ≈5%（只借操作分类思想，向量检索/LLM 判定全不用） | 不可行（需向量库+LLM） | github.com/mem0ai/mem0 · arxiv.org/abs/2504.19413 |
| XTDB | 活跃（v2 演进中，文档完善） | 未核验（本轮只读文档） | bitemporal 双轴、point-in-time 查询、delete≠物理删 | ≈10%（只借 as-of 派生式与「过期≠删除」语义） | 不可行（整库替换，Clojure/JVM 栈） | v1-docs.xtdb.com/concepts/bitemporality |
| Datomic | 未核验（本轮网络未达） | 商业/免费层 | datom+history 的 bitemporal 思想（同族 XTDB 已覆盖） | — | 不可行（同上） | 待验证，仅线索 |
| Splink | 2,561★ · pushed 2026-11-04 · 活跃 | MIT | 概率实体解析（Fellegi–Sunter）的「身份先行」反面教材：有先验键时无需模型 | ≈0%（我们走 canonical key，不用概率链接） | 不引入（stdlib 手工键足够） | github.com/moj-analytical-services/splink |
| Dedupe | 4,527★ · pushed 2025-07-29 · 放缓 | MIT | 模糊去重/主动学习标注流程 | ≈0% | 不引入（同上） | github.com/dedupeio/dedupe |
| SQLite（已在用） | 持续维护 | Public Domain | BEGIN IMMEDIATE 写锁语义、部分唯一索引=不变量落库 | 100%（现有并发/约束全部承载层） | 已集成 | sqlite.org/lang_transaction.html |
| Wikidata/Wikibase | Wikimedia 运营 | —（数据模型文档） | statement ranks（值级降档不删）+ qualifier 双层设计 | 模式参考（本机未核验，待验证） | 不引入（外部服务） | wikidata.org/wiki/Help:Ranking（本轮不可达） |

---

## 五、对现方案的逐项校准表（十项）

> 裁决分级：**必须改 / 可选 / 不建议**。总判定：**必须改 = 无**，方案结构无需变更即可开工；可选项 3 条、不建议项 8 条均有理由。

| # | 项 | 现方案 | 文献/实践锚点 | 裁决 | 最小改动 / 不采纳理由 |
|---|---|---|---|---|---|
| 1 | subject | 全库唯一 `<归一域名>:<topic>`，部分唯一索引收敛 | canonical key 路线；有先验身份时无需概率 ER（Splink/Dedupe 适用面之外，元数据已确认）；Kimball Type 2 current 唯一性的 DB 约束化 | **保留**（必须改：无） | 可选：写入时对 subject 做 strip+lower 归一（+1 行，防 `DevFlow`/`devflow` 两写法重演 domain 三裂）。不建议：自动实体解析（无先验键场景才需要；依赖与约束冲突） |
| 2 | kind | fact 缺省 / state 可带 valid_until | state=带有效期的环境快照（XTDB valid-time；Graphiti valid/invalid 轴）；缺省 fact 误标代价不对称论证成立 | **保留**（必须改：无） | 最小改动：无。不建议：自动分类（「不自动判定真伪」同源；42 条候选保持 fact 由 curation 人工处理正确） |
| 3 | valid_until | state 专用、可空、`_parse_ts` 校验+归一存储、到期只退读面 | valid time 上界单点简化；XTDB「delete 只抹可见性，eviction 才物理删」同构「到期≠删除」；可逆恢复 | **保留**（必须改：无） | 不建议：`valid_from` 下界列（事实写入即生效，无回溯声明业务；+1 列+校验面纯增摩擦）。不建议：到期自动写状态（破坏可逆性与 I5 审计） |
| 4 | last_verified | fact 专用，回填=created，90 天 doctor 告警不自动下架 | verification time 概念；LongMemEval 时间推理任务证明无验证时间戳的事实会被高估；**90 天无文献公认值**（如实） | **保留**（必须改：无） | 最小改动：无。拍板③证据建议=先按 90 天跑，收口后按 doctor 告警率回调取值（§六）。不建议：复核守护进程（违反零后台约束） |
| 5 | supersedes / superseded_by | 单链前向指针，单事务原子取代，多 id=合并 | Graphiti `t_invalid` 前向失效同构（无 DAG、无 merge 节点，HTML 全文确认）；Mem0 四操作中我们取「显式取代」而非原位 UPDATE/物理 DELETE（审计更强）；TEPA 实证显式失效传播必要 | **保留**（必须改：无） | 可选：doctor 加一条「新行 valid_until 早于被取代行 resolved」倒挂检查（+6 行，见 §七 反例 3）。不建议：DAG/独立 merged_into 列（读面效果同一，省一列是 PRD 正确裁决）；不建议 invalid_at 区间下界（下界=created） |
| 6 | current | 读面派生式，不引第三存储态 | SQL:2011「约束只作用 current rows」；Kimball current flag；Graphiti 当前视图过滤 `t_valid ≤ now < t_invalid`——三源同构 | **保留**（必须改：无） | 最小改动：无。SQL 粗滤+Python 精滤（`_is_current_row` 单点）防御跨时区字符串比较，正确 |
| 7 | read（读面契约） | current 缺省 + 4 枚举 + 排序翻转；all 升序 | XTDB point-in-time 派生式（零新列可算 as-of）；Fowler record-history 读法（单轴退化够用） | **保留**（必须改：无） | 可选：pool-usage 补一句 as-of 派生式文本（0 行代码，供收口批/审计手动查询）。不建议：`as_of` 参数（工具面 16 冻结；104 行规模人工可扫） |
| 8 | is_current | pool_search clues 每行附派生标记，行不移除 | Kimball current flag 的读侧投影；「旧行必须留在检索面防 re-add 重复」= 查重铁律 + Fowler「record history is append only」同源 | **保留**（必须改：无） | 最小改动：无（tech doc 已把 fetch 三面顺手补齐，成本 +10 字节/行，预算内） |
| 9 | domain | 别名表归一（写入口+读入口+迁移清零） | canonical key 归一化；domain 三裂实证（devflow 10 / devflow-plugin 3 / devflow插件 4）证明等值过滤漏行代价 | **保留**（必须改：无） | 最小改动：无。可选：subject 项的 strip+lower 同属「写入口归一」一类，可与 #1 合并实施 |
| 10 | reason | 前缀约定（refuted:/expired:/promoted:/superseded:/merged:/other:），不拆列 | AGM 三操作映射完备（expansion/contraction/revision→add/resolve/supersedes），证明无需扩态；Doyle justification 的审计文本版；双层设计（status 枚举 + 前缀 + 自由文本）零迁移 | **保留**（必须改：无） | 最小改动：无（tech doc 已定 doctor notes 级无前缀提醒，避免存量 4 条 stale 误报）。不建议：reason_code 列（+1 列迁移+枚举校验，前缀集合未稳定，Wikidata「枚举+自由文本」双层同构支持现状；待验证标注） |

---

## 六、六项拍板证据建议

| # | 拍板项 | 建议默认 | 证据取得方法（本轮检索给出的事实） |
|---|---|---|---|
| ① | state 的 valid_until 是否强制必填 | **可空** | XTDB「未指定时 valid=tx 同值」（不设期是合法缺省，官方文档已确认）；本机快照 33 条带固定日期多为「写入时刻」非「失效时刻」，强制填期会逼写假数据。证据补强：curation 三组的 state 行（#3 python3 shim 带 180 天期）证明「有自然失效日才填」可行 |
| ② | subject 命名规范权威位置 | **pool-usage** | pool-usage 实测 48 行 / 60 预算，headroom 12 行（tech doc 实测）；规范是「纪律非机制」（有先验身份，机制已由唯一索引承担），归 skill 文本不归 cheatsheet |
| ③ | 90 天复核阈值启用与取值 | **启用 90 天** | 无文献公认值（如实）；证据方法=启用后按 doctor「超期未复核」告警计数实测：本机 104 条 last_verified 回填 created（最早 2026-11-03），90 天窗内零告警，首个告警批次出现后按噪声率回调。**取值属性=工程初值，非文献值，终报如实标注** |
| ④ | 首轮全量复核 | **不做** | TEPA 实证机械失效传播会错杀（MAB 冲突切片 1/3 gold 对「最新获胜」不可达）；42 条时间敏感候选全量改判=高摩擦低收益；curation 只动已证 3 组（6 行）正确 |
| ⑤ | token 增幅上限 | **5%** | tech doc 估算 tools/list +600 字节 ≈ <400 token/会话；实测回填点=README:39 基线行③（tools/list 字节断言，F-08） |
| ⑥ | reason 前缀 vs reason_code | **前缀约定** | AGM/TMS 映射证明状态机无需扩列；Wikidata「rank 枚举+qualifier 自由文本」双层同构（待验证，线索级）；前缀集合稳定后可走 tech doc R6 升级路径，现在固化过早 |

---

## 七、反例 / 失败模式（3 个）

1. **纯 append-only 无失效传播有害**（TEPA，arXiv:2608.07429，受控反转实验 50 seeds）：append-only 与 last-write-wins 在知识完全反转时 0.210，**低于无记忆基线 0.309**；可撤销（显式失效）机制 TEPA 达 0.950。映射：若只加 subject 不加 supersedes 纪律，#11+#65 双 open 并存就是这个失败模式的本机实例。我们方案「append-only 行保留 + 显式失效指针」正是实证指向的正确形态。（已确认，arXiv API）
2. **「新信息优先」启发式会错杀 + 评测 gold 自带噪声**（MAB 冲突切片分析，arXiv:2610.09193）：零学习 last-write resolver 达 80.25%，但 67/262K 题目（约 1/3 抽样子集）gold 对该规则不可达——被覆盖的旧陈述反而对应 gold。映射：clue_resolve/supersedes 前「新的一定对」不成立；误取代恢复=对新行再 supersedes、链条增长，属审计特性；**收口 curation 必须逐链人工核对，禁止机械批量执行**（拍板④的另一依据）。（已确认）
3. **双失效通道倒挂**（Kimball 区间衔接规则的反面教材）：Type 2 规范要求本行 expiration == 下一行 effective（kimballgroup 官方，已确认）；我们的 valid_until（自然到期）与 supersedes（显式取代）是两条独立失效通道，理论可构造「新行 valid_until 已过、被取代行 resolved 晚于该时点」的倒挂窗口。影响小（读面 current 派生式逐行独立判定，不依赖跨行衔接），doctor 加一条对账即可兜住（校准表 #5 可选项，约 +6 行）。其余 94 条 open 不动=「自然衰减」策略与「不做全量清洗」非目标自洽。

---

## 八、评估任务建议（借 LongMemEval / MAB / MemBench / LoCoMo 任务设计，不引产品）

1. **知识更新反转**（LongMemEval knowledge-update 构造法）：curation 三组迁移后，对每主题断言——缺省 current 面只出现新行；旧行 pool_search 可见且 `is_current=0`；`clue_fetch(subject=…, status='all')` 全链可追。对应 F-02/F-03/F-06 断言集，补一条「旧值审计可达」断言（防 TEPA 式「假删除」）。
2. **时效区间判定**（LongMemEval temporal reasoning 构造法）：构造 state 行 valid_until=过去/未来各一，断言读面差异 + 到期行 doctor 告警命中；进阶（可选）按 §三-Q6 派生式手动验证 as-of 时点视图。
3. **失效传播 / 弃权**（MAB selective-forgetting + LongMemEval abstention 构造法）：构造「新行 supersedes 旧行后，消费侧查询命中旧行」场景，断言纪律文本折价句存在（F-07 grep「is_current=0 只作审计」）；弃权任务反向借鉴=「current 面查无此事实时，agent 应查重后 clue_add 而非臆造」——可作为 agent 评测任务集（docs/research/agent-eval-tasks.md）候选用例。MemBench 的「参与/观察」双场景提示：验收批应覆盖「写入者自验」与「下游只读消费」两个视角（现有 F 矩阵已隐含，命名对齐即可）。

---

## 九、来源清单（标注精读/扫描；已确认/待验证/推测）

**精读（全文/官方文档/一手 API 实取）**
1. Zep: A Temporal Knowledge Graph Architecture for Agent Memory — arxiv.org/abs/2501.13956 + HTML 全文（四时间戳/失效触发/当前视图）【已确认】
2. Mem0 — arxiv.org/abs/2504.19413 + HTML 全文（ADD/UPDATE/DELETE/NOOP tool-call 判定）【已确认】
3. Fowler, Bitemporal History — martinfowler.com/articles/bitemporal-history.html（Snodgrass 术语出处、record history append-only）【已确认】
4. XTDB Bitemporality — v1-docs.xtdb.com/concepts/bitemporality（双轴/point-in-time/delete≠物理删）【已确认】
5. SQLite BEGIN TRANSACTION — sqlite.org/lang_transaction.html（IMMEDIATE/BUSY/WAL 语义）【已确认】
6. SEP, Logic of Belief Revision — plato.stanford.edu/entries/logic-belief-revision（AGM 三操作/Levi identity/JSL 题录）【已确认】
7. LongMemEval — arxiv.org/abs/2410.10813（五能力/500 题/ICLR 2025）【已确认】
8. LoCoMo — arxiv.org/abs/2402.17753（任务类型/规模）【已确认】
9. MemoryAgentBench — arXiv API（2507.05257，四能力）【已确认】；MemBench — arXiv API（2506.21605，双层级双场景）【已确认】；TEPA（2608.07429）与 MAB 冲突分析（2610.09193）摘要【已确认】
10. Crossref API：Doyle TMS（AI 12(3):231–272, 1979, DOI 10.1016/0004-3702(79)90008-0）、HEARSAY-II（ACM CSUR 12(2):213–253, 1980, DOI 10.1145/356810.356816）、Linda（TOPLAS 7(1):80–112, 1985, DOI 10.1145/2363.2433）题录【已确认】
11. api.github.com：graphiti/mem0/splink/dedupe 仓库元数据（star/license/pushed_at）【已确认】

**扫描（搜索结果级/摘要级，未读全文）**
12. SQL:2011 时态特性（Kulkarni & Köhler DOI 10.1145/2380776.2380786 + PostgreSQL Wiki SQL2011Temporal + MS Learn temporal tables）【已确认（两源）】
13. Kimball SCD Type 2（kimballgroup.com 官方技术页 + Part 2 文章：三列/区间衔接）【已确认（两源）】
14. Splink/Dedupe 适用域（Fellegi–Sunter 概率链接）【仓库元数据已确认；模型细节待验证】

**待验证（本轮网络受限未核验，仅作线索，不影响结论）**
15. Wikidata statement ranks（wikidata.org/wiki/Help:Ranking）——wikidata.org 与 wikipedia.org 本机持续超时，三档语义未核实【待验证】
16. Datomic datom/history 模型【待验证；XTDB 已覆盖同族启示】
17. de Kleer ATMS 卷页（S2 确认 title+1986，venue 空）【部分待验证】
18. HEARSAY-II / Linda 机制细节（题录已确认，原文未读）【待验证】

**推测（本报告分析段落）**
19. 所有「最小启示/落点/校准裁决」段落为基于上述已确认来源的分析推导，非来源原文【推测】

---

## 十、遗留问题（3 条）

1. **Wikidata ranks 与 Datomic 两处待验证**：本机到 Wikimedia 基础设施持续超时（5 次重试），不影响主结论（已有 XTDB+SQL:2011+Kimball 三源覆盖）；如需闭环可在网络可用环境补 1–2 次 WebFetch。
2. **90 天阈值无文献公认值**：终报如实标注为工程初值；首个 doctor 告警批次出现后应回调（建议在 pool-usage 或 backlog 记一笔「90 天后复盘告警率」）。
3. **三个可选项未拍板**（subject strip+lower、doctor 倒挂检查、as-of 派生式文本）——均为 ≤6 行级最小改动，建议随实现批顺带裁决，不阻塞方案评审。
