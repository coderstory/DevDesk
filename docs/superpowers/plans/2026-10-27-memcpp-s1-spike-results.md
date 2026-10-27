# memcpp S1 spike 五项实测结果

- 日期：2026-10-27；执行环境：Windows 10.0.19045 x64，Python 3.12.15（uv 管理，telemem 区间 3.10~3.12；系统默认 3.14.7 在区间外被排除），telemem 1.10.1（PyPI 最新 release）
- 可复跑脚本：`tools/memcpp-integration/spike/run_spike.py`（venv 在 `spike/.venv/`，已 gitignore；`.runtime/` 为配置与库文件、`results/` 为原始 JSON，均 gitignore）
- 上游依据：spec §3 S1 表与 §5 待验证清单；telemem 拆解 `docs/superpowers/research/2026-10-27-memcpp-telemem-reference.md`

## 总表

| # | 实测项 | 数字 | 判定 |
|---|---|---|---|
| 1 | Windows pip 安装 | 退出码 0，耗时 97s，49 个依赖全部预编译轮子、零编译；`import telemem`/`telemem.mcp` 通过 | **通过** |
| 2 | 常驻内存 | 进程树 RSS：启动态 123.4~124.1MB → 懒加载+一次调用后 156.5~157.0MB；解释器子进程峰值 WS 149.8~150.3MB（双跑一致） | **通过**（< 300MB 红线） |
| 3 | 注入延迟 p95 | blocked：缺 embedding 端点（依赖 S1-5） | **blocked** |
| 4 | 冻结集 Recall@5 | blocked：缺 embedding 端点（依赖 S1-5）；脚本与资产装载已就绪自检通过 | **blocked** |
| 5 | 网关 embedding 端点 | 本机无可直接使用的带凭据 OpenAI 兼容 embedding 端点 | **blocked，需用户提供** |

**触发线判定未生效**：S1-4 blocked，升级 C 与否悬置，待用户端点后复跑 `run_spike.py eval` 出数。

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

### 3. 注入延迟：blocked

依赖链：S1-5 无可用 embedding 端点 → 每次检索的查询向量化无法执行 → 无法出 p50/p95。脚本已就绪（`latency` 子命令：27 条入库后经 MCP streamable-http `search_memories` 实测 ≥10 样本，阈值 500ms），端点就绪后一条命令出数。

### 4. 冻结集转测：blocked

同上依赖。已完成的前置工作：

- 评测口径与 memory-lab 基线对齐：`load_frozen()` 复用 eval_retrieval.py 的 doc 合成式（标题。叙述。事实。概念），Recall@5 = 前 5 名命中任一 gold 等价组的查询占比；基线 Recall@5=1.00、2 字词命中 端口 5 / 配置 3 / 模块 11；触发线 85% / 不低于 5-3-11。
- 源码确认：`add(infer=False)` 走 `_add_raw_memories` 直存，**不调 LLM、无写缓冲**——复跑只需 embedding 端点可用，LLM 可不给（提炼链路不受影响地另行验证）。
- 2 字词探针的向量档口径：top-20 结果中文本包含该词的条数，与全文索引"命中数"并排记录（纯向量无"匹配"概念，口径差异在结果文档同表呈现）。

### 5. 网关 embedding 端点：blocked，需用户提供

**先答「必须是 OpenAI 兼容吗」——精确口径：LLM 侧不必须，embedding 侧基本必须。**

provider 配置面（源码级核实，file:line 为证）：

- 配置链：`TELEMEM_CONFIG` → `load_config`（telemem/utils.py:48）→ `TeleMemoryConfig`（telemem/configs.py:6，继承 mem0 `MemoryConfig`，llm/embedder 字段原样暴露、无收窄）→ `Memory.__init__` 按 provider 走工厂（mem0/memory/main.py:492、499）。
- **提炼 LLM：18 个 provider 方言白名单**（mem0/llms/configs.py:12-35 校验器；工厂表 mem0/utils/factory.py:32-53）：openai、ollama、**anthropic**、groq、together、aws_bedrock、litellm、azure_openai、openai_structured、azure_openai_structured、gemini、deepseek、minimax、xai、sarvam、lmstudio、vllm、langchain。`provider: anthropic` 走 Anthropic 原生 Messages API（mem0/llms/anthropic.py:46-48 `self.client.messages.create`，支持 `anthropic_base_url`/`ANTHROPIC_BASE_URL`）；telemem 侧调用点 `self.llm.generate_response`（telemem/mem0.py:188、494、564）provider 无关。
- **Embedding：11 个 provider 方言白名单**（mem0/embeddings/configs.py:15-31）：openai、ollama、huggingface、azure_openai、gemini、vertexai、together、lmstudio、langchain、aws_bedrock、fastembed。**无 anthropic**——Anthropic 本身不提供 embedding API，这是服务端事实不是适配缺口；telemem 调用点 `self.embedding_model.embed`（telemem/mem0.py:94）provider 无关。
- **首跑安装代价**：`anthropic` SDK 不在 telemem 依赖树（telemem-1.10.1 METADATA 0 次声明；本 venv 实测 `import anthropic` 失败）——选 anthropic 原生方言需在 `pip install telemem` 之外补装。
- **DevDesk 落点**：用户接口若非 OpenAI 兼容——提炼 LLM 可原生直配 anthropic（代价 +1 依赖 + S2 安装引导多一步）；embedding 只能是上述 11 方言之一，否则必须由 DevDesk 网关代理转译成 OpenAI 兼容。**全云端档的最小公约数仍是 OpenAI 兼容接口**（一次配置同时覆盖提炼+向量化）。
- 实测边界：本次实测只验证了 `provider: openai` 形态的配置注入生效（S1-2 探针即此形态）；anthropic 方言为源码级核实，未端到端跑通（无凭据）。

只读发现结果（未读任何 DevDesk 生产凭据配置）：

- 环境变量：仅 `ZAI_BUSINESS_BASE_URL=https://api.z.ai`、`ZCODE_BASE_URL=https://zcode.z.ai`——无成对 API key 可归 DevDesk 使用；无 OPENAI/LLM/embedding 类凭据变量。
- 本机端口：11434（Ollama）/ 1234（LM Studio）/ 8080 均未监听 → 无本机 OpenAI 兼容服务。
- ZCode 会话代理 `127.0.0.1:51295` 不是 embedding 端点（占位端点经其返回 503），且属工具链临时代理，不构成依赖。
- 与 spec §5-1 前案一致（可行性报告：网关带凭据配置均无 embedding 接口，MiniMax 余额 0），本次为未复测后的实测补充：**仍未发现**。

**用户待办（一句话）**：请提供一个有余额的 OpenAI 兼容 embedding 接口（顺带提炼 LLM 接口），按下列变量提供后即可复跑出齐 3/4 两项数字：

```
TELEMEM_SPIKE_EMBED_BASE_URL=https://<网关>/v1     # OpenAI 兼容 base_url，含 /v1
TELEMEM_SPIKE_EMBED_API_KEY=sk-xxx                 # 该网关密钥
TELEMEM_SPIKE_EMBED_MODEL=<embedding 模型名>        # 如 text-embedding-3-small / embedding-3
TELEMEM_SPIKE_LLM_BASE_URL=https://<网关>/v1
TELEMEM_SPIKE_LLM_API_KEY=sk-xxx
TELEMEM_SPIKE_LLM_MODEL=<对话模型名>                # 记忆提炼用
```

复跑（仓库 worktree 内）：

```
spike/.venv/Scripts/python.exe run_spike.py latency   # 出 p50/p95（阈值 500ms）
spike/.venv/Scripts/python.exe run_spike.py eval      # 出 Recall@5 + 2 字词命中数
```

注意：本机配置了系统代理（http_proxy=127.0.0.1:51295），telemem 的 openai 客户端会走代理——若网关需直连，复跑时补 `NO_PROXY` 覆盖（S3 网关注入时同样要处理该面）。

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
| S1-3 / S1-4 出数 | blocked | 用户提供端点（上文 6 个变量）→ 复跑 latency/eval |
| 触发线判定 | 悬置 | eval 出数后对照 85% / 5-3-11，自此生效 |
| S2 Python 检测区间 | 无阻塞 | 按 3.10~3.12 实现，本机 3.14 实况印证拦截必要性 |
| S3 代理面 | 新发现 | 网关注入需处理 http_proxy/NO_PROXY，避免云端调用被本机代理截胡 |
