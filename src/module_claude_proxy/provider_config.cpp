// src/module_claude_proxy/provider_config.cpp
// 供应商完整配置纯函数实现（platform::json）
#include "provider_config.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "env_var_hints.h"
#include "platform/json.h"

namespace module::claude {

namespace {

// 槽位映射表真源 = provider_config.h 的 DEVDESK_MODEL_SLOT_LIST，此处只提供
// 基于 modelSlotTable() 的派生判定，不维护平行表

// 非官方变量别名表：AUTO_COMPACT_ENABLED → 官方关闭自动压缩键 DISABLE_AUTO_COMPACT
struct EnvAlias {
    const char* alias;
    const char* official;
    bool invertValue;   // 值取反：ALIAS=1(开) → OFFICIAL=0(不禁用)
};

constexpr EnvAlias kEnvAliases[] = {
    {"AUTO_COMPACT_ENABLED", "DISABLE_AUTO_COMPACT", true},
};

// 官方清单没有、也不是槽位表托管键，才算未识别（由槽位表派生，加槽位不必改这里）
bool isUnrecognizedEnvKey(const std::string& key) {
    return envVarHint(key) == nullptr && !isModelSlotEnvKey(key);
}

// 别名值取反：真值 → "0"（不禁用），假值/空 → "1"；非空非假值一律按真处理
std::string invertedBoolValue(const std::string& v) {
    if (v.empty() || v == "0" || v == "false" || v == "off" || v == "OFF" ||
        v == "False" || v == "FALSE") {
        return "1";
    }
    return "0";
}

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

const char* modelSlotDefEnvKey() {
    // 单一来源 = 槽位表 default 行的 envKey；空串返回 = 表被改坏时的理论兜底
    for (const auto& m : modelSlotTable()) {
        if (std::string(m.jsonKey) == "default") return m.envKey;
    }
    return "";
}

bool isModelSlotEnvKey(const std::string& key) {
    for (const auto& m : modelSlotTable()) {
        if (key == m.envKey) return true;
        if (m.nameKey != nullptr && key == m.nameKey) return true;
    }
    return false;
}

// 1M 上下文标记：语义对齐 cc-switch useModelState.ts（见头文件）；零正则零 locale
namespace {

// 尾部空白裁剪等价 JS trimEnd：不用 locale 相关 std::isspace 避免全角空格歧义
std::string trimEndCopy(const std::string& text) {
    size_t end = text.size();
    while (end > 0) {
        const char c = text[end - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        --end;
    }
    return text.substr(0, end);
}

} // namespace

bool hasOneMMarker(const std::string& model) {
    const std::string trimmed = trimEndCopy(model);
    constexpr size_t kMarkerLen = 4;   // "[1M]" 的字节数（与大小写无关）
    if (trimmed.size() < kMarkerLen) return false;
    const std::string tail = trimmed.substr(trimmed.size() - kMarkerLen);
    return tail == "[1M]" || tail == "[1m]";
}

std::string stripOneMMarker(const std::string& model) {
    if (!hasOneMMarker(model)) return model;   // 未命中原样返回（含尾空白）
    const std::string trimmed = trimEndCopy(model);
    return trimEndCopy(trimmed.substr(0, trimmed.size() - 4));
}

std::string setOneMMarker(const std::string& model, bool enabled) {
    const std::string base = stripOneMMarker(model);
    // base 的空判定照 cc-switch：strip 后再 trim()（两侧空白都不算内容）
    size_t begin = 0;
    size_t end = base.size();
    while (begin < end && (base[begin] == ' ' || base[begin] == '\t' ||
                           base[begin] == '\r' || base[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (base[end - 1] == ' ' || base[end - 1] == '\t' ||
                           base[end - 1] == '\r' || base[end - 1] == '\n')) {
        --end;
    }
    const std::string core = base.substr(begin, end - begin);
    if (core.empty()) return "";   // 空 base 不许带标记（cc-switch 同款）
    return enabled ? core + kOneMMarkerSuffix : core;
}

void normalizeOneMMarkers(ModelsConfig& config) {
    for (const auto& m : modelSlotTable()) {
        ModelSlot& slot = config.*m.slot;
        if (!hasOneMMarker(slot.model)) continue;   // 无标记 → 原样（含空槽位）
        const std::string base = stripOneMMarker(slot.model);
        slot.model = m.supportsOneM ? (base + kOneMMarkerSuffix)   // 支持 → 规范化大小写
                                    : base;                  // 不支持 → 强制剥离
    }
}

core::Result<std::string> serializeModelsConfig(const ModelsConfig& config) {
    platform::json::JsonObject obj;
    for (const auto& m : modelSlotTable()) {
        platform::json::JsonObject slot;
        slot.set("model", platform::json::makeString((config.*m.slot).model));
        slot.set("name", platform::json::makeString((config.*m.slot).name));
        obj.set(m.jsonKey, platform::json::JsonValue(std::move(slot)));
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
    for (const auto& m : modelSlotTable()) {
        auto sv = obj.get(m.jsonKey);
        if (!sv.isObject()) continue;  // 槽位缺失/非对象 → 空（容错）
        auto so = sv.asObject();
        auto mv = so.get("model");
        auto nv = so.get("name");
        if (mv.isString()) (out.*m.slot).model = mv.asString();
        if (nv.isString()) (out.*m.slot).name = nv.asString();
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

// ---- 高级设置 JSON：生效合并 / 键枚举 / 文档并入 / 校验 ----

namespace {

// 生效 JSON 的单侧解析（容错）：空/空白 → 未配置；超限/失败/非对象 →
// 警告 + 按未配置处理。side = 警告文案里的来源名
std::optional<platform::json::JsonObject> parseExtraSettingsSide(
    const std::string& text, const char* side,
    std::vector<std::string>* warnings) {
    if (isBlankText(text)) return std::nullopt;
    if (text.size() > kMaxExtraSettingsBytes) {
        if (warnings != nullptr) {
            warnings->push_back(std::string(side) +
                "高级设置 JSON 超过 1MB 上限，按未配置处理");
        }
        return std::nullopt;
    }
    auto parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        if (warnings != nullptr) {
            warnings->push_back(std::string(side) +
                "高级设置 JSON 解析失败，按未配置处理: " + parsed.errorMessage());
        }
        return std::nullopt;
    }
    if (!parsed.value().isObject()) {
        if (warnings != nullptr) {
            warnings->push_back(std::string(side) +
                "高级设置 JSON 顶层必须是 JSON 对象，按未配置处理");
        }
        return std::nullopt;
    }
    return parsed.value().asObject();
}

// "env" 顶层键的读侧兜底警告（防 DB 直写等非 UI 路径绕过校验后静默丢弃）
void warnEnvKeySkipped(const char* side, std::vector<std::string>* warnings) {
    if (warnings != nullptr) {
        warnings->push_back(std::string(side) +
            "高级设置 JSON 含顶层 \"env\" 键，已跳过（env 请用模型映射 / "
            "高级 env 页配置）");
    }
}

} // namespace

std::string effectiveExtraSettingsJson(const std::string& profileJson,
                                       const std::string& fragmentJson,
                                       std::vector<std::string>* warningsOut) {
    if (warningsOut != nullptr) warningsOut->clear();
    auto profileObj = parseExtraSettingsSide(profileJson, "Profile 的", warningsOut);
    auto fragmentObj = parseExtraSettingsSide(fragmentJson, "片段的", warningsOut);
    if (!profileObj && !fragmentObj) return {};   // 双空 → 空串（含双未配置）

    // 逐键叠加：profile 先入、片段后入（后写者赢）；片段值从不写进 profile 行
    platform::json::JsonObject merged;
    auto addSide = [&](const std::optional<platform::json::JsonObject>& obj,
                       const char* side) {
        if (!obj) return;
        for (const auto& k : obj->keys()) {
            if (k == "env") {
                warnEnvKeySkipped(side, warningsOut);
                continue;
            }
            merged.set(k, obj->get(k));
        }
    };
    addSide(profileObj, "Profile 的");
    addSide(fragmentObj, "片段的");

    if (merged.keys().empty()) return {};
    // pretty 规范形输出（键序 = std::map 字典序）
    auto text = platform::json::stringify(
        platform::json::JsonValue(std::move(merged)), /*pretty=*/true);
    if (text.isFail()) return {};   // 序列化失败理论不可达；按空处理
    return text.value();
}

// settings.json 原文 → 非 env 顶层属性 JSON（pretty）：输入上限同 kMaxImportTextBytes。
std::string nonEnvTopLevelJson(const std::string& settingsDocText) {
    if (settingsDocText.empty() ||
        settingsDocText.size() > kMaxImportTextBytes) {
        return {};
    }
    auto parsed = platform::json::parse(settingsDocText);
    if (parsed.isFail() || !parsed.value().isObject()) return {};
    auto obj = parsed.value().asObject();
    platform::json::JsonObject out;
    for (const auto& k : obj.keys()) {
        if (k == "env") continue;   // env 属 env 域（基础/槽位/env 页）
        out.set(k, obj.get(k));
    }
    if (out.keys().empty()) return {};
    auto text = platform::json::stringify(
        platform::json::JsonValue(std::move(out)), /*pretty=*/true);
    if (text.isFail()) return {};
    if (text.value().size() > kMaxExtraSettingsBytes) return {};   // 超限 = 未配置
    return text.value();
}

std::vector<std::string> extraSettingsTopLevelKeys(
    const std::string& extraSettingsJson) {
    std::vector<std::string> out;
    // best-effort 枚举：解析失败警告由 applyExtraSettingsToSettingsDoc 统一发，防双报
    if (isBlankText(extraSettingsJson) ||
        extraSettingsJson.size() > kMaxExtraSettingsBytes) {
        return out;
    }
    auto parsed = platform::json::parse(extraSettingsJson);
    if (parsed.isFail() || !parsed.value().isObject()) return out;
    auto obj = parsed.value().asObject();
    for (const auto& k : obj.keys()) {
        if (k == "env") continue;   // 永不写入
        out.push_back(k);
    }
    return out;
}

ExtraSettingsApply applyExtraSettingsToSettingsDoc(
    platform::json::JsonObject& root, const std::string& extraSettingsJson) {
    ExtraSettingsApply out;
    if (isBlankText(extraSettingsJson)) return out;   // 空 = 无 JSON，零写入
    if (extraSettingsJson.size() > kMaxExtraSettingsBytes) {
        out.warnings.push_back(
            "高级设置 JSON 超过 1MB 上限，本轮忽略（不并入 settings.json）");
        return out;
    }
    auto parsed = platform::json::parse(extraSettingsJson);
    if (parsed.isFail()) {
        out.warnings.push_back("高级设置 JSON 解析失败，本轮忽略: " +
                               parsed.errorMessage());
        return out;
    }
    if (!parsed.value().isObject()) {
        out.warnings.push_back("高级设置 JSON 顶层必须是 JSON 对象，本轮忽略");
        return out;
    }
    auto obj = parsed.value().asObject();

    // 逐键并入：非 env 键赢过 live settings.json 既有同名键
    //（被覆盖旧值已在 sidecar 快照中，还原无损）
    bool writeModel = false;
    for (const auto& k : obj.keys()) {
        if (k == "env") {
            warnEnvKeySkipped("生效", &out.warnings);
            continue;
        }
        root.set(k, obj.get(k));
        out.writtenKeys.push_back(k);
        if (k == "model") {
            const auto mv = obj.get(k);
            if (mv.isString() && !mv.asString().empty()) writeModel = true;
        }
    }

    // 模型键抑制：顶层 "model" 非空字符串 → 从 root.env 移除 ANTHROPIC_MODEL（def 槽位值失效）；本轮无 "model" 时由陈旧删除逻辑撤走
    if (writeModel) {
        out.modelSuppressed = true;
        if (root.has("env") && root.get("env").isObject()) {
            auto envObj = root.get("env").asObject();
            const std::string defKey = modelSlotDefEnvKey();
            if (!defKey.empty() && envObj.has(defKey)) {
                platform::json::JsonObject rebuilt;
                for (const auto& k : envObj.keys()) {
                    if (k == defKey) continue;
                    rebuilt.set(k, envObj.get(k));
                }
                root.set("env", platform::json::JsonValue(std::move(rebuilt)));
            }
        }
        // root.env 缺失 / 非对象（调用方未按前置准备）→ 只标记不抑制（不炸）
    }

    std::sort(out.writtenKeys.begin(), out.writtenKeys.end());
    out.writtenKeys.erase(
        std::unique(out.writtenKeys.begin(), out.writtenKeys.end()),
        out.writtenKeys.end());
    return out;
}

core::Result<void> validateExtraSettingsJson(const std::string& text) {
    if (isBlankText(text)) return core::Result<void>::ok();   // 空串 = 未配置
    if (text.size() > kMaxExtraSettingsBytes) {
        return core::Result<void>::fail(
            core::ErrorCode::ValidationError,
            "高级设置 JSON 超过 1MB 上限: " + std::to_string(text.size()) +
                " 字节");
    }
    auto parsed = platform::json::parse(text);
    if (parsed.isFail()) {
        return core::Result<void>::fail(parsed.errorCode(),
            "高级设置 JSON 解析失败: " + parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
            "高级设置 JSON 顶层必须是 JSON 对象");
    }
    auto obj = parsed.value().asObject();
    for (const auto& k : obj.keys()) {
        if (k == "env") {
            return core::Result<void>::fail(core::ErrorCode::ValidationError,
                "高级设置 JSON 不能包含 env 键（env 请用模型映射 / 高级 env 页配置）");
        }
    }
    return core::Result<void>::ok();
}

ModelsConfig effectiveModelsConfig(const std::string& modelsConfigJson) {
    ModelsConfig out;
    auto parsed = parseModelsConfig(modelsConfigJson);
    if (parsed.isOk()) {
        out = parsed.value();
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
            if (isUnrecognizedEnvKey(key)) {
                out.unrecognizedKeys.push_back(key);
            }
        }
    }
    return out;
}

AuthField authFieldFromText(const std::string& text) {
    if (text == "bearer") return AuthField::Bearer;
    if (text == "api_key") return AuthField::ApiKey;
    return AuthField::Both;   // 'both' / 未知 / 空串
}

const char* authFieldToText(AuthField field) {
    switch (field) {
        case AuthField::Bearer: return "bearer";
        case AuthField::ApiKey: return "api_key";
        case AuthField::Both: break;
    }
    return "both";
}

core::Result<ImportedConfig> parseImportedEnv(const std::string& text) {
    ImportedConfig out;
    if (isBlankText(text)) {
        return core::Result<ImportedConfig>::fail(
            core::ErrorCode::ValidationError, "粘贴内容为空");
    }

    // 先于 parse 检查：超大输入不进 JSON 解析 / 深拷贝链
    if (text.size() > kMaxImportTextBytes) {
        return core::Result<ImportedConfig>::fail(
            core::ErrorCode::ValidationError,
            "导入文本超过 1MB 上限: " + std::to_string(text.size()) + " 字节");
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

    // 两种形状：含 "env" 键 = 整段 settings.json（取 env 子对象），否则裸 env 对象
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

    if (envObj.keys().size() > 64) {
        return core::Result<ImportedConfig>::fail(
            core::ErrorCode::ValidationError,
            "env 键数超过上限 64: " + std::to_string(envObj.keys().size()));
    }

    std::map<std::string, std::string> leftover;   // 非基础/槽位键 → extraEnv 候选
    bool sawApiKey = false;
    bool sawAuthToken = false;

    for (const auto& key : envObj.keys()) {
        // 键名格式不设限（表单侧红边框提示，不阻断）；仅保留空键防御
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

        bool matchedSlot = false;
        for (const auto& m : modelSlotTable()) {
            if (key == m.envKey) {
                (out.models.*m.slot).model = v;
                matchedSlot = true;
                break;
            }
            if (m.nameKey != nullptr && key == m.nameKey) {
                (out.models.*m.slot).name = v;
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

    // 认证字段证据：键名 = 该配置原本以哪种形态发送且可用
    if (sawApiKey && sawAuthToken) {
        out.authFieldFromEnv = "both";
    } else if (sawApiKey) {
        out.authFieldFromEnv = "api_key";
    } else if (sawAuthToken) {
        out.authFieldFromEnv = "bearer";
    }

    auto aliased = applyEnvAliases(leftover);
    out.extraEnv = std::move(aliased.env);
    out.aliasMappedKeys = std::move(aliased.aliasMappedKeys);
    out.unrecognizedKeys = std::move(aliased.unrecognizedKeys);

    for (const auto& m : modelSlotTable()) {
        if (!(out.models.*m.slot).model.empty()) ++out.recognizedSlots;
    }
    return core::Result<ImportedConfig>::ok(std::move(out));
}

std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson,
    int proxyPort,
    std::map<std::string, EnvSource>* sourcesOut) {
    std::map<std::string, std::string> out;

    // 来源标注：sourcesOut 非空时记每个键最后被哪层写入（env 是扁平 map 无出处）。
    if (sourcesOut != nullptr) sourcesOut->clear();
    auto markOne = [&](const std::string& k, EnvSource s) {
        if (sourcesOut != nullptr) (*sourcesOut)[k] = s;
    };
    auto markAll = [&](const std::map<std::string, std::string>& m, EnvSource s) {
        if (sourcesOut == nullptr) return;
        for (const auto& [k, v] : m) (*sourcesOut)[k] = s;
    };

    // extra_env 先入表（别名归一），托管键随后写入 → 冲突时托管键优先
    auto extra = parseExtraEnv(extraEnvJson);
    if (extra.isOk()) {
        out = applyEnvAliases(extra.value()).env;
        // 非法 JSON：读路径容错，整体忽略（与 effectiveModelsConfig 语义一致）
        markAll(out, EnvSource::Profile);
    }

    out["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:" + std::to_string(proxyPort);
    markOne("ANTHROPIC_BASE_URL", EnvSource::Managed);
    // 占位符不是密钥；真实上游 key 由代理转发时注入
    out["ANTHROPIC_AUTH_TOKEN"] = kProxyAuthPlaceholder;
    markOne("ANTHROPIC_AUTH_TOKEN", EnvSource::Managed);

    const ModelsConfig mc = effectiveModelsConfig(modelsConfigJson);
    for (const auto& m : modelSlotTable()) {
        const ModelSlot& slot = mc.*m.slot;
        if (slot.model.empty()) continue;  // 空槽位跳过
        out[m.envKey] = slot.model;
        markOne(m.envKey, EnvSource::Managed);
        if (m.nameKey != nullptr) {
            out[m.nameKey] = slot.name.empty()
                ? deriveDisplayName(slot.model)  // 显示名派生
                : slot.name;
            markOne(m.nameKey, EnvSource::Managed);
        }
    }
    return out;
}

std::map<std::string, std::string> buildDirectEnv(
    const std::string& baseUrl, const std::string& apiKeyPlain,
    const std::string& authField,
    const std::string& modelsConfigJson,
    const std::string& extraEnvJson) {
    std::map<std::string, std::string> out;

    // extra_env 先入表（别名归一），托管键最后写入压过（导入数据可能带旧键）。
    auto extra = parseExtraEnv(extraEnvJson);
    if (extra.isOk()) {
        out = applyEnvAliases(extra.value()).env;
        // 非法 JSON：读路径容错，整体忽略（与 effectiveModelsConfig 语义一致）
    }

    // BASE_URL 只剥一个尾斜杠；空串不写（回落内置默认）。
    if (!baseUrl.empty()) {
        std::string url = baseUrl;
        if (url.back() == '/') url.pop_back();
        if (!url.empty()) out["ANTHROPIC_BASE_URL"] = url;
    }

    // 密钥键只写一个并清掉另一密钥键：extra_env 可能带进反向 token 键，直连携带两个凭据时网关可能取到过期的那个（代理路径免疫）。密钥空串 → 不写
    const AuthField field = authFieldFromText(authField);
    const char* tokenKey = (field == AuthField::ApiKey)
        ? "ANTHROPIC_API_KEY"
        : "ANTHROPIC_AUTH_TOKEN";
    const char* otherTokenKey = (field == AuthField::ApiKey)
        ? "ANTHROPIC_AUTH_TOKEN"
        : "ANTHROPIC_API_KEY";
    if (!apiKeyPlain.empty()) {
        out[tokenKey] = apiKeyPlain;
        out.erase(otherTokenKey);
    }

    const ModelsConfig mc = effectiveModelsConfig(modelsConfigJson);
    for (const auto& m : modelSlotTable()) {
        const ModelSlot& slot = mc.*m.slot;
        if (slot.model.empty()) continue;  // 空槽位跳过
        out[m.envKey] = slot.model;
        if (m.nameKey != nullptr) {
            out[m.nameKey] = slot.name.empty()
                ? deriveDisplayName(slot.model)
                : slot.name;
        }
    }
    return out;
}

// 托管键全集与槽位是否有值无关（buildProxyEnv 只在槽位非空时写键，检测侧需要全集）
std::vector<std::string> managedEnvKeyNames() {
    std::vector<std::string> out;
    out.reserve(2 + 2 * kModelSlotCount);  // 2 个固定键 + 每行至多 2 个槽位键
    out.push_back("ANTHROPIC_BASE_URL");
    out.push_back("ANTHROPIC_AUTH_TOKEN");
    for (const auto& m : modelSlotTable()) {
        out.push_back(m.envKey);
        if (m.nameKey != nullptr) out.push_back(m.nameKey);
    }
    return out;
}

// ---- 反向同步（settings.json env → profile）----

namespace {

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 严格点分四段十进制（每段 ≤ 255）且首段为 127 —— "127.0.0.1.evil.com" /
// "127.0.0.1evil" / 短写 "127.1" 都不算（不做前缀匹配）
bool isLoopbackIpv4(const std::string& host) {
    int seg = 0;
    int value = 0;
    bool hasDigit = false;
    for (size_t i = 0; i <= host.size(); ++i) {
        const bool atEnd = (i == host.size());
        if (!atEnd && host[i] != '.') {
            const char c = host[i];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
            if (value > 255) return false;
            hasDigit = true;
            continue;
        }
        if (!hasDigit) return false;  // 空段：连续点 / 首点 / 尾点
        if (seg == 0 && value != 127) return false;
        ++seg;
        if (seg > 4) return false;
        value = 0;
        hasDigit = false;
        if (atEnd) break;
    }
    return seg == 4;
}

bool modelsConfigEqual(const ModelsConfig& a, const ModelsConfig& b) {
    for (const auto& m : modelSlotTable()) {
        const ModelSlot& x = a.*m.slot;
        const ModelSlot& y = b.*m.slot;
        if (x.model != y.model || x.name != y.name) return false;
    }
    return true;
}

bool containsKey(const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

} // namespace

bool isLoopbackUrl(const std::string& url) {
    std::string s = url;
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    const auto tail = s.find_first_of("/?#");
    if (tail != std::string::npos) s = s.substr(0, tail);
    if (s.empty()) return false;

    // 3. host[:port]（IPv6 去方括号；无方括号的多冒号形式按整个 host 处理）
    std::string host;
    if (s.front() == '[') {
        const auto close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(1, close - 1);
    } else {
        const auto first = s.find(':');
        const bool singleColon =
            first != std::string::npos && first == s.rfind(':');
        host = singleColon ? s.substr(0, first) : s;
    }

    host = toLowerAscii(host);
    if (host == "localhost" || host == "::1") return true;
    return isLoopbackIpv4(host);
}

ReverseSyncParse parseEnvForReverseSync(
    const std::string& envJson,
    const std::vector<std::string>& managedKeys) {
    ReverseSyncParse out;
    if (envJson.empty()) {  // 空 = 空 env 对象（无真相源，读侧已按 no-op 拦下）
        out.ok = true;
        return out;
    }
    auto parsed = platform::json::parse(envJson);
    if (parsed.isFail()) {
        out.error = "env JSON 解析失败: " + parsed.errorMessage();
        return out;
    }
    if (!parsed.value().isObject()) {
        out.error = "env 必须是 JSON 对象";
        return out;
    }
    auto env = parsed.value().asObject();

    // 跳过键登记：原始键名 + 别名归一后的键名都要记（只记原始键名会漏保护）
    auto markSkipped = [&out](const std::string& key) {
        out.cfg.skippedKeys.push_back(key);
        for (const auto& a : kEnvAliases) {
            if (key == a.alias) out.cfg.skippedKeys.push_back(a.official);
        }
    };
    // token 目标只有一个：任一 token 键命中守卫都按「整个 token 字段不写也不清」登记
    auto markTokenSkipped = [&markSkipped]() {
        markSkipped("ANTHROPIC_AUTH_TOKEN");
        markSkipped("ANTHROPIC_API_KEY");
    };

    // 第一遍：托管键 / 空值 / 非字符串或超长 → 跳过（不写也不清）
    std::map<std::string, std::string> valid;
    for (const auto& key : env.keys()) {
        if (containsKey(managedKeys, key)) {
            markSkipped(key);
            continue;
        }
        auto val = env.get(key);
        if (!val.isString()) {
            out.cfg.warnings.push_back("env 值非字符串，已跳过: " + key);
            markSkipped(key);
            continue;
        }
        const std::string v = val.asString();
        if (v.size() > 4096) {
            out.cfg.warnings.push_back("env 值超过 4KB 上限，已跳过: " + key);
            markSkipped(key);
            continue;
        }
        if (v.empty()) {
            markSkipped(key);
            continue;
        }
        valid[key] = v;
    }

    // loopback BASE_URL（本地代理形态）→ base_url 与本次 token 都不写也不清
    bool loopbackBaseUrl = false;
    {
        auto it = valid.find("ANTHROPIC_BASE_URL");
        if (it != valid.end() && isLoopbackUrl(it->second)) {
            loopbackBaseUrl = true;
        }
    }
    if (loopbackBaseUrl) {
        valid.erase("ANTHROPIC_BASE_URL");
        valid.erase("ANTHROPIC_AUTH_TOKEN");
        valid.erase("ANTHROPIC_API_KEY");
        markSkipped("ANTHROPIC_BASE_URL");
        markTokenSkipped();
    } else {
        auto it = valid.find("ANTHROPIC_BASE_URL");
        if (it != valid.end()) {
            out.cfg.baseUrl = it->second;  // 尾斜杠清洗与 updateProfile 同款，交上层
            valid.erase(it);
        }
    }

    // token：AUTH_TOKEN 优先，其次 API_KEY（同现 → AUTH_TOKEN + warning）；
    // 占位符 → 不写也不清（保持 profile 原密钥）
    {
        auto auth = valid.find("ANTHROPIC_AUTH_TOKEN");
        auto api = valid.find("ANTHROPIC_API_KEY");
        if (auth != valid.end() && auth->second != kProxyAuthPlaceholder) {
            out.cfg.authToken = auth->second;
            if (api != valid.end()) {
                out.cfg.warnings.push_back(
                    "同时含 ANTHROPIC_API_KEY 与 ANTHROPIC_AUTH_TOKEN，"
                    "已采用 AUTH_TOKEN");
            }
        } else if (auth != valid.end()) {  // AUTH_TOKEN 占位符
            markTokenSkipped();
        } else if (api != valid.end() && api->second != kProxyAuthPlaceholder) {
            out.cfg.authToken = api->second;
        } else if (api != valid.end()) {  // API_KEY 占位符
            markTokenSkipped();
        }
        valid.erase("ANTHROPIC_AUTH_TOKEN");
        valid.erase("ANTHROPIC_API_KEY");
    }

    for (const auto& m : modelSlotTable()) {
        auto mit = valid.find(m.envKey);
        if (mit != valid.end()) {
            (out.cfg.models.*m.slot).model = mit->second;
            valid.erase(mit);
        }
        if (m.nameKey == nullptr) continue;  // 该槽位无 _NAME 变体
        auto nit = valid.find(m.nameKey);
        if (nit != valid.end()) {
            (out.cfg.models.*m.slot).name = nit->second;
            valid.erase(nit);
        }
    }

    // 其余键 → extra_env（别名归一复用 applyEnvAliases；未识别键照收）
    auto aliased = applyEnvAliases(valid);
    out.cfg.extraEnv = std::move(aliased.env);
    out.cfg.aliasMappedKeys = std::move(aliased.aliasMappedKeys);
    out.cfg.unrecognizedKeys = std::move(aliased.unrecognizedKeys);

    for (const auto& m : modelSlotTable()) {
        if (!(out.cfg.models.*m.slot).model.empty()) ++out.cfg.recognizedSlots;
    }
    std::sort(out.cfg.skippedKeys.begin(), out.cfg.skippedKeys.end());
    out.cfg.skippedKeys.erase(
        std::unique(out.cfg.skippedKeys.begin(), out.cfg.skippedKeys.end()),
        out.cfg.skippedKeys.end());
    out.skippedKeys = out.cfg.skippedKeys;

    out.ok = true;
    return out;
}

ReverseSyncMerge mergeReverseSyncEnv(const ImportedConfig& cfg,
                                     const std::string& curModelsConfigJson,
                                     const std::string& curExtraEnvJson) {
    ReverseSyncMerge out;
    const auto isKeep = [&cfg](const std::string& key) {
        return containsKey(cfg.skippedKeys, key);
    };

    // ---- models_config：以 effectiveModelsConfig(现值) 为基底 ----
    const ModelsConfig cur =
        effectiveModelsConfig(curModelsConfigJson);
    ModelsConfig merged = cur;
    for (const auto& m : modelSlotTable()) {
        if (!isKeep(m.envKey)) {
            (merged.*m.slot).model = (cfg.models.*m.slot).model;  // 出现覆盖 / 缺席清空
        }
        // nameKey 为空的槽位无官方 _NAME 变体 → name 不属托管字段，任何情况下不动
        if (m.nameKey != nullptr && !isKeep(m.nameKey)) {
            (merged.*m.slot).name = (cfg.models.*m.slot).name;
        }
    }
    // 反向同步走同一套 1M 归一：手写 [1M] 到不支持槽位时不剥会落库与开关不一致；放在比较前，唯一差异是本就该剥的标记时不产生无意义写库（幂等）
    normalizeOneMMarkers(merged);

    out.modelsConfigJson = curModelsConfigJson;
    if (!modelsConfigEqual(merged, cur)) {
        auto text = serializeModelsConfig(merged);
        if (text.isOk()) {
            out.modelsConfigJson = text.value();
            out.modelsChanged = true;
        }
        // 序列化失败（理论不可达）→ 保持现值不写，避免半写
    }

    // ---- extra_env：键集合完全由 settings.json 决定（出现覆盖 / 缺席删除）----
    std::map<std::string, std::string> curExtra;
    auto parsedCur = parseExtraEnv(curExtraEnvJson);
    if (parsedCur.isOk()) curExtra = parsedCur.value();  // 非法 JSON → 容错为空表

    std::map<std::string, std::string> mergedExtra;
    for (const auto& [k, v] : cfg.extraEnv) {
        if (!isKeep(k)) mergedExtra[k] = v;  // 出现覆盖 / 新增（守卫键不写）
    }
    for (const auto& [k, v] : curExtra) {
        if (isKeep(k)) mergedExtra[k] = v;  // 守卫键保持原值（不清）
    }
    out.extraEnvJson = curExtraEnvJson;
    if (mergedExtra != curExtra) {
        if (mergedExtra.empty()) {
            out.extraEnvJson.clear();  // 空表 = 未配置（本层空串约定）
            out.extraEnvChanged = true;
        } else {
            auto text = serializeExtraEnv(mergedExtra);
            if (text.isOk()) {
                out.extraEnvJson = text.value();
                out.extraEnvChanged = true;
            }
        }
    }
    return out;
}

std::string makeCopyName(const std::string& baseName,
                         const std::vector<std::string>& existingNames) {
    for (int i = 1; i <= 100; ++i) {
        std::string candidate = baseName + " 副本";
        if (i > 1) candidate += " " + std::to_string(i);
        if (!containsKey(existingNames, candidate)) return candidate;
    }
    return {};  // 100 次仍冲突：调用方按命名失败处理
}

bool shouldFailoverTo(int statusCode, const std::string& errorKind) {
    // DevDesk 自产 503 no_active_profile → 不换家（本地配置错误非上游故障；
    // writeNoProfileError 转发前直接回，判定点理论收不到）
    if (errorKind == "no_active_profile") return false;
    // 传输失败（无响应头 / DNS / TLS / 连接 / 超时）
    if (statusCode == 0) return true;
    // 负列表：请求自身非法，换家同结果白烧一次请求
    switch (statusCode) {
        case 400:
        case 405:
        case 406:
        case 413:
        case 414:
        case 415:
        case 422:
        case 501:
            return false;
        default:
            // 401/403 / 429 / 5xx / 其余 4xx → 换；2xx/3xx 到不了判定点，保守不换
            return statusCode >= 400 && statusCode < 600;
    }
}

} // namespace module::claude
