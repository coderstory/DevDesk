// src/module_claude_proxy/provider_config.cpp
// 供应商完整配置纯函数实现（platform::json）
#include "provider_config.h"

#include <algorithm>

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

    for (const auto& s : kSlots) {
        if (!(out.models.*s.slot).model.empty()) ++out.recognizedSlots;
    }
    return core::Result<ImportedConfig>::ok(std::move(out));
}

std::map<std::string, std::string> buildProxyEnv(
    const std::string& modelsConfigJson, const std::string& legacyModel,
    const std::string& extraEnvJson,
    int proxyPort, const std::string& sharedExtraEnvJson) {
    std::map<std::string, std::string> out;

    // extra_env 先入表（别名归一），托管键随后写入 → 冲突时托管键优先（规则 5）
    auto extra = parseExtraEnv(extraEnvJson);
    if (extra.isOk()) {
        out = applyEnvAliases(extra.value()).env;
        // 非法 JSON：读路径容错，整体忽略（与 effectiveModelsConfig 语义一致）
    }

    // 共享配置片段叠在 profile 自身 env 之上（片段赢，设计 §2.3 优先级 2 > 3）：
    // 解绑后同名键自动回落 profile 值（profile 行里的原值从未被删）；非法 JSON
    // 同样读路径容错整体忽略
    auto shared = parseExtraEnv(sharedExtraEnvJson);
    if (shared.isOk()) {
        for (const auto& [k, v] : applyEnvAliases(shared.value()).env) {
            out[k] = v;
        }
    }

    out["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:" + std::to_string(proxyPort);
    // 代理仅监听 127.0.0.1、无鉴权，此键只是让 Claude Code 有值可发（占位符，
    // 不是密钥；真实上游 key 由代理在转发时注入 profile 的 key）
    out["ANTHROPIC_AUTH_TOKEN"] = kProxyAuthPlaceholder;

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

// 6 槽位逐字段相等（model + name）
bool modelsConfigEqual(const ModelsConfig& a, const ModelsConfig& b) {
    for (const auto& s : kSlots) {
        const ModelSlot& x = a.*s.slot;
        const ModelSlot& y = b.*s.slot;
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
    const std::vector<std::string>& managedKeys,
    const std::vector<std::string>& ownedKeys) {
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

    // 第一遍：G1 托管键 / G10 绑定片段键 / G4 空值 / G6 非字符串或超长 → 跳过
    //（不写也不清）。G10 排在 G1 前面：片段键通常也在 sidecar 托管键并集里，两者
    // 处置相同（都是跳过），但先判 G10 才能给出「被片段冻结」这条更准确的警告
    std::map<std::string, std::string> valid;
    std::vector<std::string> frozenKeys;  // G10 命中（聚合成一条警告，不逐键刷屏）
    for (const auto& key : env.keys()) {
        if (containsKey(ownedKeys, key)) {  // G10：捕获目标所绑片段冻结的键
            markSkipped(key);
            frozenKeys.push_back(key);
            continue;
        }
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

    // G10 汇总一条警告（设计 §2.4：live 里片段键的改动不回流，且下次接管会被
    // 片段值覆盖回去——必须让用户看得见，否则观感是「我改了却没保存」）
    if (!frozenKeys.empty()) {
        std::string joined;
        for (const auto& k : frozenKeys) {
            if (!joined.empty()) joined += ", ";
            joined += k;
        }
        out.cfg.warnings.push_back(
            "键 " + joined + " 由共享配置片段冻结，settings.json 中的改动不回写");
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

    // 模型槽位 / *_NAME 槽位（映射表复用 kImportSlots）
    for (const auto& s : kImportSlots) {
        auto mit = valid.find(s.modelKey);
        if (mit != valid.end()) {
            (out.cfg.models.*s.slot).model = mit->second;
            valid.erase(mit);
        }
        if (s.nameKey == nullptr) continue;  // def / subagent 无 _NAME 变体
        auto nit = valid.find(s.nameKey);
        if (nit != valid.end()) {
            (out.cfg.models.*s.slot).name = nit->second;
            valid.erase(nit);
        }
    }

    // 其余键 → extra_env（别名归一复用 applyEnvAliases；未识别键照收）
    auto aliased = applyEnvAliases(valid);
    out.cfg.extraEnv = std::move(aliased.env);
    out.cfg.aliasMappedKeys = std::move(aliased.aliasMappedKeys);
    out.cfg.unrecognizedKeys = std::move(aliased.unrecognizedKeys);

    for (const auto& s : kSlots) {
        if (!(out.cfg.models.*s.slot).model.empty()) ++out.cfg.recognizedSlots;
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
                                     const std::string& curLegacyModel,
                                     const std::string& curExtraEnvJson) {
    ReverseSyncMerge out;
    const auto isKeep = [&cfg](const std::string& key) {
        return containsKey(cfg.skippedKeys, key);
    };

    // ---- models_config：以 effectiveModelsConfig(现值, 旧 model 列) 为基底 ----
    const ModelsConfig cur =
        effectiveModelsConfig(curModelsConfigJson, curLegacyModel);
    ModelsConfig merged = cur;
    for (const auto& s : kImportSlots) {
        if (!isKeep(s.modelKey)) {
            (merged.*s.slot).model = (cfg.models.*s.slot).model;  // 出现覆盖 / 缺席清空（G5）
        }
        // def / subagent 无官方 _NAME 变体 → name 不属托管字段，任何情况下不动
        if (s.nameKey != nullptr && !isKeep(s.nameKey)) {
            (merged.*s.slot).name = (cfg.models.*s.slot).name;
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

} // namespace module::claude
