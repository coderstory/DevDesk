# 供应商余量查询 + 首页卡片 + 存储统一（2026-10-02，rev.2）

> **来源**：用户 2026-10-02 三条需求——① 供应商余量查询（先做 opencode go / 火山 Coding Plan / deepseek / minimax cn）；② profile token 移出 Keychain 存数据库；③ app 配置全部进 SQLite、消灭 JSON 文件存储。
> **上游参考**：`~/dev/gui/cc-switch` @ `1ee2fdc3`（只读借鉴）。
> **调研**：`.superpowers/sdd/2026-10-02-quota-feature/research-ccswitch.md`、`research-devdesk.md`。
> **rev.2**：并入独立审计 `spec-audit.md`（2 blocker / 13 major / 9 minor）的处置，逐条见 §11。

## 0. 已定用户决策

- 刷新：**定时自动 + 手动刷新**
- 首页卡片：**列出所有 profile**，未配置的显示「去配置」
- 额外凭据（火山 AK/SK）：**与 profile token 同走数据库**（裁决见 §6）
- 无迁移、无兼容代码：直接切存储读路径

## 1. 概念模型：两类余量显式分开

```
QuotaProvider（抽象基类）
  ├─ Tiered    套餐额度：N 个时间窗口的已用百分比（火山 / opencode go / minimax cn）
  └─ Balance   账户余额：剩余金额（deepseek，无窗口、无重置）
```

- **统一语义只有一条**：窗口一律存**已用百分比 0–100**（`double usedPercent`），剩余 = 100 − used。MiniMax 的反转、ZenMux 的 ×100 这类归一化**在 provider 的 parse 内做完**，UI 层只见 `usedPercent`。
- 余额类不进窗口模型，单独字段。

## 2. 供应商抽象层：显式静态表 + 自声明字段

> **审计 F27**：静态自注册在静态库里会**静默失效**（未引用的 TU 不被链接器拉入，自注册构造不跑，无编译错误）。全仓既有模式一律是显式 constexpr 表（`app_registry.h:42 kModuleRegistry`）。故不用自注册。

```cpp
// src/module_claude_proxy/quota/quota_provider.h
enum class QuotaKind { Tiered, Balance };

enum class QuotaError {   // 审计 F14：对齐 cc-switch CredentialStatus
    None, Auth, Entitlement, Shape, Transient, NotConfigured
};

struct QuotaWindow { std::string key; double usedPercent; std::string resetsAt; };

struct QuotaResult {
    QuotaKind kind = QuotaKind::Tiered;
    std::vector<QuotaWindow> windows;      // Tiered：按 key 固定序
    double balanceRemaining = 0.0;         // Balance
    std::string currency;                   // Balance
    std::string planName;
    QuotaError error = QuotaError::None;   // 唯一错误通道（审计 F15）
    std::string errorMessage;
};

struct ProviderField { std::string key; std::string label; bool secret; };

class QuotaProvider {
public:
    virtual ~QuotaProvider() = default;
    virtual std::string id() const = 0;
    virtual std::string displayName() const = 0;
    virtual std::vector<ProviderField> fields() const = 0;
    virtual QuotaResult fetch(const ProviderConfig&, HttpSendFn) = 0;
};

// 显式注册表（审计 F27）
struct QuotaProviderEntry { const char* id; QuotaProvider* (*create)(); };
constexpr QuotaProviderEntry kQuotaProviders[] = { /* … */ };
```

- **注册表 = 显式 constexpr 表**，新增供应商 = 新增一个 `.cpp` + **表里加一行 + CMake 显式源码列表加一行**（审计 F27 指明实际是 3–4 处，不是 1 处；这个成本写明，不粉饰）。
- **配置字段自声明**：`ProviderField{key,label,secret}` 驱动弹窗动态渲染——直接服务用户「先选供应商类型，再决定什么配置项」的明确要求。
- **错误通道唯一**（审计 F15）：`fetch` **恒返回 `QuotaResult`**（不用 `Result<T>`），失败一律走 `error` + `errorMessage`；`Result::fail` 只留给「编程不变量破坏」，不用于上游错误。
- **注入缝**（审计 F23）：`HttpSendFn` 沿用 `module_agent/llm_client.h:22-25` 的函数指针注入范式（仓库无 `HttpClient` 类，`platform::http` 只有自由函数 `send`）。
- **base_url 探测**（审计 F28，可选但便宜）：`detectByBaseUrl(baseUrl)` 在弹窗打开时给默认选中项，存量 profile 少点几下；手选仍是主路径。

## 3. 四家口径（逐条核实自 cc-switch 源码）

### 3.1 OpenCode Go — Tiered
- `GET https://opencode.ai/zen/go/v1/usage`；`Authorization: Bearer {apiKey}`（**与推理侧 `/messages` 只认 `x-api-key` 相反，不可互换**）
- `{"usage":{"rolling"|"weekly"|"monthly":{"status":"ok"|"rate-limited","percent":0-100,"resetsAt":ISO}}}`
- 窗口：`rolling`→五小时 / `weekly`→周 / `monthly`→月（**按 key 名映射，不按 resetsAt 排序**）
- `percent==0` 时 resetsAt 是占位值 → 丢弃倒计时；形态不认识 → `QuotaError::Shape`，**不渲染空卡**
- 403（key 有效但无 Go 订阅）→ `QuotaError::Entitlement`，与 401（`QuotaError::Auth`）文案分开
- Zen 按量版 `/zen/v1` 不实现（实测 404）

### 3.2 火山方舟 Coding/Agent Plan — Tiered
- 控制面：`POST https://open.volcengineapi.com/?Action=<Action>&Region=<region>&Version=2024-01-01`
- 认证：**火山签名 V4（AK/SK）**，需在弹窗**另填 AccessKey ID + Secret**（与推理 Bearer key 两套）
- 签名三处偏离标准 SigV4：① canonical headers 固定序 `host;x-date;x-content-sha256;content-type`；② algorithm 串 `HMAC-SHA256`（无 `AWS4` 前缀）；③ credential scope 结尾 `request`，`kDate = HMAC(SK, date)`
- 先 `GetAFPUsage`（Agent Plan）→ `AFPFiveHour/AFPWeekly/AFPMonthly.{Quota,Used,ResetTime(ms)}`，`Quota<=0` 跳过；未命中再 `GetCodingPlanUsage` → `QuotaUsage[]`，`Level ∈ {session,weekly,monthly}` + `Percent` + `ResetTimestamp`(秒)
- Region 从 base_url host 段取 `cn-*`/`ap-*`，默认 `cn-beijing`；`AFPDaily` 跳过；DouBaoSeed 按量（`/api/v3`）不命中
- 错误分类 Auth / Soft / Transient

### 3.3 MiniMax 中国站 — Tiered
- `GET https://api.minimaxi.com/v1/api/openplatform/coding_plan/remains`（**旧域**，即使 profile base_url 是 `api.minimax.cn`）
- `Authorization: Bearer {apiKey}`；取 `model_remains[]` 中 `model_name=="general"`；业务错误在 `base_resp.{status_code,status_msg}`
- 五小时：`current_interval_remaining_percent` → **反转** `used = 100 - remain`，重置 `end_time`
- 周：`current_weekly_status == 1` 才渲染（`current_weekly_remaining_percent` / `weekly_end_time`）
- **无月窗口** → 不画月条（不画 0%）

### 3.4 DeepSeek — Balance
- `GET https://api.deepseek.com/user/balance`；`Authorization: Bearer {apiKey}`
- `balance_infos[].{currency,total_balance,...}`、`is_available`
- `balance_remaining = total_balance`，`currency` 原样；`is_available=false` → 文案「余额不足」
- **无窗口/无重置** → UI 只画金额条

## 4. 存储（合并用户 #16 / #17）

### 4.1 表：直接写进 `core/schema.h` 的 `kSql`

> **审计 F22**：仓库**没有** schema_version 表、没有 ALTER 迁移序列（`core/schema.h:4-20` 明写），且 `tests/test_core/test_schema.cpp:126` 有契约用例断言 `schema_version` 表**不存在**。故：**新表直接写进 kSql + 在 test_schema.cpp 补逐列断言**。spec 原稿说的「迁移机制 / 新版本号」作废。

```sql
CREATE TABLE profile_quota_config (
  profile_id INTEGER PRIMARY KEY,
  provider_id TEXT NOT NULL DEFAULT '',
  extra_json TEXT NOT NULL DEFAULT '',
  secrets_json TEXT NOT NULL DEFAULT '',
  enabled INTEGER NOT NULL DEFAULT 0,
  auto_refresh INTEGER NOT NULL DEFAULT 1,
  interval_minutes INTEGER NOT NULL DEFAULT 15
);
CREATE TABLE profile_quota_result (
  profile_id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL DEFAULT '',
  windows_json TEXT NOT NULL DEFAULT '',
  balance_remaining REAL NOT NULL DEFAULT 0,
  currency TEXT NOT NULL DEFAULT '',
  plan_name TEXT NOT NULL DEFAULT '',
  fetched_at INTEGER NOT NULL DEFAULT 0,
  last_error TEXT NOT NULL DEFAULT '',
  last_error_kind TEXT NOT NULL DEFAULT ''   -- QuotaError 名，UI 据此选文案
);
```

### 4.2 token 迁 Keychain → DB：调用点全清单（审计 F2 / F5 / F6）

先**作废**不变式：`profile_repository.h:6-11`「API key 明文永不入 SQLite」由本设计推翻，改写该注释。

| 类别 | 位置 | 处置 |
|---|---|---|
| 读 | `profile_repository.cpp:740` `loadApiKey` | 改读 DB |
| 写 | `profile_repository.cpp:100` / `:107-108` / `:135-136` `TwoPhaseKeyWrite`（两段写 + 补偿） | 改写 DB，保留两段式与补偿语义 |
| 校验 | `claude_service.cpp:64` `keyAtOldValue`（CPX-07） | 改比 DB 值 |
| 反向捕获 | `claude_service.cpp:791` `syncCaptureActiveProfile`（从 settings.json 回写 key） | 改写 DB |
| 孤儿清理 | `profile_repository.cpp:721` `deleteSecretsNotIn` | 改按 DB 列 |
| 转发取 key | `proxy_router.cpp:348` | 改读 DB |
| 间接 | `provider_config.cpp:616-620` `buildDirectEnv`（直连） | 随上游走 |
| 间接 | `claude_module.cpp:346-349` 启动孤儿清理 | 随上游走 |

三列重设计（审计 F6）：`claude_profiles` 17 列中**无凭据列**（`core/schema.h:101-119`）。
- 新增列 `api_key_encrypted TEXT`（存储形态见 §6 分平台）
- `keychain_account` → 废弃（不再有 keychain 引用）
- `api_key_hint` → 保留（显示用前 8 字符）
- `api_key_check` → 改为**全量 SHA256**（原语义是「库里引用 vs Keychain 明文」比对，前提消失），作「密钥是否变更」判据

### 4.3 消灭 JSON 文件配置：完整盘点（审计 F3 修正）

| 文件 | 位置 | 判定 |
|---|---|---|
| `config.json` | `core/paths.cpp:46` | → SQLite（app 配置） |
| `pet_config.json` | `module_pet/pet_config.cpp:28` | → SQLite |
| `markdown_view_state.json` | `module_markdown/md_view_state.cpp:35` | → SQLite（视图状态也是 app 状态，统一） |
| `todo_view_state.json` | `module_todo/todo_view_state.cpp:44` | → SQLite |
| `run-settings/run-<id>-<ts>.json` | `claude_service.cpp:619` / `:651-663` | → SQLite（**含明文 API key**，见 §6） |
| `claude_settings_snapshot.json` | `settings_sync.cpp:324` / `:371` | → SQLite（接管前 settings.json 快照，**可能含真 token**） |
| `~/.claude/settings.json` | Claude Code 接管契约 | **保持文件**（动了就断接管） |
| `~/.claude/managed-settings.json` + `<cwd>/.claude/settings{,.local}.json` 只读 overlay（`settings_sync.cpp:321-334`） | Claude Code 契约 | **保持文件** |
| `run-scripts/run-*.command`（macOS 派发脚本） | 同族临时文件，仅含路径 | **保持文件** + 清理口径 |
| 宠物皮肤 / EUI-NEO 资产 / 日志文件 / 笔记 | — | **保持文件** |

**伴生文件（权限口径必须与主文件同批定义）**：`config.json.bak`、`devdesk.db-wal`、`devdesk.db-shm`（WAL 模式会把凭据页复制进 -wal）。

**迁移后成孤儿、需明确处置的目录**：`~/.devdesk/<service>_<account>.key`（POSIX 明文后备，**不在 `DEVDESK_DATA_ROOT` 下** → 不随数据根隔离，跨 worktree 共享的老坑就是它）、`<dataRoot>/keychain/**`（Windows DPAPI blob 目录）。

> `claude_shared_configs` 本来就是 SQLite 表，列在「进 SQLite」里是空转——已从盘点表移除。

## 5. UI

### 5.1 首页余量卡片：全宽卡，不进统计网格（审计 F5 几何不可行）

审计实测：1200 宽窗口下统计卡 `statW ≈ 135px` × 固定 144px 高，**放不下「每 profile 一行的多窗口进度条」**。故：

- 余量卡 = 统计网格**下方**的**独立全宽区块**（不占第 6 张统计格），高度随 profile 数自适应 + `scrollView` 兜底
- 每个已配置 profile 一行：名称 + 供应商名 + 状态；Tiered 每窗口一条线性进度条（`components::progress`，EUI-NEO 无环形原语，与用户预览图一致）
- 语义 = **已用百分比**，右侧标 `62%`；色带 ≥90 红 / ≥70 橙 / 其余绿；`resetsAt` 有值标倒计时
- Balance：一行金额条 + 币种
- 右上角 `[刷新]` 手动刷新全部

**状态三态 + 空结果分支**（审计 F16 / F17）：

| 状态 | 判据 | 呈现 |
|---|---|---|
| 未配置 | `provider_id` 为空 | 「去配置」按钮 → 打开配置弹窗 |
| 配置不完整 | provider 已选但必填字段为空 / api key 读不到 | `QuotaError::NotConfigured`，文案「配置不完整，去补全」，**不画 0% 条** |
| 正常 | 有窗口 / 有余额 | 进度条 |
| 失败（有过成功值） | `last_error_kind != None` 且有旧数据 | 旧进度条 + 「可能过期」角标 |
| 失败（从未成功） | `last_error_kind != None` 且 `windows_json` 空 | **只渲染错误行，绝不画 0% 条** |

### 5.2 profile 详情「余量查询」按钮 + 配置弹窗

- 按钮**常驻**详情动作行（用户明确要求：不管什么供应商都有这个按钮）。
- **排期约束**（审计 F5）：详情动作行当前在批次 C 里被改动（删 3 个冗余按钮），claude_ui_detail.cpp:911 的几何与 `test_claude_ui.cpp:195` 的断言都要动；**余量按钮必须等批次 C 合并后再加**，且一并更新几何与断言。
- 弹窗流程（用户明确要求）：① 先选供应商类型 → ② 按 `fields()` **动态渲染**（火山 = AccessKey ID + Secret；其余 = 无额外项）→ ③ `[测试连接]` 真发一次并把结果/错误显示在弹窗内 → ④ `[保存]` / `[取消]`
- 用 `ui_kit::DialogFrame` 范式。

### 5.3 刷新策略

- **定时器选型（审计 F12）**：**Scheduler 式真后台线程**（`module_agent/scheduler.cpp:128-143` 的 sleep 切片 tick 先例），**不用 UI `onTimer`**——后者在窗口隐藏/挂起时整帧不进 compose → 刷新静默停止，与用户「定时自动」的决策相悖，且两平台同病。
- 结果落库，UI 只收割渲染（顺带解决 home 与 claude_proxy 生命周期解耦）。
- 手动：卡片 `[刷新]` / 弹窗 `[测试连接]`。
- 失败韧性：保留上次成功值 + 「可能过期」；鉴权类（`Auth` / `Entitlement` / `NotConfigured` / `Shape`）立即透出不掩盖；`Transient` 才走保留。

## 6. 安全裁决与分平台处置（审计 F10 / F11 / F30 / F31）

### 6.1 冲突裁决

用户先选「火山 AK/SK 走 Keychain」，后要求「token 全进 DB、配置全进 SQLite」。两条并行 = 两套存储，违背统一意图。**裁决：全部凭据统一进 SQLite**，以后者（更晚、更强的统一口径）为准。

### 6.2 分平台保护（blocker）

| 平台 | 现状 | 迁移后 | 处置 |
|---|---|---|---|
| macOS | Keychain（钥匙串级） | SQLite 列 + 文件权限 | **平台拆分实现**（铁律）：`secure_file_perms_macos.cpp` = `chmod 0600`；`_win.cpp` 另述；`_posix.cpp`。抄 `keychain_posix.cpp:34-49`「写完 chmod 0600，失败即回滚」口径 |
| Windows | DPAPI 加密（`keychain_win.cpp:1-13`，用户态加密） | **DPAPI 加密后的 blob 存 DB 列** | 「不用 Keychain」≠「明文落盘」：改为调用 DPAPI 加密后入库，保住加密级保护。**这一条必须在设计里写明，不能让实现者照字面落成明文** |

### 6.3 明文凭据副本必须一并处置（blocker，审计 F30）

`chmod 600` 只盖 `devdesk.db` 一个文件是不够的——同数据根下还有：

1. `run-settings/run-<id>-<ts>.json`（`claude_service.cpp:619/651-663`，**明文 API key**，`atomic_write` 无 chmod）
2. `claude_settings_snapshot.json`（`settings_sync.cpp:324/371`，接管前 settings.json 原 env，**可能含真 token**）
3. SQLite 的 `-wal` / `-shm`（`sqlite_db.cpp:238`，WAL 模式下凭据页会复制进去）

处置：① 前两者按 §4.3 迁进 SQLite；② `-wal`/`-shm` 与主库同权限目录 + 每次打开校正；③ 主库本身当前实测是 `-rw-r--r--`（同机可读），必须收紧。

**陈述修正**：只有上述全部落地后，「保护级别 = 文件系统权限级」才成立（macOS）／「加密级」（Windows DPAPI blob）。

**必须同步改写的成文不变式与文案**（三处代码注释 + 一处 UI 文案，正面推翻「API key 明文永不入 SQLite」）：
`profile_repository.h:6-11`、`claude_service.h:5-9`、`core/schema.h:105-107`、以及 UI 上「存入 Keychain，不落数据库」这类面向用户的说明——不改文案 = 功能与文档互相打脸。

**清理器口径修正**（`platform/keychain.h:14-31`）：白名单只放行 `claude:` 前缀；前缀外的 key **保留为残留（不删、不计数）**，三平台一致。所以旧 key 迁走后不会自动消失，需一次性清理口径（迁 DB 后启动时清一次残留），否则用户机器上会留一把没人管的旧钥匙。

**数据根要进 git 的风险**（盘点新发现，最重的一条）：`paths.h` 定的数据根是**默认可见**目录，且规划中 notes 要在该根下做 git 仓库。凭据进 DB 后，0644 的 `devdesk.db` 与 `devdesk.db-wal`（WAL 会把凭据页复制进去）就躺在**将来会被 `git add .` 扫到**的目录里。必做：
- 数据根内置 `.gitignore`（至少 `*.db` / `*.db-wal` / `*.db-shm` / `config.json*` / `run-settings/`）并在 UI 暴露该文件路径
- db 三件套与 `run-settings` 一律 0600（平移 `keychain_posix.cpp:34-50` 的「写完 chmod 0600，失败即回滚」口径）
- 这条不落实，明文密钥就会随一次误提交上远端

### 6.4 导出路径继续脱敏（审计 F31）

「复制配置」（`claude_module.cpp:653-726`）今天特意传空 key（`:700-702` 注释「导出无 token 键」）。凭据入 DB 后，这个「无凭据」性质**没有任何守卫保证会保留**（一键就能改成读库带真 token → 剪贴板是全局可读出口）。**硬约束：导出 / 复制 / 剪贴板路径继续脱敏，并补一条测试钉住。**

## 7. 从 cc-switch 抄的教训

1. **窗口分类绝不按 resetsAt 排序**——周期末尾周桶可能比 5h 桶更早重置（cc-switch issue #3036 / commit `65d69299`）；一律按供应商显式字段（`unit` / `Level` / 响应 key 名）映射。
2. **量纲各不相同**：MiniMax 剩余%（反转）、ZenMux 0–1 小数（×100）、火山 AFP 绝对值（Used/Quota）；归一化在 provider 内做完。
3. **形态不认识就报错**，不渲染空卡。
4. **额外凭据与推理 key 分开存**。
5. **不抄它的无注册表 match 分派**（它自己的待办承认：加供应商改 4 处）。

## 8. 模块隔离落点

- 表在 `core/schema.h`（core 层），`module_home` 直读——**先例已有**：现有首页卡片就读 `claude_call_logs`
- 触发查询走既有 `IClaudeProxyService` + `ServiceLocator`（`main.cpp:277` 先例），**home / claude_proxy 之间不新增依赖**
- provider 实现全落 `module_claude_proxy/quota/`，不跨模块

## 9. 验证（审计 F19 / F20 / F21 修正）

- **fixture 矩阵**按 provider × 坑列全：opencode（percent==0 占位 resetsAt / 形态不认识 / 403 vs 401 / 三窗口齐全）；火山（AFP 正常 / Quota<=0 跳过 / Coding Plan Level / **签名结构契约**——无服务端金标准向量，只能锁结构）；minimax（**反转 `used=100-remain`** / `current_weekly_status!=1` 不出周条 / `base_resp.status_code!=0` 业务错误 / 无月）；deepseek（多币种 / 空数组 / `is_available=false`）
- **归一化不变量写成反向断言**：越界输入被**拒或记 error**，**不是** clamp 后断言落在 [0,100]（clamp 断言恒真）；小数百分比**不得截断**（`double` 贯穿，展示层才格式化）
- **弹窗字段断言钉死具体集合**：火山 = `{accessKeyId, secretAccessKey}` 且 `secret==true`（输入框类型为密码框），其余 provider = 空集——不能用 `fields()` 自己当期望值（自证）
- 出网真测：opencode go 用真实 profile 手测一次；**火山若有账号必须真测一次并把「签名对不对」写进报告**，没有账号则明确标注「未测，签名正确性未验证」
- 首页卡片：全宽区块不挤压既有统计卡（probe 几何断言）

## 10. 交付顺序

1. 批次 A / B1 / C / D + Dock 图标（已派单，在飞）
2. **存储统一**（#16 #17，含 §4.2 全清单 + §6.2 分平台 + §6.3 明文副本）——余量功能依赖它
3. 余量功能（§1–§5），其中「余量查询」按钮必须排在批次 C 之后
4. 顺带插空：火山签名真测

## 11. 审计处置索引（spec-audit.md rev.2）

| finding | 处置 |
|---|---|
| F1 火山零真测 | §9 增「有账号必须真测 + 签名对不对写进报告；无账号标注未测」+ 签名失败独立文案 |
| F2 token 迁移无调用点 | §4.2 全清单（读 1 / 写 3 / 校验 1 / 反向捕获 1 / 孤儿 1 / 转发 1 / 间接 2） |
| F3 JSON 盘点不实 | §4.3 补齐 6 个文件并逐条判定 |
| F5 Keychain 调用点 6+3 | §4.2 表格 |
| F6 明文不变式未作废 + 无凭据列 | §4.2 显式作废 `profile_repository.h:6-11`，三列重设计 |
| F4 / F30 明文副本 | §6.3 三处逐一处置 |
| F10 / F11 Windows | §6.2 分平台表 + 平台拆分文件口径；Windows 用 DPAPI blob 入库 |
| F12 定时器选型 | §5.3 定为 Scheduler 式后台线程 |
| F14 错误分类缺失 | §2 `QuotaError` 六值枚举 |
| F15 双错误通道 | §2 `fetch` 恒返回 `QuotaResult`，`Result::fail` 只留编程不变量 |
| F16 / F17 未配置与空结果 | §5.1 五态表 + 空结果不画 0% |
| F19 fixture 缺 5 类坑 | §9 fixture 矩阵列全 |
| F20 弹窗断言自证 | §9 钉死具体字段集合与 secret 类型 |
| F21 归一化恒真断言 | §9 反向断言 + 小数不截断 |
| F22 migration 机制不存在 | §4.1 改为直接改 kSql + test_schema 逐列断言 |
| F23 HttpClient 不存在 | §2 改用 `HttpSendFn` 注入缝（照 `llm_client.h:22-25`） |
| F24 usedPercent 整数截断 | §2 改 `double` |
| F27 静态自注册静默失效 | §2 改显式 constexpr 表 |
| F28 缺 base_url 探测 | §2 增 `detectByBaseUrl`（可选，默认选中） |
| F31 导出脱敏无守卫 | §6.4 硬约束 + 测试 |
| minor（9 条） | 实现期按报告逐条核，其中「余量按钮与批次 C 动作行冲突」已升级为 §5.2 排期约束 |
