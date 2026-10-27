# memory-lab 夜间执行台账（2026-10-26）

- 总目标：用 Python 在 `tools/memory-lab/` 验证记忆算法链路，为 memcpp
  （纯 C++ / 单机单用户 / MCP + hook）提供可量化参数。
- 设计文档：`docs/superpowers/specs/2026-10-26-memory-lab-design.md`
- 状态图例：⬜ 未开始　🔵 进行中　✅ 已完成
- 纪律：每行「证据」必须是可复现的命令/输出/文件路径，不写估算；
  状态推进由负责人本人改为 🔵/✅，其他人不改他人行。

| 任务 | 负责人 | 状态 | 产物 | 证据 |
|------|--------|------|------|------|
| A1 基础层·契约冻结与骨架（models/config） | mem/a-foundation | ✅ | `tools/memory-lab/memory_lab/{__init__,models,config}.py`、`run.py`、`README.md` | 21 条单元测试全过；`python -m unittest discover -s tests -v` |
| A2 基础层·DB schema 与读写 | mem/a-foundation | ✅ | `memory_lab/db.py`（7 张常规表 + FTS5 虚表 + 3 个同步触发器）、`tests/test_db.py` | FTS5 `tokenize='trigram'` 在 SQLite 3.50.4 实测可用；观察 list↔text roundtrip 与触发器增删改测试全过 |
| A3 基础层·会话摄取 | mem/a-foundation | ✅ | `memory_lab/ingest.py`、`tests/test_ingest.py` | 真实数据 `ingest --project D--project-gui --limit 2`：1412 条事件、坏行 0；同命令二次执行新增 0 条（幂等） |
| B LLM + embedding + 压缩 | 并行 agent（mem/b 等） | ⬜ | `compress.py`、`embed.py`、`llm.py`；observations / embeddings 落库；LlmUsage 统计 | 待填：真实会话压缩比、token 用量、模型/兜底向量标记 |
| C 检索 + MCP + CLI | 并行 agent | ⬜ | `search.py`、`mcp_server.py`；5 个 memory_* 工具；CLI search | 待填：stdio JSON-RPC 请求/响应原文；纯全文/纯向量/融合三组结果 |
| D dream + 验证 | 并行 agent | ⬜ | `dream.py`、验证脚本与报告；dream_proposals 数据 | 待填：merge/promote/stale 提案数与抽样；Recall@K / MRR（30~50 查询集） |
| E 集成收口 | 主会话 | ⬜ | 端到端跑通记录、参数定稿、README 更新 | 待填：真实数据全链路一次跑通；二次摄取幂等；所有工具可调用 |
| F 双独立审计 | 独立审计 agent | ⬜ | 两份独立审计报告 + 问题清单 | 待填：审计结论、修复闭环、与设计文档一致性核对 |
| G 与 memcpp 参数对齐（待出数字） | 待定 | 🔵 | 四值判定阈值、L1/L2/L3 分层验证结论、三池半衰期（90/7/2 天）实测参数 | 见 `docs/superpowers/research/2026-10-27-memcpp-feasibility.md`：L2 检索 Recall@5=1.00、混合 MRR=0.90（27 条记忆、15 题，小样本）；SKIP/ADD 阈值与 stale 倍数未标定，方案已建议写入侧只 ADD + 后台提案 |
| H 可行性论证文档 | 文档 agent | ✅ | `docs/superpowers/research/2026-10-27-memcpp-feasibility.md` | 总判定「有条件可行」；7 项方案修改 + 7 条证伪门槛 + 14 项未验证 |

## 依赖与交接

- A1 → A2 → A3 已完成；B 依赖 A 的 `models.py` 契约与 `db.py` 表结构。
- C 依赖 B 的 observations + embeddings；MCP 工具签名以设计文档第 9 节为准。
- D 依赖 B/C；stale 参数以设计文档 13.3 节为准。
- E 依赖 B/C/D 全部 ✅；F 在 E 之后。
- G 的「待出数字」来自 B/C/D/E 的实测，不在代码里写死猜测值。