// src/module_claude_proxy/provider_config.cpp
// 供应商完整配置纯函数实现（platform::json）
#include "provider_config.h"

#include <algorithm>
#include <optional>

#include "env_var_hints.h"
#include "platform/json.h"

namespace module::claude {

namespace {

// 槽位映射表的真源只有一个：provider_config.h 的 DEVDESK_MODEL_SLOT_LIST
//（审计 CPX-08 收敛）。此处只提供基于 modelSlotTable() 的派生判定，不再各自
// 维护 kSlots / kImportSlots / kSlotEnvRows 三张平行表（曾因三表不同步而静默漏键）。

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

// 「未识别键」判定（审计 CPX-09 + CPX-08）：官方清单（env_var_hints.h 全量）里
// 没有、也不是槽位表派生的托管键，才算未识别——原实现把 FABLE 两个自造键硬编码
// 在一处单独的判定函数里，现在由槽位表派生，加槽位不必再改这里
bool isUnrecognizedEnvKey(const std::string& key) {
    return envVarHint(key) == nullptr && !isModelSlotEnvKey(key);
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

const char* modelSlotDefEnvKey() {
    // 单一来源 = 槽位表：jsonKey=="default" 行的 envKey（F-10，不手抄键名）。
    // 槽位表恒有 default 行（DEVDESK_MODEL_SLOT_LIST 声明契约）；空串返回值
    // 仅为理论兜底（表被改坏时抑制步静默失效，不崩）
    for (const auto& m : modelSlotTable()) {
        if (std::string(m.jsonKey) == "default") return m.envKey;
    }
    return "";
}

bool isModelSlotEnvKey(const std::string& key) {
    // 槽位表托管的模型键（envKey 或 *_NAME 键）。原匿名命名空间 helper：F-10
    // 起提升为公开判定，claude_ui_detail 的 isModelMappingEnvKey 转发本函数，
    // 单一来源 = modelSlotTable()
    for (const auto& m : modelSlotTable()) {
        if (key == m.envKey) return true;
        if (m.nameKey != nullptr && key == m.nameKey) return true;
    }
    return false;
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

// ---- 高级设置 JSON：生效合并 / 键枚举 / 文档并入 / 校验（advanced-json-editor
// ---- 设计 §4.3；三条生成链共用，合并规则单一来源）----

namespace {

// 生效 JSON 的单侧解析（容错版，设计 §4.2 步骤 1）：空串 / 空白 → 未配置
//（nullopt，非警告）；超限 / 解析失败 / 顶层非对象 → 警告 + 按未配置处理。
// side = 警告文案里的来源名（"Profile 的" / "片段的"）
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

// "env" 顶层键的读侧兜底警告（D-5：防 DB 直写等非 UI 路径绕过校验后静默丢弃）
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

    // 逐键叠加：profile 先入、片段后入（后写者赢 = 片段赢，D-10 / §4.2 步骤 1）；
    // 解绑（片段为空）后同名键回落 profile 自身值——片段值从不写进 profile 行
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
    // pretty 规范形输出（D-2：parse→stringify(pretty) 同一条序列化通道；
    // 键序 = std::map 字典序）
    auto text = platform::json::stringify(
        platform::json::JsonValue(std::move(merged)), /*pretty=*/true);
    if (text.isFail()) return {};   // 序列化失败理论不可达；按空处理
    return text.value();
}

std::vector<std::string> extraSettingsTopLevelKeys(
    const std::string& extraSettingsJson) {
    std::vector<std::string> out;
    // best-effort 枚举（静默容错）：空串 / 超限 / 非法 / 顶层非对象 → 空集。
    // 用户可见的解析失败警告由 applyExtraSettingsToSettingsDoc 统一发，此处
    // 不重复（takeOver 的快照段先于 apply 步跑，双报会刷两条同文警告）
    if (isBlankText(extraSettingsJson) ||
        extraSettingsJson.size() > kMaxExtraSettingsBytes) {
        return out;
    }
    auto parsed = platform::json::parse(extraSettingsJson);
    if (parsed.isFail() || !parsed.value().isObject()) return out;
    auto obj = parsed.value().asObject();
    for (const auto& k : obj.keys()) {
        if (k == "env") continue;   // 永不写入（D-5）
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

    // 逐键并入（§4.2 步骤 2）：非 env 键赢过 live settings.json 既有同名键
    //（D-3：被覆盖旧值已在 sidecar 快照中，还原无损）
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

    // 模型键抑制（已拍板 D-4①；机制时序按审计 F-5：发生在合并写入之后的
    // apply 步，不是陈旧删除步）——生效 JSON 含非空字符串顶层 "model" → 从
    // root.env 移除 ANTHROPIC_MODEL（def 槽位 / extra_env / 别名归一来的同名
    // 键一并抑制），槽位 def 值失效。该键仍在 buildProxyEnv 产出与
    // managedKeys 并集里（还原语义不变）；若本轮 JSON 不含 "model"，该键回到
    // buildProxyEnv 生成、由既有陈旧删除逻辑自然撤走
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
    // 非法 JSON → 容错为全空槽位（读路径不炸）
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
            // 未识别键判定：官方键源自 env_var_hints.h 清单（审计 CPX-09），
            // 槽位托管键源自槽位表（CPX-08）——两处都不命中才标未识别
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

    // 整体文本上限（审计 2026-09-27 L6）：先于 parse 检查——超大输入不进
    // JSON 解析 / 深拷贝链（platform::json 按值 = 整树深拷贝）
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

        // 模型槽位键（槽位表派生）
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

    // 认证字段证据（设计 §5）：键名 = 该配置原本以哪种形态发送且可用
    if (sawApiKey && sawAuthToken) {
        out.authFieldFromEnv = "both";
    } else if (sawApiKey) {
        out.authFieldFromEnv = "api_key";
    } else if (sawAuthToken) {
        out.authFieldFromEnv = "bearer";
    }

    // 其余键走别名归一（§4.3 规则 6）：别名映射 + 未识别键警告
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

    // 来源标注（2026-09-28 详情面板 P3）：sourcesOut 非空时，每写一层顺手记
    // 「这个键最后被哪一层写入」——合并规则本身一个字不动，只是把「谁赢」这件
    // 事现出来。env 是扁平 map，结果里没有出处（见本文件头与 .h 的规则 6），
    // 所以这里用可选出参而不是另建一套 provenance 框架。
    if (sourcesOut != nullptr) sourcesOut->clear();
    auto markOne = [&](const std::string& k, EnvSource s) {
        if (sourcesOut != nullptr) (*sourcesOut)[k] = s;
    };
    auto markAll = [&](const std::map<std::string, std::string>& m, EnvSource s) {
        if (sourcesOut == nullptr) return;
        for (const auto& [k, v] : m) (*sourcesOut)[k] = s;
    };

    // extra_env 先入表（别名归一），托管键随后写入 → 冲突时托管键优先（规则 5）
    auto extra = parseExtraEnv(extraEnvJson);
    if (extra.isOk()) {
        out = applyEnvAliases(extra.value()).env;
        // 非法 JSON：读路径容错，整体忽略（与 effectiveModelsConfig 语义一致）
        markAll(out, EnvSource::Profile);
    }

    //（原「共享配置片段叠在 profile 自身 env 之上」段随 advanced-json-editor
    // §8.4 拆除：片段叠加走 JSON 域，env 域不再吃片段）

    out["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:" + std::to_string(proxyPort);
    markOne("ANTHROPIC_BASE_URL", EnvSource::Managed);
    // 代理仅监听 127.0.0.1、无鉴权，此键只是让 Claude Code 有值可发（占位符，
    // 不是密钥；真实上游 key 由代理在转发时注入 profile 的 key）
    out["ANTHROPIC_AUTH_TOKEN"] = kProxyAuthPlaceholder;
    markOne("ANTHROPIC_AUTH_TOKEN", EnvSource::Managed);

    const ModelsConfig mc = effectiveModelsConfig(modelsConfigJson);
    for (const auto& m : modelSlotTable()) {
        const ModelSlot& slot = mc.*m.slot;
        if (slot.model.empty()) continue;  // 空槽位跳过（规则 3）
        out[m.envKey] = slot.model;
        markOne(m.envKey, EnvSource::Managed);
        if (m.nameKey != nullptr) {
            out[m.nameKey] = slot.name.empty()
                ? deriveDisplayName(slot.model)  // 显示名派生（规则 4）
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

    // 与 buildProxyEnv 同款（profile-run 设计 §4.2 规则 5）：extra_env 先入表
    // （别名归一），托管键随后写入压过——导入数据可能带旧 BASE_URL / token，
    // 托管键必须最后落子。（原共享片段叠加段随 advanced-json-editor §8.4 拆除）
    auto extra = parseExtraEnv(extraEnvJson);
    if (extra.isOk()) {
        out = applyEnvAliases(extra.value()).env;
        // 非法 JSON：读路径容错，整体忽略（与 effectiveModelsConfig 语义一致）
    }

    // 托管键 1：BASE_URL 去尾斜杠（照 claude_service.cpp 的 stripTrailingSlash
    // 同款语义：只剥一个尾斜杠）。空串 → 不写（Claude Code 回落内置默认）。
    if (!baseUrl.empty()) {
        std::string url = baseUrl;
        if (url.back() == '/') url.pop_back();
        if (!url.empty()) out["ANTHROPIC_BASE_URL"] = url;
    }

    // 托管键 2：密钥键按认证字段三态只写一个（直连形态；见 provider_config.h
    // buildDirectEnv 注释的三态表）。密钥值为空串 → 不写（读 Keychain 成功但
    // 条目为空属于异常数据，宁可不写也不发空值）。
    // 写选中键的同时**清掉另一密钥键**：extra_env 可能带进反向的 token 键
    //（如默认 both 形态下 extra_env 里有 ANTHROPIC_API_KEY）——留着会让直连
    // 请求携带两个凭据，网关可能取到过期/错误的那一个（代理路径免疫：客户端
    // 鉴权头被代理剥掉，直连路径没有这层保护，必须单键）。advanced-json-editor
    // §8.4 起片段不再进 env 域，原文的「共享片段带进」字样随之删除
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

    // 模型槽位（与 buildProxyEnv 规则 3/4 同款）：空槽位跳过；name 空则派生显示名
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

// 托管键全集（审计 CPX-12 + CPX-08）：BASE_URL + 占位 token + 槽位表派生的
// 每行 envKey / *_NAME 键，与槽位是否有值无关（buildProxyEnv 只在槽位非空时
// 写槽位键，检测侧需要全集）
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

// ---- 反向同步（settings.json env → profile，设计 §3.2 映射表 / §3.3 守卫表）----

namespace {

// ASCII 小写（URL host 大小写不敏感）
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

// 槽位逐字段相等（model + name；槽位集合遍历槽位表）
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
    // 1. 剥 scheme（任意 "xxx://"）
    const auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    // 2. 截到 path / query / fragment 之前
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

    // 跳过键登记：原始键名 + 别名归一后的键名都要记（profile 的 extra_env 存
    // 的是归一后的键，只记原始键名会漏保护 DISABLE_AUTO_COMPACT 一类）
    auto markSkipped = [&out](const std::string& key) {
        out.cfg.skippedKeys.push_back(key);
        for (const auto& a : kEnvAliases) {
            if (key == a.alias) out.cfg.skippedKeys.push_back(a.official);
        }
    };
    // token 目标只有一个（Keychain 条目）：任一 token 键命中守卫都按「整个
    // token 字段不写也不清」登记
    auto markTokenSkipped = [&markSkipped]() {
        markSkipped("ANTHROPIC_AUTH_TOKEN");
        markSkipped("ANTHROPIC_API_KEY");
    };

    // 第一遍：G1 托管键 / G4 空值 / G6 非字符串或超长 → 跳过（不写也不清）。
    //（原 G10 绑定片段键分支随 advanced-json-editor §8.4 拆除——片段不再
    // 携带 env 键，守卫是永不触发的死通道）
    std::map<std::string, std::string> valid;
    for (const auto& key : env.keys()) {
        if (containsKey(managedKeys, key)) {  // G1
            markSkipped(key);
            continue;
        }
        auto val = env.get(key);
        if (!val.isString()) {  // G6
            out.cfg.warnings.push_back("env 值非字符串，已跳过: " + key);
            markSkipped(key);
            continue;
        }
        const std::string v = val.asString();
        if (v.size() > 4096) {  // G6
            out.cfg.warnings.push_back("env 值超过 4KB 上限，已跳过: " + key);
            markSkipped(key);
            continue;
        }
        if (v.empty()) {  // G4
            markSkipped(key);
            continue;
        }
        valid[key] = v;
    }

    // G2：loopback BASE_URL（本地代理形态）→ base_url 与本次 token 都不写也不清
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
    // G3 占位符 → 不写也不清（保持 profile 原密钥）
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
        } else if (auth != valid.end()) {  // AUTH_TOKEN 占位符 → G3
            markTokenSkipped();
        } else if (api != valid.end() && api->second != kProxyAuthPlaceholder) {
            out.cfg.authToken = api->second;
        } else if (api != valid.end()) {  // API_KEY 占位符 → G3
            markTokenSkipped();
        }
        valid.erase("ANTHROPIC_AUTH_TOKEN");
        valid.erase("ANTHROPIC_API_KEY");
    }

    // 模型槽位 / *_NAME 槽位（遍历槽位表派生）
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
            (merged.*m.slot).model = (cfg.models.*m.slot).model;  // 出现覆盖 / 缺席清空（G5）
        }
        // nameKey 为空的槽位无官方 _NAME 变体 → name 不属托管字段，任何情况下不动
        if (m.nameKey != nullptr && !isKeep(m.nameKey)) {
            (merged.*m.slot).name = (cfg.models.*m.slot).name;
        }
    }
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
        if (isKeep(k)) mergedExtra[k] = v;  // 守卫键保持原值（G1–G4/G6：不清）
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
    // 兜底防御（§3.8 末行）：DevDesk 自产 503 no_active_profile → 不换家
    //（本地配置错误，非上游故障；writeNoProfileError 在转发前直接回，
    // 判定点理论收不到这档）
    if (errorKind == "no_active_profile") return false;
    // 传输失败：无响应头 / DNS / TLS / 连接 / 超时（statusCode==0 /
    // 502 upstream_unreachable）
    if (statusCode == 0) return true;
    // 负列表（请求自身非法——换家同结果，白烧一次请求）
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
            // 401/403（换 key 才可能变好）/ 429（限流）/ 5xx（上游侧故障，
            // 含 500/502/503/504/529 过载）/ 其余 4xx（负列表之外都试）→ 换；
            // 2xx/3xx 到不了判定点（调用方只在失败时判），保守不换
            return statusCode >= 400 && statusCode < 600;
    }
}

} // namespace module::claude
