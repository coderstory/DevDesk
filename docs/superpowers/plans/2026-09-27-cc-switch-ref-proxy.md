# cc-switch 本地代理侧只读研究（对照 DevDesk）

- 研究对象：`D:\project\gui\cc-switch` v3.20.4（`src-tauri/Cargo.toml:4`），**只读**，未做任何改动。
- 对照对象：DevDesk `src/module_claude_proxy/*`、`src/platform/http_*`、`src/core/*`。
- 行号取自 2026-09-27 工作区快照，会漂移；每条附符号名便于重新定位。没读到源码的推测一律标「待验证」。

## 1. cc-switch 代理机制速览

### 1.1 代理本体（框架 / 监听 / 路由 / 流式 / 超时）

- **框架**：Tokio + axum 0.7 + hyper 1.x（`Cargo.toml:51,54`），上游客户端 reqwest 0.12 + hyper-util（`Cargo.toml:40,55`）。起法**不是** `axum::serve`，而是手写 accept loop + `hyper::server::conn::http1::Builder::new().preserve_header_case(true)`（`src/proxy/server.rs:141-206`）。每条连接先 `TcpStream::peek` 原始字节抓 header 原始大小写（`server.rs:158-175`，`OriginalHeaderCases` 定义在 `proxy/hyper_client.rs`），再用 `hyper::service::service_fn` 把请求桥进 axum `Router`（`server.rs:177-193`）。
- **监听**：`proxy_config.listen_address` 默认 `127.0.0.1`、`listen_port` 默认 `15721`（`database/schema.rs:129-130`）；允许配 `0.0.0.0`/`::`，但写回客户端时折成回环（`services/proxy.rs:1955-1958 build_proxy_urls`）。
- **路由**：按端点自动分派应用、URL 无 app 前缀——`/v1/messages`→Claude、`/v1/chat/completions`+`/v1/responses`→Codex、`/v1beta/*path`→Gemini、`/grokbuild/...`（`proxy/server.rs:291-400`；设计说明 `services/proxy.rs:2025-2031`）。没有 `.fallback()`：未登记路径由 axum 直接 404（`server.rs:291-400` 全表已核，无兜底路由）。
- **流式**：handler 用 `axum::body::Body::from_stream(...)` 直接透传（`proxy/handlers.rs:536,1229,1435,1808`；`proxy/response_processor.rs:202`）。SSE 原语在 `proxy/sse.rs`：`take_sse_block:7`、`append_utf8_safe:37`（跨 chunk 的 UTF-8 半字符 + 非法字节处理）。**没有显式队列**——靠 async 流「不 poll 即背压」。
- **超时**：reqwest 全局 600s + connect 30s + `pool_max_idle_per_host(10)` + `tcp_keepalive(60s)`（`proxy/http_client.rs:216-222`）；**流式请求把总超时抬到 24h**（`proxy/forwarder.rs:2356-2359`），靠首包/静默超时兜底；非流式默认 600s（`forwarder.rs:2324-2327`）。
  - 实际执行点在 `proxy/response_processor.rs:706-736`（首个 chunk 用 first_byte、其后用 idle），来源 `RequestContext::streaming_timeout_config()`（`proxy/handler_context.rs:266-278`）。
  - `forwarder.rs:253` 的 `_streaming_idle_timeout` 带下划线**未被使用**；DB 列默认 120s（`database/schema.rs:134`），claude 行调成 90/180（`schema.rs:154`）。
  - 门控：**failover 关 → 流式超时整体禁用**（`handler_context.rs:266-278` 返回 0,0）。
- **取消**：客户端断开由 hyper/axum 的 body 流 drop 传导（未见显式 cancel API；在途计数由 `proxy/forwarder.rs` 的 `ActiveConnectionGuard` 承担）。

### 1.2 接管：写什么、怎么还原

- **触发键是 per-app `proxy_config.enabled`**（`services/proxy.rs:1150 set_takeover_for_app`）。`enable_local_proxy` 只是首页面板显隐（`settings.rs:373`；前端 `src/components/settings/ProxyTabContent.tsx:130`）——**不看代码会误判成接管开关**。
- **写入形态**（`apply_claude_takeover_fields_with_policy_and_models`，`services/proxy.rs:486`）：
  - `env.ANTHROPIC_BASE_URL = http://127.0.0.1:15721`（`proxy.rs:507`，不带 `/v1`）；
  - 先删 12 个模型覆盖键（`CLAUDE_MODEL_OVERRIDE_ENV_KEYS`，`proxy.rs:32-45`；删除动作 `proxy.rs:509-511`）；
  - 再写**稳定别名** `ANTHROPIC_DEFAULT_{HAIKU,SONNET,OPUS,FABLE}_MODEL = claude-haiku-4-5 / claude-sonnet-5 / claude-opus-5 / claude-fable-5` 及 `*_MODEL_NAME`（`proxy.rs:47-50`、`proxy.rs:569-632`）；
  - 凭据键写占位 `PROXY_MANAGED`（`proxy.rs:25`；`proxy.rs:518-561`，分「保留原键」与「托管账号只留一个键」两种策略）。
- **别名 → 真模型由代理映射**（`proxy/model_mapper.rs:69 map_model`；fable 未配时降级到 opus，`model_mapper.rs:73-80`）。切 profile/换模型 = 代理侧换映射，**不必重写 settings.json**。
- **备份**：SQLite `proxy_live_backup(app_type PK, original_config, backed_up_at)`（`database/schema.rs:267-271`；DAO `database/dao/proxy.rs:779/810/847`）。备份前若发现 live 本身已是代理态则**跳过备份**（`services/proxy.rs:1848-1889`；严格版 `proxy.rs:1915-1946`）。
- **还原**：三层兜底 backup → SSOT 重建 → 清占位符（`restore_live_config_for_app_with_fallback_inner`，`services/proxy.rs:2303-2370`；SSOT 版 `proxy.rs:2408`）。备份自身若是代理态 → 弃用改走 SSOT（`proxy.rs:2329-2338`）。触发点：关接管（`proxy.rs:1340-1400`）、退出 `cleanup_before_exit`（`lib.rs:1903-1941`）、启动 `recover_from_crash`（`services/proxy.rs:2677`，调用 `lib.rs:1239`）。还原后删备份（避免长期存 token，`proxy.rs:1350-1356`）。
- **残留检测不看 DB 标志**：`detect_takeover_in_live_configs` 按「占位符还在不在」判（`proxy.rs:2701`）；`set_live_takeover_active` 已废弃 no-op（`database/dao/proxy.rs:489`），真状态 = per-app `enabled`。

### 1.3 鉴权与安全

- **客户端 token 完全不校验**：路由 handler 无 Authorization 检查；唯一校验点是 Claude Desktop 网关专用 `validate_claude_desktop_gateway_auth`（`proxy/handlers.rs:271`）。
- 占位符的用途是**反方向防御**：若解析出的上游凭据仍是 `PROXY_MANAGED`，直接报错拒发（`reject_proxy_placeholder_for_managed_account_upstream`，`proxy/forwarder.rs:3486-3499`；常量 `forwarder.rs:40`）。
- 监听默认回环但**不强制**（可配 `0.0.0.0`，`database/schema.rs:129`）。

### 1.4 故障转移

- **开关** `auto_failover_enabled`（`database/schema.rs:132`）；`set_auto_failover_enabled`（`commands/failover.rs:167`）要求先开代理接管（`failover.rs:188`），队列空则把当前供应商自动补为 P1 免死锁（`failover.rs:191-244`），开启前先真正切到 P1、失败回滚（`failover.rs:256-268`）。
- **队列**：`providers.in_failover_queue`（`database/dao/failover.rs:23,64`）。关闭=只用当前供应商；开启=**只用队列**（P1→P2…），`select_providers`（`proxy/provider_router.rs:53-116`）。
- **判据** `categorize_proxy_error`（`proxy/forwarder.rs:2777-2824`）：Timeout/ForwardFailed/ProviderUnhealthy 可重试；`UpstreamError` 中 400/405/406/413/414/415/422/501 **不**重试，其余（含 401/403/429/5xx）重试；AuthError 一般可重试，但 codex official / xai oauth 例外（换卡会串账号）。每次客户端请求最多试 `max_retries+1` 家（`forwarder.rs:187-190,261`；claude 默认 6+1，`schema.rs:154`）。
- **熔断**：`proxy/circuit_breaker.rs`（失败阈值 4 / 成功 2 / open 60s / 错误率 0.6 / 最小 10 次，`circuit_breaker.rs:64-73`）；**单 provider 时跳过熔断**（`forwarder.rs:461`）。
- **切换与回切**：**没有回切**。切到工作那家后调 `hot_switch_provider` 改「逻辑当前供应商」并落库（`forwarder.rs:557-575` → `proxy/failover_switch.rs:66 try_switch` → `services/proxy.rs:2962`），此后一直留在那家，直到用户手动切。
- **可见性**：`ProxyStatus.failover_count/current_provider/last_error/success_rate`（`proxy/types.rs:60-96`）+ 托盘菜单重建 + 前端事件 `provider-switched`（`failover_switch.rs:120-130`）。

### 1.5 用量 / 仪表盘

- **两路来源**：①代理侧解析响应/SSE（`proxy/usage/parser.rs:104,131,249,347,396` 各协议 `from_*`）+ 计价 `proxy/usage/calculator.rs` + 写库 `proxy/usage/logger.rs:446 log_with_calculation`；②**会话 JSONL 导入**（无代理模式）：`services/session_usage.rs:1-8` 数据流注释、`sync_claude_session_logs:187`（codex/gemini/grokbuild 各有独立文件）。
- **存储**：统一落 `proxy_request_logs`（`database/schema.rs:199-213`：input/output/cache_read/cache_creation tokens、费用（TEXT 存 Decimal）、latency_ms、**first_token_ms**、session_id、`data_source`）；日聚合 `usage_daily_rollups`（`schema.rs:279-297`）；定价 `model_pricing`（`schema.rs:238`）。
- **写入时机**：流式在流末 `spawn_log_usage`（`proxy/response_processor.rs:562` → `:625 log_usage_internal`）；首包时刻由 collector 记（`response_processor.rs:391 mark_first_collected_event_time`）。
- **跨源去重**：`services/usage_stats.rs:364 should_skip_session_insert`（request_id 已存在，或时间窗内存在 app/model/4 类 token 数指纹相同的 proxy 行）+ `session_usage_dedup` 表（`schema.rs:324-336`）——代理行与会话行不会重复计费。
- `usage_confirmed` **不是**数据新鲜度：是首次打开「用量脚本」的一次性用户同意（`settings.rs:379`；前端 `src/components/UsageScriptModal.tsx:478-490`）。

### 1.6 会话管理（`session-manager.md` + 实现）

- 解决的问题：会话记录散落难找、恢复命令要背、恢复时忘了当时工作目录（`session-manager.md:9-16`）。
- 机制：`src-tauri/src/session_manager/mod.rs` 里 `scan_sessions` 用 `std::thread::scope` 并行扫 8 个 CLI 的会话目录（`mod.rs:67-95`），统一成 `SessionMeta`（含 `project_dir / last_active_at / resume_command`，`mod.rs:11-24`）；Claude 侧扫 `~/.claude/projects/*.jsonl`（`session_manager/providers/claude.rs:17,32,88`）。恢复 = 复制/执行 resume 命令 + 带 cwd 起终端（`session_manager/terminal/mod.rs:3 launch_terminal`，8 种 macOS 终端 osascript）。
- **隐藏协同**：同一批 JSONL 也是它的用量数据源（§1.5 来源②），即「不开代理也能统计」。

## 2. 逐项对照表

| 维度 | cc-switch（file:line） | DevDesk 现状（file:line） | 差异判断 |
|---|---|---|---|
| HTTP 服务 | axum+hyper 手写 accept loop，peek 保 header 大小写（`proxy/server.rs:141-206`） | cpp-httplib `platform::http::Server`，固定绑定 127.0.0.1（`claude_service.cpp:1046-1047`，`claude_service.h:191`） | 相当。大小写透传我们不需要（头表本就大小写不敏感 + 黑名单，`proxy_router.cpp:36-57`） |
| 路由面 | 显式端点表，未登记路径 404（`proxy/server.rs:291-400`） | 5 method × `/(.*)` 全量转发（`proxy_router.cpp:102-115`） | 我们更宽松（新端点不会 404），代价是任意路径都打上游 |
| 流式 | `Body::from_stream` 透传，靠 async 流天然背压（`handlers.rs:536`；`sse.rs:7,37`） | 三线程 + 显式队列，背压 100/50 + 断流补 error 帧（`streaming_proxy.h:55-56`，`proxy_router.cpp:332-334`，`streaming_proxy.cpp:266`） | **我们更显式**（推式队列必须自己反压）；cc-switch 靠拉式自然背压，等价但少一层 |
| 超时 | per-app 可配（首包 60/90、静默 120/180、非流式 600；`schema.rs:134,154`）+ failover 关则禁用 + 流式 24h 上限（`forwarder.rs:2356-2359`） | 硬编码：非流式 120s、等响应头 30s、流式 1B/s×60s 低速、无总上限（`proxy_router.cpp:186,332-334,371`） | 我们够用；cc-switch 的「可配 + 门控」在单 provider 时无收益 |
| 连接复用 | 非保大小写路径走 reqwest 连接池（`http_client.rs:216-222`）；保大小写路径每次 raw TLS（`forwarder.rs:2346-2352` 注释） | **无复用**：每请求 `curl_easy_init/cleanup`（`platform/http_client.cpp:70,111,445,553`） | **我们更弱**：每请求重做 TCP+TLS（远端 1-3 RTT） |
| 接管写入 | BASE_URL + 删 12 个模型覆盖键 + 稳定别名 + PROXY_MANAGED（`services/proxy.rs:486-561`） | BASE_URL + PROXY_MANAGED + 6 槽位**真实模型名** + 用户 extra env（`provider_config.cpp:355-395`） | 我们多写了用户 extraEnv（更强）；**但没有「删上一轮残留模型键」这一步**（见 §3.1） |
| 快照载体 | SQLite 表（`schema.rs:267-271`） | sidecar JSON（`settings_sync.h:11-25`；`settings_sync.cpp:195-250`） | 相当；sidecar 对用户可读可手救 |
| 还原兜底 | backup → SSOT 重建 → 清占位符；备份自身是代理态则弃用（`proxy.rs:2303-2370,2329-2338,2408`） | 键级还原；**无 sidecar 即 no-op**（`settings_sync.cpp:296-299`），sidecar 损坏则中止保留（`:302-309`） | **我们更弱**：sidecar 丢失 + settings.json 仍是代理态 → 永久指向死代理（见 §3.2） |
| 崩溃恢复 | 启动 `recover_from_crash`（`proxy.rs:2677`/`lib.rs:1239`）+ 退出 `cleanup_before_exit`（`lib.rs:1903`） | 启动 `recoverPendingSettingsRestore`（`claude_service.cpp:1160`）+ `stopProxy`→`syncRestore`（`:1083`） | 相当，但同样以 sidecar 存在为前提 |
| 鉴权 | 不校验入站；靠「别把占位符转发出去」后置守卫（`forwarder.rs:3486-3499`） | 已移除鉴权（`claude_service.h:191`）；转发前擦除客户端凭据再注入 profile key（`upstream_dialect.cpp:246-255`） | **我们更强**：占位符构造上不可能出站 |
| 故障转移 | 队列 + 错误分类 + 熔断 + 热切换，无回切（§1.4） | 无（单 active profile，一请求一次转发，`proxy_router.cpp:118-274`） | 我们缺一整块；是否需要见 §5.1 |
| 用量 | tokens + 费用 + first_token + 日聚合 + 会话导入 + 跨源去重（§1.5） | 进程内速率卡（`core/proxy_stats.h:17-22`）+ 调用日志无 token/费用（`core/migrations.h:104-116`） | **我们弱**：无历史用量；但 `SseUsageParser` 已在手（`streaming_proxy.cpp:209,334`），缺的只是落库 |
| 会话管理 | 8 CLI 会话浏览 + resume 命令 + 终端拉起（§1.6） | 无 | 需新模块；最省的切入口是「JSONL → 用量」（见 §3.5） |

## 3. 值得抄的（函数级，按价值排序）

1. **`settings_sync::takeOver()`（`settings_sync.cpp:195`）：写 env 前先删「上一轮托管但本轮不再生成」的键。**
   我们只在 `managed` 里 set（`:266-268`），靠 sidecar 的 managedKeys 并集（`:230-240`）保证最终能还原；但槽位被清空后（`buildProxyEnv` 对空槽位直接 skip，`provider_config.cpp:382-383`）旧模型名会**继续留在 settings.json 里**，直到下次还原——客户端会拿旧模型名请求新供应商。cc-switch 是每轮先 `env.remove()` 12 个覆盖键再写（`services/proxy.rs:509-511`）。
   代价：约 10 行 + 1 个单测（清空槽位后 re-takeover，断言旧键消失）。
2. **快照污染守卫 + 无 sidecar 兜底（`takeOver()` 与 `restore()`，`settings_sync.cpp:195,294`）。**
   规则：接管前扫现有 env，若 `ANTHROPIC_BASE_URL` 指向回环地址**或**任一凭据键等于 `kProxyAuthPlaceholder`（`provider_config.h:125`），则这些键**不进 envSnapshot**（它们是别的工具/上次残留的托管值，不是用户原值）；`restore()` 在无 sidecar 但 env 仍带上述特征时，走「按 active profile 重建 env（复用 `provider_config.cpp:355 buildProxyEnv`）或直接删键」的兜底，而不是 no-op。
   为什么值：现在是 `restore()` 无 sidecar 直接 no-op（`:296-299`）、`recoverPendingSnapshot` 也以 sidecar 为前提（`:380-386`），且 `isClaudeCodeManaged()` 只反映 sidecar（`claude_service.h:246-249`）——用户清 AppData、或与 cc-switch 互抢 settings.json 时，会静默卡在死代理且 UI 显示「未接管」。cc-switch 对应三层守卫：`services/proxy.rs:1848-1889`（备份前跳过）、`proxy.rs:2329-2338`（备份本身可疑则弃用）、`proxy.rs:2408`（SSOT 重建）、`proxy.rs:2701`（按占位符判残留）。
   代价：约 40 行 + 4 个单测（临时目录）。
3. **上游连接复用：`platform/http_client.cpp` 的 `send()` / `sendStreaming()` 改用 thread_local easy handle 池。**
   现在每请求 `curl_easy_init`→`curl_easy_cleanup`（`:70,111`、`:445,553`），每次转发都重做 TCP+TLS；cc-switch 在不需要保 header 大小写的路径直接用 reqwest 池（`forwarder.rs:2346-2352`+`http_client.rs:216-222`）。
   代价：约 40 行（`curl_easy_reset` 复用 + 每线程一份，注意别跨线程共享同一 easy handle）+ 回归「连续两个请求」用例。收益：远端网关每次请求省 1-3 RTT。
4. **用量落库最小版：`claude_call_logs` 加 token 列 + 流末写入。**
   `SseUsageParser`（`streaming_proxy.cpp:209`）与 `core/proxy_stats.h` 已有累计 token，在 `forwardStream`（`streaming_proxy.cpp:320`）收尾时把该流的 input/output/cache tokens 与首事件毫秒写进 call log；不加费用、不加聚合。cc-switch 对照实现：`proxy/response_processor.rs:562,625` + 列定义 `database/schema.rs:199-213`。
   代价：migration 加 3-4 列（`core/migrations.h`，参照 `:104-116`）+ 一次 INSERT 传参，约 60 行 + 测试。**不建议**顺带做费用/定价（见 §4）。
5. **会话 JSONL → 用量（复用面最窄的一条）：扫 `~/.claude/projects/*.jsonl`，解析 usage 后按 `data_source='claude_session'` 写同一张用量表。**
   价值：用量统计不该以「代理开着」为前提；cc-switch 靠 `data_source` 列 + 指纹去重让两条来源并存（`services/usage_stats.rs:364`、`schema.rs:324-336`）。
   代价：JSONL 解析 + 增量游标（cc-switch 用 `session_log_sync` 存字节游标，`schema.rs:309`）+ 去重键，1-2 天。做成后同时喂「用量」和「会话浏览」两个需求。
6. **（条件性）错误分类 + 首包前换一次 profile。**
   值钱的是那张分类表：400/405/406/413/414/415/422/501 不该换家重试，401/403/429/5xx 该（`forwarder.rs:2777-2824`）。摘成纯函数 `shouldRetryWithOtherProfile(status/err)`，只在响应头还没回给客户端时、且存在备用 profile 时换一次。
   代价：得把 `proxy_router.cpp:118-274 / 304-431` 的「一请求一 profile」结构改成「候选列表」——本清单里唯一动结构的项，**先拍板要不要多 key**（§5.1）。

## 4. 不该抄的

- **多 app / 多协议矩阵**：codex/gemini/grokbuild 四套接管写入 + 各协议 transform/streaming 转换（`src/proxy/providers/*`；整个 `proxy/` 模块 24,604 行）。我们只服务 Claude Code 一种客户端，抄了就是 800 行变 2 万行。
- **raw TCP + `preserve_header_case` 保原始 header 大小写**（`server.rs:141-206` + `hyper_client.rs` 942 行）：为把客户端 header 大小写原样透给上游；我们用黑名单 + 小写比较（`proxy_router.cpp:36-57`），没有这个需求。
- **熔断器 + HalfOpen 探测名额 + 整流器/媒体消毒/thinking 预算修正**（`circuit_breaker.rs` 495 行；`forwarder.rs` 5,525 行）：单 key 场景零收益。
- **计价体系**：`model_pricing` 表 + 文本 Decimal + 自定义定价 + 日聚合 rollup + 账单审计（`proxy/usage/calculator.rs`、`services/model_pricing.rs`、`dao/usage_rollup.rs`）。我们只要「跑了多少 token」。
- **会话管理的终端矩阵**（`session_manager/terminal/mod.rs:3-440`：osascript/kitty/wezterm/ghostty/otty/warp…）：Windows 上真正可用的只有 `wt.exe` / `cmd start`。
- **会话扫描 8 个 CLI**（`session_manager/mod.rs:67-95`）：我们只需 Claude Code。
- **外围**：`usage_script`（rquickjs 跑用户 JS 拉额度）、WebDAV/S3 同步、deep link、OMO/copilot/xai OAuth、Claude Desktop 网关——与「本地代理」无关。

## 5. 待拍板项

1. **要不要多供应商故障转移？** 要 → 按 §3.6 做「错误分类 + 换一次 profile」，先不碰队列/熔断/顺序 UI；不要 → §3.6 直接砍掉。
2. **客户端模型名策略**：继续写真实模型名（现状，`/model` 里能看见真名）还是改成 cc-switch 式稳定别名 + 代理映射（换 profile 不必重写 settings.json，但菜单显示 `claude-sonnet-5`）？两端证据：`provider_config.cpp:382-395` vs `services/proxy.rs:569-632` + `model_mapper.rs:69`。
3. **用量落库范围**：只记 token（§3.4）还是含费用估算？后者要引入定价表与维护成本。
4. **会话模块是否立项**：立项则先做「JSONL → 用量」（§3.5，复用面窄），浏览器 UI + 一键恢复另排。
5. **与 cc-switch 共存策略**：本机同时装两者时谁让路？我们已有 `setSettingsTakeOverEnabled` 手动开关（`claude_service.h:238-239`）；是否要在 §3.2 的「检测到别的工具已接管」时默认不自接管并提示？

## 附：存疑 / 待验证

- cc-switch 客户端断连时是否显式 cancel 上游：只确认了在途计数 `ActiveConnectionGuard` 与 `_conn_guard` 随流 drop，未见显式 abort —— 待验证。
- cc-switch `enable_logging`（`schema.rs:131`）与 `usage_logging_enabled`（`response_processor.rs:610`）只门控「是否写用量行」，是否还有别的功能影响 —— 待验证；对我们无影响。
- §1.4「无回切」结论基于全仓 grep（`回切/switch_back/preferred` 无命中）与 `hot_switch_provider` 读写路径；若后续版本加入定时探测回切需重查。
