# Claude 供应商完整配置 — 调研与设计

日期：2026-09-22 ｜ 状态：**已实施**（Phase A–E 全量落地 2026-09-22，233/233 全绿；真机热切换验证留给用户） ｜ 关联 spec：§8.3（Profile 数据模型）、§10.4（本地代理）

## 1. 需求与用户价值

DevDesk 的 Claude 切换器目前每个 profile 只有一组「名称 + Base URL + API Key + 单个默认模型」。实际使用中（参考 cc-switch 工具与用户提供的 Claude Code settings.json 模板），一套完整的供应商配置远不止这些：

- **6 个模型槽位**：`ANTHROPIC_MODEL`（默认）+ `SONNET / OPUS / HAIKU / FABLE / SUBAGENT` 五个槽位各自映射到上游实际模型名，每个槽位还带一个**界面显示名**（`*_MODEL_NAME`，Claude Code 界面显示用，通常剥除 `[1M]` 这类上下文窗口标记）
- **行为开关**：`AUTO_COMPACT_ENABLED`、`CLAUDE_AUTOCOMPACT_PCT_OVERRIDE`、`DISABLE_AUTOUPDATER` 等——纯客户端行为，与请求转发无关
- **自定义 env**：任意键值对透传给 Claude Code

用户价值：**多供应商一键切换**时，模型映射和行为开关跟随 profile 一起切，不用手工改 `~/.claude/settings.json`。

用户模板（真实例子，本环境正在用的网关）：

```json
"env": {
  "ANTHROPIC_AUTH_TOKEN": "sk_ind_test_...",
  "ANTHROPIC_BASE_URL": "https://codingxrui.geely-test.com/fangzhou/anthropic",
  "ANTHROPIC_DEFAULT_FABLE_MODEL": "fangzhou/glm-5.3[1M]",
  "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME": "fangzhou/glm-5.3",
  "ANTHROPIC_DEFAULT_HAIKU_MODEL": "fangzhou/glm-5.3",
  "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME": "fangzhou/glm-5.3",
  "ANTHROPIC_DEFAULT_OPUS_MODEL": "fangzhou/glm-5.3[1M]",
  "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME": "fangzhou/glm-5.3",
  "ANTHROPIC_DEFAULT_SONNET_MODEL": "fangzhou/glm-5.3[1M]",
  "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME": "fangzhou/glm-5.3",
  "ANTHROPIC_MODEL": "fangzhou/glm-5.3[1M]",
  "AUTO_COMPACT_ENABLED": "1",
  "CLAUDE_AUTOCOMPACT_PCT_OVERRIDE": "75",
  "CLAUDE_CODE_AUTO_COMPACT_WINDOW": "800000",
  "CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING": "1",
  "CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS": "1",
  "CLAUDE_CODE_SUBAGENT_MODEL": "fangzhou/glm-5.3[1M]",
  "DISABLE_AUTOUPDATER": "1"
}
```

**三条硬性需求**（用户 2026-09-22 追加，优先级高于本文档自身的方案权衡）：

1. **默认不启动代理**：应用启动不再自动拉起 127.0.0.1:8888；代理改为用户手动启动，设置页可配「随应用启动」（默认关）
2. **启动代理后实时改写 Claude Code 配置自动接入**：代理一启动 / 切换 profile，就自动写 `~/.claude/settings.json` 的 env 块（BASE_URL 指向本地代理、AUTH_TOKEN 用代理 token、模型映射按当前 profile）——用户零手工配置，Claude Code 下一个请求自动走代理
3. **停止时的还原边界**：代理停止时要考虑是否还原 settings.json；用户已有的非 env 字段必须原样保留，只动 env 块；写入前有备份/合并语义，不能弄丢用户手工配置

## 2. 调研发现

### 2.1 DevDesk 现状（源码核实）

**Profile 数据模型**（migrations.h v1，当前 schema 版本 2）：

```sql
CREATE TABLE claude_profiles (
    id, name UNIQUE, base_url, api_key_hint, api_key_check,
    keychain_account, model, is_active, created_at, updated_at
);
-- model 是单个 TEXT 字段（"默认模型"），没有模型槽位概念
```

API key 全程走 Keychain（`claude:<id>` account），SQLite 只存 hint + SHA256 前 8 位，这个安全设计**保持不变**。

**代理转发逻辑**（proxy_router.cpp）：
- 缓冲路径 `handleRequest`（:85）与流式路径 `handleStreamingRequest`（:281）都：取 active profile → Keychain 读 key → **强制覆盖 `x-api-key` header** → body 原样转发（**不改写 `model` 字段**）
- 客户端鉴权：`Authorization: Bearer <代理 token>`（http_server pre_routing_handler 校验 401）
- 也就是说：**token 注入机制已存在**，代理模式下客户端不需要知道上游真实 key

**代理启动现状（硬性需求 1 要改的）**：
- main.cpp `startClaudeProxy()`（:218）在 `dslAppConfig()` 初始化时无条件拉起 127.0.0.1:8888，并写 `proxy_token.txt` 到应用数据目录
- claude_module.cpp `onStart()`（:724）还有一层兜底 `startProxy`（模块独立运行时保代理）
- settings_module.cpp `doRegenerateToken()`（:163）：**「重新生成 Token」在代理未运行时会先把代理拉起来**（stop + start 同端口重启的写法在「本来就停着」时等于启动）——审计发现的怪行为，本次一并修掉

**设置页现状**（module_settings）：`SettingsConfig { darkTheme, fontScalePercent, logRetentionDays }` 存 config.json（platform::json / yyjson）；设置页已展示代理 token + 重新生成按钮（settings 模块对 claude_proxy 的依赖有注释豁免先例）。

**platform::json 能力**（src/platform/json.h）：`parse()` / `stringify(pretty)` / `JsonObject::set/get/keys` / `JsonValue` 值语义——读-改-写 settings.json（保留未知字段：解析出整棵树 → 只改 `env` 子对象 → 序列化回写）**现有封装够用**，无需扩依赖。

**cc-switch**（github.com/farion1231/cc-switch，README 已核实）：
- 跨平台桌面工具，管理 Claude Code / Codex / Gemini CLI 等多家 CLI 的供应商配置
- 供应商数据存自己的 SQLite（`~/.cc-switch/cc-switch.db`），**切换时写入目标工具的配置文件**（Claude Code 即 `~/.claude/settings.json`）
- 原子写保护配置不损坏；自动备份轮换（保留最近 10 份，`~/.cc-switch/backups/`）
- 50+ 供应商预设、一键导入、系统托盘快切
- README FAQ 声称 **Claude Code 是唯一支持热切换的工具**（改 settings.json 后无需重启终端；其余工具要重启）——**已获官方文档背书**（code.claude.com/docs/en/settings-reference，独立事实核查 2026-09-22）：env 键在「at startup, and again in the running session when a saved change alters the merged env」时重新加载，即保存后运行中的会话即生效；例外是 `model` settings 键（会话中途切换需 `/model` 命令），**env 路径不受该例外影响**。实施时顺手验证一次即可（见开放问题 1，已关闭）

### 2.2 关键技术事实（设计前提）

| env 变量 | 作用域 | 代理能否注入 | 结论 |
|---|---|---|---|
| `ANTHROPIC_BASE_URL` / `ANTHROPIC_AUTH_TOKEN` | 客户端连接哪个服务 | 否（决定客户端发到哪） | 走 settings.json 通道：BASE_URL → `http://127.0.0.1:8888`，AUTH_TOKEN → 代理 token（真实上游 key 由代理注入，现有机制） |
| `ANTHROPIC_DEFAULT_*_MODEL` / `ANTHROPIC_MODEL` / `CLAUDE_CODE_SUBAGENT_MODEL` | 客户端本地决定请求里填什么模型名 | 理论可（重写请求体 `model` 字段），但见 §5 分析 | 走 settings.json 通道（写实际模型名，客户端直发，代理零改动转发） |
| `*_MODEL_NAME` | 纯 UI 显示 | 否 | 只能走 settings.json 通道 |
| `DISABLE_AUTO_COMPACT` / `CLAUDE_CODE_*` / `DISABLE_AUTOUPDATER` 等行为开关 | 纯客户端行为 | 否 | 只能走 settings.json 通道。**注意**：用户模板里的 `AUTO_COMPACT_ENABLED` **不是官方变量**（官方关闭自动压缩用 env `DISABLE_AUTO_COMPACT` 或 settings 键 `autoCompactEnabled`），导入/生成时需映射或警告（见 §4.3 规则 6） |

**`[1M]` 上下文标记**：本环境网关模型名形如 `fangzhou/glm-5.3[1M]`（发上游的实际名），而 `_MODEL_NAME` 显示名是剥除了 `[1M]` 的 `fangzhou/glm-5.3`。设计上体现为：模型槽位存两个字段——`model`（实际名，原样保存）和 `displayName`（显示名）；**displayName 留空时自动从 model 派生：剥除尾部 `[...]` 标记**（`[1M]`/`[200K]`/`[1m]` 等），导入和手填都走同一派生函数。

## 3. 现状差距

| # | 差距 | 现状 |
|---|------|------|
| 1 | 数据模型只有单 `model` 字段 | 6 槽位 × 2 字段 + 自定义 env 无处存 |
| 2 | 代理开机自动拉起 | main.cpp 无条件 start + claude_module onStart 兜底 + 设置页重新生成 token 顺带拉起 |
| 3 | Claude Code 侧零联动 | 代理启动后用户要手工把 BASE_URL/token 抄进 `~/.claude/settings.json` |
| 4 | 表单只有 4 个字段 | 无模型映射表单、无 env 编辑器、无导入 |
| 5 | 详情面板只显示一个模型名 | 6 槽位映射不可见 |

## 4. 数据模型设计

### 4.1 migration v3（向后兼容，不动现有列）

```sql
-- version 3: claude provider full config
ALTER TABLE claude_profiles ADD COLUMN models_config TEXT NOT NULL DEFAULT '';
ALTER TABLE claude_profiles ADD COLUMN extra_env TEXT NOT NULL DEFAULT '';
```

两个新列都是 **JSON 文本列**（SQLite 动态类型足够，与 todo_tasks.tags 的 TEXT-JSON 先例一致）。存量数据零迁移成本：空串 = 未配置。

`models_config` 结构（6 槽位，每槽位 `model` 实际名 + `name` 显示名）：

```json
{
  "default":   { "model": "fangzhou/glm-5.3[1M]", "name": "fangzhou/glm-5.3" },
  "sonnet":    { "model": "", "name": "" },
  "opus":      { "model": "", "name": "" },
  "haiku":     { "model": "", "name": "" },
  "fable":     { "model": "", "name": "" },
  "subagent":  { "model": "", "name": "" }
}
```

`extra_env` 结构（值一律字符串——env 变量本质是字符串）：

```json
{ "AUTO_COMPACT_ENABLED": "1", "DISABLE_AUTOUPDATER": "1" }
```

### 4.2 兼容语义

- **旧 `model` 列保留**，语义降级为「`default` 槽位未配置时的 fallback」（列表卡片继续显示它，一行 SQL 不用改）
- `models_config` 为空串的 profile：行为与今天完全一致（表单读出来 default 槽位预填旧 `model` 值，保存时整体写入 `models_config`，旧列同步更新保持一致——单数据源太琐碎，双向同步两个字段成本低于迁移存量数据）
- Profile struct 增加两个 `std::string` 成员（`modelsConfigJson` / `extraEnvJson` 原文存取），**槽位解析/派生/合并逻辑收敛到一个纯函数**（见 §5.3），便于单测

### 4.3 生成 env 的纯函数（核心逻辑，一处实现）

```cpp
// module_claude_proxy 内，纯函数、无 IO，直接单测
// 输入：profile（含 models/extraEnv JSON）+ 代理端口 + 代理 token
// 输出：要写入 settings.json env 块的完整键值 map
std::map<std::string, std::string> buildProxyEnv(
    const Profile& p, int proxyPort, const std::string& proxyToken);
```

规则：
1. `ANTHROPIC_BASE_URL` = `http://127.0.0.1:<port>`
2. `ANTHROPIC_AUTH_TOKEN` = 代理 token（**上游真实 key 永不出现在 settings.json**，代理注入）
3. 模型槽位 → `ANTHROPIC_MODEL` / `ANTHROPIC_DEFAULT_{SONNET|OPUS|HAIKU|FABLE}_MODEL` / `CLAUDE_CODE_SUBAGENT_MODEL`；槽位空则跳过该键（Claude Code 回落内置映射）
4. 显示名 → 对应 `*_MODEL_NAME`（`ANTHROPIC_MODEL` 没有官方 `_NAME` 变体，不生成）；`name` 空则用派生函数剥 `[...]` 后缀
5. `extra_env` 原样平铺；键与 1-4 冲突时 **1-4 优先**（导入的 JSON 里可能带旧的 BASE_URL/AUTH_TOKEN，必须被代理的覆盖）
6. **非官方变量别名表**（导入 cc-switch 模板时的常见坑）：`AUTO_COMPACT_ENABLED` → 官方 `DISABLE_AUTO_COMPACT`，**值取反**（`AUTO_COMPACT_ENABLED=1` 表示开 → 生成 `DISABLE_AUTO_COMPACT=0` 或直接省略该键；`=0` → `DISABLE_AUTO_COMPACT=1`）。`parseImportedEnv()` 命中别名表即自动映射并在 UI 提示「已映射为官方变量」；别名表外无法识别官方文档出处的键（如部分 `CLAUDE_AUTOCOMPACT_*`）原样透传 + 警告角标（不阻断，行为由用户自负）

## 5. 生效机制设计（本文档核心决策）

### 5.1 双通道分析

**通道 (a)：代理重写请求体 `model` 字段**
- 做法：handleRequest / handleStreamingRequest 在转发前解析 body JSON，把 `model` 改写成当前 profile 的上游实际模型名
- 优点：DevDesk 侧全控，客户端不配 settings.json 也能吃到模型映射
- 缺点：① 只覆盖模型映射，**覆盖不了 AUTO_COMPACT / DISABLE_AUTOUPDATER 等行为开关和 `_MODEL_NAME` 显示名**（纯客户端行为，代理物理上碰不到）；② 流式 + 缓冲两条路径都要做 body 解析重写，SSE 路径多一次完整 body 拷贝/重组，侵入已压测通过的转发链路；③ 客户端界面仍显示它自己以为的模型名，显示名问题依旧要靠 settings.json 解决——**单靠 (a) 需求不闭环**

**通道 (b)：生成/切换 Claude Code 的 settings.json（cc-switch 方式）**
- 做法：把 §4.3 生成的 env 块合并写入 `~/.claude/settings.json`
- 优点：① **一个通道覆盖全部需求**（BASE_URL、token、6 槽位模型映射、显示名、行为开关、自定义 env）；② cc-switch 已验证这条路可行且有成熟工程经验（原子写 + 备份轮换）；③ 代理转发链路**零改动**
- 缺点：触碰用户的 `~/.claude/settings.json`，必须做合并语义 + 备份 + 还原（§5.3 的写入协议解决）

### 5.2 决策（含三条硬性需求）

硬性需求 2 已直接确认走通道 (b)，且触发时机 = 代理启动 / profile 切换。结合分析：

> **推荐：只做通道 (b)，通道 (a) 本期不做。**
> 理由：(b) 生效后 Claude Code 发出的请求里 `model` 已是上游实际名，代理原样转发即可，(a) 变成纯冗余；(a) 的增量价值只剩「客户端不接 settings.json 管理时的兜底」，该场景不存在于本需求。将来若出现「代理侧兜底改写」的真实需求（例如某网关拒绝 `[1M]` 后缀），再在转发层加 normalize，属独立小改动。

三条硬性需求的落地：

**需求 1（默认不启动代理）**
- `SettingsConfig` 增加 `bool claudeProxyAutoStart = false`（默认关），config.json 持久化
- main.cpp：`startClaudeProxy()` 调用改为读配置门控；**无论是否启动代理，`getClaudeService()` 必须仍被调用**（模块 onStart 依赖 `ClaudeService::global()` 非 null，W3 修过的启动顺序不能回退）
- claude_module `onStart()`：删掉兜底 `startProxy`，只做状态日志；`onStop()` 的 stopProxy 保留（应用退出释放端口，随退随停与「手动启动」语义不冲突）
- settings `doRegenerateToken()`：代理未运行时**按钮置灰**（token 在 startProxy 时生成，没有「预生成」概念）；运行中重新生成后同步触发 settings.json 刷新（token 变了）
- 影响评估（已核）：Home 快捷跳转只是 navigateTo，不依赖代理运行；Claude 页已有「已停止，点右上角启动」空态文案；testConnection 直连上游不经代理。**改动面仅上述四处调用点**

**需求 2（启动即自动接入）**——触发点三个，全部收敛到一个函数 `syncClaudeCodeSettings()`：
1. `startProxy()` 成功后（含手动启动、设置页重新生成 token 的重启）
2. 代理运行中 `setActiveProfile()` 成功后
3. 代理未运行时切换 profile：只更新 DB，**不动 settings.json**（它当前指向已停止的代理或已还原的旧值，写了反而制造死指向）

**需求 3（停止还原边界）**——见 §5.3 写入协议。

### 5.3 settings.json 写入协议（备份 / 合并 / 还原）

**路径**：`core::paths` 新增 `claudeSettingsFile()`（Windows `%USERPROFILE%\.claude\settings.json`，macOS `~/.claude/settings.json`；路径解析按现有平台拆文件模式放 paths 的平台实现里）。目录不存在则创建。

**写入（merge，不是覆盖）**：
1. 读现有 settings.json（不存在 → 视为空对象 `{}`；解析失败 → **中止并报错，绝不写**——宁可不让用户自动接入，不能弄坏用户文件）
2. 整树保留：`permissions`、`model`、`hooks` 等所有非 `env` 顶层字段**原样不动**
3. `env` 对象内：`buildProxyEnv()` 产出的键写入/覆盖（这些键归 DevDesk 管理）；用户自己的其他 env 键原样保留
4. **首份快照**：接管前把原 `env` 对象完整存入 sidecar（`appDataDir()/claude_settings_snapshot.json`：`{ "managedKeys": [...], "envSnapshot": {...} }`）——sidecar 放 DevDesk 数据目录，**不往 settings.json 里塞标记字段**（「非 env 字段原样保留」的红线，多一个下划线键也是违规）
5. 原子写：写同目录临时文件 → rename 替换
6. ~~备份轮换（`.devdesk.bak.<n>` 保留 10 份）~~ **降级取舍（W2 终验 2026-09-22）**：不实现——原子写（tmp+rename）+ 首份快照 sidecar + 崩溃启动补还原已覆盖「不弄坏 / 可还原」的数据安全面，轮换备份的增量收益（用户手改后被托管覆盖的历史找回）不抵其复杂度；已在 KNOWN_LIMITATIONS 登记

**还原（stopProxy 时，含应用退出）**：
- 读 sidecar；`env` 中 managedKeys：快照里有值 → 恢复原值，快照里没有 → 删除该键；其余一切不动；写回后删除 sidecar
- **代理停止 = settings.json 永不指向死代理**：显式停止和应用退出（onStop → stopProxy）都走还原；用户重启 DevDesk 不启动代理，Claude Code 回到自己原本的供应商配置，行为自洽
- **崩溃兜底**：启动时若发现 sidecar 存在（上次没走完还原）且 settings.json 仍含管理的 BASE_URL → 自动补还原，日志说明

**已接受的边界（如实记录）**：接管期间用户手工改了 managedKeys 里的值 → 下次触发同步会被覆盖（managed 键语义归 DevDesk）；用户改其他字段/env 键 → 完全不受影响。

**上层覆盖检测（settings 优先级风险面）**：Claude Code 的 settings 合并顺序是 **managed（企业策略）> project（`<项目>/.claude/settings.json` + `settings.local.json`）> user（`~/.claude/settings.json`，DevDesk 只写这一层）**——上两层里同名的 env 键会**静默覆盖**DevDesk 的接管结果，Claude Code 仍可能不走代理。`syncClaudeCodeSettings()` 写入成功后顺带检测：读当前工作目录的 `.claude/settings.json`、`.claude/settings.local.json` 与 managed 配置路径（Windows managed 路径按官方文档，`core::paths` 提供常量），任一文件的 env 块含 `ANTHROPIC_BASE_URL`/`ANTHROPIC_AUTH_TOKEN`/`ANTHROPIC_MODEL` 键 → 设置页「接管状态」行追加警告（见 §6.3）+ 日志 WARN，**提示但不代改**（上层文件语义归用户/企业管理员，DevDesk 不越权写）。

**实现落点**：新文件 `src/module_claude_proxy/settings_sync.h/.cpp`（ClaudeService 调用；模块内依赖 platform::json + core::paths，分层合法）。ClaudeService 的 startProxy/stopProxy/setActiveProfile 在**成功路径末尾**调 `syncClaudeCodeSettings()` / `restoreClaudeCodeSettings()`——触发逻辑收在 service 层，UI 和 main 不感知。

## 6. UI 设计

### 6.1 Profile 编辑弹窗升级（tabs，用户强调的重点）

现状 520×440 四字段弹窗装不下 6 槽位 + env 编辑器。升级为 **760×600，tabs 三页**（EUI-NEO 有 tabs 组件，能力盘点已核实）：

```
┌─ 编辑 Profile ──────────────────────────────────────────────── ✕ ─┐
│ [ 基础 ] [ 模型映射 ] [ 高级 env ]                    ← tabs      │
│──────────────────────────────────────────────────────────────────│
│  名称      [ 官方中转 ________________________ ]                  │
│  Base URL  [ https://codingxrui.geely-test.com/.../anthropic ]    │
│  API Key   [ 留空 = 保持现有 Key（sk_ind***test）]                │
│                                                                    │
│  ⓘ 模型映射和高级 env 在另外两个标签页配置                         │
│──────────────────────────────────────────────────────────────────│
│                                     [ 取消 ]  [ 保存 ]            │
└──────────────────────────────────────────────────────────────────┘

「模型映射」页：
┌─ 编辑 Profile ──────────────────────────────────────────────── ✕ ─┐
│ [ 基础 ] [●模型映射] [ 高级 env ]                                  │
│──────────────────────────────────────────────────────────────────│
│  槽位          上游模型名（发往上游）         显示名（CC 界面）     │
│  默认 (MODEL)  [ fangzhou/glm-5.3[1M] ____ ] [ fangzhou/glm-5.3 ] │
│  Sonnet        [ fangzhou/glm-5.3[1M] ____ ] [ (自动派生)      ] │
│  Opus          [ ________________________ ] [ __________________ ] │
│  Haiku         [ ________________________ ] [ __________________ ] │
│  Fable         [ ________________________ ] [ __________________ ] │
│  Subagent      [ ________________________ ] [ __________________ ] │
│                                                                    │
│  ⓘ 显示名留空时自动剥除模型名的 [1M]/[200K] 等上下文标记           │
│    槽位留空 = 不写入该 env 键，Claude Code 用内置映射              │
└──────────────────────────────────────────────────────────────────┘

「高级 env」页：
┌─ 编辑 Profile ──────────────────────────────────────────────── ✕ ─┐
│ [ 基础 ] [ 模型映射 ] [●高级 env]                                  │
│──────────────────────────────────────────────────────────────────│
│  额外环境变量（原样写入 settings.json env 块）                     │
│  ┌───────────────────────────────┬───────────────┬────┐           │
│  │ AUTO_COMPACT_ENABLED          │ 1             │ ✕  │           │
│  │ CLAUDE_AUTOCOMPACT_PCT_OVERRIDE│ 75           │ ✕  │           │
│  │ DISABLE_AUTOUPDATER           │ 1             │ ✕  │           │
│  └───────────────────────────────┴───────────────┴────┘           │
│  [+ 添加变量]                                                      │
│                                                                    │
│  ── 从 cc-switch / settings.json 粘贴导入 ──────────────────       │
│  ┌──────────────────────────────────────────────────────┐        │
│  │ { "env": { "ANTHROPIC_BASE_URL": "...", ... } }      │        │
│  │ （多行文本框，粘贴整段 settings.json 或仅 env 对象）    │        │
│  └──────────────────────────────────────────────────────┘        │
│  [ 解析导入 ]   ⓘ BASE_URL/AUTH_TOKEN 归基础页字段，模型键归       │
│                 模型映射页，其余进入上表                            │
└──────────────────────────────────────────────────────────────────┘
```

- **导入解析规则**（复用 `buildProxyEnv` 的逆操作，同为纯函数 `parseImportedEnv()`）：`ANTHROPIC_BASE_URL`/`ANTHROPIC_AUTH_TOKEN` → 基础页两个输入框；`*_MODEL` / `*_MODEL_NAME` / `CLAUDE_CODE_SUBAGENT_MODEL` → 模型映射页对应槽位；其余 → env 键值表。校验见 §7
- 显示名输入框 placeholder 显示派生预览（`(自动派生)` 位实际渲染派生结果灰字）

### 6.2 详情面板与列表卡片

- 详情面板「模型」单字段改为**模型映射摘要**：`默认 fangzhou/glm-5.3[1M] · 6 槽位已配 3` 一行式（点击弹完整映射只读表；完整 6×2 网格塞不进现有面板，弹窗展示）
- 列表卡片 `model:` 行改为显示 default 槽位（未配则旧 `model` 列 fallback）

### 6.3 设置页新增项

```
┌─ 设置 ────────────────────────────────────────────────────────────┐
│ ...（主题 / 字号 / 日志保留，现有）                                 │
│                                                                    │
│  Claude 代理                                                        │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │ 随应用启动代理          [○ 关] ← switch，默认关（硬性需求1）  │   │
│  │ 代理状态                运行中 :8888 / 已停止               │   │
│  │ 代理 Token              a3f8***c21e  [复制] [👁]            │   │
│  │ 重新生成 Token          [ 重新生成 ]（未运行时置灰）         │   │
│  │ Claude Code 接管状态     ● 已接管 ~/.claude/settings.json   │   │
│  │   ⚠ 检测到项目级 .claude/settings.json 含同名 env 键，     │   │
│  │     可能覆盖接管（查看）← 上层覆盖警告，仅出现于检测命中时   │   │
│  └────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────┘
```

「接管状态」行从 sidecar 是否存在推导，让还原机制对用户可见（用户强调的「别弄丢手工配置」的可观测面）。

### 6.4 Claude 页头部（需求 1 的引导）

启动按钮已是现状；补一条未启动提示条：代理未运行且从未启动过时，头部下方显示一次性引导文案「启动代理后将自动接管 Claude Code 配置（~/.claude/settings.json），停止时自动还原」。

## 7. 安全设计

1. **上游 API key 永不入 settings.json / SQLite**（现有 Keychain 机制不变）；settings.json 里只有短命代理 token（本机回环 + Bearer 校验，泄漏面 = 本机进程）
2. **token 展示脱敏**：设置页代理 token 默认 `a3f8***c21e`，[👁] 临时显形，[复制] 进剪贴板（显形不持久化）
3. **settings.json 原子写**：tmp+rename；解析失败中止不写（备份轮换已降级取舍，见 §5.3 第 6 条与 KNOWN_LIMITATIONS）
4. **导入校验**（`parseImportedEnv()` 输入侧）：
   - 顶层必须是 JSON 对象；接受整段 settings.json（取 `env`）或裸 env 对象两种形状
   - env 值必须是字符串（数字/布尔 → 拒绝并提示，不做隐式转换——避免 `"1"` vs `1` 的语义歧义）
   - 键名白名单正则 `^[A-Z][A-Z0-9_]*$`（防注入奇怪键名）；值长度上限 4KB/键、总键数上限 64
   - `ANTHROPIC_API_KEY` 与 `ANTHROPIC_AUTH_TOKEN` 同现 → 警告提示二选一（AUTH_TOKEN 走代理注入优先）
   - 别名表命中（如 `AUTO_COMPACT_ENABLED`）→ 按规则 6 映射为官方变量 + UI 提示；别名表外疑似非官方键 → 透传 + 警告角标（§4.3 规则 6）
5. **删除 profile**：若删的是激活 profile 且代理运行中 → 停代理（触发还原），不让 settings.json 指向无 profile 的代理

## 8. 分阶段实施

### Phase A：数据模型 + 纯函数（无 UI，先行解耦）
- migration v3 两列 + Profile struct 两字段 + CRUD SQL 更新（create/update/list/get 补两列读写）
- `buildProxyEnv()` / `parseImportedEnv()` / displayName 派生函数
- **测试**：迁移幂等（旧库升级 + 新库直建同 schema）；buildProxyEnv 的槽位跳过/派生/extra_env 冲突优先级/空 profile/**别名映射（AUTO_COMPACT_ENABLED → DISABLE_AUTO_COMPACT 值取反）**；parseImportedEnv 的两种形状/非法值/键名校验/别名命中提示
- **验收**：137 用例基线全绿 + 新用例全绿；旧 profile（models_config 空）读出行为与今天一致

### Phase B：代理手动启动（硬性需求 1）
- `SettingsConfig.claudeProxyAutoStart`（默认 false）+ main.cpp 门控（保 `getClaudeService()` 注册顺序）+ claude_module onStart 去兜底 + 设置页开关 + doRegenerateToken 未运行置灰
- **测试**：config 读写往返（默认 false）；启动路径单测（mock 不现实，走人工验证清单）
- **验收**：默认启动后代理**不**监听 8888、无「已启动」日志；开关打开重启后自动启动；模块 onStart 零 WARN；重新生成 token 在停止态不可点

### Phase C：settings.json 接管通道（硬性需求 2+3，依赖 A）
- `core::paths::claudeSettingsFile()`（平台拆文件）+ `settings_sync.h/.cpp`（写入/还原/sidecar/崩溃补还原；备份轮换降级取舍见 §5.3 第 6 条）+ service 三触发点接线
- **测试**（临时目录注入路径，全单测）：合并保留非 env 字段与用户 env 键；快照还原（有值恢复/无值删除）；sidecar 缺失不写不删；解析失败中止；崩溃补还原；**上层覆盖检测**（临时目录伪造 project/local settings.json 含同名 env 键 → 检测命中；无 → 不警告）
- **验收**：启动代理 → settings.json env 块含 BASE_URL=127.0.0.1:port + 代理 token + 模型映射 + 行为开关，其余字段 diff 为空；切 profile → env 跟随；停代理 → settings.json 恢复到接管前内容（逐字节 diff 仅 env 块）；杀进程重启 → 自动补还原
- **人工验证**：真实 Claude Code 连代理跑一次对话（同时验证开放问题 1 的热切换）

### Phase D：UI（依赖 A）✅ 完成（2026-09-22，分支 `feat/provider-config-de`）
- 编辑弹窗 tabs 三页（6 槽位表单 + env 键值表 + 导入多行框）+ 详情面板摘要 + 设置页新块 + Claude 页引导条
- **测试**：表单状态读写往返（填 6 槽位 → 保存 → 重开弹窗回显）；导入 → 三页字段分布正确
- **验收**：贴用户模板 JSON 一键导入后保存、启动代理、settings.json 与模板语义等价（BASE_URL/AUTH_TOKEN 换成代理侧值）；按 CLAUDE.md 验证原则补截屏

### Phase E：收尾 ✅ 完成（2026-09-22，分支 `feat/provider-config-de`）
- CHANGELOG、README、spec §8.3/§10.4 增补引用本文档、git commit

**工作量预估**：A ≈ 0.5 天，B ≈ 0.5 天，C ≈ 1.5 天，D ≈ 1.5 天，E ≈ 0.25 天，合计 **约 4~4.5 个开发日**（含测试与截屏，AI 开发口径）。

## 9. 开放问题

1. **~~热切换时效~~（已关闭，2026-09-22 独立核查）**：官方 settings-reference 文档明确 env 键在保存后「again in the running session when a saved change alters the merged env」重新加载——免重启热切换成立，`model` settings 键的 `/model` 例外不影响 env 路径。Phase C 人工验证时顺手打一条真实请求确认一次即可，不再是不确定项。**状态回填（Phase E，2026-09-22）**：真机验证（真实 Claude Code 连本地代理跑一次对话 + 热切换后立即生效）尚未执行，留给用户人工点验，已在 KNOWN_LIMITATIONS #12 登记
2. **`ANTHROPIC_DEFAULT_FABLE_MODEL` 官方语义**：fable 是较新的模型档位，`_MODEL`/`_MODEL_NAME` 变量名以用户模板为准照抄；若官方文档另有约定（如 FABLE 无 `_NAME` 变体），生成时跳过即可，属 Phase A 纯函数一行调整。**顺带记录**：官方还存在 `ANTHROPIC_DEFAULT_*_MODEL_DESCRIPTION` 变体（模型描述文案），导入时可一并识别进槽位结构（`models_config` 每槽位可加可选 `description` 字段，不生成不丢失），作为导入增强排进 Phase D 可选项。**状态回填（Phase E，2026-09-22）**：`_MODEL_NAME` 变体按用户模板照抄落地（Phase A 已实现）；`_MODEL_DESCRIPTION` 识别为可选项**未做**——导入时该键落入 extra_env 原样透传（不丢失），无真实诉求不做，已在 KNOWN_LIMITATIONS #13 登记
3. **多机/`.claude` 目录定制**（`CLAUDE_CONFIG_DIR`）：一期只认默认 `~/.claude`；有用户反馈再支持自定义路径（settings 页加一个路径输入框的事）。**状态回填（Phase E，2026-09-22）**：维持一期不识别，已在 KNOWN_LIMITATIONS #10 登记
4. **代理侧模型名 normalize（通道 a）**：本期不做；若某网关拒绝 `[1M]` 后缀的真实案例出现，在 proxy_router 转发前加剥除逻辑，独立小改动
5. **~~settings.local.json 优先级~~（已升级为设计项，2026-09-22 核查扩展）**：原以为只有 local 一层，实际覆盖面是三层——**project 级 `.claude/settings.json`（project > user）+ `.claude/settings.local.json` + 企业 managed settings** 都会静默压过用户级接管。检测 + 引导提示已纳入 §5.3「上层覆盖检测」和 §6.3 设置页警告行。残留开放点：managed settings 路径在各平台的准确位置以官方文档为准，Phase C 实现时核实；DevDesk 只提示不代改上层文件（越权写企业策略文件不可接受）
6. **并发写**：DevDesk 写 settings.json 与 Claude Code 自身写（如勾选 trust dialog）撞车——原子写保证不损坏，最后写者胜；概率低，一期接受
