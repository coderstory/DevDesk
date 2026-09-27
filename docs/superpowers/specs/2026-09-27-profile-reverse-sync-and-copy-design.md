# Profile 反向同步与复制 — 设计

日期：2026-09-27 ｜ 状态：**设计（未实施）** ｜ 关联：`2026-09-22-claude-provider-config-design.md`（§4.3 env 生成 / §5.3 写入协议 / §6.1 导入）、`2026-09-27-orchestration.md`（"接管默认关"的当前状态口径）

## 1. 需求与已定决策

| # | 需求（用户原话） | 已定决策（用户拍板，实施前置约束） |
|---|------------------|-----------------------------------|
| **R1** | 「当用户在 claude code 中修改配置后，需要同步到 app 的配置中，同步实际为切换 profile 的时候，也就是用户切换 profile 前，将 settings 同步到当前激活的 profile 内」 | S-1 回写**全量 env（带守卫）**：BASE_URL / key / 模型槽位 / 额外 env 都回写，跳过**本地代理形态**的值；S-2 回写失败**中止本次切换**（保住 profile 不被半写；**文件不存在** → 不中止——no-op 成功照常切换，2026-09-27 主会话裁定）；S-5 **缺席即清空**（2026-09-27 用户拍板修订）：settings.json 为唯一真相——托管字段（`base_url` / key / 6 个模型槽位 / 4 个 `_NAME` / `extra_env` 的键——集合定义见 §3.4）在 settings.json 中缺席 → profile 对应值清空（2026-09-27 主会话裁定）；**守卫键不写也不清**（本地代理形态值 / 空值 / sidecar 托管键一律保持 profile 原值） |
| **R2** | 「profile 新增复制功能」 | S-3 **含 key 全复制**（名称 + base_url + 槽位 + 额外 env + API key）；S-4 副本**不激活**，名称自动加「副本」后缀，重名按既有唯一约束避让 |

用户价值：R1 = 用户在 Claude Code 侧（或经 cc-switch / 手改 settings.json）调整过配置后，切 profile 不会把这些改动丢掉——它们先被收进"当前那一份"profile；R2 = 多供应商场景下不用从零重填一套配置。

## 2. 现状事实（源码核实，file:line）

| 事实 | 证据 |
|------|------|
| `setActiveProfile` 是激活状态的**唯一咽喉**：生产仅两处调用方（UI「启用」按钮、`createProfile` 首建自动激活） | `claude_module.cpp:2300` / `claude_service.cpp:356`（全仓 grep） |
| 切 profile 已有"代理运行中 → 写 settings.json"分支（本需求要在它**之前**插入反向读） | `claude_service.cpp:662-667`（`if (result.isOk() && isProxyRunning()) syncTakeOver();`） |
| 接管/还原的另外两个触发点：`startProxy()` 成功末尾、`stopProxy()` 末尾 | `claude_service.cpp:970` / `:986` |
| 接管通道门控：`settingsTakeOverEnabled_` 默认 **false**，且 `src/` 中**无人调用** `setSettingsTakeOverEnabled`（＝当前状态下 DevDesk 不碰 settings.json，编排表 #8 计划恢复） | `claude_service.h:227-228` / `:298`、`claude_service.cpp:1079`、`:1108` |
| 接管写入的"代理形态值"只有两个：`BASE_URL=http://127.0.0.1:<port>`、`AUTH_TOKEN=PROXY_MANAGED` | `provider_config.cpp:358`、`:361`、`kProxyAuthPlaceholder`（`provider_config.h:113`） |
| sidecar 结构：`{"managedKeys": {键:""}, "envSnapshot": {...}}`（managedKeys 是**对象**不是数组，`JsonObject` 无 erase） | `settings_sync.cpp:133-150`、`:334-347` |
| 严格版导入解析 `parseImportedEnv`：非字符串值 → 整体 fail、>4KB → fail、>64 键 → fail | `provider_config.cpp:283-293`、`:266-270` |
| 别名表 / 官方键白名单已存在（`AUTO_COMPACT_ENABLED` → `DISABLE_AUTO_COMPACT`，值取反） | `provider_config.cpp:34-36`、`:41-63`、`:203-225` |
| `updateProfile`：`apiKeyPlain` 非空 → **先 Keychain 后单条 UPDATE**；旧 `model` 列自动跟随 `models_config.default.model` | `claude_service.cpp:432-470`、`:424-430` |
| `createProfile`：Keychain 失败 → **已内建 DELETE 回滚**；成功则回填 `keychain_account` | `claude_service.cpp:305-342` |
| Windows Keychain 覆盖写是 `ios::trunc` 后写，失败**不保证**保留旧值 | `keychain_win.cpp:84-99` |
| 仓库中**没有** `saveApiKey` 一类 API：写 key 的唯一 service 入口是 `updateProfile` 里 `apiKeyPlain` 非空的分支（底层 `platform::keychain::storeSecret`），读是 `loadApiKey`（底层 `loadSecret`） | `claude_service.cpp:432-441`、`:584-591`、`platform/keychain.h:13-23` |
| 详情页操作区是 4 键按钮组（`actW = (w0 - actGap*3)/4`），列表卡片无右键菜单（`claude_module.cpp` 零 `contextMenu`） | `claude_module.cpp:724-824`、`:1132` |
| 「导入当前配置」已有"读 `svc->settingsSyncPaths().settingsFile`"的取路径范式 | `claude_module.cpp:1313-1317` |

## 3. R1：反向同步（settings.json → 当前激活 profile）

### 3.1 触发点：`ClaudeService::setActiveProfile` 内部第一段（**不是** UI 层）

放置位置：`claude_service.cpp:623` 的 `setActiveProfile` 内，**校验 id 存在之后、清零/置位事务之前**。

| 候选 | 结论 | 理由 |
|------|------|------|
| UI 层 `doSetActive`（`claude_module.cpp:2296`）调用前 | ❌ | ① 漏掉 `createProfile(activateIfNoneActive=true)` 这条内部路径（`claude_service.cpp:356`）；② S-2「中止切换」在 UI 层无法强制——service 被直接调用时不变式失效；③ 反向写要发生在"代理运行中 syncTakeOver"（`:664-666`）**之前**，两者同函数才能保证顺序，跨层无法保证 |
| service 层 `setActiveProfile` 内 | ✅ | 唯一咽喉：UI 与首建自动激活两条路径都覆盖；与既有 `syncTakeOver/syncRestore` 同层同款（`claude_service.cpp:1075-1122`）；测试可直接驱动 |

调用序号（全部在 `setActiveProfile` 内）：

1. `id == 0` / `getProfile` 失败 → 原样返回（不动文件）
2. `getActiveProfile()` 失败（无激活）→ **直接跳过反向同步，不读文件**（否则首建 profile 会被一个坏 settings.json 卡住）
3. `syncCaptureActiveProfile()` — 读 settings.json → 按 §3.4 语义对齐（出现覆盖 + 缺席清空，守卫键跳过）→ 写激活 profile；**文件不存在 → no-op 成功（G7）继续切换；文件为空 → 同款 no-op 成功（G9，视为不存在）；文件存在但读不了/解析失败（G8）→ return fail（S-2，本次不切换）**（2026-09-27 主会话裁定）
4. 既有事务（清零 → 置位）→ 5. 既有 `isProxyRunning() → syncTakeOver()`

### 3.2 字段映射表（env 键 → profile 字段）

| # | settings.json env 键 | 目标 | 备注 |
|---|----------------------|------|------|
| 1 | `ANTHROPIC_BASE_URL` | `Profile.baseUrl`（去尾斜杠，与 `updateProfile:402` 同款清洗） | 守卫 G2 |
| 2 | `ANTHROPIC_AUTH_TOKEN` | Keychain `claude:<id>`（经 `updateProfile.apiKeyPlain`） | 守卫 G3 |
| 3 | `ANTHROPIC_API_KEY` | 同上，**仅当 #2 缺席** | 两键同现 → AUTH_TOKEN 优先 + WARN（与 `parseImportedEnv:299-308` 同口径） |
| 4 | `ANTHROPIC_MODEL` | `models_config.default.model`（旧 `model` 列由 `updateProfile:424-430` 自动跟随） | |
| 5-8 | `ANTHROPIC_DEFAULT_{SONNET,OPUS,HAIKU,FABLE}_MODEL` | 对应槽位 `model` | |
| 9 | `CLAUDE_CODE_SUBAGENT_MODEL` | `subagent.model` | |
| 10-13 | `ANTHROPIC_DEFAULT_{SONNET,OPUS,HAIKU,FABLE}_MODEL_NAME` | 对应槽位 `name` | `ANTHROPIC_MODEL` / `SUBAGENT` 无 `_NAME` 变体（与 `kImportSlots:82-89` 一致） |
| 14 | 其余键 → `applyEnvAliases` 归一后 | `extra_env`（键值对，**以 settings.json 为准对齐**——出现覆盖；**键集合完全由 settings.json 决定**：env 中除去托管字段后的全部键经别名表归一，某键缺席即从 `extra_env` 删除；2026-09-27 主会话裁定） | 别名表复用 `kEnvAliases:34`；未识别键照收（`knownOfficialEnvKeys:41` 只用于 UI 角标，不阻断） |

映射表本身**复用 `kImportSlots`（`provider_config.cpp:82`）与 `applyEnvAliases`（`:203`）**，不新写键名常量。

**不直接复用 `parseImportedEnv` 本体**：它的"非法即整体 fail"（`:283/:289/:266`）是**粘贴导入 UI 的契约**（不部分导入），搬到切换路径会让一个无关的怪异 env 值阻断用户切 profile。反向同步用容错版（跳过 + WARN），见 §3.5。

### 3.3 守卫表（哪些值绝不回写、也绝不清空）

| # | 守卫 | 判定 | 动作 |
|---|------|------|------|
| G0 | 无激活 profile | `getActiveProfile()` 失败 | 不读文件、不写库（§3.1 第 2 步） |
| G1 | **sidecar 托管键** | sidecar 存在且键 ∈ `managedKeys` | 跳过该键——**不写也不清**（这些值来自 profile 自身，回写无意义；也是"接管期间自我污染"的主防线） |
| G2 | **loopback BASE_URL** | `isLoopbackUrl(值)`：剥 scheme → 取 `host[:port]`（IPv6 去方括号）→ 小写后 ∈ {`127.0.0.0/8` 全段, `localhost`, `::1`} | 跳过 `base_url`（**不写也不清**），**并联动跳过本次的 token（#2/#3）**（同样不写也不清） |
| G3 | 占位 token | 值 == `kProxyAuthPlaceholder`（`"PROXY_MANAGED"`） | 跳过 token（**不写也不清**——保持 profile 原密钥） |
| G4 | 空值 | 键存在但值为空串 | 跳过该键——**不写也不清**（不拿空串覆盖，也不因空值清空 profile 现有值） |
| G5 | 键缺席 | 键不在 settings.json env 中 | **清空**对应 profile 字段（**缺席即清空**，2026-09-27 用户拍板修订；守卫例外见下注与 §3.4） |
| G6 | 值形态非法 | 非字符串 / 长度 > 4096 | 跳过该键 + WARN（**不**中止切换；同样**不写也不清**） |
| G7 | **无真相源：文件不存在** | settings.json 路径不存在 | 整次 no-op 成功（不是失败；**不写 profile、不清空任何字段**，**照常完成本次 profile 切换**）——用户可能根本没装/没配过 Claude Code，中止会把人挡死（2026-09-27 主会话裁定） |
| G8 | **文件存在但读不了 / 解析失败** | 读取失败（IO）/ 权限错 / 解析失败（非法 JSON）/ 顶层非对象 / `env` 非对象（**空文件不属于此类——视为不存在，见 G9**，2026-09-27 主会话裁定） | **中止本次 profile 切换**（S-2，profile 一字未改；同样不写也不清）——**与 G7“不存在”是两种不同处置，不得合并**（2026-09-27 主会话裁定） |
| G9 | 存在但无 env 真相（无 `env` 对象；**或为空文件——视为不存在**） | 解析成功、无 `env` 键；**或文件为 0 字节空文件**（2026-09-27 主会话裁定） | 整次 no-op 成功（不是失败；**不写 profile、不清空任何 profile 字段**，照常完成本次切换） |

**G2 联动跳过 token 是本设计的关键补充**（读代码 + 当前环境得出）：本机 cc-switch(15721) 自己就是本地代理，settings.json 里 `BASE_URL=http://127.0.0.1:15721` + 一个**非占位符**的本地 token；若只守 BASE_URL 不守 token，会把 cc-switch 的本地 token 当成用户的上游 key 存进 Keychain。

**（2026-09-27 用户拍板修订）清空语义下的守卫动作**：凡命中守卫而**跳过**的键（G1–G4、G6，含 G2 联动跳过的 token）一律**不写 profile、也不清 profile**——保持 profile 原值。理由：本机 cc-switch 也在托管 `~/.claude/settings.json`（写的是 `127.0.0.1:15721` + `PROXY_MANAGED`）；若守卫键参与「缺席即清空」，切一次 profile 就会把 profile 的 `base_url`/key 清掉。清空的作用范围见 §3.4。

### 3.4 出现/缺席语义（**缺席即清空**，2026-09-27 用户拍板修订）

- **settings.json 是唯一真相**：托管字段在 settings.json 中**出现** → 用其值覆盖 profile；在 settings.json 中**缺席** → **清空** profile 对应值。
- **清空只作用于**：能读到、且**非守卫**的托管字段（`base_url` / key / 6 个模型槽位 / 4 个 `_NAME` / `extra_env` 的键——键集合定义见下方专属细则）在 settings.json 中缺席时。
- **守卫键不写也不清**：命中 G1–G4/G6 而跳过的键（含 G2 联动跳过的 token；sidecar 托管键即便缺席也不清——接管期间其真值在 profile 侧）保持 profile 原值，既不写也不清（理由见 §3.3 表下注）。
- **可读性前置 / 失败路径不清空**（2026-09-27 主会话裁定，两种处置分别列出）：settings.json **不存在 / 为空文件（视为不存在）** → 整次 no-op 成功（G7 / G9）、不做任何清空、**照常完成本次切换**；settings.json **存在但读取失败 / 权限错 / 解析失败**（**空文件不在中止侧**） → **中止切换**（G8，S-2）、同样不做任何清空（profile 一字未改，见 §3.5）。
- `models_config` / `extra_env` 的重建：**以 settings.json 为准对齐**——出现过的槽位/键取 settings.json 值（仍以 `effectiveModelsConfig(现值, 旧 model 列)` 为基底落位），缺席的非守卫槽位/键清空；只有真的变了才落库（与 §3.5 零写入一致）。
- **`extra_env` 清空边界**（2026-09-27 主会话裁定，补原文留白细则）：`profile.extra_env` 的键集合**完全由 settings.json 决定**——即“settings.json env 中除去托管字段（`BASE_URL` / token / 6 槽位 model / 4 个 `_NAME`）后的全部键”，经既有别名表（`applyEnvAliases:203`）归一；某键在 settings.json 中缺席即从 `extra_env` 删除。守卫键（loopback `BASE_URL`、`PROXY_MANAGED`、空值）仍按“不写也不清”。
- key 的清空 = **用现成 `platform::keychain::deleteSecret`**（`platform/keychain.h:22`；用法同 `deleteProfile` 清理段 `claude_service.cpp:519-527`：`deleteSecret(keychainService_, profile.keychainAccount)`；测试侧同款 `platform::keychain::deleteSecret(kTestKeychainService, "claude:<id>")`）删除 Keychain 条目 `claude:<id>`，并清 `keychain_account` / `apiKeyHint` / `apiKeyCheck`；现有 service 无“单点删除 key”入口（写 key 唯一入口是 `updateProfile` 的 `apiKeyPlain` 非空分支），实现需新增最小清除路径（`updateProfile` 的空 `apiKeyPlain` 分支不触碰上述三列，故清除走新的最小 UPDATE）；**keychain 层不新增 API**（2026-09-27 主会话裁定）。
- 代价（已接受，见 §8.1/§8.7）：在 settings.json 中删键 = 在 profile 中清值（“唯一真相”的直接后果）；由其它工具（如 cc-switch）写入的模型名/额外 env 会在切换时被吸收进 profile（守卫键除外）。

### 3.5 失败与一致性（S-2 的落点）

| 失败点 | 处理 |
|--------|------|
| settings.json **不存在 / 为空文件（视为不存在）** | 非失败：no-op 成功，继续切换（G7 / G9）；**不写 profile、不做任何清空**（清空仅在文件可读且解析成功时发生，见 §3.4）——不得中止切换（**空文件同此处置**，2026-09-27 主会话裁定） |
| settings.json **存在但读取失败 / 权限错 / 解析失败**（顶层非对象 / `env` 非对象；**空文件除外**——视为不存在，见上行，2026-09-27 主会话裁定） | **fail 中止切换**（S-2，2026-09-27 主会话裁定）——profile 一字未改、不发生清空。错误文案含文件路径 + 原因，经既有 `doSetActive` 的 `showToast("启用失败", r.errorMessage(), true)`（`claude_module.cpp:2301-2304`）直接呈现，**UI 零改动** |
| sidecar 损坏 | **不中止**：`managedKeys` 按空处理 + WARN（G2/G3 仍挡代理形态值） |
| 无变化 | 零写入（不 UPDATE、不写 Keychain、不 bump `updated_at`）——先做逐字段比较（含缺席清空带来的差异），只有真的变了才落库 |
| Keychain 写失败 | `updateProfile` 在**写 DB 之前**返回 fail（`claude_service.cpp:436-441`）→ 中止切换，DB 完全未动 |
| DB UPDATE 失败（Keychain 已写） | 补偿：用 `loadApiKey()` 事先取到的旧值回写一次 Keychain；补偿失败 → `LOG_ERROR` + 中止切换，文案注明"该 Profile 的密钥可能处于不可用态，重新填一次 API Key 即可恢复"（元数据与其余字段完好） |

**两段写的最小可行结论**：不做真两段提交（把 key 存进 SQLite 会破坏"明文永不入 SQLite"的安全设计，spec §6.2.2）。只是把顺序固定为 **Keychain → DB、失败即中止**，并在 DB 失败时做一次 best-effort 回写。Windows 侧 `storeSecret` 是 trunc 后写（`keychain_win.cpp:84`），**不承诺**写失败时旧值必然完好——该风险如实登记（§8），不假装原子。

### 3.6 与 sidecar / 接管开关的关系

| 场景 | 反向同步行为 |
|------|--------------|
| 接管**开**、代理运行中切 profile | settings.json 里是托管值：G1 跳过全部托管键 → 实际只捕获用户自加的 env 键；随后步骤 4/5 正常切换 + `syncTakeOver` 写新 profile 值。**不会自我污染** |
| 接管**关**（当前默认，编排 #8 待恢复） | settings.json 是 cc-switch / 用户的真实配置 → 正常捕获（G2/G3 挡本地代理形态——守卫键不写也不清；缺席的非守卫字段按 §3.4 清空）。**本需求在接管关闭状态下同样有价值** |
| sidecar 残留但已不还原（接管被关掉后的遗留） | G1 跳过托管键 + G2/G3 兜底，不会把死指向（`127.0.0.1:8888`）收进 profile |
| 反向同步**不**受 `settingsTakeOverEnabled_` 门控 | 它只读不写外部文件；唯一开关性守卫是"是否注入了 `syncPaths_`"（`settingsFile.empty()` → 直接 return，与 `syncTakeOver:1076` 同款） |

### 3.7 文件改动（R1）

| file:line | 动作 |
|-----------|------|
| `src/module_claude_proxy/settings_sync.h:60`（`SyncResult` 之后） | 新增 `struct SettingsEnvRead { bool ok=true; bool present=false; std::string error; std::string envJson; std::vector<std::string> managedKeys; };` + `SettingsEnvRead readSettingsEnvForSync(const Paths&);` 声明 |
| `src/module_claude_proxy/settings_sync.cpp:386`（文件末尾） | 新增 `readSettingsEnvForSync` 实现：复用本文件既有 `readTextFile:46` / `parseJsonObjectFile:107` / `managedKeysFromSidecar:144`，**不新增 IO 助手**；env 对象用 `platform::json::stringify` 取原文 |
| `src/module_claude_proxy/provider_config.h:118`（`buildProxyEnv` 之后） | 新增 `bool isLoopbackUrl(const std::string&);`、`struct ReverseSyncParse { bool ok=false; std::string error; ImportedConfig cfg; std::vector<std::string> skippedKeys; };` + `ReverseSyncParse parseEnvForReverseSync(const std::string& envJson, const std::vector<std::string>& managedKeys);`、`struct ReverseSyncMerge { std::string modelsConfigJson; std::string extraEnvJson; bool modelsChanged=false; bool extraEnvChanged=false; };` + `ReverseSyncMerge mergeReverseSyncEnv(const ImportedConfig&, const std::string& curModelsConfigJson, const std::string& curLegacyModel, const std::string& curExtraEnvJson);`（入参出参全用 `std::string` + 既有 `ImportedConfig`，保持本 header 不依赖 `Profile` / `JsonObject` 的既有约定） |
| `src/module_claude_proxy/provider_config.cpp:388`（文件末尾） | 三个纯函数实现，映射表复用 `kImportSlots:82` + `applyEnvAliases:203` |
| `claude_service.h:272`（`syncTakeOver/syncRestore` 声明旁） | 新增私有 `core::Result<void> syncCaptureActiveProfile();` |
| `claude_service.cpp:630-633`（`setActiveProfile` 的 `getProfile` 校验之后） | 插入 G0 判断 + `syncCaptureActiveProfile()`，失败 `return fail`（S-2；**仅指“存在但读不了/解析失败”——文件不存在是 no-op 成功，见 §3.3 G7/G8**，2026-09-27 主会话裁定） |
| `claude_service.cpp:1073`（`syncTakeOver` 之前） | 新增 `syncCaptureActiveProfile` 实现：读 → 纯函数 → 差异比较（`loadApiKey` 取现值）→ `updateProfile` |
| `claude_service.cpp`（`syncCaptureActiveProfile` 实现内，清空 key 分支） | key 缺席清空（§3.4）**直接调用现成 `platform::keychain::deleteSecret(keychainService_, profile.keychainAccount)`**（`platform/keychain.h:22`；`deleteProfile` 清理段 `claude_service.cpp:519-527` 同款；测试侧同款 `tests/test_modules/test_claude_service.cpp:69-72`）删除 `claude:<id>` 条目，再清 `keychain_account` / `api_key_hint` / `api_key_check` 三列；**不新增 keychain 层 API**（2026-09-27 主会话裁定） |
| `claude_module.cpp:2296-2307` | **不动**（失败已由既有 toast 呈现） |

## 4. R2：Profile 复制

### 4.1 入口：详情页操作按钮组第 5 键「复制」

| 候选 | 结论 |
|------|------|
| 详情页按钮组（`claude_module.cpp:732-824`） | ✅ 与既有「启用 / 测试连通 / 编辑 / 删除」同款同区，零新交互基建；`actW` 由 `(w0-36)/4` 改 `(w0-48)/5`（`w0 = screen.width-80-400-16-56`：最小窗 1200 → actW=120px、默认窗 1600 → 200px，容纳 4 字按钮无虞，见 `claude_module.cpp:134-141`） |
| profile 卡片右键菜单 | ❌ 本模块零 `contextMenu` 使用（全仓仅 todo/markdown 有），为单个动作引入一套新交互不划算；卡片仍需先选中才能编辑/删除，语义与按钮组一致 |

按钮 id `claude.act.dup`（点击节点 `.bg`），源 = 当前详情面板显示的 profile（`shown`，与测试连通/编辑同款取法 `:738-739`）。

### 4.2 步骤与命名（`ClaudeService::duplicateProfile(int64_t srcId)`）

1. `getProfile(srcId)` → 失败即返回（不建行）
2. `listProfiles()` 取现有名称集合 → `makeCopyName(src.name, names)`（纯函数，可单测）
3. `loadApiKey(srcId)` → 失败即返回（**不建无 key 的副本**，与 `createProfile` 的"apiKey 必填"约束一致）
4. `createProfile(copyName, src.baseUrl, key, effectiveModelsConfig(src).def.model, /*activateIfNoneActive=*/false)` → 一行 SQLite + 一条 Keychain（account 自动为 `claude:<新 id>`，与源条目**不同**）
5. 源 `modelsConfigJson` / `extraEnvJson` 非空 → 取回新行、填两字段、`updateProfile`（与 UI 新建流程 `claude_module.cpp:1421-1439` 同款二段补写）
6. **不激活**（S-4）；`apiKeyHint` / `apiKeyCheck` 由 `createProfile` 按新 key 自行派生

命名规则：`candidate(0) = base + " 副本"`；重名则 `base + " 副本 " + N`（N 从 2 递增，与既有 name UNIQUE 约束避让）；上限 100 次后返回明确错误（避免病态死循环）。后缀用空格分隔（与既有 `suggestImportName` 的「导入 X」同风格）。

### 4.3 失败处理

| 失败点 | 处理 |
|--------|------|
| Keychain 写失败 | `createProfile` **已内建**：DELETE 掉刚插的行后返回 fail（`claude_service.cpp:308-328`）→ 复制侧零残留、零额外代码 |
| 二段 `updateProfile` 失败（如源 JSON 在库里已损坏，legacy 行） | best-effort `deleteProfile(newId)` 回滚（它先清 Keychain 再删行 `:519-539`）；回滚成功 → 返回 fail（无残留）；回滚失败 → 返回 fail + 文案"已创建不完整副本 id=N，请手动删除" |
| 名称避让 100 次仍冲突 | 返回 fail，不建行 |
| 源 key 缺失（Keychain 孤儿） | 返回 fail「源 Profile 的密钥缺失，无法复制」 |

副本不激活 ⇒ 不触发 `setActiveProfile` ⇒ **不触发 R1 反向同步**（两需求零耦合）。

### 4.4 文件改动（R2）

| file:line | 动作 |
|-----------|------|
| `provider_config.h`（同 §3.7 新增段） | 新增 `std::string makeCopyName(const std::string& baseName, const std::vector<std::string>& existingNames);` |
| `claude_service.h:116`（`setActiveProfile` 之后） | 新增 `core::Result<Profile> duplicateProfile(int64_t srcId);` 声明 |
| `claude_service.cpp:668`（`setActiveProfile` 实现之后） | 新增 `duplicateProfile` 实现（复用 `getProfile:185` / `listProfiles:155` / `loadApiKey:584` / `createProfile:214` / `updateProfile:386` / `deleteProfile:504`） |
| `claude_module.cpp:731` | `actW = (w0 - actGap * 3.0f) / 4.0f` → `actW = (w0 - actGap * 4.0f) / 5.0f` |
| `claude_module.cpp:822`（删除键 `.build()` 之后、row 的 `.content(...)` 内） | 新增第 5 键「复制」`claude.act.dup`（`shown.id == 0` → toast「请先选择 Profile」，与同区其他键同款） |
| `claude_module.cpp:2307`（`doSetActive` 之后） | 新增 `doDuplicate(int64_t)`：标脏 `dataDirty_` → `duplicateProfile` → 失败 toast / 成功 toast「已创建副本「X 副本」（未激活）」 |

## 5. 测试策略

| 层 | 用例（全部离线，不碰真实 `~/.claude`） |
|----|--------------------------------------|
| 纯函数（`test_provider_config.cpp` / `test_settings_sync.cpp` 扩展） | `isLoopbackUrl`：`http://127.0.0.1:8888` ✅、`http://127.5.5.5` ✅（127/8）、`LocalHost` ✅（大小写）、`[::1]:8888` ✅、`https://api.anthropic.com` ❌、`http://127.0.0.1.evil.com` ❌（不做前缀误判）、空 ❌ · `parseEnvForReverseSync`：映射表 14 条逐条、缺席产出「清空」标记（守卫键除外）、G1 托管键跳过、G2 loopback + token 联动、G3 占位符、G4 空值、G6 非字符串跳过不 fail、别名归一（`AUTO_COMPACT_ENABLED`→`DISABLE_AUTO_COMPACT` 取反）、`ANTHROPIC_API_KEY`/`AUTH_TOKEN` 同现取后者 · `mergeReverseSyncEnv`：**缺席非守卫槽位/键确实清空**（关键用例，2026-09-27 用户拍板修订）、**守卫跳过 → 不写也不清**、出现槽位覆盖、extra_env 出现覆盖 + 缺席键从 extra_env 删除（键集合完全由 settings.json 决定——2026-09-27 主会话裁定）、无变化 → changed=false · `makeCopyName`：无冲突 / 「副本 2」/「副本 3」/ 空名 |
| service（`test_claude_service.cpp` 扩展，`TestDb` + `kTestKeychainService` + 临时 `settings_sync::Paths` 注入，仿 `test_settings_sync.cpp` 的 `TempDir`） | ① 切 profile 前捕获：临时 settings.json 写真实上游值 → `setActiveProfile(b)` → 断言 a 的 `base_url` / `loadApiKey(a)` / 槽位 / extra_env 全部等于 settings.json 值 ② **守卫键不写也不清**（2026-09-27 用户拍板修订）：loopback BASE_URL（G2 联动 token）/ `PROXY_MANAGED` / 空值三形态各自 → 断言 `base_url` 与 key **保持原值** ③ **缺席即清空**：合法 settings.json 缺某些非守卫槽位/extra 键 → 断言 a 对应值被清空（关键用例） ④ S-2：坏 JSON → `setActiveProfile` fail 且 `is_active` 仍是 a、a 字段一字未改（未发生清空） ⑤ 文件**不存在 / 空文件** → no-op 成功（G7 / G9，**空文件视为不存在**：不写、不清、照常切换）且 a 一字未改（未发生清空）；**读取失败 / 坏 JSON** → fail 中止（S-2）且 a 一字未改（2026-09-27 主会话裁定） ⑥ 无激活 → 坏 JSON 下 `createProfile(activateIfNoneActive=true)` 仍成功（G0） ⑦ 无变化 → `updated_at` 不变（零写入） ⑧ sidecar 造 `managedKeys` 含 `ANTHROPIC_MODEL` → 该槽位未被覆盖（G1，不写也不清） ⑨ 回归：代理运行中切 profile 仍写 settings.json（既有用例） · 复制：字段/Keychain 全等同源、id 与 account 不同、`is_active` 未变、源行未动、重名避让、源 key 缺失 → fail 且无新行、二段失败回滚（用直接 SQL 写坏 `models_config` 造 legacy 脏行） |
| 无头 UI（`test_claude_ui.cpp` 扩展，照既有 `click(ui, id+".bg")` 模式 `:324-331`） | `compose` 后 `ui.find("claude.act.dup")` 非空；无服务时不崩；点 `claude.act.dup.bg` → profile 数 +1、选中态/激活态未变；点 `claude.profile.0.bg` 选中后点启用 → 断言 settings.json 的值已进原 profile |
| 人工点验（用户侧） | 复制一个含 6 槽位 + env 的 profile → 改 key 后副本独立；在 Claude Code 侧改 settings.json 后切 profile → 切回时改动仍在（出现键）；在 settings.json 删掉一个非守卫键后切 profile → 对应 profile 值被清空；本地代理形态（`127.0.0.1:15721` + `PROXY_MANAGED`）下切 profile → profile 的 `base_url`/key 未被清空 |

## 6. 拆任务（每项 ≤1 天）

| 里程碑 | 内容 | 依赖 | 验收 |
|--------|------|------|------|
| **M1** 读侧（≈0.5d） | `settings_sync` 新增 `readSettingsEnvForSync` + 单测 | — | 6 种文件形态（缺失/空/坏 JSON/无 env/env 非对象/正常）——“缺失/空文件”（no-op；**空文件视为不存在**——2026-09-27 主会话裁定）与“存在但读不了/解析失败”（中止）必须分别处置、结果可区分 + sidecar 损坏不阻断 |
| **M2** 纯函数（≈0.5d） | `isLoopbackUrl` / `parseEnvForReverseSync` / `mergeReverseSyncEnv` / `makeCopyName` + 单测 | — | §5 纯函数用例全绿（守卫表逐条有对应用例） |
| **M3** 反向同步接线（≈0.5d） | `syncCaptureActiveProfile` + `setActiveProfile` 插入点 + 一致性补偿 + service 用例 | M1, M2 | §5 service ①-⑨ 全绿；既有 514 用例零回归 |
| **M4** 复制（≈0.5d） | `duplicateProfile` + 命名避让 + 失败回滚 + 用例 | M2（`makeCopyName`） | §5 复制用例全绿 |
| **M5** UI + 收尾（≈0.5d） | 第 5 按钮 + `doDuplicate` + 无头 UI 用例 + 截屏 + CHANGELOG/spec 索引登记 | M3, M4 | 按钮可见可点、toast 正确、截屏留档 |

合计 **≈2.5 开发日**（AI 开发口径，含测试与截屏）。M1/M2 可并行；建议按 M1+M2 → M3 → M4 → M5 顺序串行落地（build 是稀缺资源，见编排表规则 1）。

## 7. 不做什么

1. **不做 settings.json 变更监听/轮询**：不引入 watch 线程、不做"改完即同步"；只在切 profile 这一个时点收编。
2. **不做定时/自动同步**，也不在 profile 新建/编辑/删除时同步（唯一例外是 `createProfile` 内部走 `setActiveProfile` 那一条）。
3. **不把 settings.json 的非 env 字段**（`permissions` / `hooks` / `model` / `statusLine` 等）纳入 profile 数据模型。
4. **不读 project / local / managed 层**做反向同步（只读 user 层 `Paths.settingsFile`；`overlayFiles` 仍只服务既有接管告警）。
5. **不做反向同步的独立开关**（读路径无外部副作用，写目标是自己的 profile）；也不让它受 `settingsTakeOverEnabled_` 门控（理由见 §3.6）。
6. **不做复制时的 key 重命名/脱敏策略**、不做副本自动激活、不做批量复制。
7. **不改** `parseImportedEnv` / `buildProxyEnv` / 接管写入协议的任何既有行为（只新增函数）。

## 8. 已接受的边界（如实登记）

1. 用户在 settings.json 里**删除**某键（非守卫）→ profile 对应值被**清空**（缺席即清空，2026-09-27 用户拍板修订；守卫键不适用——不写也不清），下次接管不会写回 settings.json。
2. 接管期间用户手工改**托管键**的值 → 被 G1 跳过且下次 `takeOver` 覆盖（沿用 provider-config §5.3 已接受的"managed 键语义归 DevDesk"）。
3. 别名归一后若 profile 既有 `AUTO_COMPACT_ENABLED` 又新收 `DISABLE_AUTO_COMPACT`，两键并存（不做旧别名键清理）。
4. 坏 settings.json 会**锁住 profile 切换**（S-2 的直接后果）：文案给路径与原因，用户需先修文件（失败/不可读路径下 profile 一字未改，不发生清空）。**锁定仅针对“存在但读不了/解析失败”**；settings.json **不存在** → no-op 成功、照常切换、不锁（2026-09-27 主会话裁定）。
5. Keychain 写入非原子（Windows trunc 后写，`keychain_win.cpp:84-99`）：DB 失败后的补偿回写失败时，profile 的 key 可能不可用（重填一次 API Key 即恢复）。
6. 反向同步是"切 profile 时收编"，不是实时镜像：切换前的多次改动以**最后一次落盘状态**为准（中间态不留痕）。

7. **采纳「缺席即清空」的必然效应**（2026-09-27 用户拍板修订，用户已接受）：接管关闭期间切 profile，会把 settings.json 里**由其它工具（如 cc-switch）写入**的模型名与额外 env **吸收进 profile**（这是“以 settings.json 为准”的必然结果）；**守卫键除外**——不会被吸收，也不会被清空（`base_url`/key 在本地代理形态下保持 profile 原值，见 §3.3）。
8. **清空语义的回归测试必须覆盖**（2026-09-27 用户拍板修订，硬性）：① 守卫键（loopback BASE_URL / G2 联动 token / `PROXY_MANAGED` / 空值）存在时**不写也不清**；② 非守卫托管键在 settings.json 缺席时**确实清空**；③ settings.json **不存在 / 为空文件**（**空文件视为不存在**，2026-09-27 主会话裁定） → no-op 成功（G7 / G9）；**存在但读不了/解析失败（空文件除外）** → 中止切换（S-2）——两种处置下 **profile 一字未改、不发生任何清空**（2026-09-27 主会话裁定）。

## 9. 原开放问题（2026-09-27 用户拍板，全部关闭）

1. **缺席语义**：settings.json 里**没有**的键，profile 该保留原值还是清空？
   - (A) 保留：**已否决**（2026-09-27 用户拍板修订）。
   - (B) **清空：拍板采用**——settings.json 成为唯一真相；**守卫键不写也不清**（本地代理形态值 / 空值 / sidecar 托管键，见 §3.3/§3.4）。代价与硬性回归要求见 §8.1/§8.7/§8.8。
2. **同步范围**：只读用户级 `~/.claude/settings.json`（本 spec 采用），还是也读 project 级 `.claude/settings.json`？
   - (A) **只读 user 层：拍板确认（2026-09-27）**——维持本 spec 结论，project 层不读（确需时另开需求）。
3. **R2 复制入口**：详情页操作按钮组第 5 键「复制」——**拍板确认（2026-09-27）**，维持 §4.1（右键菜单方案已否决）。
