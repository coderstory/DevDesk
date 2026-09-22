// src/module_claude_proxy/provider_config.cpp
// 供应商完整配置纯函数实现（platform::json）
#include "provider_config.h"

#include "platform/json.h"

namespace module::claude {

namespace {

// 槽位 key ↔ ModelsConfig 成员 的映射表（§4.1 的 6 个槽位）
struct SlotRef {
    const char* key;
    ModelSlot ModelsConfig::*slot;
};

constexpr SlotRef kSlots[] = {
    {"default",  &ModelsConfig::def},
    {"sonnet",   &ModelsConfig::sonnet},
    {"opus",     &ModelsConfig::opus},
    {"haiku",    &ModelsConfig::haiku},
    {"fable",    &ModelsConfig::fable},
    {"subagent", &ModelsConfig::subagent},
};

// 非官方变量别名表（§4.3 规则 6）。
// 目前唯一条目：AUTO_COMPACT_ENABLED 非官方，官方关闭自动压缩用 DISABLE_AUTO_COMPACT
struct EnvAlias {
    const char* alias;
    const char* official;
    bool invertValue;   // 值取反：ALIAS=1(开) → OFFICIAL=0(不禁用)
};

constexpr EnvAlias kEnvAliases[] = {
    {"AUTO_COMPACT_ENABLED", "DISABLE_AUTO_COMPACT", true},
};

// 官方已知 env 键集合（§2.2 表 + 用户模板中的官方变量）。
// 不在此集合也不在别名表的键 → 原样透传 + 警告角标（unrecognizedKeys）。
// ponytail: 白名单按设计文档已知键收录，Phase D 导入 UI 如遇漏报再补
const std::map<std::string, std::string>& knownOfficialEnvKeys() {
    static const std::map<std::string, std::string> kKeys = {
        {"ANTHROPIC_API_KEY", ""},
        {"ANTHROPIC_AUTH_TOKEN", ""},
        {"ANTHROPIC_BASE_URL", ""},
        {"ANTHROPIC_MODEL", ""},
        {"ANTHROPIC_DEFAULT_SONNET_MODEL", ""},
        {"ANTHROPIC_DEFAULT_OPUS_MODEL", ""},
        {"ANTHROPIC_DEFAULT_HAIKU_MODEL", ""},
        {"ANTHROPIC_DEFAULT_FABLE_MODEL", ""},
        {"ANTHROPIC_DEFAULT_SONNET_MODEL_NAME", ""},
        {"ANTHROPIC_DEFAULT_OPUS_MODEL_NAME", ""},
        {"ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME", ""},
        {"ANTHROPIC_DEFAULT_FABLE_MODEL_NAME", ""},
        {"CLAUDE_CODE_SUBAGENT_MODEL", ""},
        {"DISABLE_AUTO_COMPACT", ""},
        {"DISABLE_AUTOUPDATER", ""},
        {"CLAUDE_CODE_AUTO_COMPACT_WINDOW", ""},
        {"CLAUDE_CODE_DISABLE_ADAPTIVE_THINKING", ""},
        {"CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS", ""},
    };
    return kKeys;
}

// 别名值取反：真值（开启自动压缩）→ DISABLE_AUTO_COMPACT="0"；
// 假值/空 → "1"。非空非假值一律按真处理（宁可不误禁用）
std::string invertedBoolValue(const std::string& v) {
    if (v.empty() || v == "0" || v == "false" || v == "off" || v == "OFF" ||
        v == "False" || v == "FALSE") {
        return "1";
    }
    return "0";
}

} // namespace

std::string deriveDisplayName(const std::string& model) {
    if (model.empty() || model.back() != ']') return model;
    const size_t pos = model.rfind('[');
    if (pos == std::string::npos || pos == 0) return model;
    return model.substr(0, pos);
}

core::Result<std::string> serializeModelsConfig(const ModelsConfig& config) {
    platform::json::JsonObject obj;
    for (const auto& s : kSlots) {
        platform::json::JsonObject slot;
        slot.set("model", platform::json::makeString((config.*s.slot).model));
        slot.set("name", platform::json::makeString((config.*s.slot).name));
        obj.set(s.key, platform::json::JsonValue(std::move(slot)));
    }
    return platform::json::stringify(platform::json::JsonValue(std::move(obj)));
}

core::Result<ModelsConfig> parseModelsConfig(const std::string& json) {
    ModelsConfig out;
    if (json.empty()) {
        return core::Result<ModelsConfig>::ok(std::move(out));  // 空串 = 未配置
    }
    auto parsed = platform::json::parse(json);
    if (parsed.isFail()) {
        return core::Result<ModelsConfig>::fail(parsed.errorCode(),
                                                parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<ModelsConfig>::fail(
            core::ErrorCode::ValidationError,
            "models_config 顶层必须是 JSON 对象");
    }
    auto obj = parsed.value().asObject();
    for (const auto& s : kSlots) {
        auto sv = obj.get(s.key);
        if (!sv.isObject()) continue;  // 槽位缺失/非对象 → 空（容错）
        auto so = sv.asObject();
        auto mv = so.get("model");
        auto nv = so.get("name");
        if (mv.isString()) (out.*s.slot).model = mv.asString();
        if (nv.isString()) (out.*s.slot).name = nv.asString();
    }
    return core::Result<ModelsConfig>::ok(std::move(out));
}

core::Result<std::string> serializeExtraEnv(
    const std::map<std::string, std::string>& env) {
    platform::json::JsonObject obj;
    for (const auto& [k, v] : env) {
        obj.set(k, platform::json::makeString(v));
    }
    return platform::json::stringify(platform::json::JsonValue(std::move(obj)));
}

core::Result<std::map<std::string, std::string>> parseExtraEnv(
    const std::string& json) {
    std::map<std::string, std::string> out;
    if (json.empty()) {
        return core::Result<std::map<std::string, std::string>>::ok(
            std::move(out));
    }
    auto parsed = platform::json::parse(json);
    if (parsed.isFail()) {
        return core::Result<std::map<std::string, std::string>>::fail(
            parsed.errorCode(), parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<std::map<std::string, std::string>>::fail(
            core::ErrorCode::ValidationError,
            "extra_env 顶层必须是 JSON 对象");
    }
    auto obj = parsed.value().asObject();
    for (const auto& key : obj.keys()) {
        auto val = obj.get(key);
        if (!val.isString()) {
            return core::Result<std::map<std::string, std::string>>::fail(
                core::ErrorCode::ValidationError,
                "extra_env 值必须是字符串: " + key);
        }
        out[key] = val.asString();
    }
    return core::Result<std::map<std::string, std::string>>::ok(std::move(out));
}

ModelsConfig effectiveModelsConfig(const std::string& modelsConfigJson,
                                   const std::string& legacyModel) {
    ModelsConfig out;
    auto parsed = parseModelsConfig(modelsConfigJson);
    if (parsed.isOk()) {
        out = parsed.value();
    }
    // 非法 JSON → 容错为全空槽位（读路径不炸，§4.2）
    if (out.def.model.empty() && !legacyModel.empty()) {
        out.def.model = legacyModel;  // 旧 model 列 fallback
    }
    return out;
}

EnvAliasMapping applyEnvAliases(const std::map<std::string, std::string>& env) {
    EnvAliasMapping out;
    for (const auto& [key, value] : env) {
        const EnvAlias* hit = nullptr;
        for (const auto& a : kEnvAliases) {
            if (key == a.alias) {
                hit = &a;
                break;
            }
        }
        if (hit) {
            out.env[hit->official] =
                hit->invertValue ? invertedBoolValue(value) : value;
            out.aliasMappedKeys.push_back(key);
        } else {
            out.env[key] = value;
            if (knownOfficialEnvKeys().find(key) == knownOfficialEnvKeys().end()) {
                out.unrecognizedKeys.push_back(key);
            }
        }
    }
    return out;
}

} // namespace module::claude
