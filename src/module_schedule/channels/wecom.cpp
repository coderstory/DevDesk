// src/module_schedule/channels/wecom.cpp
// 企业微信 webhook 渠道实现。契约与凭据纪律见 wecom.h 文件头。
#include "module_schedule/channels/wecom.h"

#include <chrono>

#include "core/logging.h"
#include "platform/json.h"
#include "platform/keychain.h"

namespace module::schedule {

// ---- 纯函数 ----

std::string truncateUtf8(const std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    // 从首个被排除的字节向前回退，跳过 UTF-8 续字节（0b10xxxxxx）；
    // 停处若是 ASCII 直接安全，若是多字节首字节则整字符一并排除。
    while (cut > 0 &&
           (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return text.substr(0, cut);
}

std::string wecomKeychainAccount(const WecomConfig& cfg) {
    if (!cfg.secretRef.empty()) return cfg.secretRef;
    return "wecom." + std::to_string(cfg.channelDbId);
}

std::string buildWecomUrl(const WecomConfig& cfg, const std::string& key) {
    const std::string& base =
        cfg.endpoint.empty() ? kWecomDefaultEndpoint : cfg.endpoint;
    return base + "?key=" + key;
}

core::Result<platform::http::ProxyMode> resolveWecomProxyMode(
    const WecomConfig& cfg) {
    if (cfg.proxyMode == "follow_system") {
        return core::Result<platform::http::ProxyMode>::ok(
            platform::http::ProxyMode::FollowSystem);
    }
    if (cfg.proxyMode == "disabled") {
        return core::Result<platform::http::ProxyMode>::ok(
            platform::http::ProxyMode::Disabled);
    }
    if (cfg.proxyMode == "custom") {
        if (cfg.proxyAddr.empty()) {
            return core::Result<platform::http::ProxyMode>::fail(
                core::ErrorCode::ValidationError,
                "proxy_mode=custom 必须同时配置 proxy 地址");
        }
        return core::Result<platform::http::ProxyMode>::ok(
            platform::http::ProxyMode::Custom);
    }
    return core::Result<platform::http::ProxyMode>::fail(
        core::ErrorCode::ValidationError,
        "未知 proxy_mode：" + cfg.proxyMode);
}

namespace {

// config_json 里字符串数组字段的严格读取：非数组/含非字符串元素 → ValidationError。
core::Result<std::vector<std::string>> readStringArray(
    const platform::json::JsonValue& value) {
    if (!value.isArray()) {
        return core::Result<std::vector<std::string>>::fail(
            core::ErrorCode::ValidationError, "字段必须是字符串数组");
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(value.size()));
    for (int i = 0; i < value.size(); ++i) {
        platform::json::JsonValue item = value[i];
        if (!item.isString()) {
            return core::Result<std::vector<std::string>>::fail(
                core::ErrorCode::ValidationError, "数组元素必须是字符串");
        }
        out.push_back(item.asString());
    }
    return core::Result<std::vector<std::string>>::ok(std::move(out));
}

}  // namespace

core::Result<WecomConfig> parseWecomConfig(const std::string& configJson,
                                           std::int64_t channelDbId,
                                           const std::string& secretRef) {
    WecomConfig cfg;
    cfg.channelDbId = channelDbId;
    cfg.secretRef = secretRef;
    if (configJson.empty()) return core::Result<WecomConfig>::ok(std::move(cfg));

    auto parsed = platform::json::parse(configJson);
    if (parsed.isFail()) {
        return core::Result<WecomConfig>::fail(
            parsed.errorCode(),
            "schedule_channel.config_json 解析失败：" + parsed.errorMessage());
    }
    if (!parsed.value().isObject()) {
        return core::Result<WecomConfig>::fail(
            core::ErrorCode::ValidationError, "config_json 必须是 JSON 对象");
    }
    platform::json::JsonObject obj = parsed.value().asObject();
    for (const std::string& key : obj.keys()) {
        platform::json::JsonValue v = obj.get(key);
        if (key == "msgtype") {
            if (!v.isString()) {
                return core::Result<WecomConfig>::fail(
                    core::ErrorCode::ValidationError, "msgtype 必须是字符串");
            }
            cfg.msgtype = v.asString();
        } else if (key == "endpoint") {
            if (!v.isString()) {
                return core::Result<WecomConfig>::fail(
                    core::ErrorCode::ValidationError, "endpoint 必须是字符串");
            }
            cfg.endpoint = v.asString();
        } else if (key == "proxy_mode") {
            if (!v.isString()) {
                return core::Result<WecomConfig>::fail(
                    core::ErrorCode::ValidationError,
                    "proxy_mode 必须是字符串");
            }
            cfg.proxyMode = v.asString();
        } else if (key == "proxy") {
            if (!v.isString()) {
                return core::Result<WecomConfig>::fail(
                    core::ErrorCode::ValidationError, "proxy 必须是字符串");
            }
            cfg.proxyAddr = v.asString();
        } else if (key == "timeout") {
            if (!v.isNumber()) {
                return core::Result<WecomConfig>::fail(
                    core::ErrorCode::ValidationError, "timeout 必须是数字");
            }
            cfg.timeoutMs = v.asInt();
        } else if (key == "mentions" || key == "mentioned_list") {
            auto items = readStringArray(v);
            if (items.isFail()) {
                return core::Result<WecomConfig>::fail(items.errorCode(),
                                                       items.errorMessage());
            }
            cfg.mentionUsers = items.value();
        } else if (key == "mentioned_mobile_list") {
            auto items = readStringArray(v);
            if (items.isFail()) {
                return core::Result<WecomConfig>::fail(items.errorCode(),
                                                       items.errorMessage());
            }
            cfg.mentionMobiles = items.value();
        }
        // 其余字段未知：忽略（渠道配置前向兼容，UI 波新增字段不破坏旧行）
    }
    if (cfg.timeoutMs <= 0) cfg.timeoutMs = 10000;
    if (cfg.msgtype != "text" && cfg.msgtype != "markdown" &&
        cfg.msgtype != "markdown_v2") {
        return core::Result<WecomConfig>::fail(
            core::ErrorCode::ValidationError, "未知 msgtype：" + cfg.msgtype);
    }
    // proxy_mode 值非法在装配期报错（resolveWecomProxyMode 是唯一真源）
    auto proxyCheck = resolveWecomProxyMode(cfg);
    if (proxyCheck.isFail()) {
        return core::Result<WecomConfig>::fail(proxyCheck.errorCode(),
                                               proxyCheck.errorMessage());
    }
    return core::Result<WecomConfig>::ok(std::move(cfg));
}

core::Result<WecomPayload> buildWecomPayload(const WecomConfig& cfg,
                                             const Delivery& d) {
    const bool isText = cfg.msgtype == "text";
    const bool isMarkdown = cfg.msgtype == "markdown";
    const bool isMarkdownV2 = cfg.msgtype == "markdown_v2";
    if (!isText && !isMarkdown && !isMarkdownV2) {
        return core::Result<WecomPayload>::fail(
            core::ErrorCode::ValidationError, "未知 msgtype：" + cfg.msgtype);
    }

    WecomPayload payload;
    payload.msgtype = cfg.msgtype;

    std::string content;
    if (isText) {
        content = d.title.empty() ? d.body : (d.title + "\n" + d.body);
        content = truncateUtf8(content, kWecomTextMaxBytes);
    } else {
        content = d.title.empty() ? d.body : ("**" + d.title + "**\n\n" + d.body);
        if (isMarkdown) {
            // markdown 官方无列表字段：@ 人以正文 <@userid> 追加（截断之前）
            for (const std::string& user : cfg.mentionUsers) {
                content += "\n<@" + user + ">";
            }
        }
        // markdown_v2 不支持 @（官方）：mentions 一律忽略
        content = truncateUtf8(content, kWecomMarkdownMaxBytes);
    }
    payload.content = content;

    platform::json::JsonObject root;
    root.set("msgtype", platform::json::JsonValue(payload.msgtype));
    platform::json::JsonObject inner;
    inner.set("content", platform::json::JsonValue(content));
    if (isText) {
        // @ 人默认不开：mentions 空即不出字段（官方字段可省略）
        if (!cfg.mentionUsers.empty()) {
            inner.set("mentioned_list",
                      platform::json::makeStringArray(cfg.mentionUsers));
        }
        if (!cfg.mentionMobiles.empty()) {
            inner.set("mentioned_mobile_list",
                      platform::json::makeStringArray(cfg.mentionMobiles));
        }
    }
    root.set(isText ? "text" : (isMarkdown ? "markdown" : "markdown_v2"),
             platform::json::JsonValue(std::move(inner)));
    auto json = platform::json::stringify(
        platform::json::JsonValue(std::move(root)));
    if (json.isFail()) {
        return core::Result<WecomPayload>::fail(json.errorCode(),
                                                json.errorMessage());
    }
    payload.json = json.value();
    return core::Result<WecomPayload>::ok(std::move(payload));
}

WecomErrorVerdict classifyWecomError(bool networkError, int httpStatus,
                                     bool hasErrcode, int errcode) {
    if (networkError) return {WecomErrorClass::Retryable, 0};
    if (httpStatus >= 500) return {WecomErrorClass::Retryable, 0};
    if (httpStatus >= 400) return {WecomErrorClass::Permanent, -1};
    if (!hasErrcode) return {WecomErrorClass::Retryable, 0};  // 2xx 无合法 errcode
    switch (errcode) {
        case 0:
            return {WecomErrorClass::Ok, 0};
        case 45009:  // 接口调用超限：官方 20 条/分钟，建议等 60s
            return {WecomErrorClass::Retryable, 60};
        case 45033:  // 并发调用超限：建议指数退避（起点 30s，引擎按 attempt 递增）
            return {WecomErrorClass::Retryable, 30};
        case 93000:  // webhook 非法或已被移除
        case 93001:  // 群不允许机器人推送
        case 93004:  // 机器人已被停用
        case 93017:  // 空请求被拒
            return {WecomErrorClass::Permanent, -1};
        default:  // 未知业务码：按瞬态可重试（不停用渠道，避免误杀）
            return {WecomErrorClass::Retryable, 0};
    }
}

TokenBucket::TokenBucket(int capacity, double refillPerSec)
    : capacity_(capacity),
      refillPerSec_(refillPerSec),
      tokens_(static_cast<double>(capacity)) {}

bool TokenBucket::tryAcquire(std::int64_t nowMs) {
    if (lastMs_ == 0) {
        lastMs_ = nowMs;  // 首次：只记时、不回填（构造即满）
    } else if (nowMs > lastMs_) {
        const double elapsedSec =
            static_cast<double>(nowMs - lastMs_) / 1000.0;
        tokens_ += elapsedSec * refillPerSec_;
        if (tokens_ > static_cast<double>(capacity_)) {
            tokens_ = static_cast<double>(capacity_);
        }
        lastMs_ = nowMs;
    }
    // nowMs < lastMs_：不回补、不重置（生产用单调钟不会回拨）
    if (tokens_ >= 1.0) {
        tokens_ -= 1.0;
        return true;
    }
    return false;
}

// ---- Receipt 状态串与说明（稳定串，见 wecom.h 状态表）----

namespace {

std::string statusFor(const WecomErrorVerdict& verdict, bool networkError,
                      int httpStatus, bool hasErrcode, int errcode) {
    if (verdict.cls == WecomErrorClass::Ok) return "ok";
    if (networkError) return "retryable:network_error";
    if (httpStatus >= 500) return "retryable:http_5xx";
    if (httpStatus >= 400) return "permanent:http_4xx";
    if (hasErrcode) {
        const char* prefix = verdict.cls == WecomErrorClass::Permanent
                                 ? "permanent:errcode_"
                                 : "retryable:errcode_";
        return std::string(prefix) + std::to_string(errcode);
    }
    return "retryable:bad_body";
}

std::string errcodeDetail(int errcode) {
    switch (errcode) {
        case 45009:
            return "接口调用超限（官方每分钟 20 条额度），建议 60 秒后由引擎重试";
        case 45033:
            return "并发调用超限，建议指数退避后由引擎重试";
        case 93000:
            return "webhook 非法或已被移除，判定永久失败：引擎应停用渠道并本地告警";
        case 93001:
            return "群不允许机器人推送，判定永久失败：引擎应停用渠道并本地告警";
        case 93004:
            return "机器人已被停用，判定永久失败：引擎应停用渠道并本地告警";
        case 93017:
            return "空请求被拒，判定永久失败：引擎应停用渠道并检查配置";
        default:
            return "企业微信业务错误，可由引擎重试";
    }
}

}  // namespace

// ---- 渠道实现 ----

WecomChannel::WecomChannel(WecomConfig cfg, WecomHttpSender sender)
    : cfg_(std::move(cfg)),
      sender_(std::move(sender)),
      bucket_(kWecomRateCapacity, kWecomRateRefillPerSec) {}

std::string WecomChannel::id() const { return kChannelWecom; }

core::Result<Receipt> WecomChannel::deliver(const Delivery& d) {
    // 1. 限频（官方 20 条/分钟，桶 18 留余量；任意线程直调 → 桶加锁）
    {
        std::lock_guard<std::mutex> lock(bucketMutex_);
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (!bucket_.tryAcquire(nowMs)) {
            return core::Result<Receipt>::ok(Receipt{
                "rate_limited",
                "本地限频：每机器人 18 条/分钟额度已用尽（官方 20 条留余量），由引擎稍后重试"});
        }
    }

    // 2. 凭据：即时读、用后即弃；key 不落库、不进日志、不进任何对象字段。
    const std::string account = wecomKeychainAccount(cfg_);
    auto secret = platform::keychain::loadSecret(kWecomKeychainService, account);
    if (secret.isFail() || secret.value().empty()) {
        LOG_WARN("企业微信渠道密钥缺失（account " + account +
                 "）：请在设置页配置群机器人 webhook key");
        return core::Result<Receipt>::ok(Receipt{
            "missing_secret",
            "keychain 无渠道密钥（account " + account +
                "）：请先在设置页配置群机器人 webhook key"});
    }
    const std::string& key = secret.value();

    // 3. 载荷（纯函数：截断 + 组装；失败 = 配置非法，按 S5 契约返回 fail）
    auto payload = buildWecomPayload(cfg_, d);
    if (payload.isFail()) {
        return core::Result<Receipt>::fail(payload.errorCode(),
                                           payload.errorMessage());
    }

    // 4. 请求。URL 含 key：本函数之外的任何日志/Receipt 不得携带它。
    auto proxyMode = resolveWecomProxyMode(cfg_);
    if (proxyMode.isFail()) {
        return core::Result<Receipt>::fail(proxyMode.errorCode(),
                                           proxyMode.errorMessage());
    }
    platform::http::Request req;
    req.method = "POST";
    req.url = buildWecomUrl(cfg_, key);
    req.headers["Content-Type"] = "application/json";
    req.body = payload.value().json;
    req.timeoutMs = cfg_.timeoutMs > 0 ? cfg_.timeoutMs : 10000;
    req.proxyMode = proxyMode.value();
    req.proxy = cfg_.proxyAddr;

    // 5. 发送 + 错误分类（必须解析 body 的 errcode，不能只看 HTTP 状态）。
    auto resp = (sender_ ? sender_ : &platform::http::send)(req);
    const bool sendFailed = resp.isFail();
    const platform::http::Response r = resp.valueOr(platform::http::Response{});
    const bool networkError = sendFailed || r.networkError;

    bool hasErrcode = false;
    int errcode = 0;
    if (!networkError) {
        auto parsed = platform::json::parse(r.body);
        if (parsed.isOk() && parsed.value().isObject()) {
            platform::json::JsonObject obj = parsed.value().asObject();
            if (obj.has("errcode")) {
                platform::json::JsonValue v = obj.get("errcode");
                if (v.isNumber()) {
                    hasErrcode = true;
                    errcode = static_cast<int>(v.asInt());
                }
            }
        }
    }

    const WecomErrorVerdict verdict =
        classifyWecomError(networkError, r.statusCode, hasErrcode, errcode);
    const std::string status =
        statusFor(verdict, networkError, r.statusCode, hasErrcode, errcode);

    if (verdict.cls == WecomErrorClass::Ok) {
        return core::Result<Receipt>::ok(Receipt{status, "企业微信投递成功"});
    }

    // 说明文本不含 key/URL/正文——凭据与用户内容都不进日志。
    std::string detail;
    if (networkError) {
        detail = "网络传输失败（未收到有效响应），可由引擎按 0s/30s/300s 重试";
    } else if (r.statusCode >= 500) {
        detail = "企业微信服务端错误（HTTP " + std::to_string(r.statusCode) +
                 "），可由引擎重试";
    } else if (r.statusCode >= 400) {
        detail = "HTTP " + std::to_string(r.statusCode) +
                 " 请求被拒，判定永久失败：建议停用渠道并检查配置";
    } else if (hasErrcode) {
        detail = errcodeDetail(errcode);
    } else {
        detail = "响应体无合法 errcode（无法判定），可由引擎重试";
    }
    LOG_WARN("企业微信渠道投递失败[" + status + "]：" + detail);
    return core::Result<Receipt>::ok(Receipt{status, detail});
}

std::unique_ptr<Channel> makeWecomChannel(const WecomConfig& cfg,
                                          WecomHttpSender sender) {
    return std::make_unique<WecomChannel>(cfg, std::move(sender));
}

core::Result<void> registerWecomChannel(ChannelRegistry& registry,
                                        const WecomConfig& cfg) {
    auto result = registry.registerChannel(makeWecomChannel(cfg));
    if (result.isFail() &&
        result.errorCode() == core::ErrorCode::AlreadyExists) {
        // 幂等装配：与 S5 registerBuiltinChannels 同口径，保留首个实现
        LOG_WARN("企业微信渠道重复注册被跳过（保留首个实现）");
        return core::Result<void>::ok();
    }
    return result;
}

}  // namespace module::schedule
