# memcpp 本地向量模型候选核实（Qwen3-Embedding / bge-m3 / 2025 扫描）

- 日期：2026-10-27；分支 feat/memcpp @ 750dc9c9；检索均为当日联网一手来源。
- 背景：记忆面板已拍板"向量化走本机"。已验证基线 bge-small-zh-v1.5（2023 年模型，Q8_0 GGUF 25MiB，llama.cpp 实测冷启动 92ms、RSS 32MB、512 维）。用户嫌老，本文核实两个更新候选并扫描有无更优遗漏。
- 硬约束：CPU 推理、llama.cpp 可跑（GGUF 量化）、单机体积可控（<1.5GB 量级）、中文为主、许可证允许随应用分发。
- 约定：GGUF = 一种把模型压缩打包、llama.cpp 能直接读的文件格式；Q8_0 / Q4_K_M = 不同压缩档位，数字越小文件越小、精度损失略增；MTEB / C-MTEB = 公开的中英文检索能力榜单（分数越高越好）；MRL = "输出向量维度可调"能力，维度调小可省存储且精度损失可控。

## 1. 结论（先行）

1. **首选进评测：Qwen3-Embedding-0.6B（Q8_0，639MB）**。2025 年 6 月发布、Apache-2.0（可随应用分发）、官方 GGUF、中文成绩 C-MTEB 66.33 显著高于基线 57.82；还支持维度可调（32~1024），可把向量库存压小。
2. **次选进评测：bge-m3（Q8_0，634MB）**。MIT 许可、8192 长文本、中文检索约 65.14（T2Ranking）；但发布于 2024 年 2 月，只比基线新一年，且 llama.cpp 下只能用它的 dense（普通向量）输出，稀疏检索等附加能力用不上。
3. **2025 年无更优遗漏**：扫过 Conan v1/v2、gte-multilingual、jina v3/v4、MiniCPM-Embedding、EmbeddingGemma——或禁止商用（CC-BY-NC）、或没有 GGUF / llama.cpp 不支持、或中文不强。详见第 4 节，逐条给出排除原因。
4. **代价要心里有数**：两个候选的运行内存约 700~800MB（估算），是基线 32MB 实测值的 20 倍以上；体积从 25MiB 涨到约 639MB。换来的收益是中文检索分 +8~9 分（57.82 → 66.33，满分制相同）。是否值得，交给冻结集评测定。
5. 体积可再压：官方不出 Q4 档，Qwen3 有社区 Q4_K_M；bge-m3 有第三方 Q4_K_M（438MB）。但**查不到公开的量化前后检索质量对比数字**，不建议首轮就押 Q4。

## 2. 候选一：Qwen/Qwen3-Embedding-0.6B（通义千问，2025-06-05 发布）

- **发布时间**：2025 年 6 月 5 日（官方仓库成绩表标注"as of June 5, 2025"；论文 arXiv:2506.05176）。来源：https://github.com/QwenLM/Qwen3-Embedding
- **许可证**：Apache-2.0（宽松开源许可，允许商用与随软件分发，仅需保留声明）。来源：https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF
- **成绩（发布时官方自报，0.6B 档）**：
  - MTEB 多语言榜 64.33 分（同表 bge-m3 为 59.56；8B 大哥 70.58 当日排全榜第一）。
  - C-MTEB（中文榜）66.33 分（8B 为 73.84、4B 为 72.27）。
  - 来源：https://github.com/QwenLM/Qwen3-Embedding （官方 README 成绩表）
- **维度与 MRL**：最高 1024 维，官方明确"支持用户自定义输出维度 32~1024"。原文："supports user-defined output dimensions ranging from 32 to 1024"。来源：https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF
- **上下文**：32K tokens。
- **GGUF 可用性**：官方仓库仅两档——`Qwen3-Embedding-0.6B-Q8_0.gguf` 639,150,592 字节（639MB）、f16 1,197,629,632 字节（1.2GB）。**官方无 Q4 档**；社区有 Q4_K_M（如 https://huggingface.co/Jgriosanz/Qwen3-Embedding-0.6B-Q4_K_M-GGUF ，非官方、质量数字未证实）。文件大小经 HF API 逐字节核实：https://huggingface.co/api/models/Qwen/Qwen3-Embedding-0.6B-GGUF?blobs=true
- **llama.cpp 支持状态**：官方模型卡直接给出命令，属一线支持。原文命令：`llama-server -m model.gguf --embedding --pooling last -ub 8192`；Ollama/LM Studio 亦收录。注意两点：池化必须用 `--pooling last`；查询侧要加 instruction 前缀（官方说加对可提升 1~5%，不加可能掉分）。
- **内存足迹**：未实测；按基线倍率（实测 RSS 32MB / 文件 25MiB ≈ 1.23 倍）外推 Q8_0 约 780MB。**估算值，进评测时实测覆盖**。

## 3. 候选二：BAAI/bge-m3（北京智源，2024-02 发布）

- **发布时间**：2024 年 2 月（论文 arXiv:2402.03216；模型卡动态始于 2024-02）。注意：**它是 2024 年模型，不是 2025**，比基线（2023）只新一年。来源：https://huggingface.co/BAAI/bge-m3
- **许可证**：MIT。来源：https://huggingface.co/BAAI/bge-m3/raw/main/README.md （YAML `license: mit`）
- **成绩**：
  - MTEB 多语言 59.56（出处为 Qwen 官方对比表，属竞品转引）：https://github.com/QwenLM/Qwen3-Embedding
  - 中文检索 T2Ranking 约 65.14、C-MTEB 均值约 63.7——来自搜索综合与第三方评测文，**官方模型卡的 C-MTEB 数字只在图片里，未取到一手文本，标注"未直接核实"**。
  - 参数量约 5.7 亿（按官方 F16 文件 1,157,671,200 字节 ÷ 2 字节/参数推算，非官方声明）。
- **维度/上下文**：1024 维、8192 tokens 上下文、一个模型同时给 dense/sparse/ColBERT 三种向量（后两种 llama.cpp 不输出）。
- **GGUF 可用性**：BAAI 官方不出 GGUF；社区 gpustack/bge-m3-GGUF（自标 MIT）全档齐全，Q8_0 634,553,760 字节（634MB）、Q4_K_M 437,778,496 字节（438MB）。文件大小经 HF API 核实：https://huggingface.co/api/models/gpustack/bge-m3-GGUF?blobs=true
- **llama.cpp 支持状态**：成熟。Ollama 内置 bge-m3（底层即 llama.cpp）；社区确认 llama.cpp 仅暴露 dense 输出。来源：https://ollama.com/library/bge-m3
- **内存足迹**：未实测，按同法外推 Q8_0 约 775MB、Q4_K_M 约 535MB。估算值。
- **与基线比提升幅度**：中文均值 57.82 → 约 63.7（+5.9，第三方口径）；检索单项 61.77 → 约 65.14（+3.4）；上下文 512 → 8192。

## 4. 2025 年新模型扫描（有无更优遗漏）——结论：没有

按硬约束逐条排查（检索日期 2026-10-27）：

| 候选 | 排除/保留原因 | 关键证据 |
|---|---|---|
| Conan-embedding-v1（腾讯） | **排除：禁商用**。C-MTEB 72.62 分数虽高，许可证 CC-BY-NC-4.0（非商业），与"随应用分发"冲突 | https://huggingface.co/TencentBAC/Conan-embedding-v1/raw/main/README.md （`license: cc-by-nc-4.0`） |
| Conan-embedding-v2 | **排除：无 GGUF**。Apache-2.0、C-MTEB 74.24 均最优，但 1.48B 参数、3584 维，未查到任何 llama.cpp GGUF 转换（官方与社区皆无） | https://huggingface.co/TencentBAC/Conan-embedding-v2/raw/main/README.md |
| gte-multilingual-base（阿里） | **排除：无 GGUF**。自定义 "NewModel" 架构（带旋转位置编码的 XLM-R 变体），社区确认 "GGUF or CT2 versions are unavailable"，只走 ONNX 路线 | https://github.com/huggingface/transformers.js/issues/891 ；https://huggingface.co/keisuke-miyako/gte-multilingual-base-onnx-int8 |
| jina-embeddings-v3 / v4 | **排除：禁商用**。v3 许可证 CC-BY-NC-4.0；v4 约 3.8B 体积也超 | https://huggingface.co/jinaai/jina-embeddings-v3/raw/main/README.md |
| MiniCPM-Embedding / -Light（OpenBMB，0.4B 轻量版） | **排除：无 GGUF**。官方只发 transformers 路线，llama.cpp 官方转换脚本未收录 | https://huggingface.co/openbmb/MiniCPM-Embedding |
| EmbeddingGemma-300m（Google，2025-09） | **不优于 Qwen3-0.6B**。300M 级、量化后 <200MB 很轻，但许可证是 Gemma 自定义条款（非 MIT/Apache，附 Google 使用政策）；主打多语言与英文榜单，无中文榜单优势 | https://huggingface.co/google/embeddinggemma-300m ；https://arxiv.org/abs/2509.20354 |

小结：同时满足"中文强 + CPU + llama.cpp GGUF + 可分发 + 2025 年新"的，目前只有 Qwen3-Embedding-0.6B 一个，没有第三个更值得入选的。

## 5. 对比表与实测建议

| 项 | 基线 bge-small-zh-v1.5 | bge-m3 | Qwen3-Embedding-0.6B |
|---|---|---|---|
| 发布 | 2023 | 2024-02 | 2025-06-05 |
| 参数量 | 0.24 亿 | 约 5.7 亿（推算） | 6 亿 |
| 文件体积 Q8_0 | 25MiB | 634MB | 639MB |
| 内存足迹（RSS） | 32MB（实测） | 约 775MB（估算） | 约 780MB（估算） |
| 中文成绩 | C-MTEB 57.82（官方 README） | 均值约 63.7 / 检索 65.14（第三方，未核实一手） | C-MTEB 66.33（官方自报） |
| 上下文 | 512 | 8192 | 32K |
| 维度 | 512 固定 | 1024 固定 | 1024，MRL 可调 32~1024 |
| 许可证 | MIT | MIT | Apache-2.0 |
| llama.cpp 成熟度 | 已实测跑通 | 成熟（Ollama 内置；仅 dense） | 官方一线支持（`--pooling last`） |
| 分数口径 | 官方 | 竞品表+第三方 | 官方自报 |

基线中文成绩出处：https://huggingface.co/BAAI/bge-small-zh-v1.5/raw/main/README.md （表内 Avg 57.82 / Retrieval 61.77，与既有 license 清单文档同源）。

**实测建议（进冻结集评测的前二 + 基线三跑）**：
1. Qwen3-Embedding-0.6B Q8_0（官方 GGUF，639MB）——查询侧记得加 instruction、`--pooling last`；评测时顺带实测 MRL 降到 512 维后的检索损失（能降则向量库存减半）。
2. bge-m3 Q8_0（gpustack 转换，634MB）——对照组，检验 Qwen 官方自报分数是否在水；若我们的语料常超 512 tokens，它的 8192 上下文是独有优势。
3. 基线 bge-small-zh-v1.5 Q8_0 保留作参照（32MB 兜底档，若新模型内存超预算可退回）。
4. **Q4 档位**：Qwen3 官方无 Q4；bge-m3 有社区 Q4_K_M（438MB）。所有候选都查不到"量化前后检索分差"的公开数字（社区共识是 embedding 比 LLM 更怕量化，Qdrant 讨论：https://github.com/orgs/qdrant/discussions/6772 ），首轮评测不建议押 Q4；若评测后内存吃紧，再把 Q4_K_M 作为加测项并自测分差。

## 引用清单（来源等级 / 检索日期均为 2026-10-27）

- 官方仓库 README（成绩表、发布时间）：https://github.com/QwenLM/Qwen3-Embedding 【官方，一手】
- Qwen3-Embedding-0.6B-GGUF 模型卡（许可证、档位、llama.cpp 命令、MRL）：https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF 【官方】
- Qwen 官方 GGUF 文件逐字节大小（HF API）：https://huggingface.co/api/models/Qwen/Qwen3-Embedding-0.6B-GGUF?blobs=true 【官方数据】
- bge-m3 模型卡 raw（MIT、1024 维、8192）：https://huggingface.co/BAAI/bge-m3/raw/main/README.md 【官方】
- bge-m3 全档 GGUF 逐字节大小：https://huggingface.co/api/models/gpustack/bge-m3-GGUF?blobs=true 【社区转换（MIT 自标）】
- bge-small-zh-v1.5 C-MTEB 成绩表：https://huggingface.co/BAAI/bge-small-zh-v1.5/raw/main/README.md 【官方】
- Conan-embedding-v1 许可证（CC-BY-NC）：https://huggingface.co/TencentBAC/Conan-embedding-v1/raw/main/README.md 【官方】
- Conan-embedding-v2 模型卡（Apache-2.0、C-MTEB 74.24、1.48B）：https://huggingface.co/TencentBAC/Conan-embedding-v2/raw/main/README.md 【官方】
- gte-multilingual-base 无 GGUF 佐证：https://github.com/huggingface/transformers.js/issues/891 【官方 issue】
- jina v3 许可证（CC-BY-NC）：https://huggingface.co/jinaai/jina-embeddings-v3/raw/main/README.md 【官方】
- EmbeddingGemma 论文与模型卡：https://arxiv.org/abs/2509.20354 ；https://huggingface.co/google/embeddinggemma-300m 【论文/官方】
- bge-m3 中文分 65.14/63.7（第三方综合，未取到一手表格，存疑保留）：https://agentset.ai/embeddings/baaibge-m3 【技术博客，未证实】
- 量化敏感性社区讨论：https://github.com/orgs/qdrant/discussions/6772 【论坛，未证实】
