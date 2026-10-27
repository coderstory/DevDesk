# memcpp P0 基础链路实现规格书（拍板前预备）

- 日期：2026-10-27
- 读者：后续开工写码的 AI 开发 + 项目负责人（不熟悉 C++）
- 上游依据（本文只读其快照，正被另一任务线修订中，冲突时以上游修订为准并回改本文）：
  - 可行性报告：`docs/superpowers/research/2026-10-27-memcpp-feasibility.md`（P0 口径 §6.1、分词 §4.6、写入纪律 §4.1、污染对策 §4.8）
  - 业界实证 v2：`docs/superpowers/research/2026-10-27-memcpp-prior-art-evidence-v2.md`（§2 FTS5 中文）
  - 提案卡片设计（下称 B4）：`docs/superpowers/plans/2026-10-27-memcpp-proposal-card-design.md`（§1 契约、§5 拆票）
  - 已验证集成方式：`tools/memcpp-probe/`（CMakeLists.txt + README.md，commit `d08f4ac7`/`6a8f8e0c`）
  - 交接：`docs/handoffs/2026-10-27-memcpp-handoff.md`
- 范围：**P0 只做单 exe 记忆核心（采集 / 存储 / 检索 / recall）。云端抽取不在 P0**（等拍板①分发形态）。本地 embedding / 向量检索也在 P1（报告 §6.1），P0 不链 llama.cpp。

---

## 0. P0 口径与出口条件（照抄报告 §6.1，本文不另立）

P0 内容 = 增量采集（尾读 jsonl + hook 触发/注入骨架）、SQLite 存储、FTS5 `unicode61` + 中文双字分词 + LIKE 短查询兜底、MCP `recall`、单 exe 三子命令骨架。

出口条件（四条，逐条对应 §6 测试计划）：
1. 真机接进 Claude Code；
2. 手动 `recall` 能查到写入的事实；
3. 2 字中文查询可用；
4. hook 失败不影响使用。

术语（首次出现就地人话，后同）：jsonl = 每行一个 JSON 对象的文本文件；FTS5 = SQLite 内置的全文检索引擎；token = 把文本切出来的检索单元；双字/bigram = 相邻两字一组的滑窗切块（"端口配置"→`端口 口配 配置`）；hook = Claude Code 预留给外部程序、到固定时机就调用的口子；MCP = Claude Code 调用外部工具的协议；stdio = 标准输入/输出流；epoch 秒 = 从 1970-01-01 起的秒数；DDL = 建表的 SQL 语句。

---

## 1. 目录与模块

### 1.1 源码目录（新建，推荐仓库顶层 `memcpp/`）

```
memcpp/
  CMakeLists.txt          # 独立工程根，不进 DevDesk 主构建树（理由见 1.3）
  README.md               # 构建与复现说明（照 probe README 体例）
  third_party/sqlite3/    # SQLite amalgamation（sqlite3.c/.h，编译宏开 FTS5）
  src/
    main.cpp              # 子命令分发：hook / mcp / daemon / recall
    cli/                  # 参数解析与全局覆盖项（--db、--projects-root）
    util/                 # json 解析、字符串规范化、日志
    store/                # schema DDL、打开/迁移、DAO（memories/sessions/proposals/水位）
    tokenize/             # 双字分词器（入库与查询共用同一实现）
    collect/              # jsonl 扫描、尾读、解析、过滤
    retrieval/            # FTS 主路径 + LIKE 兜底 + 排序
    hook/                 # hook 子命令（stdin 协议桩，见 §4.3）
    mcp/                  # MCP stdio server（recall，见 §4.2）
    daemon/               # 尾读轮询循环（--once 单轮模式供测试）
  tests/                  # 单测（复用仓库 tests/doctest.h 迷你框架，相对路径引用）
```

- 与 DevDesk 主程序关系：**memcpp 是独立 exe，不进 `src/` 模块体系、不依赖 EUI-NEO/GUI**。备选"复用 `src/core/` 的 sqlite 封装"已否：memcpp 独立 CMake 树无法复用主构建树的 target（主树顶层串着 EUI-NEO），自持薄封装成本更低、构建树完全解耦。
- 对接只走两个本地契约：
  1. **同一个 SQLite 库文件**（§2.1 位置）。P2 的 DevDesk 记忆页卡片（B4 §5 C7 接线）读写同一张 `proposals` 表。
  2. **Claude Code 侧的 hook/MCP 配置**。P0 用手动配置验证（真机出口条件），**不自动写 `~/.claude/settings.json`**（handoff 红线：该文件由 DevDesk/cc-switch 管理）；自动接线属 DevDesk 设置页职责（P1+）。
- **插件状态单一注册源红线（CLAUDE.md 轻资产原则）落到 memcpp 的口径**：记忆、提案、采集水位、元信息全部以这一个库文件为唯一真相；DevDesk UI、hook、MCP 都只是它的读者/受控写者，任何一侧不得落第二份状态副本（反例即 dsh 插件状态散落五处、坏一处要五处同步）。
- 并发口径（跨文件不变量）：**P0 单写者 = daemon**（采集落库）；`mcp` / `hook` / `recall` 以只读方式打开库（SQLite WAL 模式允许并发读）。P2 起 DevDesk UI 成为第二写者（只写 proposals 表），届时靠 WAL + `busy_timeout` 处理，P0 不做更多。

### 1.2 数据位置

- 库文件默认：`<数据根>/memcpp/memory.db`。数据根解析同 DevDesk 规则（`src/core/paths.h`）：环境变量 `DEVDESK_DATA_ROOT` 优先，否则 Windows `%USERPROFILE%/DevDesk/`。另支持 `MEMCPP_DB_PATH` 单独覆盖库文件（测试隔离用；测试纪律照 CLAUDE.md：独立 `DEVDESK_DATA_ROOT`，不碰用户真实数据）。
- Claude Code 会话根默认 `~/.claude/projects/`，支持 `MEMCPP_CC_PROJECTS_ROOT` 覆盖（测试喂 fixture 用）。
- 建库时幂等确保数据根 `.gitignore` 含 `memcpp/` 条目（沿用 `paths.h` 里"防误提交"机制精神：WAL 文件默认可被同机他人直读）。

### 1.3 CMake 组织

- 独立工程（参照 `tools/memcpp-probe/CMakeLists.txt` 已验证形态）：`project(memcpp C CXX)`、C++20（仓库现行标准；probe 用 17 已验证 MinGW 可编，memcpp 无 EUI-NEO 依赖可自由取 20）。
- SQLite 取 amalgamation 源码并入 `third_party/sqlite3/`，编译宏 `-DSQLITE_ENABLE_FTS5`（自持版本、FTS5 能力可控；备选 MSYS2 系统包不可控 FTS5 编译宏，不取）。
- 静态契约照 probe：`target_link_options(memcpp PRIVATE -static)`——libgcc/libstdc++/winpthread 全静态，验收口径与 DevDesk 主程序相同（`objdump -p` 只剩 Windows 系统 DLL）。
- **llama.cpp 并入桩位（P1 才启用，P0 不编）**：照 `tools/memcpp-probe/CMakeLists.txt:10-19` 全套开关（`LLAMA_BUILD_EXAMPLES/TESTS/SERVER/TOOLS=OFF`、`LLAMA_CURL=OFF`、`BUILD_SHARED_LIBS=OFF`、`GGML_OPENMP=OFF`、`GGML_NATIVE=ON`）+ `add_subdirectory(llama.cpp)` + link `llama`；llama.cpp 源码不入库，构建脚本按 probe README 方式锁 commit `c6fb3b5be2209f6c12339833198190f6f6829581` fetch。P0 的 CMakeLists 留注释桩位即可。
- 构建入口：新增 `scripts/build-memcpp.sh`（PATH 前置 / ccache 三件套 / Debug 默认 / `--filter` 转发，镜像 `build-test-run.sh` 约定）。**待裁决**：并入 `build-test-run.sh` 加 `--memcpp` 开关 vs 独立脚本。推荐独立脚本，理由：P1 起 llama.cpp 编译时长不应挂在主程序每次增量构建上，probe 已示范独立工程形态可行。

---

## 2. 存储设计

### 2.1 总则

- SQLite，WAL 日志模式（写前日志，允许读写并发），`foreign_keys=ON`，`busy_timeout=5000`（本文自设值：P0 单写者 + 只读连接，5 秒退避足够；实验台 db.py:104 为 `timeout=10` 即 10 秒，**并非同一口径**，5000 无上游背书，待标定）。
- **零物理删除（报告 §4.1/§4.8）**：memories/proposals 任何"删"都是 UPDATE 状态标记；schema 不提供 DELETE 路径，FTS 同步触发器仍保留 delete 分支（服务状态翻转时的索引重建，不是删行）。
- schema 版本：`PRAGMA user_version`，迁移只加列加表、不改旧列。

### 2.2 DDL 草案

```sql
-- 会话登记：jsonl 文件 → 会话 id（文件名去 .jsonl，即 Claude Code 的会话 uuid）
CREATE TABLE IF NOT EXISTS sessions (
  id         TEXT PRIMARY KEY,
  project    TEXT NOT NULL,          -- ~/.claude/projects 下的项目目录名（如 D--project-gui）
  file_path  TEXT NOT NULL,
  started_at INTEGER,                -- epoch 秒
  ended_at   INTEGER
);

-- 记忆条目。P0 无抽取器，写入单元 = 过滤后的段级原文（kind='segment'）；
-- 抽取上线（P0.5/P3）后增 kind='fact'，段级候选由提案流程 superseded
-- （superseded = 被新条目取代的标记：原文保留、只改 status/superseded_by，绝不物理删除），绝不原地改写
CREATE TABLE IF NOT EXISTS memories (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  schema_version INTEGER NOT NULL DEFAULT 1,
  project        TEXT NOT NULL DEFAULT '',
  kind           TEXT NOT NULL DEFAULT 'segment',   -- P0: segment；预留 fact
  content        TEXT NOT NULL,        -- 面向人的正文（recall 返回这个）
  search_text    TEXT NOT NULL,        -- 规范化原文：ASCII 小写、压空白；LIKE 兜底扫这列
  tokens         TEXT NOT NULL,        -- 双字分词结果，空格连接；FTS5 索引这列
  source_trust   TEXT NOT NULL CHECK (source_trust IN
                   ('user_explicit','assistant_derived','tool_result','external_doc')),  -- 报告 §4.8 信任分级
  session_id     TEXT,                 -- 证据链：来源会话（sessions.id）
  event_seq      INTEGER,              -- 证据链：来源事件在会话内的行序号
  confidence     REAL NOT NULL DEFAULT 0.5,  -- P0 段级候选固定 0.5（无抽取器无置信依据；设计值待标定）
  status         TEXT NOT NULL DEFAULT 'active' CHECK (status IN
                   ('active','invalidated','superseded')),
  superseded_by  INTEGER REFERENCES memories(id),
  version        INTEGER NOT NULL DEFAULT 1, -- 提案 baseVersion 前置校验基准（B4 §1）
  created_at     INTEGER NOT NULL,
  invalid_at     INTEGER,              -- Graphiti 式失效时间，可空（报告 §2.3）
  access_count   INTEGER NOT NULL DEFAULT 0,
  last_accessed_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_memories_project_status ON memories(project, status);
CREATE INDEX IF NOT EXISTS idx_memories_session ON memories(session_id);
-- 幂等键（§3.2 双职责：截断重写去重 + 崩溃恢复第二道防线）：同一 (session_id,event_seq) 至多一条 active 行。
-- superseded 行不受限，保留供溯源；部分唯一索引只约束 kind='segment'（fact 行与来源事件无一一对位概念，豁免）
CREATE UNIQUE INDEX IF NOT EXISTS idx_memories_session_event
  ON memories(session_id, event_seq) WHERE kind='segment' AND session_id IS NOT NULL AND status='active';

-- FTS5 虚表：external content（索引数据回头读 memories.tokens），触发器同步
CREATE VIRTUAL TABLE IF NOT EXISTS fts_memories USING fts5(
  tokens,
  content='memories', content_rowid='id', tokenize='unicode61'
);
-- 三个触发器（AFTER INSERT / AFTER UPDATE / AFTER DELETE）照 db.py:55-74 形态：
-- delete 分支用 fts_memories(fts_memories, rowid, tokens) 特例语句移除旧索引行

-- 采集水位（断点续读），语义照 tools/memory-lab/memory_lab/ingest.py 头注释，带一处零物理删除修正：
-- 只追加（size 变大）→ 从 offset 续读；变小或重写 → offset 回退 0 重读，派生行只翻标记不清（§3.2）；
-- 只消费以换行结尾的完整行
CREATE TABLE IF NOT EXISTS ingest_state (
  file_path TEXT PRIMARY KEY,
  size      INTEGER NOT NULL,
  mtime     REAL    NOT NULL,
  offset    INTEGER NOT NULL
);

-- 提案（B4 §1 契约的落库映射）。P0 只落表结构与幂等键，无任何写路径；状态机在 P2（B4 §5 C4）
CREATE TABLE IF NOT EXISTS proposals (
  id             TEXT PRIMARY KEY,     -- 幂等键（§5）；格式示例 = B4 §1.1（prop_YYYYMMDD_ + 唯一尾），
                                       -- 生成口径以"全库唯一 + 幂等语义"为准，勿把后缀位数当规范抄
  schema_version INTEGER NOT NULL,
  kind           TEXT NOT NULL CHECK (kind IN ('merge','invalidate','forget','add')),
  status         TEXT NOT NULL DEFAULT 'pending' CHECK (status IN
                   ('pending','accepted','rejected','expired')),
  target_ids     TEXT NOT NULL DEFAULT '[]',  -- JSON 数组文本（memories.id 列表）
  result_json    TEXT,                 -- B4 result 对象（merge/add 用；invalidate/forget 为空）
  evidence_json  TEXT NOT NULL DEFAULT '[]',
  confidence     REAL,
  source         TEXT,                 -- dedupe|conflict|decay|quarantine|inject
  source_trust   TEXT,
  created_at     INTEGER NOT NULL,
  expires_at     INTEGER,
  decided_at     INTEGER,
  applied_at     INTEGER,
  base_version   INTEGER,              -- 出建议时目标记忆的 version
  undo_of        TEXT REFERENCES proposals(id),
  payload_json   TEXT NOT NULL         -- 原始完整 JSON 全文（契约演进兜底，列删改后仍可对账）
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_proposals_undo_pending
  ON proposals(undo_of) WHERE undo_of IS NOT NULL AND status='pending';  -- B4 §1.2 不变量：同一 undoOf 至多一张未决

CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
```

### 2.3 分词选型：unicode61 + 入库前双字（不做自定义 tokenizer）

结论（报告 §4.6 + 实证 v2 §2，不再开放）：

1. **不用 `trigram`**（3 字滑窗切块）：官方文档明文少于 3 字符 "do not match any rows"，实测 2 字中文（端口/配置/模块）命中 0 行。
2. **不用 FTS5 自定义 tokenizer**（用 C 写进 SQLite 的切词插件）：需自写回调与 Unicode 处理、维护索引一致性，无证据表明质量收益；现成扩展 `simple` 需外挂 + jieba 词典首载约 4 秒 + MIT/GPL 双许可，不取。
3. **取"入库前双字 + `unicode61` 词级索引"**：unicode61 下中文连续串成一个 token，必须外部预分词；双字先例三大生态齐备（MySQL ngram 默认 2 元、PostgreSQL pg_bigm、Lucene CJKAnalyzer），冻结集实测 Recall@5 1.00。
4. **驼峰不切分**：Solr WordDelimiterGraphFilter 有切分先例，但 FTS5 无现成过滤器需自写；裁决维持原文 + 子串兜底（v2 §2）。

C++ 分词器（`tokenize/`，入库与查询**必须共用同一实现**——两边不一致等于索引作废，此为反重构警告）：

- 逐码点扫描：连续字母/数字串 = 一个 token，整体小写（`getUserById` → `getuserbyid`；下划线/点/斜杠等非字母数字是边界，`snake_case` → `snake` `case`）。
- 连续中文串切重叠双字（"端口配置" → `端口 口配 配置`）；落单的中文字符取自身。
- 输出即 `memories.tokens`（空格连接）与查询 token 序列。

### 2.4 检索 SQL 形态（主路径 + 兜底）

查询串先过同一分词器得 token 序列 `[t1..tn]`，然后：

1. **n < 2（如单个 2 字中文词 → 1 个 token）→ 直接走 LIKE 兜底**（报告 §4.6 原口径）。
2. **n ≥ 2 → FTS 主路径**（token 都是字母数字/汉字，无 FTS 特殊字符，无需转义）：
   ```sql
   SELECT m.* FROM fts_memories f JOIN memories m ON m.id = f.rowid
   WHERE fts_memories MATCH :or_query          -- 't1 OR t2 OR ...'
     AND m.status = 'active' AND m.project MATCH 过滤见下
   ORDER BY bm25(fts_memories) LIMIT :n;   -- bm25 = 全文检索通用相关度打分公式（FTS5 内置，值越小越相关）
   ```
   （project 过滤用 `AND m.project = :project` 拼进 WHERE；`MATCH` 只用于 fts 列。）
3. **FTS 返回 0 行 → LIKE 兜底**：各 token 对 `search_text` 做 `%token%`，OR 连接，命中即入，按命中 token 数降序排（次序信号弱，仅兜底）：
   ```sql
   SELECT * FROM memories
   WHERE status='active' AND (search_text LIKE '%t1%' ESCAPE '\' OR search_text LIKE '%t2%' ESCAPE '\')
   LIMIT :n;
   ```
   LIKE 双通配必走全表线性扫（官方明文），记忆量几千到几万条可接受（报告 §4.6）。

**验收形态（出口条件 3 的 SQL 依据）**：查"端口"→ 1 token → LIKE `%端口%` 命中；查"端口配置"→ tokens `端口 口配 配置` → FTS OR 命中且 bm25 排序；查"getUserById"→ 1 token → LIKE `%getuserbyid%` 命中（两侧都规范化小写，不依赖 LIKE 的大小写默认行为）。性能参照报告 §3.7：FTS 查询 0.04~0.57 毫秒（1 万条）、建索引 1 万条 1.47 秒——毫秒级，不构成瓶颈。

---

## 3. 采集接口：jsonl 会话尾读器

### 3.1 输入契约

- 扫描根：`MEMCPP_CC_PROJECTS_ROOT`（默认 `~/.claude/projects/`）下一层项目目录，每个目录顶层 `*.jsonl`（非递归；子代理会话不入 P0，与实验台 `scan_sessions` 同口径）。
- 解析口径以 `tools/memory-lab/memory_lab/ingest.py` 为基准（真实数据 1412 条事件、坏行 0 已验证）：坏行跳过并计数，绝不中断；时间戳统一 epoch 秒。
- 轮询间隔：daemon 默认 5 秒扫一轮（**设计值，报告未给数，待标定**；一轮 = stat 全部已登记文件 + 收录 mtime 变新的新文件）。

### 3.2 断点续读水位

`ingest_state` 表（§2.2），语义逐条照 ingest.py 头注释（重写行带零物理删除修正，见下），其余为已验证行为不是新设计：

| 文件状态 | 判定 | 动作 |
|---|---|---|
| 只追加 | size 变大 | 从上次 `offset` 续读 |
| 被截断/重写 | size 变小，或 mtime 变而 size 不变 | offset 回退 0 从头重读；**不删任何行**，去重与标记走下方重写处理路径 |
| 写入中 | 末行无换行符 | 半行留到下一轮，只消费完整行 |
| 无变化 | size/mtime 均不变 | 跳过 |

- 文件删除：水位行保留（文件可能回归），不派生动作。
- 幂等：同一文件稳定时二次执行新增 0 条（实验台实测口径，收进单测）。
- **重写处理路径（零物理删除修正）**：实验台"清派生行"清的是可重建的原始 events，本表存 memories，P0/P0.5+ 均禁止物理删（§2.2 不变量）。检测到重写（上表第 2 行）后：
  1. `offset` 回退 0，从头重读，全程不删任何 memories 行；
  2. 重遇 `(session_id, event_seq)` 同键且 `search_text` 相同（规范化原文，§2.2 列义）的段 → 跳过（幂等，等价稳定文件回放）；
  3. 同键不同内容（该事件被重写）→ 同一事务内按序三步：旧行 `status` 翻 `'superseded'`（`superseded_by` 暂空，释放 active 唯一槽位）→ ADD 新行（active）→ 旧行 `superseded_by` 回填新行 id（新 id 已存在，`foreign_keys=ON` 下 FK 校验通过）——写路径仍是只 ADD，旧行只做状态翻转（§2.2 零物理删除框架，T2 断言口径；三步一次提交，任一步中断整体回滚）；
  4. 重读完成后 → `event_seq` 大于本轮重读最大事件序号的旧行（会话尾部被重写截掉）批量翻 `'superseded'`，`superseded_by` 留空；
  5. superseded 行检索自然隐身：主/兜底两条 SQL 均固定 `status='active'` 过滤（§2.4），无需新增分支。
- **事务边界（崩溃恢复，对应报告 §5 门槛 6）**：每个文件每轮的"INSERT memories 行 + 推进 `ingest_state.offset`"包进一个 SQLite 事务一次提交——两写之间任何时刻崩溃即整体回滚，重启从旧水位重放该轮，无半途态；部分唯一索引 `idx_memories_session_event`（§2.2）为第二道防线，即使出现绕过事务的写路径也产不出重复行。

### 3.3 落库前最小过滤（报告 §4.2 四道，P0 全部落地）

1. **角色过滤（P0 收窄，显式决策而非实验台同口径）**：`thinking`、`tool_use`、`tool_result` 事件一律不落 memories。实验台 `_SKIP_KINDS = {thinking, tool_result}`（compress.py）在段内保留工具名/入参，报告 §4.2 也要求保留"工具名/入参摘要"——P0 无抽取器，工具文本产不出记忆收益，收窄以减入库噪声；抽取器上线后可放宽回"保留工具名/入参摘要"，只动本道过滤、不涉 schema。
2. **剔除注入块**：`system-reminder` 块、hook 输出、被引用的整篇文档/README/规范，不落库（报告实测：不过滤会把插件/技能文档当项目记忆抽出）。
3. **单事件截断**：单事件正文超 4000 字截断（实验台 `TEXT_LIMIT=4000`；真实数据单事件可达 86.5KB~290KB，不截断必炸）。
4. **段级落库单位**：过滤后正文按约 4KB 聚段（实验台 `build_segments` 默认 6000 字，本文刻意收紧对齐单事件 4000 字截断口径——设计值待标定）；一段内无任何用户/助手文本则整段跳过。

落库形态：每段一条 `memories` 行，`kind='segment'`、`source_trust` 按段内来源最高档（用户原文= `user_explicit`，否则 `assistant_derived`）、`confidence=0.5`、`content` = 段原文、`search_text`/`tokens` 由分词器派生。`event_seq` 取该段首个事件的行序号（锚事件；锚定规则：一个事件至多参与一个段）——该列与 `session_id` 合成重写幂等键（§2.2 部分唯一索引强制，§3.2）。P0 写路径：正文内容只 ADD、绝不原地改写；唯一允许的 UPDATE 是状态标记翻转（§2.2 零物理删除框架，本文唯一出现点 = §3.2 重写路径的 superseded 标记），无任何物理删除（报告 §4.1）。

---

## 4. 对外接口

### 4.1 CLI 三子命令（报告 §2.6 口径：hook / mcp / daemon）

| 子命令 | 职责 | P0 边界 |
|---|---|---|
| `memcpp daemon [--once]` | 常驻采集：尾读轮询循环落库 | P0 无任何网络/管道监听面（报告 §4.7 的 ACL/令牌/崩溃自拉等安全设计随 P1 HTTP hook 通道立项，P0 以"无网络面"回避）。`--once` 扫一轮即退出（测试/CI 用） |
| `memcpp mcp` | MCP stdio server，暴露 `recall` 工具 | 生命周期由 Claude Code 管理（它拉起子进程） |
| `memcpp hook` | Claude Code hook 调用入口：stdin 读事件 JSON，stdout 回注入内容 | 骨架/桩位（§4.3） |
| `memcpp recall "查询" [--project X] [--limit N]` | 只读调试动词 | 不在报告三子命令口径内，**仅为出口条件 2"手动 recall"提供不依赖 Claude Code 的人工验证入口**，实现是 retrieval 直调；可砍不留依赖 |

全局覆盖项：`--db PATH`（= `MEMCPP_DB_PATH`）、`--projects-root DIR`。daemon 自启托管（计划任务/注册表，报告 §4.7）不在 P0，P0 手动启动。

### 4.2 MCP `recall` 接口

- 传输：stdio 上的 JSON-RPC 2.0（MCP 协议事实：`initialize` 握手 → `tools/list` → `tools/call`；协议版本协商以实现时官方 spec 为准——注意 `D:/wt-mcp` 是另一会话的 MCP 骨架，只可参考、不可触碰，handoff 纪律）。
- **传输纪律（反重构警告）**：协议帧只走 stdout，任何日志/调试输出一律 stderr——一条杂散 printf 即静默破坏整条 MCP 通道，是实现期最难查的断连根因。
- 工具：`recall`。

| 项 | 契约 |
|---|---|
| 入参 | `query`（string，必填，1~512 字符）；`project`（string，可选，缺省不限项目）；`limit`（integer，可选，默认 5，范围 1~20） |
| 出参 | `content: [{type:"text", text}]`；每条记忆一行：`[m#<id> · <YYYY-MM-DD> · <status> · <中/高/低置信>] <content>（来源 <session_id>/事件<event_seq>）`；带日期与状态是报告 §4.5"注入内容带时间、可能过时标记和来源 ID"的同一要求。无结果 → text 为"（无匹配记忆）"，不算错误 |
| 错误码 | JSON-RPC 标准层：`-32700` 解析失败 / `-32600` 非法请求 / `-32601` 方法不存在 / `-32602` 参数非法（query 缺失或超 512、limit 越界）。工具层故障（库打不开等）：`tools/call` 正常响应但 `isError: true` + 一句话原因，**进程不崩、stdio 循环不退** |
| 性能 | 每次调用现开只读连接，毫秒级返回（§2.4）；不加载任何模型 |

### 4.3 hook 预载通道对接桩位（协议事实已钉版：`43a4d75f` hook-contract-facts）

- 输入：stdin 收 Claude Code hook 事件 JSON（`UserPromptSubmit` 场景含 `session_id`、`hook_event_name`、`prompt` 字段——**权威出处 = `docs/superpowers/research/2026-10-27-memcpp-hook-contract-facts.md`**（commit `43a4d75f`，核验时点 Claude Code 2.1.311，已关闭报告 §7.18"未钉版"项）；后续升级漂移靠该文 §3 的 P1 契约测试捕捉）。
- 输出：stdout 回 `{"hookSpecificOutput":{"hookEventName":"UserPromptSubmit","additionalContent 占位":"…"}}` 形态的注入 JSON；P0 骨架内检索可返回空串（骨架不承载真注入，注入预算/触发判定在 P1）。
- **失败开放（出口条件 4）**：任何异常——stdin 非法、库打不开、查询报错——一律输出为空 + 退出码 0（claude-mem 已验证的失败开放模式，报告 §2.1）；退出码非 0 会阻塞用户，此为反重构警告。
- 时限意识：同步 hook 超时会丢弃注入内容（30 秒默认，报告 §3.5），hook 路径内不做任何慢操作（无模型、无全量扫描）。

---

## 5. 与提案卡片的衔接

- **落点：独立 `proposals` 表**（§2.2 DDL），不复用"memories 表 + 状态行"。理由：提案状态机字段多（status/decidedAt/appliedAt/baseVersion/undoOf/expiresAt），混进记忆表会让检索、注入、统计处处多一道"剔除提案行"过滤；撤销不变量"同一 `undoOf` 至多一张未决"（B4 §1.2）靠部分唯一索引表达，混表做不干净。备选"复用记忆表加 kind"已否，不再开放。
- 列 ↔ JSON 字段映射逐一对齐 B4 §1（id/schemaVersion/kind/targetIds/result/evidence/confidence/source/sourceTrust/createdAt/expiresAt/status/decidedAt/appliedAt/baseVersion/undoOf），另存 `payload_json` 全文兜底。
- **P0 只落存储形态**：建表 + 幂等键语义（同 `id` 同内容 = INSERT OR IGNORE 直接返回；同 `id` 不同内容 = 拒绝入库 + 审计日志留痕，B4 §1.1）。状态机写路径、`baseVersion` 前置校验、批量、撤销、审核 UI 全部在 P2（B4 §5 C4~C8）。P0 内 proposals 表零写入。
- 衔接收益：B4 §5 "等 P0 存储定稿才可开工"的 C4（提案存储）以本节为定稿依据；C2 UI 骨架可对着真 schema 造 mock 数据先行（B4 §5 原文许可）。

---

## 6. 测试计划

### 6.1 单测点清单（先红后绿：每条写明"修掉什么会红"）

| # | 用例 | 断言 | 红的条件（先红） |
|---|---|---|---|
| T1 | 建库幂等 | 重复 `init_schema` 不报错不清数据；`user_version` 迁移只增 | 迁移逻辑重建表/清数据 |
| T2 | 零物理删除 | 调"删除"路径后行仍在、status 翻转、version+1；全库 row 数不变 | 有人实现成真 DELETE |
| T3 | FTS 触发器同步 | insert/update memories 后 fts_memories 行数与内容一致 | 触发器漏分支 |
| T4 | 双字分词 | "端口配置"→`端口 口配 配置`；`getUserById`→单 token 小写；单汉字→自身；中英混排边界 | 分词实现与 §2.3 规则漂移 |
| T5 | 2 字中文检索（出口 3） | fixture 含"端口"的行，query"端口"命中；"配置""模块"同 | 换成 trigram（实测 0 命中）即红 |
| T6 | 4 字查询走 FTS | "端口配置" → FTS MATCH 命中且 bm25 排序生效 | OR 拼接/排序错 |
| T7 | LIKE 兜底自动触发 | 构造 FTS 0 行查询 → 结果来自兜底路径；单 token（2 字词）直接走兜底 | 兜底分支被删 |
| T8 | 驼峰标识符检索 | 存"getUserById 相关记忆"，query"getUserById"命中（小写化后 LIKE） | 规范化漏小写/错切驼峰 |
| T9 | 尾读断点 | 追加 → 只读增量；半行挂起；size 变小 → 重读后同键同行幂等跳过、被重写/尾部截掉的旧行翻 superseded 且**全库行数不变**；二次执行新增 0 | 水位语义偏离 §3.2 口径（含零物理删除修正），或重写路径出现物理删行 |
| T10 | 过滤四道 | system-reminder 块不落库；>4000 字截断；纯 thinking/tool_result 段跳过；来源信任档正确 | 过滤规则缺失（对应报告 §4.8 污染入口） |
| T11 | proposals 幂等 | 同 id 同内容重复投递只落一条；同 id 异内容拒绝且审计留痕；同一 undoOf 第二张未决被唯一索引拒绝 | 幂等键/部分唯一索引缺失 |
| T12 | MCP 契约 | initialize/tools/list/tools/call 往返；非法参数 → `-32602`；坏 JSON → `-32700`；不存在的方法 → `-32601`；非 JSON-RPC 非法请求 → `-32600`；空结果 → 正常响应"（无匹配记忆）"；库打不开 → `isError:true` 且进程存活 | 错误码/失败开放语义缺失（四个错误码任缺一即红） |
| T13 | hook 失败开放（出口 4） | stdin 喂垃圾/库路径指向不可写盘 → 退出码 0、stdout 空；合法输入 → 输出含 hookSpecificOutput 键 | 有人把异常变成非 0 退出 |
| T14 | 崩溃恢复事务（§3.2） | 构造"行已插、水位未推"（模拟两写之间中断）→ 重跑同轮：行数不变（崩溃重放撞 active 行仍被部分唯一索引拒绝，防重职责不变）、水位正常推进；事务内中途回滚 → 重启重放落库正确且无重复 | 行插入与水位推进入了不同事务/自动提交，中断后重启重复入库 |

### 6.2 probe / 集成层

- 无 UI，真窗口 probe 不适用。**CLI 集成测试**替代：合成微型会话 fixture（放 `memcpp/tests/fixtures/`）→ `memcpp daemon --once` 落库 → 起 `memcpp mcp` 子进程走一轮真实 stdio JSON-RPC recall 查到该事实 → 断言退出码与输出。单进程内可把 mcp 收发抽成可测函数，子进程形态只留一条冒烟用例。
- 真机出口条件（人工步，不进 CI）：手动把 hook + MCP 配置写进 Claude Code → 跑一段真实会话 → `memcpp recall "2字中文词"` 命中 → 拔掉 memcpp（改名 exe）确认 Claude Code 不受影响。

### 6.3 P0 出口条件对照

| 出口（报告 §6.1） | 验证物 |
|---|---|
| 真机接进 Claude Code | §6.2 人工步：hook 不阻塞会话 + MCP 工具可见可调用 |
| 手动 recall 能查到写入的事实 | `memcpp recall` 调试动词 + §6.2 CLI 集成冒烟（daemon --once 落库 → 真实 stdio recall 命中同一事实） |
| 2 字中文查询可用 | T5（端口/配置/模块三词固定 fixture） |
| hook 失败不影响使用 | T13 失败开放断言 + 真机拔除验证 |

---

## 7. 待拍板输入清单（每项标影响面与 P0 默认假设）

| # | 待拍板 | 影响哪节 | P0 默认假设（拍板前照此写码） |
|---|---|---|---|
| 1 | **包体上限**（<200MB / <500MB，报告 §8.2） | §1.3 CMake（P1 模型是否随包） | **不影响 P0 代码**：P0 不链 llama.cpp、不带模型，包体 exe 级（probe 单 exe 实测 9.94 MiB 量级）。拍板只决定 P1 资产口径 |
| 2 | **打包方式**（bundled vs 首次使用下载，报告 §8.2） | §1.3 CMake + 未来 installer 接线 | 按 **bundled** 起草（probe 三数字即按随包口径测：exe 9.94 + 模型 25.25 = 35.19 MiB）；首次下载路线需加下载器组件，属 P1 新立项，P0 无代码受影响 |
| 3 | **分发形态①**（纯本地 / 本地+中转网关 / 私有化，报告 §8.1） | §3 采集落库后的抽取挂点、§4 接口（云端抽取客户端 + 脱敏层接线——脱敏层已实现并过 110 条红队样本，commit `a98e3f91`/`2f545b41`） | P0 无抽取器，**代码不受影响**；按"本地+网关"预留抽取器接口缝（只定义接口位，不实现）。若拍板纯本地，P0.5 本地抽取 spike 转正为默认路径 |

补充（非拍板项的外部依赖）：hook 协议事实已由 `43a4d75f` 钉版（§4.3 引用已回改）；`~/.claude/settings.json` 的自动接线归 DevDesk 设置页（§1.1），P0 一律手动配置。

---

## 8. 开工顺序建议（本规格内的施工序，不新增决策）

1. `store/`：DDL + 打开/迁移 + 零物理删除 DAO（T1~T3 先红）。
2. `tokenize/`：分词器（T4），store 写入接上派生列。
3. `retrieval/`：FTS 主路径 + LIKE 兜底（T5~T8）。
4. `collect/`：扫描 → 尾读 → 过滤 → 落库（T9~T10、T14），接 `daemon --once`。
5. `mcp/` + `hook/` + `recall` 调试动词（T12~T13）。
6. CLI 集成冒烟（§6.2）→ 真机人工出口（§6.3）。

*本文所有"设计值/待标定"处（轮询间隔 5 秒、confidence 0.5、MCP limit 上限 20）均为占位，落地后按真实数据校准；所有选型结论（分词、proposals 落点、独立 exe）均引上游文档，无本文新立架构决策。*
