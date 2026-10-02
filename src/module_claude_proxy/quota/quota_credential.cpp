// src/module_claude_proxy/quota/quota_credential.cpp
// 默认凭据来源实现：读本 profile 自己的 api_key_encrypted（E1 已落库）+ 配置表。
#include "quota_credential.h"

#include <map>
#include <mutex>

#include "module_claude_proxy/profile_repository.h"
#include "platform/http_client.h"
#include "platform/json.h"
#include "quota_store.h"
#include "ccswitch_source.h"

namespace module::claude::quota {

namespace {

namespace pj = platform::json;

// {"accessKeyId":"AK","secretAccessKey":"SK"} → map。坏 JSON / 非对象 →
// **空 map**（容错：配置写坏了不该让整次查询崩成 Shape，让 provider 自己去
// 撞它对字段的校验，出的错才是用户看得懂的那种）。
//
// **非字符串值的口径（评审 m5）：丢，不报错。** extra/secrets 里的数字 /
// 布尔 / 嵌套对象一律不进 map（`fields` 的契约是 map<string,string>，
// ProviderConfig::fields 也是这么声明的）。理由：这些字段的消费者是 provider
// 自己——它会对缺失的必填字段给出「配置不完整，去补全」这种用户看得懂的
// 错误；若在这里把数字 stringify 塞进去，provider 收到的是一个**看着合法、
// 实则类型不对**的值，报错会指向错误的方向。丢 = 少一个字段 = provider
// 说「这个字段没填」，这是对的。
std::map<std::string, std::string> flatMapFromJson(const std::string& json) {
    std::map<std::string, std::string> out;
    if (json.empty()) return out;
    auto parsed = pj::parse(json);
    if (parsed.isFail() || !parsed.value().isObject()) return out;
    pj::JsonObject obj = parsed.value().asObject();
    for (const std::string& key : obj.keys()) {
        pj::JsonValue v = obj.get(key);
        if (v.isString()) out[key] = v.asString();
    }
    return out;
}

QuotaResult notConfigured(const std::string& message) {
    QuotaResult r;
    r.error = QuotaError::NotConfigured;
    r.errorMessage = message;
    return r;
}

}  // namespace

QuotaResult ProfileCredentialSource::resolveAndFetch(int64_t profileId,
                                                      const QuotaProvider& provider,
                                                      const HttpSendFn& http) {
    // 配置行：provider_id 空 = 用户没配 → NotConfigured（不是 None：那是「配了
    // 但读不到密钥」共用的失败类，G7 据此引导「去补全」）。
    auto cfg = store_.loadConfig(profileId);
    if (cfg.isFail()) {
        return notConfigured("未配置余量查询");
    }
    // 「有配置行」不等于「配了供应商」：provider_id 空串才是未配置（schema 里
    // 这列 DEFAULT ''，G6 的「不配置」选项就写空串）。漏这一判会拿着一个没选
    // 供应商的 profile 去 fetch，出来的错误文案答非所问。
    if (cfg.value().providerId.empty()) {
        return notConfigured("未配置余量查询");
    }

    auto profile = profiles_.getProfile(profileId);
    if (profile.isFail()) {
        return notConfigured("读不到该 profile");
    }

    // 凭据：api_key_encrypted 经 platform::credential 解密（E1 已在
    // ProfileRepository::loadApiKey 内做掉，这里只消费明文，永不落日志）。
    auto key = profiles_.loadApiKey(profileId);
    if (key.isFail() || key.value().empty()) {
        return notConfigured("该 profile 没有可用密钥");
    }

    ProviderConfig pcfg;
    pcfg.providerId = cfg.value().providerId;
    pcfg.baseUrl = profile.value().baseUrl;
    pcfg.apiKey = key.value();
    // fields：extra（非 secret）叠 secrets（secret）。后者覆盖前者 —— 同一键
    // 若两处都写了，以 secrets 为准（secret 是更受限的那一份）。
    for (auto& kv : flatMapFromJson(cfg.value().extraJson)) {
        pcfg.fields[kv.first] = kv.second;
    }
    for (auto& kv : flatMapFromJson(cfg.value().secretsJson)) {
        pcfg.fields[kv.first] = kv.second;
    }
    // ---- G6：cc-switch 覆盖（引用优先于任何落库副本）----
    // ccswitch_provider_id 非空 → 该行的 extra/secrets **按不变量 2 必然是空**
    //（quota_dialog_config.cpp 的 buildQuotaConfigRow 无条件清空），所以这里
    // 直接覆盖是安全的；万一外部改了库留了副本，覆盖方向仍是「cc-switch 赢」
    //——与「引用优先」的语义一致。
    if (ccSwitch_ && !cfg.value().ccswitchProviderId.empty()) {
        const CcSwitchProviderList list = ccSwitch_();
        const CcSwitchProviderInfo* matched = nullptr;
        for (const CcSwitchProviderInfo& info : list.providers) {
            if (info.id == cfg.value().ccswitchProviderId) {
                matched = &info;
                break;
            }
        }
        // 引用悬空（用户在 cc-switch 里删了这条）→ **不**静默用手填副本顶上：
        // 报 NotConfigured，UI 引导「重新选择 cc-switch 供应商」。静默顶替会让
        // 用户以为在查 cc-switch 的账号，实际发的是另一套凭据。
        if (matched == nullptr) {
            return notConfigured("cc-switch 里已找不到引用的供应商（"
                                 "「" + cfg.value().ccswitchProviderId +
                                 "」），请重新选择或改为手动填写");
        }
        if (!matched->accessKeyId.empty()) {
            pcfg.fields["accessKeyId"] = matched->accessKeyId;
        }
        if (!matched->secretAccessKey.empty()) {
            pcfg.fields["secretAccessKey"] = matched->secretAccessKey;
        }
    }
    return provider.fetch(pcfg, http);
}

CcSwitchProviderList readRealCcSwitchProviders() {
    return listCcSwitchProviders();
}

HttpSendFn makeRealHttpSendFn() {
    return [](const platform::http::Request& req) {
        return platform::http::send(req);
    };
}

}  // namespace module::claude::quota
