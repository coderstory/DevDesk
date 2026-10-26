# memcpp 可借开源源码清单

日期：2026-10-26
用途：为 memcpp（DevDesk 记忆系统）评估可直接移植或作为设计蓝本的现成实现。
范围：只做源码调研，不改 DevDesk 任何源码；参考仓库克隆在 `D:/software/src/oss-ref/<repo>`（仓库外，不进 git）。

采集方式：`git clone --depth 1`（generative_agents 额外用 `--filter=blob:none`），代理 `socks5h://127.0.0.1:10808`。下列 sha 均为 `git rev-parse HEAD` 实测值。

| 仓库 | commit sha | 许可证 | 本地参考目录 |
|---|---|---|---|
| thedotmack/claude-mem | `7325fb7818ff7426665d85922db642ec0931bdcd` | Apache-2.0（含 NOTICE） | `D:/software/src/oss-ref/claude-mem` |
| mem0ai/mem0 | `2c81af3bbe27d9bccb09c772d583df9e9be72ee5` | Apache-2.0 | `D:/software/src/oss-ref/mem0` |
| getzep/graphiti | `5c27edfa8d3e0f49b2571bcebb4261bcc89ee037` | Apache-2.0 | `D:/software/src/oss-ref/graphiti` |
| joonspk-research/generative_agents | `fe05a71d3e4ed7d10bf68aa4eda6dd995ec070f4` | Apache-2.0 | `D:/software/src/oss-ref/generative_agents` |
| unum-cloud/usearch | `a985f77553d2d58d5ef1516eafaae81480bb3c30` | Apache-2.0 | `D:/software/src/oss-ref/usearch` |
| ggml-org/llama.cpp | `c6fb3b5be2209f6c12339833198190f6f6829581` | MIT | `D:/software/src/oss-ref/llama.cpp` |

说明：`generative_agents` 仓库有 21 万个检出文件（主要是 `environment/frontend_server` 资源），本次用 `git show HEAD:<path>` 直接读对象，工作区未完成检出；sha 仍以 HEAD 为准。其余仓库工作区完整。

---

## 一页总表

| 能力 | 来源（仓库@sha） | 关键文件 | 可借内容一句话 | 许可证 |
|---|---|---|---|---|
| 近重复检测（两档） | claude-mem@`7325fb7` | `src/services/dedup/normalize.ts`、`idf.ts`、`tfidfCosine.ts`、`idfVeto.ts`、`nearDuplicate.ts`、`src/services/sqlite/dedup-store.ts` | 精确归一标题直接合并；IDF 加权余弦 + 稀有词否决只产“候选”，不静默合并 | Apache-2.0 |
| 观测抽取提示词/XML 协议 | claude-mem@`7325fb7` | `src/sdk/prompts.ts`、`plugin/modes/code.json` | `<observation>` 字段协议、`<skip_summary>` 哨兵、格式错重试一次、指令与用户请求拆成 system/user | Apache-2.0 |
| 周期性归纳流水线 | claude-mem@`7325fb7` | `plugin/skills/weekly-digests/SKILL.md` | 按 ISO 周切文件，每周一个串行子代理，携带上一周 carry-forward 块 | Apache-2.0 |
| 记忆条目字段模型 | claude-mem@`7325fb7` | `src/core/schemas/memory-item.ts` | kind/type/title/subtitle/text/narrative/facts/concepts/files/metadata + 来源表 | Apache-2.0 |
| Hook 失败开放与落盘队列 | claude-mem@`7325fb7` | `src/shared/hook-io.ts`、`hook-spool.ts`、`hook-constants.ts` | 只允许 exit 0；stdout/stderr/exit 单点出口；一事件一文件原子落盘，worker 顺序消费 | Apache-2.0 |
| 抽取 + 只增 + 链接 | mem0@`2c81af3` | `mem0/configs/prompts.py`、`mem0/memory/main.py` | 现有记忆映射为整数 idx 防幻觉；单次 JSON 抽取；批嵌入；MD5 哈希去重；`linked_memory_ids` 关联 | Apache-2.0 |
| 四操作 ADD/UPDATE/DELETE/NONE | mem0@`2c81af3` | `mem0/configs/prompts.py:176`、`:406` | 经典“对比旧记忆决定四种操作”提示词；当前 HEAD 主链路未接线，只作参考 | Apache-2.0 |
| 事实时序失效/矛盾处理 | graphiti@`5c27edf` | `graphiti_core/prompts/extract_edges.py`、`prompts/dedupe_edges.py`、`utils/maintenance/edge_operations.py` | 抽 `valid_at`/`invalid_at`；保守矛盾判定（只认显式终止/替换）；按日期比较置 `invalid_at`/`expired_at` | Apache-2.0 |
| 反思触发与记忆簇 | generative_agents@`fe05a71` | `persona/cognitive_modules/reflect.py`、`retrieve.py`、`memory_structures/scratch.py`、`prompt_template/v2/*.txt` | 重要性累计到 150 触发；3 个 focal points；每点检索 30 条（时近/重要/相关各权重 1）；生成 5 条带证据的 insight | Apache-2.0 |
| C++ 向量索引 | usearch@`a985f77` | `include/usearch/index.hpp`、`index_dense.hpp`、`index_plugins.hpp`、`cpp/README.md` | header-only HNSW：`add/search/save/load/view`，无异常风格返回 result 对象 | Apache-2.0 |
| 本地 embedding | llama.cpp@`c6fb3b5` | `examples/embedding/embedding.cpp`、`include/llama.h` | GGUF 加载、批量编码、按序列池化取向量、L2 归一化 | MIT |

---

## 1. claude-mem（Apache-2.0）

### 1.1 近重复检测（dream 去重直接可移植）

关键文件与入口：

- `src/services/dedup/normalize.ts:14` `normalizeTitle`：NFC 归一、小写、只保留 Unicode 字母/数字/组合符，其余标点空白折叠为单空格；结果为纯标点/emoji 时返回空串。
- `src/services/dedup/normalize.ts:26` `tokenizeWs`：**只按空白切分**并小写，保留 `rdlp-api`、`ffmpeg-7.1.conf` 这类复合标识为单 token。
- `src/services/dedup/idf.ts:17`：平滑 IDF `log(1 + N/(df+0.5))`；`:22` 由 df 查询函数和语料总量构造 `token -> idf`。
- `src/services/dedup/tfidfCosine.ts:20`：短标题按词袋、tf=1、IDF 加权后算余弦；注释明确说明为什么不用 Levenshtein/排序 token（稀有区分词会把相似度错误拉高）。
- `src/services/dedup/idfVeto.ts:18`：对称差里任一 token 的 `idf > theta` 即否决合并（Fellegi-Sunter 的 blocking key 思想）。
- `src/services/dedup/nearDuplicate.ts:49` `classifyPair`：两档判定
  - Tier-0 `exact`：归一标题非空且相等，直接静默合并；
  - Tier-1 `candidate`：共享 token 数 >= `minSharedTokens`（默认 2）且 IDF 余弦 >= 阈值、且否决未触发，只产候选，**从不静默合并**；
  - 其余 `none`。
- `src/services/sqlite/dedup-store.ts`：`token_df(project, token, df)` 文档频率表 + `dedup_meta.doc_count` 语料量，每插入一条真实观测就前向 +1；`observation_dedup_candidates.status` 取 `pending/merged/distinct/dismissed`；`title_norm_key` 在应用层预计算后建索引，避免 SQLite 无法做 Unicode 归一的问题。`:95-105` 说明 `idfVetoDf` 与该严格大于阈值的换算关系。

源码注释里的实测结论（`nearDuplicate.ts` 顶部）：在 7,651 条真实观测库上验证，narrative 的 simhash 假阳性严重，最终采用“IDF 余弦 + 稀有词否决”。

移植注意：

1. **中文/日文适配是首要问题**：`tokenizeWs` 按空白切分只适合英文。中文必须换成词级分词（如 cppjieba）或字符二元组，否则整段中文成为一个 token，IDF/余弦全部失真。`normalizeTitle` 的 Unicode 归一与 `\p{L}\p{N}\p{M}` 分类，C++ 标准库没有，需要 ICU 或 utf8proc；需先确认 DevDesk/EUI-NEO 是否已有等价工具（**未核对**）。
2. JS `Map`/`Set` → `std::unordered_map`/`std::unordered_set`；`number` 用 `double`。
3. 文档频率表必须与观测插入同事务维护；并发写需要锁或串行化。Tier-0 合并不增加文档，不要再 +df（源码注释明确）。
4. 阈值没有内置默认值：`ClassifyThresholds.cosineThreshold`、`vetoThetaIdf` 由 store/设置层解析。移植时不要照抄一个数字就用，需要在 memcpp 自己的语料上标定。
5. 归一后空串必须返回“不可比较”，禁止多个空标题互相精确合并（源码有显式防数据丢失注释）。

确认程度：算法五个文件全部读完（共 195 行）；`dedup-store.ts` 只读了头部与关键 SQL/阈值段，完整建表 DDL 未逐行核对。

### 1.2 观测抽取提示词与 XML 协议

关键文件：

- `src/sdk/prompts.ts:373` `buildObservationPrompt`、`:394` `buildObservationPromptParts`、`:430` `renderObservationPrompt`、`:450` `buildSummaryPrompt`、`:485` `buildContinuationPrompt`、`:518` `splitFramingPrompt`。
- `plugin/modes/code.json` 的 `prompts` 字段：`observer_role`、`recording_focus`、`skip_guidance`、`type_guidance`、`concept_guidance`、`field_guidance`、`output_format_header`、`footer` 以及各 XML 占位符。`observation_types` 为 `bugfix/feature/refactor/change/discovery/decision/security_alert/security_note/sensitive`。

可借要点：

- 观测输出协议是固定 XML：每块含 `<type> <title> <subtitle> <facts><fact>… <narrative> <concepts><concept>… <files_read> <files_modified>`。用标签而不是自由 JSON，便于流式与容错解析。
- 无内容时必须返回语义哨兵 `<skip_summary reason="noise" />`；明确禁止空回复、禁止 “Skipping” 之类散文。
- 若模型用错标签，追加一次 `OBSERVATION_SCHEMA_REMINDER` 重问；仍错则丢弃（`:447-448`）。
- 会话摘要使用独立根标签 `<summary>`，并反复强调“不得用 `<observation>` 作根标签，否则丢弃”，避免两种任务串协议。
- `splitFramingPrompt`（`:518`）把同一段提示拆成 system（指令/格式）与首条 user（用户请求）；注释说明小模型在长对话里保持格式更稳。这对本地小模型尤其值得借。
- 被截断字段会插 `<elided chars=… />`，并明确要求模型“只描述可见部分，不要推断被省略范围”。

移植注意：XML 解析需要能容忍模型输出杂质；提示词里所有占位符建议集中在资源文件（如 `resources/prompts/*.json`），不要在 C++ 里拼接长字符串。C++ 端要把“是否重问一次”做成显式状态，不要无限重试。

确认程度：`prompts.ts` 527 行只完整读了协议构建相关段（约 1-60、373-527）；`code.json` 只抽取了键名、长度、类型列表和 `output_format_header`，各提示词全文未逐字核对。

### 1.3 周期性归纳（weekly-digests）

文件：`plugin/skills/weekly-digests/SKILL.md`。

可借的是流水线设计，不是代码：

1. 拉取全量时间线，按日期头解析；
2. 用 `date.isocalendar()` 按 ISO 周（周一起）切成一週一文件，原文逐字保留，不做改写；
3. 双遍 sanity check：分配出去的观测总数必须等于源文件计数；
4. 每周一个串行 subagent（**不能并行**），每个 agent 必须接收上一周的 `STORY_SO_FAR` carry-forward 块，才能形成连贯的多章叙事；
5. 周数由数据决定，不写死章节数；空周跳过。

对 memcpp 的映射：dream 的“周归纳”可以先按时间桶切分原始记忆，再串行地把上一桶的摘要作为下一桶的上下文；最后做一次计数对账，防止漏条目。

确认程度：SKILL.md 只读了前约 120 行（步骤 1-5 的主体），后续输出格式细节未逐行核对。

### 1.4 记忆条目字段模型

文件：`src/core/schemas/memory-item.ts:8`。

字段：`id / projectId / serverSessionId / legacyObservationId / kind(observation|summary|prompt|manual) / type / title / subtitle / text / narrative / facts[] / concepts[] / filesRead[] / filesModified[] / metadata / createdAtEpoch / updatedAtEpoch`；另有 `MemorySource`（`sourceType`、`sourceUri`、`legacyTable/legacyId`）记录来源。

可借点：把“原始观测、会话摘要、用户提示、手工条目”统一成一种条目类型，用 `kind` 区分；`facts` 与 `narrative` 分开存，前者可结构化检索，后者供人读；来源单独一张表，便于回溯与去重。`UpdateMemoryItemSchema` 特意 `.removeDefault()`，用“缺省即保留原值”表达 PATCH 语义，这个细节值得照搬。

移植注意：时间用 epoch 数值（此处为 epoch 秒/毫秒量级，字段名 `Epoch`；移植时统一毫秒并注明）。C++ 用 `int64_t`，不要用 `double` 存时间戳。

### 1.5 Hook 失败开放与落盘队列

文件：

- `src/shared/hook-io.ts`：整个 hook 执行路径**唯一**允许调用 `process.stdout.write/stderr.write/exit` 的地方；意图分为 `DIAGNOSTIC`（stderr）、`MODEL_CONTEXT`（stdout）、`USER_HINT`、`EXIT_SIGNAL`。`:179 exitGraceful` 最终 `process.exit(0)`。
- `src/shared/hook-constants.ts`：`HOOK_EXIT_CODES` 只有 `SUCCESS: 0`，注释明确“Claude Code 把 exit 2 当 block，claude-mem 是可选后台服务，任何失败都不能阻塞用户”；另有分级超时预算（健康检查 3s、API 30s、会话初始化 10s 等，Windows 乘 1.5）。
- `src/shared/hook-spool.ts`：一事件一文件，文件名即幂等键（session+platform+tool_use_id 或内容哈希）；原子写；worker 在启动、hook nudge、目录 watch、慢速兜底扫描时按 `enqueuedAtEpochMs` 顺序 drain。避免多进程并发写同一 append-only 文件（Windows 上尤其容易交错）。

对 memcpp 的映射：DevDesk 不是 Claude Code hook，但“采集端只落盘、立即返回；消费端异步 drain；失败绝不阻塞主流程；事件幂等键去重”这套模式可直接用于桌面事件采集与 dream 后台任务。

移植注意：Windows 原子替换用 `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)`；文件名哈希注意大小写与非法字符；drain 顺序字段必须持久化在文件体内，不能依赖文件系统时间。

确认程度：`hook-io.ts` 读头部与 `exitGraceful`；`hook-constants.ts` 读前 60 行；`hook-spool.ts` 读头部设计注释。drain 实现细节未逐行核对。

---

## 2. mem0（Apache-2.0）

### 2.1 当前主链路：ADD-only + 链接（V3）

文件：`mem0/memory/main.py:881 _add_to_vector_store`，阶段注释在 `:925 / :942 / :993 / :1007 / :1047`。

流程（以同步版为准，异步版 `:2556` 同构）：

1. Phase 0/1：取最近 10 条消息；用查询嵌入从向量库取 top 10 现有记忆。
2. **反幻觉映射**：把现有记忆的 UUID 映射成连续整数 `"0".."n"`，提示词里只出现整数 id（`uuid_mapping`），LLM 不可能编造出真实 UUID。
3. Phase 2：单次 LLM 调用，system 用 `ADDITIVE_EXTRACTION_PROMPT`（`:468`），user 用 `generate_additive_extraction_prompt`（`:1016`）拼装：Summary、Last k Messages、Recently Extracted Memories、Existing Memories、New Messages、Observation Date、Current Date、Custom Instructions。
4. Phase 3：对所有抽取文本批量嵌入，失败再逐条兜底。
5. Phase 4/5：每条文本算 MD5，与现有 `payload.hash` 及本批 `seen_hashes` 比对，重复直接跳过。
6. Phase 6：批量插入向量库；只把真正持久化成功的记录写历史/返回；全失败抛 `VectorStoreError`，不再静默返回空数组。

`ADDITIVE_EXTRACTION_PROMPT` 的关键规则（中文概括）：

- 只做 ADD，不做 UPDATE/DELETE；用户与助手消息都要抽。
- 每条记忆必须自包含、上下文丰富；相对时间必须锚定到 `Observation Date`（例如“上周”要落成具体日期），否则半年后无意义。
- `Recently Extracted Memories` 是首要去重参照；`Existing Memories` 只用于去重与链接，禁止从中再抽取。
- 与现有记忆同主题/同实体/偏好变化/后续事件时，把现有记忆 UUID 放进 `linked_memory_ids`；新记忆自己的 id 仍是顺序整数。
- 实体出现过不代表其所有信息都已捕获：同一只狗的新事件必须另抽一条并链接。
- 当前日期只作系统信息，不能用来解释消息里的相对时间（必须用 Observation Date）。

### 2.2 经典四操作提示词（参考，不是当前主链路）

- `mem0/configs/prompts.py:176 DEFAULT_UPDATE_MEMORY_PROMPT`：ADD / UPDATE / DELETE / NONE 四操作，带大量示例；UPDATE 要求保留原 id；DELETE 用于“新事实与旧记忆矛盾”；NONE 用于已存在或无关。
- `:406 get_update_memory_messages` 负责把旧记忆与新抽取事实拼成该提示词的用户消息。
- 事实抽取提示词：`:15 FACT_RETRIEVAL_PROMPT`（通用用户记忆）、`:63 USER_MEMORY_EXTRACTION_PROMPT`、`:124 AGENT_MEMORY_EXTRACTION_PROMPT`。

重要事实：在本 commit 中 `get_update_memory_messages` **只有定义、仓库内未发现调用点**；同步/异步 `add` 主链路都走 ADD-only V3 提示词。也就是说，若 memcpp 想要“更新/删除/失效”，不能声称 mem0 当前版本已提供该流程，只能把四操作提示词当作设计参考自行接线。

移植注意：

1. Python 的 `json.loads` 容错（`remove_code_blocks`、`extract_json`）要移植；本地小模型经常带 markdown 围栏。
2. MD5 只用于内容去重，不是安全用途；C++ 可用项目已有 OpenSSL，但建议统一 SHA-256，并注意换算法后旧数据要迁移哈希。
3. `lemmatize_for_bm25` 是英文词形还原；中文需要自己的分词/归一，不能直接搬。
4. 批量嵌入失败要有逐条兜底，持久化失败必须回滚“已报告成功”的记录；这两点是 mem0 源码里明确修过的坑。
5. `linked_memory_ids` 与实体表更新在 `main.py:624-693、1171-1189` 有增量维护逻辑（删除记忆时反向清理链接），移植时不要只做正向链接。

确认程度：`main.py` 的 V3 同步链路 881-1080 行读完；`prompts.py` 只读了 468-560、1016-1062 以及四操作提示词 176-330；抽取提示词全文（约 470 行）未逐行核对。

---

## 3. graphiti（Apache-2.0）

### 3.1 事实抽取与时间边界

文件：`graphiti_core/prompts/extract_edges.py:26 class Edge`、`:95 edge()`、`:179 # DATETIME RULES`。

`Edge` 的字段：`source_entity_name`、`target_entity_name`、`relation_type`（SCREAMING_SNAKE_CASE）、`fact`、`valid_at`、`invalid_at`、`episode_indices`。

提示词规则（中文概括）：

- 只抽 ENTITIES 列表里两个不同实体之间的关系；实体名必须来自列表，否则边会被拒绝。
- 保留全部具体细节：专有名词、品牌、型号、数量、颜色、地点；禁止把 “Gamecube” 泛化成 “gaming console”。
- 后续事件若给旧事实补充了更具体的属性（品牌/数量/颜色/地点），算**新事实**，不是重复；只有同等具体度才算重复。
- `valid_at`：事实开始为真的时间；将来时的计划/承诺用“陈述参考时间”，不是未来发生日期。
- `invalid_at`：事实停止为真的时间；软截止（“到 2026 年 3 月前”）不是 `invalid_at`；只有“持续状态在该日结束”才用未来日期。
- 相对时间用 `REFERENCE_TIME` / 每条 episode 自己的时间戳解析；无明确时间就留 null。

### 3.2 去重与矛盾判定

文件：`graphiti_core/prompts/dedupe_edges.py:25 EdgeDuplicate`、`:44 resolve_edge()`。

- 输出两个整数列表：`duplicate_facts`（只允许来自 EXISTING FACTS 的 idx）与 `contradicted_facts`（可来自两个列表）。
- 两个列表的 idx **连续编号**：EXISTING FACTS 在前，FACT INVALIDATION CANDIDATES 从其后继续。提示词反复强调这一点，防止模型错位。
- 矛盾判定非常保守：必须有显式证据表明旧事实“停止为真/被替换/被终止/被转让”；不得仅因为“更新”“状态变化”标题、会议日期、episode 日期、或同一实体的新活动就判矛盾。
- 角色/归属/责任类事实，不因同一人做了新工作、发了新版本、做了汇报就被判失效。
- 同一断言只有“明确不兼容的变更”时才可以既是 duplicate 又是 contradicted。
- 反例：`Alice works at Acme as software engineer` vs `senior engineer` → 矛盾不是重复；`Bob ran 5 miles Tuesday` vs `3 miles Wednesday` → 既非重复也非矛盾。

### 3.3 时序失效逻辑

文件：`graphiti_core/utils/maintenance/edge_operations.py:715 _apply_temporal_invalidation`、`:748 resolve_edge_contradictions`。

算法（不依赖图数据库，可独立移植）：

1. 若新边自带 `invalid_at` 且尚无 `expired_at`，说明它一到达就是历史事实，直接 `expired_at = now`。
2. 若新边未过期，把候选边按 `valid_at` 升序排；若某候选的 `valid_at` 晚于新边的 `valid_at`，则新边被“更晚开始的候选”取代：`resolved.invalid_at = candidate.valid_at`，`resolved.expired_at = now`，停止。
3. 对每个候选做日期兼容性检查：
   - 候选已在“新边生效之前”失效，或新边在“候选生效之前”失效 → 两条时间线不重叠，不处理；
   - 候选的 `valid_at` 早于新边的 `valid_at` → 新边使候选失效：`candidate.invalid_at = resolved.valid_at`，`candidate.expired_at = now`，收集为 invalidated。
4. 只置失效时间，**不物理删除**，历史仍可追溯；最后为失效边补建嵌入。

移植注意：Python `datetime` 的 naive/aware 混用有专门的 `ensure_utc` 兜底；C++ 侧统一存 UTC epoch 毫秒，禁止本地时区。移植时保持“先按日期排序、再判矛盾”的顺序，顺序错了会把新边自己误标过期。`expired_at` 是“系统写入时间”，`invalid_at` 是“事实世界时间”，两个字段不要合并。

确认程度：提示词两文件相关段读完；`edge_operations.py` 只读 715-790 与 600-650 调用段，其余维护流程未逐行核对；未运行其测试。

---

## 4. generative_agents（Apache-2.0）

### 4.1 反思触发

文件：`persona/cognitive_modules/reflect.py:135 reflection_trigger`；参数在 `memory_structures/scratch.py:57-63`。

- `importance_trigger_max = 150`，`importance_trigger_curr` 初始等于 150；每产生一条重要记忆就减去该条的重要性分数（在 `memorize` 流程里扣减，本次未逐行核对调用点）。
- 触发条件：`importance_trigger_curr <= 0` 且记忆里至少有一条 event/thought。
- 反思结束后把计数重置为 max（`reset_reflection_counter`）。

### 4.2 记忆簇与洞察

文件：`reflect.py:21 generate_focal_points`、`:38 generate_insights_and_evidence`、`:99 run_reflect`；检索在 `retrieve.py:132 extract_recency`、`:175 extract_relevance`、`:199 new_retrieve`；提示词 `prompt_template/v2/generate_focal_pt_v1.txt`、`insight_and_evidence_v1.txt`。

- 取最近的 `importance_ele_n` 条 event/thought 的 `embedding_key` 拼成语句表，问模型“给定上述信息，最能回答的 3 个高层次问题是什么”（focal points）。
- 每个 focal point 调 `new_retrieve`：候选池为该 focal 相关的 event/thought；分别算
  - recency：`recency_decay ** i`，decay=0.99；
  - importance：节点 poignancy；
  - relevance：节点嵌入与 focal 嵌入的余弦；
  - 三个分数各自归一化到 [0,1]，再按 `recency_w=1 / relevance_w=1 / importance_w=1` 加权求和，取 top 30。
- 对每个 focal point 的检索结果编号后问模型生成 5 条 insight，格式 `insight (because of 1, 5, 3)`；代码把证据编号映射回节点 id。
- 每条 thought 落库：`created`、`expiration = created + 30 天`、三元组、关键词、poignancy、嵌入、证据节点 id。

移植注意：

1. 这是研究代码：`generate_insights_and_evidence` 用 `try/except` 失败时返回 `{"this is blank": "node_1"}`；C++ 移植必须改成显式校验（编号范围、数量、非空），失败返回 `Result<T>`，不能把占位串写进记忆。
2. numpy 余弦 → 手写 double 点积/范数即可；分数归一化要防止全零除零。
3. 提示词模板是 text-davinci 时代产物，示例格式和温度参数要按现在的本地模型改写；`generate_focal_pt_v1.txt` 全文只有一句核心问题 + 编号起始，适合做极小提示词的参考。
4. `importance` 分数由另一个 LLM 调用（poignancy）产生（`run_gpt_prompt_event_poignancy`），memcpp 需要决定是复用 LLM 打分还是用规则分；这是移植时最大的成本项。
5. 30 天过期是硬编码，应做成配置。

确认程度：reflect/retrieve/scratch 相关函数与两个 v2 模板读完；`run_gpt_prompt.py` 只读了 `run_gpt_prompt_focal_pt` 与 insight 函数入口；poignancy 等其余提示词未核对。

---

## 5. usearch（Apache-2.0）

版本 2.26.6；三个头文件约 530 KB（`index.hpp` 220K、`index_dense.hpp` 120K、`index_plugins.hpp` 192K），header-only C++11；CMake 导出 INTERFACE target（`CMakeLists.txt:117` 起）。

最小用法（`cpp/README.md`）：

```cpp
#include <usearch/index.hpp>
#include <usearch/index_dense.hpp>
using namespace unum::usearch;
metric_punned_t metric(dim, metric_kind_t::cos_k, scalar_kind_t::f32_k);
index_dense_t index = index_dense_t::make(metric);
index.reserve(n);
index.add(key, vec);
auto results = index.search(query, k);
index.save("index.usearch");   // 或 load() 拷贝载入 / view() 内存映射
```

要点：

- `index_dense_t` 是 `index_dense_gt<>` 别名（`index_dense.hpp:2426`）；`add/search` 有 f64/f32/f16/bf16/i8 等重载，内部转换。
- 返回值不是异常：`add_result_t`、`search_result_t`、`serialization_result_t` 都可 `explicit operator bool`，失败带 `error` 字段；`save/load/view(char const*, ...)` 返回 `serialization_result_t`（`index_dense.hpp:1594/1601`）。
- 余弦度量的 distance 是 `1 - cosine`（`index_plugins.hpp:2291`）；取出后要自己转回相似度。
- `add` 线程安全，可配合 OpenMP；`reserve` 建议用 `try_reserve` 并检查返回值。
- 支持聚类（`cluster()` / 全库聚类），但 memcpp 第一版不必用。
- `view()` 是 mmap 只读视图，文件被外部改写/截断会出问题；索引文件建议“临时文件 + 原子 rename”发布。

移植注意：

1. **无异常规则**：Debug 构建（未定义 `NDEBUG`）下断言失败会进 `usearch_raise_runtime_error`；在 Windows 上该函数走 `std::terminate()`，非 Windows 且开了异常才 `throw`。DevDesk 不抛异常，务必 Release/NDEBUG 构建并检查所有 result；Debug 下也要保证参数先自检，别让库断言。
2. `index_dense_t` 的对象较大，建议 `std::unique_ptr` 持有；`metric_punned_t` 的维度必须与嵌入维度严格一致。
3. 索引文件格式带版本号，跨 usearch 版本不保证兼容；vendor 头文件时必须同时锁 commit，并在文件头写清格式版本，升级前做读写回归。
4. Windows 下 `char const*` 路径的编码行为未验证（**未核对**）；建议传 UTF-8 并先用 ASCII 路径做冒烟。
5. 若 memcpp 规模只有几千条记忆，精确余弦暴力检索足够，usearch 可作为数据量增长后的替换实现；接口先抽象成 `VectorIndex`，避免绑死。

确认程度：README、API 签名、余弦实现、result 结构已读；未编译、未跑基准；`view` 的 Windows 行为未验证。

---

## 6. llama.cpp（MIT）

### 6.1 embedding 示例

文件：`examples/embedding/embedding.cpp`（413 行）；构建：`examples/embedding/CMakeLists.txt` 中 `llama-embedding` 链接 `llama-common` + `llama`，C++17。

示例流程：

1. `common_params_parse` + `params.embedding = true`；
2. `llama_backend_init()` / `llama_numa_init`；
3. `common_init_from_params(params)` 得到 model/context；
4. `llama_model_get_vocab`、`common_tokenize` 分词；
5. 按 `n_batch` 与 `n_seq_max` 切批，`common_batch` 逐 token 加入；
6. `llama_memory_clear(llama_get_memory(ctx), true)`（嵌入不关心 KV）；
7. `llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch)`；
8. 池化非 NONE 时 `llama_get_embeddings_seq(ctx, seq_id)`，NONE 时 `llama_get_embeddings_ith(ctx, i)`；
9. `common_embd_normalize(embd, out, n_embd_out, embd_norm)` 做 L2/税/最大绝对值归一化。

### 6.2 可直接使用的公开 API（llama.h）

- `llama_backend_init()` / `llama_backend_free()`（`:489/492`）
- `llama_model_default_params()` / `llama_model_load_from_file()`（`:482/525`）
- `llama_context_default_params()` / `llama_init_from_model()`（`:483/552`）
- `llama_free(ctx)` / `llama_model_free(model)`（`:562/550`）
- `llama_model_n_embd_out(model)`（`:602`）
- `llama_get_embeddings_seq(ctx, seq_id)` / `llama_get_embeddings_ith(ctx, i)`（`:1169` 附近）
- `llama_batch_init` / `llama_batch_free` / `llama_encode` / `llama_decode`（`:976/981/990/1006`）
- 新接口 `llama_process(ctx, LLAMA_PROCESS_TYPE_ENCODE|DECODE, llama_batch_ext*)`（`:1092`；`llama_process_type` 在 `:297`）

关键参数：

- `llama_context_params.embeddings = true`（`:408`）；
- `pooling_type`：`UNSPECIFIED/NONE/MEAN/CLS/LAST/RANK`（`:177-184`）；加载后若模型元数据也没指定，默认是 `NONE`（`src/llama-context.cpp:308-312`），embedding 模型通常要显式设 `MEAN`（或按模型卡设 `CLS`）；
- `attention_type = NON_CAUSAL`：非因果嵌入模型必须设，否则池化结果不对；
- `n_batch` / `n_ubatch` / `n_seq_max` 控制批大小与并行序列数。

移植注意：

1. **示例依赖 `common/` 辅助库**（`common_init_from_params`、`common_batch`、`common_tokenize`、`common_embd_normalize`），不是一个可独立拷贝的单文件。C++ 集成有两条路：链接 `llama-common` 静态库，或在 memcpp 里用 `llama.h` 公开 API 重写薄封装。后者更可控，但分词/批处理代码要自己写。
2. 当前 HEAD 的示例用了新的 `llama_process` + `common_batch`；公开头里仍保留 `llama_encode/llama_decode`。头文件与示例的 API 都在演进，**必须锁 commit**，不要跟 master 浮动。
3. `llama_get_embeddings_seq` 在 `pooling_type == NONE` 时返回 NULL；此时只能按 token 取。embedding 模型应走池化路径，一条输入一个向量。
4. L2 归一化在 `common/common.cpp:1923`，是普通浮点计算；memcpp 自己实现即可（注意零范数）。
5. 编码器模型用 `llama_encode`（不使用 KV cache），解码器式嵌入模型用 `llama_decode`；用 `llama_model_has_encoder/has_decoder` 判断。示例的 `llama_process(DECODE, ...)` 针对它支持的模型，不要无条件照抄。
6. 构建产物大、首次编译慢；Windows/MinGW 需按仓库根 CMake 构建静态 `llama`。GGUF 模型本身有独立许可证，不随 MIT 代码授权。
7. 本地推理的延迟/内存占用要在 DevDesk 目标机上实测；不要假设桌面空闲时推理无感。

确认程度：embedding 示例与 llama.h 相关段读完；未构建、未跑模型；`llama_process` 与示例 helper 的内部实现未核对。

---

## 不能借 / 需自研

以下没有可直接搬的现成实现，必须在 memcpp 内设计：

1. **C++ 记忆存储层**：条目表、来源表、链接表、失效时间字段、迁移与索引。六个仓库里没有 C++ 存储实现；claude-mem 的 SQLite 表结构（`dedup-store.ts`、memory item schema）只能作字段设计参考。
2. **dream 调度与生命周期**：空闲触发、并发限制、失败重试、任务持久化。claude-mem 的 worker/spool 是 Node 守护进程，只能借“事件落盘 + 异步 drain + 只 exit 0”的思想。
3. **中文分词与 IDF 语料维护**：claude-mem 的 `tokenizeWs` 只适用空格语言；中文必须自研/引入分词器，并重新标定 `cosineThreshold`/`vetoThetaIdf`/`minSharedTokens`。
4. **Unicode NFC 与类别过滤**：JS 的 `normalize('NFC')` + `\p{L}\p{N}\p{M}` 在 C++ 标准库没有；需要 ICU/utf8proc 或 DevDesk 现有工具（未核对）。
5. **embedding 模型管理**：GGUF 选择、下载、缓存、版本校验、失败回退。llama.cpp 只给推理 API。
6. **矛盾/失效的复核 UI 与队列**：claude-mem 有候选状态表（pending/merged/distinct/dismissed），但桌面端审核交互要自研。
7. **混合检索排序**：generative agents 的 1:1:1 权重是研究默认值，mem0 主链路是纯向量检索；memcpp 需要自己的“向量 + 关键词 + 时间 + 重要性”融合与评测。
8. **提示词工程本身**：六个仓库的提示词都绑定了各自领域（编码观测、个人事实、剧情角色）；memcpp 的记忆类型、抽取粒度、中文表达必须重写，不能整段照抄。可借的是结构与判定规则。

---

## 许可证结论

- claude-mem、mem0、graphiti、generative_agents、usearch 均为 **Apache-2.0**；llama.cpp 为 **MIT**。全部允许商用、修改、再分发。
- Apache-2.0 义务：保留版权与许可声明；随分发提供 LICENSE 副本；如上游有 NOTICE 必须一并保留（**claude-mem 有 `NOTICE`**，其余本次未见 NOTICE 文件）；修改过的文件应注明变更。
- MIT 义务：保留版权声明与许可全文。
- 建议落位：
  - `third_party/<repo>/LICENSE`（原文）
  - `third_party/<repo>/NOTICE`（仅 claude-mem）
  - `third_party/<repo>/UPSTREAM.md`（仓库 URL、commit sha、取用范围、修改说明）
  - 仓库根 `NOTICE` 汇总所有第三方组件与 commit
  - 移植到 memcpp 的源文件头保留 SPDX 标识与来源注释；提示词文本被整体搬用时，在资源文件头注明来源与许可证。
- 不要因为“只借了算法思想”就省略声明：本次可借内容包含可受版权保护的提示词文本与代码结构，按 Apache-2.0 归属最稳妥。
- 模型文件（GGUF）与训练数据不在上述代码许可证覆盖范围内，需单独确认。

---

## 复现与核对命令

```bash
for n in claude-mem mem0 graphiti generative_agents usearch llama.cpp; do
  echo "$n $(git -C D:/software/src/oss-ref/$n rev-parse HEAD)"
done
```

本次已实测的 HEAD：

```
claude-mem         7325fb7818ff7426665d85922db642ec0931bdcd
mem0               2c81af3bbe27d9bccb09c772d583df9e9be72ee5
graphiti           5c27edfa8d3e0f49b2571bcebb4261bcc89ee037
generative_agents  fe05a71d3e4ed7d10bf68aa4eda6dd995ec070f4
usearch            a985f77553d2d58d5ef1516eafaae81480bb3c30
llama.cpp          c6fb3b5be2209f6c12339833198190f6f6829581
```