// src/module_claude_proxy/provider_config.h
// Claude 供应商完整配置——数据模型纯函数层
// （spec docs/superpowers/specs/2026-09-22-claude-provider-config-design.md §4 + §4.3 规则 6）
//
// 设计要点：
//   - 全部为纯函数（platform::json，无 IO / 无 DB），单测直接覆盖
//   - Profile 里只存 JSON 原文（modelsConfigJson / extraEnvJson），
//     槽位解析/派生/兼容合并收敛到这里
//   - 空串 = 未配置（存量数据零迁移，读时兼容）
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/result.h"

namespace module::claude {

// 单个模型槽位：model = 发往上游的实际模型名（原样保存，含 [1M] 等标记）；
// name = Claude Code 界面显示名（可空，空则派生）
struct ModelSlot {
    std::string model;
    std::string name;
};

// 6 个模型槽位（§4.1）
struct ModelsConfig {
    ModelSlot def;      // "default"  → ANTHROPIC_MODEL
    ModelSlot sonnet;   // → ANTHROPIC_DEFAULT_SONNET_MODEL
    ModelSlot opus;     // → ANTHROPIC_DEFAULT_OPUS_MODEL
    ModelSlot haiku;    // → ANTHROPIC_DEFAULT_HAIKU_MODEL
    ModelSlot fable;    // → ANTHROPIC_DEFAULT_FABLE_MODEL
    ModelSlot subagent; // → CLAUDE_CODE_SUBAGENT_MODEL
};

// 显示名派生：剥除尾部 [1M]/[200K]/[1m] 等上下文窗口标记（§2.2）
std::string deriveDisplayName(const std::string& model);

// ---- models_config 序列化 / 反序列化 ----
// 序列化固定写全 6 槽位（空槽位写空串，roundtrip 稳定）
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

// 旧数据兼容读取（§4.2）：
//   - models_config 为空串 / 非法 JSON → 全空槽位（读路径容错，不炸）
//   - default 槽位为空且旧 model 列非空 → 旧值填入 default 槽位（fallback）
ModelsConfig effectiveModelsConfig(const std::string& modelsConfigJson,
                                   const std::string& legacyModel);

// ---- 非官方变量别名映射（§4.3 规则 6）----
struct EnvAliasMapping {
    // 映射后的完整 env：别名键已转换为官方键，其余键原样透传
    std::map<std::string, std::string> env;
    // 命中别名表的原始键（UI 提示「已映射为官方变量」）
    std::vector<std::string> aliasMappedKeys;
    // 别名表外、且不在官方已知键集合中的键（原样透传 + 警告角标，行为由用户自负）
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

// ---- 接管 env 生成（§4.3，Phase C 接管通道的输入）----
// 输入：profile 的三个数据源字段 + 代理端口；输出：要写入
// ~/.claude/settings.json env 块的托管键值（纯函数，无 IO）。
// 规则：
//   1. ANTHROPIC_BASE_URL = http://127.0.0.1:<port>（上游真实 key 永不出现在
//      settings.json，由代理注入）
//   2. ANTHROPIC_AUTH_TOKEN = kProxyAuthPlaceholder（占位符：代理无鉴权，
//      此键只是让 Claude Code 有值可发）
//   3. 模型槽位（effectiveModelsConfig 兼容旧 model 列）→ ANTHROPIC_MODEL /
//      ANTHROPIC_DEFAULT_{SONNET|OPUS|HAIKU|FABLE}_MODEL / CLAUDE_CODE_SUBAGENT_MODEL；
//      槽位空则跳过（Claude Code 回落内置映射）
//   4. 显示名 → 对应 *_MODEL_NAME（ANTHROPIC_MODEL / SUBAGENT 无官方 _NAME
//      变体，不生成）；name 空则 deriveDisplayName 派生
//   5. extra_env 经 parseExtraEnv（非法则整体忽略，读路径容错）+ applyEnvAliases
//      别名归一后平铺；与 1-4 冲突时 1-4 优先（导入数据可能带旧 BASE_URL/token）
//   6. sharedExtraEnvJson（profile 所绑共享配置片段的 extra_env 原文，空串 =
//      未绑定）叠在 profile 自身 extra_env **之上**（片段赢，设计 §2.3 优先级
//      2 > 3）：冻结 = 该键值由片段单点决定，profile 的同名值不生效（行里保留，
//      解绑后恢复生效）；片段键仍被 1-4 的托管键覆盖（优先级 1 最高）

// 接管时写入 settings.json 的占位凭据（代理仅监听 127.0.0.1、无鉴权；
// 真实上游 key 由代理转发时注入）
inline constexpr const char* kProxyAuthPlaceholder = "PROXY_MANAGED";

std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson, const std::string& legacyModel,
    const std::string& extraEnvJson,
    int proxyPort, const std::string& sharedExtraEnvJson = "");

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
//   G10 键 ∈ ownedKeys（捕获目标所绑共享配置片段的键集）→ 跳过该键 + 警告
//      ｜provider_config_guard_G10_shared_config_key
//      ownedKeys 语义 = 「该 profile 绑定的片段」的键集，不是「全部片段的并集」：
//      live 里的片段键恒由「绑定了该片段的那个 profile」的接管写入，切换时正好
//      收编回它自己，按绑定关系挡即可；取并集会让没绑该片段的 profile 无法收编
//      用户在 live 的改动（误伤面更大，设计 §2.4）
//      **G10 判在 G1 之前**（设计 §2.9 流程图把 G1 列在前）：takeOver 会把片段键
//      并入 sidecar managedKeys，故捕获时它们通常也命中 G1 —— 两者处置相同
//      （不写也不清），但只有先判 G10 才能给出「被片段冻结」这条更准确的警告
struct ReverseSyncParse {
    bool ok = false;
    std::string error;
    ImportedConfig cfg;
    // 命中守卫而跳过的键名（镜像 cfg.skippedKeys，便于调用方就近读取）
    std::vector<std::string> skippedKeys;
};

ReverseSyncParse parseEnvForReverseSync(
    const std::string& envJson,
    const std::vector<std::string>& managedKeys,
    const std::vector<std::string>& ownedKeys = {});

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
                                     const std::string& curLegacyModel,
                                     const std::string& curExtraEnvJson);

// 副本命名（§4.2）：base + " 副本"；重名递增 base + " 副本 N"（N 从 2 起）；
// 100 次仍冲突 → 空串（调用方按命名失败处理）
std::string makeCopyName(const std::string& baseName,
                         const std::vector<std::string>& existingNames);

} // namespace module::claude
