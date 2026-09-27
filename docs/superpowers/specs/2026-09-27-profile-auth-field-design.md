# Profile「认证字段」（鉴权头三态）— 设计

日期：2026-09-27 ｜ 状态：**设计（未实施）** ｜ 关联：`2026-09-22-claude-provider-config-design.md`（§4.3 env 生成 / 接管写入协议）、`2026-09-27-profile-reverse-sync-and-copy-design.md`（反向同步——本设计**不改**其行为）、`2026-09-27-cc-switch-ref-provider-sync.md`（cc-switch 对照研究）

## 1. 需求与已定决策

| # | 需求（用户原话） | 已定决策（2026-09-27 用户拍板） |
|---|------------------|--------------------------------|
| **R1** | 「cc switch 里有个配置名字叫 认证字段」→「补」 | 在 DevDesk 的 Profile 设置页补齐同名配置项：**profile 级三态**，决定代理转发到上游时注入哪个鉴权头 |

来源证据：cc-switch 的该配置项在 Claude 供应商表单里是一个下拉，取值 `ANTHROPIC_AUTH_TOKEN（默认）` / `ANTHROPIC_API_KEY`，hint「选择写入配置的认证环境变量名」（`cc-switch/src/components/providers/forms/ClaudeFormFields.tsx:853-885`）；同一名字在 Codex 路由高级选项里含义是"代理往上游发哪个头（两者只发其一）"（`cc-switch/src/i18n/locales/zh.json:1548-1549`）。DevDesk 的对应物只需后半重含义——真密钥永不出现在 settings.json（存 Keychain，由代理转发时注入）。

实施前置约束（决策表）：

| # | 决策 | 理由 |
|---|------|------|
| **D-1** | 三态：`both`（默认）/ `bearer`（只发 `Authorization: Bearer`）/ `api_key`（只发 `x-api-key`） | `both` = 现状行为，默认值使本次改动对存量数据与既有用户零影响；另两态服务"网关只容忍一种鉴权头"的兼容场景 |
| **D-2** | 作用域 = **profile 级**（非全局设置、非共享配置片段） | 鉴权头形态是**网关属性**：官方 API 与各中转可能要求不同头，一个 profile 对一个上游 |
| **D-3** | 生效点 = 上游方言层 `UpstreamDialect::applyHeaders`（流式转发 / 非流式转发 / 「测试连通」三处共用同一注入点） | 该函数既有定位就是"各网关鉴权头形态的收敛点"；三处调用方各自零改动装配逻辑 |
| **D-4** | **接管 settings.json 的占位符不随本字段变**：仍固定 `ANTHROPIC_AUTH_TOKEN = PROXY_MANAGED` | 占位符是给 **Claude Code 客户端**看的（让它"有值可发"），不是给网关的；新版 CC 对未知 API key 弹确认框且默认 `No (recommended)`——cc-switch v3.19.2 正因此把占位符从 API_KEY 改成 AUTH_TOKEN（`cc-switch/docs/release-notes/v3.19.2-zh.md`）。改用 API_KEY 占位会把用户挡在登录门外 |
| **D-5** | 粘贴导入按键名自动选；**反向同步（切 profile 收编 settings.json）不改** | 见 §5：导入侧键名是"cc-switch 当初这么发且能用"的直接证据；反向同步侧 token 通常是占位符（既有守卫 G3 已跳过），且 `both` 是超集、不会失败，收窄只服务罕见场景，属用户显式选择而非被动推断 |
| **D-6** | 客户端送来的凭据仍一律先清（行为不变） | 代理只用 profile 的 key（既有语义） |
| **D-7** | **不做**：全局开关 / 共享配置片段带该字段 / 详情页标签 / 反向同步接入 | 见 §7 |
| **D-8** | DB 读到未知值 / 空值 → 兜底 `both`（读路径容错） | 与 `effectiveModelsConfig` 的"读路径不炸"同款（`provider_config.cpp:191-203`） |

## 2. 现状事实（源码核实，file:line）

| 事实 | 证据 |
|------|------|
| 鉴权头注入**唯一收敛点**：先清掉 `x-api-key` / `authorization` 的所有大小写变体，再写 `x-api-key` + `Authorization: Bearer` **双发** | `upstream_dialect.cpp:244-270`（`applyHeaders`）；接口注释 `upstream_dialect.h:43-46` |
| 双发的既有理由：真 Anthropic API 收 x-api-key、忽略 Bearer；只认 Bearer 的 Anthropic-compatible 网关（百炼等）也通 | `upstream_dialect.cpp:252-254` |
| `applyHeaders` 的调用方三处，**都持有完整 `Profile` 对象**（非流式 / 流式 / 连通测试） | `proxy_router.cpp:236-237`、`streaming_proxy.cpp:331-332`、`claude_service.cpp:1102-1103` |
| 转发路径**每请求**读激活 profile + Keychain key → 改本字段**即时生效，无需重启代理** | `proxy_router.cpp:195-226`（`getActiveProfile` + `loadApiKey` 均在请求处理内） |
| 方言选择只看 baseUrl host，与鉴权形态无关 | `upstream_dialect.cpp:234-236`（`forBaseUrl`） |
| `Profile` 是"DB 原始行"风格：JSON 类字段存原文、使用点再解析 | `claude_service.h:40-55`；`modelsConfigJson` / `extraEnvJson` 的用法（`parseModelsConfig` / `parseExtraEnv`） |
| 迁移机制：版本化列表，当前最新 v6；`all()` 顺序敏感 | `core/migrations.h:205-231`（`kClaudeSharedConfigs` = v6） |
| 表单已有 dropdown 先例（含"dropdown 无 position 接口 → 包定位 stack"的写法） | `claude_module.cpp:2229-2234`；`#include <components/dropdown.h>` 在 `claude_module.cpp:26` |
| 表单基础页现有元素 y 轴：名称 104/122、Base URL 174/192、API Key 244/262、副提示 316、错误红字 340、接管警告 362 | `claude_module.cpp:1628-1700`（`drawFormBasicTab`） |
| 粘贴导入已能分辨两个键名（局部变量 `sawApiKey` / `sawAuthToken`），但**未外露**到结果结构 | `provider_config.cpp:283-284`、`:309-314`；`ImportedConfig`（`provider_config.h:79-95`）只有合并后的 `authToken` |
| 导入应用处是"非空才覆盖"的逐项填表 | `claude_module.cpp:1420-1428`（`doApplyImport`） |

## 3. 数据模型

**DB 列**（migration v7）：

| 项 | 值 |
|----|----|
| 列名 | `claude_profiles.auth_field` |
| 定义 | `ALTER TABLE claude_profiles ADD COLUMN auth_field TEXT NOT NULL DEFAULT 'both';` |
| 值域 | `'both'` / `'bearer'` / `'api_key'`；其余（含空串）一律按 `'both'` 读 |
| 迁移 | `core/migrations.h` 新增 `kClaudeProfileAuthField`（`version = 7`），追加进 `all()` 末尾（顺序敏感） |

**代码侧类型**：

- `provider_config.h` 新增 `enum class AuthField { Both, Bearer, ApiKey };` + `AuthField authFieldFromText(const std::string&);` + `const char* authFieldToText(AuthField);`
  - 放 `provider_config` 的理由：与 `applyEnvAliases` / `parseImportedEnv` 同层（都是"配置语义"纯函数），且 `claude_service.h` 不必新增 include——维持 Profile 存原文的既有约定。
- `Profile` 新增 `std::string authField;`（存 DB 原文；默认 `"both"`，即 DB DEFAULT 的镜像；使用点经 `authFieldFromText` 解析）。

## 4. 行为规格

### 4.1 `applyHeaders` 三态（含空 key）

| `authField` | 清客户端凭据 | 注入 `x-api-key` | 注入 `Authorization: Bearer` |
|-------------|--------------|------------------|------------------------------|
| `both` | 是 | 有 key 时写 | 有 key 时写 |
| `bearer` | 是 | **不写** | 有 key 时写 |
| `api_key` | 是 | 有 key 时写 | **不写** |
| 任意态 × **空 key** | 是 | 不写 | 不写 |

UA 覆盖 / `anthropic-version` 补默认 / `x-opencode-session` 注入（仅 OpencodeZen）**一律不变**。

函数签名：`void applyHeaders(std::map<std::string, std::string>& headers, const std::string& apiKey, AuthField authField) const;`
**不设默认参数**——调用方漏传即编译失败，避免"漏改一处、静默保持旧行为"。

### 4.2 三个调用点

| 调用点 | 传参 |
|--------|------|
| `proxy_router.cpp:237`（非流式转发） | `applyHeaders(fwd, apiKey, authFieldFromText(profile.authField))` |
| `streaming_proxy.cpp:332`（流式转发） | 同上（该处现有实参是 `profile.apiKeyPlain`） |
| `claude_service.cpp:1103`（`testConnection`「测试连通」） | 同上——保持"探测与转发同源"的既有约束（`claude_service.cpp:1099-1101` 注释） |

### 4.3 接管 / 还原（**不改**）

`buildProxyEnv`（`provider_config.cpp:355-408`）、占位符常量（`provider_config.h:129`）、接管与还原写入协议**一字不改**——理由见 D-4。本行显式记录，防止后续"顺手对齐"。

## 5. 导入联动（已定）

粘贴导入（`parseImportedEnv`）按**键名证据**给出建议值：`ImportedConfig` 新增 `std::string authFieldFromEnv;`

| 粘贴内容出现的键 | `authFieldFromEnv` | 语义 |
|------------------|--------------------|------|
| 只有 `ANTHROPIC_API_KEY` | `"api_key"` | 该配置原本以 x-api-key 发送且可用 |
| 只有 `ANTHROPIC_AUTH_TOKEN` | `"bearer"` | 原本以 Bearer 发送且可用 |
| 两键同现 | `"both"` | 保守取超集（既有 `authToken` 取 AUTH_TOKEN + WARN 的行为不变） |
| 都没有 | `""`（空串 = 无证据） | **不覆盖**表单现值 |

`doApplyImport`（`claude_module.cpp:1420`）在 `authFieldFromEnv` 非空时填表单的 `formAuthField_`；用户仍可在表单里改。

**反向同步不改**（`parseEnvForReverseSync` / `mergeReverseSyncEnv` 零改动）：见 D-5。

## 6. 界面

- 表单「基础」页（`drawFormBasicTab`）在 **API Key 输入框（y 244/262）之后**新增一行：
  - 标签「认证字段」（沿用 11px `textTertiary` 样式），元素 id `claude.form.basic.authfield.label`
  - `components::dropdown`（id `claude.dialog.form.authfield.dd`，用定位 stack 包裹，仿 `claude.dialog.calls.status.dd` 的写法）
  - 三选项文案：`双发（默认）` / `只发 Authorization: Bearer` / `只发 x-api-key`
  - 一行灰字说明（id `claude.form.basic.authfield.hint`）：`网关只接受一种鉴权头时切换；默认两种都发`
- 其后的「副提示 / 错误红字 / 接管警告」三块整体下移一行（约 64 px），保证 600 高弹窗不溢出、且不与条件元素（错误、接管警告）重叠。
- 表单状态新增 `AuthField formAuthField_ = AuthField::Both;`；新建重置 / 编辑载入 / 提交（`submitForm`）三条路径同步；`serializeFormData` 不涉及（该字段不是 env/模型的 JSON）。

## 7. 文件改动清单

| 文件:位置 | 动作 |
|-----------|------|
| `src/core/migrations.h:218`（v6 之后） | 新增 `kClaudeProfileAuthField`（version 7）+ 追加进 `all()` |
| `src/module_claude_proxy/provider_config.h:79` 附近 | `ImportedConfig` 加 `authFieldFromEnv`；新增 `AuthField` 枚举 + 两个 helpers 声明 |
| `src/module_claude_proxy/provider_config.cpp` | 两 helpers 实现；`parseImportedEnv` 按 `sawApiKey`/`sawAuthToken` 填 `authFieldFromEnv` |
| `src/module_claude_proxy/claude_service.h:40-55` | `Profile` 加 `std::string authField;` |
| `src/module_claude_proxy/claude_service.h:97` | `createProfile` 追加尾参 `const std::string& authField = "both"`（默认值保持既有调用方零改动） |
| `src/module_claude_proxy/claude_service.cpp:159`、`:190` | 两处 SELECT 列清单加 `auth_field` |
| `src/module_claude_proxy/claude_service.cpp:118-154` | `rowToProfile` 读该列 |
| `src/module_claude_proxy/claude_service.cpp:252-256` | `createProfile` INSERT 列表 + bind |
| `src/module_claude_proxy/claude_service.cpp:449`、`:481` | `updateProfile` 两条 UPDATE（换 key / 不换 key）各加列 + bind |
| `src/module_claude_proxy/claude_service.cpp:701-760` | `duplicateProfile` 把 `src.authField` 传给 `createProfile` |
| `src/module_claude_proxy/upstream_dialect.h:50-51` / `.cpp:244` | `applyHeaders` 第三参 + 三态分支 |
| `src/module_claude_proxy/proxy_router.cpp:237`、`streaming_proxy.cpp:332`、`claude_service.cpp:1103` | 传 `authFieldFromText(profile.authField)` |
| `src/module_claude_proxy/claude_module.cpp:1628-1700`（基础页）/ `:2560-2583`（表单状态）/ `:1547`（`submitForm`）/ `:1420`（`doApplyImport`） | UI 新行 + 状态 + 三条路径 |
| `tests/test_modules/test_provider_config.cpp`、`test_upstream_dialect.cpp`、`test_claude_service.cpp`、`test_claude_ui.cpp` | 用例扩展（见 §9） |

## 8. 不做（YAGNI）

1. **全局开关**：鉴权头是网关属性，按 profile 走（D-2）。
2. **共享配置片段带该字段**：片段定位是"纯 env 装饰层，不进路由 / 不进凭据链"（`migrations.h:193-204`），鉴权头形态不属于 env。
3. **详情页 / 列表卡片加标签**：要看就在表单里看，加标签是纯展示增量。
4. **反向同步接入**（§5 已述）。
5. **不改** `parseImportedEnv` 的既有校验语义（值 >4KB、键数 >64、非法整体 fail）与 `buildProxyEnv` 的任何输出。

## 9. 测试策略

| 层 | 用例 |
|----|------|
| 纯函数（`test_provider_config.cpp`） | `authFieldToText` / `authFieldFromText` 三态往返；未知值 `"x"` / 空串 → `Both`；`parseImportedEnv` 四态证据（只有 API_KEY / 只有 AUTH_TOKEN / 同现 / 都无 → `""`） |
| 方言层（`test_upstream_dialect.cpp`） | 三态 × 有 key / 空 key 共 6 条：断言目标头存在**且另一个头不存在**（负断言是核心）；UA、`anthropic-version`、`x-opencode-session` 不受影响 |
| service（`test_claude_service.cpp`） | 新建默认 `both`（不传参 → 读回 `both`，即 DB DEFAULT 生效）；显式三态往返（create → getProfile）；`updateProfile` 两条分支（换 key / 不换 key）都能改该字段；`duplicateProfile` 副本字段与源一致 |
| 无头 UI（`test_claude_ui.cpp`） | 表单内 `claude.dialog.form.authfield.dd` 存在；切换选项后保存 → DB 字段值正确；编辑态载入回显 |
| 人工点验 | 起代理 → 同一 profile 依次设三态 → 点「测试连通」+ 实发一轮 → 上游侧确认收到的头形态（只发一种时另一个头不出现） |

## 10. 拆任务（每项 ≤0.5 天；**涉及编译 / 测试的子任务一律 worktree 隔离**）

| 里程碑 | 内容 | 依赖 | 验收 |
|--------|------|------|------|
| **M1** 数据层 | 迁移 v7 + `Profile.authField` + 两处 SELECT / `rowToProfile` / `createProfile` / `updateProfile` 两分支 / `duplicateProfile` + service 用例 | — | service 用例全绿 |
| **M2** 纯函数 + 方言层 | `AuthField` 枚举 + 两 helpers + `applyHeaders` 第三参 + 三调用点 + 两文件用例 | — | 方言 6 条用例全绿（含负断言） |
| **M3** 导入联动 | `ImportedConfig.authFieldFromEnv` + `parseImportedEnv` 填值 + 纯函数用例 | M2 | 四态证据用例全绿 |
| **M4** UI | 基础页新行 + 表单状态 + 载入 / 提交 / `doApplyImport` + 无头用例 + 截屏 | M1, M3 | 无头用例全绿；截屏留档 |

**M1 与 M2 有交叠文件**（`claude_service.cpp`：M1 改 CRUD、M2 改调用点）→ 合并为**一条串行链**，不并行。M3、M4 依赖前序。

## 11. 已接受的边界

1. 选 `bearer` / `api_key` = **主动放弃**另一种头：打到需要另一种头的网关会 401/403，该选择由用户自负（表单说明文案已提示"默认两种都发"）。
2. 多个 profile 指向同一网关时需各设一次（不做继承 / 不挂到共享片段）。
3. 代理运行中改该字段：**下一请求即生效**（转发每请求读 profile），无需重启代理（§2 已证）。
4. 组合行为不变：空 key 仍不发任何鉴权头；客户端凭据仍一律丢弃。
5. 存量数据行为零变化：迁移后所有老行为 `both`，即今天的双发。
