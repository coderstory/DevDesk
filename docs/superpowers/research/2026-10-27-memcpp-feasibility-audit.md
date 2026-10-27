# memcpp 可行性报告独立审计

- 审计对象：`docs/superpowers/research/2026-10-27-memcpp-feasibility.md`（470 行）
- 原始方案：`D:/Data/desktop/memcpp技术方案.md`
- 审计方式：独立复跑 + 官方文档核实 + 上游源码抽验；不采信被审文档的自述数字
- 工作树：`D:/wt-mem/audit`（mem/audit-feasibility）
- 时间：2026-10-27

## 总体裁决

**修后可**。报告的主体方向（有条件可行、关闭思考、只增不改、首版不上 HNSW、注入需常驻进程）被独立证据支撑，可以作为决策底稿；但有两处 blocker——**检索评测与推荐的 FTS5 trigram 方案不匹配**、**云端外发/脱敏表述与真实管线不符且隐私风险未评估**——以及若干 major 级过度表述，修掉后才能作为正式决策依据。

## 独立证据摘要（本次实际执行的核验）

- 网关：从 `~/.cc-switch/cc-switch.db` 重新枚举 15 个 provider，其中 10 个带 URL+token，去重 4 个域名；qianfan/fangzhou 的 `/v1/embeddings` 404、bailian 返回 `model null`、MiniMax 原生格式返回 `insufficient balance`；`/v1/messages`（Bearer，`qianfan/deepseek-v4.1-flash`）返回 200。
- 思考开关：用实验台真实 4KB 段 + `mem_batch` 抽取提示词，各 2 次、`max_tokens=4000`：thinking 默认 2/2 `stop_reason=max_tokens`、正文 0 字；`thinking:disabled` 2/2 `end_turn`、正文 1.8~2.6KB 且 JSON 合法。另用 `compress.py` 的 1~3 条提示词 + 2.3KB 段：thinking 默认 2/2 成功——**失败与提示词/输出长度强相关，不是模型固有属性**。
- 产出统计：`mem_obs_batch.json` 独立统计 31 条、唯一标题 27、3 组重复（4 条冗余）、缺 evidence 0 条；31/31 条 evidence 均能在两个源 jsonl 中逐字找到。
- 检索：`mem_eval.py` 重跑 3 次——Recall@5 全 1.00（稳定）；MRR 分别为 bm25 0.83/0.79/0.83、vec 0.78/0.66/0.76、hybrid 0.84/0.78/0.82；报告值 0.82/0.83/0.90 未能复现，且 hybrid 并非每次高于 bm25。
- 注入：`mem_inject.py` 重跑——5/5 文本有变化，但按“项目内具体做法”人工判定只有 3/5 明确采用了记忆（Q3/Q4 仍是通用建议）；实验强制注入 top-3，且未做重复对照。
- 工程：SQLite 3.50.4 trigram 可用；真实 6 列 schema 1 万条插入 1.47s、查询 0.04~0.57ms；**2 字中文查询（端口/配置/模块）返回 0 行**。Python 冷启动 171~185ms，`spawn.exe` 52~55ms（3 次）。
- 协议：抓取 `https://code.claude.com/docs/en/hooks.md` 核实——`additionalContext` 明确覆盖 `PreToolUse`/`UserPromptSubmit`/`SessionStart`/`PostToolUse`；10,000 字符上限存在（超限落文件+2,000 字预览，非丢弃）；`UserPromptSubmit` 的 command/http/mcp_tool 默认超时降为 30s；**官方支持 `type:"http"` hook**，可直接 POST 到本地常驻进程，报告未考虑。
- 源码：claude-mem/mem0/graphiti 的 sha 与路径全部命中；mem0 的 `get_update_memory_messages` 仅测试有调用点。

## 断言核验表

| # | 报告断言 | 判定 | 独立证据（命令/文件/输出摘要） | 严重度 | 需要怎么改 |
|---|---|---|---|---|---|
| 1 | 16 个项目、1164 个 jsonl、约 1.4GB | CONFIRMED | 递归 `find ~/.claude/projects -name '*.jsonl'`：16 个项目、1178 个 jsonl、`du` 1.4G（数字随会话增长，1164 为当时快照） | — | 注明“快照时间”，无需实质修改 |
| 2 | 本项目 19 个主会话、约 1.1GB | CONFIRMED | `D--project-gui` 顶层 19 个 jsonl，递归 859 个（含子代理/会话子目录），1.1G | — | 注明“主会话=顶层 jsonl，不含子代理” |
| 3 | 摄取 2 会话 1412 条、坏行 0、二次执行新增 0 | CONFIRMED（机制） | `memory.db` events：109+1303=1412；ingest_state 按 byte offset 续读。二次执行“新增 0”无法原地复测（两个源文件已分别从 1.60MB/5.78MB 增长到 2.80MB/6.47MB） | minor | 补一句“文件稳定时幂等；增长时会增量摄取” |
| 4 | 单会话清洗后 575 事件 / 193,752 字 / 40 段 | CONFIRMED（±5%） | 复刻 `mem_clean.py` 规则得 601 事件 / 200,301 字 / 42 段；差异来自清洗阈值 | minor | 注明清洗规则与统计时点 |
| 5 | 31 条产出、3 组重复标题、27 条唯一 | CONFIRMED | `mem_obs_batch.json`：31 条；3 组重复共 4 条冗余；唯一标题 27；缺 evidence 0 | — | 无需改 |
| 6 | 证据链可信（evidence） | CONFIRMED | 31/31 条 evidence 在两个源 jsonl 中逐字命中 | — | 建议把“证据逐字校验”写进报告作为正面证据 |
| 7 | 开思考 15 段仅 3 成功、12 失败（全部 max_tokens、正文 0） | CONFIRMED（机制）/ 计数 UNVERIFIABLE | 真实 4KB 段 + 实际抽取提示词：thinking 默认 2/2 `max_tokens`、正文 0；关闭 2/2 成功。但 2.3KB 段 + 1~3 条提示词时 thinking 默认 2/2 成功。原始 15 段运行无日志/查询集留存 | major（表述） | 把结论限定为“在该抽取提示词 + 4KB 段 + 4000 预算下”；补存原始运行日志 |
| 8 | 关闭思考 5/5 成功、0.9~9.4s | CONFIRMED（方向） | 本次 4 次关闭思考均成功（4.3~6.6s）；耗时随负载/段长波动 | minor | 标注“单次快照” |
| 9 | 产出密度 0/0/2/0/14 与 8/12/1/0/3 | UNVERIFIABLE | 无该两次运行的留存产物；最终 `compress.py` 已限制 3 条/段，与“14 条”条件不同 | minor | 说明两次运行使用的提示词版本不同 |
| 10 | 单事件最大 32KB | OVERSTATED（方向反了） | 实验两会话中最大 text 事件 86.5KB；全 GUI 最大 text 事件 290KB | minor | 改成“单事件可达几十~几百 KB”，截断阈值按实测定 |
| 11 | 31 条产出 3 组重复约 10% | CONFIRMED | 3 组 / 4 条冗余（13% 条目），3 组/31 ≈ 9.7% | — | 写清“按组还是按条目” |
| 12 | 10 个网关配置、4 个上游域名 | CONFIRMED | cc-switch 15 个 provider，10 个有 URL+token；去重域名 4 个（office/codingxrui/minimax/deepseek） | — | 无需改 |
| 13 | 全部网关无可用 embedding 接口 | OVERSTATED | qianfan/fangzhou `/v1/embeddings` 404；bailian 返回 `model null`；office 403/404；**MiniMax 有原生 embeddings 端点**（`texts` 格式），但返回 `insufficient balance` | minor | 改为“无可用（有余额且 OpenAI 兼容）的 embedding 接口”；注明 MiniMax 是余额问题而非无接口 |
| 14 | `qianfan/deepseek-v4.1-flash` + Bearer 可用 | CONFIRMED | `/v1/messages` 返回 200，`stop_reason=end_turn`，echo model=`deepseek-v4.1-flash-1017` | — | 无需改 |
| 15 | Recall@5 全文/向量/混合 = 1.00 | CONFIRMED | 3 次重跑均 1.00（27 条 / 15 问） | — | 保留，但强调天花板效应 |
| 16 | MRR 全文 0.82 / 向量 0.83 / 混合 0.90 | UNVERIFIABLE / OVERSTATED | 3 次重跑：bm25 0.79~0.83、vec 0.66~0.78、hybrid 0.78~0.84；查询由 LLM 每次重新生成，无冻结查询集 | major | 冻结 15 条查询并入库，报告改为“多次运行的区间”；不再写单点 0.90 |
| 17 | “全文”指 FTS5 trigram | REFUTED | `mem_eval.py` 的全文路是 Python BM25（字符 bigram + 正则 latin token），全程未调用 SQLite/FTS5 | blocker | 要么用真实 FTS5 trigram 重测，要么把该路改名为“BM25-bigram 代理” |
| 18 | trigram 中文不需分词即可子串匹配 | OVERSTATED | SQLite 3.50.4：3 字以上中文匹配正常；`端口`/`配置`/`服务`/`骨架` 等 **2 字查询返回 0 行**，`端口*` 也无效，仅 `LIKE '%端口%'` 可用 | blocker | 方案补“2 字查询走 LIKE/字符 bigram 兜底”或改用 unicode61+bigram；评测必须覆盖 2 字中文 |
| 19 | 混合 MRR 高于单路，方向一致 | OVERSTATED | 重跑 hybrid 并非每次都高于 bm25（0.84>0.83、0.78<0.79、0.82<0.83） | major | 改“混合稳定优于向量；与 BM25 互有胜负” |
| 20 | 注入 5 题 4 题从通用建议变为项目内做法 | OVERSTATED | 重跑：文本变化 5/5；按“明确采用项目记忆”判定 3/5（Q3/Q4 仍为通用建议）；单次运行、未做重复对照 | major | 改为“3/5 明确采用，1 题边界，1 题未采用”；补重复运行与人工判定表 |
| 21 | 主动唤起“机制成立” | OVERSTATED | 只做了离线“强制注入 top-3”对比；hook 全链路一次未跑（报告 §7.9 自认）；无触发/未命中统计 | major | 改为“协议支持 + 强制注入初测有效；真实唤起率与 hook 投递未验证” |
| 22 | UserPromptSubmit/SessionStart/PostToolUse/PreToolUse 均支持 additionalContext | CONFIRMED | 官方 hooks 文档 “Where the reminder appears” 明确列出 PreToolUse/PostToolUse 等 | — | 无需改；补文档版本/日期 |
| 23 | 单条注入上限 10,000 字符 | CONFIRMED | 官方文档：单字符串上限 10,000；超限落 session 文件，给 2,000 字预览（不是截断丢弃） | minor | 补“超限落文件”行为，避免误以为内容被丢 |
| 24 | UserPromptSubmit 默认 600→30 秒 | CONFIRMED | 官方文档：command/http/mcp_tool 在 UserPromptSubmit 上默认降为 30s | — | 无需改 |
| 25 | 异步 hook 超时会丢弃输出 | OVERSTATED（机制表述错） | 文档：async hook 不受 timeout 约束；是**同步** hook 超时才取消并丢弃输出；async 输出本就不用于注入 | minor | 改为“注入必须同步；同步超时会丢弃 additionalContext” |
| 26 | hook 不能每次提问起新进程，必须是硬约束 | OVERSTATED | `spawn.exe` 实测 52~55ms（报告 87~123ms）；3.1s/9.1s 异常值仅 2 次观测，无法复现；官方支持 `type:"http"` hook，可直接 POST 本地常驻进程，无需薄转发进程 | major | 结论保留“常驻进程”，但把“硬约束”降为“体验选择”；设计改用 HTTP hook，删掉“hook 进程只做管道转发” |
| 27 | FTS5 1 万条建索引 4.9s、查询 0.8~8.9ms | OVERSTATED（保守方向） | 真实 6 列 schema + 触发器：插入 1.47s；查询 0.04~0.57ms | minor | 用本机复测值替换，或标注原值的查询/数据条件 |
| 28 | Python 冷启动 265~513ms；C++ exe 稳态 87~123ms | OVERSTATED / 未复现 | 本机 3 次：Python 171~185ms；spawn.exe 52~55ms | minor | 标注测量条件；结论“进程启动不可忽略”仍成立 |
| 29 | claude-mem 的 IDF 余弦 + 稀有词否决 | CONFIRMED | `src/services/dedup/{idf,tfidfCosine,idfVeto,nearDuplicate}.ts` 存在；nearDuplicate 注释含 7,651 条验证与 simhash 假阳性结论 | — | 注明“7,651 条验证来自上游注释，非本项目复测” |
| 30 | Graphiti 保守矛盾判定、只置 invalid_at | CONFIRMED | `graphiti_core/prompts/dedupe_edges.py`：显式“stopped being true / replaced / terminated”才判矛盾，禁止从 recency/episode 日期推断 | — | 无需改 |
| 31 | mem0 主链路 ADD-only，四操作提示词无调用点 | CONFIRMED（细微偏差） | `main.py:881` 存在；`prompts.py:176` 存在；`get_update_memory_messages` 生产代码无调用点，仅 `tests/configs/test_prompts.py` 调用 | minor | 把“无调用点”改为“生产代码无调用点，测试保留” |
| 32 | 六个仓库 sha 与文件路径 | CONFIRMED | 逐个 `git rev-parse HEAD` 与报告一致；llama.cpp `examples/embedding/embedding.cpp`、`llama.h` 的 `llama_get_embeddings_seq` 等均在 | — | 无需改 |
| 33 | “实测”数字均有实验依据、可追溯 | REFUTED | 除 Recall@5 外，关键脚本（mem_batch/mem_think/mem_eval/mem_inject）只在 `%TEMP%`，查询集/MRR/15 段日志/4/5 判定均未入库；`D:/wt-mem/w` 路径不存在 | major | 把实验脚本、冻结查询集、原始输出归档进仓库；数字附 artifact 路径 |
| 34 | “只有脱敏后的抽取片段外发，原始工具 I/O 不出机器” | REFUTED | `mem_batch.py` 实际把 `[tool_use]` 与 `[tool_result]` 截断文本、`system-reminder`、插件/技能文档一起发给云端网关；代码中无脱敏函数 | blocker | 补真实脱敏层（密钥/路径/PII 白名单）并复测；否则改写分发章节的隐私承诺 |
| 35 | 第 5 节 90%/20%/30%/500ms/0.5% 等硬门槛 | OVERSTATED | 均为未推导的建议值；无用户拍板或外部依据 | minor | 明确标注“建议门槛，待拍板”，不要称“硬门槛” |
| 36 | 6~10 人周 / 8~14 人周 | UNVERIFIABLE | 估算，无同类项目基线；P0.5 spike 与常驻进程/IPC 的实际成本未计入 | minor | 标注不确定性区间与 spike 前置 |
| 37 | 附录引用 `D:/wt-mem/w/...` | REFUTED | `D:/wt-mem/w` 不存在；实际文档在 `D:/wt-mem/audit/docs/...` | minor | 改为当前工作树路径 |

## 报告未覆盖的重大风险

1. **记忆污染长期不可逆 + 提示注入**（blocker 级遗漏）
   报告只列了“长期噪声与记忆膨胀”，没有把“被污染的抽取结果会被当作事实反复注入”当作风险。实际管线会把工具输出/被引用文档抽成记忆（报告自己也观察到插件/技能文档被抽成记忆）；`ADD-only` 让错误条目没有回收路径。需要：来源与证据链、类型/来源白名单、注入前的“未复核隔离”、可回滚版本链、以及一次红队测试（往会话里塞一条错误事实，验证它不会在后续会话被当真理注入）。

2. **云端外发边界与隐私/合规**（blocker）
   报告 §8.1 声称“只有脱敏后的抽取片段外发”，但代码里没有脱敏；`mem_batch.py` 把工具 I/O 摘要、`system-reminder`、路径、可能的密钥片段发给公司共享网关。报告未评估：网关的数据留存/审计策略、跨项目代码外发、用户凭据/令牌误入抽取文本、以及“本地+网关”分发形态的合规成本。建议先实现并验证脱敏层，再让用户拍板 §8.1。

3. **评测不可复现与选择偏差**（major）
   查询由 LLM 每次重新生成，未冻结；MRR 单次快照；15 题的正确答案就是生成查询时输入的同一批记忆。除 Recall@5 外，报告的数字无法由第三方重放。需要：冻结 15（或 30）条查询与标准答案入库、固定模型/温度、保存原始响应、报告多轮均值与区间。

4. **trigram 对 2 字中文/代码符号的召回缺口**（blocker，已在上表 #17/#18 展开）
   报告 §7.14 只承认“代码标识符未专门测”，但没发现 2 字中文查询直接 0 命中。这会直接影响“不做自定义分词器”的方案取舍。

5. **模型资产许可证与分发**（minor~major）
   §7.11 列了“GGUF 许可证未核对”，但 §8.2 仍推荐具体 embedding 模型，且没有分析 bge 模型许可证、GGUF 再分发条款、首次下载/包体与“单 exe 零依赖”的冲突。建议在拍板模型资产前做一次许可证清单。

6. **Hook 协议版本漂移**（minor）
   `additionalContext`、`PreToolUse` 注入、10,000 字符上限、30s 超时都是“当前文档”的事实；报告未钉文档版本，也未做本机 hook 冒烟。长期运行需要版本检测/契约测试/回退到 MCP。

7. **Windows 常驻进程的安全与生命周期**（minor）
   命名管道/本地 TCP 的 ACL、一次性令牌、崩溃自拉、升级时的进程替换、日志中的敏感信息都未评估；报告只说“用命名管道或 TCP”。

8. **成本与配额**（minor）
   报告自认“云端费用没有记账”；按 40 段/会话、19 会话、每段数千 token 估算，费用与网关配额可能成为硬约束；需要预算与 per-call 成本日志。

## 过度自信表述（逐条）

- “机制全部跑通过”“主动唤起机制成立”——hook 全链路一次未跑，注入实验是离线强制注入。
- “检索 可行（质量待扩样）”——全文路是 BM25 bigram 代理，不是 FTS5 trigram；trigram 2 字查询 0 命中；应降为“机制原型可行，质量与全文实现均未验证”。
- “4/5 从通用建议变为项目内具体做法”——独立重跑为 3/5 明确采用，且单次、无对照。
- “混合 MRR 0.90 高于单路，方向一致”——重跑 0.78~0.84，且与 BM25 互有胜负。
- “hook 不能每次提问起新进程……是硬约束”——实测 52ms，官方 HTTP hook 可完全避免进程启动；“常驻”是体验/异常值下的合理选择，不是协议硬约束。
- “向量必须本地算，这不是可选项”——MiniMax 有原生 embedding 端点（当前余额为 0），结论今天成立，但不是“没有接口”。
- “C++ exe 稳态 87~123ms”“Python 265~513ms”——本次复测 52~55ms / 171~185ms；异常值 3.1s/9.1s 未复现。
- “单个事件可达 32KB”——实验会话中最大 86.5KB，全 GUI 290KB；报告低估了截断的必要性。
- “第 5 节硬门槛”——数值均为建议值，无推导、无用户拍板；称“硬”会误导决策。

## 我认为最可能翻盘的三条

1. **检索证据与方案不匹配**：`mem_eval.py` 用 Python BM25 字符 bigram 冒充“全文”，而推荐方案是 FTS5 trigram；trigram 对“端口/配置/模块”这类 2 字中文查询 0 命中。修正后“检索可行”可能降级为“需换分词/兜底方案后再评”，并直接影响 P1 的范围与工期。
2. **主动唤起价值被高估**：4/5 实际 3/5、无 hook 全链路、无真实唤起率；若 30 次真实提问的采纳率达不到第 5 节门槛（30%），报告 §6.3 把注入提前到 P1 的排期理由就不成立，应退回 MCP 优先。
3. **隐私/脱敏与记忆污染**：实际管线把工具 I/O 摘要和注入块发到共享网关且无脱敏；同时 ADD-only 会把错误/恶意事实长期注入。若用户或合规不接受该外发边界，“本地+中转网关”这一推荐分发形态会被直接否决，P0/P3 的方案与成本结构随之改写。

## 复现命令（审计使用，脚本在 `%TEMP%`，未入仓）

```bash
# 网关/模型/embedding（不打印 token）
/d/software/venvs/memory-lab/Scripts/python.exe -I probe_msg.py
/d/software/venvs/memory-lab/Scripts/python.exe -I embed_probe.py
# 产出统计 / 检索 / 注入
/d/software/venvs/memory-lab/Scripts/python.exe -I mem_eval.py
/d/software/venvs/memory-lab/Scripts/python.exe -I mem_inject.py
# FTS5 trigram（含 2 字中文）
/d/software/venvs/memory-lab/Scripts/python.exe -I fts_real.py
# 进程冷启动
/d/software/venvs/memory-lab/Scripts/python.exe -I spawn_test.py
# 官方 hook 文档：抓 .md 端点（网页端点只返回片段）
node D:/project/x-rs-tools/plugins/x-coding-tools/skills/cs-web-fetch/scripts/fetch.js https://code.claude.com/docs/en/hooks.md
```

*审计结论仅针对 2026-10-27 工作树内可见证据与本次复跑；`%TEMP%` 脚本未归档，复现前需先固定这些实验脚本。*