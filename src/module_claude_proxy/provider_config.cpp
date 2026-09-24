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

// 导入键 → 槽位成员的映射表（§6.1 导入解析规则）
struct ImportSlotRef {
    const char* modelKey;             // 发往上游的模型名键
    const char* nameKey;              // 显示名键（nullptr = 该槽位无 _NAME 变体）
    ModelSlot ModelsConfig::*slot;
};

constexpr ImportSlotRef kImportSlots[] = {
    {"ANTHROPIC_MODEL",                 nullptr,                              &ModelsConfig::def},
    {"ANTHROPIC_DEFAULT_SONNET_MODEL",  "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME",  &ModelsConfig::sonnet},
    {"ANTHROPIC_DEFAULT_OPUS_MODEL",    "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME",    &ModelsConfig::opus},
    {"ANTHROPIC_DEFAULT_HAIKU_MODEL",   "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME",   &ModelsConfig::haiku},
    {"ANTHROPIC_DEFAULT_FABLE_MODEL",   "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME",   &ModelsConfig::fable},
    {"CLAUDE_CODE_SUBAGENT_MODEL",      nullptr,                              &ModelsConfig::subagent},
};

// 键名格式硬校验已于 2026-09-24 移除：env 块内的变量名一律可导入
//（用户要求——小写/连字符/数字开头等非标准键也收；「不在官方清单」
// 由表单侧红边框提示，不阻断导入）

bool isBlankText(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
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

core::Result<ImportedConfig> parseImportedEnv(const std::string& text) {
    ImportedConfig out;
    if (isBlankText(text)) {
        return core::Result<ImportedConfig>::fail(
            core::ErrorCode::ValidationError, "粘贴内容为空");
    }

    auto parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        return core::Result<ImportedConfig>::fail(parsed.errorCode(),
                                                  parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<ImportedConfig>::fail(
            core::ErrorCode::ValidationError,
            "顶层必须是 JSON 对象（整段 settings.json 或裸 env 对象）");
    }
    auto obj = parsed.value().asObject();

    // 两种形状：含 "env" 键 → 整段 settings.json（取 env 子对象）；
    // 否则视为裸 env 对象（所有值必须是字符串，下面统一校验）
    bool hasEnvKey = false;
    for (const auto& k : obj.keys()) {
        if (k == "env") { hasEnvKey = true; break; }
    }
    platform::json::JsonObject envObj;
    if (hasEnvKey) {
        auto envVal = obj.get("env");
        if (!envVal.isObject()) {
            return core::Result<ImportedConfig>::fail(
                core::ErrorCode::ValidationError,
                "settings.json 的 env 字段必须是 JSON 对象");
        }
        envObj = envVal.asObject();
    } else {
        envObj = obj;
    }

    // 键数上限（§7.4）
    if (envObj.keys().size() > 64) {
        return core::Result<ImportedConfig>::fail(
            core::ErrorCode::ValidationError,
            "env 键数超过上限 64: " + std::to_string(envObj.keys().size()));
    }

    std::map<std::string, std::string> leftover;   // 非基础/槽位键 → extraEnv 候选
    bool sawApiKey = false;
    bool sawAuthToken = false;

    for (const auto& key : envObj.keys()) {
        // 键名格式不设限（2026-09-24 放开，见上方注释）；仅保留空键防御
        if (key.empty()) {
            return core::Result<ImportedConfig>::fail(
                core::ErrorCode::ValidationError, "env 键名为空");
        }
        auto val = envObj.get(key);
        if (!val.isString()) {
            return core::Result<ImportedConfig>::fail(
                core::ErrorCode::ValidationError,
                "env 值必须是字符串: " + key);
        }
        const std::string v = val.asString();
        if (v.size() > 4096) {
            return core::Result<ImportedConfig>::fail(
                core::ErrorCode::ValidationError,
                "值超过 4KB 上限: " + key);
        }

        if (key == "ANTHROPIC_BASE_URL") {
            out.baseUrl = v;
            continue;
        }
        if (key == "ANTHROPIC_AUTH_TOKEN") {
            sawAuthToken = true;
            out.authToken = v;   // 同现时 AUTH_TOKEN 优先（API_KEY 不覆盖）
            continue;
        }
        if (key == "ANTHROPIC_API_KEY") {
            sawApiKey = true;
            if (!sawAuthToken) out.authToken = v;
            continue;
        }

        // 模型槽位键
        bool matchedSlot = false;
        for (const auto& s : kImportSlots) {
            if (key == s.modelKey) {
                (out.models.*s.slot).model = v;
                matchedSlot = true;
                break;
            }
            if (s.nameKey != nullptr && key == s.nameKey) {
                (out.models.*s.slot).name = v;
                matchedSlot = true;
                break;
            }
        }
        if (matchedSlot) continue;
        leftover[key] = v;
    }

    if (sawApiKey && sawAuthToken) {
        out.warnings.push_back(
            "同时含 ANTHROPIC_API_KEY 与 ANTHROPIC_AUTH_TOKEN，已采用 AUTH_TOKEN");
    }

    // 其余键走别名归一（§4.3 规则 6）：别名映射 + 未识别键警告
    auto aliased = applyEnvAliases(leftover);
    out.extraEnv = std::move(aliased.env);
    out.aliasMappedKeys = std::move(aliased.aliasMappedKeys);
    out.unrecognizedKeys = std::move(aliased.unrecognizedKeys);

    for (const auto& s : kSlots) {
        if (!(out.models.*s.slot).model.empty()) ++out.recognizedSlots;
    }
    return core::Result<ImportedConfig>::ok(std::move(out));
}

std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson, const std::string& legacyModel,
    const std::string& extraEnvJson,
    int proxyPort, const std::string& proxyToken) {
    std::map<std::string, std::string> out;

    // extra_env 先入表（别名归一），托管键随后写入 → 冲突时托管键优先（规则 5）
    auto extra = parseExtraEnv(extraEnvJson);
    if (extra.isOk()) {
        out = applyEnvAliases(extra.value()).env;
        // 非法 JSON：读路径容错，整体忽略（与 effectiveModelsConfig 语义一致）
    }

    out["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:" + std::to_string(proxyPort);
    if (!proxyToken.empty()) {
        out["ANTHROPIC_AUTH_TOKEN"] = proxyToken;
    }

    const ModelsConfig mc = effectiveModelsConfig(modelsConfigJson, legacyModel);
    // 槽位 → env 键（nameKey 为空表示该槽位无官方 _NAME 变体，不生成）
    struct SlotEnv {
        const ModelSlot* slot;
        const char* envKey;
        const char* nameKey;
    };
    const SlotEnv rows[] = {
        {&mc.def,      "ANTHROPIC_MODEL",                    nullptr},
        {&mc.sonnet,   "ANTHROPIC_DEFAULT_SONNET_MODEL",     "ANTHROPIC_DEFAULT_SONNET_MODEL_NAME"},
        {&mc.opus,     "ANTHROPIC_DEFAULT_OPUS_MODEL",       "ANTHROPIC_DEFAULT_OPUS_MODEL_NAME"},
        {&mc.haiku,    "ANTHROPIC_DEFAULT_HAIKU_MODEL",      "ANTHROPIC_DEFAULT_HAIKU_MODEL_NAME"},
        {&mc.fable,    "ANTHROPIC_DEFAULT_FABLE_MODEL",      "ANTHROPIC_DEFAULT_FABLE_MODEL_NAME"},
        {&mc.subagent, "CLAUDE_CODE_SUBAGENT_MODEL",         nullptr},
    };
    for (const auto& row : rows) {
        if (row.slot->model.empty()) continue;  // 空槽位跳过（规则 3）
        out[row.envKey] = row.slot->model;
        if (row.nameKey != nullptr) {
            out[row.nameKey] = row.slot->name.empty()
                ? deriveDisplayName(row.slot->model)  // 显示名派生（规则 4）
                : row.slot->name;
        }
    }
    return out;
}

} // namespace module::claude
