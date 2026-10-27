# memcpp S1 spike 五项实测结果

- 日期：2026-10-27；执行环境：Windows 10.0.19045 x64，Python 3.12.15（uv 管理，telemem 区间 3.10~3.12；系统默认 3.14.7 在区间外被排除），telemem 1.10.1（PyPI 最新 release）
- 可复跑脚本：`tools/memcpp-integration/spike/run_spike.py`（venv 在 `spike/.venv/`，已 gitignore；`.runtime/` 为配置与库文件、`results/` 为原始 JSON，均 gitignore）。补跑轮升级：embedding_dims 显式 1024（env 可覆盖）/ NO_PROXY 自动旁路 / 单次动作重试上限 2 / token 用量估算
- 上游依据：spec §3 S1 表与 §5 待验证清单；telemem 拆解 `docs/superpowers/research/2026-10-27-memcpp-telemem-reference.md`

## 总表

| # | 实测项 | 数字 | 判定 |
|---|---|---|---|
| 1 | Windows pip 安装 | 退出码 0，耗时 97s，49 个依赖全部预编译轮子、零编译；`import telemem`/`telemem.mcp` 通过 | **通过** |
| 2 | 常驻内存 | 进程树 RSS：启动态 123.4~124.1MB → 懒加载+一次调用后 156.5~157.0MB；解释器子进程峰值 WS 149.8~150.3MB（双跑一致） | **通过**（< 300MB 红线） |
| 3 | 注入延迟 p95 | 未出数：embedding 通道计费不可用（S1-5 补跑实测，见「端点实测」） | **blocked** |
| 4 | 冻结集 Recall@5 | 未出数：同上；脚本已升级（dimensions/NO_PROXY/重试上限/token 估算）并离线验证 | **blocked** |
| 5 | 网关 embedding 端点 | 用户现用智谱 key 实测：**纯套餐 key**——paas/v4 鉴权通过但 429 无余额/资源包（重试一致）、模型目录无 embedding；套餐 anthropic 端点 200 | **实测完成：embedding 不可用** |

**触发线判定：不触发升级（维持 C 独跑）**：S1-4 无 Recall@5 数字，「≥85% 升 C」与「<85% 降 A」两分支均以出数为前提，故本轮不改变升级决策；阻塞根因已从「缺端点」收窄为「key 无标准通道计费额度」（见端点实测小节）。

## 逐项细节

### 1. Windows pip 安装：通过

干净 venv（Python 3.12.15）`pip install --no-cache-dir telemem`：

- 退出码 0，97 秒；`Successfully installed ... telemem-1.10.1`（完整清单见 `spike/results/pip_install.log`）。
- 关键依赖落位：faiss-cpu 1.15.1（有 cp312 Windows 轮子）、mem0ai 2.0.20（钉住 <2.1，与拆解一致）、numpy 2.5.4、mcp 2.3.0（SDK v2）、openai 3.39.0、pydantic 2.14.1。
- **零源码编译**：无 "Building wheel"、无 .tar.gz 回退，全部二进制/纯 Python 轮子 → §5-3 的 Windows 轮子担忧实测排除。
- `import telemem` 与 `import telemem.mcp` 均通过（install-check 子命令可复验）。
- S2 印证：首跑 Python 检测按 3.10~3.12 区间实现（spec 定案 6）与本机实况吻合——3.14 机器必须引导，不能放行到 pip。

### 2. 常驻内存：通过

拉起 `python -m telemem.mcp --transport streamable-http`（TELEMEM_CONFIG 注入，回环 127.0.0.1:8421），PowerShell Get-Process 按进程树测量：

| 时点 | 树合计 WS | 解释器子进程 WS | 子进程峰值 WS |
|---|---|---|---|
| 端口就绪（启动态，未建 Memory） | 123.4~124.1 MB | 116.7~117.4 MB | 同左 |
| 懒加载 Memory + 一次 add 调用后 | 156.5~157.0 MB | 149.8~150.3 MB | 149.8~150.3 MB |

- 峰值 < 300MB 红线 → 通过（§5-5 首个实测证据）。27 条冻结集量级的索引增量可忽略（Faiss 平面索引 MB 级推断与实测方向一致）。
- 附注：子进程 Private(提交)内存可到 ~2.5GB（含保留地址空间），非判据口径（spec 判 RSS），S2 资源监控按 WS 采集。

### 3. 注入延迟：blocked（补跑实测后仍未出数）

依赖链收窄：S1-5 补跑实测 embedding 通道不可用（计费态，非缺端点）→ 查询向量化无法执行 → 无法出 p50/p95。脚本已就绪并升级（`latency` 子命令：27 条入库后经 MCP streamable-http `search_memories` 实测 12 样本，阈值 500ms；host 自动 NO_PROXY 旁路、单次动作重试上限 2、payload 附 token 用量估算），标准通道可计费后一条命令出数。

### 4. 冻结集转测：blocked（补跑实测后仍未出数）

同 §3 依赖。本轮新增/验证的前置工作：

- 智谱口径落进脚本：embedder 配置显式 `embedding_dims: 1024`（mem0 仅配置该字段才向 API 透传 `dimensions` 参数；embedding-3 默认 2048 且无 768 档），离线验证 Memory 构造后 `config.embedding_dims=1024`、`_pass_dimensions_to_api=True`。
- `with_retry` 契约：仅连接失败/5xx/限流 429 重试（上限 2）；计费态 429（「余额不足或无可用资源包」）与 4xx 快速失败——本轮实测确认计费态重试不改变结局。
- `token_estimate`：docs+查询+探针字符量按 1 字 ≈ 0.6~1.0 token 估区间，随 eval/latency payload 输出。

原有前置工作（保持有效）：

- 评测口径与 memory-lab 基线对齐：`load_frozen()` 复用 eval_retrieval.py 的 doc 合成式（标题。叙述。事实。概念），Recall@5 = 前 5 名命中任一 gold 等价组的查询占比；基线 Recall@5=1.00、2 字词命中 端口 5 / 配置 3 / 模块 11；触发线 85% / 不低于 5-3-11。
- 源码确认：`add(infer=False)` 走 `_add_raw_memories` 直存，**不调 LLM、无写缓冲**——复跑只需 embedding 端点可用，LLM 可不给（提炼链路不受影响地另行验证）。
- 2 字词探针的向量档口径：top-20 结果中文本包含该词的条数，与全文索引"命中数"并排记录（纯向量无"匹配"概念，口径差异在结果文档同表呈现）。

### 5. 端点实测（2026-10-27 补跑）：智谱现 key 为纯套餐 key，embedding 通道不可用

**先答「必须是 OpenAI 兼容吗」——精确口径：LLM 侧不必须，embedding 侧基本必须。**

provider 配置面（源码级核实，file:line 为证）：

- 配置链：`TELEMEM_CONFIG` → `load_config`（telemem/utils.py:48）→ `TeleMemoryConfig`（telemem/configs.py:6，继承 mem0 `MemoryConfig`，llm/embedder 字段原样暴露、无收窄）→ `Memory.__init__` 按 provider 走工厂（mem0/memory/main.py:492、499）。
- **提炼 LLM：18 个 provider 方言白名单**（mem0/llms/configs.py:12-35 校验器；工厂表 mem0/utils/factory.py:32-53）：openai、ollama、**anthropic**、groq、together、aws_bedrock、litellm、azure_openai、openai_structured、azure_openai_structured、gemini、deepseek、minimax、xai、sarvam、lmstudio、vllm、langchain。`provider: anthropic` 走 Anthropic 原生 Messages API（mem0/llms/anthropic.py:46-48 `self.client.messages.create`，支持 `anthropic_base_url`/`ANTHROPIC_BASE_URL`）；telemem 侧调用点 `self.llm.generate_response`（telemem/mem0.py:188、494、564）provider 无关。
- **Embedding：11 个 provider 方言白名单**（mem0/embeddings/configs.py:15-31）：openai、ollama、huggingface、azure_openai、gemini、vertexai、together、lmstudio、langchain、aws_bedrock、fastembed。**无 anthropic**——Anthropic 本身不提供 embedding API，这是服务端事实不是适配缺口；telemem 调用点 `self.embedding_model.embed`（telemem/mem0.py:94）provider 无关。
- **首跑安装代价**：`anthropic` SDK 不在 telemem 依赖树（telemem-1.10.1 METADATA 0 次声明；本 venv 实测 `import anthropic` 失败）——选 anthropic 原生方言需在 `pip install telemem` 之外补装。
- **DevDesk 落点**：用户接口若非 OpenAI 兼容——提炼 LLM 可原生直配 anthropic（代价 +1 依赖 + S2 安装引导多一步）；embedding 只能是上述 11 方言之一，否则必须由 DevDesk 网关代理转译成 OpenAI 兼容。**全云端档的最小公约数仍是 OpenAI 兼容接口**（一次配置同时覆盖提炼+向量化）。
- 实测边界：本次实测只验证了 `provider: openai` 形态的配置注入生效（S1-2 探针即此形态）；anthropic 方言为源码级核实，未端到端跑通（无凭据）。

补跑实测（用户拍板「直接用现有智谱 key」）：key 取自用户既有 Claude Code 配置（`ANTHROPIC_AUTH_TOKEN`/`ANTHROPIC_BASE_URL`，运行时读入环境变量注入，全文档/日志零落盘），实测该配置 `ANTHROPIC_BASE_URL=https://open.bigmodel.cn/api/anthropic`（只记端点不记 key）。判定链（每探针重试上限 2 内）：

| 探针 | 结果 | 判读 |
|---|---|---|
| paas/v4 `POST /embeddings`（embedding-3，dimensions=1024） | **429「余额不足或无可用资源包，请充值」**，重试 1 次一致（<300ms/次） | 鉴权层**通过**（非 401/403）：key 被标准通道识别，但账户无按量额度；计费态错误重试不改变结局 |
| paas/v4 `GET /models` | 200：11 个模型全为 glm chat 系列，**无 embedding 类** | 辅证（模型枚举可能本就不含 embedding 类，不独立成判据） |
| 套餐 anthropic 端点 `GET /v1/models` | HTTP 200 但业务体 `{"code":500,"msg":"404 NOT_FOUND"}` | 该兼容层未实现 /models，无法零消耗判定 |
| 套餐 anthropic 端点 `POST /v1/messages`（glm-5.3-flash，max_tokens=1） | **200**，usage 13+1 token | key 在套餐通道有效 → **纯套餐 key 判定成立**，与调研「2026-10-16 起两类 key 互不通用」一致 |

- **代理面闭环**：请求 host 并入 `NO_PROXY`/`no_proxy`（幂等合并，MCP 子进程一并继承）旁路本机系统代理 127.0.0.1:51295 后，拿到的是智谱业务层响应（429/200）而非 S1 的代理 503——旁路生效，行为已固化进脚本（`apply_no_proxy`）。
- **不硬试项**：`/api/coding/paas/v4/embeddings` 未试——调研明确套餐通道对非指定工具调用有风控约束（限流/冻结/封禁），本分支按任务规矩如实记录。
- **结论**：embedding 不可用，S1-3/4 出数阻塞根因 = 标准通道无计费额度。解绑动作（用户侧二选一）：① 同账号另建标准 API key 并备少量余额（embedding-3 按 0.5 元/百万 token，本轮预算仅几十次调用）；② 现账号充值开通按量。任一就绪后按下表注入复跑即出数。

复跑注入（标准通道可计费后；提炼与 embedding 同走标准按量通道）：

```
TELEMEM_SPIKE_EMBED_BASE_URL=https://open.bigmodel.cn/api/paas/v4
TELEMEM_SPIKE_EMBED_API_KEY=<有按量额度的 key>
TELEMEM_SPIKE_EMBED_MODEL=embedding-3
TELEMEM_SPIKE_EMBED_DIMENSIONS=1024   # 可选，脚本默认 1024
TELEMEM_SPIKE_LLM_BASE_URL=https://open.bigmodel.cn/api/paas/v4
TELEMEM_SPIKE_LLM_API_KEY=<同上或同账号另一把标准 key>
TELEMEM_SPIKE_LLM_MODEL=glm-5.3-flash
```

复跑（仓库 worktree 内）：

```
spike/.venv/Scripts/python.exe run_spike.py latency   # 出 p50/p95（阈值 500ms）
spike/.venv/Scripts/python.exe run_spike.py eval      # 出 Recall@5 + 2 字词命中数
```

**消耗对账（本轮实付）**：embedding 尝试 3 次全部被拒（零成功零计费）；chat 1 次（max_tokens=1 探针，13+1 token，计套餐额度）；元数据 GET 2 次（免费）。远低于本轮预算（embedding ≈52 次、零 LLM chat）。

## 顺带验证成立的事实（反哺 spec §1/§5）

- **懒加载 + 工具级结构化报错契约**（§5-2 前半）：占位端点下 `add_memory` 返回 `{"result":{"error":"InternalServerError","detail":"Error code: 503"}}`，进程不崩、端口不挂——spec §1「配置错误表现为工具级结构化报错而非进程崩溃」实测成立。注意错误载荷嵌在 `result` 键内且 `is_error=false`，S2 健康检查不能只看 is_error 位，要查载荷内 `error` 键。
- **TELEMEM_CONFIG 注入**：环境变量喂 yaml 配置文件即生效，进程按配置启动回环 8421（spec §1 配置形态成立）。
- **Windows 实机运行**（§5-2）：进程拉起/回环绑定/懒加载/报错不崩全部实测通过，云端链路（提炼+embedding）仍待端点后补验。
- **venv 进程树陷阱**：Windows venv 的 `python.exe` 是 ~6.7MB 启动桩，真实解释器是其子进程——S2 做健康检查/资源监控/终止时必须按进程树操作（`taskkill /T`），单 PID 操作会漏掉真正干活、真正占内存的进程。
- **MCP SDK v2 接口面**：client 为 `streamable_http_client`（非 streamablehttp_client），结果字段 snake_case（`structured_content`/`is_error`）；HTTP 端点路径 `/mcp`。
- **遥测**：`import telemem` 强制 `MEM0_TELEMETRY=False`（源码 `telemem/__init__.py`），默认无外发统计。

## 遗留与待办

| 项 | 状态 | 动作 |
|---|---|---|
| S1-3 / S1-4 出数 | blocked（根因实测收窄为计费） | 标准通道可计费（另建标准 key 或充值）→ 复跑 latency/eval |
| 触发线判定 | **不触发升级（维持 C 独跑）** | eval 出数后复核——Recall@5 ≥85% 才改升 C |
| S2 Python 检测区间 | 无阻塞 | 按 3.10~3.12 实现，本机 3.14 实况印证拦截必要性 |
| S3 代理面 | spike 层已闭环 | NO_PROXY 自动注入固化进脚本；S3 网关注入沿用该模式 |
