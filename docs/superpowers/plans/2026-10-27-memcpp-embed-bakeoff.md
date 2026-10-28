# memcpp 本地向量模型同场跑分（embed bakeoff）

- 日期：2026-10-27（qwen3 受控补跑 2026-10-28）
- 结论先行：**当前 27 记忆规模下基线 bge-small-zh-v1.5 够用（29/30 命中、RSS 49MB、p50 9.4ms），不必上大的；难集要满分再升 Qwen3，且部署必须显式 `-c` 限上下文，否则 RSS 飙到 4.1GB。bge-m3 无独立优势，不推荐。**
- 口径：统一 llama-server（OpenAI 兼容 `/v1/embeddings`）= 未来 telemem 生产形态，跑分环境即生产环境。冻结集复用 `tools/memory-lab/experiments/`（27 记忆 / 15 查询，只读），另加 10 条难查询（构造原则见 `hard_queries.json` meta）。排序为纯本地余弦，零 API。
- 机器：Windows x64 CPU，llama-server 0.7.0-dev（build 11926, d78f40d9d，预编译 CPU 包）；延迟为 24 个单条请求样本（任务要求 ≥10），RSS 取 WorkingSet（与 S1 同口径）。
- 复现：`spike/.venv/Scripts/python.exe tools/memcpp-integration/spike/run_bakeoff.py <bge-small|bge-m3|qwen3>`；原始结果 JSON 在 `spike/.bakeoff/results/`（不入库）。

## 对比总表（各模型按模型卡默认用法）

| 模型 | 维度 | 原集 R@5 / MRR | 难集 R@5 / MRR | RSS 稳态 MB | 延迟 p50 / p95 ms | Q8_0 体积 MB |
|---|---|---|---|---|---|---|
| bge-small-zh-v1.5 Q8_0（基线） | 512 | 1.0 / 0.9667 | 0.9 / 0.7167 | 49 | 9.4 / 11.2 | 25 |
| bge-m3 Q8_0 | 1024 | 1.0 / 0.9667 | 0.9 / 0.8433 | 515 | 73.1 / 87.2 | 605 |
| Qwen3-Embedding-0.6B Q8_0（模型卡默认旗标，ctx 自适应 32K×4） | 1024 | 1.0 / 0.9667 | 1.0 / 0.95 | 4133 | 158.3 / 180.8 | 610 |
| Qwen3-Embedding-0.6B Q8_0（+`-c 8192`，部署推荐形态） | 1024 | 1.0 / 0.9667 | 1.0 / 0.95 | 1439 | 140.5 / 168.9 | 610 |

辅助数字：加载耗时 bge-small 0.51s / bge-m3 2.55s / qwen3 2.53s；27 条文档灌入 bge-small 0.30s / bge-m3 4.64s / qwen3 5.80s。

## 推荐

- **现在：基线 bge-small-zh-v1.5。** 原集 15/15 全中，难集 9/10；RSS 49MB 是另两家的 1/10 与 1/30，p50 9.4ms，体积 25MB——嵌入常驻桌面应用的唯一"无感"档。唯一难集 miss（hq03，口语化"像插件一样随时加新工具"，P1/P3 型措辞距离）说明的是"换措辞仍可能掉"，而 hq03 恰是可通过查询改写/分块手法补的类别，不构成换模型的理由。
- **升档条件：记忆规模上量、或间接表达类查询占比高、且内存预算能容纳 ~1.5GB 常驻 → Qwen3-Embedding-0.6B**。它是三家中唯一难集满分（含 hq03），C-MTEB 66.33 有公开榜背书，且 Apache-2.0 可随应用分发、支持 MRL 降维。
- **不推荐 bge-m3**：质量不超 Qwen3（难集同为 0.9，MRR 0.843 低于 Qwen3 0.95），成本又接近 Qwen3（RSS/延迟同量级）；它的独有优势（8192 长文本、稀疏检索）在 llama.cpp 下用不上稀疏、我们的记忆条目也远短于 512 tokens，当前无用武之地。

## 每模型正确用法备注（跑分公平性的关键）

- **bge-small-zh-v1.5**：`--pooling cls`；查询侧加官方 s2p 指令前缀「为这个句子生成表示以用于检索相关文章：」，文档侧不加。新版 llama-server 自适应上下文 512，无需显式 `-c`。
- **bge-m3**：`--pooling cls`（GGUF 元数据 pooling_type=2），**不**加查询指令；社区 gpustack 转换、仅 dense 可用。自适应上下文 8192，无需显式 `-c`。
- **Qwen3-Embedding-0.6B**：`--pooling last` + `-ub 8192`（官方模型卡原文命令）；查询侧必须加英文 instruction 前缀（`Instruct: ...\nQuery: `，官方称缺失可掉 1~5%）。**新版 llama-server 会按 GGUF 元数据把上下文自适应到 32K×4 slots → RSS 4.1GB；部署必须显式 `-c 8192`（本跑分实测 1.44GB）**。质量在三组旗标（`-c 8192`、`-c 8192 -ub 8192`、模型卡默认）下完全一致（R@5/MRR 零差异），旗标只影响内存与吞吐。

## 遗留 / 待办

- MRL 降维实测（Qwen3 1024→512 维后检索损失）未做，qwen3 正式采用前补一次，能降则向量库存减半。
- 难集只有 10 条且同源构造，结论在更大规模记忆（数百条）下需复测再定升档。
- hq03 类（口语化/间接表达）的召回缺口优先用查询改写手法补，而非默认换大模型。
