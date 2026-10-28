// 调度企业微信 webhook 渠道（波 S6b）测试：UTF-8 截断边界 / errcode 分类全表 /
// 令牌桶节奏 / 请求体组装（msgtype·mentions）/ 凭据不泄进日志字符串。
// 每条用例对应一个业务场景并注明防的具体事故；网络经注入缝回放，不触真实 HTTP。
#include "doctest.h"

#include "module_schedule/channels/wecom.h"
#include "platform/json.h"
#include "platform/keychain.h"
#include "platform/utf8.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

using module::schedule::Delivery;
using module::schedule::WecomConfig;
using module::schedule::WecomErrorClass;
using module::schedule::WecomErrorVerdict;

const char* kKey = "SECRET-KEY-abc123";  // 断言它不出现在任何日志字符串里的密钥样本

Delivery makeDelivery() {
    Delivery d;
    d.title = "提醒标题";
    d.body = "提醒正文";
    d.level = 2;
    d.runId = 42;
    d.dedupeKey = "rule:7@planned:1700000000";
    return d;
}

// ---- 假 keychain 后端（经官方注入缝 setBackendForTest 安装）----
struct FakeKeychainBackend {
    static core::Result<void> store(const std::string&, const std::string&,
                                    const std::string&) {
        return core::Result<void>::ok();
    }
    static core::Result<std::string> load(const std::string& service,
                                          const std::string& account) {
        DOCTEST_CHECK_EQ(service, module::schedule::kWecomKeychainService);
        accounts.push_back(account);
        if (failLoad) {
            return core::Result<std::string>::fail(core::ErrorCode::NotFound,
                                                   "fake missing");
        }
        return core::Result<std::string>::ok(kKey);
    }
    static core::Result<void> del(const std::string&, const std::string&) {
        return core::Result<void>::ok();
    }
    static core::Result<std::vector<std::string>> delNotIn(
        const std::string&, const std::vector<std::string>&) {
        return core::Result<std::vector<std::string>>::ok({});
    }
    static inline bool failLoad = false;
    static inline std::vector<std::string> accounts;  // 记录渠道读过的 account 名
};

const platform::keychain::Backend kFakeKeychainBackend{
    &FakeKeychainBackend::store, &FakeKeychainBackend::load,
    &FakeKeychainBackend::del, &FakeKeychainBackend::delNotIn};

struct KeychainBackendGuard {
    KeychainBackendGuard() {
        FakeKeychainBackend::failLoad = false;
        FakeKeychainBackend::accounts.clear();
        platform::keychain::setBackendForTest(&kFakeKeychainBackend);
    }
    ~KeychainBackendGuard() { platform::keychain::setBackendForTest(nullptr); }
};

// ---- 假 HTTP 发送器（捕获 Request + 回放 canned 响应）----
struct FakeHttp {
    std::vector<platform::http::Request> requests;
    platform::http::Response canned;
    bool failResult = false;  // true = 模拟 Result::fail（传输层失败）

    core::Result<platform::http::Response> operator()(
        const platform::http::Request& req) {
        requests.push_back(req);
        if (failResult) {
            return core::Result<platform::http::Response>::fail(
                core::ErrorCode::NetworkUnreachable, "fake transport fail");
        }
        return core::Result<platform::http::Response>::ok(canned);
    }
};

platform::http::Response httpResp(int status, const std::string& body) {
    platform::http::Response r;
    r.statusCode = status;
    r.body = body;
    return r;
}

std::unique_ptr<module::schedule::WecomChannel> makeChannel(
    const WecomConfig& cfg, FakeHttp& http) {
    return std::make_unique<module::schedule::WecomChannel>(cfg,
                                                            std::ref(http));
}

}  // namespace

// 防：text 正文超 2048 字节被官方拒收——长度纪律是请求体组装的一部分而非可选
REGISTER_TEST(wecomTruncateAsciiExactLimit, "截断边界：超限 ASCII 精确截到 2048 字节，≤ 上限原样返回", {
    const std::string over = std::string(3000, 'a');
    DOCTEST_CHECK_EQ(module::schedule::truncateUtf8(over,
                                                    module::schedule::kWecomTextMaxBytes),
                     std::string(2048, 'a'));
    DOCTEST_CHECK_EQ(module::schedule::truncateUtf8(std::string(2048, 'b'),
                                                    module::schedule::kWecomTextMaxBytes),
                     std::string(2048, 'b'));
    return true;
});

// 防：按字节硬切把汉字切半——输出非法 UTF-8，机器人侧显示乱码/豆腐块
REGISTER_TEST(wecomTruncateNotCutHalfCjk, "截断边界：切口落在汉字中间时回退到字符边界，末字符合法", {
    // 2047 ASCII + 1 汉字（3 字节）= 2050 字节，上限 2048 切口落在第 2 个续字节上
    std::string mixed = std::string(2047, 'a') + "汉";
    const std::string out = module::schedule::truncateUtf8(
        mixed, module::schedule::kWecomTextMaxBytes);
    DOCTEST_CHECK_EQ(out.size(), 2047u);
    DOCTEST_CHECK(platform::utf8::isValid(out));

    // 纯汉字串：2100 字节 = 700 个 3 字节字符 → 截后必须是 3 的倍数且 ≤ 上限
    std::string cjk;
    for (int i = 0; i < 700; ++i) cjk += "汉";
    const std::string outCjk = module::schedule::truncateUtf8(
        cjk, module::schedule::kWecomTextMaxBytes);
    DOCTEST_CHECK_LE(outCjk.size(), module::schedule::kWecomTextMaxBytes);
    DOCTEST_CHECK_EQ(outCjk.size() % 3u, 0u);
    DOCTEST_CHECK(platform::utf8::isValid(outCjk));
    return true;
});

// 防：截断偏移一位——恰好贴上限的多字节串被误截或放行半个字符
REGISTER_TEST(wecomTruncateBoundaryExactFit, "截断边界：恰好 ≤ 上限的多字节串不截、超 1 字节截到安全点", {
    const std::string exact = std::string(2045, 'a') + "汉";  // 2048 字节整
    DOCTEST_CHECK_EQ(module::schedule::truncateUtf8(exact, 2048).size(), 2048u);
    const std::string overOne = std::string(2046, 'a') + "汉";  // 2049 字节
    const std::string out = module::schedule::truncateUtf8(overOne, 2048);
    DOCTEST_CHECK_EQ(out.size(), 2046u);
    DOCTEST_CHECK(platform::utf8::isValid(out));
    return true;
});

// 防：maxBytes=0 / 短输入路径回归（空正文不应产出断言崩溃或截断异常）
REGISTER_TEST(wecomTruncateZeroAndShort, "截断边界：maxBytes=0 得空串，短输入原样", {
    DOCTEST_CHECK_EQ(module::schedule::truncateUtf8("hello", 0), std::string(""));
    DOCTEST_CHECK_EQ(module::schedule::truncateUtf8("hello", 5), std::string("hello"));
    DOCTEST_CHECK_EQ(module::schedule::truncateUtf8("", 2048), std::string(""));
    return true;
});

// 防：可重试/永久分错——45009 被当永久=自断渠道；93000 被当可重试=已死渠道反复打、
// 每次重试都白耗限频额度；4xx 与 5xx 混判=重试策略错误
REGISTER_TEST(wecomClassifyErrcodeFullTable, "errcode 分类全表：可重试三类（网络/5xx/45009/45033）vs 永久四类（93000/93001/93004/93017/4xx）", {
    auto cls = [](bool net, int http, bool hasCode, int code) {
        return module::schedule::classifyWecomError(net, http, hasCode, code).cls;
    };
    DOCTEST_CHECK_EQ(cls(true, 0, false, 0), WecomErrorClass::Retryable);   // 网络错误
    DOCTEST_CHECK_EQ(cls(false, 500, false, 0), WecomErrorClass::Retryable);
    DOCTEST_CHECK_EQ(cls(false, 503, false, 0), WecomErrorClass::Retryable);
    DOCTEST_CHECK_EQ(cls(false, 400, false, 0), WecomErrorClass::Permanent);
    DOCTEST_CHECK_EQ(cls(false, 404, false, 0), WecomErrorClass::Permanent);
    DOCTEST_CHECK_EQ(cls(false, 200, true, 0), WecomErrorClass::Ok);        // errcode 0
    DOCTEST_CHECK_EQ(cls(false, 200, false, 0), WecomErrorClass::Retryable); // 无 errcode
    DOCTEST_CHECK_EQ(cls(false, 200, true, 45009), WecomErrorClass::Retryable);
    DOCTEST_CHECK_EQ(cls(false, 200, true, 45033), WecomErrorClass::Retryable);
    DOCTEST_CHECK_EQ(cls(false, 200, true, 99999), WecomErrorClass::Retryable); // 未知业务码
    for (int code : {93000, 93001, 93004, 93017}) {
        DOCTEST_CHECK_EQ(cls(false, 200, true, code), WecomErrorClass::Permanent);
    }
    // 建议延迟：45009 → 60s；45033 → 30s（指数退避起点）；永久 → -1
    WecomErrorVerdict v45009 = module::schedule::classifyWecomError(false, 200, true, 45009);
    DOCTEST_CHECK_EQ(v45009.suggestedDelaySec, 60);
    WecomErrorVerdict v45033 = module::schedule::classifyWecomError(false, 200, true, 45033);
    DOCTEST_CHECK_EQ(v45033.suggestedDelaySec, 30);
    WecomErrorVerdict v93000 = module::schedule::classifyWecomError(false, 200, true, 93000);
    DOCTEST_CHECK_EQ(v93000.suggestedDelaySec, -1);
    return true;
});

// 防：本地限频失效突穿官方 20 条/分钟——官方对超限直接 45009，反复超发有封机器人风险
REGISTER_TEST(wecomBucket19thRejectedWithinMinute, "令牌桶节奏：1 分钟内第 19 条被拒（容量 18 留 2 条余量）", {
    module::schedule::TokenBucket bucket(module::schedule::kWecomRateCapacity,
                                         module::schedule::kWecomRateRefillPerSec);
    std::int64_t t = 1000;
    for (int i = 0; i < 18; ++i) {
        DOCTEST_CHECK(bucket.tryAcquire(t));
    }
    DOCTEST_CHECK(!bucket.tryAcquire(t));  // 第 19 条：同一分钟内拒
    return true;
});

// 防：回填公式错——速率 0 则渠道被永久限死（静默不投递），速率过大则突穿官方额度
REGISTER_TEST(wecomBucketRefillPace, "令牌桶节奏：整分钟回填 18、按 0.3/s 线性部分回填", {
    module::schedule::TokenBucket bucket(module::schedule::kWecomRateCapacity,
                                         module::schedule::kWecomRateRefillPerSec);
    std::int64_t t = 1000;
    for (int i = 0; i < 18; ++i) {
        DOCTEST_CHECK(bucket.tryAcquire(t));
    }
    DOCTEST_CHECK(bucket.tryAcquire(t + 60000));  // 整分钟：回填满 18

    module::schedule::TokenBucket b2(18, 0.3);
    for (int i = 0; i < 18; ++i) {
        DOCTEST_CHECK(b2.tryAcquire(5000));
    }
    DOCTEST_CHECK(b2.tryAcquire(15000));  // 10s × 0.3/s = 3 枚
    DOCTEST_CHECK(b2.tryAcquire(15000));
    DOCTEST_CHECK(b2.tryAcquire(15000));
    DOCTEST_CHECK(!b2.tryAcquire(15000));  // 3 枚用尽
    return true;
});

// 防：@ 人字段组装错——mentioned_list 漏发/@all 默认误扰全群（默认必须不 @）
REGISTER_TEST(wecomPayloadTextMentionsAssembly, "请求体组装 text：content 合并截断、mentions 非空才出字段、默认不 @", {
    WecomConfig cfg;
    cfg.msgtype = "text";
    cfg.mentionUsers = {"@all", "zhangsan"};
    cfg.mentionMobiles = {"13800000000"};
    auto payload = module::schedule::buildWecomPayload(cfg, makeDelivery());
    DOCTEST_REQUIRE(payload.isOk());
    DOCTEST_CHECK_EQ(payload.value().msgtype, std::string("text"));
    DOCTEST_CHECK_EQ(payload.value().content, std::string("提醒标题\n提醒正文"));

    auto parsed = platform::json::parse(payload.value().json);
    DOCTEST_REQUIRE(parsed.isOk());
    auto root = parsed.value().asObject();
    DOCTEST_CHECK_EQ(root.get("msgtype").asString(), std::string("text"));
    auto text = root.get("text").asObject();
    DOCTEST_CHECK_EQ(text.get("content").asString(), std::string("提醒标题\n提醒正文"));
    DOCTEST_CHECK(text.has("mentioned_list"));
    DOCTEST_CHECK_EQ(text.get("mentioned_list").size(), 2);
    DOCTEST_CHECK_EQ(text.get("mentioned_list")[0].asString(), std::string("@all"));
    DOCTEST_CHECK(text.has("mentioned_mobile_list"));
    DOCTEST_CHECK_EQ(text.get("mentioned_mobile_list").size(), 1);

    // 默认不 @：mentions 空 → 两个字段都不出现
    WecomConfig plain;
    auto plainPayload = module::schedule::buildWecomPayload(plain, makeDelivery());
    DOCTEST_REQUIRE(plainPayload.isOk());
    auto plainParsed = platform::json::parse(plainPayload.value().json);
    auto plainText = plainParsed.value().asObject().get("text").asObject();
    DOCTEST_CHECK(!plainText.has("mentioned_list"));
    DOCTEST_CHECK(!plainText.has("mentioned_mobile_list"));
    return true;
});

// 防：markdown 发 mentioned_list（官方 markdown 无此字段）被拒；markdown_v2 带 @ 被官方明确拒绝
REGISTER_TEST(wecomPayloadMarkdownAndV2Mentions, "请求体组装 markdown/markdown_v2：正文 <@userid>、v2 忽略 @、4096 截断", {
    WecomConfig md;
    md.msgtype = "markdown";
    md.mentionUsers = {"lisi"};
    auto payload = module::schedule::buildWecomPayload(md, makeDelivery());
    DOCTEST_REQUIRE(payload.isOk());
    auto root = platform::json::parse(payload.value().json).value().asObject();
    DOCTEST_CHECK_EQ(root.get("msgtype").asString(), std::string("markdown"));
    auto markdown = root.get("markdown").asObject();
    DOCTEST_CHECK(markdown.get("content").asString().find("\n<@lisi>") !=
                  std::string::npos);
    DOCTEST_CHECK(!markdown.has("mentioned_list"));  // markdown 官方无列表字段
    DOCTEST_CHECK_LE(markdown.get("content").asString().size(),
                     module::schedule::kWecomMarkdownMaxBytes);

    WecomConfig v2;
    v2.msgtype = "markdown_v2";
    v2.mentionUsers = {"lisi"};
    auto payloadV2 = module::schedule::buildWecomPayload(v2, makeDelivery());
    DOCTEST_REQUIRE(payloadV2.isOk());
    auto rootV2 = platform::json::parse(payloadV2.value().json).value().asObject();
    DOCTEST_CHECK_EQ(rootV2.get("msgtype").asString(), std::string("markdown_v2"));
    DOCTEST_CHECK(rootV2.has("markdown_v2"));
    // markdown_v2 不支持 @：mentions 一律忽略
    DOCTEST_CHECK_EQ(rootV2.get("markdown_v2").asObject()
                         .get("content").asString().find("<@lisi>"),
                     std::string::npos);
    return true;
});

// 防：重试改正文违反幂等契约（§4.9）——同 (rule, planned_at) 的重试必须逐字节同正文
REGISTER_TEST(wecomPayloadDeterministicRetrySameBody, "幂等契约：同一 (cfg, Delivery) 两次组装逐字节相同（无时间戳/attempt 注入）", {
    WecomConfig cfg;
    cfg.mentionUsers = {"@all"};
    const Delivery d = makeDelivery();
    auto a = module::schedule::buildWecomPayload(cfg, d);
    auto b = module::schedule::buildWecomPayload(cfg, d);
    DOCTEST_REQUIRE(a.isOk());
    DOCTEST_REQUIRE(b.isOk());
    DOCTEST_CHECK_EQ(a.value().json, b.value().json);
    DOCTEST_CHECK_EQ(a.value().content, b.value().content);
    return true;
});

// 防：非法 msgtype 静默按 text 发出——格式错投，机器人侧拒绝或显示异常
REGISTER_TEST(wecomPayloadUnknownMsgtypeFails, "载荷组装：未知 msgtype 返回 ValidationError（不静默降级）", {
    WecomConfig cfg;
    cfg.msgtype = "image";  // 官方有 image 类型但 v1 不支持
    auto payload = module::schedule::buildWecomPayload(cfg, makeDelivery());
    DOCTEST_CHECK(payload.isFail());
    DOCTEST_CHECK_EQ(payload.errorCode(), core::ErrorCode::ValidationError);
    return true;
});

// 防：secret_ref 契约断——account 派生错导致读错 keychain 条目（拿错 key / 读不到 key）
REGISTER_TEST(wecomConfigParseDefaultsAndFields, "config_json 解码：空串全默认、全字段解析、secretRef 优先于派生、timeout≤0 兜底", {
    auto def = module::schedule::parseWecomConfig("", 7, "");
    DOCTEST_REQUIRE(def.isOk());
    DOCTEST_CHECK_EQ(def.value().msgtype, std::string("text"));
    DOCTEST_CHECK_EQ(def.value().proxyMode, std::string("follow_system"));
    DOCTEST_CHECK(def.value().endpoint.empty());
    DOCTEST_CHECK_EQ(def.value().timeoutMs, 10000L);
    DOCTEST_CHECK_EQ(module::schedule::wecomKeychainAccount(def.value()),
                     std::string("wecom.7"));  // secretRef 空 → 派生

    const std::string full = R"({"msgtype":"markdown","endpoint":"https://example.com/hook","mentions":["@all","zhangsan"],"mentioned_mobile_list":["13800000000"],"proxy_mode":"custom","proxy":"127.0.0.1:7890","timeout":5000,"future_field":1})";
    auto parsed = module::schedule::parseWecomConfig(full, 9, "wecom.9");
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK_EQ(parsed.value().msgtype, std::string("markdown"));
    DOCTEST_CHECK_EQ(parsed.value().endpoint, std::string("https://example.com/hook"));
    DOCTEST_CHECK_EQ(parsed.value().mentionUsers.size(), 2u);
    DOCTEST_CHECK_EQ(parsed.value().mentionUsers[0], std::string("@all"));
    DOCTEST_CHECK_EQ(parsed.value().mentionMobiles.size(), 1u);
    DOCTEST_CHECK_EQ(parsed.value().proxyMode, std::string("custom"));
    DOCTEST_CHECK_EQ(parsed.value().proxyAddr, std::string("127.0.0.1:7890"));
    DOCTEST_CHECK_EQ(parsed.value().timeoutMs, 5000L);
    DOCTEST_CHECK_EQ(module::schedule::wecomKeychainAccount(parsed.value()),
                     std::string("wecom.9"));  // secretRef 优先

    auto zeroTimeout = module::schedule::parseWecomConfig(R"({"timeout":0})", 1, "");
    DOCTEST_REQUIRE(zeroTimeout.isOk());
    DOCTEST_CHECK_EQ(zeroTimeout.value().timeoutMs, 10000L);
    return true;
});

// 防：脏配置进装配期静默运行——坏 JSON/错类型/非法值必须在装配期报错而不是投递期随机炸
REGISTER_TEST(wecomConfigParseInvalid, "config_json 非法输入全表：坏 JSON/非对象/非法 msgtype/非法 proxy_mode/mentions 非数组", {
    auto badJson = module::schedule::parseWecomConfig("not-json", 1, "");
    DOCTEST_CHECK(badJson.isFail());
    DOCTEST_CHECK_EQ(badJson.errorCode(), core::ErrorCode::JsonParseError);

    auto notObj = module::schedule::parseWecomConfig("[]", 1, "");
    DOCTEST_CHECK(notObj.isFail());
    DOCTEST_CHECK_EQ(notObj.errorCode(), core::ErrorCode::ValidationError);

    auto badMsgtype = module::schedule::parseWecomConfig(R"({"msgtype":"image"})", 1, "");
    DOCTEST_CHECK(badMsgtype.isFail());

    auto badProxy = module::schedule::parseWecomConfig(R"({"proxy_mode":"banana"})", 1, "");
    DOCTEST_CHECK(badProxy.isFail());

    auto missingAddr =
        module::schedule::parseWecomConfig(R"({"proxy_mode":"custom"})", 1, "");
    DOCTEST_CHECK(missingAddr.isFail());  // custom 缺 proxy 地址

    auto badMentions =
        module::schedule::parseWecomConfig(R"({"mentions":"@all"})", 1, "");
    DOCTEST_CHECK(badMentions.isFail());  // 非数组

    auto badMentionsItem =
        module::schedule::parseWecomConfig(R"({"mentions":[1,2]})", 1, "");
    DOCTEST_CHECK(badMentionsItem.isFail());  // 数组元素非字符串
    return true;
});

// 防：proxy_mode 不生效——公司网络必须走代理的机器人投递全失败，或该直连的走了错代理
REGISTER_TEST(wecomProxyModeMapping, "proxy_mode 三态映射（S6a）：follow_system/disabled/custom + custom 缺地址报错", {
    WecomConfig fs;
    fs.proxyMode = "follow_system";
    DOCTEST_CHECK_EQ(module::schedule::resolveWecomProxyMode(fs).value(),
                     platform::http::ProxyMode::FollowSystem);

    WecomConfig off;
    off.proxyMode = "disabled";
    DOCTEST_CHECK_EQ(module::schedule::resolveWecomProxyMode(off).value(),
                     platform::http::ProxyMode::Disabled);

    WecomConfig custom;
    custom.proxyMode = "custom";
    custom.proxyAddr = "127.0.0.1:7890";
    auto customRes = module::schedule::resolveWecomProxyMode(custom);
    DOCTEST_CHECK(customRes.isOk());
    DOCTEST_CHECK_EQ(customRes.value(), platform::http::ProxyMode::Custom);

    WecomConfig customEmpty;
    customEmpty.proxyMode = "custom";
    DOCTEST_CHECK(module::schedule::resolveWecomProxyMode(customEmpty).isFail());

    WecomConfig unknown;
    unknown.proxyMode = "socks5";
    DOCTEST_CHECK(module::schedule::resolveWecomProxyMode(unknown).isFail());
    return true;
});

// 防：端点/方法/头拼错、key 泄进 body（key 只允许在 URL 查询串）
REGISTER_TEST(wecomDeliverSuccessRequestShape, "投递成功路径：POST 官方端点 + ?key、JSON 头、body 含 msgtype 不含 key、errcode 0 → ok", {
    KeychainBackendGuard keychainGuard;
    WecomConfig cfg;
    cfg.channelDbId = 7;
    FakeHttp http;
    http.canned = httpResp(200, R"({"errcode":0,"errmsg":"ok"})");
    auto channel = makeChannel(cfg, http);

    auto receipt = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(receipt.isOk());
    DOCTEST_CHECK_EQ(receipt.value().status, std::string("ok"));
    DOCTEST_REQUIRE_EQ(http.requests.size(), 1u);
    const auto& req = http.requests[0];
    DOCTEST_CHECK_EQ(req.method, std::string("POST"));
    DOCTEST_CHECK_EQ(req.url,
                     std::string(module::schedule::kWecomDefaultEndpoint) +
                         "?key=" + std::string(kKey));
    auto contentType = req.headers.find("Content-Type");
    DOCTEST_REQUIRE(contentType != req.headers.end());
    DOCTEST_CHECK_EQ(contentType->second, std::string("application/json"));
    DOCTEST_CHECK_NE(req.body.find("\"msgtype\":\"text\""), std::string::npos);
    DOCTEST_CHECK_EQ(req.body.find(kKey), std::string::npos);  // key 不进 body
    // keychain account 走 secretRef 派生（secretRef 空 → wecom.<channelDbId>）
    DOCTEST_REQUIRE_EQ(FakeKeychainBackend::accounts.size(), 1u);
    DOCTEST_CHECK_EQ(FakeKeychainBackend::accounts[0], std::string("wecom.7"));
    return true;
});

// 防：key 泄进日志（§4.9 硬约束）——detail 是唯一进日志的字段，所有失败路径都不许带 key/URL
REGISTER_TEST(wecomDeliverKeyNeverInLogStrings, "凭据纪律：网络错/5xx/4xx/errcode 各失败路径的 Receipt.detail 均不含 key 与 ?key=", {
    KeychainBackendGuard keychainGuard;
    WecomConfig cfg;
    cfg.channelDbId = 7;
    FakeHttp http;
    auto channel = makeChannel(cfg, http);

    // 路径 1：网络错误
    http.canned = platform::http::Response{};
    http.canned.networkError = true;
    auto r1 = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_EQ(r1.value().status, std::string("retryable:network_error"));
    DOCTEST_CHECK_EQ(r1.value().detail.find(kKey), std::string::npos);
    DOCTEST_CHECK_EQ(r1.value().detail.find("?key="), std::string::npos);

    // 路径 2：HTTP 500
    http.canned = httpResp(500, "");
    auto r2 = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value().status, std::string("retryable:http_5xx"));
    DOCTEST_CHECK_EQ(r2.value().detail.find(kKey), std::string::npos);

    // 路径 3：HTTP 404
    http.canned = httpResp(404, "");
    auto r3 = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK_EQ(r3.value().status, std::string("permanent:http_4xx"));
    DOCTEST_CHECK_EQ(r3.value().detail.find(kKey), std::string::npos);

    // 路径 4：errcode 93000
    http.canned = httpResp(200, R"({"errcode":93000,"errmsg":"invalid webhook"})");
    auto r4 = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(r4.isOk());
    DOCTEST_CHECK_EQ(r4.value().status, std::string("permanent:errcode_93000"));
    DOCTEST_CHECK_EQ(r4.value().detail.find(kKey), std::string::npos);
    DOCTEST_CHECK_EQ(r4.value().detail.find("?key="), std::string::npos);

    // key 确实被用于请求（证明上面不是"没读到 key 所以没泄"）
    DOCTEST_CHECK(http.requests.back().url.find(kKey) != std::string::npos);
    return true;
});

// 防：缺凭据时裸 key 空串打请求——既白耗限频额度又污染远端日志
REGISTER_TEST(wecomDeliverMissingSecretNoHttp, "缺凭据：keychain 失败 → missing_secret 且不发 HTTP 请求", {
    KeychainBackendGuard keychainGuard;
    FakeKeychainBackend::failLoad = true;
    WecomConfig cfg;
    cfg.channelDbId = 7;
    FakeHttp http;
    auto channel = makeChannel(cfg, http);

    auto receipt = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(receipt.isOk());
    DOCTEST_CHECK_EQ(receipt.value().status, std::string("missing_secret"));
    DOCTEST_CHECK_EQ(http.requests.size(), 0u);
    return true;
});

// 防：本地限频失效突穿官方 20 条/分钟（与桶单测互补：验证 deliver 真的在桶后发请求）
REGISTER_TEST(wecomDeliverRateLimitedNoHttp, "限频端到端：1 分钟内第 19 次投递 → rate_limited 且不发 HTTP", {
    KeychainBackendGuard keychainGuard;
    WecomConfig cfg;
    cfg.channelDbId = 7;
    FakeHttp http;
    http.canned = httpResp(200, R"({"errcode":0})");
    auto channel = makeChannel(cfg, http);

    for (int i = 0; i < 18; ++i) {
        auto receipt = channel->deliver(makeDelivery());
        DOCTEST_REQUIRE(receipt.isOk());
        DOCTEST_CHECK_EQ(receipt.value().status, std::string("ok"));
    }
    auto nineteenth = channel->deliver(makeDelivery());
    DOCTEST_REQUIRE(nineteenth.isOk());
    DOCTEST_CHECK_EQ(nineteenth.value().status, std::string("rate_limited"));
    DOCTEST_CHECK_EQ(http.requests.size(), 18u);  // 第 19 条未出网
    return true;
});

// 防：只看 HTTP 状态漏判业务错（§4.9 错误判定硬约束）——HTTP 200 但 errcode 失败必须分类
REGISTER_TEST(wecomDeliverErrcodeStatusStrings, "投递错误判定：必须解析 body errcode——45009/45033/未知码可重试，93000/93001/93004/93017 永久", {
    KeychainBackendGuard keychainGuard;
    WecomConfig cfg;
    cfg.channelDbId = 7;
    FakeHttp http;
    auto channel = makeChannel(cfg, http);

    http.canned = httpResp(200, R"({"errcode":45009,"errmsg":"hit freq limit"})");
    auto r1 = channel->deliver(makeDelivery());
    DOCTEST_CHECK_EQ(r1.value().status, std::string("retryable:errcode_45009"));
    DOCTEST_CHECK_NE(r1.value().detail.find("60"), std::string::npos);

    http.canned = httpResp(200, R"({"errcode":45033})");
    auto r2 = channel->deliver(makeDelivery());
    DOCTEST_CHECK_EQ(r2.value().status, std::string("retryable:errcode_45033"));

    http.canned = httpResp(200, R"({"errcode":93000})");
    auto r3 = channel->deliver(makeDelivery());
    DOCTEST_CHECK_EQ(r3.value().status, std::string("permanent:errcode_93000"));
    DOCTEST_CHECK_NE(r3.value().detail.find("停用"), std::string::npos);

    http.canned = httpResp(200, R"({"errcode":93001})");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("permanent:errcode_93001"));
    http.canned = httpResp(200, R"({"errcode":93004})");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("permanent:errcode_93004"));
    http.canned = httpResp(200, R"({"errcode":93017})");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("permanent:errcode_93017"));

    http.canned = httpResp(200, R"({"errcode":99999})");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("retryable:errcode_99999"));  // 未知码按可重试

    http.canned = httpResp(500, "");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("retryable:http_5xx"));
    http.canned = httpResp(404, "");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("permanent:http_4xx"));
    http.canned = httpResp(200, "not-json");
    DOCTEST_CHECK_EQ(channel->deliver(makeDelivery()).value().status,
                     std::string("retryable:bad_body"));  // 无合法 errcode 无法判定

    http.failResult = true;  // 传输层 Result::fail
    auto rNet = channel->deliver(makeDelivery());
    DOCTEST_CHECK_EQ(rNet.value().status, std::string("retryable:network_error"));
    return true;
});

// 防：重复装配换掉已上线渠道实现——注册表契约要求重复 id 幂等保留首个（与 S5 同口径）
REGISTER_TEST(wecomRegisterSeamIdempotent, "注册接缝：registerWecomChannel 注册后可查找、重复注册幂等不替换", {
    module::schedule::ChannelRegistry registry;
    registry.resetForTest();
    WecomConfig cfg;
    cfg.channelDbId = 7;

    auto first = module::schedule::registerWecomChannel(registry, cfg);
    DOCTEST_CHECK(first.isOk());
    DOCTEST_CHECK_NE(registry.find(module::schedule::kChannelWecom),
                     static_cast<const module::schedule::Channel*>(nullptr));

    auto dup = module::schedule::registerWecomChannel(registry, cfg);
    DOCTEST_CHECK(dup.isOk());  // AlreadyExists 被吞掉 → 幂等
    DOCTEST_CHECK_NE(registry.find(module::schedule::kChannelWecom),
                     static_cast<const module::schedule::Channel*>(nullptr));
    registry.resetForTest();
    return true;
});
