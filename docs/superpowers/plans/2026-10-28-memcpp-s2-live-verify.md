# S2 记忆模块 live 层验证报告（memcpp 分支 feat/memcpp worktree）

- 日期：2026-10-28；执行者：功能实证子代理（独立于实现/测试）
- 环境：Windows 10.0.19045 x64；DevDesk worktree `D:/wt-memcpp`；独立数据根
  `C:/Users/coderstory/AppData/Local/Temp/devdesk_mem_verify`（全程未触碰用户真实数据根与安装产物）
- 基线提交：fd9ee4d8 起五提交；验证中新增修复提交 1331450c / 90cf043f / 473204ff / 00ac9931

## 总结论：可交付（S2 范围内五项全过；发现并修复 4 处真实缺陷）

## 逐项实证

### 1. 真 venv + pip 首跑 —— 通过
- 步骤/预期：模块 Python 定位逻辑真跑（编译探针直链 `python_locator*.cpp` 真源 +
  `systemVersionProbe` 真进程探测）；本机 PATH 默认 3.14 应被拦、uv 管的 3.12 应通过；
  用选中解释器按 `buildVenvArgs`/`buildPipInstallArgs` 的真实 argv 建 venv 并装 telemem。
- 实际：
  - 修复前：`findQualifiedPython` = **NOT_FOUND**（3.14 判定 qualified=0 正确拦截，但
    uv 3.12 找不到——新装 uv 的 python 落在 `%APPDATA%\Roaming\uv\python`，代码只
    glob `%LOCALAPPDATA%\uv\python`，本机又无 Python310/311/312 官方目录）→ 真缺陷。
  - 修复后（1331450c）：FOUND uv 3.12.15；3.14 仍被拦（qualified=0）。
  - venv：`<dataRoot>/memory/venv` 4 秒建成；`pip install --disable-pip-version-check
    telemem` 68 秒（PyPI），`import telemem` 1.10.1 OK。
- 证据：本报告 §工具输出存档 `.verify-tmp/t*.out`；探针 `.verify-tmp/locate_probe.exe`
  前后两次输出（修复前 NOT_FOUND / 修复后 FOUND）；venv 时间戳
  `devdesk_mem_verify/pip-install.log`。
- 备注：venv/pip 步骤为 bash 手动等价执行（`runInstallStep` 是 telemem_engine.cpp
  文件内静态函数，UI-only，S3 才接线）——argv 与模块纯函数产物逐字一致。

### 2. 真引擎拉起对账 —— 通过（发现 3 处缺陷并修复）
- 步骤/预期：用 `generateTelememConfigYaml` 真源产物 + `buildTelememServerArgs` 真源
  argv 拉起 telemem 于回环 8421；健康探活真实命中。
- 实际（对账 7 点，全部以 venv 内真类执行验证，非读码推断）：
  | # | 生成代码 | telemem/mem0 1.10.1 实际 | 证据 | 修复 |
  |---|---|---|---|---|
  | 1 | `-m telemem serve --http` | 无 `__main__`，真实入口 `-m telemem.mcp --transport streamable-http` | `No module named telemem.__main__`；`telemem.mcp --help` | 90cf043f |
  | 2 | llm.config.`base_url` | AnthropicConfig 收 `anthropic_base_url`，写 base_url = TypeError | 真类执行 TypeError | 90cf043f |
  | 3 | llm.config.`api_key_env` | 任何层都无此字段（telemem/mem0 全仓零引用）；key 真实通道 = api_key 空串 → 环境回退 `ANTHROPIC_API_KEY` | 真类 TypeError + 源码 `config.api_key or os.getenv("ANTHROPIC_API_KEY")` | 90cf043f（kLlmKeyEnvName 改名，key 仍只进子进程环境） |
  | 4 | embedder.config.`base_url` | BaseEmbedderConfig 收 `openai_base_url` | 真类执行 TypeError | 90cf043f |
  | 5 | embedder 无 embedding_dims | mem0 缺省 1536 且仅显式设置才对维度 | `OpenAIEmbedding.__init__` 行为 | 补 `embedding_dims: 512` |
  | 6 | vector_store 无 embedding_model_dims | FAISSConfig 默认 1536、无 embedder→向量库联动，512 维向量首次 add 必炸 | FAISSConfig 实例 dims=1536 | 补 `embedding_model_dims: 512` |
  | 7 | 健康探活 `GET /` | 404；`GET /mcp` = 405；POST /mcp 需 Accept 双类型否则 -32600；200 响应体是 SSE 信封 | curl 实测 404/405/-32600/text-event-stream | 90cf043f 改 POST /mcp initialize + 473204ff 剥 SSE 信封 |
- 健康契约命中：`POST /mcp` initialize → HTTP 200 + JSON body（`body.result.error`
  嵌套错误契约在真实 add 失败样本中直接观察到：ImportError/ParserError 均长在
  `structuredContent.result.error`——judgeTelememHealth 判据与真实形态一致）。
- 证据：本节 curl 原始响应在会话记录；单测钉死新契约
  （`tests/test_modules/test_memory_engine.cpp`，api_key_env/裸 base_url 禁现断言）。

### 3. llama-server 联动 + 端到端 add/search —— 通过
- 步骤/预期：`buildLlamaServerArgs` 同参数真起 llama-server（.bakeoff/bin，CPU 包）于
  回环 11435 + `bge-small-zh-v1.5-q8_0.gguf`；telemem embedder 指向它；真实 add（infer=false，
  内容「验证用的记忆条目」）+ search。
- 实际：
  - llama-server `GET /health` → `{"status":"ok"}`；模型加载日志 "listening on
    http://127.0.0.1:11435"。
  - 首次 add 失败（ImportError: anthropic 缺包）→ 真缺陷：telemem 元数据无 anthropic
    依赖（extras 仅 all/mcp/video 且都不含）→ 00ac9931 修 buildPipInstallArgs 显式同装。
  - 补装后 add：`{"status":"stored","scope":{"user_id":"verify_user"}}`，**2316ms**
    （含 faiss 首次建索引）；search query「记忆条目」→ `{"query":"记忆条目","memories":
    "验证用的记忆条目"}`，**157ms**；`faiss_db/mem0.faiss` 落盘（2093B）。
  - telemem 日志两条 `POST http://127.0.0.1:11435/v1/embeddings 200 OK`——embedder
    真实走 llama-server；`dimensions=512` 参数 llama-server 接受（200）。
- 证据：`devdesk_mem_verify/telemem.log`（httpx 嵌入请求行）、`memory/faiss_db/`。

### 4. app 真窗口三视图截图 —— 通过
- 步骤/预期：独立 DEVDESK_DATA_ROOT 下，进程内帧捕获 + 真实指针注入（probe harness
  `queuePointerButton` 点 `memory.tabs.N.hit` 真节点）导航「记忆」页三视图。
- 实际：三张 PNG 落 `D:/wt-memcpp/.verify-screenshots/`（探针自报 PASS，1200x1000）：
  - `memory-1-console.png`（主控台，143KB）
  - `memory-2-review.png`（记忆审核，101KB——Tab 点击后切换）
  - `memory-3-config.png`（配置，114KB——Tab 点击后切换）
- 证据：三文件 + 探针 stdout（三行落图 + PASS）。临时探针已删除、CMakeLists 已还原，
  仓库无残留（`.verify-screenshots/`、`.verify-tmp/` 为未跟踪验证产物）。

### 5. key 红线复查 —— 通过
- 步骤/预期：TELEMEM_CONFIG 文件内容 + 全部验证日志 grep 零 key 明文。
- 实际：配置文件 `api_key: ""` 恒空（文件头注释只出现环境变量**名** ANTHROPIC_API_KEY，
  非 key 值）；`sk-…` 形态 grep 零命中；验证用哑 key（仅存在于子进程环境变量）
  grep telemem.log / llama-server.log / pip-install.log / 构建日志全部 CLEAN。

## 未验证（如实列出）
- 引擎 `startAsync` 全链路的 UI 接线（面板按钮→后台线程→拉起）：S2 无 UI 接线，S3 验。
- add infer=true（需真实智谱 LLM key 在线提炼）：本验证用哑 key，未走真 LLM。
- 模型在线下载路径（ensureEmbedModelDownloaded）：本机已有模型文件，未触发下载。
- macOS/Linux 平台面（python_locator_posix、telemem_process_posix）：本机为 Windows。
- 全量 `build-test-run.sh`：验证起点与收尾各跑一次 `--no-test --no-run` 均
  「✅ 构建成功」；全量单测 1490 例 Passed 1481 / Failed 0 / Skipped 9 / RC=0。

## 环境事件（非本仓库缺陷）
- `D:/project/gui/EUI-NEO`（兄弟仓、共享检出）验证中途出现 staged WIP：
  `core/window/window_types.h` 78 行重复声明 `bool visible`，令一次增量重编 eui_neo
  目标失败（约 11:15）。该 WIP 随后被其作者修复，验证收尾重跑
  `scripts/build-test-run.sh --no-test --no-run` 全量通过（「✅ 构建成功」，
  DevDesk.exe 11:23 重链，含本次全部引擎修复）。
- 已知既有未跟踪/改动文件（非本次验证产生）：identifier-eval-audit.md 改动、
  两个 *ref-audit.md 未跟踪文件——未纳入任何本次提交。

## 修复提交清单
| commit | 内容 |
|---|---|
| 1331450c | python_locator_win 补 `%APPDATA%\uv\python`（Roaming，live 实测 uv 新默认） |
| 90cf043f | serve 入口/配置字段名/健康探活路径与判据（对账 ①②③④⑤⑥⑦） |
| 473204ff | MCP 健康探活剥 SSE 信封 + Accept 双类型 |
| 00ac9931 | pip 安装目标补 anthropic |
