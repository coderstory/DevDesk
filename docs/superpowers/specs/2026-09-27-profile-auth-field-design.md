# Profile「认证字段」（鉴权头三态）— 设计

日期：2026-09-27 ｜ 版本：**V2**（V1 `b060700` 经两条独立审计后修订）｜ 状态：**设计（未实施）** ｜ 关联：`2026-09-22-claude-provider-config-design.md`、`2026-09-27-profile-reverse-sync-and-copy-design.md`（反向同步——本设计**不改**其行为）、`2026-09-27-cc-switch-ref-provider-sync.md`

## 1. 需求与已定决策

| # | 需求（用户原话） | 已定决策（2026-09-27 用户拍板） |
|---|------------------|--------------------------------|
| **R1** | 「cc switch 里有个配置名字叫 认证字段」→「补」 | 在 DevDesk 的 Profile 设置页补齐同名配置项：**profile 级三态**，决定代理转发到上游时注入哪个鉴权头 |

来源证据：cc-switch 该配置项在 **Claude 供应商表单**里是下拉，取值 `ANTHROPIC_AUTH_TOKEN（默认）` / `ANTHROPIC_API_KEY`，hint「选择写入配置的认证环境变量名」（`cc-switch/src/components/providers/forms/ClaudeFormFields.tsx:853-885`）；同名键在 **Codex 供应商表单的「高级选项」折叠区**（仅当上游格式 = Anthropic Messages 时出现，`cc-switch/src/components/providers/forms/CodexFormFields.tsx:875-885`、`:959`）含义是"代理往上游发哪个头（两者只发其一）"（`cc-switch/src/i18n/locales/zh.json:1548-1549`）。DevDesk 的对应物只需后半重含义。

实施前置约束（决策表）：

| # | 决策 | 理由 |
|---|------|------|
| **D-1** | 三态：`both`（默认）/ `bearer`（只发 `Authorization: Bearer`）/ `api_key`（只发 `x-api-key`） | `both` = 现状行为，默认值使改动对**存量数据**零影响（注：§5 的导入联动会**有意**改变导入路径结果，已拍板）；另两态服务"网关只容忍一种鉴权头"的兼容场景 |
| **D-2** | 作用域 = **profile 级**（非全局设置、非共享配置片段） | 鉴权头形态是**网关属性**：官方 API 与各中转可能要求不同头，一个 profile 对一个上游 |
| **D-3** | 生效点 = 上游方言层 `UpstreamDialect::applyHeaders`（流式转发 / 非流式转发 / 「测试连通」三处共用同一注入点） | 该函数既有定位就是"各网关鉴权头形态的收敛点"；三处调用方各自零改动装配逻辑 |
| **D-4** | **接管 settings.json 的占位符不随本字段变**：仍固定 `ANTHROPIC_AUTH_TOKEN = PROXY_MANAGED` | 占位符是给 **Claude Code 客户端**看的（让它"有值可发"），不是给网关的；新版 CC 对未知 API key 弹确认框且默认 `No (recommended)`——cc-switch v3.19.2 正因此把**其接管路径**的占位符从 API_KEY 改成 AUTH_TOKEN（`cc-switch/docs/release-notes/v3.19.2-zh.md:118`；该次改动范围仅 Copilot 接管路径，显式选过 API_KEY 的供应商保持旧行为——但"避免新增确认框"这一动机对 DevDesk 同样成立） |
| **D-5** | 粘贴导入按键名自动选；**反向同步（切 profile 收编 settings.json）不改** | 见 §5：导入侧键名是"cc-switch 当初这么发且能用"的直接证据；反向同步侧 token 通常是占位符（既有守卫 G3 已跳过），且 `both` 是超集、不会失败，收窄只服务罕见场景，属用户显式选择而非被动推断 |
| **D-6** | 客户端送来的凭据仍一律先清（行为不变） | 代理只用 profile 的 key（既有语义） |
| **D-7** | **不做**：全局开关 / 共享配置片段带该字段 / 详情页标签 / 反向同步接入 | 见 §8 |
| **D-8** | **写侧规范化 + 读侧兜底（双保险）**：写路径落库前把值规范成三态之一（未知 → `both`）；读路径 `rowToProfile` 对未知/空值同样兜底为 `both` | 写侧规范化防"未来新调用方把非法值写进库且无人报错"；读侧兜底防手工 SQL / 老库。与 `effectiveModelsConfig` 的"读路径不炸"同款（`provider_config.cpp:191-203`） |

## 2. 现状事实（源码核实，file:line）

| 事实 | 证据 |
|------|------|
| 鉴权头注入**唯一收敛点**：先清掉 `x-api-key` / `authorization` 的所有大小写变体，再写 `x-api-key` + `Authorization: Bearer` **双发** | `upstream_dialect.cpp:244-270`；接口注释 `upstream_dialect.h:43-46`；全仓无第二处注入（其余命中均为注释/文案） |
| 双发的既有理由：真 Anthropic API 收 x-api-key、忽略 Bearer；只认 Bearer 的 Anthropic-compatible 网关（百炼等）也通 | `upstream_dialect.cpp:252-254` |
| `applyHeaders` 恰好 **3 个调用方**，**都持有完整 `Profile` 对象**；三处现有实参各不相同 | 非流式 `proxy_router.cpp:236-237`（局部变量 `apiKey`，其值 = `profile.apiKeyPlain`，见 `:222`）；流式 `streaming_proxy.cpp:331-332`（直接传 `profile.apiKeyPlain`）；连通测试 `claude_service.cpp:1102-1103`（`key` 来自 `loadApiKey(profile.id)`，**不读** `Profile::apiKeyPlain`——`rowToProfile:152` 恒清空该字段） |
| 转发路径**每请求**取激活 profile + Keychain key → 改本字段**下一请求即生效，无需重启代理**，流式与非流式**均成立** | 非流式 `proxy_router.cpp:194`（`getActiveProfile`）+ `:203`（`loadApiKey`）在请求处理内；流式经 `resolveProfile()`（`proxy_router.cpp:324-345` 定义、`:358` 每请求调用）；`ProxyRouter` 无缓存 Profile 成员（`proxy_router.h:69-74`） |
| 方言选择只看 baseUrl host，与鉴权形态无关 | `upstream_dialect.cpp:234-236`（`forBaseUrl`） |
| **读 `claude_profiles` 的 SELECT 共 3 处**（漏改任一处 = 该路径拿不到该列，`rowToProfile` 的 `if (isOk())` 守卫会把缺失**静默**吞成默认值） | `claude_service.cpp:159-163`（`listProfiles`）、`:189-193`（`getProfile`）、**`:614-618`（`getActiveProfile` —— 代理转发主路径）**；`rowToProfile` 的守卫写法见 `:126-150` |
| **`createProfile` 的返回值是手工构造**，只赋 id/name/baseUrl/hint/check/keychainAccount/model/sharedConfigId/isActive/时间——**不含** models_config / extra_env / 新字段 | `claude_service.cpp:376-389` |
| 因此既有"二段补写"模式（先 createProfile 再 updateProfile 补 models/env）会把**结构体默认值**写进新列 | UI 新建二段 `claude_module.cpp:1577-1587`（条件：models/env 非空）；复制二段 `claude_service.cpp:758-763`（条件：源 models/env 非空） |
| `Profile` 是"DB 原始行"风格：JSON 类字段存原文、使用点再解析 | `claude_service.h:40-55`；`provider_config.h:6-9` 注释（"Profile 里只存 JSON 原文…解析收敛到这里"） |
| 迁移机制：版本化列表，最新 v6；`all()` 顺序敏感；每条迁移事务包裹、失败回滚；库比二进制新有降级守卫 | `core/migrations.h:205-231`；`sqlite_db.cpp:300-360`（`runMigrations`，含 `:311-326` 降级守卫） |
| **3 处既有测试硬编码 `schemaVersion() == 6`**（加 v7 必红） | `tests/test_core/test_migrations.cpp:210`、`:291`、`tests/test_modules/test_provider_config.cpp:380` |
| 表单已有 dropdown 先例（`dropdown` 无 position 接口 → 包定位 stack） | `claude_module.cpp:2229-2248`；`#include <components/dropdown.h>`（`:26`）；接口 `../EUI-NEO/components/dropdown.h:56-84`（`size/items/selected/open/onOpenChange/onChange/openUp/theme/zIndex`） |
| 表单基础页现有元素 y 轴：名称 104/122、Base URL 174/192、API Key 244/262、副提示 316、错误红字 340、接管警告 362；弹窗高 600、底部按钮 y=540 | `claude_module.cpp:1628-1700`、`:1992`（h）、`:2051`（按钮） |
| 表单三条生命周期的落点：新建 `openCreateForm`、编辑 `openEditForm`、提交 `submitForm`、导入应用 `doApplyImport` | `claude_module.cpp:1360-1373`、`:1375-1396`、`:1547`、`:1420` |
| `upstream_dialect.h` 当前只 include `<map>` / `<string>`（引入 `AuthField` 需补 include 或前向声明） | `upstream_dialect.h:10-11` |
| 粘贴导入已能分辨两个键名（局部 `sawApiKey` / `sawAuthToken`），但**未外露**；`ImportedConfig` 只有合并后的 `authToken` | `provider_config.cpp:283-284`、`:309-317`、`:338-341`；`provider_config.h:79-94` |
| 导入应用处是"非空才覆盖"的逐项填表；另有"全自动导入"入口先 `openCreateForm` 再 `doApplyImport` | `claude_module.cpp:1420-1428`、`:1498-1512` |

## 3. 数据模型

**DB 列**（migration v7）：

| 项 | 值 |
|----|----|
| 列名 | `claude_profiles.auth_field` |
| 定义 | `ALTER TABLE claude_profiles ADD COLUMN auth_field TEXT NOT NULL DEFAULT 'both';` |
| 值域 | `'both'` / `'bearer'` / `'api_key'`；其余（含空串）一律按 `'both'` 读 |
| 迁移 | `core/migrations.h` 新增 `kClaudeProfileAuthField`（`version = 7`），追加进 `all()` 末尾 |
| 合法性 | SQLite `ADD COLUMN … NOT NULL DEFAULT <常量>` 合法，老行取默认值（本仓既有先例 `migrations.h:151`、`:216`） |

**代码侧类型**：

- `provider_config.h` 新增 `enum class AuthField { Both, Bearer, ApiKey };` + `AuthField authFieldFromText(const std::string&);` + `const char* authFieldToText(AuthField);`（未知/空 → `Both`）
- `Profile` 新增 `std::string authField = "both";`——**带成员初值**并与 DB DEFAULT 对齐：任何"部分字段 Profile"（如二段补写）落库时退化为规范值 `both`，而不是空串（D-8 的双保险之一）。
- **写侧规范化**：`createProfile` / `updateProfile` 在 bind 前统一 `authFieldToText(authFieldFromText(profile.authField))`，保证库里只有三个规范值。

## 4. 行为规格

### 4.1 `applyHeaders` 三态（含空 key）

| `authField` | 清客户端凭据 | 注入 `x-api-key` | 注入 `Authorization: Bearer` |
|-------------|--------------|------------------|------------------------------|
| `both` | 是 | 有 key 时写 | 有 key 时写 |
| `bearer` | 是 | **不写** | 有 key 时写 |
| `api_key` | 是 | 有 key 时写 | **不写** |
| 任意态 × **空 key** | 是 | 不写 | 不写 |

UA 覆盖 / `anthropic-version` 补默认 / `x-opencode-session` 注入（仅 OpencodeZen）**一律不变**。

签名：`void applyHeaders(std::map<std::string, std::string>& headers, const std::string& apiKey, AuthField authField) const;`
**不设默认参数**——调用方漏传即编译失败。

### 4.2 三个调用点（各自传 `authFieldFromText(profile.authField)`）

| 调用点 | 现有实参（保持不变） |
|--------|----------------------|
| `proxy_router.cpp:237` | 局部 `apiKey`（= `profile.apiKeyPlain`） |
| `streaming_proxy.cpp:332` | `profile.apiKeyPlain` |
| `claude_service.cpp:1103`（`testConnection`） | `key`（来自 `loadApiKey`）——保持"探测与转发同源"的既有约束 |

### 4.3 接管 / 还原（**不改**）

`buildProxyEnv`（`provider_config.cpp:355-408`）、占位符常量（`provider_config.h:129`）、接管与还原写入协议**一字不改**（理由见 D-4）。本行显式记录，防止后续"顺手对齐"。

## 5. 导入联动（已定）

粘贴导入（`parseImportedEnv`）按**键名证据**给出建议值：`ImportedConfig` 新增 `std::string authFieldFromEnv;`

| 粘贴内容出现的键 | `authFieldFromEnv` | 语义 |
|------------------|--------------------|------|
| 只有 `ANTHROPIC_API_KEY` | `"api_key"` | 该配置原本以 x-api-key 发送且可用 |
| 只有 `ANTHROPIC_AUTH_TOKEN` | `"bearer"` | 原本以 Bearer 发送且可用 |
| 两键同现 | `"both"` | 保守取超集（既有 `authToken` 取 AUTH_TOKEN + WARN 不变） |
| 都没有 | `""`（空串 = 无证据） | **不覆盖**表单现值 |

`doApplyImport`（`claude_module.cpp:1420`）在 `authFieldFromEnv` 非空时填 `formAuthField_`；"全自动导入"入口（`:1498-1512`，先 `openCreateForm` 再 `doApplyImport`）由该语句覆盖（顺序上 `doApplyImport` 在后）。用户仍可在表单里改。

**行为变化登记（有意，D-5 已拍板）**：只带 `ANTHROPIC_API_KEY` 的粘贴配置，导入后的 profile 会从默认"双发"变为"只发 x-api-key"——这是把 cc-switch 原本的行文形态忠实复刻，不是意外副作用。

**反向同步不改**：`parseEnvForReverseSync` / `mergeReverseSyncEnv` 零改动。**注意**：`syncCaptureActiveProfile` 走的是 `updateProfile`，而该函数现在会 bind 本列——它传入的 `Profile` 来自 `getActiveProfile()`（改造后携带本列），故**自动保持原值**，实施时不得把它置空/置默认。

## 6. 界面

- 表单「基础」页（`drawFormBasicTab`）在 **API Key 输入框（底 y=302）之后**新增一行：标签 y=314（11px `textTertiary`，id `claude.form.basic.authfield.label`）、`components::dropdown` y=332 高 40（id `claude.dialog.form.authfield.dd`，用定位 stack 包裹，仿 `claude.dialog.calls.status.dd` 写法）。
- 三选项文案：`双发（默认）` / `只发 Authorization: Bearer` / `只发 x-api-key`；映射统一走 `authFieldToText` / `authFieldFromText`（UI 不另立平行定义）。
- 灰字说明（id `claude.form.basic.authfield.hint`）：`网关只接受一种鉴权头时切换；默认两种都发`。
- 既有「副提示 316 / 错误 340 / 接管警告 362」三块**整体下移 64px**（→ 380 / 404 / 426），保证不重叠；下移后最低元素 426+36=462 < 底部按钮 540，不越界。
- **已知且接受**：下拉展开时，弹层（3 项约 112px，`zIndex` 20）会**视觉覆盖**其下三条信息行——属标准下拉行为，不改布局。
- 表单状态 `AuthField formAuthField_ = AuthField::Both;`，四条路径同步：
  - `openCreateForm`：重置为 `Both`（防残留上次选择）
  - `openEditForm`：`authFieldFromText(p.authField)`（**编辑必须回显当前值**——否则用户以为看到的是默认值，一保存就把原值坐实覆盖）
  - `submitForm`：新建路径把 `authFieldToText(formAuthField_)` **传进 `createProfile`**（尾参在 `sharedConfigId` 之后）；编辑路径 `p.authField = authFieldToText(formAuthField_)`
  - `doApplyImport`：见 §5

## 7. 文件改动清单

> 行号会漂移；核对时按符号名搜索。**加粗 = 审计发现的阻断级遗漏，实施时必须逐条落实**。

| 文件:位置 | 动作 |
|-----------|------|
| `src/core/migrations.h:218`（v6 之后） | 新增 `kClaudeProfileAuthField`（version 7）+ 追加进 `all()` |
| `src/module_claude_proxy/provider_config.h:79` 附近 | `ImportedConfig` 加 `authFieldFromEnv`；新增 `AuthField` 枚举 + 两 helpers 声明 |
| `src/module_claude_proxy/provider_config.cpp` | 两 helpers 实现；`parseImportedEnv` 按 `sawApiKey`/`sawAuthToken` 填 `authFieldFromEnv` |
| `src/module_claude_proxy/claude_service.h:40-55` | `Profile` 加 `std::string authField = "both";` |
| `src/module_claude_proxy/claude_service.h:97-102` | `createProfile` 追加尾参 `const std::string& authField = "both"` |
| **`src/module_claude_proxy/claude_service.cpp:614-618`** | **`getActiveProfile` 的 SELECT 列清单加 `auth_field`**（代理转发主路径；漏改 = 功能假生效、不报错） |
| `src/module_claude_proxy/claude_service.cpp:159-163`、`:189-193` | `listProfiles` / `getProfile` 的 SELECT 列清单加 `auth_field` |
| `src/module_claude_proxy/claude_service.cpp:118-154` | `rowToProfile` 读该列（`authFieldFromText` 兜底） |
| **`src/module_claude_proxy/claude_service.cpp:376-389`** | **`createProfile` 返回值构造补 `out.authField = <规范化值>`**（否则二段补写把用户选择静默覆盖） |
| `src/module_claude_proxy/claude_service.cpp:252-256` + bind | `createProfile` INSERT 列清单 + 规范化后 bind |
| `src/module_claude_proxy/claude_service.cpp:449`、`:481` | `updateProfile` 两条 UPDATE 各加列 + 规范化 bind |
| `src/module_claude_proxy/claude_service.cpp:707-760` | `duplicateProfile` 把 `src.authField` 传给 `createProfile` |
| `src/module_claude_proxy/upstream_dialect.h:10-11`、`:50-51`；`.cpp:244` | 补 include（或 `enum class AuthField : uint8_t;` 前向声明 + .cpp 内 include；避免头文件环依赖）；`applyHeaders` 第三参 + 三态分支 |
| `src/module_claude_proxy/proxy_router.cpp:237`、`streaming_proxy.cpp:332`、`claude_service.cpp:1103` | 传 `authFieldFromText(profile.authField)` |
| **`src/module_claude_proxy/claude_module.cpp:1360-1373`、`:1375-1396`** | **`openCreateForm` 重置 / `openEditForm` 回显** |
| `src/module_claude_proxy/claude_module.cpp:1570` | **UI 新建调用点把 `authFieldToText(formAuthField_)` 传进 `createProfile`（尾参）** |
| `src/module_claude_proxy/claude_module.cpp:1617` | 编辑保存写 `p.authField` |
| `src/module_claude_proxy/claude_module.cpp:2564-2575`、`:1547`、`:1420`、`:1628-1700` | 表单状态 + 提交 + 导入应用 + 基础页新行与下移 |
| **`tests/test_core/test_migrations.cpp:210`、`:291`、`tests/test_modules/test_provider_config.cpp:380`** | **`schemaVersion()` 期望值 6 → 7** |
| `tests/test_modules/test_provider_config.cpp`、`test_upstream_dialect.cpp`、`test_claude_service.cpp`、`test_claude_ui.cpp` | 用例扩展（见 §9） |

## 8. 不做（YAGNI）

1. **全局开关**：鉴权头是网关属性，按 profile 走（D-2）。
2. **共享配置片段带该字段**：片段定位是"纯 env 装饰层，不进路由 / 不进凭据链"（`migrations.h:193-204`）。
3. **详情页 / 列表卡片加标签**。
4. **反向同步接入**（§5 已述）。
5. **不改** `parseImportedEnv` 的既有校验语义、`buildProxyEnv` 的任何输出、`rowToProfile` 的既有守卫写法（只新增一列读取）。

## 9. 测试策略

| 层 | 用例 |
|----|------|
| 纯函数（`test_provider_config.cpp`） | 三态往返；未知值 / 空串 → `Both`；`parseImportedEnv` 四态证据（只有 API_KEY / 只有 AUTH_TOKEN / 同现 / 都无 → `""`） |
| 方言层（`test_upstream_dialect.cpp`） | 三态 × 有 key / 空 key 共 6 条：断言目标头存在**且另一个头不存在**（负断言是核心）；UA / `anthropic-version` / `x-opencode-session` 不受影响 |
| service（`test_claude_service.cpp`） | ① 新建默认 `both`；② 显式三态往返（create → **`getActiveProfile`** → 断言——**这是"漏改 SELECT 列静默失效"的回归闸**）；③ `updateProfile` 两条分支都能改该字段；④ **二段补写不覆盖**：createProfile 带 `bearer` + 二段 updateProfile 补 models/env → 仍为 `bearer`；⑤ `duplicateProfile` 副本字段与源一致；⑥ 写侧规范化：传非法串（直接构造 Profile）→ 落库为 `both` |
| 数据层（`test_core/test_migrations.cpp` 等） | 三处 `schemaVersion()` 期望更新为 7；老行迁移后为 `both` |
| 无头 UI（`test_claude_ui.cpp`） | ① 表单内 `claude.dialog.form.authfield.dd` 存在；② 新建选 `bearer` 保存 → DB 值 = `bearer`（含带槽位/env 的二段路径）；③ 编辑打开 `api_key` 的 profile → 下拉回显正确 |
| 人工点验 | 同一 profile 依次设三态 → 点「测试连通」+ 实发一轮 → 上游侧确认头形态（只发一种时另一个头不出现） |

## 10. 拆任务（每项 ≤0.5 天；**涉及编译 / 测试的子任务一律 worktree 隔离**）

| 里程碑 | 内容 | 依赖 | 验收 |
|--------|------|------|------|
| **M1** 数据层 | 迁移 v7 + `Profile.authField` + **三处 SELECT** / `rowToProfile` / `createProfile`（INSERT + **返回值**）/ `updateProfile` 两分支 / `duplicateProfile` + 三处 `schemaVersion` 断言更新 + service 用例 | — | service 与迁移用例全绿 |
| **M2** 纯函数 + 方言层 | `AuthField` 枚举 + 两 helpers + `applyHeaders` 第三参 + 三调用点 + 两文件用例 | — | 方言 6 条用例全绿（含负断言） |
| **M3** 导入联动 | `ImportedConfig.authFieldFromEnv` + `parseImportedEnv` 填值 + 纯函数用例 | M2 | 四态证据用例全绿 |
| **M4** UI | 基础页新行 + 表单状态 + 四条路径（新建重置 / 编辑回显 / 提交传参 / 导入应用）+ 无头用例 + 截屏 | M1, M3 | 无头用例全绿；截屏留档 |

**M1 与 M2 有交叠文件**（`claude_service.cpp`）→ 合并为**一条串行链**，不并行；M3、M4 依赖前序。

## 11. 已接受的边界

1. 选 `bearer` / `api_key` = **主动放弃**另一种头：打到需要另一种头的网关会 401/403，用户自负（表单说明已提示"默认两种都发"）。
2. 多个 profile 指向同一网关需各设一次（不做继承 / 不挂共享片段）。
3. 代理运行中改该字段：**下一请求即生效**（流式与非流式均每请求取 profile），无需重启代理。
4. 组合行为不变：空 key 仍不发任何鉴权头；客户端凭据仍一律丢弃。
5. 存量数据零变化（迁移后老行 = `both`）；但**导入路径结果有意改变**（§5）。
6. 本次发布后，**旧版 exe 打开新库会显式失败**（既有降级守卫 `sqlite_db.cpp:311-326`，非本设计引入的新风险）。
7. 下拉展开时弹层覆盖其下三条信息行（§6，标准下拉行为）。
8. "真密钥永不进 settings.json"只对 **DevDesk 自管的 profile 密钥链**成立（Keychain → 代理转发时注入）；用户在「高级 env」页手填的凭据仍会原样写入 settings.json——既有行为，不在本设计范围。
