// src/module_claude_proxy/provider_config.h
// Claude 供应商完整配置——数据模型纯函数层
// （spec docs/superpowers/specs/2026-09-22-claude-provider-config-design.md §4 + §4.3 规则 6）
//
// 设计要点：
//   - 全部为纯函数（platform::json，无 IO / 无 DB），单测直接覆盖
//   - Profile 里只存 JSON 原文（modelsConfigJson / extraEnvJson），
//     槽位解析/派生/兼容合并收敛到这里
//   - 空串 = 未配置（读路径容错，全空槽位）
#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"
#include "platform/json.h"

namespace module::claude {

// 单个模型槽位：model = 发往上游的实际模型名（原样保存，含 [1M] 等标记）；
// name = Claude Code 界面显示名（可空，空则派生）
struct ModelSlot {
    std::string model;
    std::string name;
};

// ---- 槽位单一真相源 ----
// 表已迁到 src 根中立头 model_slots.h（module_agent 的模型槽位下拉与本模块共用
// 同一份真源；两模块不可互相 include，故落中立头）。宏名与行参数契约见该文件。
#include "model_slots.h"

// 模型槽位（§4.1）：成员由槽位表生成，声明顺序 = 表顺序
struct ModelsConfig {
#define DEVDESK_MODEL_SLOT_MEMBER(name, jsonKey, label, formLabel, envKey, nameKey) \
    ModelSlot name;
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_MEMBER)
#undef DEVDESK_MODEL_SLOT_MEMBER
};

// 槽位元数据（表行）
struct ModelSlotMeta {
    const char* jsonKey;    // models_config 槽位键
    const char* label;      // 详情面板 / 摘要显示名
    const char* formLabel;  // 表单页槽位列名
    const char* envKey;     // 发往上游的模型键
    const char* nameKey;    // 官方 *_NAME 键；nullptr = 无该变体
    ModelSlot ModelsConfig::*slot;  // 对应数据成员
};

// 槽位表（顺序 = UI 展示顺序 = managedEnvKeyNames 的槽位键顺序）。
// 消费方一律遍历本表派生，不再手写 N 个槽位分支
#define DEVDESK_MODEL_SLOT_ROW(name, jsonKey, label, formLabel, envKey, nameKey) \
    ModelSlotMeta{jsonKey, label, formLabel, envKey, nameKey, &ModelsConfig::name},
inline constexpr ModelSlotMeta kModelSlotMetas[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_ROW)
};
#undef DEVDESK_MODEL_SLOT_ROW

// 槽位数（由表派生：加一行即 +1；全项目不再硬编码 6）
#define DEVDESK_MODEL_SLOT_COUNT_ONE(name, jsonKey, label, formLabel, envKey, nameKey) +1
inline constexpr std::size_t kModelSlotCount =
    0 DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_COUNT_ONE);
#undef DEVDESK_MODEL_SLOT_COUNT_ONE

// 编译期守卫：成员数必须 == 表行数——给 ModelsConfig 加了成员却忘了在
// DEVDESK_MODEL_SLOT_LIST 加行，两侧构型大小对不上，这里直接编译失败。
//（ModelsConfig 成员全是 ModelSlot、无填充，故可按下式比较）
static_assert(sizeof(ModelsConfig) == kModelSlotCount * sizeof(ModelSlot),
              "新增/删除模型槽位后请同步 DEVDESK_MODEL_SLOT_LIST："
              "ModelsConfig 成员数必须与槽位表行数一致");

inline std::span<const ModelSlotMeta> modelSlotTable() { return kModelSlotMetas; }

// 显示名派生：剥除尾部 [1M]/[200K]/[1m] 等上下文窗口标记（§2.2）
std::string deriveDisplayName(const std::string& model);

// def 槽位的托管 env 键名（审计 F-10 单一来源）：从槽位表派生
//（jsonKey=="default" 行的 envKey），不手抄键名。新增抑制判定 /
// claude_ui_detail 的 isModelMappingEnvKey（UI 层，转发本处判定）都以此为准
const char* modelSlotDefEnvKey();

// 键是否属于槽位表托管的模型键（envKey 或 *_NAME 键）。原 .cpp 匿名命名空间
// helper 提升：claude_ui_detail.cpp 的 isModelMappingEnvKey 转发本判定
//（F-10：provider_config 不能反向 include UI 文件，判定落本层、UI 复用），
// 两处注释互指单一来源
bool isModelSlotEnvKey(const std::string& key);

// ---- models_config 序列化 / 反序列化 ----
// 序列化固定写全槽位表所有行（空槽位写空串，roundtrip 稳定）
core::Result<std::string> serializeModelsConfig(const ModelsConfig& config);

// 容错语义：
//   - 空串 → ok，全空槽位（未配置）
//   - 顶层非对象 / JSON 解析失败 → fail
//   - 槽位缺失或非对象 → 该槽位为空；model/name 非字符串 → 为空（容错）
core::Result<ModelsConfig> parseModelsConfig(const std::string& json);

// ---- extra_env 序列化 / 反序列化（值一律字符串）----
core::Result<std::string> serializeExtraEnv(
    const std::map<std::string, std::string>& env);

// 空串 → ok 空表；顶层非对象 → fail；值非字符串 → fail（值语义歧义，不隐式转换）
core::Result<std::map<std::string, std::string>> parseExtraEnv(
    const std::string& json);

// ---- 认证字段（鉴权头三态；2026-09-27 profile-auth-field 设计 §3/§4）----
// both    = 双发 x-api-key + Authorization: Bearer（默认，两种网关都兼容）
// bearer  = 只发 Authorization: Bearer
// api_key = 只发 x-api-key
enum class AuthField { Both, Bearer, ApiKey };

// 文本 → 枚举：'bearer' / 'api_key' / 其余（'both'、未知、空串）→ Both（设计 D-8 读侧兜底）
AuthField authFieldFromText(const std::string& text);

// 枚举 → 文本：规范值（写侧规范化用，保证库里只有三个规范值之一）
const char* authFieldToText(AuthField field);

// models_config 读入槽位：为空串 / 非法 JSON → 全空槽位（读路径容错，不炸）
ModelsConfig effectiveModelsConfig(const std::string& modelsConfigJson);

// ---- 非官方变量别名映射（§4.3 规则 6）----
struct EnvAliasMapping {
    // 映射后的完整 env：别名键已转换为官方键，其余键原样透传
    std::map<std::string, std::string> env;
    // 命中别名表的原始键（UI 提示「已映射为官方变量」）
    std::vector<std::string> aliasMappedKeys;
    // 别名表外、且不在官方键清单（env_var_hints.h 全量官方变量 + DevDesk 自造
    // 的 FABLE 扩展键）中的键（原样透传 + 警告角标，行为由用户自负）
    std::vector<std::string> unrecognizedKeys;
};

EnvAliasMapping applyEnvAliases(const std::map<std::string, std::string>& env);

// ---- cc-switch / settings.json 粘贴导入（§6.1 + §7.4，Phase D）----
// 输入：粘贴的原文（两种形状皆可：整段 settings.json（取其 env 子对象）或
// 裸 env 对象）。纯函数，无 IO，UI 导入入口直接消费。
struct ImportedConfig {
    std::string baseUrl;    // ANTHROPIC_BASE_URL（未出现 = 空串，不覆盖表单）
    std::string authToken;  // ANTHROPIC_AUTH_TOKEN / ANTHROPIC_API_KEY（同现取
                            // AUTH_TOKEN 并给 warning）
    ModelsConfig models;    // *_MODEL / *_MODEL_NAME / SUBAGENT → 槽位
    std::map<std::string, std::string> extraEnv;  // 其余键（别名归一后）
    std::vector<std::string> aliasMappedKeys;     // 「已映射为官方变量」提示
    std::vector<std::string> unrecognizedKeys;    // 警告角标（透传，行为自负）
    std::vector<std::string> warnings;            // 其他警告（API_KEY/AUTH_TOKEN 同现等）
    int recognizedSlots = 0;  // 预览用：model 非空的槽位数
    // 粘贴里两个键名的证据（设计 §5）：'api_key'（只出现 ANTHROPIC_API_KEY）/
    // 'bearer'（只出现 ANTHROPIC_AUTH_TOKEN）/ 'both'（同现）/ ""（都没出现
    // = 无证据，UI 不覆盖表单现值）
    std::string authFieldFromEnv;
    // 反向同步（§3.3 守卫表）专用：命中守卫而跳过的键名（含别名归一后的键名
    // 与 G2 联动跳过的 token 键）——语义是「不写也不清」。mergeReverseSyncEnv
    // 靠它区分 G5「键缺席（清空）」与 G1–G4/G6「跳过（保持 profile 原值）」；
    // parseImportedEnv（粘贴导入）恒为空，行为不受影响
    std::vector<std::string> skippedKeys;
};

// 校验（§7.4）：env 值必须字符串（数字/布尔拒绝）；键名 ^[A-Z][A-Z0-9_]*$；
// 值 ≤ 4KB/键；键数 ≤ 64；整体文本 ≤ kMaxImportTextBytes（1MB）。非法输入整体
// fail，不部分导入。
core::Result<ImportedConfig> parseImportedEnv(const std::string& text);

// 粘贴导入的整体文本上限（审计 2026-09-27 L6，参考 platform/http_server.h 的
// kMaxBodyBytes 口径）：挡在 JSON 解析之前，避免超大输入进入解析 / 深拷贝链
//（platform::json 按值 = 整树深拷贝，见 platform/json.h）。合法载荷上限约
// 64 键 × 4KB 值 ≈ 256KB，1MB 留 ~4x 余量。超限整体 fail（文案含上限）。
inline constexpr size_t kMaxImportTextBytes = 1u * 1024u * 1024u;

// ---- 高级设置 JSON（2026-09-30 advanced-json-editor 设计 §4.3）----
// settings.json 除 env 外全部顶层属性（model / permissions / statusLine /
// hooks / …）的合并 / 校验纯函数（无 IO / 无 DB / 无日志——warnings 由调用方
// 记账与呈现），三条生成链（接管 takeOver / 运行 runProfile / 复制配置导出）
// 共用，合并规则只此一处（同 buildProxyEnv「合并一处」项目口径）。

// 整体文本上限（设计 D-7）：紧邻 kMaxImportTextBytes 同一理由——platform::json
// 按值 = 整树深拷贝（json.h:18-23），超大输入挡在解析之前。settings.json 实际
// 合法载荷（hooks / permissions）远小于 1MB
inline constexpr size_t kMaxExtraSettingsBytes = 1u * 1024u * 1024u;

// 生效 JSON：片段叠加在 profile JSON 之上（逐键叠加，**片段赢**，设计 D-10 /
// §4.2 步骤 1——与 env 域 buildProxyEnv 规则 6 同方向）。解绑（fragmentJson 为
// 空串）后同名键回落 profile 自身值；任一侧顶层 "env" 键跳过 + warnings 记录
//（D-5 读侧兜底：防 DB 直写等非 UI 路径绕过校验）；非法 / 超限 / 顶层非对象
// 一侧按空处理（warnings 记录），另一侧照用；两侧都空 → 空串。
// 输出 pretty 规范形（stringify(value, /*pretty=*/true)；键序为字典序——
// JsonImpl::objVal 是 std::map，与 settings_sync 写 settings.json 同一条
// 序列化通道，设计 D-2）。warningsOut 非空则先清空再填
std::string effectiveExtraSettingsJson(
    const std::string& profileJson,
    const std::string& fragmentJson,
    std::vector<std::string>* warningsOut = nullptr);

// 生效 JSON 本轮将写入的顶层键（best-effort 枚举）：空串 / 非法 / 超限 /
// 顶层非对象 → 空集；"env" 键不在列（它永不写入，D-5）。takeOver 的
// 快照 / 陈旧键删除输入（§5.2③⑤）——用户可见的解析失败警告由
// applyExtraSettingsToSettingsDoc 统一发，本函数静默防双报
std::vector<std::string> extraSettingsTopLevelKeys(
    const std::string& extraSettingsJson);

// settings.json **原文** → 高级设置 JSON（除 env 外全部顶层属性，pretty 规范形）。
// batch-C C-4：「导入当前配置」用——parseImportedEnv 按职责只取 env 子对象，
// 非 env 顶层属性（model / permissions / statusLine / hooks / …）不在它的返回值
// 里，必须在此独立抽取，否则导入后高级配置恒空。
// 空串 / 超 kMaxImportTextBytes / 解析失败 / 顶层非对象 / 除 env 外无键 → 空串
//（= 未配置，与 extraSettingsJson 语义一致）；结果超 kMaxExtraSettingsBytes
// 同样按空串（与 validateExtraSettingsJson 的上限口径一致，导入不产生超限值）
std::string nonEnvTopLevelJson(const std::string& settingsDocText);

// 并入 settings 文档的应用结果（§4.3）
struct ExtraSettingsApply {
    std::vector<std::string> writtenKeys;   // 本轮写入的非 env 顶层键（排序去重）
    bool modelSuppressed = false;           // 顶层 "model" 触发 ANTHROPIC_MODEL 抑制
    std::vector<std::string> warnings;      // "env" 键跳过 / 解析失败等（调用方记账）
};

// 并入 settings 文档（§4.2 步骤 2-3）：对生效 JSON 的每个顶层键 K：
//   K == "env"  → 跳过 + WARN（D-5 读侧兜底）
//   其余        → root[K] = 值（**赢过 live settings.json 既有同名键**，设计
//                 D-3：被覆盖旧值已在 sidecar 快照中，还原无损）
// "model" 为非空字符串时（已拍板 D-4①）：同时从 root["env"] 移除
// "ANTHROPIC_MODEL"（modelSlotDefEnvKey 派生，不论该键来自 def 槽位 /
// extra_env / 别名归一）——抑制唯一落点在本函数（§4.2 步骤 3，审计 F-5 时序
// 口径：发生在合并写入之后的 apply 步，不是陈旧删除步）；其余槽位键
//（SONNET/OPUS/…）无顶层对映键，照常写入。root["env"] 缺失 / 非对象时抑制步
// 跳过（不炸；调用方前置保证 env 已是对象）。
// extraSettingsJson 空串 / 非法 / 超限 / 顶层非对象 → warnings 记录且零写入
ExtraSettingsApply applyExtraSettingsToSettingsDoc(
    platform::json::JsonObject& root,
    const std::string& extraSettingsJson);

// 应用 / 保存校验（UI 与服务共用，设计 D-5/D-6/D-7）：空串 OK（= 未配置）；
// 文本 ≤ kMaxExtraSettingsBytes；必须解析成功；顶层必须是对象；顶层不得含
// "env" 键。失败带中文错误（表单 formError_ / 片段弹窗红字直出）
core::Result<void> validateExtraSettingsJson(const std::string& text);

// ---- 接管 env 生成（§4.3，Phase C 接管通道的输入）----
// 输入：profile 的三个数据源字段 + 代理端口；输出：要写入
// ~/.claude/settings.json env 块的托管键值（纯函数，无 IO）。
// 规则：
//   1. ANTHROPIC_BASE_URL = http://127.0.0.1:<port>（上游真实 key 永不出现在
//      settings.json，由代理注入）
//   2. ANTHROPIC_AUTH_TOKEN = kProxyAuthPlaceholder（占位符：代理无鉴权，
//      此键只是让 Claude Code 有值可发）
//   3. 模型槽位（effectiveModelsConfig）→ 槽位表每行的 envKey
//      （见 DEVDESK_MODEL_SLOT_LIST）；槽位空则跳过（Claude Code 回落内置映射）
//   4. 显示名 → 对应行的 *_MODEL_NAME 键（表行 nameKey 为 nullptr 的槽位无官方
//      _NAME 变体，不生成）；name 空则 deriveDisplayName 派生
//   5. extra_env 经 parseExtraEnv（非法则整体忽略，读路径容错）+ applyEnvAliases
//      别名归一后平铺；与 1-4 冲突时 1-4 优先（导入数据可能带旧 BASE_URL/token）
//（原规则 6「共享片段 env 叠加」随 2026-09-30 advanced-json-editor §8.4 拆除：
//  片段已重构为高级属性 JSON 预设，叠加走 effectiveExtraSettingsJson /
//  applyExtraSettingsToSettingsDoc 的 JSON 域，env 域不再吃片段）

// 接管时写入 settings.json 的占位凭据（代理仅监听 127.0.0.1、无鉴权；
// 真实上游 key 由代理转发时注入）
inline constexpr const char* kProxyAuthPlaceholder = "PROXY_MANAGED";

// 生效 env 的来源分层（2026-09-28 详情面板 P3）——**只为标注出处**，不参与
// 合并决策（合并顺序写在 buildProxyEnv 里，一处）。优先级（高→低）：
//   **托管键 > 片段 > profile 自身** —— 即写入顺序 profile → 片段 → 托管键，
//   后写者赢（与 buildProxyEnv 的规则 5「托管键随后写入 → 冲突时托管键优先」
//   和 settings_sync.cpp「托管键在最上层」同口径）。
//（2026-09-29 审计 F-04：本条此前把顺序写反成「片段 > profile 自身 > 托管键」，
//  与规则 5 矛盾；代码一直是对的，错的只是这行注释。）
// 为什么需要它：env 是扁平 map，合并结果里没有出处（规则 6 的注释），
// 详情面板要标「这条键是谁给的」就只能让合并方顺手记一层。
enum class EnvSource {
    Profile = 0,  // profile 自身 extra_env（含别名归一）
    Shared,       // 该 profile 所绑共享配置片段
    Managed,      // DevDesk 托管键（BASE_URL / 占位 token / 模型槽位表派生键）
};

std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson,
    int proxyPort,
    // 可选出参：每个键的来源（见 EnvSource）。非空则先清空再填；传 nullptr
    // （默认）= 不记录，既有调用方零改动。（advanced-json-editor §8.4：原
    // sharedExtraEnvJson 形参拆除——片段不再携带 env 键）
    std::map<std::string, EnvSource>* sourcesOut = nullptr);

// ---- 直连 env 生成（2026-09-28 profile-run 设计 §4.2，D-1：临时 settings + --settings）----
// 输入 = profile 数据字段 + 已读出的密钥明文（**本函数不碰存储**，读失败由
// 调用方在调本函数前中止）；输出 = 临时 settings.json 的 env 子对象
// （纯函数，无 IO）。与 buildProxyEnv 同款的槽位 loop / extra_env 别名归一，
// 仅托管键不同（直连形态）。（advanced-json-editor §8.4：原共享片段叠加形参
// 拆除——片段叠加走 JSON 域）
//   1. ANTHROPIC_BASE_URL = baseUrl 去尾斜杠（照 updateProfile 写侧口径）；空
//      串 → 不写该键（Claude Code 回落内置默认上游）
//   2. 密钥键按 profile 的「认证字段」三态**只写一个**（直连路径携带的
//      **唯一凭据**；「双发」是代理路径的特性，且代理会剥客户端鉴权头、直连
//      没有这层保护——extra_env 带进的另一密钥键会被清掉，防止两个凭据并存时
//      网关取到过期的那一个；advanced-json-editor §8.4 起片段不再进 env 域，
//      原文的「共享片段带进」字样随之删除）：
//        bearer    → ANTHROPIC_AUTH_TOKEN
//        api_key   → ANTHROPIC_API_KEY
//        both      → ANTHROPIC_AUTH_TOKEN（默认；若上游网关要求 x-api-key，
//                    把该 profile 的认证字段设为 api_key 即可）
//      密钥值空串 → 不写该键（上层已用读凭据列失败挡住绝大部分空值）
//   3. 模型槽位（effectiveModelsConfig）：空槽位跳过；*_NAME 派生显示名，
//      subagent 槽位对应 CLAUDE_CODE_SUBAGENT_MODEL（照表派生）
//   4. extra_env 先入表；托管键（BASE_URL 与密钥键）最后写入压过它们
std::map<std::string, std::string> buildDirectEnv(
    const std::string& baseUrl, const std::string& apiKeyPlain,
    const std::string& authField,
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson);

// 托管键全集（审计 CPX-12）：BASE_URL + 占位 token + 槽位表派生的 env 键与
// *_NAME 键（CPX-08 收敛后由 modelSlotTable() 遍历生成，当前 6 槽位 = 12 键）；
// 与槽位是否有值无关（槽位为空时 buildProxyEnv 不生成该键，但上层同名键仍会
// 压过接管，检测侧需要全集）。
// 消费方：settings_sync 的上层覆盖检测（与「本轮实际写入的托管键」取并集）
std::vector<std::string> managedEnvKeyNames();

// ---- 反向同步（settings.json env → profile，设计 §3.2/§3.3，M1/M2）----
// 全部为纯函数（无 IO / 无 DB / 无日志），单测直接覆盖。

// loopback URL 判定（G2「本地代理形态」）：剥 scheme → 取 host[:port]（IPv6 去
// 方括号）→ 小写后 ∈ {127.0.0.0/8 全段, localhost, ::1}。不做前缀匹配
//（http://127.0.0.1.evil.com / http://127.0.0.1evil.com 都不算）
bool isLoopbackUrl(const std::string& url);

// 反向同步的**容错版** env 解析（§3.2 映射表 + §3.3 守卫表）：
// 与 parseImportedEnv 的「非法即整体 fail」不同——单个怪异值只跳过该键 +
// warning（粘贴导入的严格契约搬过来会让一个无关的怪异值阻断用户切 profile）。
// 输入 = settings.json 的 env 对象原文（settings_sync 读侧产出）。
// 输出 cfg 复用 ImportedConfig；cfg.skippedKeys 是「不写也不清」的键名集合
//（G1–G4/G6；含别名归一后的键名，因 profile 的 extra_env 存的是归一后的键）。
// 仅 env 本身非法（非 JSON 对象）才 ok=false。
//
// 守卫表 G1–G6：**每条守卫一个专测**，编号与用例名一一对应（增键/改守卫时同批
// 更新，漏改测试则名字对不上、一眼可见）：
//   G1 sidecar 托管键（键 ∈ managedKeys）        → 跳过该键（不写也不清）
//      ｜provider_config_guard_G1_sidecar_managed_key
//   G2 loopback BASE_URL（isLoopbackUrl 命中）   → base_url 与本次 token 都跳过
//      ｜provider_config_guard_G2_loopback_base_url
//   G3 占位 token（值 == kProxyAuthPlaceholder） → 跳过 token（保持 profile 原密钥）
//      ｜provider_config_guard_G3_placeholder_token
//   G4 空值（键存在但值为空串）                   → 跳过该键（不写也不清）
//      ｜provider_config_guard_G4_empty_value
//   G5 键缺席（非守卫托管字段不在 env 里）         → 清空 profile 对应值
//      ｜provider_config_guard_G5_absent_clears（语义落在 mergeReverseSyncEnv）
//   G6 值形态非法（非字符串 / 长度 > 4096）        → 跳过该键 + WARN，不整体 fail
//      ｜provider_config_guard_G6_invalid_value
//（G10「键 ∈ 绑定片段 ownedKeys」守卫随 2026-09-30 advanced-json-editor §8.4
//  拆除：片段不再携带 env 键，ownedKeys 供给方 sharedEnvKeysFor 已删——守卫
//  永不触发的死通道按「真拆」裁决移除，专测同步删除）
struct ReverseSyncParse {
    bool ok = false;
    std::string error;
    ImportedConfig cfg;
    // 命中守卫而跳过的键名（镜像 cfg.skippedKeys，便于调用方就近读取）
    std::vector<std::string> skippedKeys;
};

ReverseSyncParse parseEnvForReverseSync(
    const std::string& envJson,
    const std::vector<std::string>& managedKeys);

// 反向同步的第二段（§3.4 出现/缺席语义）：把 cfg 对齐到 profile 现值上。
//   - 出现（cfg 非空）→ 覆盖；缺席 → 清空；命中守卫（skippedKeys）→ 保持原值
//   - def / subagent 的 name 无官方 _NAME 变体（不属托管字段）→ 任何情况下不动
//   - 以 effectiveModelsConfig(现值, 旧 model 列) 为基底落位
//   - only-changed：与现值逐字段比较，没变则 *Changed=false 且输出回传入参原文
//   输出为空表时 modelsConfigJson / extraEnvJson 用空串（本层「空串 = 未配置」约定）
struct ReverseSyncMerge {
    std::string modelsConfigJson;
    std::string extraEnvJson;
    bool modelsChanged = false;
    bool extraEnvChanged = false;
};

ReverseSyncMerge mergeReverseSyncEnv(const ImportedConfig& cfg,
                                     const std::string& curModelsConfigJson,
                                     const std::string& curExtraEnvJson);

// 副本命名（§4.2）：base + " 副本"；重名递增 base + " 副本 N"（N 从 2 起）；
// 100 次仍冲突 → 空串（调用方按命名失败处理）
std::string makeCopyName(const std::string& baseName,
                         const std::vector<std::string>& existingNames);

// ---- failover 换家判据（2026-09-27 shared-config-and-failover 设计 §3.8）----
// 纯函数：两条转发路径（缓冲 / 流式）共用，避免 if 表漂移；单测逐条覆盖。
// 输入：上游返回的 statusCode（send 失败无响应 = 0）与 errorKind
//（"upstream_unreachable" = 传输失败；DevDesk 自产 503 = "no_active_profile"）。
// 判据表（§3.8，按 §3.1 新 503 口径重定；只换一跳，调用方负责次数）：
//   statusCode==0（传输失败：无响应头/DNS/TLS/连接/超时，我们这侧 =
//     502 upstream_unreachable）                 → true（换）
//   401 / 403（只有换 key 才可能变好）             → true
//   429（上游限流）                               → true
//   5xx 含 500/502/503/504/529（上游侧故障）       → true
//   400 / 405 / 406 / 413 / 414 / 415 / 422 / 501（请求自身非法，换家同结果
//      ——白烧一次请求）                          → false（不换）
//   其余 4xx（404 / 408 等，负列表之外都试；404 可能是「该网关没有这个
//     模型」，failover 恰好能救）                  → true
//   errorKind == "no_active_profile" → false（兜底防御：DevDesk 自身 503 由
//     writeNoProfileError 在转发**前**直接回（proxy_router 缓冲 :202 / 流式
//     :352），判定点理论收不到；本地配置错误不换家，单测注释锚定此语义）
bool shouldFailoverTo(int statusCode, const std::string& errorKind);

} // namespace module::claude
