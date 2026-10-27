# TeleMem 参照实现拆解（用户指定参照）

- 调研日期：2026-10-27（检索当日，全部证据为当日抓取）；同日二稿修订：按用户已定集成模式"DevDesk pip 安装并托管其配置与进程"更新 §4 进程模型契合度（1→2）与 §5 结论
- 对象：<https://github.com/TeleAI-UAGI/telemem>（任务书原写 `teleiai-uagi` 多一个 i 导致 404，正确组织名 **TeleAI-UAGI**，已用 GitHub API 确认存在、非空壳、非改名残留）
- 与 mem0-mcp 参照文档的关系：`docs/superpowers/research/2026-10-27-memcpp-mem0-mcp-reference.md` 已落盘（commit `23524087`，先于本文），本文 §5 以其为对照基线，ADD-only 等表述已核对一致。
- 结论一句话：**能**——按已定集成模式（DevDesk 用 pip 安装它、托管其配置与进程）落地可行，纯本地档可全离线；它仍是 Python 进程、不进 C++ 进程内，脱敏前置、hook 注入壳、逐条预算格式化三件事仍要 memcpp 自建。

硬需求对照物（只读引用，本文不改其结论）：
- `docs/superpowers/research/2026-10-27-memcpp-feasibility.md` §5（风险门槛表）与 §8（三件拍板事项：脱敏前置 / 注入预算 ≤5 条 ≤500 字 / 写入只 ADD / 无人值守）
- `docs/superpowers/specs/2026-10-27-memcpp-p0-core-spec.md`（自研基线：SQLite+FTS5 双字分词 / MCP recall / 尾读采集）

---

## 1. 它是什么

**一段话**：TeleMem 是一个 Python 写的"AI 助手长期记忆库"，定位是 mem0 的即插即用替换品（`import telemem as mem0`，接口参数与返回格式和 mem0 一致）。它把对话自动抽取成事实条目存进本地向量索引，支持按语义查询，官方还带一个 MCP 服务器（8 个工具），可被 Claude Code / Claude Desktop / Cursor 直接挂载。同组织有 arXiv 技术报告（arXiv:2601.06037）。

**架构一图流**（依据 README"Core Functions"与源码 `telemem/mem0.py`）：

```
对话消息 ──> LLM 抽取（全局事件摘要 + 按角色摘要，中文提示词）
              │        （infer=False 时不调 LLM，原文直存）
              v
        向量化（embedding 模型）+ 相似旧记忆检索（阈值 0.95）
              v
        写缓冲区（默认攒 64 条）
              v
        LLM 语义聚类融合（相似记忆合并去重，失败则原样写入）
              v
        FAISS 向量索引 + JSON 元数据 双写（本地文件 ./faiss_db/）
        + history.db（SQLite，记录每条记忆的 ADD/UPDATE/DELETE 历史）

查询 ──> 向量化 ──> FAISS 检索（毫秒级，官方口径）──> 可选重排 ──> 融合成一段文字返回
```

| 维度 | 事实 | 证据 |
|---|---|---|
| 语言/运行时 | Python，要求 3.10~3.12 | `pyproject.toml`：`requires-python = ">=3.10"` |
| 存储后端 | **纯本地**：FAISS 向量索引 + JSON 元数据文件 + SQLite 历史（`db/history.db`）；无云存储 | README"Data Storage"节；`config.yaml`：`history_db_path: db/history.db` |
| embedding 提供方 | 任意 OpenAI 兼容接口；默认配置要求 `OPENAI_API_KEY`；**全本地档走 Ollama**（`nomic-embed-text`，768 维） | `config/config.ollama.yaml`："fully local, no cloud, no API key" |
| LLM（抽取用） | 任意 OpenAI 兼容接口；官方预置 Ollama（本地 `qwen3:8b`）/ DeepSeek / Kimi / MiniMax 配置 | `config/` 四个 yaml；README"More LLM Providers"表 |
| 是否要求网络与 API key | 非必须：`config.ollama.yaml` 全本地、无 key；默认配置则需要 OpenAI key | 同上 |
| 部署形态 | ① Python 库（`import telemem as mem0`）；② MCP 服务器（`telemem-mcp`，stdio 或本地 HTTP :8421）；③ `uvx telemem` 免安装运行。**不是独立云服务，不是 C++ 可嵌入库** | README"MCP Server"节；`docs/MCP.md` |
| 多模态 | 视频 → 抽帧 → 生成字幕说明 → 向量库，另带 `add_mm`/`search_mm` 两个视频问答接口（需额外装 `telemem[video]`） | README"Multimodal Extensions" |
| 硬依赖 | `mem0ai>=2.0,<2.1`（**钉死小版本**，且调用 mem0 私有函数）+ faiss-cpu + numpy + openai + pydantic + pyyaml + tqdm + mcp SDK，共 8 个直接依赖 | `pyproject.toml` dependencies 与其注释 |

**隐私面**：自身无遥测；其依赖 mem0 自带的匿名使用统计（PostHog）被 TeleMem **默认关闭**（`import telemem` 时强制 `MEM0_TELEMETRY=False`）。README"Telemetry & Privacy"节。

## 2. 接口面全清单

### 2.1 MCP 工具（8 个，源码 `telemem/mcp/server.py:156-383`）

| 工具 | 入参 | 出参 | 备注 |
|---|---|---|---|
| `add_memory` | `text` 或 `messages`（二选一）、`user_id`/`agent_id`/`run_id` 作用域、`metadata`、`infer`（默认 true=LLM 抽取，false=原文直存） | `{"status":"stored","scope":{...}}` | 什么都不传返回 `{"error":"invalid_arguments"}` |
| `search_memories` | `query`（必填）、作用域、`limit`（默认 20）、`threshold` | **融合成的一段文字**（`_fuse_search_results` 把多条拼接，无逐条 ID/日期/分数） | 与我们 P0 的"逐条带日期+来源 ID"格式不兼容，需绕过此工具 |
| `get_memories` | 作用域、`limit` | 记忆列表（含 memory_id） | 可列 `user_id="events"` 的共享事件记忆 |
| `get_memory` | `memory_id` | 单条记忆；不存在返回 `{"error":"not_found"}` | |
| `update_memory` | `memory_id`、`text` | 覆写指定条目 | 人工操作用 |
| `delete_memory` | `memory_id` | 删单条 | 人工操作用 |
| `delete_all_memories` | 必须显式给作用域 | 清空该作用域 | **拒绝无作用域的清空**（`missing_scope` 错误） |
| `memory_history` | `memory_id` | 该条的 ADD/UPDATE/DELETE 历史轨迹 | |

**错误处理**（`server.py:103-121`）：所有工具异常统一捕获，返回结构化 `{"error": 类型名, "detail": 消息}`，服务器进程不崩；`Memory` 实例**首次调用时才创建**（lazy），配置错误也变成工具级错误而不是启动失败——这与我们 P0 规格 §4.2"工具层故障 `isError:true` + 进程不退"的口径一致。
**协议版本**：官方 MCP Python SDK v2，实现 2026-07-28 版规范，兼容老客户端；每个工具带 title / 只读·破坏性标注 / 结构化输出（`docs/MCP.md`）。

### 2.2 Python 库接口（mem0 兼容）

`add()` / `search()` / `add_batch()` / `add_mm()` / `search_mm()`，外加继承自 mem0 的 `get` / `get_all` / `update` / `delete` / `delete_all` / `history`。`add()` 签名与参数逐条说明见 README"Core Functions"。

### 2.3 与 MCP recall 生态的兼容度

**高**：本身就是标准 MCP 服务器，`claude mcp add telemem ... -- telemem-mcp` 一行接入（`docs/MCP.md`"Claude Code"节），上 MCP 官方注册表（v1.7.1 起，README 更新日志 2026-06-12 条目）。对 memcpp 而言：我们 P0 自研的 `recall` 工具可以直接被 telemem-mcp 替代；但 hook 注入通道（`UserPromptSubmit` 同步预载）它没有，需要 memcpp 自己的壳去调它。

## 3. 记忆生命周期

| 环节 | TeleMem 的做法 | 证据 |
|---|---|---|
| 写入 | 默认 LLM 自动抽取：按"全局事件 + 各角色视角"用**中文提示词**抽事实（提示词面向角色扮演/对话场景：人物关系、情节、角色特征、物品地点）；`infer=False` 原文直存不调 LLM；`prompt` 参数可整体替换抽取提示词 | `telemem/utils.py:95-127`（`get_person_prompt`）；`mem0.py:344-361`（`_add_raw_memories`） |
| 更新/删除语义 | **自动管线只增不改不删**：`_flush_buffer` 与 `_sync_memory_to_vector_store` 里所有落库都是 `_create_memory`（ADD），全文件无一处调用 mem0 的 update/delete；LLM 融合提示词名义叫"增删操作"，实际只是决定"新记忆哪些值得写入"，**旧的相似记忆既不覆写也不删除**，会留在库里 | `telemem/mem0.py:119-217,553-589`；融合提示词 `utils.py:129-152`。这与 mem0 主线 2026-04 起的 ADD-only 转向一致（本仓 `2026-10-27-memcpp-feasibility.md` §4.1 已考证 mem0 commit `2c81af3`），也与我们已拍板的"写入只 ADD"**同构** |
| 检索 | **纯向量**（FAISS），无全文检索；查询侧也走 embedding；支持作用域过滤、相似度阈值、可选重排；共享事件库（伪用户 `events`）永远随查 | `mem0.py:591-690`（search） |
| 去重 | 两层：向量余弦相似 ≥0.95 聚簇 + LLM 融合决定去留；**融合失败时兜底原样写入**（不丢数据但可能重复） | `mem0.py:87-117`（`_cluster_memories_by_embedding`）、`:186-204`（fallback） |
| 冲突处理 | 无显式冲突消解；靠融合提示词"重复/无价值的去除"软处理，且旧条目不删，冲突事实可能并存 | 同上 |
| 人审/提案环节 | **没有**。全自动写入，人工只有事后 `update_memory`/`delete_memory`/`memory_history` 三个 MCP 工具可追溯可修，无"提案待审"状态机 | `server.py` 全部工具面 |

## 4. memcpp 适配度评分（0-2 分制，满分 14）

| 维度 | 分 | 理由（含量化） |
|---|---|---|
| 本地离线可用 | **2** | Ollama 官方配置全本地（本地 LLM 抽取 + 本地 embedding + 本地 FAISS/SQLite），无遥测默认关。唯一保留意见：本地抽取质量未由我们验证（对上 feasibility §5 门槛 7） |
| 依赖重量 | **1** | 8 个直接运行时依赖；mem0 传递依赖拉进 qdrant-client、sqlalchemy、posthog、protobuf、pytz（PyPI 元数据核实）；轮子体积：faiss-cpu PyPI cp312 各轮 13.7~18.9 MB（量级十余 MB）、numpy 12 MB、sqlalchemy 2.3 MB，纯 Python 包若干——Python 依赖安装后估计 120~150 MB，**另需 Python 3.10+ 解释器**（约 25~100 MB，估）。对照"单 exe 红线"已超，但方向已改为"不强自研单 exe"，故给 1 不给 0。运行时内存未实测（FAISS 平面索引几千条为 MB 级，属推断） |
| 脱敏兼容（外发什么数据） | **0** | 无任何脱敏/正则过滤层；`infer=True` 把**原始对话全文**拼进提示词发给所配 LLM 端点（`mem0.py:479-497`），查询原文发给 embedding 端点。走本地 Ollama 档数据不出机器（可满足"纯本地"档）；走云端档则触碰 feasibility §8.1 红线——脱敏必须由 memcpp 在调 `add()` **之前**自建 |
| 注入通道兼容（hook 同步预载） | **1** | 无 Claude Code hook 集成，但常驻本地 MCP HTTP（127.0.0.1:8421）可被 memcpp 的 C++ hook 壳以 JSON-RPC 调用，进程模型成立；`search_memories` 返回融合段落、无逐条日期/来源 ID，不满足"≤5 条、每条一行带日期与来源"预算格式，需改调 Python API 原始结果或换 `get_memories`。**端到端延迟未实测**：检索含一次 embedding 调用（本地 Ollama 十几毫秒量级，推断），对 feasibility §5 门槛 5（p95<500ms）大概率可过但无证据 |
| 许可证 | **2** | Apache-2.0（仓库 LICENSE 与 `pyproject.toml` 双证），可商用可分发，无传染性；依赖链 mem0ai 同为 Apache-2.0 |
| 活跃度 | **2** | 494 星 / 37 fork（2026-10-26 API 抓取）；2025-12-05 建仓至今 189 个 commit，近三月 7+9+10 个，**最新 release v1.10.1 于 2026-10-23**（4 天前）；8 位贡献者，主力 dell-zhang 103 commits；issue 面仅 1 个开放。风险一条：钉死 `mem0ai<2.1` 而 mem0ai 主线已 2.2.1（PyPI），私有 API 漂移时需上游跟进 |
| 进程模型契合（无人值守 C++ 桌面常驻） | **2** | 按**已定集成模式**评（DevDesk pip 安装并托管其配置与进程）：`telemem-mcp` 支持配置文件注入（`TELEMEM_CONFIG` / `--config`）、常驻 stdio 或仅绑回环的本地 HTTP（默认 127.0.0.1:8421）、`Memory` 实例懒加载、工具级结构化报错进程不崩——托管方只需拉起/终止进程与写一个配置文件，无人值守成立。保留两点警示：pip 安装仍要求机器上有 Python 3.10+（解释器如何随 DevDesk 分发是 DevDesk 侧议题）；仓库 CI 仅 ubuntu（`.github/workflows/ci.yml` 无 Windows 矩阵），Windows 桌面环境属"能用但官方未测" |

**总分：10 / 14。**

## 5. 结论

- **直接内置可行性**：**能**——按已定集成模式（DevDesk pip 安装并托管其配置与进程）落地可行，纯本地档全离线（Ollama 配置）；它不进 C++ 进程内，但该边界已由集成模式消化。
- **vs mem0-mcp**：对照基线为本仓已落盘的 mem0-mcp 参照（`docs/superpowers/research/2026-10-27-memcpp-mem0-mcp-reference.md`）——其三种后端（mem0 云 / Supabase pgvector / mem0 开源版内存库）全部要求联网与 API key，"本地模式"默认内存态、重启即丢，对"本地离线"轴出局；TeleMem 纯本地 FAISS+Ollama 全离线、能内置，恰好补上该文指出的"零网络+重启不丢在 MCP 记忆生态是空位"。TeleMem 是 mem0 的即插即用替换（API 同形），官方自测中文长对话基准 ZH-4O 上 86.33% 对 Mem0 70.20%（相对高 19%，**官方口径、无第三方复现**，其 issue #10 计划在 LongMemEval 复现尚未完成）；且官方仓库自带 8 工具 MCP 服务器（mem0 OSS 本体没有内建 MCP 服务器，OpenMemory MCP 是独立项目）。代价：钉死 mem0ai 旧小版本 + 面向角色扮演的中文抽取提示词（写代码场景需用 `prompt` 参数整体替换）。
- **比选最关键的单一事实**：TeleMem 的自动写入管线与我们已拍板的"只 ADD"完全同构（源码全文无一处自动 update/delete），加上 Ollama 档全离线——说明**需求本身可由第三方满足、自研 P0 的存储方案不是唯一解**；但它只能以"外挂 Python 进程 + 自研 C++ 壳"的方式进来，memcpp 的脱敏层、hook 注入壳、逐条预算格式化三件事无论选谁都得自研——集成模式已定（pip 安装 + DevDesk 托管进程/配置）之后，"内置第三方 vs 自研 P0"的差别就只剩**存储/检索/抽取引擎用谁的**这一层。
- **保留意见（裁决前必读）**：TeleMem 检索纯向量、无全文通道，对 2 字中文/标识符/错误码类查询未验证（见文末附录资产表"评测资产"行）；若走 telemem 路线，此类查询需补全文通道或维持 DevDesk 侧 LIKE 兜底——检索适配过冻结集之前，"能内置"仅指工程可行，不含"能检准"。

## 附：若内置它，memcpp 既有资产哪些作废哪些保留

| 档 | 资产 | 处置 |
|---|---|---|
| 保留 | 脱敏层设计 + 100 条红队样本（feasibility §8.1 前置条件） | 必须保留：TeleMem 无脱敏，云端抽取档的硬门槛，需在调 add() 前执行 |
| 保留 | hook 协议契约事实（2026-10-27-memcpp-hook-contract-facts.md）与失败开放模式 | hook 壳不变，壳内检索调用换成 MCP/HTTP 调 telemem |
| 保留 | 注入预算设计（≤5 条/≤500 字/带日期与来源 ID/静默跳过） | TeleMem 的融合段落返回不满足此格式，格式化自做 |
| 保留 | 评测资产（27 条冻结集、Recall@5 与注入采用率口径，feasibility §9.2） | 口径不动，被测对象换成 telemem search 重跑；尤其中文 2 字短查询在纯向量方案上的表现（正对我们"端口/配置/模块"场景），未验证 |
| 保留 | 评测资产 eval_identifiers（tools/memory-lab/：identifiers.json + 脚本 + tests） | 向量检索档下该评测线部分保留——2 字中文与路径类查询仍是全文通道的验收资产，转测 telemem search 未过即触发 §5 保留意见的补通道/兜底选项 |
| 保留 | proposals 提案卡存储设计（P0 规格 §5） | TeleMem 无人审环节；保留"人审/提案"需求的话这套仍自建 |
| 部分保留 | CLI 三子命令骨架（hook/mcp/daemon） | 语义保留；memcpp mcp 的自研 recall 实现改为转发 telemem |
| 部分保留 | P0 规格 §2.2 存储层 DDL | 主记忆表作废；proposals 表 + 幂等键语义不受影响 |
| 作废 | SQLite+FTS5 双字分词、短查询 LIKE 兜底、FTS5 查询延迟探针 | 被 FAISS 向量检索替代（评测集别扔，转测向量方案） |
| 作废 | 自研 MCP recall 工具 | telemem-mcp 8 工具现成 |
| 作废 | bge-small-zh-v1.5 量化模型选型（feasibility §8.2） | embedding 走所配接口（Ollama nomic-embed-text 等）；若保留混合检索备胎则另议 |

## 引用清单

一手来源（均为 2026-10-27 抓取，官方仓库/源码级）：
1. 仓库本体：https://github.com/TeleAI-UAGI/telemem（README、telemem/mem0.py、telemem/mcp/server.py、telemem/utils.py、telemem/configs.py、pyproject.toml、config/config.ollama.yaml、docs/MCP.md、.github/workflows/ci.yml，经 GitHub API 与 raw.githubusercontent.com 直读）
2. GitHub API 元数据（星数/commit/贡献者/release）：https://api.github.com/repos/TeleAI-UAGI/telemem
3. PyPI 依赖元数据：https://pypi.org/pypi/telemem/json、https://pypi.org/pypi/mem0ai/json，及 faiss-cpu/numpy/sqlalchemy/qdrant-client/posthog 各自 PyPI JSON
4. 技术报告：arXiv:2601.06037（README 引用，正文未逐页核对，相关数字标"官方口径"）

未证实项汇总：hook 端到端注入延迟、Windows 实机运行（CI 仅 Linux）、运行时内存占用、ZH-4O 86.33% 的第三方复现、本地 Ollama 抽取质量。
