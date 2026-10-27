# memcpp 先例与实证调研（量化档位 / FTS5 中文 / 记忆人审先例）

- 检索日期：2026-10-27；工具：WebSearch / WebFetch / webReader / HuggingFace·GitHub 官方 API。
- 输入口径：`2026-10-27-memcpp-feasibility.md` §5/§8、`2026-10-27-memcpp-proposal-card-design.md` §1/§4。
- 纪律：术语首现配一句人话；查不到的写"未查到 + 已查来源"，不做推断填充。

## 1. bge-small-zh-v1.5 量化档位的召回代价（拍板②：Q8_0 26MB vs Q4_K_M 15MB）

### 结论（先读这 3 行）
1. 8-bit 档（与 Q8_0 同位宽）在公开基准上检索质量保留 90.8%~100%（不加重排），加重排约 99%——基本无损。
2. **未查到任何 embedding 模型 4-bit（Q4_K_M 属此类）检索质量的公开实证**；相邻证据（1-bit 二值量化不加重排掉 3.5%~25.2%）表明 4-bit 风险不可忽略。
3. 模型越小量化越伤：约 22M 参数的模型 8-bit 只保 90.79%，3 亿+参数级保 97%——bge-small-zh-v1.5 是 24M 级，落在敏感区间。

（量化 = 把模型权重从 16 位小数压成 8 位/4 位整数换体积速度；召回 Recall@5 = 检索前 5 条含正确答案的比例；MTEB = 公开的向量质量基准，C-MTEB 为其中文版；重排/rescore = 用原始精度向量对候选二次打分。）

### 证据表
| # | 来源（URL） | 关键数字/原文 | 日期 |
|---|---|---|---|
| 1 | 官方模型卡 https://huggingface.co/BAAI/bge-small-zh-v1.5 | 24M 参数、512 维；C-MTEB Avg 57.82 / Retrieval 61.77；MIT | 访问 2026-10-27 |
| 2 | GGUF 文件清单（HF API blobs）https://huggingface.co/CompendiumLabs/bge-small-zh-v1.5-gguf | f32 95,211,424 B / f16 47,886,240 B / **q8_0 26,472,640 B** / **q4_k_m 15,448,256 B**。我方 B2 探针实测模型恰为 26,472,640 B = **q8_0 档**（见 commit 6a8f8e0c README：exe 10,423,028 + 模型 26,472,640 = 35.19 MiB） | 访问 2026-10-27 |
| 3 | HF 官方博客（mixedbread 量化研究）https://huggingface.co/blog/embedding-quantization | "We conducted our experiments on the retrieval subset of the MTEB containing 15 benchmarks"。int8 不加重排保留率：mxbai-embed-large-v1 97%、e5-base-v2 94.68%、**all-MiniLM-L6-v2（约 22M，bge-small 同级）90.79%**、Cohere-v3 100%；"A rescore multiplier of 4-5 already leads to a remarkable performance retention of 99% using int8"；二值不加重排：mxbai 92.53%、e5-base 仅 74.77%；二值+重排 74.77%~96.45% | 访问 2026-10-27 |
| 4 | sentence-transformers 官方文档 https://sbert.net/examples/sentence_transformer/applications/embedding-quantization/README.html | "preserve up to ~96% of the total retrieval performance, while reducing the memory and disk space usage by 32x"（二值+重排） | 访问 2026-10-27 |
| 5 | **未查到项** | "GGUF 4-bit（Q4_K_M）embedding 检索质量公开基准"未查到。已查：GitHub issue 检索 API（repo:ggml-org/llama.cpp embedding+quantization，命中为功能/崩溃类，无质量评测）、unsloth/bge-small-en-v1.5-GGUF 卡（无量化质量说明）、多次 WebSearch（限流，无直接命中） | 2026-10-27 |

外推声明：证据 3 无 24M 级中文模型，all-MiniLM-L6-v2（约 22M、英文）是最近邻规模，其 90.79% 视为同规模外推；4-bit 结论由 1-bit/8-bit 区间夹逼，非直接实证。

### 对拍板②的建议
- **不改：维持 Q8_0（26MB）**。Q8_0 位宽有 90.8%~100% 保留实证；B2 探针 35.19 MiB 全绿链路就是 Q8_0，换 Q4_K_M 省 11 MB（→约 24.2 MiB）但质量零实证、且同级小模型 8-bit 都只保 90.79%，风险收益不成比例。
- 若未来必须省 11 MB：先在我方冻结检索集上实测 Q4_K_M 的 Recall@5 与 f16/q8_0 差值（复用 feasibility §5 门槛 2 测法），有数字再切。

## 2. SQLite FTS5 中文检索选型（P0：双字分词 + unicode61 + LIKE 兜底）

### 结论（先读这 3 行）
1. 我方 trigram 实测（2 字中文 0 命中）被官方文档原文证实：少于 3 个 Unicode 字符的子串在全文本查询下 "do not match any rows"。
2. **双字（bigram）是业界内置默认**：MySQL 为 CJK 内置 ngram 全文解析器且 "default ngram token size of 2"；同方向还有 858 星 simple 扩展（中文逐字 + 拼音）。
3. jieba（中文分词词典）路线无质量基准数字（simple 自己只有 TODO"给出性能数据"），且有词典首载 ~4 秒实测开销；代码标识符驼峰在所有先例里**都不切分**，靠子串匹配兜底。

（FTS5 = SQLite 内置全文检索引擎；tokenizer/切词器 = 把文本切成检索单元的组件；unicode61 = SQLite 内置的按 Unicode 类别切词的切词器；trigram/bigram = 按 3 字/2 字滑动窗口切块；LIKE = SQL 模糊匹配；jieba = 流行中文分词词典。）

### 证据表
| # | 来源（URL） | 关键数字/原文 | 日期 |
|---|---|---|---|
| 1 | SQLite 官方 FTS5 文档 https://www.sqlite.org/fts5.html | trigram："Substrings consisting of fewer than 3 unicode characters do not match any rows when used with a full-text query."；trigram 表的 LIKE/GLOB 走索引，但 "If a LIKE or GLOB pattern does not contain at least one sequence of non-wildcard unicode characters, FTS5 falls back to a linear scan of the entire table."；unicode61：Unicode 类别 L/N/Co 为 token 字符，"Each contiguous run of one or more token characters is considered to be a token"（中文连续串成一整 token，证实必须外部预分词）；自定义切词器须实现 xCreate/xDelete/xTokenize 三函数注册 | 访问 2026-10-27 |
| 2 | MySQL 8.0 官方手册 https://dev.mysql.com/doc/refman/8.0/en/fulltext-search-ngram.html | "MySQL provides an ngram full-text parser that supports Chinese, Japanese, and Korean (CJK)"；"The ngram parser has a default ngram token size of 2 (bigram)." | 现行手册，访问 2026-10-27 |
| 3 | simple 扩展 https://github.com/wangfenjin/simple | 中文+拼音 FTS5 tokenizer（858 stars，MIT）；实现"微信移动端全文检索多音字方案四"；加载实测：扩展 <2ms、拼音首用 ~500ms、jieba 词典首载 ~4s；**无与 unicode61/trigram 的质量对比**（README 自列 TODO"给出性能数据"） | 访问 2026-10-27 |
| 4 | simple 切词器源码 https://github.com/wangfenjin/simple/blob/master/src/simple_tokenizer.cc | ASCII 按字符类别（字母/数字/空白/其他）切换切词，无大小写边界逻辑——"camelCase like 'fooBar' stays as one token (then gets lowercased)"，**驼峰不切分**；CJK 逐字成单字 token；USE_JIEBA 编译时换 cppjieba | 访问 2026-10-27 |
| 5 | Zoekt 代码搜索引擎 https://github.com/sourcegraph/zoekt | "Fast trigram based code search"；"supports fast substring and regexp matching on source code"；ctags 符号信息只用于排序——**代码标识符检索业界走 trigram/子串路径，不做分词** | 访问 2026-10-27 |

方案对比：双字预分词+unicode61（我方 P0）＝质量有小样本实证（冻结集 Recall@5 1.00/MRR 0.9667）+MySQL 内置同款、复杂度低（纯入库预处理）、索引中；trigram＝零复杂度但官方明文 <3 字符失效、索引大；jieba 自定义切词器＝质量无公开数字、C 扩展+词典资产、加载 ~4s；LIKE 兜底＝零成本、官方确认走线性扫描（几千到几万条可接受）。

### 对 P0 的建议
- **不改：双字预分词 + unicode61 + LIKE 兜底**。仅升级表述：trigram 缺陷由"我方实测"升级为"官方文档明文+实测双重证实"。
- **不加 jieba**：无质量收益证据 + ~4s 词典加载 + 词典资产分发，与"单二进制零依赖"冲突。
- **标识符路线不改**：驼峰不拆（simple 同款），`getUserById` 靠原文列 LIKE/子串兜底（Zoekt 先例：标识符用子串不做词切分）；若后续实测召回不足，再评估 ASCII 前缀查询，仍不做驼峰切分。

## 3. 记忆变更"提案/人工审核"先例（对照 B4 卡片）

### 结论（先读这 3 行）
1. 四个活跃方案（mem0 / Zep / Letta / Cognee）的记忆合并/失效/遗忘**全部全自动，无人工审核环节**。
2. 最接近的人审机制是 Letta 的**逐工具审批**（API 级 approve/deny），可选配置、示例闸的是写库类敏感操作，记忆工具默认不受闸；另有 ChatGPT 的**事后管理 UI**（设置页查看/删除 + 回答下方来源标注）。
3. "事前审批队列 + 差异对比 + 批量 + 撤销"的卡片形态在检索范围内**无同构先例，属差异化设计**；自动/人工分流阈值无业界经验数字。

（HITL = human-in-the-loop，人工审核环节；diff = 差异对比视图。）

### 证据表
| # | 来源（URL） | 关键原文/事实 | 日期 |
|---|---|---|---|
| 1 | mem0 论文 https://arxiv.org/abs/2504.19413（全文 https://arxiv.org/html/2504.19413v1） | 四操作 ADD/UPDATE/DELETE/NOOP；"The LLM itself determines which of four distinct operations to execute"；"Rather than using a separate classifier, we leverage the LLM's reasoning capabilities"；Algorithm 1 无审批步——全自动 | 2025-04，访问 2026-10-27 |
| 2 | Zep/Graphiti README https://github.com/getzep/graphiti | "Automatic fact invalidation with temporal history preserved"；"New data integrates immediately without batch recomputation"——冲突事实自动失效，无审核 | 访问 2026-10-27 |
| 3 | Letta sleep-time compute https://www.letta.com/blog/sleep-time-compute | 后台 agent "using their downtime to reorganize information"、自动改写记忆块；全文无人工审核——全自动 | 访问 2026-10-27 |
| 4 | Letta 逐工具审批 https://docs.letta.com/v1-sdk/tools/human-in-the-loop | 受闸工具调用返回 stop_reason "requires_approval"，客户端发 `{"type":"approval","approve":true/false}` 恢复；配置粒度=单工具（API 页 https://docs.letta.com/api/resources/agents/methods/update 的工具规则对象，搜索摘录措辞 "tool rule configuration which requires approval before the tool can be invoked"，页面较长未逐字复核）；示例闸 database_write，**未提记忆工具默认受闸** | 访问 2026-10-27 |
| 5 | ChatGPT 记忆 FAQ https://help.openai.com/en/articles/8590148-memory-faq | 自动保存："details that you explicitly ask ChatGPT to remember or that ChatGPT saves as useful context"；事后管理：Settings→Personalization→Memory、"Memory summary"、回答下方 Sources 可 "correct or delete a saved memory"——**事后可见，无事前审批** | 访问 2026-10-27 |
| 6 | Cognee README https://github.com/topoteretes/cognee | "With no LLM key configured, Cognee extracts the graph with the local GLiNER model..."——自动管线；仅模糊提到 "Session distillation curates accepted lessons into permanent memory"，无明确人审闸 | 访问 2026-10-27 |

### 对 B4 卡片的建议
- **不改状态机与分流**：pending/accepted/rejected/expired 与"隔离区来源只走卡片"无业界证据可推翻；半自动档是差异化价值（mem0 论文自认 LLM 判定可能出错，人审是直接回应）。
- **改一处定位表述**：对外宣称写"业界以全自动为主（mem0/Zep/Letta/Cognee）；Letta 有逐工具审批 API 但无卡片/diff UI 公开描述"——差异化成立，但不是业界惯例。
- **阈值维持"待标定"**：未检索到任何方案公开"置信度阈值自动 vs 人工"经验数字；B4 高线 0.8 只能靠我方 §5 门槛 1/4 抽检流程标定。
- **可吸收的先例**：ChatGPT "来源标注 + 事后纠正" ↔ 我方来源链 + 按来源批量撤回（已设计，无需新增功能）。

## 本轮核实后需要修订的既有文档清单
1. `docs/superpowers/research/2026-10-27-memcpp-feasibility.md` §8.2：把"量化后体积在几十 MB 量级（推断，需选型确认）"落成实数——q8_0=26,472,640 B（B2 实测即此档）、q4_k_m=15,448,256 B（HF API 文件清单）；拍板②选项直接引用本文 §1。
2. `docs/superpowers/research/2026-10-27-memcpp-feasibility.md` §4.6：trigram 证据栏补官方文档原文（"fewer than 3 unicode characters do not match any rows"），结论由"实测推断"升级为"官方文档+实测"。
3. `docs/superpowers/plans/2026-10-27-memcpp-proposal-card-design.md` §0/§4（可选）：补一句先例定位（全自动为主、Letta 逐工具审批 API、ChatGPT 事后管理），防止"半自动档是常见做法"的误读；不改变任何设计决定。
