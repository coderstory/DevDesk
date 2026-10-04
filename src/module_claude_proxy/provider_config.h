// Claude 供应商配置数据模型纯函数层（platform::json，无 IO/DB）。
// Profile 只存 JSON 原文，槽位解析/派生/兼容合并收敛于此；空串 = 未配置（读路径容错）。
#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"
#include "platform/json.h"

namespace module::claude {

// 单个模型槽位：model = 发往上游的模型名（原样保存，含 [1M] 等标记）；name = CC 界面显示名（空则派生）
struct ModelSlot {
    std::string model;
    std::string name;
};

// 槽位单一真相源表在 src 根中立头 model_slots.h（与 module_agent 共用；两模块不可互 include）
#include "model_slots.h"

// 模型槽位：成员由槽位表生成，声明顺序 = 表顺序
struct ModelsConfig {
#define DEVDESK_MODEL_SLOT_MEMBER(name, jsonKey, label, formLabel, envKey, nameKey, oneM) \
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
    // supportsOneM：该槽位是否允许串尾带 [1M]（非「当前是否已带」，当前勾态另由 hasOneMMarker 判）
    bool supportsOneM;
};

// 槽位表（顺序 = UI 展示顺序 = managedEnvKeyNames 的槽位键顺序）；消费方一律遍历本表派生
#define DEVDESK_MODEL_SLOT_ROW(name, jsonKey, label, formLabel, envKey, nameKey, oneM) \
    ModelSlotMeta{jsonKey, label, formLabel, envKey, nameKey, &ModelsConfig::name, oneM},
inline constexpr ModelSlotMeta kModelSlotMetas[] = {
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_ROW)
};
#undef DEVDESK_MODEL_SLOT_ROW

// 槽位数（由表派生，加一行即 +1）
#define DEVDESK_MODEL_SLOT_COUNT_ONE(name, jsonKey, label, formLabel, envKey, nameKey, oneM) +1
inline constexpr std::size_t kModelSlotCount =
    0 DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_COUNT_ONE);
#undef DEVDESK_MODEL_SLOT_COUNT_ONE

// 编译期守卫：ModelsConfig 成员数必须 == 表行数（成员全为 ModelSlot、无填充）
static_assert(sizeof(ModelsConfig) == kModelSlotCount * sizeof(ModelSlot),
              "新增/删除模型槽位后请同步 DEVDESK_MODEL_SLOT_LIST："
              "ModelsConfig 成员数必须与槽位表行数一致");

inline std::span<const ModelSlotMeta> modelSlotTable() { return kModelSlotMetas; }

// 显示名派生：剥除尾部 [1M]/[200K]/[1m] 等上下文窗口标记（§2.2）
std::string deriveDisplayName(const std::string& model);

// def 槽位的托管 env 键名（单一来源）：从槽位表 jsonKey=="default" 行派生
const char* modelSlotDefEnvKey();

// 键是否属于槽位表托管的模型键（envKey 或 *_NAME）；claude_ui_detail 的 isModelMappingEnvKey 转发本判定
bool isModelSlotEnvKey(const std::string& key);

// 1M 上下文标记（存储上是模型名尾部 [1M] 后缀；照 cc-switch 参考实现）：
// hasOneMMarker 判尾（trimEnd 后大小写不敏感）；stripOneMMarker 命中才切尾再
// trimEnd；setOneMMarker 先 strip，base 空返回空串。三者都不碰 *_NAME
inline constexpr const char* kOneMMarkerSuffix = "[1M]";
bool hasOneMMarker(const std::string& model);
std::string stripOneMMarker(const std::string& model);
std::string setOneMMarker(const std::string& model, bool enabled);

// 保存前归一（幂等；UI 三条写入口都需调用）：支持 1M 且有标记→规范成 [1M]，
// 不支持且有标记→强制剥离，无标记/空→原样；序列化保持无损
void normalizeOneMMarkers(ModelsConfig& config);

// models_config 序列化：固定写全槽位表所有行（空槽位写空串，roundtrip 稳定）
core::Result<std::string> serializeModelsConfig(const ModelsConfig& config);

// 容错语义：空串→ok 全空槽位；顶层非对象/解析失败→fail；槽位缺失或非对象→该槽位空；
// model/name 非字符串→空。
core::Result<ModelsConfig> parseModelsConfig(const std::string& json);

// extra_env 序列化/反序列化（值一律字符串）；读：空串→ok 空表，顶层非对象或值非字符串→fail
core::Result<std::string> serializeExtraEnv(
    const std::map<std::string, std::string>& env);

core::Result<std::map<std::string, std::string>> parseExtraEnv(
    const std::string& json);

// 认证字段（鉴权头三态）：both=双发 x-api-key+Authorization Bearer（默认）；bearer=只发 Bearer；api_key=只发 x-api-key
enum class AuthField { Both, Bearer, ApiKey };

// 文本 → 枚举：'bearer'/'api_key'/其余（含 'both'、未知、空串）→ Both（读侧兜底）
AuthField authFieldFromText(const std::string& text);

// 枚举 → 文本：规范值（写侧规范化用，库中只有三个规范值之一）
const char* authFieldToText(AuthField field);

// models_config 读入槽位：空串/非法 JSON → 全空槽位（读路径容错，不炸）
ModelsConfig effectiveModelsConfig(const std::string& modelsConfigJson);

struct EnvAliasMapping {
    // 映射后的完整 env：别名键已转为官方键，其余键原样透传
    std::map<std::string, std::string> env;
    // 命中别名表的原始键（UI 提示「已映射为官方变量」）
    std::vector<std::string> aliasMappedKeys;
    // 别名表外且不在官方键清单中的键（原样透传 + 警告角标，行为由用户自负）
    std::vector<std::string> unrecognizedKeys;
};

EnvAliasMapping applyEnvAliases(const std::map<std::string, std::string>& env);

// cc-switch/settings.json 粘贴导入：输入原文（整段 settings.json 取其 env 子对象，或裸 env 对象），纯函数无 IO
struct ImportedConfig {
    std::string baseUrl;    // ANTHROPIC_BASE_URL（未出现 = 空串，不覆盖表单）
    std::string authToken;  // ANTHROPIC_AUTH_TOKEN / ANTHROPIC_API_KEY（同现取 AUTH_TOKEN 并给 warning）
    ModelsConfig models;    // *_MODEL / *_MODEL_NAME / SUBAGENT → 槽位
    std::map<std::string, std::string> extraEnv;  // 其余键（别名归一后）
    std::vector<std::string> aliasMappedKeys;     // 「已映射为官方变量」提示
    std::vector<std::string> unrecognizedKeys;    // 警告角标（透传，行为自负）
    std::vector<std::string> warnings;            // 其他警告（API_KEY/AUTH_TOKEN 同现等）
    int recognizedSlots = 0;  // 预览用：model 非空的槽位数
    // 粘贴里两个键名的证据：'api_key' / 'bearer' / 'both'（同现）/ ""（都没出现 = 无证据，UI 不覆盖表单现值）
    std::string authFieldFromEnv;
    // 反向同步专用：命中守卫而跳过的键名——「不写也不清」。mergeReverseSyncEnv
    // 靠它区分「键缺席（清空）」与「跳过（保持 profile 原值）」；parseImportedEnv 恒为空
    std::vector<std::string> skippedKeys;
};

// 校验：env 值必须字符串（数字/布尔拒绝）；键名 ^[A-Z][A-Z0-9_]*$；值 ≤4KB/键；键数 ≤64；
// 整体文本 ≤ kMaxImportTextBytes（1MB）。非法输入整体 fail，不部分导入。
core::Result<ImportedConfig> parseImportedEnv(const std::string& text);

// 粘贴导入整体文本上限（1MB）：挡在 JSON 解析之前，避免超大输入进入深拷贝链（platform::json 按值=整树深拷贝）。
// 合法载荷约 64 键 × 4KB ≈ 256KB，1MB 留 ~4x 余量。超限整体 fail（文案含上限）。
inline constexpr size_t kMaxImportTextBytes = 1u * 1024u * 1024u;

// 高级设置 JSON（settings.json 除 env 外全部顶层属性）的合并/校验纯函数（无 IO/DB/日志）。
// 三条生成链（takeOver/runProfile/复制配置导出）共用，合并规则只此一处。

// 整体文本上限：同 kMaxImportTextBytes（platform::json 按值深拷贝，超大输入挡在解析前）
inline constexpr size_t kMaxExtraSettingsBytes = 1u * 1024u * 1024u;

// 生效 JSON：片段逐键叠加在 profile JSON 之上（片段赢，同 buildProxyEnv 规则 6 方向）；解绑后同名键回落 profile。
// 任一侧 "env" 键跳过并记 warnings；一侧非法/超限/非对象按空处理，另一侧照用；两侧空→空串。输出 pretty 字典序。
std::string effectiveExtraSettingsJson(
    const std::string& profileJson,
    const std::string& fragmentJson,
    std::vector<std::string>* warningsOut = nullptr);

// 生效 JSON 本轮将写入的顶层键（best-effort）：非法/超限/非对象→空集；"env" 不在列（它永不写入）
std::vector<std::string> extraSettingsTopLevelKeys(
    const std::string& extraSettingsJson);

// settings.json 原文 → 高级设置 JSON（除 env 外全部顶层属性，pretty）。
// parseImportedEnv 只取 env，非 env 顶层属性必须在此独立抽取，否则导入后高级配置恒空。
// 空串/超限/解析失败/非对象/除 env 外无键 → 空串（= 未配置）。
std::string nonEnvTopLevelJson(const std::string& settingsDocText);

// 并入 settings 文档的应用结果：writtenKeys=本轮写入的非 env 顶层键（排序去重）；
// modelSuppressed=顶层 "model" 触发 ANTHROPIC_MODEL 抑制；warnings=调用方记账。
struct ExtraSettingsApply {
    std::vector<std::string> writtenKeys;   // 本轮写入的非 env 顶层键（排序去重）
    bool modelSuppressed = false;           // 顶层 "model" 触发 ANTHROPIC_MODEL 抑制
    std::vector<std::string> warnings;      // "env" 键跳过 / 解析失败等（调用方记账）
};

// 并入 settings 文档：对生效 JSON 每个顶层键 K，K=="env" 跳过+WARN，其余 root[K]=值（赢过既有同名键）。
// "model" 为非空字符串时同时从 root["env"] 移除 ANTHROPIC_MODEL（抑制唯一落点，在合并写入后的 apply 步）。
// root["env"] 缺失/非对象时抑制步跳过。extraSettingsJson 空串/非法/超限/非对象 → warnings 记录且零写入。
ExtraSettingsApply applyExtraSettingsToSettingsDoc(
    platform::json::JsonObject& root,
    const std::string& extraSettingsJson);

// 应用/保存校验（UI 与服务共用）：空串 OK（= 未配置）；文本 ≤ kMaxExtraSettingsBytes；必须解析成功；
// 顶层必须是对象且不含 "env"。失败带中文错误（表单 formError_ / 片段弹窗红字直出）。
core::Result<void> validateExtraSettingsJson(const std::string& text);

// ---- 接管 env 生成 ----
// 输入 profile 的数据源字段 + 代理端口，输出要写入 ~/.claude/settings.json
// env 块的托管键值（纯函数，无 IO）。规则：
//   1. ANTHROPIC_BASE_URL = http://127.0.0.1:<port>（上游真实 key 永不出现在
//      settings.json，由代理注入）
//   2. ANTHROPIC_AUTH_TOKEN = kProxyAuthPlaceholder（占位：代理无鉴权，此键
//      只是让 Claude Code 有值可发）
//   3. 模型槽位 → 槽位表每行的 envKey；槽位空则跳过（Claude Code 回落内置映射）
//   4. 显示名 → 对应行的 *_MODEL_NAME 键（nameKey 为 nullptr 的槽位不生成）；
//      name 空则 deriveDisplayName 派生
//   5. extra_env 经 parseExtraEnv（非法则整体忽略）+ applyEnvAliases 归一后
//      平铺；与 1-4 冲突时 1-4 优先

// 接管时写入 settings.json 的占位凭据（代理仅监听 127.0.0.1、无鉴权；
// 真实上游 key 由代理转发时注入）
inline constexpr const char* kProxyAuthPlaceholder = "PROXY_MANAGED";

// 生效 env 的来源分层——**只为详情面板标注出处**，不参与合并决策（合并顺序
// 写在 buildProxyEnv 一处）。优先级（高→低）：托管键 > 片段 > profile 自身，
// 即写入顺序 profile → 片段 → 托管键，后写者赢。env 是扁平 map 合并后没有
// 出处，标注只能让合并方顺手记一层
enum class EnvSource {
    Profile = 0,  // profile 自身 extra_env（含别名归一）
    Shared,       // 该 profile 所绑共享配置片段
    Managed,      // DevDesk 托管键（BASE_URL / 占位 token / 模型槽位表派生键）
};

std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson,
    int proxyPort,
    // 可选出参：每个键的来源（见 EnvSource）。非空则先清空再填；nullptr = 不记录
    std::map<std::string, EnvSource>* sourcesOut = nullptr);

// ---- 直连 env 生成 ----
// 输入 = profile 数据字段 + 已读出的密钥明文（**本函数不碰存储**，读失败由
// 调用方在调用前中止）；输出 = 临时 settings.json 的 env 子对象（纯函数）。
// 与 buildProxyEnv 同款的槽位 loop / 别名归一，仅托管键不同：
//   1. ANTHROPIC_BASE_URL = baseUrl 去尾斜杠；空串 → 不写该键（回落内置默认）
//   2. 密钥键按认证字段三态**只写一个**（直连路径的唯一凭据——代理会剥客户端
//      鉴权头，直连没有这层保护，extra_env 带进的另一密钥键会被清掉，防两个
//      凭据并存时网关取到过期的一个）：bearer → ANTHROPIC_AUTH_TOKEN；
//      api_key → ANTHROPIC_API_KEY；both → ANTHROPIC_AUTH_TOKEN（默认）。
//      密钥值空串 → 不写该键
//   3. 模型槽位：空槽位跳过；*_NAME 派生显示名
//   4. extra_env 先入表；托管键（BASE_URL 与密钥键）最后写入压过它们
std::map<std::string, std::string> buildDirectEnv(
    const std::string& baseUrl, const std::string& apiKeyPlain,
    const std::string& authField,
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson);

// 托管键全集：BASE_URL + 占位 token + 槽位表派生的 env 键与 *_NAME 键。
// 与槽位是否有值无关（槽位空时 buildProxyEnv 不生成该键，但上层同名键仍会
// 压过接管，检测侧需要全集）。消费方：settings_sync 的上层覆盖检测
std::vector<std::string> managedEnvKeyNames();

// ---- 反向同步（settings.json env → profile）----
// 全部为纯函数（无 IO / DB / 日志），单测直接覆盖

// loopback URL 判定（G2「本地代理形态」）：剥 scheme → 取 host[:port]（IPv6 去
// 方括号）→ 小写后 ∈ {127.0.0.0/8 全段, localhost, ::1}。不做前缀匹配
//（http://127.0.0.1.evil.com / http://127.0.0.1evil.com 都不算）
bool isLoopbackUrl(const std::string& url);

// 反向同步的**容错版** env 解析：与 parseImportedEnv 的「非法即整体 fail」
// 不同——单个怪异值只跳过该键 + warning（严格契约会让一个无关怪异值阻断用户
// 切 profile）。输入 = settings.json 的 env 对象原文；cfg 复用 ImportedConfig，
// 其 skippedKeys 是「不写也不清」的键名集合（含别名归一后的键名）。仅 env
// 本身非法（非 JSON 对象）才 ok=false。
// 守卫表（每条守卫一个专测，编号与用例名一一对应，增键/改守卫时同批更新）：
//   G1 托管键 → 跳过该键   G2 loopback BASE_URL → base_url 与 token 都跳过
//   G3 占位 token → 跳过   G4 空值 → 跳过   G5 键缺席 → 清空对应值
//   G6 值非法（非字符串 / >4096）→ 跳过 + WARN，不整体 fail
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

// 反向同步的第二段（出现/缺席语义）：把 cfg 对齐到 profile 现值上。
//   - 出现（cfg 非空）→ 覆盖；缺席 → 清空；命中守卫（skippedKeys）→ 保持原值
//   - def / subagent 的 name 无官方 _NAME 变体（不属托管字段）→ 任何情况下不动
//   - 以 effectiveModelsConfig(现值, 旧 model 列) 为基底落位
//   - only-changed：与现值逐字段比较，没变则 *Changed=false 且输出回传入参原文
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

// ---- failover 换家判据 ----
// 纯函数：两条转发路径（缓冲 / 流式）共用，避免 if 表漂移；单测逐条覆盖。
// 输入：上游 statusCode（send 失败无响应 = 0）与 errorKind。
// 判据（只换一跳，调用方负责次数）：
//   换：statusCode==0（传输失败）、401/403（只有换 key 才可能变好）、429（限流）、
//       5xx（上游侧故障）、其余 4xx（404 可能是「该网关没有这个模型」，恰能救）
//   不换：400/405/406/413/414/415/422/501（请求自身非法，换家同结果，白烧请求）
//   errorKind=="no_active_profile" → 不换（兜底防御：DevDesk 自产 503 由
//     writeNoProfileError 在转发前直接回，判定点理论收不到；本地配置错误不换家）
bool shouldFailoverTo(int statusCode, const std::string& errorKind);

} // namespace module::claude
