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

struct ModelsConfig {
#define DEVDESK_MODEL_SLOT_MEMBER(name, jsonKey, label, formLabel, envKey, nameKey, oneM) \
    ModelSlot name;
    DEVDESK_MODEL_SLOT_LIST(DEVDESK_MODEL_SLOT_MEMBER)
#undef DEVDESK_MODEL_SLOT_MEMBER
};

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

// 显示名派生：剥除尾部 [1M]/[200K]/[1m] 等上下文窗口标记
std::string deriveDisplayName(const std::string& model);

// def 槽位的托管 env 键名（单一来源）：从槽位表 jsonKey=="default" 行派生
const char* modelSlotDefEnvKey();

bool isModelSlotEnvKey(const std::string& key);

// 1M 上下文标记（模型名尾部 [1M] 后缀）：has 判尾（trimEnd 后大小写不敏感）；
// strip 命中才切尾再 trimEnd；set 先 strip，base 空返回空串。三者都不碰 *_NAME
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

// 粘贴导入整体文本上限（1MB）：挡在 JSON 解析之前，避免超大输入进入深拷贝链
//（platform::json 按值 = 整树深拷贝）。超限整体 fail（文案含上限）。
inline constexpr size_t kMaxImportTextBytes = 1u * 1024u * 1024u;


// 接管 env 生成：输出写入 settings.json 的 env 块（纯函数）：BASE_URL = 127.0.0.1:<port>、
// token = 占位符、槽位键（空槽位跳过）、extra_env 归一平铺被压过 ----

// 接管时写入 settings.json 的占位凭据（代理仅监听 127.0.0.1、无鉴权；
// 真实上游 key 由代理转发时注入）
inline constexpr const char* kProxyAuthPlaceholder = "PROXY_MANAGED";

// 生效 env 的来源分层，只为详情面板标注出处，不参与合并决策（合并顺序只在
// buildProxyEnv 一处）。优先级（高→低）：托管键 > 片段 > profile 自身
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

// 直连 env 生成
// 输入 = profile 字段 + 密钥明文（不碰存储）；输出 = 临时 settings.json 的 env（纯函数）。
// 与 buildProxyEnv 同款槽位 loop / 别名归一，差异：BASE_URL = baseUrl 去尾斜杠（空串不写）；
// 密钥按认证字段只写一个（extra_env 带进的另一密钥键清掉，防网关取到过期凭据）。
std::map<std::string, std::string> buildDirectEnv(
    const std::string& baseUrl, const std::string& apiKeyPlain,
    const std::string& authField,
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson);

// 托管键全集：BASE_URL + 占位 token + 槽位派生键（与槽位是否有值无关，检测侧需要全集）。
std::vector<std::string> managedEnvKeyNames();

// 反向同步（settings.json env → profile）
// 全部为纯函数（无 IO / DB / 日志）

// loopback URL 判定（本地代理形态）：剥 scheme → 取 host[:port]（IPv6 去方括号）
// → 小写后 ∈ {127.0.0.0/8 全段, localhost, ::1}。不做前缀匹配（如 127.0.0.1.evil.com 不算）
bool isLoopbackUrl(const std::string& url);

// 反向同步的容错版 env 解析：单个怪异值只跳过该键 + warning，不整体 fail。
// 守卫跳过：托管键 / loopback BASE_URL（连同 token）/ 占位 token / 空值 / 值非法
//（非字符串或 >4096）；键缺席 → 清空。cfg.skippedKeys = 「不写也不清」的键名集合。
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

// 反向同步第二段（出现/缺席语义）：出现 → 覆盖，缺席 → 清空，命中守卫 → 保持原值。
// def / subagent 的 name 无官方 _NAME 变体，任何情况下不动；以 effectiveModelsConfig
//（现值, 旧 model 列）为基底落位；only-changed：没变则 *Changed=false。
struct ReverseSyncMerge {
    std::string modelsConfigJson;
    std::string extraEnvJson;
    bool modelsChanged = false;
    bool extraEnvChanged = false;
};

ReverseSyncMerge mergeReverseSyncEnv(const ImportedConfig& cfg,
                                     const std::string& curModelsConfigJson,
                                     const std::string& curExtraEnvJson);

// 副本命名：base + " 副本"；重名递增 base + " 副本 N"（N 从 2 起）；
// 100 次仍冲突 → 空串（调用方按命名失败处理）
std::string makeCopyName(const std::string& baseName,
                         const std::vector<std::string>& existingNames);

// failover 换家判据
// 纯函数（缓冲 / 流式两路共用；输入 send 失败无响应 = 0；只判单次，次数调用方管）。
// 换：statusCode==0、401/403、429、5xx、其余 4xx；不换：400/405/406/413/414/
// 415/422/501（请求自身非法）；errorKind=="no_active_profile" 不换
bool shouldFailoverTo(int statusCode, const std::string& errorKind);

} // namespace module::claude
