# 外部调用面超时/错误反馈盲区矩阵（app-polish 审计 · 走查前基线）

- 产出：code-reviewer 只读审查（pool:need:284），2026-11-18。手册异常表锚点直接引用（06-claude代理 / 07-智能体 / 10-mcp），仅对「未知/半处理/未覆盖面」下钻代码。
- 判定分级：P0=会挂死无任何反馈 ｜ P1=有错误但无超时或提示不可达 ｜ P2=提示可用但体验差/提示可达性未核实 ｜ P3=已闭环 ｜ 未知=查无锚点（附查证路径）。
- 全局背景：全应用禁 C++ 异常、错误链统一 Result<T>（src/core/result.h）；全局 toast 桥 toast_notice_bridge.h:13-18 实际仅 main/home/schedule 接入，其余模块用各自页面内 toast/状态行。

## 一、矩阵（24 行）

### ① LLM 非流式

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 1 | agent 直连 LLM 请求 | HTTP 120s（src/module_agent/direct_llm_client.cpp:24、287） | 无（未见重试调用锚点） | 任务卡异常徽章+运行记录详情（agent_module.cpp:1062-1069；手册07:64「run failed，错误=http_client 超时文案」） | 无单任务取消钮（运行中禁重复启动 agent_module.cpp:1128；模块 onStop 统一 cancel agent_module.cpp:439-467） | run failed 落运行记录 | P2（120s 长等待无进度/无中途取消） |
| 2 | agent_loop 请求预算 | 单请求 60s、总预算=轮数×60s（agent_loop.cpp:41、196） | 同上 | 同上 | 同上 | — | P3 |
| 3 | memory telemem LLM 调用（Python mem0 进程内 Anthropic 客户端） | 未知——DevDesk 仅透 key/base_url（telemem_config.cpp:40、71），进程内无超时控制锚点 | 未知 | 未知 | 未知 | 未知 | 未知（偏 P1：记忆任务可能长挂无反馈）。查证路径：实跑 telemem 断网观测 mem0 AnthropicLLM 默认超时与报错冒泡 |
| 4 | memory 本地模型下载（bge embedding） | timeoutMs=0 但低速超时兜底 lowSpeedBytes/TimeSec（llama_host.cpp:188-190）；中断→NetworkTimeout「下载中断」（llama_host.cpp:230） | 无 | 错误文案中文（llama_host.cpp:230）；下载进度/失败是否上 UI 未核 | cancel() 有调用（llama_host.cpp:213） | 失败清临时文件（llama_host.cpp:228） | P2 |

### ② LLM 流式（claude_proxy）

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 5 | 流式转发·上游读 | **无空闲超时**：阻塞 readChunk，长挂流只能靠对端断开（streaming_proxy.h:94；手册06:59 已证半处理） | 无 | 断流→合成 event:error SSE 帧客户端可感知（streaming_proxy.cpp:255-264）；DevDesk 页面无专门提示 | 客户端断开→cancel() 中止上游（streaming_proxy.h:94、100） | error 帧后结束 | **P1**（挂死类：上游半开连接时 indefinitely 阻塞） |
| 6 | 非流式转发·上游失败/超时 | 120s（proxy_router.cpp:238）；失败→502+upstream_unreachable+错误文案（proxy_router.cpp:257-266） | 无 | 502 body 透给 Claude Code；页面调用日志落 status 0（proxy_router.cpp:262-264） | 无（转发无中途取消） | — | P3 |
| 7 | 非流式转发·上游非 200 | 同上 120s | 无 | 状态码透传（proxy_router.cpp:272）+调用日志落库（proxy_router.cpp:295-301）；手册06:60 的「未知」行据此可升级为已处理 | 无 | — | P3 |
| 8 | 停代理/改配置时在途请求收尾 | 未知（手册06:67 已标未核） | 未知 | 未知 | 停代理按钮存在 | 未知 | 未知。查证路径：streaming_proxy 停止路径压测（带在途长流时停代理） |
| 9 | 连通/前置探测 | 15s 测试连通（claude_service.cpp:236、238）、5s 导入预检（:118）、2s 启动探测（:144）、非 2xx 算不过（:344） | 无 | 分类 toast（claude_module.cpp:577-588、611-616；手册06:21/23） | — | 探测不过→拒绝启动代理 | P3 |

### ③ MCP

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 10 | hub 工具调用（服务端执行） | **外层无总闸**：请求线程同步执行无 timeout（mcp_protocol.cpp:195-290；手册10:48 已证）；内置工具自带超时：bash.run 30s 默认/600s 上限（agent_builtin_tools.cpp:685-687）、net.fetch 10s（net_fetch_tool.h:25）、逐跳 10s/探测 300ms（mcp_tools_builtin.cpp:105、733-734；mcp_tools_builtin.h:35） | 无 | 解析失败 -32603 兜底（手册10:52）；**工具本身挂死则客户端请求挂死** | 无 | — | **P1**（挂死类：外层补总闸即可收口） |
| 11 | MCP 页面健康探测 | 1.5s（mcp_page_module.cpp:57、87） | 无 | 页面状态行 | — | — | P3 |

### ④ HTTP 通用（platform 层与工具层）

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 12 | http_client 基础请求 | 默认 30s（http_client.h:54；CURLOPT_TIMEOUT_MS http_client.cpp:161）；超时独立 NetworkTimeout 错误码（http_client.cpp:199-202） | 无（由调用方决定） | 错误码+文案随 Result 冒泡 | — | tcpReachable 带 timeout（http_client.cpp:216-231） | P3 |
| 13 | http_client 流式 | waitForStatus 30s（http_client.h:110-112、http_client.cpp:390-393；proxy_router.cpp:382→502 :385-388）；背压 deadline 兜底（http_client.cpp:270） | 无 | 同上 | cancel() | — | P3 |
| 14 | net.fetch（agent 工具） | 默认 10s（net_fetch_tool.h:25；net_fetch_tool.cpp:114-116、154） | 无 | 错误文案含 timeout 分类（net_fetch_tool.cpp:85）；异常回喂不中止运行（agent_loop.cpp:308、325） | — | — | P3 |
| 15 | github_api（git 模块） | 30s（github_api.cpp:37、400） | 无 | 错误链 Result；错误是否冒到 UI toast 未核 | — | — | P2 |
| 16 | hot_tool 热点拉取 | 每源 8s（hot_tool.cpp:35、296、391） | 无 | 工具结果回喂 agent | — | — | P3 |

### ⑤ keychain

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 17 | keychain store/load/delete | 无超时概念（Security API 阻塞等授权框，用户可取消） | 无 | Result 错误链完整、区分 NotFound/授权拒绝/用户取消（keychain_macos.cpp:21、45-132）；调用方 UI 提示未逐点核（memory_module.cpp:84 仅 bool 判断） | 用户在授权框可取消 | delete 幂等 ok（keychain_macos.cpp:135-138） | P2（Windows 侧 credential_protection_win.cpp 未核，走查时补） |

### ⑥ 文件 I/O

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 18 | markdown 读/索引/保存 | 本地盘无超时概念 | 无 | Result+中文文案（markdown_service.cpp:283-294、301-306）；saveEditor 失败 dirty 保持、阻止切换（md_module_tree.cpp:634-654）；失败是否 toast 未逐点核 | — | dirty 链防丢稿 | P2 |
| 19 | 壁纸 mpv 视频播放 | IPC 写超时判定通道死亡→TerminateProcess 收尾、控制线程绝不永久阻塞（mpv_controller.cpp:9） | 无 | recordError 记录+平台不支持文案（mpv_controller_stub.cpp:24）；recordError 是否上 UI 未核 | — | 就地 TerminateProcess | P2 |
| 20 | 壁纸视频/图片扫描 | 本地扫描无网络 | 无 | 可用性评估上 UI（wallpaper_service.cpp:685-692） | — | — | P3 |

### ⑦ SQLite

| # | 调用面 | 超时机制 | 重试 | 用户可见提示（通道+锚点） | 取消途径 | 降级/恢复 | 判定 |
|---|---|---|---|---|---|---|---|
| 21 | devdesk.db/各库打开与操作 | busy_timeout 5s+WAL（sqlite_db.cpp:275-277） | 无（busy 等 5s 后失败） | 错误映射完整：CORRUPT→DatabaseCorrupted、BUSY→DatabaseOpenFailed、PERM→FilePermissionDenied、FULL→FileWriteFailed（sqlite_db.cpp:16-31）；模块级「库打不开→用户提示」未逐模块核 | — | — | P2 |

矩阵外备注：agent 运行记录含提示词/工具结果明文落库（agent_store.cpp:310-315、436-437）——安全面非超时面，手册07:70 已判未处理，不计入本矩阵盲区统计。

## 二、老板三问直答

**a) 大模型调用超时哪些面有处理+提示：**
- 直连 LLM：120s 总超时（direct_llm_client.cpp:24、287）+每请求 60s 预算（agent_loop.cpp:41、196），失败→任务卡异常徽章+运行记录（agent_module.cpp:1062-1069）。
- 代理非流式转发：120s（proxy_router.cpp:238），上游失败→502+错误文案（proxy_router.cpp:257-266），非 200 状态码透传+调用日志（proxy_router.cpp:272、295-301）。
- 代理前置探测：15s/5s/2s 三档（claude_service.cpp:236/118/144）+分类 toast（claude_module.cpp:577-588）。
- 流式断流：客户端可感知合成 event:error SSE 帧（streaming_proxy.cpp:255-264）——但仅此而已，见 b。

**b) 缺失哪些：**
- 流式转发上游读**无空闲超时**（阻塞 readChunk，streaming_proxy.h:94）——上游半开即 indefinitely 挂，P1。
- MCP hub 工具调用**外层无超时总闸**（mcp_protocol.cpp:195-290）——慢工具挂死整个请求线程，P1。
- memory telemem 的 LLM 调用在 Python 进程内，DevDesk 完全不可见（超时/提示/取消全未知，行 3）。
- 停代理时在途请求收尾未核（行 8）。
- keychain/markdown/sqlite/mpv/github 错误链均完整（Result+中文文案），但「错误是否真到达用户眼睛」未逐点核实（行 15/17/18/19/21）。

**c) 盲区大小：** 24 行中 P0=0、P1=2、P2=7、P3=13、未知=2。P0/P1/未知合计 4/24≈17%（且 P1 两条都是「挂死类」：流式上游读、MCP 工具执行）；把 P2（提示可用性未闭环）算入则 11/24≈46%。**一句话：大模型调用的常规超时面已基本闭环，真正的洞集中在「流式上游读无空闲超时」与「MCP 工具调用无外层超时」两个挂死点，外加 memory telemem 通道整体不可见。**
