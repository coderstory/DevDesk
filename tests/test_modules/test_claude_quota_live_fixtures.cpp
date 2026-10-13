// tests/test_modules/test_claude_quota_live_fixtures.cpp
// 用脱敏后的真实服务端响应体做 fixture，通过假发送回放，零真实网络。
// 跨文件前提：
//   - 本文件**不含任何真实密钥 / token / AK / SK**。若将来某家上游开始回显凭据，
//     **先打码再贴进来**，不要原样粘贴。
//   - 各 provider 的查询端点写死在各自实现里，与 profile 的 base_url 不是同一档：
//     opencode_go 走 /zen/go/v1/usage（base_url 只到 /zen/go）；
//     minimax_cn 走旧域 api.minimaxi.com（base_url 是 api.minimax.cn）；
//     volcano 走 open.volcengineapi.com 控制面 + 从 base_url 的 host 段提取 Region。
//   - volcano 用控制面 AK/SK 签名，不是 profile 的推理 key（两套凭据）。
//   - SSE / JSON 解析按 yyjson 语义：整数 percent 不许被当成 0。
#include "doctest.h"

#include "core/result.h"
#include "platform/http_client.h"
#include "quota/deepseek_provider.h"
#include "quota/opencode_go_provider.h"
#include "quota/provider_minimax_cn.h"
#include "quota/quota_error_text.h"
#include "quota/quota_volcano.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

using module::claude::quota::createDeepSeekProvider;
using module::claude::quota::createMinimaxCn;
using module::claude::quota::createOpenCodeGoProvider;
using module::claude::quota::HttpSendFn;
using module::claude::quota::ProviderConfig;
using module::claude::quota::QuotaError;
using module::claude::quota::QuotaKind;
using module::claude::quota::QuotaProvider;
using module::claude::quota::QuotaResult;
using module::claude::quota::volcanoProvider;

struct CapturedHttp {
    platform::http::Request lastReq;
    int calls = 0;
};

HttpSendFn httpReturningBody(CapturedHttp& out, int status, const std::string& body) {
    return [&out, status, body](const platform::http::Request& req)
               -> core::Result<platform::http::Response> {
        out.lastReq = req;
        out.calls += 1;
        platform::http::Response resp;
        resp.statusCode = status;
        resp.body = body;
        return core::Result<platform::http::Response>::ok(std::move(resp));
    };
}

// 火山一次查询要连打两个 Action（GetAFPUsage → GetCodingPlanUsage），按调用序
// 依次回放抓到的两份响应。
HttpSendFn httpReturningSequence(CapturedHttp& out, const std::vector<int>& statuses,
                                 const std::vector<std::string>& bodies) {
    return [&out, statuses, bodies](const platform::http::Request& req)
               -> core::Result<platform::http::Response> {
        out.lastReq = req;
        const int index = out.calls;
        out.calls += 1;
        platform::http::Response resp;
        resp.statusCode = statuses[static_cast<std::size_t>(index)];
        resp.body = bodies[static_cast<std::size_t>(index)];
        return core::Result<platform::http::Response>::ok(std::move(resp));
    };
}

ProviderConfig configWithKey(const std::string& baseUrl, const std::string& apiKey) {
    ProviderConfig cfg;
    cfg.providerId = "x";
    cfg.baseUrl = baseUrl;
    cfg.apiKey = apiKey;
    return cfg;
}

}  // namespace

REGISTER_TEST(claude_quota_live_opencode_go_real_body,
              "claude_quota_live: opencode_go 真机响应体（2026-10-03）→ 三个窗口 + 占位重置被丢弃", {
    // percent 是**已用**百分数；rolling=0 时上游给的 resetsAt 是「now + 窗口
    // 时长」的占位值，按上游规则必须丢弃。
    const char* body = R"({"usage":{"rolling":{"status":"ok","percent":0,"resetsAt":"2026-10-02T23:50:07.738Z"},"weekly":{"status":"ok","percent":97,"resetsAt":"2026-10-05T00:00:00.000Z"},"monthly":{"status":"ok","percent":82,"resetsAt":"2026-10-23T09:06:17.000Z"}}})";
    CapturedHttp fake;
    QuotaProvider* p = createOpenCodeGoProvider();
    const QuotaResult r = p->fetch(
        configWithKey("https://opencode.ai/zen/go", "sk-placeholder-not-a-real-key"),
        httpReturningBody(fake, 200, body));

    DOCTEST_CHECK_EQ(fake.calls, 1);
    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.kind == QuotaKind::Tiered);
    DOCTEST_REQUIRE_EQ(r.windows.size(), 3u);
    // 窗口 key 按**上游 key 名**映射（rolling→five_hour / weekly / monthly），
    // 不是按 resetsAt 排序猜的。
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[1].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(r.windows[2].key, std::string("monthly"));
    // 整数 percent 不许被读成 0（yyjson Int/Double 双分支陷阱）
    DOCTEST_CHECK_EQ(r.windows[1].usedPercent, 97.0);
    DOCTEST_CHECK_EQ(r.windows[2].usedPercent, 82.0);
    // rolling percent==0 → 占位 resetsAt 丢弃，窗口本身保留
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 0.0);
    DOCTEST_CHECK(r.windows[0].resetsAt.empty());
    DOCTEST_CHECK_EQ(fake.lastReq.url, std::string("https://opencode.ai/zen/go/v1/usage"));
    return true;
})

REGISTER_TEST(claude_quota_live_deepseek_real_body,
              "claude_quota_live: deepseek 真机响应体（2026-10-03）→ Balance 字符串小数", {
    // total_balance 是**字符串** "5.99"（不是 JSON 数字）——这正是 numberField
    // 要同时收两种形态的原因。
    const char* body = R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"5.99","granted_balance":"6.00","topped_up_balance":"-0.00"}]})";
    CapturedHttp fake;
    QuotaProvider* p = createDeepSeekProvider();
    const QuotaResult r = p->fetch(
        configWithKey("https://api.deepseek.com/anthropic", "sk-placeholder-not-a-real-key"),
        httpReturningBody(fake, 200, body));

    DOCTEST_CHECK_EQ(fake.calls, 1);
    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.kind == QuotaKind::Balance);
    DOCTEST_CHECK_EQ(r.balanceRemaining, 5.99);
    DOCTEST_CHECK_EQ(r.currency, std::string("CNY"));
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_EQ(fake.lastReq.url, std::string("https://api.deepseek.com/user/balance"));
    return true;
})

REGISTER_TEST(claude_quota_live_minimax_expired_plan_is_entitlement,
              "claude_quota_live: minimax 真机「套餐过期」响应 → Entitlement（不是 Transient / 不是 Auth）", {
    // HTTP **200**，但 body 是业务失败。`model_remains` 是 null（订阅已失效，
    // 上游连数组都不给）——解析必须**先**看 base_resp，否则会落进「响应缺少
    // model_remains 数组」的 Shape，把「套餐过期」报成「供应商改了接口」。
    const char* body = R"({"model_remains":null,"base_resp":{"status_code":2062,"status_msg":"no active token plan subscription"}})";
    CapturedHttp fake;
    QuotaProvider* p = createMinimaxCn();
    const QuotaResult r = p->fetch(
        configWithKey("https://api.minimax.cn/anthropic", "sk-placeholder-not-a-real-key"),
        httpReturningBody(fake, 200, body));

    DOCTEST_CHECK_EQ(fake.calls, 1);
    DOCTEST_CHECK(r.error == QuotaError::Entitlement);
    // 反向钉死：Transient 会把用户引去反复重试，Auth 会引去换一把**本来正确**
    // 的密钥，Shape 是「供应商改了接口」——三者都指向错误方向。
    DOCTEST_CHECK(r.error != QuotaError::Transient);
    DOCTEST_CHECK(r.error != QuotaError::Auth);
    DOCTEST_CHECK(r.error != QuotaError::Shape);
    DOCTEST_CHECK(r.errorMessage.find("2062") != std::string::npos);
    DOCTEST_CHECK(r.errorMessage.find("no active token plan subscription") != std::string::npos);
    DOCTEST_CHECK(r.windows.empty());
    // 用户可见文案的方向必须对：说「换密钥没用」，不是「重试」
    const std::string headline = module::claude::quota::quotaErrorHeadline(
        r.error, std::string());
    DOCTEST_CHECK(headline.find("换密钥没用") != std::string::npos);
    DOCTEST_CHECK(headline.find("重试") == std::string::npos);
    DOCTEST_CHECK_EQ(fake.lastReq.url,
                     std::string("https://api.minimaxi.com/v1/api/openplatform/coding_plan/remains"));
    return true;
})

REGISTER_TEST(claude_quota_live_volcano_real_coding_plan,
              "claude_quota_live: volcano 真机响应体（2026-10-03）→ 签名通过 + AFP 零额度回落 Coding Plan", {
    // GetAFPUsage。HTTP 200、签名被网关接受（ResponseMetadata 里没有 Error），
    // 但账号买的是 Coding Plan 不是 Agent Plan → 四个窗口 Quota 全 0。
    const char* afpBody = R"({"ResponseMetadata":{"RequestId":"LIVE-CAPTURED-REQUEST-ID-0000","Action":"GetAFPUsage","Version":"2024-01-01","Service":"ark","Region":"cn-beijing"},"Result":{"PlanType":"","AFPFiveHour":{"Quota":0,"Used":0,"SubscribeTime":0,"ResetTime":0},"AFPWeekly":{"Quota":0,"Used":0,"SubscribeTime":0,"ResetTime":0},"AFPMonthly":{"Quota":0,"Used":0,"SubscribeTime":0,"ResetTime":0},"AFPDaily":{"Quota":0,"Used":0,"SubscribeTime":0,"ResetTime":0}}})";
    // GetCodingPlanUsage。Level 逐条映射周期（session/weekly/monthly），
    // ResetTimestamp:-1 = 无活跃会话 → 无重置时间；其余是**秒级**时间戳。
    const char* codingBody = R"({"ResponseMetadata":{"RequestId":"LIVE-CAPTURED-REQUEST-ID-0001","Action":"GetCodingPlanUsage","Version":"2024-01-01","Service":"ark","Region":"cn-beijing"},"Result":{"Status":"Running","UpdateTimestamp":1790978979,"QuotaUsage":[{"Level":"session","Percent":0,"ResetTimestamp":-1,"Cap":100,"RewardTotalPercent":0},{"Level":"weekly","Percent":95.40395933333333,"ResetTimestamp":1791129600,"Cap":100,"RewardTotalPercent":0},{"Level":"monthly","Percent":47.701979666666666,"ResetTimestamp":1793289599,"Cap":100,"RewardTotalPercent":0}],"HasReward":false}})";

    CapturedHttp fake;
    QuotaProvider* p = volcanoProvider();
    ProviderConfig cfg = configWithKey("https://ark.cn-beijing.volces.com/api/coding",
                                       "ark-placeholder-not-a-real-key");
    cfg.fields["accessKeyId"] = "AKplaceholder0000000000000000000000";
    cfg.fields["secretAccessKey"] = "SKplaceholder000000000000000000000000000000000000000000";

    const QuotaResult r = p->fetch(
        cfg, httpReturningSequence(fake, {200, 200}, {afpBody, codingBody}));

    // 两个 Action 各打一次，且**顺序**是先 AFP 后 Coding Plan（httpReturningSequence 按调用序回放）
    DOCTEST_CHECK_EQ(fake.calls, 2);
    DOCTEST_CHECK(fake.lastReq.url.find("Action=GetCodingPlanUsage") != std::string::npos);
    // region 从 base_url 的 host 段提取：ark.cn-beijing.volces.com → cn-beijing。
    // 取错会签名 scope 错、被网关拒签，而错误文案还把用户引向「检查 AK/SK」。
    DOCTEST_CHECK(fake.lastReq.url.find("Region=cn-beijing") != std::string::npos);
    DOCTEST_CHECK(fake.lastReq.url.find("Version=2024-01-01") != std::string::npos);
    DOCTEST_CHECK(fake.lastReq.url.find("https://open.volcengineapi.com/?") == 0u);

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.planName, std::string("Coding Plan"));
    DOCTEST_REQUIRE_EQ(r.windows.size(), 3u);
    // Level→规范键映射，**不按 ResetTimestamp 排序**：session 的 ResetTimestamp
    // 是 -1，排序会把它排到 weekly/monthly 后面，周期全错。
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[1].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(r.windows[2].key, std::string("monthly"));
    // 小数不截断（上游给 15 位小数）
    DOCTEST_CHECK(r.windows[1].usedPercent > 95.40 && r.windows[1].usedPercent < 95.41);
    DOCTEST_CHECK(r.windows[2].usedPercent > 47.70 && r.windows[2].usedPercent < 47.71);
    // ResetTimestamp == -1 = 无活跃窗口 → 空串（不是「1970 年」）
    DOCTEST_CHECK(r.windows[0].resetsAt.empty());
    DOCTEST_CHECK_EQ(r.windows[1].resetsAt, std::string("2026-10-04T16:00:00Z"));
    return true;
})

REGISTER_TEST(claude_quota_live_volcano_signature_rejected_still_names_credential_and_signature,
              "claude_quota_live: 签名被拒（400 + 错误信封）→ Auth 且文案同时点出 AK/SK 与签名两条方向", {
    // 「AK/SK 不对」会长什么样：HTTP 400 + 与 200 路径同款的
    // ResponseMetadata.Error 信封（不是 401/403）。文案必须同时点出
    // 「AK/SK 是否正确」与「签名是否被接受」两条排查方向。
    const char* rejected = R"({"ResponseMetadata":{"Error":{"Code":"InvalidAuthorization","Message":"The request signature we calculated does not match the signature you provided."}}})";
    CapturedHttp fake;
    QuotaProvider* p = volcanoProvider();
    ProviderConfig cfg = configWithKey("https://ark.cn-beijing.volces.com/api/coding",
                                       "ark-placeholder-not-a-real-key");
    cfg.fields["accessKeyId"] = "AKplaceholder0000000000000000000000";
    cfg.fields["secretAccessKey"] = "SKplaceholder000000000000000000000000000000000000000000";

    const QuotaResult r = p->fetch(cfg, httpReturningBody(fake, 400, rejected));

    DOCTEST_CHECK(r.error == QuotaError::Auth);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK(r.errorMessage.find("AK/SK") != std::string::npos);
    DOCTEST_CHECK(r.errorMessage.find("签名") != std::string::npos);
    return true;
})
