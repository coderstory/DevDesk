# Profile 高级设置 JSON（非 env 属性）+ 生成链合并 + 共享配置片段重构——设计

日期：2026-09-30 ｜ 状态：**已实施（2026-09-30，merge a1451e9；设计已过独立审计——10 findings 全处置，见 §12）** ｜ 来源：用户需求「高级设置里的 json 是除 env 属性之外的全部属性之合」+「支持 json 自动格式化」+ 共享配置片段重构追加 ｜ 关联：`2026-09-30-batch-fix-design.md`（项 B 建立的表单「高级配置」tab 与详情 pane，本文在其上加 JSON 编辑框区块）、`2026-09-27-shared-config-and-failover-design.md`（片段绑定既有拍板，本文 §8 迁移其语义）、`2026-09-28-profile-run-design.md`（runProfile 临时 settings 生成链）、审计留痕 `plans/2026-09-30-advanced-json-audit.json` ｜ **依赖：master `0f1f734`**

> 用户已拍板三项：① 模型类键冲突 = **JSON 赢**（显式手写覆盖 UI 槽位）；② 「复制配置」导出从 `{"env":{...}}` 升级为**完整 settings.json 形态**；③ 停代理还原的 managedKeys 记账**扩到非 env 属性**（migration v11，无迁移包袱口径直接扩）。
> UI tab 结构一概不动（编辑弹窗四 tab、详情五 pane），本文只新增：高级配置 tab 在现有方言/备用下拉之下加 JSON 编辑框区块。

## 0. 总表

| 项 | 内容 | 类型 | 依赖与边界 |
|----|------|------|-----------|
| **A** | 数据层：`Profile::extraSettingsJson` + `SharedConfig::extraSettingsJson` 字段 + migration v11 + Repository 读写 | 数据层 | v9/v10 迁移先例；SQLite ≥3.35 已是既有要求（RETURNING），DROP COLUMN 可用 |
| **B** | 表单「高级配置」tab JSON 编辑框：预填 / 可编辑 / 应用校验（红字不落库）/ 「格式化」按钮 / 落库统一 pretty-print | UI | platform::json **已有 pretty-print**（§2.1），**不自写格式化器**；复用 cc-switch 粘贴导入框的多行 input 先例（§2.3） |
| **C** | 「复制配置」导出升级全量 settings.json 形态 + toast 文案同步 | UI 层纯函数 | `buildProfileExportJson` 扩展（batch-fix §4.2② 同一函数） |
| **D** | 生成链合并 + 接管/还原扩展：takeOver / runProfile / export 三条 settings.json 生成链统一并入口；sidecar 记账扩 `nonEnvKeys`/`nonEnvSnapshot`；模型键 JSON 赢抑制 | 服务层 | `settings_sync.cpp` takeOver/restore 既有结构内扩展；`syncTakeOver` 签名随片段重构同步（§8.4） |
| **E** | 共享配置片段重构：片段弹窗 JSON 化 + 绑定语义迁移（生成时叠加、片段赢）+ 旧 env 片段数据弃用 | 数据 + UI | 独立节 §8；绑定 UI 落位（高级配置 tab）审计未持异议；拆除清单已按 F-2/F-6 补全（§12③ 真拆） |

---

## 1. 需求与决策

| 决策 | 内容 | 理由 |
|------|------|------|
| **D-1** | Profile 新增 `extraSettingsJson`：settings.json **除 env 外全部顶层属性**（model / permissions / statusLine / hooks / …）整体作为一份原始 JSON 存储，空串 = 未配置 | 用户口径「除 env 属性之外的全部属性之合」；一份 JSON 原文，不做字段拆解 |
| **D-2** | 格式化 = `platform::json::parse` + `stringify(value, /*pretty=*/true)`，**不自写格式化器** | pretty-print 已实装（yyjson `YYJSON_WRITE_PRETTY_TWO_SPACES`，`src/platform/json.cpp:329`；声明 `json.h:98`），两空格缩进，与 settings_sync 写 settings.json 的序列化通道完全同一条 |
| **D-3** | 合并语义（生成链，见 §4）：最终 settings.json = env（现有 buildProxyEnv / buildDirectEnv 链）∪ 生效 JSON（非 env 属性）。**JSON 的键赢过 live settings.json 已有的同名非 env 属性**（profile 赢） | profile 的 JSON 是接管时点的显式手写配置；被覆盖的 live 旧值无损失——首份快照（sidecar）已记下、停代理还原时恢复（§5）。与 env 链「托管键覆盖既有 env 值」同一方向 |
| **D-4** | 模型类键冲突 = **JSON 赢**（已拍板①）：生效 JSON 含顶层 `"model"`（非空字符串）时，本轮接管**不生成 `ANTHROPIC_MODEL` env 键**（def 槽位 / extra_env / 别名归一来的同名键一并抑制），槽位 def 值失效 | settings.json 的 `model` 是顶层属性、env 的 `ANTHROPIC_MODEL` 是另一条通道，二者在 Claude Code 侧互为竞争面；「显式手写覆盖 UI 槽位」在我们的写侧落地 = 不写会压过它的托管键。其余槽位键（SONNET/OPUS/…）无顶层对映键，照常写入。抑制发生在合并纯函数一处（§4.3），三条生成链同时生效 |
| **D-5** | JSON 里出现顶层 `env` 键：**UI 保存时拒绝**（红字）；合并读路径再兜底**跳过 + WARN**（防未来写入方绕过 UI） | fail-fast 优于静默丢弃：用户以为配了 env 实际被忽略是排雷困难型缺陷；但 DB 直写等非 UI 路径仍需读侧容错 |
| **D-6** | 非法 JSON 应用：红字（`formError_` 语义）+ **不落库**；落库值统一 pretty-print | 用户口径「解析失败红字不落库」「应用落库统一 pretty-print」 |
| **D-7** | 超大 JSON：整体文本 ≤ `kMaxExtraSettingsBytes = 1MB`（新常量，落 `provider_config.h`，紧邻既有 `kMaxImportTextBytes = 1MB`，`provider_config.h:188`） | 同一理由（`provider_config.h:184-188` 注释）：platform::json 按值 = 整树深拷贝（`json.h:18-23`），超大输入挡在解析之前。settings.json 实际合法载荷（hooks/permissions）远小于 1MB |
| **D-8** | 「复制配置」导出 = 完整 settings.json 形态（env + 非 env 属性；已拍板②）；toast 文案同步 | 用户口径 |
| **D-9** | 停代理还原记账扩到非 env 属性（已拍板③）：sidecar 增 `nonEnvKeys` + `nonEnvSnapshot`，还原时按键级撤走/恢复；migration v11 | 与 env 托管键同一套记账闭环（§5）；无迁移包袱口径直接扩，旧 sidecar 缺新字段 = 空集（读侧 `.has()` 守卫，旧接管从未写过非 env 属性，语义安全） |
| **D-10** | 共享配置片段重构（§8）：片段内容改为非 env 属性 JSON，绑定语义 = **生成时叠加、片段赢**（解绑回落 profile 自身值）；旧片段 env 数据**弃用不转换** | 片段即预设模板（协调者建议采纳）；「片段值从不写进 profile 行」是 09-27 片段设计的核心拍板（`profile_repository.h:190-196`），JSON 域沿用同构语义；不转换的理由见 §8.3 |
| **D-11** | 环境变量 tab 的 cc-switch 粘贴导入与「导入当前配置」弹窗**不动** | 那是 env 域的，独立价值（用户口径） |

---

## 2. 关键既有能力盘点（本文每条断言的出处）

### 2.1 platform::json 已有 pretty-print，无需自写格式化器

- `core::Result<std::string> stringify(const JsonValue& value, bool pretty = false)`（`src/platform/json.h:98`）；`pretty=true` 追加 `YYJSON_WRITE_PRETTY_TWO_SPACES`（`src/platform/json.cpp:329`），两空格缩进。settings.json 的既有写入（`settings_sync.cpp:410, :495`）与 sidecar 序列化已走这条通道。
- **键序行为（必须在文档与测试里写明）**：`JsonImpl::objVal` 是 `std::map<std::string, JsonImpl>`（`json.cpp:23`），对象键按**字典序**输出，不保持输入文档顺序。格式化是「解析→重序列化」的规范化：解析结果语义不变（round-trip 等值），字节序会重排。这与 settings_sync 既有写入行为一致（settings.json 本就每轮被按字典序重排），Claude Code 不依赖键序，可接受；§10 格式化器断言按「2 空格缩进 + 字典序 + round-trip 等值 + 幂等」写。
- 解析：`parse`（`json.h:95`）→ 失败返回 `ErrorCode::JsonParseError`。yyjson 严格模式，非法 JSON 必失败，满足应用校验需要。
- 深拷贝代价与 1MB 上限先例：`json.h:18-23` + `kMaxImportTextBytes`（`provider_config.h:184-188`）。

### 2.2 接管/还原链现状（扩非 env 属性的落点）

- `settings_sync::takeOver(profile, proxyPort, paths, sharedExtraEnvJson)`（`settings_sync.h:99-100`，实现 `settings_sync.cpp:294-433`）：① 读 settings.json（解析失败中止不写）；② 首份快照 env → sidecar（`{"managedKeys":{<键>:""}, "envSnapshot":{…}}`，`settings_sync.cpp:131-134`，已存在则保留首份并集 managedKeys）；③ 删「上一轮托管、本轮不再生成」的陈旧托管键（`:386-401`）；④ 合并 env，「**非 env 顶层字段不动**」（`:403-407` 注释即此边界——本文正是要改这一行）；⑤ pretty 原子写；⑥ 上层覆盖检测。
- `settings_sync::restore(paths)`（`:435-516`）：按键级还原——managedKeys 内的键从快照恢复 / 原本不存在则删除，其余不动；写回后删 sidecar。**当前只作用于 env 对象**；非 env 属性零触碰。
- 门面接线：`ClaudeService::syncTakeOver`（`claude_service.cpp:755-769`）→ `SettingsTakeOver::syncTakeOver(activeProfile, proxyPort, sharedExtraEnvJson)`（`settings_takeover.h:64-65`）。profile 对象整体传入——**`extraSettingsJson` 落在 Profile 结构上后，接管链零新增签名即可读到它**（§8.4 的参数变化只来自片段重构）。
- env 生成：`buildProxyEnv(modelsConfigJson, legacyModel, extraEnvJson, proxyPort, sharedExtraEnvJson="", sourcesOut=nullptr)`（`provider_config.h` §4.3 注释块规则 1-6）——托管键（BASE_URL / 占位 token / 6 槽位 envKey+nameKey）压过 extra_env；`EnvSource{Profile, Shared, Managed}` 枚举只为来源标注（`claude_ui_detail.cpp:57-64` `envSourceLabel`：片段/本 Profile/托管键）。
- 模型键集单一来源：`DEVDESK_MODEL_SLOT_LIST`（`provider_config.h:47-62`），def 槽位 envKey = `ANTHROPIC_MODEL`。**层次落点（审计 F-10 修正）**：既有 `isModelMappingEnvKey` 定义在 `claude_ui_detail.cpp:74`（UI 层），`provider_config` 不能反向 include UI 文件——本文新增的抑制判定落 `provider_config`（从 `kModelSlotMetas` 派生键名的新纯函数，见 §4.3），`claude_ui_detail.cpp:74` 改为复用/转发该函数，两处注释互指单一来源，不手抄键名。

### 2.3 UI 先例

- 多行 input：cc-switch 粘贴导入框 `components::input(ui, "claude.form.import.text")… .multiline(true).scrollbar(true)`（`claude_ui_form.cpp:910-916`，盒高 82）；EUI-NEO `components/input.h:59` `multiline(bool)`。
- 校验红字两层形态：tab 内即时红字（导入解析失败 `claude.form.import.error`，`claude_ui_form.cpp:958-976`，`t.colors.error`）+ 提交闸门 `formError_`（弹窗底部 y=h-104，`claude_ui_form.cpp:982-986` 注释与既有渲染；`formError_` 赋值点 `:243-374`）。
- 「高级配置」tab 现有内容（`drawFormAdvancedTab`，`claude_ui_form.cpp:503-631`）：方言下拉（label y104、ddwrap y122-162）、备用 Profile 下拉（label y174、ddwrap y192-232），**:631 函数即止**——JSON 区块插在 y≥244。弹窗 h=600，tab 内容底 ≈ h-104 = 496（formError 行位）。
- 片段绑定现状（要迁移的整套）：env tab 顶部共享配置下拉（`claude_ui_form.cpp:709-770`，`formSharedConfigId_`）+ 冻结键置灰/「来自片段 X」chip（`formFrozenKeys_`，`:280-290, :790`）；片段编辑弹窗 `drawSharedConfigDialog`（`:1222` 起，h=460，name + `envRows_` 键值编辑器 + 删除）。
- 来源标注先例（0edd3cf）：`EnvSource` 枚举 + `envSourceLabel`（§2.2）+ env pane 两带行卡（`drawDetailEnv`，`claude_ui_detail.cpp:267`）——JSON 赢场景的键来源展示沿用该形态（§7）。

### 2.4 迁移与仓库层先例

- 迁移形态：v9 `kClaudeProfileDialectOverride`（`migrations.h:262-278`）、v10 `kClaudeFailoverColumns`（`:280-291`）——`ALTER TABLE … ADD COLUMN … NOT NULL DEFAULT`，版本号顺延注释；`all()` 表（`:293-308`）尾插。`runMigrations` 每条迁移单事务内 `exec(m.sql)`（`sqlite_db.cpp:326-359`），多语句 SQL 已有先例（v1 建表）。
> 行号为设计时点快照（master `0f1f734`），实施以符号名为准。
- Profile 列加法四处同步点：`Profile` 结构（`profile_repository.h:27-55`）、SELECT 列清单（listProfiles / getProfile / getActiveProfile，`profile_repository.cpp:209, :240, :729` 附近）、`rowToProfile`（`:151-208`）、createProfile INSERT（`:305-330`）/ updateProfile 两分支 UPDATE（`:524-600`）/ duplicateProfile（`:789-867`，一建 + 二段补写 `:841-861` 经 `updateProfile`）。
- 服务层签名先例：`createProfile` 尾插默认参（`claude_service.h:75-89`，dialectOverride/fallbackProfileId 同款）。

---

## 3. 项 A：数据层——字段 + v11 迁移 + Repository

### 3.1 结构字段

- `Profile` 增 `std::string extraSettingsJson;`（`profile_repository.h`，紧邻 `extraEnvJson`）：settings.json 非 env 顶层属性 JSON 原文，空串 = 未配置。**仅存原文，不拆字段**。
- `SharedConfig` 的 `std::string extraEnvJson` 改名 `std::string extraSettingsJson`（§8，随 v11 列改名同批）。

### 3.2 migration v11（一条迁移、三条语句；`exec` 支持多语句，§2.4）

```
// version 11：claude profiles add extra settings json; shared configs
// rework to non-env settings json (old fragment env data discarded)
ALTER TABLE claude_profiles ADD COLUMN extra_settings_json TEXT NOT NULL DEFAULT '';
ALTER TABLE shared_configs ADD COLUMN extra_settings_json TEXT NOT NULL DEFAULT '';
ALTER TABLE shared_configs DROP COLUMN extra_env;
```

- `DROP COLUMN` 需 **SQLite ≥3.35**（3.35.0 才加入；3.25 是 RENAME COLUMN，不可混写）——与项目既有 `INSERT … RETURNING` 要求同一条下限（`profile_repository.cpp:304` 注释），两端平台 SQLite 均 ≥3.45。`extra_env` 列无索引/约束，可安全删除。旧片段 env 数据**弃用**（理由 §8.3）。
- 命名/注释照 v10 形态（版本号顺延说明 + 「ADD COLUMN 无 IF NOT EXISTS，靠 schema_version 保证只执行一次」）。

### 3.3 Repository 读写

- SELECT 三处列清单 + `rowToProfile` + `rowToSharedConfig` 增读 `extra_settings_json`（`row.getString` 容错形态照 `:167-172` 同款）。
- `createProfile` 尾插默认参 `const std::string& extraSettingsJson = std::string()`，INSERT 列 + bind 同步；服务层 `ClaudeService::createProfile` 尾插同名参（`claude_service.h:75-89`），UI 新建分支传值。
- `updateProfile` 两分支 UPDATE 增列（`Profile` 结构已携带，服务层签名零变化）；`duplicateProfile`（`profile_repository.cpp:789-867`）复制链带上 `extraSettingsJson`：createProfile 调用点（`:829-831` 尾参同款，与 dialect_override「复制即配置」口径一致）传入，且二段补写块（`:841-861`）的门槛条件扩为 `!src.modelsConfigJson.empty() || !src.extraEnvJson.empty() || !src.extraSettingsJson.empty()`——**审计 F-3 必修**：纯 JSON profile（两 env 字段均空、仅 extraSettingsJson 非空）复制不得因门槛不成立静默丢字段。
- `sharedExtraEnvJsonFor` 改名 `sharedExtraSettingsJsonFor`（返回片段的 `extra_settings_json`；消费点 §8.4）。

---

## 4. 项 D（核心）：生成链合并——顺序、冲突语义、纯函数落点

### 4.1 生成链的三个消费者（同一套纯函数，合并规则只此一处）

| 消费者 | 现状 | 本文 |
|---|---|---|
| 接管 takeOver → `~/.claude/settings.json` | 只写 env（§2.2④「非 env 顶层字段不动」） | env 链照旧 + 生效 JSON 并入顶层（§5 记账扩展） |
| runProfile → `run-settings/run-<id>-<ts>.json` 临时 settings（`claude_service.cpp:470-510`） | `buildDirectEnv` → `{"env":{…}}` | 同一合并（**已裁决纳入**，§12①：直连会话同样带 hooks/permissions，三链不分叉；临时文件随会话自删，无记账负担） |
| 复制配置 export → 剪贴板（§6） | `buildProfileExportJson` → `{"env":{…}}` | 升级完整形态（已拍板②） |

### 4.2 合并顺序与冲突语义（一句话版 + 展开版）

**一句话版**：`最终 settings.json = env 对象（现有链产出） ⊕ 生效 JSON（片段 JSON 赢过 profile JSON，二者赢过 live 文件既有非 env 属性；"env" 键两端都拒绝/跳过；"model" 键抑制 ANTHROPIC_MODEL）`。

展开版（按应用顺序）：

1. **片段叠加（仅存在绑定时）**：`生效 JSON = effectiveExtraSettingsJson(profile.extraSettingsJson, fragment.extraSettingsJson)`——逐键叠加，**片段赢**（解绑后同名键回落 profile 自身值；片段值从不写进 profile 行，D-10）。任一侧 JSON 非法/超限 → WARN + 按空处理，另一侧照用；两侧都空 → 空串。
2. **并入文档**：`applyExtraSettingsToSettingsDoc(root, 生效JSON)`——对生效 JSON 的每个顶层键 K：`K=="env"` → 跳过 + WARN（D-5 读侧兜底）；其余 `root[K] = 值`（**赢过 live settings.json 既有同名键**，D-3 理由：被覆盖旧值已在首份快照中，还原无损）。返回本轮写入的非 env 键集（§5 记账输入）。
3. **模型键抑制（已拍板①；机制描述按审计 F-5 修正）**：生效 JSON 含非空字符串顶层 `"model"` → `applyExtraSettingsToSettingsDoc` 在并入时同时从 `root.env` 中**移除 `ANTHROPIC_MODEL`**（不论它来自 def 槽位、extra_env 还是别名归一）；其余槽位键不动。时序：抑制发生在合并写入**之后**的 apply 步，**不是**陈旧删除步——陈旧删除（`settings_sync.cpp:386-401`）在合并之前运行、以 `buildProxyEnv` 产出为「本轮生成键」，`ANTHROPIC_MODEL` 仍在其中（`generatedThisRound=true`），不被该步处理。净结果一致：live 已移除 + 该键仍在 managedKeys 并集（buildProxyEnv 产出未变）→ restore 语义不变；若本轮 JSON 不含 `model`，则该键回到 buildProxyEnv 生成、由既有陈旧删除逻辑自然撤走。
4. env 对象内部优先级**完全不动**（托管键 > extra_env；片段 env 域随 §8.4 拆除）。

### 4.3 纯函数落点（`provider_config.{h,cpp}`，可无头测试）

```
// 生效 JSON：片段叠加在 profile JSON 之上（片段赢），"env" 跳过 + WARN，
// 非法/超限一侧按空处理；输出 pretty 规范形（空串 = 无）
std::string effectiveExtraSettingsJson(const std::string& profileJson,
                                       const std::string& fragmentJson,
                                       std::vector<std::string>* warningsOut = nullptr);

// 并入 settings 文档：root["env"] 必须已是对象；返回写入的非 env 键集 +
// 是否发生 model 抑制 + warnings。"env" 键跳过 + WARN；"model" 键触发
// ANTHROPIC_MODEL 抑制。键名判定不得手抄：provider_config 新增从
// kModelSlotMetas 派生 def 行 envKey 的纯函数（审计 F-10——既有
// isModelMappingEnvKey 在 claude_ui_detail.cpp:74 属 UI 层，不可反向
// include；该函数改为复用/转发本判定，两处注释互指单一来源）
struct ExtraSettingsApply {
    std::vector<std::string> writtenKeys;   // 本轮写入的非 env 顶层键
    bool modelSuppressed = false;           // ANTHROPIC_MODEL 被抑制
    std::vector<std::string> warnings;
};
ExtraSettingsApply applyExtraSettingsToSettingsDoc(platform::json::JsonObject& root,
                                                   const std::string& extraSettingsJson);

// 应用/保存校验（UI 与服务共用）：空串 OK；≤ kMaxExtraSettingsBytes（D-7）；
// 必须解析成功；顶层必须是对象；顶层不得含 "env" 键。失败带中文错误
core::Result<void> validateExtraSettingsJson(const std::string& text);
```

三个函数都是无 IO 纯函数，紧邻 `buildProxyEnv`（合并规则单一来源的项目口径，同 `provider_config.h` §4.3 注释「合并顺序写在 buildProxyEnv 里，一处」）。**不在此处做 pretty 打磨以外的一切**——格式化 = `parse` + `stringify(pretty)`（D-2）。

---

## 5. 项 D：接管/还原记账扩展（sidecar v2）

### 5.1 sidecar 形态扩展

```
{"managedKeys": {<env 键>: ""},            // 既有，不动
 "envSnapshot": {…},                        // 既有，不动
 "nonEnvKeys": {<本轮+历史写过的非 env 顶层键>: ""},
 "nonEnvSnapshot": {<首份快照拍下的原值>}}  // 键在接管前不存在 → 不在快照里 = 还原时删除
```

旧 sidecar（无新字段）→ 读侧 `.has()` 守卫按空集处理，无兼容分支（D-9）。

### 5.2 takeOver 扩展（`settings_sync.cpp`，在既有步骤内插入，不改顺序）

1. 门面把「生效 JSON」预先算好放进传入的 Profile 副本：`ClaudeService::syncTakeOver`（`claude_service.cpp:755-769`）改为 `active.extraSettingsJson = effectiveExtraSettingsJson(active.extraSettingsJson, sharedExtraSettingsJsonFor(active.sharedConfigId))` 后传副本——`settings_sync::takeOver` **零新增参数**，直接消费 `profile.extraSettingsJson`。
2. 解析 profile JSON（`parseJsonObjectFile` 同款容错：非法/超限 → WARN + 当作无 JSON，接管照常，只写 env）。
3. **快照政策 = merge-only 补拍（审计 F-1 必修，与 envSnapshot 语义不同构是根因）**：env 域「sidecar 已存在不重拍」成立是因为 envSnapshot 首拍兜的是**整个 env 对象**，任何后续新托管键的原值都已在快照里；nonEnv 域只拍本轮写入键，照抄「不重拍」会在多轮接管下丢用户原值（反例：第 1 轮 JSON 无 `statusLine`、live 有 → 不进账；第 2 轮 JSON 加了它 → 覆写 live 但原值未入快照 → restore 删除用户手写值）。因此：sidecar 不存在 → 对本轮将写入的每个非 env 键 K，live root 有 K 则记入 `nonEnvSnapshot`；sidecar 已存在 → **只对「本轮将写入、且此前不在 nonEnvKeys 并集里」的新增键 K 补拍**（live 有 K 且快照无 K → 补入），已在并集里的键维持原快照条目不重拍（其 live 值必是 DevDesk 写的托管值，重拍会把托管值当用户原值——保留原意图）。
4. `nonEnvKeys` = 本轮写入键 ∪ 既有 sidecar `nonEnvKeys`（去重排序，同 managedKeys `:364-366` 形态）。
5. 陈旧非 env 键删除：在步骤③④之间对 root 重建——「在 nonEnvKeys 并集里、但本轮不写」的键从 root 移除（同 env 陈旧托管键 `:386-401` 同构）。
6. 合并写入：`applyExtraSettingsToSettingsDoc(root, profile.extraSettingsJson)`（含 model 抑制，§4.2 步骤 2-3），在既有 env 合并（`:403-407`）之后执行；`:403` 处「非 env 顶层字段不动」注释改写为新语义。
7. 上层覆盖检测（`:423-427`）维持 **env 键域不动**（managed/project 层的非 env 同名键检测不做——已有行为面，本文不扩，§12）。

### 5.3 restore 扩展

按 managedKeys 的同构逻辑扩到 root 层：重建 root——在 `nonEnvKeys` 里的键一律不透传当前值；`nonEnvSnapshot` 有该键 → 恢复原值，没有 → 删除（`settings_sync.cpp:479-490` 的 env 重建循环照抄到 root 层）。写回后删 sidecar（不变）。「settings.json 解析失败保留 sidecar」「无 sidecar 最小清除」两条既有分支不动（最小清除仍只清 env 的本机代理形态键）。

---

## 6. 项 C：「复制配置」导出升级（已拍板②）

- `buildProfileExportJson`（`claude_ui_logic.cpp:101-127`，声明 `.h:61`）：既有 `{"env":{…}}` 装配之后，追加 `applyExtraSettingsToSettingsDoc(root, effectiveExtraSettingsJson(p.extraSettingsJson, fragmentJson))`——`fragmentJson` 从既有 `cachedSharedConfigs_` 快照 `findSharedById` 取（`claude_module.cpp:545-551` 同款），不再走 env 叠加。序列化改 `stringify(…, /*pretty=*/true)`（完整形态 = 用户可直接粘贴进 settings.json 的文本）。
- 片段 JSON 非法 → WARN + 按 profile 自身 JSON 导出（同坏 extraEnvJson 的防静默丢键手法 `claude_module.cpp:534-541`，改为 WARN 语句覆盖两域）。
- `LOG_WARN` 留痕键数改为「env 键数 + 非 env 顶层键数」。
- toast 文案同步（`claude_module.cpp:566-568`）：标题「已复制配置」不动，副文案改「已把该 Profile 的完整 settings.json 配置（env + 高级属性）导出到剪贴板（API Key 为占位符，不含明文）」。

---

## 7. 项 B：高级配置 tab JSON 编辑框（UI tab 结构不动）

### 7.1 表单状态与预填

- `FormState` 增 `std::string formExtraSettingsJson_;`（紧邻 `formDialectOverride_`，`claude_ui_state.h:172` 区）；`openCreateForm` 置空、`openEditForm` 预填 `p.extraSettingsJson`（DB 里恒为 pretty 保存值；外部改库的非 pretty 原文按原样预填，格式化按钮负责规范化）。
- 绑定下拉随 §8.5 迁入本 tab（`formSharedConfigId_` 渲染位从 env tab `claude_ui_form.cpp:709-770` 整段搬迁，`formFrozenKeys_`/chip/置灰随之拆除——文本框无逐键行，冻结视觉被 §7.3 提示行与详情 pane 来源标注取代）。

### 7.2 布局（初排数字，实施对 600 弹窗逐项核底沿，formError 行 y=h-104=496 让位规则照既有注释 `:982-984`）

| y 区间 | 控件 |
|---|---|
| 104-162 | 方言下拉（不动） |
| 174-232 | 备用 Profile 下拉（不动） |
| 244-302 | **共享配置片段下拉**（自 env tab 迁入；label「绑定片段」+ hint「片段属性在写入时覆盖本页同名键，解绑回落」） |
| 314-330 | JSON label + hint：「高级设置 JSON（settings.json 除 env 外的属性：model / permissions / statusLine / hooks / …）」 |
| 332-432 | 多行编辑框 `components::input("claude.form.advanced.json")`：`.size(w-56, 100)` `.multiline(true).scrollbar(true)` `.fontSize(12)`，placeholder `{ "model": "...", "permissions": { ... } }`，`.onChange` 写 `formExtraSettingsJson_`，`.onCancel` 照既有 Esc=取消（导入框同款 `:910-916`） |
| 440-472 | 「格式化」按钮（110×32，surfaceElevated 中性样式照「解析导入」`:936-943`）：`parse` 当前文本 → 成功则 `stringify(pretty)` 回写状态；失败则红字 |
| 480-496 | 行内红字 `claude.form.advanced.json.error`（单行，`t.colors.error`；格式化/校验失败共用） |

### 7.3 校验与落库（submitForm）

- 保存前 `validateExtraSettingsJson(f.formExtraSettingsJson_)`：失败 → `formError_ = 错误文案`（红字、弹窗不关、**不落库**，D-5/D-6；含 `env` 键拒绝的专属文案「高级设置 JSON 不能包含 env 键（env 请用模型映射 / 高级 env 页配置）」）。
- 通过 → 落库前统一 pretty：`stringify(parse(text), /*pretty=*/true)` 存入 `extraSettingsJson`（D-6；空串跳过）。服务层/DB 不再二次校验格式（写侧已归一；读侧容错在 §4.2/§5.2）。
- 片段已绑定时在 JSON 框下加一行提示：「已绑定片段「X」：同名键以片段为准（解绑后回落本页值）」——取代拆除的 frozenKeys/chip。

---

## 8. 项 E：共享配置片段重构（追加需求，独立成节）

### 8.1 片段弹窗 JSON 化（`drawSharedConfigDialog`，`claude_ui_form.cpp:1222`）

- 现结构 name 行 + `envRows_` 键值编辑器 + 删除按钮（h=460）→ 改为 name 行 + **§7 同款 JSON 编辑框组件**（label / 多行框 / 格式化按钮 / 行内红字四件，盒高可放大到 ~210px：name 82-118、label 128、框 146-356、格式化 364-396、红字 404-420，实施核对）。
- `SharedConfigDialogState`（`claude_ui_state.h:177` 区）：`envRows_` → `extraSettingsJson_`；打开/预填照 `sc.extraSettingsJson`；保存走 `validateExtraSettingsJson` 同一套规则（含 `env` 键拒绝——片段是 settings.json 属性预设，不是 env 装饰层了）。
- 服务层 `createSharedConfig(name, extraEnvJson)` / `updateSharedConfig` 参数改名 `extraSettingsJson`（`profile_repository.h:198-200`）；「已保存」toast 副文案同步（`claude_ui_form.cpp:1224-1226` 区「env 装饰层」措辞改「高级属性预设」）。

### 8.2 绑定语义迁移（D-10）

- **生成时叠加，不物理合并**：绑定片段 → 该片段 `extra_settings_json` 在三条生成链入口与 profile JSON 逐键叠加（`effectiveExtraSettingsJson`，§4.2 步骤 1，**片段赢**）；解绑 → 同名键回落 profile 自身值。片段值从不写进 profile 行——与 09-27 片段设计 env 域拍板同构（`profile_repository.h:190-196`），「解绑即回落」语义保持。
- 冲突语义推荐**片段赢**（协调者建议采纳）：片段即预设模板，模板覆盖个人值的直觉与 env 域「片段赢」一致；被覆盖值无损失（profile 行里原样保留）。
- env tab 的绑定下拉 / `formFrozenKeys_` / chip / 置灰整套拆除（§7.1 已述）；`sharedEnvKeysFor`（`profile_repository.cpp:1140`、服务层 `:315-317`）与 `claude_service.cpp:616` 的冻结键排除逻辑随 env 域死亡一并拆除；`sharedFrozenKeys`（`claude_ui_logic.cpp:64`）与 `formFrozenKeys_` 全引用清空——**含展示层引用（审计 F-2 补全，grep 实测）**：片段管理卡片副文案「N 个键」（`claude_ui_list.cpp:734`，片段 JSON 化后该计数语义失效，改为「高级属性 JSON 预设」类新文案）与详情接管区绑定行「共享配置：片段 X（N 个键）」（`claude_ui_detail.cpp:174-183`，同步改造并入 §8.5 pane 改造范围）。

### 8.3 旧片段数据处置：弃用，不转换（无迁移包袱口径）

env 键值对（如 `{"ANTHROPIC_BASE_URL": "…"}`）**不是合法的非 env 属性 JSON**——把它当 settings.json 顶层属性并入，会把回环地址/占位 token 顶到 root 层写进 live 文件，是注入垃圾不是迁移。「尽力转换」同样做不了：env 键没有 settings.json 对映规则。故 v11 直接 `DROP COLUMN extra_env`（§3.2），旧片段内容清零，用户按新语义重录。此选择按 memory「No migration, no back-compat」口径执行并在迁移注释写明。

### 8.4 env 域片段管线的拆除清单（片段不再携带 env 键后成为死代码）

| 拆除点 | 位置 |
|---|---|
| `buildProxyEnv` 第 5 参 `sharedExtraEnvJson` 与规则 6 | `provider_config.h/.cpp`（调用点 `settings_sync.cpp:300`、`claude_ui_detail.cpp:294`） |
| `buildDirectEnv` 末参 `sharedExtraEnvJson` | `provider_config.{h,cpp}`（调用点 `claude_service.cpp:485`、`claude_ui_logic.cpp:107`、测试） |
| `settings_sync::takeOver` / `SettingsTakeOver::syncTakeOver` 的 `sharedExtraEnvJson` 参（**含测试同步**：`test_settings_sync.cpp:1113` 4 参调用） | `settings_sync.h:99-100`、`settings_takeover.h:64-65`（生效 JSON 经 Profile 副本传入，§5.2①） |
| `sharedExtraEnvJsonFor` → 改名 `sharedExtraSettingsJsonFor`（返回片段 JSON，消费点 `claude_service.cpp:769`→§5.2①、`:490`→runProfile 生效 JSON、`claude_module.cpp:545`→export、detail compose→§8.5） | `profile_repository.{h,cpp}:1118` |
| `EnvSource::Shared` 在 env pane 的出现 | 随片段 env 消失自然消亡；枚举与 `envSourceLabel` 保留（§8.5 复用于 JSON 键标注） |
| `buildProxyEnv` 5 参测试同步（`test_settings_sync.cpp:1064-1079`） | tests |
| `parseEnvForReverseSync` 的 `ownedKeys` 形参与 G10 守卫（审计 F-6：供给方拆除后 G10 永不触发，守卫注释与专测 `provider_config_guard_G10_shared_config_key` 指着已死语义——随批真拆） | `provider_config.h:299-307`、对应专测 |
| 片段卡片副文案「N 个键」/ 详情绑定行「片段 X（N 个键）」（审计 F-2，见 §8.2） | `claude_ui_list.cpp:734`、`claude_ui_detail.cpp:174-183` |
| `sharedFrozenKeys` 直调单测 / `sharedEnvKeysFor` 六处用例（审计 F-2） | `test_claude_ui.cpp:2994-3006`、`test_claude_service.cpp:2148-2286` |

> 拆除已裁决（§12③），**「恒传空串留死通道」退路否决**——死参数 + 死通道 + 假语义（frozenKeys/chip 的 UI 语义已拆，参数无对应 UI）正是本项目反对的形态（v10 迁移「死列」先例）。清单以 F-2/F-6 补全为真拆前置：下表为 grep 全仓对账结果，实施若再遇清单外引用点（编译器会指出），按同语义处置并回写本表。

### 8.5 详情「高级配置」pane 增 JSON 键来源展示（0edd3cf 形态复用）

- `DetailView` 增 `std::vector<std::pair<std::string, std::string>> extraSettingsKeys;`（键 → 来源标签）与 `std::string extraSettingsNote;`。compose（`claude_module.cpp:135` 区）算生效 JSON（`effectiveExtraSettingsJson`），逐顶层键给来源：片段赢的键 → `envSourceLabel(EnvSource::Shared)`（「片段」）、其余 → 「本 Profile」；`"model"` 键附加说明「覆盖模型槽位（ANTHROPIC_MODEL 不写入）」（`extraSettingsNote`）。
- `drawDetailAdvanced`（`claude_ui_detail.cpp`）在方言/备用两行下渲染键清单，行样式沿用该 pane 的 label+值盒写法（`:71-114` 同款）；上限 8 行 + 「另有 N 项」提示（env pane 溢出形态）；JSON 为空 → 单行「未配置高级属性 JSON」；解析失败 → 单行「（JSON 解析失败，接管时将忽略）」。
- **实施闸门（裁决⑤，审计 policy 5）**：`drawDetailAdvanced` 现状 `(void)contentBottom` + 固定返回（`claude_ui_detail.cpp:762-764`，高度与容器无关）——键清单引入后该不变式必破：**必须真消费 `contentBottom`**（按 env pane P2 行预算同款口径）或以硬上限 + 溢出提示保证 pane 不与动作区重叠；同步分发区「paneBottom 恒 312/340/214」注释与既有 actY/dividerY 五值相等断言（`test_claude_ui.cpp:4118-4144` 区）。8 行只是暂定数值，**contentBottom 消费才是护栏**——验收见 §10 变异⑥。

---

## 9. 改动文件清单（预估）

| 文件 | 改动 |
|------|------|
| `src/core/migrations.h` | v11（三条语句 + 弃用说明） |
| `src/module_claude_proxy/profile_repository.{h,cpp}` | Profile/SharedConfig 字段、SELECT/rowToProfile、createProfile 尾参、updateProfile 两分支、duplicateProfile 二段补写、`sharedExtraEnvJsonFor`→`sharedExtraSettingsJsonFor`、删 `sharedEnvKeysFor` |
| `src/module_claude_proxy/provider_config.{h,cpp}` | 三个纯函数（§4.3）+ `kMaxExtraSettingsBytes` + def 槽位 envKey 判定纯函数（F-10）；`buildProxyEnv`/`buildDirectEnv` 拆片段 env 参 + G10 守卫拆除（§8.4） |
| `src/module_claude_proxy/claude_service.{h,cpp}` | createProfile 尾参；syncTakeOver 生效 JSON 副本；runProfile 合并；删 `sharedEnvKeysFor` 转发与 `:616` 排除逻辑 |
| `src/module_claude_proxy/settings_sync.{h,cpp}` | takeOver 非 env 并入 + sidecar v2（merge-only 补拍）+ restore root 层记账；拆 `sharedExtraEnvJson` 参 |
| `src/module_claude_proxy/settings_takeover.{h,cpp}` | `syncTakeOver` 拆参透传 |
| `src/module_claude_proxy/claude_ui_state.h` | `formExtraSettingsJson_`；片段弹窗状态字段改名 |
| `src/module_claude_proxy/claude_ui_form.cpp` | 高级配置 tab JSON 区块 + 片段下拉迁入；submitForm 校验/pretty；env tab 拆绑定区；`drawSharedConfigDialog` JSON 化 |
| `src/module_claude_proxy/claude_ui_logic.{h,cpp}` | `buildProfileExportJson` 升级；删 `sharedFrozenKeys` |
| `src/module_claude_proxy/claude_ui_detail.{h,cpp}` | `DetailView` +2 字段；`drawDetailAdvanced` 键清单 + contentBottom 真消费（裁决⑤）；绑定行改造（F-2，`:174-183`）；`isModelMappingEnvKey` 改转发判定（F-10） |
| `src/module_claude_proxy/claude_ui_list.cpp` | 片段管理卡片副文案改造（F-2，`:734`「N 个键」语义失效） |
| `src/module_claude_proxy/claude_module.cpp` | compose 装配；doCopyConfig 片段取数改 JSON 域 + toast 文案 |
| `tests/` | §10 用例（test_claude_ui / test_settings_sync / test_claude_service / provider_config 纯函数）+ 参数拆除机械同步（§8.4 表：`test_settings_sync.cpp:1113, :1064-1079`、`test_claude_service.cpp:2148-2286`、`test_claude_ui.cpp:2994-3006`、G10 专测） |

---

## 10. 验收判据

- **定向测试**：`bash scripts/build-test-run.sh --filter claude --no-run` 全绿；`--filter json`（platform 层不受影响，回归确认）；迁移用例（ScopedAppDatabase 脚手架）：全新库 v11 生效 + 旧库（v10 行 + 旧 shared_configs env 数据）升级后 profiles 默认 ''、片段 env 列已删。
- **纯函数断言**（§4.3，无头）：
  1. `effectiveExtraSettingsJson`：片段赢（同键片段值胜）/ 解绑回落（fragmentJson="" 时 profile 值原样）/ 任一侧 "env" 键跳过且 warnings 记录 / 非法一侧按空处理 / 双空返回空串。
  2. `applyExtraSettingsToSettingsDoc`：写入键集正确 / "model" 触发 `modelSuppressed=true` 且 env 中 `ANTHROPIC_MODEL` 被移除、`ANTHROPIC_DEFAULT_SONNET_MODEL` 保留 / 顶层非对象输入返回 warnings 不写。
  3. `validateExtraSettingsJson`：空串 OK / 非法 / 顶层数组 / 超 1MB / 含 "env" 五态各断言。
- **接管/还原往返**（临时目录 Paths 注入，既有 settings_sync 测试手法）：① 接管写入 env + JSON 属性；live 文件已有同名非 env 键时 JSON 值胜，旧值进 `nonEnvSnapshot`；② `ANTHROPIC_MODEL` 抑制：JSON 含 "model" 时 env 无该键（apply 步移除，F-5 时序）且该键仍在 managedKeys 并集（restore 语义不变）；③ restore：JSON 写入且快照无原值的键被删除、快照有原值的键恢复、sidecar 删除；④ **merge-only 补拍（F-1）**：二次接管对已托管键不重拍、对新增键首写拍快照——含审计反例用例：第 1 轮 JSON=`{"model":"x"}`（live 另有手写 `"statusLine"`，不进账）→ 第 2 轮 JSON 加 statusLine 热切换 re-takeover → statusLine 原值入快照 → restore **恢复**而非删除；⑤ 旧形态 sidecar（无新字段）restore 不误删任何非 env 键；⑥ **片段叠加端到端（F-8）**：绑定片段（片段与 profile JSON 各含独有键 + 同键）→ 门面 §5.2① 装配生效 JSON → takeOver 写入片段键且同键为片段值（片段赢）→ restore 全部撤走（骨架照 `test_settings_sync.cpp:1101-1113` 既有片段接管用例平移 JSON 域——钉住门面装配这一跳，装配漏传/传未生效化原文时必红）。
- **格式化器（D-2）**：`parse→stringify(pretty)` 幂等（格式化两次输出相等）；两空格缩进断言；round-trip 等值（pretty 前后 parse 结果相等）；键序为字典序（如输入 `{"b":1,"a":2}` 输出 `a` 先行——把 D-2 的重排行为钉进测试）。
- **UI 用例**（headless，手法照 `claude_ui_advanced_tab_dialect_persists`）：编辑 profile 存 JSON → DB 中为 pretty 文本（断言含 `\n  "`）；非法 JSON 保存 → `formError_` 非空且 getProfile 值不变（不落库）；含 "env" 键 → 拒绝文案；格式化按钮：compact 输入 → 状态变 pretty；片段下拉迁入 tab 3 后绑定/保存/悬空归一与迁移前等价；片段弹窗 JSON 保存/重开回读。
- **导出**：`buildProfileExportJson` 顶层 = `env` + JSON 属性（如 `model`）；toast 文案断言。
- **变异自检**（各回退一处 → 对应用例红）：① 删 submitForm 校验 → 非法 JSON 落库用例红；② 删 model 抑制 → 往返用例②红（抑制唯一落点在 apply）；③ restore 删 root 层记账 → 往返③红；④ 落库 pretty 改存原文 → DB pretty 断言红（**该用例表单输入必须用 compact/未格式化文本**，如 `{"model":"x","permissions":{…}}` 单行——审计 F-7：输入本身是 pretty 时「存原文」与「存 pretty」字节相同，变异恒绿）；⑤ `effectiveExtraSettingsJson` 改 profile 赢 → 片段赢断言红；⑥ `drawDetailAdvanced` 退回 `(void)contentBottom`（去掉真消费）→ 键清单超限时 pane 越内容框断言红（裁决⑤：**contentBottom 消费是护栏，8 行数值不是**）。
- **截图**：probe `--dump` 真渲染（glReadPixels，锁屏/旧帧免疫 + 40 帧预热，`plans/2026-09-30-wave-b-impl-verify.md` §6 先例）：表单高级配置 tab（含 JSON 框 + 绑定下拉）、片段弹窗、详情高级配置 pane（含键来源标注）各一张，落 `docs/screenshots/`。

---

## 11. 风险与回滚

- **模型抑制静默感**：JSON 含 "model" 时槽位 def 失效，用户可能不知 → 详情 pane 来源标注 + 「覆盖模型槽位」说明（§8.5）+ 表单 hint 明示。已拍板①语义，无回退。
- **DROP COLUMN 兼容**：实施首日在两端平台（macOS 系统 SQLite / Windows MSYS2 sqlite）各跑一次 v11 迁移用例确认；失败退路 = 不 DROP、留空死列（列永不写），功能不受影响。
- **sidecar 中间态**：接管（新字段）后由旧版本二进制还原 → 旧 restore 不认识 `nonEnvKeys`，非 env 键残留 live 文件。本项目单二进制、无并行版本场景（安装即覆盖），风险仅存于手工混跑，登记不修。
- **JSON 写入 hooks 等可执行配置**：与 extra_env 同一信任模型（用户本机、用户自己的配置）；还原闭环保证停代理后 live 文件回到原状。
- **回滚**：数据层单列可空（DEFAULT ''），revert 代码即回；片段重构含 DROP COLUMN，回滚需 v11 之后不再有依赖 `extra_settings_json` 的迁移——本批全部收敛在 v11 一条内（不尾插 v12），revert 干净。

---

## 12. 裁决落定（主会话 2026-09-30，采纳独立审计 `plans/2026-09-30-advanced-json-audit.json` policy_rulings 全部五项）

| # | 议题 | 裁决 | 落点 |
|---|------|------|------|
| ① | runProfile 纳入合并链 | **纳入**：三链分叉是用户可感知的行为不一致；合并已收敛纯函数一处，纳入成本≈一次调用；临时 settings 本就是完整 settings.json 形态语义，env-only 是历史残留；无 sidecar 记账负担（临时文件随会话自删 + 24h 清扫兜底） | §4.1 / §9 claude_service 行 |
| ② | 片段赢（预设覆盖 profile 值） | **片段赢**：与 env 域既有拍板（buildProxyEnv 规则 6）同构，两域方向一致不新增认知负担；被覆盖值原样留 profile 行、解绑回落，无损失；翻转成本一处函数 | §4.2 步骤 1 / §8.2 |
| ③ | env 域片段管线拆除 vs 恒传空串 | **真拆**：「恒传空串留死通道」退路否决（死参数 + 死通道 + 假语义 = v10「死列」先例反对的形态）；前置条件 = §8.4 清单按 F-2/F-6 补全（已完成），实施再遇清单外引用点按同语义处置并回写本表 | §8.4 / §9 |
| ④ | 上层覆盖检测扩非 env 键 | **不扩**，登记后续项：非 env 域 DevDesk 写的值本就来自用户手写 JSON，managed/project 层同名键覆盖是文件分层正常语义，告警噪音大于价值；且扩域牵动 detectOverlayConflicts 的 env 专用键集来源 | §5.2⑦ 维持 env 键域 |
| ⑤ | 详情 pane 键清单上限 8 行 | **8 行暂定 + contentBottom 真消费为闸门**：`drawDetailAdvanced` 现状 `(void)contentBottom` + 固定返回的不变式必破，实施必须真消费 `contentBottom`（或硬上限 + 溢出提示保证不与动作区重叠），并同步 paneBottom 恒值注释与 actY 五值相等断言；`(void)contentBottom` 不变式已写进验收变异⑥ | §8.5 / §10 变异⑥ |

审计 10 findings 处置：F-1（§5.2③ merge-only 补拍 + §10 往返④反例）、F-2（§8.2/§8.4/§9 清单补全）、F-3（§3.3 复制链带 extraSettingsJson + 门槛扩展）、F-4（§3.2 版本口径 ≥3.35）、F-5（§4.2 步骤 3 机制时序改正）、F-6（§8.4 G10 通道拆除行）、F-7（§10 变异④ compact 输入约束）、F-8（§10 往返⑥端到端用例）、F-9（§2.4 行号校正 + 设计时点快照注）、F-10（§2.2/§4.3 判定下沉 provider_config）——全部已修，无登记不修项。
