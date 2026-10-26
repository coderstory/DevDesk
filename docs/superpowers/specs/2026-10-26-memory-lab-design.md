# memory-lab 设计（记忆实验台）

- 状态：v1 基础层已落地（配置 / 数据库 / 数据模型 / 会话摄取）
- 日期：2026-10-26
- 位置：`tools/memory-lab/`（Python，仅标准库；不改 DevDesk C++ 代码）

## 1. 背景与目标

Claude Code 每次会话都会把完整对话写进
`~/.claude/projects/<项目>/<会话>.jsonl`。这些文件是原始流水账：一次任务
可能几千条事件，里面有思考、工具输出、重复内容。人和模型都不可能每次重读。

本项目要做的是：**把这些流水账变成少量、可检索、可复用的记忆**，并验证
一套以后能用 C++ 重写的算法参数。

目标按依赖顺序：

1. 摄取：把 jsonl 增量、幂等地转成结构化事件（已完成）。
2. 压缩：把事件窗口压成「观察」（observation，一条记忆）。
3. 嵌入：给观察生成向量，支持语义相似检索。
4. 检索：全文 + 向量混合召回，返回可读片段。
5. MCP：把检索能力包装成模型可调用的工具。
6. dream：定期整理记忆，只输出提案，不自动改。
7. 验证：用真实会话和真实 MCP 协议调用量化效果。

## 2. 非目标

- 不改 DevDesk 的 C++/CMake 代码，不引入 DevDesk 运行时依赖。
- 不写回、不修改 Claude Code 的任何 jsonl 文件。
- 不做多用户、云同步、权限体系；单机单用户。
- 不保存密钥、token、图片二进制等敏感内容。
- dream 不自动删除/合并记忆；所有变更先生成提案。
- 不追求一步到位的完美压缩；先量化，再调参。

## 3. 总体架构

```
Claude Code 会话 jsonl
        │
        ▼
  ① 摄取 ingest ──────────► SQLite: sessions / events / ingest_state
        │
        ▼
  ② 压缩 compress ────────► SQLite: observations（一条 = 一条记忆）
        │
        ▼
  ③ 嵌入 embed ───────────► SQLite: embeddings（向量 BLOB）
        │
        ▼
  ④ 存储 / 索引 ──────────► observations + FTS5(trigram) + 向量表
        │
        ├──► ⑤ 检索 retrieve（全文 + 向量融合）
        │           │
        │           ▼
        │    ⑥ MCP server（stdio，JSON-RPC）
        │           │
        │           ▼
        │    模型 / DevDesk 调用 memory_* 工具
        │
        └──► ⑦ dream（merge / promote / stale）
                    │
                    ▼
             dream_proposals（待人工/上层确认，不自动改）
```

职责边界：

| 层 | 输入 | 输出 | 是否改数据 |
|----|------|------|-----------|
| 摄取 | jsonl | sessions / events | 只增不改源文件 |
| 压缩 | events 窗口 | observations | 插入新观察，标记旧观察 |
| 嵌入 | observations | embeddings | 插入/覆盖向量 |
| 检索 | 查询串 | 命中列表 | 只读 |
| MCP | 工具调用 | JSON 结果 | 检索只读；memory_save 只增 |
| dream | 统计+相似度 | 提案 | 只写 proposals |

## 4. 存储设计（SQLite）

数据库默认 `~/.devdesk/memory-lab/memory.db`，WAL 模式，开启外键。
DDL 的唯一事实来源是 `tools/memory-lab/memory_lab/db.py` 的 `SCHEMA_SQL`。

| 表 | 作用 | 关键约束 |
|----|------|----------|
| `sessions` | 一个 jsonl 会话 | `id` 主键；project/file_path 必填 |
| `events` | 会话内的结构化事件 | `UNIQUE(session_id, seq)`，重复写入用 REPLACE |
| `observations` | 一条记忆（压缩产物） | 自带 `status`（active/merged/stale…）与 `merged_into` |
| `embeddings` | 向量 | `UNIQUE(owner_type, owner_id, model)` |
| `dream_proposals` | dream 产出的待审提案 | `status` 默认 `pending` |
| `ingest_state` | 每文件增量游标 | 主键 `file_path`：size/mtime/offset |
| `meta` | 杂项键值 | key 主键 |
| `fts_observations` | 全文索引 | FTS5 external content + trigram |

`observations` 的列表字段存储约定：

- `facts`：换行分隔（一条事实一行）
- `files`：换行分隔
- `concepts`：逗号分隔

Python 侧统一用 `list[str]`，转换只在 `db.py` 里发生。

全文索引由三个触发器维护：插入、更新、删除各一个；业务代码不手动改 FTS。
外部分词器用 `trigram`，因为中文没有空格、`unicode61` 只能整段匹配。
本机 SQLite 3.50.4 已实测支持；若换环境不支持，退回 `unicode61` 并在
README 记录，此时中文检索降级为整词匹配。

## 5. 摄取（已完成）

- 扫描 `~/.claude/projects/<项目>/*.jsonl`，按修改时间从新到旧，`limit` 取最新 N 个。
- 事件抽取：
  - `type=user`：字符串正文 → `user`；列表里的 `text` → `user`，
    `tool_result` → `tool_result`（摘要 2000 字，meta 记 `tool_use_id`）。
  - `type=assistant`：`text` → `assistant`，`thinking` → `thinking`（2000 字），
    `tool_use` → `tool_use`（工具名 + 输入 JSON 1500 字）。
  - 其他顶层 type（mode、attachment、system、queue-operation 等）忽略。
  - 单条事件文本上限 4000 字。
- 增量：`ingest_state` 记录 offset/size/mtime；文件变小或同尺寸重写 → 从头重读
  并清掉该会话旧事件；只消费完整行（以换行结尾）。
- 幂等：`UNIQUE(session_id, seq)` + REPLACE。

## 6. 压缩（agent B）

把连续事件切成窗口（默认按条数或 token 估算），交给 LLM 输出结构化观察：

```
title, subtitle, narrative, facts[], concepts[], files[], importance(1-5)
```

约束：

- 每条观察必须带 `source_event_range`（如 `session:120-180`），可回溯原文。
- 一条观察对应一个主题；闲聊/寒暄不入库。
- `importance` 由模型给初值，后续可被访问次数修正。
- 同一段事件重复压缩时，先算与既有观察的相似度：
  - 高度相似且无新增信息 → **SKIP**（v1）
  - 有新信息 → **ADD**（v1）
  - 需要修正/冲突的 UPDATE / CONTRADICT → 列入后续（见第 13 节）
- LLM 调用失败不写半成品；记录 `LlmUsage`（输入/输出 token、调用次数）。

## 7. 嵌入（agent B）

优先级：

1. **本地 bge-small-zh-v1.5**（首选）：中文小模型，CPU 可跑，向量维度 512。
   通过 ONNX Runtime 推理；模型文件放 `~/.devdesk/memory-lab/models/`。
   下载走 `socks5h://127.0.0.1:10808`。
2. **哈希向量兜底**（模型不可用/未下载时）：用字符 n-gram 做确定哈希，
   L2 归一化，维度 512。质量明显低于 bge，只保证链路可跑、结果可复现。

存储：float32 小端 BLOB，`embeddings.dim` 记录维度，`model` 记录模型名；
**不同模型向量绝不混算**。相似度用余弦（归一化后即点积）。

## 8. 检索（agent C）

两路召回再融合：

- 全文：FTS5 `MATCH`，trigram 支持中文子串；短查询（<3 字）退回 `LIKE`。
- 向量：查询向量与 `embeddings` 做余弦，取 top N。
- 融合：Reciprocal Rank Fusion（RRF，`1/(60+rank)` 求和）为主，可调权重。
- 过滤：`project`、`kind`、`status='active'`、时间范围。
- 返回 `SearchHit`：`obs_id, score, title, subtitle, snippet, project, created_at, source`。
  `source` 标记命中来源（fts / vector / both）。

## 9. MCP 工具契约（agent C）

MCP（Model Context Protocol）是「模型调用本地工具」的标准协议；这里是
stdio 上的 JSON-RPC，不涉及网络端口。

| 工具 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `memory_search` | `query` 必填；`k=8`、`project`、`kind`、`since` 可选 | `SearchHit[]` | 主检索入口，全文+向量融合 |
| `memory_timeline` | `project`、`limit=50`、`before`、`after` 可选 | `[{obs_id,title,created_at,project}]` | 按时间回看，不分词 |
| `memory_get` | `id` 必填 | 完整观察（narrative/facts/concepts/files/status/来源区间） | 取详情，供模型深挖 |
| `memory_stats` | 无 | `{sessions,events,observations,embeddings,proposals,active_by_project}` | 自检/健康检查 |
| `memory_save` | `title` 必填；`narrative/facts/concepts/files/project/importance` 可选 | `{id}` | 模型主动记一条；只增不改 |

约定：返回值一律 JSON；错误用 `isError=true` + 中文可读消息；不返回 token、
绝对用户目录等敏感信息；单次返回正文有长度上限（默认 8k 字）。

## 10. dream 规则（agent D）

dream = 定期整理。**只写 `dream_proposals`，永远不直接改 observations。**

三类提案：

1. **merge**：同一 project 内两条观察向量余弦 ≥ 0.92 且主题相同 →
   提案合并，payload 记两条 id、目标标题、合并后的叙述草稿。
2. **promote**：`importance >= 4` 或被多次访问/多次复现的概念 →
   提案提升为「核心记忆」（可被 L1 预载，见第 13 节）。
3. **stale**：按记忆池的半衰期判断（见第 13 节参数表）。
   超过对应池衰减期且 `last_accessed` 久远 → 提案 stale，不删除。

提案字段：`kind, status='pending', payload_json, rationale, created_at`；
人工或上层确认后才把 `status` 改为 applied，并记录 `applied_at`。
所有阈值先写成可配置常量，验证阶段用数据校准，不拍脑袋写死。

## 11. 验证方法

验证分三层，缺一不可：

1. **真实数据链路**：用 `~/.claude/projects` 的真实会话跑摄取→压缩→嵌入→检索，
   记录会话数、事件数、观察数、压缩比、LLM 用量；不构造假数据冒充结果。
2. **检索质量**：从真实会话里挑 30～50 个「用户明确问过/说过」的问题做查询集，
   人工标出应命中的观察 id 作为 gold：
   - **Recall@K**：前 K 条里是否包含 gold（目标值待数据出来后定，台账标「待出数字」）。
   - **MRR**：第一条命中的名次取倒数再平均，衡量「相关结果排得够不够前」。
   - 对照三个配置：纯全文 / 纯向量 / 融合。
3. **真实 MCP 协议调用**：起 stdio server，用 JSON-RPC 客户端依次调用五个工具，
   保存请求/响应原文；不能只测 Python 函数直调（协议层出错函数直调看不出来）。

另外保留回归项：重复摄取幂等（第二次新增 0 条）、截断重读、坏行跳过。

## 12. 风险与边界

- 真实会话里有超长工具输出：已按 kind 截断，避免一次吃爆上下文。
- 模型输出不是合法 JSON：压缩层必须重试/丢弃，不能写坏数据。
- 向量模型与哈希向量质量差距大：验证报告必须标注本次用的是哪一种。
- FTS trigram 对 1～2 字查询无效：检索层用 `LIKE` 兜底。
- 单机单用户：不加租户、权限、并发写优化（SQLite WAL 足够）。

## 13. 与 memcpp 技术方案的对齐

memcpp 的目标形态是 **纯 C++、单机单用户、MCP + hook** 的正式记忆组件。
本平台不是它的替代品，而是它的**算法验证前置**：先用 Python 低成本试参数、
拿真实数据出数字，再把验证过的规则搬进 memcpp 的 C++ 实现。两边一一对应如下。

### 13.1 四值判定：SKIP / ADD / UPDATE / CONTRADICT

memcpp 在「写入一条候选记忆」时要对既有记忆做四值判定：

| 判定 | 含义 | 本平台 v1 | 后续 |
|------|------|-----------|------|
| SKIP | 已有等价记忆，不重复写 | 已实现（相似度 + 无新信息） | 阈值用真实数据校准 |
| ADD | 新信息，新增一条 | 已实现 | — |
| UPDATE | 新信息修正了旧记忆 | 未实现 | 建立版本链/`merged_into` 指针 |
| CONTRADICT | 新信息与旧记忆冲突 | 未实现 | 冲突检测 + 保留双方 + 待裁决标记 |

v1 只落地 ADD / SKIP，是为了先把摄取、压缩、检索链路跑通；UPDATE /
CONTRADICT 需要额外的冲突识别与版本管理，列入后续迭代。本平台验证出的
相似度阈值和判定特征，直接作为 memcpp 四值判定的实现输入。

### 13.2 三层召回：L1 静默预载 / L2 工具深挖 / L3 复盘

| 层级 | memcpp 设计 | 本平台 v1 | 后续 |
|------|-------------|-----------|------|
| L1 静默预载 | 每轮自动注入少量高相关记忆，不打断模型 | 未实现 | 用 promote 出的核心记忆做预载候选，验证注入量与噪声 |
| L2 工具深挖 | 模型主动调用 MCP 工具检索详情 | **已实现范围**：MCP 检索工具（memory_search / timeline / get / stats / save） | 调参与融合权重校准 |
| L3 复盘 | 会话结束/周期性回顾，产出新记忆与整理提案 | 未实现 | 复用 dream 提案机制 + 会话摘要 |

v1 只做 L2 的 MCP 检索，是三层里最容易独立验证的一层：它不依赖注入时机，
也不依赖会话生命周期钩子，可以先证明「检索得准不准」。L1 / L3 后续再补。

### 13.3 池与衰减参数

memcpp 把记忆按稳定性分池，用半衰期控制「多久之后算旧」：

| 记忆池 | 内容举例 | 半衰期 | 本平台 dream stale 判定 |
|--------|----------|--------|--------------------------|
| 稳定事实 | 项目代号、架构约定、长期偏好 | 90 天 | `last_accessed` 超过 90 天 → 提案 stale |
| 会话过程 | 某次任务的中间决策、临时上下文 | 7 天 | 超过 7 天 → 提案 stale |
| 易变状态 | 当前分支、临时路径、进行中的待办 | 2 天 | 超过 2 天 → 提案 stale |

本平台 dream 的 stale 判定**按上表参数执行**：先给观察归类到池（用 kind /
concepts 规则），再比较 `last_accessed` 与对应半衰期；只出提案、不自动删。
具体倍数（如「超过 1 个半衰期」还是「2 个」）作为可配置常量，验证阶段用
真实数据定，台账 G 行统一跟踪这些「待出数字」的项。

### 13.4 交付关系

- 本平台产出：可复现的 Python 实现 + 真实数据数字（压缩比、Recall@K、MRR、
  token 用量、各阈值）。
- memcpp 消费：把数字对应的规则与常量翻译成 C++；本平台代码不直接进
  memcpp 运行时。
- 约束：本平台的所有表结构、工具契约变更都要先改本文档，再改代码。