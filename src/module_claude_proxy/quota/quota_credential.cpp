// src/module_claude_proxy/quota/quota_credential.cpp
// 默认凭据来源实现：读本 profile 自己的 api_key_encrypted（E1 已落库）+ 配置表。
#include "quota_credential.h"

#include <map>
#include <mutex>

#include "module_claude_proxy/profile_repository.h"
#include "platform/http_client.h"
#include "platform/json.h"
#include "quota_store.h"

namespace module::claude::quota {

namespace {

namespace pj = platform::json;

// {"accessKeyId":"AK","secretAccessKey":"SK"} → map。坏 JSON / 非对象 →
// **空 map**（容错：配置写坏了不该让整次查询崩成 Shape，让 provider 自己去
// 撞它对字段的校验，出的错才是用户看得懂的那种）。
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
                                                      QuotaProvider& provider,
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
    return provider.fetch(pcfg, http);
}

HttpSendFn makeRealHttpSendFn() {
    return [](const platform::http::Request& req) {
        return platform::http::send(req);
    };
}

}  // namespace module::claude::quota
