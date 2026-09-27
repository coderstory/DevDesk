# cc-switch 参照研究：Claude provider 的存储 / 切换 / 反向同步

日期：2026-09-27 ｜ 性质：**只读对照研究**（未改任何代码）｜ 对象：`D:\project\gui\cc-switch` v3.20.4（HEAD `1ee2fdc3`）
关联：`../specs/2026-09-27-profile-reverse-sync-and-copy-design.md`（R1/R2）、`../specs/2026-09-22-claude-provider-config-design.md`

> 行号会漂移：每条都写「符号名 + 行号」，核对时按符号名搜索。
> **一处前置更正**：cc-switch 的 `sessionAutoSyncEnabled` **不是** provider 反向同步开关——它是「会话用量日志自动扫描」开关（`settings.rs:382-386` 注释 + `lib.rs:1284-1310` 的 60s 定时器，只读客户端会话文件算 token 费用）。provider 反向同步另有其路（见 §1.3）。

---

## 1. cc-switch 机制速览

### 1.1 数据模型与存储

| 事实 | 证据 |
|------|------|
| 模型：`Provider { id, name, settingsConfig: Value, category, notes, meta, icon, iconColor, inFailoverQueue }`——**整份 settings 语义**（Claude 下就是 `{env, statusLine, hooks, permissions…}` 原文） | `src-tauri/src/provider.rs:11`（struct）／`:444`（`ProviderMeta`，含 `commonConfigEnabled`） |
| 存储：SQLite `~/.cc-switch/cc-switch.db`；表 `providers(id, app_type, name, settings_config TEXT, …, meta TEXT, is_current, in_failover_queue)`，**主键 (id, app_type)** | `src-tauri/src/database/mod.rs:99-101`（路径）、`database/schema.rs:27-43`（DDL） |
| 早期 JSON（`~/.cc-switch/config.json`）已迁 SQLite；`provider.rs:30` 注释"仅存于 config.json"疑为**过时残留**（见文末存疑） | `database/dao/providers.rs:180 save_provider`、`:111 get_current_provider`、`:389 set_current_provider` |
| **密钥明文**：key 就写在 `settings_config` JSON 的 `env.ANTHROPIC_AUTH_TOKEN` 里，整库落盘；`Cargo.toml` 依赖段无 keyring/stronghold 类 crate；导出 = 整库 SQL dump（含明文 key） | `src-tauri/Cargo.toml:37-121`（依赖全表）、`database/backup.rs:118 export_sql_string`、`:130 export_sql`、`commands/import_export.rs:32 export_config_to_file` |
| 通用配置片段（跨 provider 共享的非机密配置）存 SQLite kv：key = `common_config_{app}`；另有 `common_config_{app}_cleared` 显式清空标记 | `database/dao/settings.rs:62 get_config_snippet`、`:67 is_config_snippet_cleared`、`:121 set_config_snippet` |

### 1.2 切换流程：写什么、怎么写

| 事实 | 证据 |
|------|------|
| 入口 `switch_provider` → `ProviderService::switch` | `commands/provider.rs:119`、`services/provider/mod.rs:5712` |
| **接管改道**：若存在 live 备份或 live 里含接管占位符 → 走 `hot_switch_provider_inner`，**不做回填、不重写 live**（live 归代理所有） | `services/provider/mod.rs:5760-5795` |
| Claude 写 live = **整份文件全量重写**（不是合并）：`sanitize_claude_settings_for_live(provider.settings_config)` → 原子写 `~/.claude/settings.json` | `services/provider/live.rs:1312 write_live_snapshot`（Claude 分支 `:1314-1317`）、`config.rs:370 write_json_file`、`:383 atomic_write`（temp+rename，键排序） |
| 写之前把**通用配置片段 deep-merge 进 provider 配置**（`json_deep_merge`）：片段来自 DB，与 provider 自身字段冲突时片段覆盖 | `services/provider/live.rs:709 write_live_with_common_config_for_state` → `:678 build_effective_settings_with_common_config` → `:617 apply_common_config_to_settings` |
| 片段是否生效：`meta.commonConfigEnabled` 显式优先；缺省时按"provider 配置里是否已含片段"推断 | `services/provider/live.rs:541 provider_uses_common_config` |
| **非 env 字段（statusLine / enabledPlugins / permissions / hooks）不靠"写入时合并"保留，而是靠"切走前回填"**：切走时整份 live（去掉片段与注入产物）存回上一 provider 的 DB 行。即"settings.json 现在长啥样 = 当前 provider 的配置" | `services/provider/mod.rs:5836-5876`（回填段）、`services/provider/live.rs:952 strip_common_config_from_live_settings` |
| 提交顺序（普通路径）：先写 current（本地 + DB）→ 再写 live；写 live 失败只报错，current 已落库 | `services/provider/mod.rs:5960-5975` |
| 用户在 UI 里编辑"当前 provider" → 只把 DB 推向 live（`sync_live_for_provider_respecting_takeover`），**不从 live 反读** | `services/provider/mod.rs:5207 update`、`:5545-5560`、`services/provider/live.rs:1595` |

### 1.3 反向同步（settings.json → provider）

| 事实 | 证据 |
|------|------|
| **时机 1（主）**：切到别的 provider 之前，对"当前 provider"做回填——读 live → 先重提取通用配置片段 → 剥掉片段 → 存回 DB 行；失败只记 `warnings.push("backfill_failed:{id}")`，**不阻断切换** | `services/provider/mod.rs:5836-5876`（`if let Ok(live_config) = read_live_settings(...)`） |
| **时机 2**：启动时/手动"导入当前配置"——把 live 收成一个名为 `default` 的 provider 并设为当前（仅在"没有任何非官方 seed provider"时） | `services/provider/live.rs:1867 import_default_config`、`:2035 should_import_default_config_on_startup`、`commands/provider.rs:212`、前端 `src/lib/api/providers.ts:94 importDefault` |
| **时机 3**：通用配置片段的"整体重提取 + 替换"（不是只并新增）——切走时用 live 重新提取片段，被删的键会从片段里消失；失败非致命 | `services/provider/mod.rs:6196-6290 sync_common_config_snippet_from_live`（含三条设计理由的注释） |
| 提取器剥什么：env 里模型/端点键（`ANTHROPIC_MODEL`、4 档 `*_MODEL`、`CLAUDE_CODE_SUBAGENT_MODEL`、`CLAUDE_CODE_MAX_CONTEXT_TOKENS`、`ANTHROPIC_BASE_URL`…）+ **任何凭据键**（模式匹配后缀 `_KEY/_API_KEY/_TOKEN/_PAT/_PWD/_PASS…` 与非分隔符写法的 `_APIKEY/_ACCESSKEY/...`，含"复数 `_TOKENS` 不误伤"的例外）＋顶层 `apiBaseUrl/primaryModel/smallFastModel` | `services/provider/mod.rs:6408 extract_claude_common_config`、`:6349 is_sensitive_config_key` |
| **守卫 A（本地代理形态）**：不靠 URL 判定，靠**占位符**——live 的 `ANTHROPIC_AUTH_TOKEN / ANTHROPIC_API_KEY / OPENROUTER_API_KEY / OPENAI_API_KEY` 等于 `PROXY_MANAGED` 即判"被接管"；命中后 `switch()` 直接改道，根本不进回填 | `services/proxy.rs:25 PROXY_TOKEN_PLACEHOLDER`、`:2729 is_claude_live_taken_over`、`:2381 detect_takeover_in_live_config_for_app` |
| **守卫 A'（写侧所有权三条件）**：`enabled && has_live_backup && 代理在跑`（或切换进行中）才认为 live 归代理，避免"中断的 teardown"被误判 | `services/provider/live.rs:1551 proxy_owns_live_config`（`:1582` 起三条件） |
| **守卫 B（导入侧）**：live 处于接管态 → 拒绝导入并给可操作文案（理由：占位符会被固化成 provider，之后写回 live 永久指向失效代理） | `services/provider/live.rs:1886-1900`（`provider.import.live_taken_over`） |
| **剥离注入产物**：回填时剥掉"程序注入、不属于用户"的键——内置上下文默认值、Codex 托管 OAuth 占位 auth、DB 所有的 `modelCatalog`/`mcp_servers`/统一会话桶等（逐项注释：注入产物若落库会固化成"用户显式值"） | `services/provider/live.rs:1057 restore_live_settings_for_provider_backfill`、`:999 strip_injected_codex_oauth_context_defaults`、`:1175 strip_codex_managed_oauth_auth_for_backfill` |
| **坏 JSON**：回填路径 `if let Ok(...)` → 静默跳过（live 坏 → provider 行原样）；导入路径直接报错返回 | `services/provider/mod.rs:5850`、`services/provider/live.rs:1928`（Claude 分支 `read_json_file(...)?`） |
| **空值 / 缺失键**：无"空值守卫"。整份 live 写回 ⇒ **缺席即清空是隐式行为**（用户删掉的键从 provider 配置里消失）；只有接管形态与注入产物会被剥 | 同上两行（无空值判定分支；`is_local_proxy_url` 只在代理还原/清理路径使用：`services/proxy.rs:2585/2623/2640`） |
| 循环保障：每次写 live 都把片段合并进去 ⇒ 切走时读到的 live 一定是"片段 + 本地改动"的超集，故"整体替换片段"不会误删其它 provider 共享内容 | `services/provider/mod.rs:6205-6211`（注释论证） |

### 1.4 复制 / 导入 / 导出

| 事实 | 证据 |
|------|------|
| **复制**：前端 `handleDuplicateProvider`；名称 = `provider.name + " copy"`（英文硬编码，**不查重**）；`settingsConfig` 深拷贝（**含 key**）；`sortIndex + 1`；**副本不激活**（只有该 app 一个 provider 都没有时 `add` 才会把它设为 current 并写 live） | `src/App.tsx:818-919`（命名 `:821`）、`services/provider/mod.rs:5186-5195`（`add` 里 `current.is_none()` 才激活） |
| 累加模式的 app 复制时会另生成 `providerKey`、`addToLive=false`（避免 DB-only 副本混进 live） | `src/App.tsx:873-893`、`:910-913` |
| **导入**：空态按钮 → `import_default_config`；`default` 是固定 id/name；已有非 seed provider 则跳过；拒绝接管态 live | `commands/provider.rs:212`、`services/provider/live.rs:1867`（`:1878` 跳过、`:1886` 拒绝） |
| **导出/导入文件** = 整库 SQL（含 key 明文；导入 = 整库替换 + 事后同步）；另有深链（URL / `config=base64`）导入路径，非标 env 键保留 | `commands/import_export.rs:32/53`、`deeplink/provider.rs:311 build_claude_settings` |
| **DB→live 的"同步"**（`sync_current_providers_live`）是**单向推送**：设置页/导入导出后调用；接管中只更新备份文件 | `commands/import_export.rs:82`、`services/provider/live.rs:1696 sync_current_to_live`、前端 `src/utils/postChangeSync.ts` |

### 1.5 多 app 隔离

| 事实 | 证据 |
|------|------|
| 10 个 app 各自一份 provider 列表：Claude / ClaudeDesktop / Codex / Gemini / GrokBuild / OpenCode / OpenClaw / Hermes / Pi / Mcode；`app_type` 是 providers 表的一部分，每家一个 `is_current` | `src-tauri/src/app_config.rs:396-425`（enum + `as_str`）、`database/schema.rs:27-43` |
| 两种语义：切换模式（Claude/Codex/Gemini，只有当前 provider 进 live）vs 累加模式（OpenCode/OpenClaw/Hermes/Pi/Mcode，所有 provider 共存于原生配置） | `app_config.rs:441-446 is_additive_mode`、`:448-453 supports_local_proxy` |
| 共享件 per app：通用配置片段 key、live 备份、代理开关都按 app 分键 | `database/dao/settings.rs:62`、`services/provider/live.rs:1551` |
| 另有**跨 app 统一供应商**（一份定义投影到多个 app）：`universal_providers` 存 settings kv，`sync_universal_to_apps` 投影 | `database/dao/universal_providers.rs:11`、`services/provider/mod.rs:7482`、`:7611 merge_json` |

---

## 2. 逐项对照

| # | 维度 | cc-switch 做法 | DevDesk 现状 | 差异 / 结论 |
|---|------|----------------|--------------|-------------|
| 1 | 存储 | SQLite `~/.cc-switch/cc-switch.db`，一行 = 整份 settings JSON；key 明文 | SQLite `claude_profiles` + Keychain 存 key（`claude_service.cpp:432-441 updateProfile` 先 Keychain 后 UPDATE） | DevDesk 密钥面**更好**（它明文落盘 + 明文导出）；数据面各存各的：它存"整份 settings"，我们存 base_url/槽位/extra_env 的结构化字段 |
| 2 | 密钥存哪 | `env.ANTHROPIC_AUTH_TOKEN` 明文进 DB（`provider.rs:11`、`schema.rs:27`） | OS Keychain（`claude_service.cpp:432`；反向同步也走 Keychain `:1276-1290`） | 不抄 |
| 3 | 切换写 live | **整份文件全量重写**（`live.rs:1314-1317`）+ 片段 deep merge（`live.rs:617`） | **只改写托管键**（BASE_URL / 占位 token / 模型槽位 / extra_env），其余键与非 env 字段原样保留（`settings_sync.h:4-27` 读-改-写协议、`settings_sync.cpp:195 takeOver`） | 我们更保守也更安全（不承担"整份文件归我"的责任）；这也正是它需要通用配置片段的原因 |
| 4 | 非 env 字段保留 | 靠"切走前回填整份 live"（`mod.rs:5836`） | 从不进 profile（settings.json 的非 env 字段天生不属于 profile） | 无需抄；若将来要把 statusLine/hooks 纳入共享才需要 §3-4 |
| 5 | 通用配置 | 独立片段层：opt-in 标记 + `_cleared` 标记 + 切走整体重提取 + 密钥模式剥离（`settings.rs:62/67`、`mod.rs:6196/6408/6349`） | 无此层（无跨 profile 共享片段概念） | 我们**没有**对应需求；一旦要做必须整套抄（尤其 `_cleared`），见 §3-4 |
| 6 | 反向同步时机 | 切走前（`mod.rs:5836`）+ 启动/手动导入（`live.rs:1867`） | `setActiveProfile` 内、事务前（`claude_service.cpp:644-650` 调 `syncCaptureActiveProfile`）+ UI「导入当前配置」（`claude_module.cpp:454`、`:1420`） | **语义等价**（都是"切换前收编当前 live"）；我们的触发点更集中（唯一咽喉） |
| 7 | 缺席语义 | 隐式清空（整份写回，无空值守卫） | 显式「出现覆盖 / 缺席清空 / 守卫键不动」（`provider_config.cpp:622 mergeReverseSyncEnv`，用 `skippedKeys` 区分 G1–G4/G6） | 我们更显式（它连"删了就没了"都没当设计点写）；保留我们这套 |
| 8 | 本地代理形态守卫 | **占位符**判定（`proxy.rs:2729`）+ 写侧三条件（`live.rs:1551`）；裸 loopback URL 而无占位符时**不拦**（`is_local_proxy_url` 只用于代理还原路径） | **URL 形态**判定：`isLoopbackUrl` 命中 → BASE_URL 与该次 token 都不写也不清（`provider_config.cpp:543-565`）；占位符另判（`:566-590`） | 我们的 G2 **比它严**（它漏"裸 loopback"这一形态，因为它自己就是 loopback 的生产者所以无感）；但"接管态改道而非硬写"的思路值得抄（§3-1） |
| 9 | 坏 JSON | 回填静默跳过 / 导入报错（`mod.rs:5850`、`live.rs:1928`） | 不存在 / 空文件 / 无 env → no-op 成功；解析失败或 env 非对象 → **中止本次切换**（`settings_sync.cpp:391-427`、`claude_service.cpp:1190-1195`） | 策略不同（我们按 S-2 中止，用户已拍板）；我们"读侧三分"比它细 |
| 10 | 回填失败策略 | 只 warning，切换照常（`mod.rs:5876`） | 中止切换（S-2，`claude_service.cpp:645-648`） | 用户已拍板，不抄 |
| 11 | 失败可见性 | `SwitchResult.warnings` 透传到前端 toast（`mod.rs:5876`、前端类型 `src/lib/api/providers.ts:20`） | `lastSyncWarnings_` 有字段有 getter（`claude_service.h:321`、`claude_service.cpp:1157`），但**全仓零消费者**；`syncTakeOver/syncRestore` 失败只 `LOG_WARN`（`:1351`、`:1375`） | **真缺口**，见 §3-2 |
| 12 | 复制 | 名称 `${name} copy`、不查重、含 key、不激活（`App.tsx:818`、`mod.rs:5186`） | `duplicateProfile`：中文「副本」+ 递增避让 + 100 次上限（`provider_config.cpp:682 makeCopyName`）、源 key 缺失即 fail（`claude_service.cpp:716`）、不激活（`:733`）、二段补写失败回滚（`:745-762`） | 我们更强；但 **service 层已实现、UI 未接线**（`grep duplicateProfile src/` 仅命中 `claude_service.cpp:688` 与头文件）→ R2 只剩 UI |
| 13 | 导入/导出 | 整库 SQL（含明文 key）+ 深链；无 profile 级分享 | 无导出；复制 = 含 key 全复制（S-3） | 若将来做"导出/分享 profile"，必须抄 `is_sensitive_config_key` 的模式剥离（`mod.rs:6349`） |
| 14 | 多 app | 10 app × 2 模式 + 跨 app 统一供应商投影（`app_config.rs:396`、`mod.rs:7482`） | 只有 Claude（含 claude_proxy 模块）；无跨 app 概念 | 不抄（YAGNI） |

---

## 3. 值得抄的（按价值排序，函数级）

1. **`syncTakeOver()` 前加"外部已接管"探测**（`claude_service.cpp:1333`）
   行为：新增纯函数（如 `isLiveExternallyManaged(envJson)`，复用 `isLoopbackUrl` + `kProxyAuthPlaceholder`），命中"BASE_URL 是 loopback 但**不是本机代理端口**"或"token=占位符但 sidecar 无快照"时，`syncTakeOver` 跳过抢写并记 warning。
   理由：cc-switch 对同场景是**改道不硬写**（`mod.rs:5760-5795`、`live.rs:1551`）；本机确实同时装 cc-switch（`claude_service.cpp:1337-1338` 注释已承认这个用户形态），现在我们会把它的 loopback 值当"用户值"覆盖掉。
   代价：~30 行纯函数 + 2 处 guard + 单测；要点是端口判定精确（只认 `127.0.0.1:<proxyPort()>` 之外的 loopback 视为外部）。

2. **把 `lastSyncWarnings_` 接到 UI**（`claude_service.h:321`、`claude_service.cpp:1157`）
   行为：`syncTakeOver`/`syncRestore` 的 `!r.ok` 分支（`:1351`、`:1375`）把错误并入 `lastSyncWarnings_`；在 Claude 页用既有警告元素（`claude_module.cpp:1658` 同款样式）展示。
   理由：cc-switch 把 `backfill_failed:{id}` 放进 `SwitchResult.warnings` 透传前端（`mod.rs:5876`）；我们现在"接管失败/被上层覆盖"只进日志，用户只看到"已启用"。
   代价：小（字段与 getter 已在，只差消费方）。

3. **`setActiveProfile` 里把"写 live 失败"从静默升级为可见 / 可回退**（`claude_service.cpp:680-682`）
   行为：让 `syncTakeOver()` 返回 `Result` 并在此分支处理——失败至少经 §3-2 上报；更彻底的做法是学 cc-switch 的 preflight：写前只读校验目标配置，失败在 DB 改动之前返回（`live.rs:728 preflight_codex_live_write_for_state`）。
   理由：现在 DB 已切换而 settings.json 里模型槽位仍是旧 profile 的值（代理按 DB 的 active 转发，故只是"槽位陈旧"而非错路由，但用户会看到模型没跟着切）。
   代价：小-中（改 `syncTakeOver` 签名 + 一处调用点；测试补"失败但切换仍完成"）。

4. **若将来做"共享 / 通用配置片段"，整条抄，不要只抄合并**（`mod.rs:6196-6290`、`settings.rs:62/67/75`）
   行为：新增 per-app 片段存储 + `meta` 上 opt-in + `_cleared` 标记；切 profile 时"整体重提取 + 替换"而非"只并新增"；提取时用模式匹配剥离凭据（`mod.rs:6349`）。
   理由：它注释里那两条反直觉 bug（"插件怎么删也删不掉"、"用户清空后又被塞回"）正是这套设计要解决的（`mod.rs:6205-6211`、`:6241-6248`）。
   代价：中（新 DB 键/列 + UI + 提取器 + 密钥剥离单测）；**仅当拍板做共享层时启动**。

5. **守卫表单测化**：把 G1–G6（`provider_config.cpp:517-590` 的六处判定）在 `tests/test_modules/test_provider_config.cpp` 里做成"每守卫一条断言"的清单，并保持头文件里的 G 编号注释同步。
   理由：cc-switch 把"哪些键算凭据"做成模式匹配而非名单（`mod.rs:6349` 注释逐条解释了 `_KEY` / `_APIKEY` / `_TOKENS` 三类写法的漏网场景）——增键时不会漏；G 表是 R1 的安全面，值得同等纪律。
   代价：小（补测试 + 注释）。

6. **快照回滚三件套**（`live.rs:1257` `CodexLiveStateSnapshot::restore`、`mod.rs:5925-5945` 事务错误处理）
   行为：若将来一次切换要写多个文件（settings.json + `~/.claude.json` 等），改成"capture 快照 → 全部写入 → 任一失败按快照恢复 → 才提交 DB"。
   理由：cc-switch 只对"token 解析可能失败"的托管 Codex 路径用这套（`mod.rs:5917-5958`），是"多写点必须原子"的现成样板。
   代价：中；单文件写场景用不上，**推迟**。

7. **`is_sensitive_config_key` 的模式匹配**（`mod.rs:6349-6405`）用于将来任何"导出 / 分享 / 公共片段"出口
   理由：我们的复制是含 key 的（S-3 有意为之），但一旦有"分享出去"的出口就必须剥离；照它的后缀表抄能覆盖 `*_APIKEY/_ACCESSKEY/_PAT/_PWD` 与"复数不误伤"例外。
   代价：小（纯函数 + 单测），**前提是先有出口**。

8. **导入路径的"接管态"处理对齐**（`live.rs:1886-1900` vs `claude_module.cpp:1658`）
   行为：cc-switch 在 `import_default_config` 直接**报错拒绝**（理由：占位符会被固化成 provider）；我们现在只在导入表单给警告不阻断。建议改为默认拒绝 + "仍要导入"显式二次确认。
   代价：小；但产品口径需拍板（见 §5-3）。

---

## 4. 不该抄的

| 项 | 为什么不抄 |
|----|-----------|
| 10 app 的 `AppType` + 切换/累加双模式（`app_config.rs:396-453`） | 我们只做 Claude；双模式是它 10 个客户端的产物 |
| 跨 app「统一供应商」投影（`dao/universal_providers.rs:11`、`mod.rs:7482`） | 单 app 下等于多一层 SSOT + 投影同步 |
| 代理接管的完整状态机（live 备份 + hot switch + stale backup 跟随 + 三层探测，`mod.rs:5760`、`live.rs:1551/1583`） | 只为"接管期间切 provider 也不改写 live"服务；我们的 takeOver 就是改 settings.json 的托管键，语义不同，硬搬会引入双真相源 |
| failover 队列 / 健康探测 / 用量仪表盘 / WebDAV·S3 同步 / 整库 SQL 导出 / 深链 / MCP·Skills·Prompts 管理 | 与 R1/R2 无关，各自是独立产品面 |
| `session_auto_sync_enabled` 那套会话日志扫描（`settings.rs:382`、`lib.rs:1284`） | 名字易误导（**不是**配置同步）；实际是把客户端会话日志算成 token 费用 |
| 把"整份 settings.json 当 provider 配置"的模型（`live.rs:1314` 全量重写） | 我们已定"profile = 托管字段结构化存储，settings.json 非 env 字段不归 profile"（`settings_sync.h:4-27`）；换成整份快照会让 statusLine/hooks 进 profile 并互相覆盖 |
| 每 app 的切换锁 + "切换进行中"标记（`live.rs:1551` 末段 `is_switch_in_progress_for_app`） | 单 UI 线程 + 我们的转发只读 DB active，竞争面不同；等真出现竞态再上 |
| 复制时的 live-id 冲突探测（`App.tsx:873-893`） | 我们的唯一性约束在 profile 名（`makeCopyName` 已处理），没有 live key 概念 |

---

## 5. 待拍板

1. **反向同步失败策略保持 S-2（中止切换）还是学它的"warning 放行"？** 我们已实现 S-2（`claude_service.cpp:645-648`）；若采纳 §3-1 的"外部接管探测"，被接管场景该走"跳过 + 放行"（不阻断）还是仍中止？
2. **是否要做"跨 profile 共享配置片段"层？** 不做则 §3-4 / §3-7 不启动；做则必须连带 opt-in + `_cleared` + 整体重提取 + 凭据模式剥离。
3. **「导入当前配置」遇到接管态 settings.json**：保持现状（警告不阻断，`claude_module.cpp:1658`）还是改为默认拒绝 + 显式确认（cc-switch 口径 `live.rs:1886`）？
4. **首启无 profile 时是否自动从 settings.json 建一个 profile**（cc-switch 的 `should_import_default_config_on_startup`，`live.rs:2035`）？它只在"零 provider 行"时触发。
5. **`lastSyncWarnings_` 上 UI 的形态**：顶部常驻警告条（可多轮可见）还是"启用"后的 toast（一次性）？这决定 §3-2 的落点（`claude_module.cpp` 状态栏 vs `showToast`）。

---

### 存疑 / 未验证

- `provider.rs:30` 注释称 meta"仅存于 `~/.cc-switch/config.json`"，与 SQLite SSOT 冲突（`schema.rs:39` 有 meta 列）——疑为过时注释，未逐行核迁移链（`database/migration.rs`）。
- cc-switch 读取 `~/.claude/settings.json` 存在旧名回退（`config.rs:243-256` 兼容 `claude.json`）；是否影响"整份重写"行为未验证。
- 未做运行时验证（不跑构建 / 测试 / app）；以上均为源码静态结论。DevDesk 侧 R1/R2 代码已完成但**未在本机运行验证**（`tests/test_modules/test_provider_config.cpp`、`test_claude_service.cpp` 有对应用例，未执行）。
