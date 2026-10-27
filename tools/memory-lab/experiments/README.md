# memory-lab 评测资产（冻结）

本目录保存 2026-10-27 可行性报告修订时使用的可重放证据。**评测只读冻结文件，不生成数据。**

## 目录

| 路径 | 说明 |
|---|---|
| `observations.json` | 27 条去重记忆（id 0..26），来自 `raw/mem_obs_batch.json`（31 条抽取产出，按标题去重） |
| `queries.json` | 15 条冻结查询；`target_id` 为出题目标，`gold_ids` 为人工标注的语义等价答案组 |
| `eval_retrieval.py` | 真实 SQLite FTS5（中文双字 + `unicode61`）、trigram 对照、向量、混合（RRF k=60）评测 |
| `results/results.json` / `.md` | 指标、逐查询排名、短查询探针、耗时 |
| `raw/eval_run.json` / `eval_stdout.txt` | 本次评测的原始逐查询输出 |
| `raw/mem_obs_batch.json` | 抽取实验原始产出（31 条，未去重） |
| `raw/query_generation.json`、`raw/queries_llm_only.json` | 查询集的一次性生成响应与未加工结果 |
| `freeze_datasets.py` | 从 raw 产出生成 `observations.json` / `queries.json`（仅在重建数据集时执行） |
| `generate_queries.py` | 一次性调用网关生成查询；评测阶段不调用 LLM |
| `archive/` | 修订前在 `%TEMP%` 运行的实验脚本，原样归档；脚本内的临时路径**不保证可重放**，仅作 provenance |

## 重放评测

```bash
cd tools/memory-lab/experiments
D:/software/venvs/memory-lab/Scripts/python.exe -I eval_retrieval.py
```

依赖：fastembed（`BAAI/bge-small-zh-v1.5`）装在 `D:/software/venvs/memory-lab`；SQLite 需带 FTS5（本机 3.50.4）。

## 冻结规则

- `observations.json` 与 `queries.json` 一经提交即固定；改数据集必须同时更新 `raw/` 与修订记录，不能只改评测脚本。
- 查询由目标记忆反推生成（`generate_queries.py` 的提示词要求不照抄标题），天然对检索有利；这批评测只能证明机制，不能作为质量结论。
- 27 条里有大量同义重复，命中 `gold_ids` 任一即算正确；否则重复条目会把 MRR 系统性压低。

## 已知限制

- FTS5 主路径是“中文重叠双字 token + unicode61”，不是报告早期推荐的 trigram；trigram 在本目录仅作对照。
- 短查询兜底 `LIKE` 在 27 条规模下测过，未在 1 万条规模下测延迟。
- 向量指标依赖 bge-small-zh-v1.5 的量化/版本；换模型必须重跑并记录。
- hook 全链路、脱敏层、记忆污染红队测试不在本目录范围内（见报告 §5 门槛与 §7 未验证清单）。