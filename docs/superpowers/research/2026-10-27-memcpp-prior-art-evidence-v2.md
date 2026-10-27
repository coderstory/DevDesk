# memcpp 业界实证独立复核 v2（量化档位 / FTS5 中文 / 记忆人审先例）

- 检索日期：2026-10-27；工具：WebSearch（多次 429 限流，按预案改用 WebFetch 直抓官方站为主：官方文档、HuggingFace 模型卡与 API、arXiv、GitHub raw 源码）。
- 性质：对 `2026-10-27-memcpp-prior-art-evidence.md`（下称 R1）三问**独立重查**——先不看旧结论、自扫一手来源，最后一步才读 R1 逐问对表。本文件不改 R1 与任何其他文件。
- 纪律：术语首现一句人话；查不到如实写"未查到 + 已查来源"，不做推断填充。

## 1. 量化档位的召回代价（拍板②：Q8_0 26MB vs Q4_K_M 15MB）

### 结论（3 行内）
1. 8-bit 档检索质量保留 90.8%~100%（不加重排）再次独立证实；同级小模型（约 22M 参数）8-bit 只保 90.79%——量化伤害与模型大小相关，24M 级处于敏感区。
2. **再次确认：未查到任何 embedding 模型 4-bit（Q4_K_M 属此类）检索质量的公开实测**。本轮比 R1 多走了 llama.cpp issue 检索 API×2、Reddit 多轮、中文社区、arXiv，均为阴性。
3. 新增相邻证据：2026-01 HF 论文实测 Llama-3.1-8B（生成式大模型）Q4_K_M 仅掉约 0.32 平均分——但那是 8B 级模型；模型越小冗余越少、量化越伤，不能类比 24M embedding。

（量化 = 把模型权重从 16 位小数压成 8/4 位整数换体积速度；MTEB = 公开向量质量基准，C-MTEB 为其中文版；重排/rescore = 用原始精度向量对候选二次打分。）

### 证据表
| # | 来源（URL） | 关键数字/原文 | 日期 |
|---|---|---|---|
| 1 | HF 官方博客（mixedbread 量化研究）https://huggingface.co/blog/embedding-quantization | int8 不加重排 MTEB Retrieval 保留率：mxbai-embed-large-v1 97%（52.79/54.39）、e5-base-v2 94.68%、**all-MiniLM-L6-v2（约 22M，与 bge-small 同级）90.79%**（37.82/41.66）、Cohere-v3 100%；"A rescore multiplier of 4-5 already leads to a remarkable performance retention of 99% using int8"；二值不加重排 74.77%~92.53%；警示原文 "quantization doesn't universally work with all embedding models"（e5-base 二值崩到 74.77%） | 访问 2026-10-27 |
| 2 | 官方模型卡 https://huggingface.co/BAAI/bge-small-zh-v1.5 | 24M 参数、512 维；C-MTEB Retrieval 61.77 / Avg 57.82；MIT | 访问 2026-10-27 |
| 3 | GGUF 模型卡 https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf | 经 HF API 搜索证实为**全网唯一** bge-small-zh-v1.5 GGUF 仓；q8_0 26MB / q4_k_m 15.4MB；厂商仅定性："quantization will not provide huge benefits, but it does generate up to a 30% speedup on CPU with minimal loss in accuracy"（无分档质量数字） | 访问 2026-10-27 |
| 4 | HF 论文 2601.14277 https://huggingface.co/papers/2601.14277 | "Which Quantization Should I Use? A Unified Evaluation"（2026-01）：只测生成式 LLM（Llama-3.1-8B）；平均分 F16 69.47 / Q8_0 69.41 / Q4_K_M 69.15（Q4_K_M −0.32，Q8_0 −0.06）；论文还称在 LLM 场景 Q8_0 的"体积-质量比"不如 Q5_0 划算——**LLM 结论，不外推到 embedding，更不外推到 25MB 小模型** | 访问 2026-10-27 |
| 5 | Reddit r/LocalLLaMA（社区帖，未证实级）https://www.reddit.com/r/LocalLLaMA/comments/1sklc53/ | 实测贴使用 Nomic Embed v1.5 **Q4_K_M GGUF（84MB）**，评价 "fast, small, and surprisingly good"——4-bit embedding 有社区真实在用，但帖内无量化前后召回对照数字 | 访问 2026-10-27 |
| 6 | **未查到项** | "GGUF 4-bit embedding 检索质量公开基准"仍未查到。已查：llama.cpp 仓库 issue 检索 API 两轮（embedding+quantization / embedding+Q4_K_M，命中均为功能支持与崩溃类，无质量评测）、Reddit 多轮、中文社区（CSDN 系文章只有"int8 召回损失 0.5%~2% 以内"的自称经验值）、arXiv API（超时未返）。中文圈另传"BAAI/bge-small-zh-v1.5-quantized 官方 int8 仓"，经 HF API 搜索**未证实**（不存在该仓库） | 2026-10-27 |

### 与旧结论对表（R1 §1）
- **一致（加固）**：R1 三条结论全部复现，数字逐一对上；"4-bit 无公开实证"的阴性结论经更宽渠道双查仍成立。
- **旧版漏了**：① LLM 侧 Q4_K_M 最新系统评测（证据 4——反而强化"小模型谨慎"，因 8B 大模型近乎无损的数字不能给 24M 模型背书）；② 厂商定性声明（证据 3）；③ 社区 Q4 真实用例（证据 5，说明 4-bit 非无人区，只是零数字）。
- R1 证据 4 所引 sbert 旧版 URL 本轮未复核成功（sbert 站相关路径 404）；其内容与 HF 官方博客一致，无实质分歧。

## 2. SQLite FTS5 中文检索（P0：双字分词 + unicode61 + LIKE 兜底）

### 结论（3 行内）
1. 官方文档原文逐句复核通过：trigram（3 字滑窗切块）少于 3 字符 "do not match any rows"；unicode61（按 Unicode 类别切词的内置切词器）下中文连续串成一个 token；`LIKE '%xx%'` 官方明文 "this optimization is not attempted"（必走全表线性扫描）。
2. 双字（bigram，2 字滑窗）先例比 R1 记的更厚：MySQL 内置默认之外，新增 PostgreSQL 的 pg_bigm（NTT DATA 系，2026-08 仍在维护）与 Lucene/Elasticsearch 官方 CJKAnalyzer（"indexes bigrams"，官方示例"我是中国人"）——三大数据库/搜索引擎生态齐了。
3. 驼峰标识符（如 getUserById）：R1"所有先例都不切分"措辞过强——Solr 官方 WordDelimiterGraphFilter 默认按大小写边界切（"CamelCase"→"Camel, Case"）；但 SQLite/代码搜索先例（simple、Zoekt）确实不切、靠子串兜底。**裁决：维持不切分**（FTS5 无现成切分过滤器，需自写，收益未证明）。

（FTS5 = SQLite 内置全文检索引擎；tokenizer/切词器 = 把文本切成检索单元的组件；LIKE = SQL 模糊匹配；jieba = 流行中文分词词典。）

### 证据表
| # | 来源（URL） | 关键原文/事实 | 日期 |
|---|---|---|---|
| 1 | SQLite 官方 FTS5 文档 https://www.sqlite.org/fts5.html | trigram："Substrings consisting of fewer than 3 unicode characters do not match any rows when used with a full-text query."；LIKE/GLOB 兜底 "falls back to a linear scan of the entire table"；unicode61：Unicode 类别 L/N/Co 为 token 字符，"Each contiguous run of one or more token characters is considered to be a token"（中文整串成单 token，证实必须外部预分词）；全文无一处提及 CJK/中文 | 访问 2026-10-27 |
| 2 | SQLite 官方查询计划文档 https://www.sqlite.org/optoverview.html | LIKE 走索引的条件之一是右值 "does not begin with a wildcard character"，"%xxx%" 形态 "this optimization is not attempted"——兜底只能线性扫。官方未给 WAL 日志模式下的专项基准（未查到），扫描成本由行数决定、与日志模式无关属机制层推断 | 访问 2026-10-27 |
| 3 | MySQL 8.0 官方手册 https://dev.mysql.com/doc/refman/8.0/en/fulltext-search-ngram.html | "The ngram parser has a default ngram token size of 2 (bigram)."；搜索词自动转 ngram 短语（'abc'→'"ab bc"'）；停用词按"包含"剔除 | 访问 2026-10-27 |
| 4 | pg_bigm（新增先例）https://github.com/pgbigm/pg_bigm | PostgreSQL 全文检索扩展："create 2-gram (bigram) index for faster full text search"；158 stars、默认分支 2026-08 有推送、PostgreSQL License、版权链 2012-2026（NTT DATA 起源）——bigram 的第二个数据库内置级先例 | 访问 2026-10-27 |
| 5 | Lucene 官方 javadoc https://lucene.apache.org/core/7_7_3/analyzers-common/org/apache/lucene/analysis/cjk/package-summary.html | CJK 分析器 "indexes bigrams"；官方示例"我是中国人"三分对照：bigram（我是-是中-中国-国人）/ 一元（我-是-中-国-人）/ SmartChinese 词级（我-是-中国-人）——ES 系 bigram 官方先例 | 访问 2026-10-27 |
| 6 | Solr 官方指南 https://solr.apache.org/guide/solr/latest/indexing-guide/filters.html | WordDelimiterGraphFilter："A change in case within a word: 'CamelCase' → 'Camel', 'Case'"，默认开、可用 splitOnCaseChange='0' 关闭——**驼峰切分在 Lucene/Solr 系是标准可选项** | 访问 2026-10-27 |
| 7 | simple 扩展 https://github.com/wangfenjin/simple | "simple 是一个支持中文和拼音的 sqlite3 fts5 拓展"；实现微信移动端全文检索"方案四"；加载实测：扩展 <2ms、拼音首用 ~500ms、jieba 词典首载 ~4s；**许可证更正：MIT OR GPL-3.0-or-later 双许可（R1 记为仅 MIT）** | 访问 2026-10-27 |
| 8 | Zoekt 代码搜索引擎 https://github.com/sourcegraph/zoekt | "based on trigram indexing and syntactic parsing"；ctags 符号信息只做排序信号——代码标识符检索业界走 trigram/子串路线，不做词切分 | 访问 2026-10-27 |
| 9 | ES SmartCN 插件 https://www.elastic.co/guide/en/elasticsearch/plugins/current/analysis-smartcn.html | 中文按概率做词级分词，但非内置、需逐节点手装插件——词级分词路线在服务端引擎里也只是可选项 | 访问 2026-10-27 |

### 与旧结论对表（R1 §2）
- **一致（加固）**：trigram <3 字符失效、unicode61 中文整串、MySQL bigram 默认、simple/Zoekt 不切驼峰、LIKE 线性扫——逐条复核通过。
- **分歧（措辞级，工程结论不变）**：R1 §2 结论第 3 行"代码标识符驼峰在**所有**先例里都不切分"不成立——Solr WordDelimiterGraphFilter 是官方标准切分先例（证据 6）。**裁决：仍不切分**——FTS5 生态没有现成的该类过滤器（需自写 C 扩展），我方已有原文列 LIKE/子串兜底且冻结集召回实测达标；但 R1 表述应改为"SQLite/代码搜索先例不切分；Lucene/Solr 系有可选切分先例，未引入"。
- **旧版漏了**：pg_bigm 与 Lucene CJKAnalyzer 两大 bigram 先例（bigram 从"MySQL 一家内置"升级为"三大生态成熟做法"）；simple 双许可证更正；"FTS5 之外的替代"复核：嵌入式单 exe 场景未查到比 FTS5 更合适的方案（pg_bigm/ES/SmartCN 均为服务端组件，不可嵌入）。

## 3. 记忆变更"提案/人工审核"先例（对照提案卡片）

### 结论（3 行内）
1. 全自动主线确认：mem0 论文（LLM 自定 ADD/UPDATE/DELETE/NOOP，无审批步）、Zep 论文（LLM 自动失效冲突边）、Memobase（异步自动画像）、Cognee（自动管线）均无事前人审。
2. **R1"卡片形态无同构先例"需修正**：Letta 现行 Dreaming 功能就是"自动生成提案 + 二段复核后应用"，只是复核者是另一个 agent——"Agent reviews before applying... does not ask you for approval"；**人工审核**的提案卡片仍未查到先例（ChatGPT 也只是事后管理）。
3. **新信号：mem0 于 2026-04 弃用自动破坏性更新**——新算法 "Single-pass ADD-only extraction -- no UPDATE/DELETE. Memories accumulate; nothing is overwritten"，以检索期排序代替覆写。业界在远离"LLM 自动删改"，与我方"变更走人审"同向。

（HITL = human-in-the-loop，人工审核环节；bi-temporal 双时间线 = 每条事实同时记录"业务生效时间"与"入库时间"，旧值不删、只标记失效，可回溯任意过去时点。）

### 证据表
| # | 来源（URL） | 关键原文/事实 | 日期 |
|---|---|---|---|
| 1 | mem0 论文 https://arxiv.org/html/2504.19413 | 四操作 ADD/UPDATE/DELETE/NOOP；"we leverage the LLM's reasoning capabilities to directly select the appropriate operation"；图记忆冲突 "marking them as invalid rather than physically removing them"——全自动、无审批步 | 论文 2025-04-28，访问 2026-10-27 |
| 2 | mem0 GitHub README（2026-04 新算法）https://github.com/mem0ai/mem0 | "Single-pass ADD-only extraction -- one LLM call, no UPDATE/DELETE. Memories accumulate; nothing is overwritten."；LoCoMo 92.5（较旧算法 +21 分）——**自家新算法放弃了自动 UPDATE/DELETE** | 访问 2026-10-27 |
| 3 | Zep 论文 https://arxiv.org/html/2501.13956v1 | 每条边带 valid_at/invalid_at；"The system employs an LLM to compare new edges against semantically related existing edges to identify potential contradictions"，重叠即自动置失效且 "consistently prioritizes new information"；双时间线四时间戳保全部历史、可查任意过去时点——**旧值不删只失效 = 撤销/回溯链的天然参照物** | 论文 2025-01-20，访问 2026-10-27 |
| 4 | Letta Dreaming（新增，结构上最接近的先例）https://docs.letta.com/guides/agents/architectures/sleeptime | "background subagents to review recent conversations, consolidate useful lessons, and update memory"；可开 "Agent reviews before applying"："review and revise proposed memory updates in a second background conversation"，且明说 "does not ask you for approval"；无 diff 展示、无历史/撤销、无人审选项 | 访问 2026-10-27 |
| 5 | Letta 逐工具审批 https://docs.letta.com/v1-sdk/tools/human-in-the-loop | 受闸工具返回 stop_reason "requires_approval"，客户端回 `{"type":"approval","approve":true/false,"reason":...}`；"Human-in-the-loop applies to server tools only"；按工具粒度配置，记忆工具未默认受闸（与 R1 一致） | 访问 2026-10-27 |
| 6 | Memobase（新增覆盖）https://github.com/memodb-io/memobase | "User Profile-Based Long-Term Memory for AI Chatbot Applications"；blob 异步缓冲自动刷出画像，无任何人工审核；人工仅事后 delete_user / 删单条 blob；2,938 stars、Apache-2.0、默认分支 2026-01 有推送 | 访问 2026-10-27 |
| 7 | Cognee https://github.com/topoteretes/cognee | 自动图谱管线（remember/recall/improve/forget）；仅模糊提到 "session distillation curates accepted lessons into permanent memory"，无明确人审闸（与 R1 一致）；v1.6.1（2026-09-24）活跃 | 访问 2026-10-27 |
| 8 | ChatGPT 记忆 FAQ https://help.openai.com/en/articles/8590148-memory-faq | 自动保存不逐条询问；事后管理：memory summary 删单条、"Delete and turn off memory"、回答下方 Sources "correct or delete a saved memory"——事后可见可删，无事前审批（与 R1 一致） | 访问 2026-10-27 |
| 9 | sleep-time compute 论文 https://arxiv.org/abs/2504.13171 | Letta 系论文："allows models to 'think' offline about contexts before queries are presented"——后台整理记忆的学术依据 | 2025-04-17，访问 2026-10-27 |

### 与旧结论对表（R1 §3）
- **一致（加固）**：mem0 论文 / Zep / Cognee / ChatGPT / Letta 逐工具审批五条全部复核通过，关键引文吻合。
- **分歧（R1 需修正一处定位表述）**：R1 §3 结论第 2 行"最接近的人审机制是 Letta 逐工具审批"已不是全部图景——Letta 现行 Dreaming 提供了结构上更贴近的先例："自动提案 → 二段会话复核 → 应用"（证据 4），只是复核者是 agent 而非人。**裁决：人工审核提案卡片仍是差异化设计（检索范围内复核者均非人类），但 R1"无同构先例"应改为"有 AI 复核的同构先例、无人审先例"；diff 展示与撤销链仍无任何先例。**
- **旧版漏了**：① mem0 2026-04 转 ADD-only（业界反思"自动破坏性更新"，我方"删改走人审"或"只追加+检索期过滤"两条路线都获业界背书）；② Zep 双时间线细节（卡片撤销链可直接参照：旧值置 invalid_at、不物理删除）；③ Memobase 先例覆盖。

## 对拍板②与 P0 选型的最终建议（3 行）
1. **拍板②维持 Q8_0（26MB）**：8-bit 保留率 90.8%~100% 实证 + "4-bit 无任何公开实证"经双线复核仍为阴性；Q4_K_M 省 11MB 抵不过零数字风险。
2. **P0 维持双字（bigram）+ unicode61 + LIKE 兜底**：bigram 先例从 1 家扩到 3 大生态（MySQL / pg_bigm / Lucene），trigram 失效与 LIKE 全扫均为官方明文；标识符维持原文 + 子串兜底，不引入驼峰切分。
3. **提案卡片维持人审定位，吸收两个先例**：对外表述改为"业界已有 AI 复核提案（Letta Dreaming）、mem0 已弃自动删改转 ADD-only"；撤销链参照 Zep 双时间线（旧值置失效、不物理删除）。

## 旧文档是否需要改
需要小改 R1 三处（均为表述级，不动任何结论与设计）：① §2 结论第 3 行"所有先例都不切分"补 Solr WDGF 反例与裁决；② §3 结论第 2 行补 Letta Dreaming 先例，"无同构先例"改为"有 AI 复核先例、无人审先例"；③ §3 证据表 mem0 行补 2026-04 ADD-only 新算法、simple 行许可证更正为 MIT OR GPL-3.0+。§1 无需改。
