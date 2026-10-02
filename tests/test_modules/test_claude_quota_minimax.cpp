// tests/test_modules/test_claude_quota_minimax.cpp
// G2：MiniMax 中国站余量 provider 契约测试
// （计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G2）
//
// 被测：src/module_claude_proxy/quota/provider_minimax_cn.{h,cpp}
// 移植自 cc-switch `src-tauri/src/services/coding_plan.rs`：
//   query_minimax:426-509 / parse_minimax_tiers:657-712 / detect_provider:38-44
//
// 全用例零真实网络：HTTP 一律经 HttpSendFn 注入缝塞假发送（G0 契约，quota_provider.h:109-118）。
#include "doctest.h"

#include "core/result.h"
#include "platform/http_client.h"
#include "quota/provider_minimax_cn.h"
#include "quota/quota_provider.h"
#include "quota/quota_registry.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

using module::claude::quota::createMinimaxCn;
using module::claude::quota::HttpSendFn;
using module::claude::quota::ProviderConfig;
using module::claude::quota::QuotaError;
using module::claude::quota::QuotaKind;
using module::claude::quota::QuotaProvider;
using module::claude::quota::QuotaResult;

// ---- 假发送：记录最后一次请求 + 返回固定 HTTP 响应（零真实网络）----
struct FakeHttp {
    platform::http::Request lastReq;
    int calls = 0;
};

HttpSendFn httpReturning(FakeHttp& out, int status, const std::string& body) {
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

// 传输层失败（连接不上 / 超时）：platform::http::send 的失败形态
HttpSendFn httpFailing(FakeHttp& out) {
    return [&out](const platform::http::Request&) -> core::Result<platform::http::Response> {
        out.calls += 1;
        return core::Result<platform::http::Response>::fail(core::ErrorCode::NetworkUnreachable,
                                                            "connection refused");
    };
}

// 中国站 profile：base_url 是 api.minimax.cn，但额度接口要打旧域 api.minimaxi.com
ProviderConfig cnConfig() {
    ProviderConfig cfg;
    cfg.providerId = "minimax_cn";
    cfg.baseUrl = "https://api.minimax.cn/v1";
    cfg.apiKey = "sk-cn-test-key";
    return cfg;
}

// 正常响应：general 在数组**第二**位（video 在前），5h 剩 98% / 周剩 95%，
// 两个重置时间都为整数毫秒。数字全部为 cc-switch 单测用过的取值。
const char* kNormalBody = R"({
  "model_remains": [
    {
      "model_name": "video",
      "current_interval_remaining_percent": 100.0,
      "current_weekly_remaining_percent": 100.0
    },
    {
      "model_name": "general",
      "current_interval_remaining_percent": 98.0,
      "current_interval_status": 1,
      "end_time": 1780329600000,
      "current_weekly_status": 1,
      "current_weekly_remaining_percent": 95.0,
      "weekly_end_time": 1780848000000
    }
  ],
  "base_resp": { "status_code": 0, "status_msg": "success" }
})";

}  // namespace

// ==================== 身份 / fields / 探测 ====================

REGISTER_TEST(claude_quota_minimax_identity_and_registry_row,
              "claude_quota: minimax_cn 身份（id/显示名/空 fields）与注册表行", {
    const QuotaProvider* fromRegistry = module::claude::quota::findProvider("minimax_cn");
    DOCTEST_CHECK(fromRegistry != nullptr);
    DOCTEST_CHECK_EQ(fromRegistry->id(), std::string("minimax_cn"));
    DOCTEST_CHECK_EQ(fromRegistry->displayName(), std::string("MiniMax 中国站"));
    // fields() 为空 = 沿用 profile 的 api key（不额外要字段）
    DOCTEST_CHECK(fromRegistry->fields().empty());
    // 工厂单例：注册表行与测试拿到的是同一个实例（只借不还，无泄漏）
    DOCTEST_CHECK(fromRegistry == createMinimaxCn());
    return true;
})

REGISTER_TEST(claude_quota_minimax_matches_base_url_label_boundary,
              "claude_quota: minimax_cn 探测两个 host 命中、伪造后缀不命中", {
    QuotaProvider* p = createMinimaxCn();
    DOCTEST_CHECK(p != nullptr);
    // 两个官方 host 都命中（新旧域名同一账号体系）
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.minimaxi.com/v1"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.minimax.cn/v1"));
    // 容忍用户手填的松散形态：裸 host / 大写 / 带端口 / 尾点
    DOCTEST_CHECK(p->matchesBaseUrl("api.minimax.cn"));
    DOCTEST_CHECK(p->matchesBaseUrl("HTTPS://API.MINIMAX.CN/v1"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.minimax.cn:443/v1"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.minimaxi.com./v1"));
    // **标签边界**：不是子串包含——伪造后缀 / 前缀粘接都必须落空
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.minimax.cn.example.com/v1"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://evilapi.minimax.cn/v1"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.minimaxi.com.evil.com/v1"));
    // 国际站（api.minimax.io）不归本 provider，且 host 不同不会误命中
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.minimax.io/v1"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl(""));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.deepseek.com"));
    return true;
})

REGISTER_TEST(claude_quota_minimax_detect_by_base_url,
              "claude_quota: detectByBaseUrl 认出中国站两档 base_url", {
    const QuotaProvider* cn =
        module::claude::quota::detectByBaseUrl("https://api.minimax.cn/v1");
    DOCTEST_CHECK(cn != nullptr);
    DOCTEST_CHECK_EQ(cn->id(), std::string("minimax_cn"));
    const QuotaProvider* legacy =
        module::claude::quota::detectByBaseUrl("https://api.minimaxi.com/v1");
    DOCTEST_CHECK(legacy != nullptr);
    DOCTEST_CHECK_EQ(legacy->id(), std::string("minimax_cn"));
    // 伪造后缀不被自动选中（选错 = 把别人的额度查到当前 profile 上）
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl(
                      "https://api.minimax.cn.example.com/v1") == nullptr);
    return true;
})

// ==================== 请求形状（旧域硬编码）====================

REGISTER_TEST(claude_quota_minimax_requests_legacy_host_even_for_cn_profile,
              "claude_quota: 即使 profile base_url 是 .cn，请求也打旧域 api.minimaxi.com", {
    QuotaProvider* p = createMinimaxCn();
    FakeHttp fake;
    const ProviderConfig cfg = cnConfig();  // baseUrl = https://api.minimax.cn/v1
    const QuotaResult r = p->fetch(cfg, httpReturning(fake, 200, kNormalBody));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(fake.calls, 1);
    DOCTEST_CHECK_EQ(fake.lastReq.method, std::string("GET"));
    // 旧域是**写死**的：不从 cfg.baseUrl 派生（cc-switch coding_plan.rs:429-436）
    DOCTEST_CHECK_EQ(fake.lastReq.url,
                     std::string("https://api.minimaxi.com/v1/api/openplatform/"
                                 "coding_plan/remains"));
    DOCTEST_CHECK_EQ(fake.lastReq.headers.at("Authorization"),
                     std::string("Bearer sk-cn-test-key"));
    DOCTEST_CHECK_EQ(fake.lastReq.headers.at("Content-Type"), std::string("application/json"));
    DOCTEST_CHECK_EQ(fake.lastReq.timeoutMs, 15000L);
    return true;
})

// ==================== 正常解析：反转 + 周门控 ====================

REGISTER_TEST(claude_quota_minimax_inverts_remaining_percent,
              "claude_quota: 五小时/周 usedPercent = 100 - 剩余（反转，钉具体数值）", {
    QuotaProvider* p = createMinimaxCn();
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, kNormalBody));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.errorMessage.empty());
    DOCTEST_CHECK(r.kind == QuotaKind::Tiered);
    // 两个窗口：five_hour + weekly（**无月窗口**——上游不提供，不许硬造）
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    // 上游给「剩余 98%」→ 已用 2%（写反了就是 98，UI 全错）
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 2.0);
    DOCTEST_CHECK_EQ(r.windows[0].resetsAt, std::string("2026-06-01T16:00:00Z"));
    DOCTEST_CHECK_EQ(r.windows[1].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(r.windows[1].usedPercent, 5.0);
    DOCTEST_CHECK_EQ(r.windows[1].resetsAt, std::string("2026-06-07T16:00:00Z"));
    // Tiered 型不许夹带余额字段
    DOCTEST_CHECK_EQ(r.balanceRemaining, 0.0);
    DOCTEST_CHECK(r.currency.empty());
    return true;
})

REGISTER_TEST(claude_quota_minimax_weekly_status_not_1_hides_weekly_window,
              "claude_quota: current_weekly_status != 1 → 只出五小时窗口（不画 0% 假周条）", {
    QuotaProvider* p = createMinimaxCn();
    // 无周限额套餐：status=3，remaining_percent 恒 100（画出来就是「已用 0%」的假象）
    const char* body = R"({
      "model_remains": [
        {
          "model_name": "general",
          "current_interval_remaining_percent": 99,
          "current_interval_status": 1,
          "current_weekly_status": 3,
          "current_weekly_remaining_percent": 100,
          "current_weekly_total_count": 0,
          "current_weekly_usage_count": 0
        }
      ],
      "base_resp": { "status_code": 0, "status_msg": "success" }
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 1.0);  // 100 - 99，整数形态一样反转
    // 没有周窗口（不是「周窗口 0%」）
    return true;
})

REGISTER_TEST(claude_quota_minimax_weekly_status_2_also_skips_weekly,
              "claude_quota: 周 status 除 1 之外都视为未激活（status=2 同样不出周条）", {
    QuotaProvider* p = createMinimaxCn();
    const char* body = R"({
      "model_remains": [
        {
          "model_name": "general",
          "current_interval_remaining_percent": 80.0,
          "current_weekly_remaining_percent": 50.0,
          "current_weekly_status": 2
        }
      ]
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 20.0);
    return true;
})

REGISTER_TEST(claude_quota_minimax_no_range_clamping,
              "claude_quota: 百分比不做 0-100 裁剪（与 cc-switch 约定一致，double 不截断）", {
    QuotaProvider* p = createMinimaxCn();
    const char* body = R"({
      "model_remains": [
        {
          "model_name": "general",
          "current_interval_remaining_percent": -5.0,
          "current_interval_status": 1,
          "current_weekly_remaining_percent": 150.0,
          "current_weekly_status": 1
        }
      ]
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 105.0);  // 100 - (-5)
    DOCTEST_CHECK_EQ(r.windows[1].usedPercent, -50.0);  // 100 - 150
    return true;
})

REGISTER_TEST(claude_quota_minimax_weekly_percent_missing_keeps_five_hour,
              "claude_quota: 周桶字段缺失只跳过该桶，五小时仍出（照上游逐桶防御解析）", {
    QuotaProvider* p = createMinimaxCn();
    const char* body = R"({
      "model_remains": [
        {
          "model_name": "general",
          "current_interval_remaining_percent": 60.0,
          "current_weekly_status": 1
        }
      ]
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 40.0);
    DOCTEST_CHECK(r.windows[0].resetsAt.empty());  // 无 end_time → 无重置时间，不编造
    return true;
})

// ==================== 错误分类 ====================

REGISTER_TEST(claude_quota_minimax_business_error_on_http_200,
              "claude_quota: HTTP 200 + base_resp.status_code != 0 → Entitlement（套餐/额度类，不是可重试）", {
    QuotaProvider* p = createMinimaxCn();
    // HTTP 200 也可能是失败：真实错误码在 body.base_resp 里
    const char* body = R"({
      "base_resp": { "status_code": 1027, "status_msg": "service busy" }
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    DOCTEST_CHECK_EQ(fake.calls, 1);
    DOCTEST_CHECK(r.error != QuotaError::None);
    // 业务失败 → Entitlement：改密钥没用、得看套餐（G8 不会挂「可能过期」角标了事）
    DOCTEST_CHECK(r.error == QuotaError::Entitlement);
    // 反向钉死：不许回退成 Transient（那会把「套餐过期」引导成「网络抖动，重试」）
    DOCTEST_CHECK(r.error != QuotaError::Transient);
    // 失败结果里不许夹带半截成功值（G7 五态：不画 0% 条）
    DOCTEST_CHECK(r.windows.empty());
    // 上游错误码与文案必须原样透出（用户/日志才能定位）
    DOCTEST_CHECK(r.errorMessage.find("1027") != std::string::npos);
    DOCTEST_CHECK(r.errorMessage.find("service busy") != std::string::npos);
    // 且要给出正确引导：套餐 / 额度，而不是「重试」
    DOCTEST_CHECK(r.errorMessage.find("过期") != std::string::npos);
    return true;
})

REGISTER_TEST(claude_quota_minimax_base_resp_without_status_code_is_business_error,
              "claude_quota: base_resp 存在但 status_code 非整数 → 同样按业务失败走 Entitlement（照上游 -1 语义）", {
    QuotaProvider* p = createMinimaxCn();
    // cc-switch `unwrap_or(-1)` + `!= 0` → 缺字段 = 业务失败，不是「当成功继续解析」
    const char* body = R"({
      "model_remains": [
        { "model_name": "general", "current_interval_remaining_percent": 90.0 }
      ],
      "base_resp": { "status_msg": "gateway hiccup" }
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    DOCTEST_CHECK(r.error == QuotaError::Entitlement);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK(r.errorMessage.find("gateway hiccup") != std::string::npos);
    return true;
})

REGISTER_TEST(claude_quota_minimax_missing_general_is_shape,
              "claude_quota: model_remains 里没有 general（只有 video）→ Shape", {
    QuotaProvider* p = createMinimaxCn();
    const char* body = R"({
      "model_remains": [
        {
          "model_name": "video",
          "current_interval_remaining_percent": 100.0,
          "current_weekly_remaining_percent": 100.0,
          "current_weekly_status": 1
        }
      ],
      "base_resp": { "status_code": 0, "status_msg": "success" }
    })";
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));

    // 结构不认识 = 该报错，不是「返回空卡」（G0 契约 2 / 约定 QuotaError::Shape）
    DOCTEST_CHECK(r.error == QuotaError::Shape);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_FALSE(r.errorMessage.empty());
    return true;
})

REGISTER_TEST(claude_quota_minimax_unparsable_shape_variants,
              "claude_quota: 缺 model_remains / 空数组 / general 无百分比 / 非 JSON → Shape", {
    QuotaProvider* p = createMinimaxCn();
    const char* cases[] = {
        R"({ "result": "no such key" })",                                  // 缺 model_remains
        R"({ "model_remains": [] })",                                       // 空数组
        R"({ "model_remains": [ { "model_name": "general" } ] })",          // general 无字段
        R"({ "model_remains": [ { "model_name": "general", )",              // 截断 JSON
        R"(not json at all)",                                               // 完全不是 JSON
    };
    for (const char* body : cases) {
        FakeHttp fake;
        const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, 200, body));
        DOCTEST_CHECK(r.error == QuotaError::Shape);
        DOCTEST_CHECK(r.windows.empty());
    }
    return true;
})

REGISTER_TEST(claude_quota_minimax_http_auth_statuses_map_to_auth,
              "claude_quota: HTTP 401/403 → Auth（照上游：403 也是「密钥无效」）", {
    QuotaProvider* p = createMinimaxCn();
    for (const int status : {401, 403}) {
        FakeHttp fake;
        const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, status, ""));
        DOCTEST_CHECK(r.error == QuotaError::Auth);
        DOCTEST_CHECK(r.windows.empty());
        DOCTEST_CHECK_FALSE(r.errorMessage.empty());
    }
    return true;
})

REGISTER_TEST(claude_quota_minimax_http_other_statuses_map_to_transient,
              "claude_quota: 其它非 2xx（429/500/404）→ Transient（可重试口径，与 G1 计划同）", {
    QuotaProvider* p = createMinimaxCn();
    for (const int status : {429, 500, 404}) {
        FakeHttp fake;
        const QuotaResult r = p->fetch(cnConfig(), httpReturning(fake, status, "boom"));
        DOCTEST_CHECK(r.error == QuotaError::Transient);
        DOCTEST_CHECK(r.windows.empty());
    }
    return true;
})

REGISTER_TEST(claude_quota_minimax_transport_failure_is_transient,
              "claude_quota: 传输层失败（Result::fail）→ Transient", {
    QuotaProvider* p = createMinimaxCn();
    FakeHttp fake;
    const QuotaResult r = p->fetch(cnConfig(), httpFailing(fake));

    DOCTEST_CHECK_EQ(fake.calls, 1);
    DOCTEST_CHECK(r.error == QuotaError::Transient);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK(r.errorMessage.find("connection refused") != std::string::npos);
    return true;
})

REGISTER_TEST(claude_quota_minimax_empty_http_seam_returns_transient,
              "claude_quota: 空 HttpSendFn → Transient（不许 std::bad_function_call）", {
    QuotaProvider* p = createMinimaxCn();
    const HttpSendFn noHttp;  // 空缝
    const ProviderConfig cfg = cnConfig();
    const QuotaResult r = p->fetch(cfg, noHttp);  // 抛异常即用例炸，红

    DOCTEST_CHECK(r.error == QuotaError::Transient);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_FALSE(r.errorMessage.empty());
    return true;
})

REGISTER_TEST(claude_quota_minimax_empty_api_key_is_not_configured,
              "claude_quota: 空 API 密钥 → NotConfigured（不发请求）", {
    QuotaProvider* p = createMinimaxCn();
    FakeHttp fake;
    ProviderConfig cfg = cnConfig();
    cfg.apiKey.clear();
    const QuotaResult r = p->fetch(cfg, httpReturning(fake, 200, kNormalBody));

    DOCTEST_CHECK(r.error == QuotaError::NotConfigured);
    DOCTEST_CHECK_EQ(fake.calls, 0);  // 配置不完整不该打网络
    DOCTEST_CHECK(r.windows.empty());
    return true;
})
