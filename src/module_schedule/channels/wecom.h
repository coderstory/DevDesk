// src/module_schedule/channels/wecom.h
// 企业微信群机器人 webhook 渠道（波 S6b，§4.9 渠道表 + 调研 §10.2，官方文档 2025-08-07 版）。
//
// 与 Channel 接口（S5）的关系：本文件只依赖 channel_registry.h 的冻结签名
// （Channel/Delivery/Receipt/ChannelRegistry），不改签名、不另起炉灶；
// 注册接缝 registerWecomChannel 由合并波在 registerBuiltinChannels 之后调用
// （schedule_channel 行 kind=="wecom" → parseWecomConfig → 注册）。
//
// 凭据纪律（§4.9 硬约束）：webhook key 只经 platform::keychain
// （service kWecomKeychainService，account 经 schedule_channel.secret_ref，
//  空则派生 wecom.<channelDbId>），不落 sqlite、不进日志、不进任何 struct；
// deliver 内即时读取、用后即弃。buildWecomUrl 的返回值含 key，禁止写进日志/Receipt。
//
// 幂等契约：无服务端幂等键，去重靠引擎的 (rule, planned_at)；buildWecomPayload
// 是纯函数（不注入时间戳/attempt 计数），重试恒产同一正文——重试不得改正文。
//
// 并发契约：deliver 任意线程可调（S5 渠道统一契约）；令牌桶非线程安全，
// 由渠道对象内部互斥量保护；keychain/http 调用自身线程安全（facade 契约）。
#pragma once

#include "core/result.h"
#include "module_schedule/channel_registry.h"
#include "platform/http_client.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace module::schedule {

// 注册表键（schedule_rule.channel_ids 引用）。S5 的四个常量在 channel_registry.h，
// 本常量随渠道文件放置，避免两个渠道头互相 include。
inline constexpr const char* kChannelWecom = "wecom";

// 官方端点与字节上限（2025-08-07 版官方文档，调研 §10.2 已核实）。
inline constexpr const char* kWecomDefaultEndpoint =
    "https://qyapi.weixin.qq.com/cgi-bin/webhook/send";
inline constexpr std::size_t kWecomTextMaxBytes = 2048;      // text.content 上限
inline constexpr std::size_t kWecomMarkdownMaxBytes = 4096;  // markdown / markdown_v2 上限
inline constexpr int kWecomRateCapacity = 18;                // 官方 20 条/分钟，留 2 条余量
inline constexpr double kWecomRateRefillPerSec = 18.0 / 60.0;
inline constexpr const char* kWecomKeychainService = "devdesk.schedule";  // keychain service

// ---- 纯函数（可单测，不触 IO/网络/时钟）----

// UTF-8 边界截断：输出 ≤ maxBytes 字节且末字符完整（不切半个多字节字符）。
// 超限时从 maxBytes 处（首个被排除的字节）向前回退，跳过续字节（0b10xxxxxx）。
// maxBytes == 0 → 空串；输入 ≤ maxBytes → 原样返回。
std::string truncateUtf8(const std::string& text, std::size_t maxBytes);

// 渠道配置（schedule_channel.config_json 的解码结果 + 行级字段）。
// secretRef = schedule_channel.secret_ref（keychain account 名）；空则由
// parseWecomConfig 派生默认 wecom.<channelDbId>。key 本体不进本结构。
struct WecomConfig {
    std::string msgtype = "text";  // text | markdown | markdown_v2（其余值装配期报错）
    std::string endpoint;          // 空 = kWecomDefaultEndpoint
    std::vector<std::string> mentionUsers;    // text mentioned_list；含 "@all" 即全员
    std::vector<std::string> mentionMobiles;  // text mentioned_mobile_list
    std::string proxyMode = "follow_system";  // follow_system|disabled|custom（S6a 三态）
    std::string proxyAddr;          // custom 时必填（"host:port"）
    long timeoutMs = 10000;         // HTTP 超时；≤0 按默认
    std::string secretRef;          // keychain account（如 wecom.3）；空 = wecom.<channelDbId>
    std::int64_t channelDbId = 0;   // schedule_channel.id：默认 account 派生与排障定位
};

// config_json 解码 + secretRef 兜底。mentions/mentioned_list/mentioned_mobile_list
// 为 JSON 字符串数组；未知字段忽略（渠道配置前向兼容）。已知字段类型错、
// msgtype/proxy_mode 值非法、JSON 非法 → ValidationError。configJson 空串 = 全默认。
core::Result<WecomConfig> parseWecomConfig(const std::string& configJson,
                                           std::int64_t channelDbId,
                                           const std::string& secretRef);

// proxy_mode 三态映射到 S6a 的 ProxyMode 枚举。custom 且 proxyAddr 空 →
// ValidationError；未知模式 → ValidationError。
core::Result<platform::http::ProxyMode> resolveWecomProxyMode(const WecomConfig& cfg);

// 请求 URL：endpoint 空 → 官方端点；拼 "?key=<key>"。返回值含 key——
// 调用方禁止把它写进任何日志/Receipt/detail（凭据纪律）。
std::string buildWecomUrl(const WecomConfig& cfg, const std::string& key);

// keychain account：secretRef 非空即原样（经 schedule_channel.secret_ref），
// 空则 "wecom." + channelDbId。account 名不是秘密，可进日志。
std::string wecomKeychainAccount(const WecomConfig& cfg);

// 载荷组装（含长度纪律 §4.9）：
//   text        → content = title 非空 ? title+"\n"+body : body，截 2048 字节；
//                 mentions 非空才出 mentioned_list / mentioned_mobile_list 字段（默认不 @）。
//   markdown    → content = title 非空 ? "**title**\n\n"+body : body，截 4096 字节；
//                 mentionUsers 以正文 "<@userid>" 追加（在截断之前，官方 markdown 无列表字段）。
//   markdown_v2 → 同 markdown 截 4096，但 @ 不支持（官方）：mentions 一律忽略。
// 未知 msgtype → ValidationError。纯函数：同一 (cfg, d) 恒产同一 json。
struct WecomPayload {
    std::string msgtype;  // 实际生效
    std::string content;  // 截断后正文（测试断言字节数/末字符合法）
    std::string json;     // 序列化后的请求体
};
core::Result<WecomPayload> buildWecomPayload(const WecomConfig& cfg,
                                             const Delivery& d);

// 错误分类（§4.9 错误判定硬约束：必须解析 body 的 errcode，不能只看 HTTP 状态）。
// 判定顺序：网络错误 → 5xx → 4xx → errcode。93000/93001/93004/93017 → 永久失败
// （引擎据此停用渠道 + 本地告警）；45009 → 可重试建议等 60s；45033 → 可重试建议
// 指数退避（起点 30s）；其余 errcode 与坏 body → 可重试；4xx → 永久（调研 §10.2）。
// 三档 0s/30s/300s 由引擎按 attempt 编排，suggestedDelaySec 只是渠道侧建议。
enum class WecomErrorClass { Ok, Retryable, Permanent };
struct WecomErrorVerdict {
    WecomErrorClass cls = WecomErrorClass::Ok;
    int suggestedDelaySec = 0;  // 永久失败时为 -1（无重试）
};
WecomErrorVerdict classifyWecomError(bool networkError, int httpStatus,
                                     bool hasErrcode, int errcode);

// ---- 令牌桶（纯计算，非线程安全——持有方加锁）----
// 容量 18、回填 18/60s：同一分钟内第 19 条被拒（官方 20 条/分钟留余量）。
// now 由调用方注入（毫秒，生产用单调钟），单测可假时钟推进。
class TokenBucket {
public:
    TokenBucket(int capacity, double refillPerSec);

    // 尝试取 1 个令牌。nowMs 回拨不回补、不重置（生产用单调钟不会回拨）。
    bool tryAcquire(std::int64_t nowMs);

private:
    int capacity_;
    double refillPerSec_;
    double tokens_;         // 含小数（浮点回填）
    std::int64_t lastMs_ = 0;  // 0 = 未初始化（首次只记时、不回填）
};

// ---- 渠道与装配缝 ----

// HTTP 发送缝：测试注入假发送器捕获 Request 并回放 canned 响应；
// nullptr = 生产 platform::http::send。
using WecomHttpSender = std::function<core::Result<platform::http::Response>(
    const platform::http::Request&)>;

// Receipt.status 稳定串（引擎重试编排/测试断言/schedule_delivery.status 落库依赖）：
//   "ok"                                         投递成功（errcode 0）
//   "rate_limited"                               本地令牌桶拒发（未发请求，引擎稍后重试）
//   "missing_secret"                             keychain 无密钥（未发请求，配置问题）
//   "retryable:network_error"                    传输层失败
//   "retryable:http_5xx"                         服务端 5xx
//   "retryable:bad_body"                         响应体无合法 errcode（无法判定）
//   "retryable:errcode_<n>"                      业务码可重试（45009/45033/未知码）
//   "permanent:http_4xx"                         4xx 永久失败
//   "permanent:errcode_<n>"                      93000/93001/93004/93017 永久失败
// 环境类失败一律走 status 不返回 Result::fail（S5 契约）；只有载荷/配置非法
// （msgtype 未知、custom 缺代理、config_json 坏）才 fail(ValidationError)。
class WecomChannel final : public Channel {
public:
    explicit WecomChannel(WecomConfig cfg, WecomHttpSender sender = nullptr);

    std::string id() const override;
    core::Result<Receipt> deliver(const Delivery& d) override;

private:
    WecomConfig cfg_;
    WecomHttpSender sender_;
    TokenBucket bucket_;
    std::mutex bucketMutex_;  // deliver 任意线程可调；桶非线程安全
};

std::unique_ptr<Channel> makeWecomChannel(const WecomConfig& cfg,
                                          WecomHttpSender sender = nullptr);

// 注册接缝：合并波在 registerBuiltinChannels 之后按 schedule_channel 行装配
//（listChannels() 过滤 kind=="wecom"，逐行 parseWecomConfig 后调用本函数）。
// 重复注册幂等（AlreadyExists 忽略，保留首个实现，与 S5 注册表契约一致）。
core::Result<void> registerWecomChannel(ChannelRegistry& registry,
                                        const WecomConfig& cfg);

}  // namespace module::schedule
