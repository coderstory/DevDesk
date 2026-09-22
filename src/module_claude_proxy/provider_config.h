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

// ---- 接管 env 生成（§4.3，Phase C 接管通道的输入）----
// 输入：profile 的三个数据源字段 + 代理端口/token；输出：要写入
// ~/.claude/settings.json env 块的托管键值（纯函数，无 IO）。
// 规则：
//   1. ANTHROPIC_BASE_URL = http://127.0.0.1:<port>（上游真实 key 永不出现在
//      settings.json，由代理注入）
//   2. ANTHROPIC_AUTH_TOKEN = 代理 token（token 为空则跳过该键）
//   3. 模型槽位（effectiveModelsConfig 兼容旧 model 列）→ ANTHROPIC_MODEL /
//      ANTHROPIC_DEFAULT_{SONNET|OPUS|HAIKU|FABLE}_MODEL / CLAUDE_CODE_SUBAGENT_MODEL；
//      槽位空则跳过（Claude Code 回落内置映射）
//   4. 显示名 → 对应 *_MODEL_NAME（ANTHROPIC_MODEL / SUBAGENT 无官方 _NAME
//      变体，不生成）；name 空则 deriveDisplayName 派生
//   5. extra_env 经 parseExtraEnv（非法则整体忽略，读路径容错）+ applyEnvAliases
//      别名归一后平铺；与 1-4 冲突时 1-4 优先（导入数据可能带旧 BASE_URL/token）
std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson, const std::string& legacyModel,
    const std::string& extraEnvJson,
    int proxyPort, const std::string& proxyToken);

} // namespace module::claude
