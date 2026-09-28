# 共享配置片段 + Failover — 交互、数据模型与守卫设计

日期：2026-09-27 ｜ 性质：**只读调研 + 设计，未改任何代码、未跑构建/测试** ｜ 基线：worktree HEAD `7e86964`（任务书给 master tip `2c663a2`，本工作区已领先 2 个合并提交）

> **2026-09-28 设计稿刷新（版本 2，只改设计稿、未改代码）**：按批 1 已合 master（`2d2ebfa`，2026-09-28 实测源码逐项核验）把 §1-§4 的落地事实收敛——migration 现状、G10/守卫、CRUD、P0、复制绑定；§3.1 错误口径按 `writeNoProfileError` 唯一出口（恒 503 `no_active_profile`）重定「不换」类别；新增 §2.10（批 2 编辑器 UI 设计 + UI 现状取证）、§3.8（批 3 failover 落地锚点）、§7（批 2/3 排期）；§5 待拍板追加 3 条。
输入：`../plans/2026-09-27-cc-switch-ref-provider-sync.md` §3-4/§5、`../plans/2026-09-27-cc-switch-ref-proxy.md` §3-6/§5-1、`2026-09-27-profile-reverse-sync-and-copy-design.md` §3.3 守卫表、`2026-09-22-claude-provider-config-design.md`

> 行号会漂移：一律给 `file:符号名`。cc-switch 的行为只作对照，**不当我们的现状**。本节所有「现状」均已读源码取证。

## 0. 结论速览

| 组 | 问题 | 结论 |
|---|---|---|
| A1 | 数据模型 | **独立表** `claude_shared_configs`（不是「特殊 profile」）；profile 侧加 `shared_config_id` 一列；**片段不含密钥 / base_url** → 零 Keychain 交互、零 `is_active` 影响 |
| A2 | 勾选关系 | **0..1 个片段**，**不嵌套**（片段不引用片段） |
| A3 | 冻结语义 | 片段键集 K = 片段 `extra_env` 的键；**片段值 > profile 自身值**（冻结 = 片段单点决定）；托管键在两者之上 |
| A4 | 防污染 | 新增守卫 **G10 = 捕获目标所绑定片段的键集 K** → 不写也不清；**片段自身永不作为捕获目标** |
| A5 | 代理侧 | 代理写的是**合并后**的 env（片段→profile→托管键），settings.json 里不留片段痕迹 |
| A6 | 生命周期 | 删片段 / 解绑 / 改片段 → 绑定 profile 自动解绑或跟随；P0（takeover 清理「上一轮托管、本轮不再生成」的键）**批 1 已落地** |
| B7 | 触发 | 分类表：传输失败 / 401 / 403 / 429 / 5xx / 其余 4xx → 换；400-422 负列表 + DevDesk 自身 503 `no_active_profile`（`writeNoProfileError` 唯一出口）→ 不换；**只换一跳** |
| B8 | 多 key | **不做**；用「再来一个 profile + 备用指向它」表达 |
| B9 | 可见性 | `claude_call_logs` 加 `failover_from` 列（**落库可审计**）+ 状态栏常驻一行；不每请求 toast |
| B10 | 与接管 | failover 是**请求级**：不改 `is_active`、不重写 settings.json；唯一硬交互是**模型名跨供应商解析**（§3.5） |

## 1. 现状锚点（设计要接的既有机制）

| 机制 | 符号（file:symbol） | 现状要点 |
|---|---|---|
| 表结构 | `core/migrations.h::claude_profiles` | `name UNIQUE` + 部分唯一索引 `idx_claude_active WHERE is_active=1`；该表无外键。`PRAGMA foreign_keys = ON` 已开（`core/sqlite_db.cpp`）→ 真加 FK 就不能用 `0` 当哨兵值 |
| 密钥 | `claude_service.h::Profile::keychainAccount` | 一条 profile = 一条 Keychain（约定 `claude:<id>`）；key 永不入 SQLite |
| env 生成 | `provider_config.cpp::buildProxyEnv` | 先 `extra_env`（别名归一）→ 再覆盖 BASE_URL / `PROXY_MANAGED` / 6 槽位；空槽位 skip |
| 接管写 | `settings_sync.cpp::takeOver` | 只 `env.set` 托管键、**从不清除**；sidecar 存**首份** `envSnapshot` + `managedKeys` 并集 |
| 还原 | `settings_sync.cpp::restore` | 按键级：快照有值→恢复、无→删除（`JsonObject` 无 erase，靠重建实现） |
| 反向同步 | `claude_service.cpp::syncCaptureActiveProfile` | **只在 `setActiveProfile` 内**调用；捕获目标是**旧 active**（`getActiveProfile()` 此刻仍指向它），**先捕获、后事务换位**、最后 `syncTakeOver()` 写新 profile |
| 守卫 | `provider_config.cpp::parseEnvForReverseSync` / `mergeReverseSyncEnv` | G1 托管键 / G2 loopback(+token 联动) / G3 占位符 / G4 空值 / G5 缺席清空 / G6 值非法；命中守卫的键进 `skippedKeys` = 「不写也不清」 |
| 代理转发 | `proxy_router.cpp::handleRequest` / `handleStreamingRequest` | 一请求一 profile；`resolveProfile()` = `getActiveProfile()` + `loadApiKey()`；方言每请求现算 `UpstreamDialect::forBaseUrl(profile.baseUrl)` |
| 首包前判定点 | 同上：缓冲路径在 `out.setStatus(...)` 之前；流式路径在 `waitForStatus(30000)` 之后、写响应头之前 | 两处都处「客户端还没收到任何字节」的位置 |
| 警告出口 | `claude_service.cpp::lastSyncWarnings()` | **已有 3 个消费者**（Claude 页头 `claude_module.cpp`、设置页 `settings_module.cpp`）→ G10 的警告**零新 UI** |

**落地现状（2026-09-28 实测复核）**：批 1（服务层/守卫，commit `2d2ebfa`，11 文件）已合 master——§2.1 表与 G10/§2.5 buildProxyEnv 合并/§2.6 解绑与复制/§2.7 P0 均已落地；剩余未开工 = 批 2 编辑器 UI（§2.10）、批 3 failover（§3.8）。**failover 仍零实现**：`fallback_profile_id|failover|shouldFailoverTo` 在 `src/`、`tests/` 只命中无关项（router 空态 `FallbackCompose`、terminal 的 `buildWindowsFallbackArgs`、片段注释里的 "failover" 字样），§3.7 两列 ALTER 未落 migration。

## 2. A. 共享配置片段

### 2.1 数据模型（A1）

```sql
-- migration：已合 master = kClaudeSharedConfigs v6（migrations.h:205-218，批 1 commit 2d2ebfa；
-- 当前 all() 末尾 = kClaudeProfileDialectOverride v9（定义 migrations.h:262，all() 条目 :281）；本段 SQL 与落地逐字一致）
CREATE TABLE IF NOT EXISTS claude_shared_configs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT UNIQUE NOT NULL,
  extra_env TEXT NOT NULL DEFAULT '',   -- 与 claude_profiles.extra_env 同构同语义
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
ALTER TABLE claude_profiles ADD COLUMN shared_config_id INTEGER NOT NULL DEFAULT 0;
```

**为什么独立表而不是「特殊 profile」**：`claude_profiles` 的每一行都被当成「可分发的上游」——`idx_claude_active` 唯一索引、`listProfiles()`、`createProfile(activateIfNoneActive)` 的自动激活、`duplicateProfile` 的命名空间、`cleanupOrphanKeychainEntries` 的孤儿判定、代理 `getActiveProfile()` 取用，共 6 条路径都默认「行 = 可用供应商」。塞一行没有 base_url/key 的特殊行，等于给这 6 条路径各加一个 `kind != shared` 判断，**漏判一次就会把片段当供应商转发**（503 或错路由）。独立表让这 6 条路径全部零改动。

| 子问 | 影响 |
|---|---|
| `claude_profiles` | 只多一列。`ALTER TABLE ADD COLUMN` 必须给非空默认值 → `DEFAULT 0` = 未绑定；与本仓既有「0 = 无」惯例一致（`setActiveProfile` 拒 id=0、`formEditId_ != 0` 判编辑态） |
| `is_active` 唯一性 | **零影响**（片段不在该表里，不参与激活） |
| Keychain `claude:<id>` | **零影响**：片段不含密钥 → 不建任何 Keychain 条目，`cleanupOrphanKeychainEntries` 看不见它 |
| 外键 | **不加 FK**：`PRAGMA foreign_keys=ON` 下 `DEFAULT 0` 会违约束；改 NULL 则 `rowToProfile`/`Profile` 要引 `std::optional`，收益不抵成本。删除清理放 service 一处（§2.6） |

**被否决备选**：(a)「特殊 profile（`claude_profiles.kind`）」——上述 6 条路径的守卫成本 + 误转发风险；(b) 片段存 kv 表（cc-switch 的 `common_config_{app}` + `common_config_{app}_cleared`）——它那个 `_cleared` 标记存在的**唯一理由**是片段没有原生编辑器（只能从 live 反推，故需「显式清空」区分「没配」与「用户清空」）；我们的片段在 UI 里原生编辑，空 `extra_env` 就是「空」，标记位是纯负债。

**为什么片段不含 base_url / 密钥**：片段一旦带 base_url，路由就有两个真相源（片段 vs profile），failover 候选、方言判定、Keychain 归属全要跟着改；「共享密钥」更等于作废「一 profile 一 Keychain」的模型。片段定位为**纯 env 装饰层**：不进路由、不进凭据链。代价：同一网关多 profile 仍需各填一次 base_url/key（用既有「复制」流程 3 秒完成）。

### 2.2 勾选关系（A2）

**结论**：一个 profile 绑定 **0 或 1 个**片段；**不允许**嵌套/链式。

理由：cc-switch 的同类层就是**每 app 单例**（一个 kv 键）——单例是能覆盖真实需求的最小数。N 个片段立刻要回答「两个片段给同一键不同值谁赢」「解绑一个要不要重算已写入的键」「UI 多选还是标签」，成本全花在冲突裁决上；嵌套更甚（环检测 + 展开顺序 + 冲突优先级），而收益只是「少抄几行重复键」——把它们放进同一个片段即可。

**被否决备选**：(a) N 个片段（需定义片段间冲突规则）；(b) 片段引用片段（需环检测）。两者都**不封死扩展**：`shared_config_id` 是一列，将来泛化成关联表 + 展开函数即可，已落地的语义不用改。

### 2.3 冻结语义与优先级（A3）

**K（片段键集）= 该片段 `extra_env` 的全部键（别名归一后）**。只有 extra_env 会被冻结——base_url / 密钥 / 6 槽位不在片段里，谈不上冻结。

| 优先（低→高） | 来源 | 写 settings.json 时 | 被反向同步捕获时 |
|---|---|---|---|
| 4 | live 的其它键 | 不直接进 env（先经反向同步收编成 profile 配置） | 真相源本身 |
| 3 | profile 自身 `extra_env` | 只写 **K 之外**的键 | 正常收编（出现覆盖 / 缺席清空） |
| 2 | 共享片段 `extra_env` | **覆盖** profile 的同名键（片段赢） | **G10：不写也不清** |
| 1 | 代理托管键（BASE_URL / `PROXY_MANAGED` / 6 槽位及 `*_NAME`） | 永远覆盖 | G1（sidecar 托管键） |

**「片段赢」的理由**：用户原话是「勾选后 **env 之类的就冻结了**」——冻结 = 该键值由片段单点决定，profile 不能反向覆盖；否则片段只是个「默认值提供者」，cc-switch 那种「改了片段不生效」的困惑会立刻回来。
**冲突时不删 profile 原值**（非破坏性）：行里保留、标「被共享片段覆盖，不生效」，解绑后自动恢复生效——这也是解绑文案的依据（§2.6）。
**UI 表现**：profile 表单「高级 env」tab 里该键**只读置灰 + chip「来自「片段 X」」**，点击提示「请在共享配置里修改」；用户新增同键时**直接拦下**并给同样提示（拦截优于事后被覆盖，避免「以为存上了」）。详情面板一行「共享配置：片段 A（N 个键）」；列表卡片可选角标「共享」。

### 2.4 与反向同步的冲突（A4，重点）

**结论**：新增守卫 **G10 = 键 ∈ 「本次捕获目标（旧 active profile）所绑定片段」的键集 K** → **不写也不清**；**片段自身永不被捕获**（捕获只写 active profile）。

**为什么 K 取「绑定片段」而不是「所有片段的并集」**——关键在一条**不变式**：
> settings.json 的 env 只能由「当前激活 profile 的 takeover」或「用户/外部工具」写入；而捕获目标恰好就是**将要离场的那一个**（`setActiveProfile` 先捕获旧 active、再换位、最后写新 active）。

所以「谁写的 live，谁收编自己」天然成立：live 里的片段键一定是「绑定了该片段的那个 profile」自己写进去的，切换时收编回它自己，G10 按绑定关系就能挡住。**若取全片段并集**，则「一个 profile 根本没绑的片段」会阻止它收编用户在 live 里的改动（用户会看到「我改了却没保存」，而原因在另一个 profile 的片段上）——这是更糟的误伤。
**边界条件**（不变式在何时不成立、G10 仍然正确）：接管关闭 / 代理未运行时 live 由用户或别的工具（本机同装 cc-switch）写，此时收编进「当时激活的 profile」是**正确**的反向同步语义（那些键就是用户配置），只有「键名恰好属于该 profile 绑定的片段」才被 G10 挡下——那正是「冻结」要表达的意思，并配一条可见警告。

**被否决备选**：cc-switch 的「切走时**整体重提取**片段 + 替换」（`sync_common_config_snippet_from_live`）。它整套（含密钥剥离 `is_sensitive_config_key` 模式匹配、`_cleared` 标记、循环保障论证）的根基是「settings.json 是共享配置的唯一编辑器」。我们片段有原生编辑器 + 上述不变式已让 live 与 profile 自洽，抄它等于把「live 改动回流片段」这条新路径打开，重新引入它注释里那两个 bug。**只吸收它的循环保障思想**，翻译成我们的写法：每次 takeover 都把片段合并进 live（§2.5）。

**片段键被 live 手工改动怎么办**：下次 takeover 用片段值覆盖回去（片段赢）。为了不重演「静默被改」的观感，takeover 时对「live 里片段键的值 ≠ 片段值」的键 push 一条警告进 `lastSyncWarnings_`（例：「`X` 的值被共享配置「片段 A」覆盖」）——这是**可见性要求**，不是可选美化。

**守卫点（全部最小改动；批 1 已全部落地，本表即落地后的形态）**：

| 位置 | 改动 |
|---|---|
| `provider_config.h::parseEnvForReverseSync` | 入参补一个 `ownedKeys`（捕获目标绑定片段的键集，空 = 未绑定）；命中 → 复用既有 `markSkipped(key)` + push warning |
| `provider_config.cpp::mergeReverseSyncEnv` | **零改动**（`isKeep()` 已按 `skippedKeys` 工作；G10 键的「不清」也自动成立） |
| `claude_service.cpp::syncCaptureActiveProfile` | 取一次捕获目标所绑片段（新 service 方法，如 `sharedEnvKeysFor(profileId)`）传下去；G7/G8/G9 文件级处置一律不变 |
| 捕获范围 | 不变：片段不进捕获链、不写片段行 |

### 2.5 与接管 / 代理的关系（A5）

- 代理写进 settings.json 的是**合并后的 env**（片段 → profile → 托管键），**不写片段原值、不留标记**。理由：settings.json 的 env 是扁平 map，无处放「出处」；且 `settings_sync.h` 的协议明写「不往 settings.json 塞标记字段」（不污染用户文件、不被别的工具当用户配置读走）。
- 实现（**批 1 已落地**）：`buildProxyEnv` 第 5 个入参 `sharedExtraEnvJson`（provider_config.h:216/242，调用点 claude_service.cpp:1619/1938），合并两步赋值：先 profile extra_env（现状），再片段 extra_env 覆盖。两条既有性质自动保住：托管键仍最高、空槽位仍 skip。
- **代理转发只用 profile 的 base_url/key**（片段不带这些）→ 片段对**路由、凭据、failover 候选**零影响。这是「片段排除 base_url」的最大回报。
- 片段保存后：若代理在跑且**激活 profile 绑定了它** → 立即 re-takeover（否则 live 留旧片段值到下次切换）。落点：片段保存 handler → 新增公开方法 `ClaudeService::reapplyTakeOver()`（内部即既有 `syncTakeOver()`，它现在私有）。
- 接管开关 `setSettingsTakeOverEnabled(false)`：片段完全不参与（不写 live、不捕获），与现状一致。
- **代理停止会连带撤走片段键**：片段键进 `managed` → 进 sidecar `managedKeys` → `restore` 时快照里没有它们 → 删除。live 干净。

### 2.6 生命周期（A6）

| 事件 | 行为 | 理由 |
|---|---|---|
| 片段**编辑** | 绑定它的 profile 下次 takeover 生效；代理在跑且属激活 profile → 立即 re-takeover | 「改了片段不生效」是 cc-switch 头号 bug，必须当场生效 |
| 片段**清空** | 同编辑；但**必须连带删除 live 上那些键** → 依赖 §2.7 的 P0 修复 | 现状只 set 不删，清空会变成「清了个寂寞」 |
| 片段**解绑** | 该 profile 的 K 变空：这些键回落为「profile 自身的同名值（若有，此时恢复生效）」或「从 live 移除」；确认弹窗写清这句话 | 非破坏性：解绑是交回控制权，不是丢配置 |
| 片段**删除** | 同事务 `UPDATE claude_profiles SET shared_config_id=0 WHERE shared_config_id=?`，提示「N 个 Profile 已解绑」；随后若代理在跑且激活 profile 在其中 → re-takeover | 不留悬空 id；不做级联删 profile |
| profile **复制** | `shared_config_id` 已随 `createProfile` 入参一并复制（**批 1 已落地**，claude_service.cpp:905）；`fallback_profile_id` 的复制留批 3（§3.7） | 复制 = 拿到同一份配置的直觉 |

### 2.7 前置修复 P0（不修则片段不可用）

> **批 1 已落地（2026-09-28 实测）**：takeOver 对本轮不再生成、但存在 sidecar `managedKeys` 的托管键走按键级裁决——快照有值→恢复、无→删除（settings_sync.cpp:476-492 同款重建循环）；本节保留为设计背景。

`settings_sync.cpp::takeOver` 现在**只 set、从不删**「上一轮托管、本轮不再生成」的键（`cc-switch-ref-proxy.md` §3.1 已登记同一缺口，症状是「清空模型槽位后旧模型名留在 settings.json」）。片段会让它变成日常：加键/删键/清空片段都触发。

正确修法**不是简单 erase**：对本轮不再生成、但存在于 sidecar `managedKeys` 的键，走与 `restore` 同款的**按键级裁决**——`envSnapshot` 有该键 → 恢复其原值；没有 → 删除。理由：`managedKeys` 是并集，其中可能有「用户接管前本来就有、后来又被移出托管」的键，直接删会弄丢用户原值。实现注意 `JsonObject` 无 `erase`，复用 `restore` 的重建循环写法。此修复同时修掉模型槽位旧 bug，是纯收益。

### 2.8 UI 落点

| 位置 | 内容 |
|---|---|
| Claude 页 | 列表区加「共享配置」区块/入口（片段增删改；编辑器复用 profile 表单「高级 env」的双列键值编辑器）——**不新建模块页**（模块隔离下新页只多一层导航，操作却仍与 profile 强相关） |
| profile 表单「高级 env」tab | 顶部一行「共享配置：[无 / 片段 A / 片段 B]」；选中后该片段键行置灰只读 + chip 标来源；新增同键被拦 |
| profile 详情面板 / 列表卡片 | 「共享配置：片段 A（N 个键）」/ 角标「共享」 |

### 2.9 流程图 1：勾选片段后的切 profile（A 绑片段 S → B 未绑）

```
用户点「启用 B」（setActiveProfile(B)，此时 active=A）
  │
  ├─ 1. 捕获（目标 = 旧 active A，先于任何 DB 改动）
  │     ├─ G7/G8/G9：live 不存在/空文件 → no-op 成功继续；存在但读不了 → 中止本次切换
  │     ├─ 取 ownedKeys = A 所绑片段 S 的键集 K(S)          ← 新增
  │     ├─ parseEnvForReverseSync(liveEnv, managedKeys, K(S))
  │     │     ├─ G1 托管键（sidecar）      → 跳过（不写也不清）
  │     │     ├─ G10 ∈ K(S)                → 跳过 + 警告（live 对 S 键的改动不回流）
  │     │     └─ 其余键                    → 正常映射（G2/G3/G4/G6 各自守卫）
  │     └─ mergeReverseSyncEnv → 写回 A（出现覆盖 / 缺席清空；A 行里 S 的同名键保持不动）
  │
  ├─ 2. 事务：A.is_active=0 → B.is_active=1
  │
  ├─ 3. 代理运行中？ → syncTakeOver(B)
  │     └─ buildProxyEnv(B.extra_env, B.models, S(B)?.extra_env, port)   ← 合并片段（片段赢）
  │        + P0 修复：本轮不再生成的旧托管键 → 快照值恢复 / 无则删除
  │
  ├─ 4. 上层覆盖检测 → lastSyncWarnings_（Claude 页头 + 设置页已有消费者）
  └─ 5. live = 片段 S(B) + B 自身 env + 托管键；A 已收编（不含片段键）
```

### 2.10 批 2：编辑器 UI 设计（2026-09-28 新增；未开工）

**UI 现状取证（2026-09-28 实测）**：`claude_ui_list/form/detail/logs/state/logic` 十二文件（6 .cpp + 6 .h）与 `claude_module.cpp` 对 `SharedConfig|shared_config` **零命中**（片段服务层 API 已合、UI 未接）；测试 `tests/test_modules/test_claude_ui.cpp` 存在。

| 现状符号 | 职责（批 2 的复用点） |
|---|---|
| `claude_module.cpp::compose`（:145 调 `drawClaudeHeader`） | 页面装配骨架；`lastSyncWarnings` 页头消费者（G10 警告**零新 UI**，沿用） |
| `claude_ui_list.h::drawProfileListPanel` :48（+ `ProfileListActions` :40） | 左栏 profile 列表——「共享配置」区块入口落点 |
| `claude_ui_form.h::drawProfileFormDialog` :54 / `openCreateForm` :39 / `openEditForm` :40 / `submitForm` :50 | 表单弹窗骨架（`FormSubmitHooks` :31）；片段的编辑弹窗复用同款形态 |
| `claude_ui_form.cpp::drawFormEnvTab` :495（tabs 装配 :724-732「基础/模型映射/高级 env」） | 键值编辑器（可见行上限 `kMaxVisibleRows=5`）；「高级 env」页顶部一行 = profile 绑定片段的下拉落点 |
| `claude_ui_state.h::FormState` :94（`formEditId_` :96 / `formTab_` :97 / **`formEnvRows_` :114 键值行**）+ `ClaudeUiState` 容器 :144 | **`formEnvRows_` 可直接复用作片段编辑器键值行**；新增 `SharedConfigDialogState` 挂容器 |
| `claude_ui_detail.h::drawDetailPanel` :67 | 右栏详情——「共享配置：片段 A（N 个键）」一行落点 |
| `claude_module.cpp::anyDialogOpen` :243-247（布尔聚合）/ `closeTopmostDialog` :268（Esc 序链：调用记录>删除确认>表单） | 弹窗管理链锚点——「各开窗点只置自身 flag」只是惯例、**非结构性互斥**；新增 `SharedConfigDialogState` **必须接入两处**（anyDialogOpen 参与 Ctrl+N/Ctrl+Enter 门控，closeTopmostDialog 收进 Esc 序链），否则片段编辑弹窗开着时按 Esc 穿透关不掉、快捷键不被门控 |

**设计（§2.3/§2.8 落点细化，全部改动在 `claude_ui_*` + `claude_module.cpp`，**零服务层改动**——CRUD 已合 claude_service.h:209-213 / cpp:964/990/1081/1136）**：

| 面 | 落点 | 内容 |
|---|---|---|
| 片段列表入口 | `drawProfileListPanel` 下方新增「共享配置」区块（同页，无新导航） | 片段卡片（name + 键数）+ 新建按钮；与 profile 列表同滚动容器 |
| 片段编辑弹窗 | 新 `SharedConfigDialogState`（挂 `ClaudeUiState` :144）；弹窗 = name 行 + 键值编辑器（复用 `formEnvRows_` 同款 + `drawFormEnvTab` 的 `kMaxVisibleRows=5` 约束） | 新建/编辑/删除 → 已合 CRUD；片段删除沿用已合同的**同事务解绑**；保存后激活 profile 绑定了它 → 调已合 `reapplyTakeOver()`（claude_service.h:222） |
| profile 绑定 | `drawFormEnvTab` 顶部一行「共享配置：[无/片段A/…]」下拉（dropdown 组件既有） | 写 `sharedConfigId`（走既有 `updateProfile`）；表单态新增 `formSharedConfigId_` |
| 片段赢预览 | 绑定后 K 键行**置灰只读 + chip「来自片段 X」**，新增同键**直接拦下**（§2.3 UI 表现） | K = `sharedEnvKeysFor(profileId)`（已合，claude_service.h:218）；只读/拦截判定在 `drawFormEnvTab` 内 |
| 详情/列表 | `drawDetailPanel` 一行「共享配置：片段 A（N 个键）」；列表卡片角标「共享」 | 数据 = `listSharedConfigs` 快照（走既有后台线程取数模式，claude_ui_state.h:153 注释） |

【待拍板】① 片段区块位置/入口形态（推荐：左栏列表下方独立区块 + 区块内「新建」按钮，与「+ 新建 Profile」同款按钮）；② 片段编辑弹窗是否可复用 `FormState` 结构加 `kind` 字段（推荐：**新增独立状态** `SharedConfigDialogState`，复用 `formEnvRows_` 写入模式，不污染 profile 表单状态；「`formOpen_` 单弹窗天然互斥」仅是惯例、非约束——新状态必须接入 `anyDialogOpen` 门控与 `closeTopmostDialog` Esc 序链两处（锚点见上表），其 Esc 关闭语义 = 与表单弹窗同级、入序链最顶层（Esc 最先关它））。
**已取证（2026-09-28 审计核验，前提证伪）**：EUI-NEO `components/input.h` 的 `InputBuilder` 全链**无 enabled/disabled/readonly 禁改态**（`components/` 下 'disabled' 零命中）——「片段赢预览」K 键置灰只读行**无现成控件**。两条可行路径：(a) EUI-NEO 上游加 disabled 输入态——跨仓库改动 + `DEVDESK_EUI_NEO_MIN_REV` 提升；(b) **推荐**——DevDesk 侧把 K 键行渲染成灰字 text + chip「来自片段 X」（非 input），零跨仓库改动，只读语义由「不可编辑控件」直接表达。落地默认 (b)；若拍板走上游 (a)，按 §7 批 2 文件面附注扩 scope。

## 3. B. Failover

### 3.1 触发与范围（B7）

**只换一跳**：每个请求最多试 2 家（active + 它的备用）。不做队列、不做顺序列表、不做熔断器、不做回切——cc-switch 的队列 + 熔断 + 顺序 UI 服务于它的多 app 多协议规模，单用户桌面端 2 家封顶已覆盖「主网关挂了 / key 用尽」。

**候选来源**：每个 profile 一个可选的**「备用 Profile」**（`fallback_profile_id`，0 = 无）。理由：**显式**（计费/合规问题的答案是「用户自己指过」）、0 新页面、随 profile 跟着走。被否决：全局顺序队列（cc-switch 的 `in_failover_queue`，需排序 UI + 「是否在队列里」第二状态）；隐式「列表里下一个」（用户无法预测请求去哪，计费场景不可接受）。

| 上游结果 | 换？ | 备注 |
|---|---|---|
| 传输失败（无响应头 / DNS / TLS / 连接 / 超时） | ✅ | 我们这侧 = `statusCode==0` / `502 upstream_unreachable` |
| 401 / 403 | ✅ | 只有换 key 才可能变好 |
| 429 | ✅ | 限流 |
| 5xx（含 500/502/503/504/529 过载） | ✅ | 上游侧故障 |
| 400 / 405 / 406 / 413 / 414 / 415 / 422 / 501 | ❌ | 请求自身非法，换家同结果——白烧一次请求（对照 cc-switch `categorize_proxy_error` 的负列表） |
| 其余 4xx（404 / 408 等） | ✅ | 采「负列表之外都试」：404 可能是「该网关没有这个模型」，failover 恰好能救 |
| DevDesk 自身 503 `no_active_profile`（`writeNoProfileError` 唯一出口） | ❌ | 本地配置错误，非上游故障；且发生在转发**前**（取 profile 失败直接回），failover 判定点根本到不了这一档 |

> **注记（2026-09-28 刷新，已源码证实）**：本行的「DevDesk 自身错误」按**新口径**重定——`proxy_router.cpp::writeNoProfileError`（:108-113）是「取 profile 失败」的**唯一出口**，恒 `writeErrorJson(out, 503, "no_active_profile", ...)`（:113），缓冲路径调用点 :202、流式路径调用点 :352；`500 keychain_read_failed` 这个 error slug 在代码里已不存在（仅剩 :103-106 一段历史注释）。类别结论（本地配置错误 → 不换）不受影响；failover 落地时 `shouldFailoverTo` 的「不换」类别按此新口径重定（§3.8），不照抄本行。

纯函数 `shouldFailoverTo(int statusCode, const std::string& errorKind)`（放 `provider_config.*` 或新 `failover_policy.*`，单测逐条覆盖）——两条转发路径共用，避免 if 表漂移。**判据表与 errorKind 取值已按新 503 口径在 §3.8 重定（批 3 落地锚点）。**
**不做**：同一 profile 内重试（key 级 429 重试要退避，收益低）；客户端可见的重试提示（`/v1/messages` 一跳重试对客户端完全透明）。

### 3.2 判定点与代码影响

| 路径 | 位置 | 改动 |
|---|---|---|
| 缓冲 | `proxy_router.cpp::handleRequest`（取 profile → 构造上游 → `platform::http::send` → `out.setStatus`） | 把「取 profile … 转发」包成一次尝试；可换且备用可解析时 → 取备用（`getProfile` + `loadApiKey`）、**重算** `forBaseUrl` 与 `upstream.url`，再试一次；两次都失败 → 返回**第二次**的结果（错误更相关） |
| 流式 | `proxy_router.cpp::handleStreamingRequest`：`waitForStatus(30000)` → 判 `upstreamStatus` → 写响应头 | 判定点就在 `waitForStatus` 之后、写响应头之前：`upstreamStatus==0` 或状态码可换 → `cancelWithReason("failover")` 丢弃本次 stream（队列里的半截 error body 随对象销毁），再用备用起一条新流。`StreamingResponse::cancelWithReason` 既有（`stopProxy` 在用） |

新增 service 方法 `resolveFallbackProfile(const Profile&)`（返回 `std::optional<Profile>`，内部 `getProfile` + `loadApiKey` + 失效/无 key 检查）——保持 `proxy_router` 不直接碰 DB/Keychain（它现在也全走 service）。
（**批 3 落点已对照当前源码**：取 profile 失败 = `writeNoProfileError` 调用点 :202（缓冲）/ :352（流式）；`shouldFailoverTo` 判定插在「`send` 返回后」/「`waitForStatus(30000)` 之后、写响应头之前」，`StreamingResponse::cancelWithReason` 既有——详见 §3.8。）
**首包预算**：每次尝试各自 30s（`waitForStatus`），最坏 60s 才把结果交给客户端。第一版**不做**预算分配（如「有备用时首试只等 15s」），登记为待测项：真机连接失败通常几百 ms 返回，30s 只在「上游吊住」时吃满；若实测痛，改一个常量（按「有无备用」分档）即可。

### 3.3 多 key（B8）

**不做。** 代价对照：

| | 做 | 不做（推荐） |
|---|---|---|
| 数据模型 | 一 profile N key → Keychain 命名要变 `claude:<id>:<n>`；`api_key_hint` / `api_key_check` 是**单值列**，要变成每 key 一份（表或 JSON） | 零改动 |
| 服务层 | `loadApiKey` 返列表 + 轮换/粘滞策略 + 「换 key」与「换 profile」两条重试路径的优先级 | 零改动 |
| UI | key 列表增删 + 「当前用第几把」 | 用既有「复制 Profile」再造一份（改 key） |
| 覆盖的失败面 | 同一 base_url 上多把 key 轮换（429 / 配额） | **同面**：第二把 key 建成第二个 profile，备用指向它 |

结论：**多 key ≈ 「再来一个 profile + `fallback_profile_id`」**，代价是多存一份 base_url/模型槽位（env 部分可用共享片段补，但**片段不带 key**——这正是 A1 决策的直接推论：key 本来就该一条 profile 一条）。真正放弃的**唯一**能力：同一账号 N 把 key 的**自动轮换**（需手工切）。v1 接受。

### 3.4 UX 可见性（B9）

| 通道 | 做法 |
|---|---|
| 调用记录（审计面） | `claude_call_logs` 加 `failover_from INTEGER NOT NULL DEFAULT 0`（0 = 无）。行内 `profile_id` = **实际服务的那家**，`failover_from` = 原计划那家 → 日志面板显示「（备用，原 A）」。**必须落库**：只在内存记的话，用户几天后回看会以为请求一直走的 A |
| 状态栏（当下） | Claude 页头部常驻一行：「代理：A（最近一次备用：B @ 12:03）」；点击可「切到 B」（走既有 `setActiveProfile(B)`） |
| toast | 只在**首个** failover 弹一次（EUI-NEO toast 单实例，每请求弹会互相顶掉）；文案含两家名字与原因码 |
| 不做 | 把 failover 塞进 `recordProxyEvent(statusCode)`（那是状态码单槽，语义会串、桌宠气泡会撒谎）；独立的 failover 历史页（日志面板已有全部信息） |

**默认值**：不设 `fallback_profile_id` = 不启用（**无全局开关**——字段非 0 就是用户显式授权；再加全局开关会造出「字段设了但开关关了」的第二状态）。表单给一行提示：「启用后，主 Profile 返回可重试错误（401/403/429/5xx/网络失败）时，请求会发往备用 Profile，**可能按另一家计费**」。想彻底避免换家：清掉该字段。

### 3.5 与共享片段、与自动接管、与模型名的交互（B10）

- **与共享片段**：无耦合。片段是 env 装饰、不参与选路；failover 换的是 profile 三元组（base_url / key / 槽位），settings.json 不因 failover 改动。
- **与自动接管**：请求级、不改 `is_active`、不触发 `setActiveProfile` → **不触发反向同步、不重写 settings.json**。这条关键：否则一次上游抖动就会顺手把 live 收编进某个 profile（还可能踩 S-2 中止）。用户想把 B 转正 → 状态栏点「切到 B」→ 走既有 `setActiveProfile`（capture → 换位 → takeover），零新代码。
- **与模型名（最硬的一处）**：settings.json 写的是 **A 的槽位真名**（如 `fangzhou/glm-5.3[1M]`），客户端会把这个名字发出来；failover 到 B 后 B 可能不认 → 400（**不换**）→ 用户看到一个本可避免的失败。两条出路：
  1. **代理侧按槽位反查改写**：用 A 的 6 槽位（模型名 + 显示名）建反查表 → 命中则把请求体 `model` 改写成 B 的**对应槽位**真名；B 该槽位为空 → 不改写（交给 B 报错）。改动与 `upstream_dialect.h::normalizeModelInBody` 同层（已有「就地改 body 顶层 model」的现成函数），且只在 failover 分支生效。
  2. **稳定别名**（cc-switch 式：settings.json 写 `claude-sonnet-5`，代理映射真名）→ failover 天然无痛，但 `/model` 里用户看到的是别名（`cc-switch-ref-proxy.md` §5-2 的待拍板项）。
  推荐 1：不动「用户看得见真名」的现状，只在换家那一刻改写；代价是维护一张反查表（槽位固定 6 个，纯函数 + 单测）。**必须与 §5-2 一起拍**：若 §5-2 选了别名，这里的 1 直接删。
- **不触发**：503 无激活 profile（本地配置问题）、客户端断开、停代理取消在途流。

### 3.6 流程图 2：failover 触发与回退

```
客户端 → 代理：POST /v1/messages
  │
  ├─ 取 active=A（+Keychain key）；取不到 → 503 直接回（不换家）
  │
  ├─ 尝试 #1：方言=forBaseUrl(A) → 转发
  │     ├─ 成功 → 写回客户端 + 日志(profile=A, failover_from=0) → 结束
  │     └─ 失败(status, err) → shouldFailoverTo? ── 否 → 原样回错 + 日志(A) → 结束
  │                                               │是
  │          ├─ 无备用 / 备用失效 / 备用无 key → 原样回错 + 日志(A) → 结束
  │          └─ 有备用 B：
  │               ├─ 在途流（流式路径）→ cancelWithReason("failover")
  │               ├─ 模型名反查：命中 A 的槽位 → 改写 body.model = B 对应槽位真名
  │               ├─ 尝试 #2：方言=forBaseUrl(B) → 转发
  │               │     ├─ 成功 → 写回 + 日志(profile=B, failover_from=A) + 状态栏更新 + 首次弹 toast
  │               │     └─ 失败 → 回 B 的错误 + 日志(B, from=A)（不再换第三家）
  │               └─ 用户点状态栏「切到 B」→ setActiveProfile(B)（capture A → 换位 → takeover）
  └─ 结束
```

### 3.7 数据模型增量（B 组）

```sql
-- migration v10（命名建议 kClaudeFailoverColumns；约定：v6 已被批 1 共享片段占用，
-- 当前 all() 末尾 = kClaudeProfileDialectOverride v9（定义 migrations.h:262，all() 条目 :281））
ALTER TABLE claude_profiles ADD COLUMN fallback_profile_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE claude_call_logs ADD COLUMN failover_from INTEGER NOT NULL DEFAULT 0;
```
- 校验（`updateProfile`）：拒 `fallback_profile_id == id`（自引用纯浪费一跳）；A→B 与 B→A 的一跳环无害，不拦。
- 删除 profile 时：同事务 `UPDATE claude_profiles SET fallback_profile_id=0 WHERE fallback_profile_id=?`。
- 复制 profile 时：`fallback_profile_id` 一并复制。
- （以上三条**批 3 一并实现**，未落地；删除清理 / 复制携带按已合批 1 同款式样：shared_config_id 同事务解绑 / `duplicateProfile` 入参携带，claude_service.cpp:905。）

### 3.8 批 3：failover 落地锚点（2026-09-28 新增；未开工）

**未开工确认（2026-09-28 复核）**：`fallback_profile_id` / `shouldFailoverTo` / `failover` 在 `src/`、`tests/` **零实现**（仅命中无关符号：router 空态 `FallbackCompose`、terminal `buildWindowsFallbackArgs`、片段注释里的 "failover" 字样），§3.7 两列 ALTER 未落 migration。

**shouldFailoverTo 判据（按 §3.1 新 503 口径重定「不换」类别）**：

| 类别 | errorKind / 判定 | 换？ |
|---|---|---|
| 传输失败（无响应头/DNS/TLS/连接/超时） | `statusCode==0`；缓冲路径 = `502 upstream_unreachable` | ✅ |
| 401 / 403 | 只有换 key 才可能变好 | ✅ |
| 429 | 上游限流 | ✅ |
| 5xx（500/502/503/504/529…） | 上游侧故障；**排除「DevDesk 自身 503 `no_active_profile`」**——它由 `writeNoProfileError` 在转发**前**直接回（:202/:352），到不了判定点，也不需要判 | ✅ |
| 400 / 405 / 406 / 413 / 414 / 415 / 422 / 501 | 请求自身非法，换家同结果 | ❌ |
| 其余 4xx（404 / 408 等） | 负列表之外都试 | ✅ |
| errorKind 兜底 | `errorKind=="no_active_profile"` → 不换（防御：判定点理论收不到，单测注释锚定此语义） | ❌ |

**触发链路（对照当前 `proxy_router.cpp`）**：
- 缓冲 `handleRequest`：取 profile（失败 = `writeNoProfileError` :202）→ 构造上游 → `send` → 判 `statusCode` → `shouldFailoverTo` 为真且 `resolveFallbackProfile` 可解析 → **重算** `forBaseUrl` 与 `upstream.url` 再试一次；两次都失败 → 回**第二次**的结果（错误更相关）。
- 流式 `handleStreamingRequest`：`waitForStatus(30000)` 后、写响应头前（取 profile 失败点 = :352）→ `cancelWithReason("failover")` 丢本次流 → 用备用起新流。
- **fallback_profile_id 读路径** = 新 service 方法 `resolveFallbackProfile(const Profile&)`：`getProfile(fallback_profile_id)` → `loadApiKey` → 任一失败 / id==0 → `std::nullopt`（原样回错，不换第三家）。
- **注入模式交互（已定义）**：`resolveProfile` 的 `profileProvider_` 注入分支（proxy_router.cpp:311-318，测试/UI 直给完整 profile、绕过 DB/Keychain）下，`resolveFallbackProfile` 内部 `getProfile`+`loadApiKey` 必命中无 profile 内存库 → 恒 `nullopt` → 注入场景 fallback 静默永不触发。因此把「**注入模式跳过 `resolveFallbackProfile`、不换家**」显式写进 `shouldFailoverTo` 调用处守卫（明确声明，不依赖恒 nullopt 的巧合）；批 3 router 级 failover 测试取数随之定义：既有注入模式用例默认断言不换家；「注入 + 换家」场景给内存库 **seed profile 行**（与既有缓冲路径测试的纯注入取数不同；扩展注入为双 profile 是备选，推荐 seed 行，改动更小）。

**扩展点（不做，登记）**：§5 待拍板 3 的「记忆性降级」（T 秒内直接走备用）若拍板纳入 v1，落点 = claude_service 内存 `map<profileId, failAt>`、不落库、重启即忘；批 3 v1 推荐纯请求级先行。

## 4. 数据模型与优先级总表

| 新增项 | 类型 / 位置 | 默认 | 语义 |
|---|---|---|---|
| `claude_shared_configs` | 新表（4 列） | — | 片段（name + extra_env） |
| `claude_profiles.shared_config_id` | INTEGER NOT NULL | 0 | 0 = 未绑定（0..1） |
| `claude_profiles.fallback_profile_id` | INTEGER NOT NULL | 0 | 0 = 无备用（0..1，单跳） |
| `claude_call_logs.failover_from` | INTEGER NOT NULL | 0 | 原计划 profile；实际服务的是 `profile_id` |
| migration | `core/migrations.h::all()` | — | 片段已落 **v6**（批 1）；failover = **v10**（新增）；**当前末尾 = v9** |

写 env 的优先级（低→高）：live 其它键（先被收编） → profile `extra_env` → 共享片段 `extra_env` → 代理托管键。
捕获 live 的处置：G1 托管键跳过 / **G10 绑定片段键跳过（新增）** / G2·G3·G4·G6 各自守卫 / 其余按「出现覆盖、缺席清空」。

## 5. 待拍板（5 条，均给推荐）

1. **绑定粒度 0..1（推荐）还是 N 个片段**？推荐 0..1：与 cc-switch 单例一致，省掉片段间同键冲突裁决；将来泛化成数组不破坏已落地语义。
2. **G10 用「捕获目标绑定的片段」（推荐）还是「全部片段并集」**？推荐前者：不变式（live 的写者恒为即将离场的 profile）保证前者够用；取并集会让「没绑该片段的 profile」无法收编用户在 live 的改动（误伤面更大）。
3. **failover 是否要「记忆性降级」**？推荐**请求级 + 内存短期记忆**：某 profile 出现可重试失败后，T（取 60）秒内直接走备用；**不落库、重启即忘**、状态栏可见。否决「改 `is_active` 的粘滞切换」（计费惊喜 + 要用户手工切回）。拒绝纯请求级（无记忆）会让主家一直挂时每个请求都先吃一次失败。
4. **模型名跨供应商怎么解**（与 `cc-switch-ref-proxy.md` §5-2 合并拍）：推荐「failover 时按槽位反查改写 `body.model`」（保留用户看得见真名）；否决「稳定别名」（改的是全时段显示）。
5. **片段 UI 落点**：推荐「Claude 页内区块 + 弹窗编辑」；否决「新模块页 / 设置页新 tab」（模块隔离下只多一层导航，操作仍与 profile 强相关）。
6. **批 2 / 批 3 可否并行**（分析见 §7）：推荐**服务层与 UI 拆开并行**——批 2（片段编辑器 UI）与批 3-服务层（migration v10 + shouldFailoverTo + resolveFallbackProfile + 校验/删除/复制）文件面不相交，可并行；**批 3-UI**（表单备用下拉 / 详情显示 / 状态栏行 / 日志 failover_from）与批 2 **同写 claude_ui_form / claude_ui_state / claude_module.cpp → 必须排批 2 合入之后串行**（同一文件同一时刻一个写者）。
7. **failover「记忆性降级」是否纳入批 3 v1**：推荐 **v1 不做**（纯请求级先行，见 §3.8 扩展点登记）；若拍板纳入，落点 = claude_service 内存 map、不落库、重启即忘。

## 6. 存疑 / 待验证

- §2.7 的 P0 **批 1 已落地**（settings_sync.cpp:476-492 按键级裁决），但未跑真实场景测试验证「清空模型槽位后旧模型名不留残留」的症状消失（落地波未跑构建/测试）。
- 「上游 401/429 前是否已消耗配额/计费」未验证 → 只影响 B9 文案措辞，不影响分类表。
- Claude Code 对不认得的 `model` 名返回 400 还是 404 未验证（影响「404 是否值得换家」）；两条都落在「换」侧，不影响结论。
- cc-switch 的 `_cleared` / 整体重提取是**有意不抄**；若将来改主意要做「从 settings.json 反推片段」，必须连带补密钥剥离（`is_sensitive_config_key` 的模式匹配），仅当那条路被拍板时才启动。
- `recordProxyEvent` 的消费者（桌宠）只看状态码；把 failover 并进去需先结构体化，本设计不做，未评估改动量。

## 7. 批 2 / 批 3 交付拆分与并行分析（2026-09-28 新增）

| 工作块 | 内容 | 文件面 | 与另一块交叠 |
|---|---|---|---|
| 批 2（编辑器 UI） | §2.10：片段区块 / 编辑器弹窗 / 绑定下拉 / 片段赢预览 / 详情行 | claude_ui_list / form / detail / state / logic + claude_module.cpp + tests/test_claude_ui.cpp（若走上游路径 (a) 的 disabled 输入，追加 EUI-NEO 仓库改动与 `DEVDESK_EUI_NEO_MIN_REV` 提升） | 零服务层改动（CRUD 已合） |
| 批 3-服务层 | §3.7/§3.8：migration v10 + shouldFailoverTo + resolveFallbackProfile + 校验/删除/复制清理 | migrations.h + provider_config.* + claude_service.* + proxy_router.cpp + tests/test_provider_config 等 | 与批 2 **零交叠** ✅ |
| 批 3-UI | §3.4 已有：备用下拉 / 详情显示 / 状态栏行 / 日志 failover_from | claude_ui_form / claude_ui_detail / claude_ui_logs / claude_ui_state / claude_module.cpp | 与批 2 **同文件** ❌ |

**推荐排期**：① 批 2 ‖ 批 3-服务层 **并行**（文件面不相交）；② 批 3-UI 在批 2 合入后**接续串行**（批 3-UI 还依赖批 3-服务层落地——v10 读完数据 UI 才有意义，天然排在批 3-服务层后）。两批都不依赖对方批次的产出；若要求「一次性合入」，整体退化为串行（批 2 → 批 3-服务层 → 批 3-UI）。
【待拍板】上述串并行拆分与顺序。
