// tests/test_modules/test_claude_quota.cpp
// 供应商余量抽象层（G0）契约测试
// （计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G0）
//
// 覆盖：
//   - 显式注册表（非静态自注册）：命中 / 未知 id / 空 id / 空表 / 重复 id 首行优先
//   - 注册表结构守卫：哨兵结尾、id 非空且无重复（G1-G4 每加一行自动进入本覆盖）
//   - detectByBaseUrl：命中 / 不命中 / 空串 / 未覆写默认不命中
//   - QuotaResult 默认值语义（逐字段）
//   - ProviderField 驱动契约（含 secret 标记 / 空 fields）
//   - fetch 契约：恒返回 QuotaResult、错误只走 error 字段、HttpSendFn 注入缝真被调用
#include "doctest.h"

#include "quota/opencode_go_provider.h"
#include "quota/deepseek_provider.h"
#include "quota/quota_provider.h"
#include "quota/quota_registry.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

using module::claude::quota::HttpSendFn;
using module::claude::quota::ProviderConfig;
using module::claude::quota::ProviderField;
using module::claude::quota::QuotaError;
using module::claude::quota::QuotaKind;
using module::claude::quota::QuotaProvider;
using module::claude::quota::QuotaProviderEntry;
using module::claude::quota::QuotaResult;
using module::claude::quota::QuotaWindow;

// ---- 测试替身：G0 **唯一**的 provider 实现 ----
// 真实供应商（opencode_go / minimax_cn / deepseek / volcano）是 G1-G4，本文件
// 一个都不实现。Fake 的三条出口（fields / matchesBaseUrl / fetch 返回值）全部
// 由构造参数驱动，便于逐条断言契约，而不是拿被测实现自证。
class FakeProvider final : public QuotaProvider {
public:
    FakeProvider(std::string id, std::string displayName,
                 std::vector<ProviderField> fields, std::string matchedBaseUrl = {},
                 QuotaResult result = {})
        : id_(std::move(id)),
          displayName_(std::move(displayName)),
          fields_(std::move(fields)),
          matchedBaseUrl_(std::move(matchedBaseUrl)),
          result_(std::move(result)) {}

    std::string id() const override { return id_; }
    std::string displayName() const override { return displayName_; }
    std::vector<ProviderField> fields() const override { return fields_; }

    // 未构造 matchedBaseUrl 时走基类默认（return false）——G0 必须验证这条默认
    // 语义：provider 忘了覆写探测时**不能**误命中任何 profile。
    bool matchesBaseUrl(const std::string& baseUrl) const override {
        if (matchedBaseUrl_.empty()) return QuotaProvider::matchesBaseUrl(baseUrl);
        return baseUrl == matchedBaseUrl_;
    }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) override {
        lastConfig_ = cfg;
        httpSeamPresent_ = static_cast<bool>(http);
        // 真把请求交给注入缝（测试传假发送 → 零真实网络）：证明 HttpSendFn 是
        // 可调用对象，而不是一个只是「长得像签名」的空壳。
        if (http) {
            platform::http::Request req;
            req.method = "GET";
            req.url = cfg.baseUrl + "/quota";
            const core::Result<platform::http::Response> resp = http(req);
            seamSawStatusCode_ = resp.isOk() ? resp.value().statusCode : -1;
        }
        return result_;
    }

    const ProviderConfig& lastConfig() const { return lastConfig_; }
    bool httpSeamPresent() const { return httpSeamPresent_; }
    int seamSawStatusCode() const { return seamSawStatusCode_; }

private:
    std::string id_;
    std::string displayName_;
    std::vector<ProviderField> fields_;
    std::string matchedBaseUrl_;
    QuotaResult result_;
    ProviderConfig lastConfig_;
    bool httpSeamPresent_ = false;
    int seamSawStatusCode_ = 0;
};

// 注册表行工厂：**进程内单例**（provider 无状态；findProvider 返回 const 指针，
// 调用方无从释放，每次 new 就是稳定泄漏）。与 G1-G4 真 provider 的工厂契约一致。
FakeProvider& fakeSingleton() {
    static FakeProvider instance("fake_provider", "假供应商",
                                 {ProviderField{"apiKey", "API 密钥", true}});
    return instance;
}

QuotaProvider* createFake() { return &fakeSingleton(); }

// 同一张显式表的两个版本：命中 / 不命中探测
FakeProvider& matchingSingleton() {
    static FakeProvider instance("matcher", "会命中的假供应商", {},
                                 "https://api.example.com");
    return instance;
}
QuotaProvider* createMatcher() { return &matchingSingleton(); }

FakeProvider& nonMatchingSingleton() {
    static FakeProvider instance("non_matcher", "不覆写探测的假供应商", {});
    return instance;
}
QuotaProvider* createNonMatcher() { return &nonMatchingSingleton(); }

// 重复 id 的表：首行 + 同 id 第二行（两个不同单例，便于分辨返回的是谁）
FakeProvider& dupFirstSingleton() {
    static FakeProvider instance("dup", "第一个同 id", {});
    return instance;
}
QuotaProvider* createDupFirst() { return &dupFirstSingleton(); }

FakeProvider& dupSecondSingleton() {
    static FakeProvider instance("dup", "第二个同 id", {});
    return instance;
}
QuotaProvider* createDupSecond() { return &dupSecondSingleton(); }

const QuotaProviderEntry kFakeTable[] = {
    {"fake_provider", &createFake},
    {"matcher", &createMatcher},
    {"non_matcher", &createNonMatcher},
    {nullptr, nullptr},  // 表尾哨兵（与生产表同约定）
};

const QuotaProviderEntry kDupTable[] = {
    {"dup", &createDupFirst},
    {"dup", &createDupSecond},
    {nullptr, nullptr},
};

// 行 id 与工厂造出来的 provider.id() **不一致**的坏表（行写 dup、造出来是
// fake_provider）：这类行「看着在表里」，实际查出来是另一个供应商——结构守卫
// 必须报出来，否则 G1-G4 复制粘贴行时会静默张冠李戴。
const QuotaProviderEntry kMismatchedTable[] = {
    {"dup", &createFake},
    {nullptr, nullptr},
};

// 落库 / UI 用的键值对：fields() 声明的键 → 值
ProviderConfig configWith(std::string providerId, std::string baseUrl,
                          std::string apiKey) {
    ProviderConfig cfg;
    cfg.providerId = std::move(providerId);
    cfg.baseUrl = std::move(baseUrl);
    cfg.apiKey = std::move(apiKey);
    return cfg;
}

// 结构守卫（对任意显式表可跑）：返回首个重复 id；无重复返回空串。
// 空 id（非哨兵位置）也按违规报——那种行永远查不到但看起来「在表里」。
std::string firstDuplicateId(const QuotaProviderEntry* table) {
    std::vector<std::string> seen;
    for (const QuotaProviderEntry* e = table; e->id != nullptr; ++e) {
        const std::string id = e->id;
        if (id.empty()) return "<empty>";
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return id;
        seen.push_back(id);
    }
    return {};
}

// 哨兵前的前缀长度 → span（注册表本体是「不完全边界」的 extern 数组，
// 消费方一律靠哨兵走，见 quota_registry.h）
std::span<const QuotaProviderEntry> tableSpan(const QuotaProviderEntry* table) {
    std::size_t n = 0;
    while (table[n].id != nullptr) ++n;
    return std::span<const QuotaProviderEntry>(table, n);
}

// 行 ↔ provider 一致性：每行都能按**自己的 id** 查到，且查到的 provider 的
// id() 与行 id 相同。返回首个不一致的行 id（无则空串）。
// 这是「加了一行 ≠ 生效」与「行 id 与实现 id 张冠李戴」的探针。
std::string firstInconsistentId(const QuotaProviderEntry* table) {
    const std::span<const QuotaProviderEntry> span = tableSpan(table);
    for (const QuotaProviderEntry& e : span) {
        const QuotaProvider* p = module::claude::quota::findProvider(e.id, span);
        if (p == nullptr || p->id() != e.id) return e.id;
    }
    return {};
}

// ---- G3：DeepSeek provider 的测试替身 ----

// provider 实际发出的那个请求（断言其 URL / 头 / 超时确实如契约）
struct CapturedRequest {
    bool called = false;
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    long timeoutMs = 0;
};

// 假发送：记录请求 + 回一个固定 HTTP 响应。**零真实网络**。
// 注意：断言宏内含 `return false`，不能写进返回非 bool 的 lambda——lambda 只记录，
// 断言一律在用例体里做（与 G0 那条注入缝用例同规矩）。
HttpSendFn httpResponding(CapturedRequest* sink, int statusCode, std::string body) {
    return [sink, statusCode, body](const platform::http::Request& req)
               -> core::Result<platform::http::Response> {
        if (sink != nullptr) {
            sink->called = true;
            sink->method = req.method;
            sink->url = req.url;
            sink->headers = req.headers;
            sink->timeoutMs = req.timeoutMs;
        }
        platform::http::Response resp;
        resp.statusCode = statusCode;
        resp.body = body;
        return core::Result<platform::http::Response>::ok(std::move(resp));
    };
}

// 传输层失败的真实形态：platform::http::send 网络失败时返回 Result::fail
//（http_client.cpp:96-101），不是 ok(Response{networkError=true})。
HttpSendFn httpTransportFailure() {
    return [](const platform::http::Request&) -> core::Result<platform::http::Response> {
        return core::Result<platform::http::Response>::fail(
            core::ErrorCode::NetworkUnreachable, "connection refused");
    };
}

ProviderConfig deepseekConfig(std::string apiKey = "sk-deepseek-test") {
    ProviderConfig cfg;
    cfg.providerId = "deepseek";
    cfg.baseUrl = "https://api.deepseek.com";
    cfg.apiKey = std::move(apiKey);
    return cfg;
}

// DeepSeek /user/balance 的真实响应形态（金额是**字符串**小数，非 JSON number——
// cc-switch balance.rs:415-420 的 parse_f64_field 两种都收，本用例两种都要钉住）
const char* const kDeepSeekBalanceMultiCurrency =
    R"({"is_available":true,"balance_infos":[)"
    R"({"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"},)"
    R"({"currency":"USD","total_balance":"4.25","granted_balance":"0.00","topped_up_balance":"4.25"}]})";

}  // namespace

// ==================== 显式注册表（非静态自注册）====================

REGISTER_TEST(claude_quota_registry_unknown_id_returns_null,
              "claude_quota: 注册表未知 id 返回 nullptr", {
    DOCTEST_CHECK(module::claude::quota::findProvider("no_such_provider") == nullptr);
    // 空 id 不是任何一行，且不能命中哨兵之后的行
    DOCTEST_CHECK(module::claude::quota::findProvider("") == nullptr);
    return true;
})

REGISTER_TEST(claude_quota_registry_finds_registered_row,
              "claude_quota: 注册表命中行返回非空且 id 一致", {
    const QuotaProvider* p =
        module::claude::quota::findProvider("fake_provider", kFakeTable);
    DOCTEST_CHECK(p != nullptr);
    DOCTEST_CHECK_EQ(p->id(), std::string("fake_provider"));
    DOCTEST_CHECK_EQ(p->displayName(), std::string("假供应商"));
    // 单例契约：重复查找拿回同一个对象（调用方只借不还，无泄漏）
    DOCTEST_CHECK(p == module::claude::quota::findProvider("fake_provider", kFakeTable));
    // 只认整串相等：前缀 / 后缀都不算命中
    DOCTEST_CHECK(module::claude::quota::findProvider("fake", kFakeTable) == nullptr);
    DOCTEST_CHECK(module::claude::quota::findProvider("fake_provider_x", kFakeTable) ==
                  nullptr);
    return true;
})

REGISTER_TEST(claude_quota_registry_empty_table_returns_null,
              "claude_quota: 空表（无任何行）查找返回 nullptr", {
    const QuotaProviderEntry empty[] = {{nullptr, nullptr}};
    DOCTEST_CHECK(module::claude::quota::findProvider("fake_provider", empty) == nullptr);
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("https://api.example.com",
                                                         empty) == nullptr);
    return true;
})

REGISTER_TEST(claude_quota_registry_duplicate_id_first_row_wins,
              "claude_quota: 重复 id 取表序首行（确定性，不随哈希漂移）", {
    // 表本身不允许重复（结构守卫见下一条用例），但查找语义必须是确定性的：
    // 命中首行而不是「随便一个」。首行 = dupFirstSingleton。
    const QuotaProvider* p = module::claude::quota::findProvider("dup", kDupTable);
    DOCTEST_CHECK(p != nullptr);
    DOCTEST_CHECK_EQ(p->id(), std::string("dup"));
    DOCTEST_CHECK_EQ(p->displayName(), std::string("第一个同 id"));  // 首行那个单例
    return true;
})

REGISTER_TEST(claude_quota_registry_table_structure_guard,
              "claude_quota: 生产注册表结构守卫（哨兵结尾 / 无空 id / 无重复 / 行↔实现一致）", {
    // 显式表的静态不变量，逐条对**真表**断言：G1-G4 每加一行自动进入本覆盖。
    // 守卫本身先证非恒真（对构造的坏表必须报出来），再对真表断言。
    const QuotaProviderEntry emptyIdTable[] = {
        {"", &createFake}, {nullptr, nullptr}};
    DOCTEST_CHECK_EQ(firstDuplicateId(kDupTable), std::string("dup"));
    DOCTEST_CHECK_EQ(firstDuplicateId(emptyIdTable), std::string("<empty>"));
    DOCTEST_CHECK_EQ(firstInconsistentId(kMismatchedTable), std::string("dup"));
    DOCTEST_CHECK_EQ(firstDuplicateId(kFakeTable), std::string(""));
    DOCTEST_CHECK_EQ(firstInconsistentId(kFakeTable), std::string(""));

    const QuotaProviderEntry* table = module::claude::quota::kQuotaProviders;
    // 哨兵结尾：表非零长（G0 阶段只有哨兵一行，G1-G4 在它之前插行）
    DOCTEST_CHECK(table != nullptr);
    DOCTEST_CHECK_EQ(firstDuplicateId(table), std::string(""));
    // 每一行都必须能被自己的 id 查到、且查到的就是它自己——**这才是「表加了行
    // ≠ 生效」的探针**，放在哨兵之后的行、行 id 与实现 id 不一致的行都会在这里露出来。
    DOCTEST_CHECK_EQ(firstInconsistentId(table), std::string(""));
    return true;
})

// ==================== detectByBaseUrl ====================

REGISTER_TEST(claude_quota_detect_by_base_url_hits,
              "claude_quota: detectByBaseUrl 命中首个匹配行", {
    const QuotaProvider* p = module::claude::quota::detectByBaseUrl(
        "https://api.example.com", kFakeTable);
    DOCTEST_CHECK(p != nullptr);
    DOCTEST_CHECK_EQ(p->id(), std::string("matcher"));
    return true;
})

REGISTER_TEST(claude_quota_detect_by_base_url_misses,
              "claude_quota: detectByBaseUrl 不命中返回 nullptr（不猜）", {
    // 未知 host
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("https://api.other.com",
                                                         kFakeTable) == nullptr);
    // 空串：宁可不选中，也不能命中任意一行
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("", kFakeTable) == nullptr);
    // 未被 provider 声明过的 baseUrl（non_matcher 用基类默认 → 恒 false）
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("https://non_matcher.example.com",
                                                         kFakeTable) == nullptr);
    // 生产表：无对应 provider 的 host 依旧不命中，且不炸（G3 起 api.deepseek.com
    // 由 deepseek provider 命中，见 G3 段；这里换成仍无主的 host）
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl(
                      "https://api.unknown-vendor.example") == nullptr);
    return true;
})

REGISTER_TEST(claude_quota_provider_default_detection_is_false,
              "claude_quota: QuotaProvider 默认 matchesBaseUrl 恒 false", {
    // 直接对基类默认断言：G1-G4 忘了覆写时只会「不被自动选中」（安全方向），
    // 而不是误命中别人的 profile。
    FakeProvider nonMatcher("x", "x", {});
    DOCTEST_CHECK_FALSE(nonMatcher.matchesBaseUrl("https://api.example.com"));
    DOCTEST_CHECK_FALSE(nonMatcher.matchesBaseUrl(""));
    return true;
})

// ==================== ProviderField / fields() 驱动契约 ====================

REGISTER_TEST(claude_quota_fields_driven_rendering_contract,
              "claude_quota: fields() 声明什么就得到什么（key/label/secret 三列钉死）", {
    const std::vector<ProviderField> declared = {
        ProviderField{"accessKeyId", "AccessKey ID", false},
        ProviderField{"secretAccessKey", "AccessKey Secret", true},
    };
    FakeProvider p("volcano_like", "火山样", declared);

    const std::vector<ProviderField> got = p.fields();
    DOCTEST_CHECK_EQ(got.size(), static_cast<std::size_t>(2));
    // 逐列断言（不拿 fields() 自己当期望值——上面 declared 是独立构造的期望）
    DOCTEST_CHECK_EQ(got[0].key, std::string("accessKeyId"));
    DOCTEST_CHECK_EQ(got[0].label, std::string("AccessKey ID"));
    DOCTEST_CHECK_FALSE(got[0].secret);
    DOCTEST_CHECK_EQ(got[1].key, std::string("secretAccessKey"));
    DOCTEST_CHECK_EQ(got[1].label, std::string("AccessKey Secret"));
    DOCTEST_CHECK_TRUE(got[1].secret);
    // secret 默认值必须是 false（漏填 = 明文框，不是密文框——语义由 provider 显式声明）
    DOCTEST_CHECK_FALSE(ProviderField{}.secret);
    return true;
})

REGISTER_TEST(claude_quota_fields_empty_is_legal,
              "claude_quota: 空 fields 合法（沿用 profile 凭据型供应商）", {
    FakeProvider p("uses_profile_key", "沿用 profile 密钥", {});
    DOCTEST_CHECK(p.fields().empty());
    return true;
})

// ==================== QuotaResult 默认值语义 ====================

REGISTER_TEST(claude_quota_result_defaults,
              "claude_quota: QuotaResult 默认值逐字段", {
    const QuotaResult r;
    DOCTEST_CHECK(r.kind == QuotaKind::Tiered);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_EQ(r.balanceRemaining, 0.0);
    DOCTEST_CHECK(r.currency.empty());
    DOCTEST_CHECK(r.planName.empty());
    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.errorMessage.empty());
    return true;
})

// ==================== fetch 契约 ====================

REGISTER_TEST(claude_quota_fetch_returns_error_only_via_field,
              "claude_quota: fetch 恒返回 QuotaResult，错误只走 error 字段", {
    QuotaResult failing;
    failing.error = QuotaError::Transient;
    failing.errorMessage = "网络不可达";
    FakeProvider p("failing", "会失败的假供应商", {}, "", failing);

    const ProviderConfig cfg = configWith("failing", "https://api.example.com", "k");
    const HttpSendFn noHttp;  // 空缝：provider 也不许抛
    const QuotaResult r = p.fetch(cfg, noHttp);

    DOCTEST_CHECK(r.error == QuotaError::Transient);
    DOCTEST_CHECK_EQ(r.errorMessage, std::string("网络不可达"));
    // 失败结果里不许夹带半截成功值（G7 五态：从未成功过 → 只渲染错误行，绝不画 0% 条）
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_EQ(r.balanceRemaining, 0.0);
    DOCTEST_CHECK(r.currency.empty());
    return true;
})

REGISTER_TEST(claude_quota_fetch_receives_config_and_http_seam,
              "claude_quota: fetch 收到完整 ProviderConfig，且注入缝真被调用", {
    QuotaResult ok;
    ok.kind = QuotaKind::Tiered;
    ok.windows.push_back(QuotaWindow{"five_hour", 38.7, "2026-10-02T12:00:00Z"});
    FakeProvider p("seam", "注入缝验证", {}, "", ok);

    ProviderConfig cfg = configWith("seam", "https://api.example.com", "sk-test");
    cfg.fields["accessKeyId"] = "AK";
    cfg.fields["secretAccessKey"] = "SK";

    bool seamCalled = false;
    std::string seenMethod;
    std::string seenUrl;
    // 注意：断言宏内含 `return false`，不能写在返回非 bool 的 lambda 里——
    // 缝里只记录，断言在 lambda 外做。
    const HttpSendFn http = [&seamCalled, &seenMethod, &seenUrl](
                                const platform::http::Request& req)
        -> core::Result<platform::http::Response> {
        seamCalled = true;
        seenMethod = req.method;
        seenUrl = req.url;
        platform::http::Response resp;
        resp.statusCode = 200;
        resp.body = "{}";
        return core::Result<platform::http::Response>::ok(std::move(resp));
    };

    const QuotaResult r = p.fetch(cfg, http);
    DOCTEST_CHECK(seamCalled);                       // 注入缝确实被 provider 调用
    DOCTEST_CHECK_EQ(seenMethod, std::string("GET"));
    DOCTEST_CHECK_EQ(seenUrl, std::string("https://api.example.com/quota"));
    DOCTEST_CHECK(p.httpSeamPresent());
    DOCTEST_CHECK_EQ(p.seamSawStatusCode(), 200);    // 且真的用它拿回了响应
    DOCTEST_CHECK_EQ(p.lastConfig().providerId, std::string("seam"));
    DOCTEST_CHECK_EQ(p.lastConfig().baseUrl, std::string("https://api.example.com"));
    DOCTEST_CHECK_EQ(p.lastConfig().apiKey, std::string("sk-test"));
    DOCTEST_CHECK_EQ(p.lastConfig().fields.size(), static_cast<std::size_t>(2));
    // const map：operator[] 不可用（会插入），按值取用 at()
    DOCTEST_CHECK_EQ(p.lastConfig().fields.at("accessKeyId"), std::string("AK"));
    DOCTEST_CHECK_EQ(p.lastConfig().fields.at("secretAccessKey"), std::string("SK"));
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 38.7);   // double 不截断
    DOCTEST_CHECK_EQ(r.windows[0].resetsAt, std::string("2026-10-02T12:00:00Z"));
    return true;
})

REGISTER_TEST(claude_quota_balance_kind_has_no_windows,
              "claude_quota: Balance 型无窗口（不许硬造窗口）", {
    QuotaResult balance;
    balance.kind = QuotaKind::Balance;
    balance.balanceRemaining = 12.34;
    balance.currency = "CNY";
    FakeProvider p("balance", "余额型", {}, "", balance);

    const QuotaResult r = p.fetch(configWith("balance", "", ""), HttpSendFn{});
    DOCTEST_CHECK(r.kind == QuotaKind::Balance);
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_EQ(r.balanceRemaining, 12.34);
    DOCTEST_CHECK_EQ(r.currency, std::string("CNY"));
    return true;
})

// ==================== G1: OpenCode Go provider ====================
//
// fixture 逐条取自 cc-switch src-tauri/src/services/coding_plan.rs（bfaaba16）：
//   detect_provider:47-50        —— /zen/go 与 /zen/go/v1 两档命中，/zen/v1 刻意不命中
//   parse_opencode_go_tiers:731  —— 三窗口按 key 名映射 / percent 解析 /
//                                   percent==0 丢占位 resetsAt / 窗口级跳过而非整体失败
//   query_opencode_go:764-833    —— 端点、Bearer 认证（与推理侧 x-api-key 相反）、
//                                   403（无订阅）与 401（密钥无效）分开、空 tiers = 形态不认识

namespace {

using module::claude::quota::createOpenCodeGoProvider;

// 假发送：记录请求 + 回预置响应（单测零真实网络）。
// 注意：断言宏内含 `return false`，只能写在返回 bool 的用例体里；这个返回
// Result 的 lambda 里只记录，断言一律在用例体做。
class RecordingHttp {
public:
    HttpSendFn fn() {
        return [this](const platform::http::Request& req)
                   -> core::Result<platform::http::Response> {
            called = true;
            lastRequest = req;
            if (transportFails) {
                return core::Result<platform::http::Response>::fail(
                    core::ErrorCode::NetworkUnreachable, "连接被拒绝");
            }
            platform::http::Response resp;
            resp.statusCode = status;
            resp.body = body;
            return core::Result<platform::http::Response>::ok(resp);
        };
    }

    bool called = false;
    platform::http::Request lastRequest;
    int status = 200;
    std::string body = "{}";
    bool transportFails = false;
};

ProviderConfig openCodeGoConfig(const std::string& apiKey = "sk-zen-go") {
    ProviderConfig cfg;
    cfg.providerId = "opencode_go";
    cfg.baseUrl = "https://opencode.ai/zen/go/v1";
    cfg.apiKey = apiKey;
    return cfg;
}

// 上游三窗口齐全的响应。weekly 的 resetsAt 故意**早于** rolling —— 靠
// resetsAt 排序猜周期的实现在这里会把窗口顺序排错（周期末尾周桶比 5h 桶先重置）。
const char* const kThreeWindowBody = R"({
  "usage": {
    "rolling": {"status": "ok", "percent": 12, "resetsAt": "2026-10-02T20:00:00Z"},
    "weekly":  {"status": "ok", "percent": 38.7, "resetsAt": "2026-10-02T15:00:00Z"},
    "monthly": {"status": "rate-limited", "percent": 100, "resetsAt": "2026-11-01T00:00:00Z"}
  }
})";

}  // namespace

REGISTER_TEST(claude_quota_opencode_go_identity_and_registry_row,
              "claude_quota: OpenCode Go 身份 / 空 fields / 注册表单例行生效", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    DOCTEST_CHECK_EQ(p->id(), std::string("opencode_go"));
    DOCTEST_CHECK_EQ(p->displayName(), std::string("OpenCode Go"));
    // 沿用 profile 的 API Key：不吃额外字段（G6 弹窗不渲染额外输入框）
    DOCTEST_CHECK(p->fields().empty());
    // 进程内单例：两次工厂返回同一对象（调用方只借不还，无泄漏）
    DOCTEST_CHECK(p == createOpenCodeGoProvider());
    // 注册表那行真的接上了（G0 结构守卫也覆盖；这里直接钉死 id ↔ 实现）
    const QuotaProvider* viaRegistry = module::claude::quota::findProvider("opencode_go");
    DOCTEST_REQUIRE(viaRegistry != nullptr);
    DOCTEST_CHECK_EQ(viaRegistry->id(), std::string("opencode_go"));
    DOCTEST_CHECK(viaRegistry == p);
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_maps_three_windows_by_key,
              "claude_quota: OpenCode Go 三窗口按 key 名映射（顺序固定，不按 resetsAt 排序）", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    http.body = kThreeWindowBody;

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.errorMessage.empty());
    DOCTEST_CHECK(r.kind == QuotaKind::Tiered);
    DOCTEST_REQUIRE_EQ(r.windows.size(), static_cast<std::size_t>(3));
    // rolling→five_hour / weekly→weekly / monthly→monthly，展示序 = key 表序
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(r.windows[1].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(r.windows[2].key, std::string("monthly"));
    // usedPercent == 上游 percent（已用，不反转）；double 不截断小数
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 12.0);
    DOCTEST_CHECK_EQ(r.windows[1].usedPercent, 38.7);
    DOCTEST_CHECK_EQ(r.windows[2].usedPercent, 100.0);
    DOCTEST_CHECK_EQ(r.windows[0].resetsAt, std::string("2026-10-02T20:00:00Z"));
    DOCTEST_CHECK_EQ(r.windows[1].resetsAt, std::string("2026-10-02T15:00:00Z"));
    DOCTEST_CHECK_EQ(r.windows[2].resetsAt, std::string("2026-11-01T00:00:00Z"));
    // Tiered 型不带余额字段（不许混用）
    DOCTEST_CHECK_EQ(r.balanceRemaining, 0.0);
    DOCTEST_CHECK(r.currency.empty());
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_zero_percent_drops_placeholder_reset,
              "claude_quota: OpenCode Go percent==0 保留窗口但丢弃占位 resetsAt", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    // percent 为 0 时上游的 resetsAt 是「now + 窗口时长」的占位值（窗口其实
    // 早已过期），拿来显示倒计时是错的 —— 丢弃（coding_plan.rs:748-752）
    http.body = R"({"usage": {
      "rolling": {"status": "ok", "percent": 0, "resetsAt": "2026-10-02T20:00:00Z"},
      "weekly":  {"status": "ok", "percent": 0},
      "monthly": {"status": "ok", "percent": 5.5, "resetsAt": "2026-11-01T00:00:00Z"}
    }})";

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_REQUIRE_EQ(r.windows.size(), static_cast<std::size_t>(3));  // 窗口保留，不是跳过
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 0.0);
    DOCTEST_CHECK(r.windows[0].resetsAt.empty());  // 占位值丢弃
    DOCTEST_CHECK(r.windows[1].resetsAt.empty());
    // 非 0 的窗口照常带重置时间（别把丢弃做成全局行为）
    DOCTEST_CHECK_EQ(r.windows[2].usedPercent, 5.5);
    DOCTEST_CHECK_EQ(r.windows[2].resetsAt, std::string("2026-11-01T00:00:00Z"));
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_skips_unparsable_window,
              "claude_quota: OpenCode Go 缺窗口 / percent 不可解析 → 跳过该窗口，不整体失败", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    // rolling 缺失、monthly 没有 percent —— 逐窗口防御解析（coding_plan.rs:741-747）
    http.body = R"({"usage": {"weekly": {"status": "ok", "percent": 42},
                              "monthly": {"status": "ok"}}})";

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_REQUIRE_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 42.0);
    DOCTEST_CHECK(r.windows[0].resetsAt.empty());
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_accepts_numeric_string_percent,
              "claude_quota: OpenCode Go percent 为数字字符串时照样解析（parse_f64 口径）", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    http.body = R"({"usage": {"rolling": {"status": "ok", "percent": "38.7"}}})";

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_REQUIRE_EQ(r.windows.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(r.windows[0].usedPercent, 38.7);
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_legacy_flat_shape_is_shape_error,
              "claude_quota: OpenCode Go 旧扁平形态 → Shape 错误，不渲染空卡", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    // 上线当天（2026-08-11）改过一次形态：旧扁平 rollingUsage / usagePercent /
    // resetInSec 已作废（coding_plan.rs:724-726）。必须报「形态不认识」，
    // 而不是渲染一张 0% 的空卡（等于告诉用户「还有 100% 可用」）。
    http.body = R"({"rollingUsage": {"usagePercent": 10, "resetInSec": 3600},
                    "weeklyUsage": {"usagePercent": 5, "resetInSec": 90000}})";

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Shape);
    DOCTEST_CHECK_FALSE(r.errorMessage.empty());
    DOCTEST_CHECK(r.windows.empty());
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_unknown_shape_is_shape_error,
              "claude_quota: OpenCode Go 根键缺失 / usage 非对象 / 非 JSON → 一律 Shape", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);

    RecordingHttp emptyBody;  // {}
    const QuotaResult r1 = p->fetch(openCodeGoConfig(), emptyBody.fn());
    DOCTEST_CHECK(r1.error == QuotaError::Shape);
    DOCTEST_CHECK(r1.windows.empty());

    RecordingHttp emptyUsage;  // usage 在但没有可用窗口
    emptyUsage.body = R"({"usage": {}})";
    DOCTEST_CHECK(p->fetch(openCodeGoConfig(), emptyUsage.fn()).error == QuotaError::Shape);

    RecordingHttp usageNotObject;  // usage 不是对象
    usageNotObject.body = R"({"usage": 42})";
    DOCTEST_CHECK(p->fetch(openCodeGoConfig(), usageNotObject.fn()).error ==
                  QuotaError::Shape);

    RecordingHttp notJson;  // 200 + 非 JSON 体
    notJson.body = "<html>gateway</html>";
    DOCTEST_CHECK(p->fetch(openCodeGoConfig(), notJson.fn()).error == QuotaError::Shape);
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_http_403_is_entitlement,
              "claude_quota: OpenCode Go HTTP 403 → Entitlement（key 有效但无 Go 订阅）", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    http.status = 403;  // Zen 与 Go 共用同一把 workspace key，403 = 该 workspace 没订阅
    http.body = R"({"error": "forbidden"})";

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Entitlement);
    DOCTEST_CHECK(r.error != QuotaError::Auth);  // 改密钥没用，得换套餐 —— 不许混
    DOCTEST_CHECK_FALSE(r.errorMessage.empty());
    DOCTEST_CHECK(r.errorMessage.find("403") != std::string::npos);
    DOCTEST_CHECK(r.windows.empty());
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_http_401_is_auth,
              "claude_quota: OpenCode Go HTTP 401 → Auth（密钥无效 / 过期）", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    http.status = 401;
    http.body = R"({"error": "unauthorized"})";

    const QuotaResult r = p->fetch(openCodeGoConfig(), http.fn());

    DOCTEST_CHECK(r.error == QuotaError::Auth);
    DOCTEST_CHECK(r.error != QuotaError::Entitlement);
    DOCTEST_CHECK_FALSE(r.errorMessage.empty());
    DOCTEST_CHECK(r.windows.empty());
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_transient_classification,
              "claude_quota: OpenCode Go 5xx / 限流 / 网络失败 / 空缝 → Transient", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);

    RecordingHttp serverError;
    serverError.status = 503;
    DOCTEST_CHECK(p->fetch(openCodeGoConfig(), serverError.fn()).error ==
                  QuotaError::Transient);

    RecordingHttp rateLimited;
    rateLimited.status = 429;
    DOCTEST_CHECK(p->fetch(openCodeGoConfig(), rateLimited.fn()).error ==
                  QuotaError::Transient);

    RecordingHttp offline;
    offline.transportFails = true;
    const QuotaResult netResult = p->fetch(openCodeGoConfig(), offline.fn());
    DOCTEST_CHECK(netResult.error == QuotaError::Transient);
    DOCTEST_CHECK(netResult.windows.empty());

    // 空缝：返回 Transient 而不是 std::bad_function_call（项目铁律：不抛异常）
    const HttpSendFn noSeam;
    const QuotaResult noSeamResult = p->fetch(openCodeGoConfig(), noSeam);
    DOCTEST_CHECK(noSeamResult.error == QuotaError::Transient);
    DOCTEST_CHECK_FALSE(noSeamResult.errorMessage.empty());
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_calls_injected_http_seam,
              "claude_quota: OpenCode Go 经注入缝发 GET /zen/go/v1/usage + Bearer 认证", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);
    RecordingHttp http;
    http.body = kThreeWindowBody;

    const QuotaResult r = p->fetch(openCodeGoConfig("sk-zen-abc"), http.fn());

    DOCTEST_CHECK(http.called);  // 注入缝真被调用（单测零真实网络）
    DOCTEST_CHECK_EQ(http.lastRequest.method, std::string("GET"));
    DOCTEST_CHECK_EQ(http.lastRequest.url,
                     std::string("https://opencode.ai/zen/go/v1/usage"));
    // 余量端点只认 Authorization: Bearer —— 与推理侧只认 x-api-key 正好相反，
    // 不可互换（coding_plan.rs:767-771）
    DOCTEST_CHECK_EQ(http.lastRequest.headers.count("Authorization"),
                     static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(http.lastRequest.headers.at("Authorization"),
                     std::string("Bearer sk-zen-abc"));
    DOCTEST_CHECK(http.lastRequest.headers.find("x-api-key") ==
                  http.lastRequest.headers.end());
    DOCTEST_CHECK_EQ(http.lastRequest.headers.count("Accept"),
                     static_cast<std::size_t>(1));
    // 缝回的响应真的被消费（不是拿了不用）
    DOCTEST_CHECK_EQ(r.windows.size(), static_cast<std::size_t>(3));
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_matches_base_url,
              "claude_quota: OpenCode Go matchesBaseUrl 两档命中、/zen/v1 与伪造域不命中", {
    QuotaProvider* p = createOpenCodeGoProvider();
    DOCTEST_REQUIRE(p != nullptr);

    // 命中：base_url 两档（claude/claude-desktop 直连档 + codex/opencode 走 /v1 档）
    DOCTEST_CHECK(p->matchesBaseUrl("https://opencode.ai/zen/go"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://opencode.ai/zen/go/v1"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://opencode.ai/zen/go/v1/"));
    DOCTEST_CHECK(p->matchesBaseUrl("HTTPS://OpenCode.AI/zen/go/v1"));
    DOCTEST_CHECK(p->matchesBaseUrl("opencode.ai/zen/go"));  // 无 scheme 也认
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.opencode.ai/zen/go"));  // 子域（同组织）

    // 不命中：Zen 按量版（实测 404，刻意不命中，coding_plan.rs:47-50）
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://opencode.ai/zen/v1"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://opencode.ai/zen"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://opencode.ai/"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl(""));

    // 不命中：host 标签边界（别裸子串包含 —— 否则下面每一条都会误命中）
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://notopencode.ai/zen/go"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://opencode.ai.evil.com/zen/go"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://evil.com/opencode.ai/zen/go"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.example.com/zen/go"));
    return true;
})

REGISTER_TEST(claude_quota_opencode_go_detected_by_base_url_in_registry,
              "claude_quota: 生产注册表 detectByBaseUrl 按 base_url 选中 opencode_go", {
    const QuotaProvider* byGo =
        module::claude::quota::detectByBaseUrl("https://opencode.ai/zen/go");
    DOCTEST_REQUIRE(byGo != nullptr);
    DOCTEST_CHECK_EQ(byGo->id(), std::string("opencode_go"));
    // 两档指向同一 provider，且与工厂单例是同一个对象
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("https://opencode.ai/zen/go/v1") ==
                  byGo);
    DOCTEST_CHECK(byGo == createOpenCodeGoProvider());
    // Zen 按量版不选中任何 provider（宁可不选，也不猜）
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("https://opencode.ai/zen/v1") ==
                  nullptr);
    return true;
})

// ==================== G3：DeepSeek 余额 provider ====================
// 移植自 cc-switch（bfaaba16）src-tauri/src/services/balance.rs：
//   detect_provider :26-43 的 deepseek 项 / query_deepseek :70-146
// DeepSeek **只有余额**：无时间窗口、无 used、无 reset（cc-switch 未实现套餐 / 速率限制）。

REGISTER_TEST(claude_quota_deepseek_identity_and_fields,
              "claude_quota: G3 DeepSeek id/显示名/空 fields（沿用 profile 凭据）", {
    const QuotaProvider* p = module::claude::quota::createDeepSeekProvider();
    DOCTEST_CHECK(p != nullptr);
    if (p == nullptr) return false;
    DOCTEST_CHECK_EQ(p->id(), std::string("deepseek"));
    DOCTEST_CHECK_EQ(p->displayName(), std::string("DeepSeek"));
    // cc-switch get_balance 只有 base_url + api_key 两个入参 → 无额外字段，
    // 密钥沿用 profile 凭据（G6 弹窗据此不渲染额外输入框）
    DOCTEST_CHECK(p->fields().empty());
    // 进程内单例：重复取回同一对象（注册表工厂契约）
    DOCTEST_CHECK(p == module::claude::quota::createDeepSeekProvider());
    return true;
})

REGISTER_TEST(claude_quota_deepseek_registered_in_production_table,
              "claude_quota: G3 DeepSeek 已进生产注册表（哨兵之前）且可按 URL 探测", {
    const QuotaProvider* byId = module::claude::quota::findProvider("deepseek");
    DOCTEST_CHECK(byId != nullptr);
    if (byId == nullptr) return false;
    DOCTEST_CHECK_EQ(byId->id(), std::string("deepseek"));
    DOCTEST_CHECK_EQ(byId->displayName(), std::string("DeepSeek"));
    const QuotaProvider* byUrl =
        module::claude::quota::detectByBaseUrl("https://api.deepseek.com/anthropic");
    DOCTEST_CHECK(byUrl != nullptr);
    if (byUrl == nullptr) return false;
    DOCTEST_CHECK_EQ(byUrl->id(), std::string("deepseek"));
    return true;
})

REGISTER_TEST(claude_quota_deepseek_matches_base_url_host_boundary,
              "claude_quota: G3 DeepSeek baseUrl 探测走 host 标签边界（伪造域不命中）", {
    const QuotaProvider* p = module::claude::quota::createDeepSeekProvider();
    // 命中：官方域（带 / 路径、带端口、带子域、大小写）
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.deepseek.com"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.deepseek.com/v1"));
    DOCTEST_CHECK(p->matchesBaseUrl("https://api.deepseek.com:443/anthropic"));
    DOCTEST_CHECK(p->matchesBaseUrl("HTTPS://API.DEEPSEEK.COM/v1"));
    // 不命中：伪造 / 别的供应商（cc-switch :28 是裸子串 contains，会把
    // api.deepseek.com.evil.com 也放进来；契约 quota_provider.h:135-139 要求标签边界）
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.deepseek.com.evil.com"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://notapi.deepseek.com"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://evil.com/?next=api.deepseek.com"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl("https://api.openai.com/v1"));
    DOCTEST_CHECK_FALSE(p->matchesBaseUrl(""));
    return true;
})

REGISTER_TEST(claude_quota_deepseek_request_shape,
              "claude_quota: G3 DeepSeek 请求 URL/Bearer 头/方法/超时（域名写死，不派生 baseUrl）", {
    CapturedRequest seen;
    // 故意把 profile 的 baseUrl 指到一个网关：余量接口域名由 provider 写死
    //（cc-switch :78 字面量；契约 quota_provider.h:97-100 baseUrl 只是事实记录）
    ProviderConfig cfg = deepseekConfig("sk-abc123");
    cfg.baseUrl = "https://my-gateway.internal/deepseek/v1";

    const QuotaResult r = module::claude::quota::createDeepSeekProvider()->fetch(
        cfg, httpResponding(&seen, 200, kDeepSeekBalanceMultiCurrency));

    DOCTEST_CHECK(seen.called);  // 注入缝真被调用（零真实网络）
    DOCTEST_CHECK_EQ(seen.method, std::string("GET"));
    DOCTEST_CHECK_EQ(seen.url, std::string("https://api.deepseek.com/user/balance"));
    DOCTEST_CHECK_EQ(seen.headers.at("Authorization"), std::string("Bearer sk-abc123"));
    DOCTEST_CHECK_EQ(seen.headers.at("Accept"), std::string("application/json"));
    DOCTEST_CHECK_EQ(seen.timeoutMs, 15000L);  // cc-switch :81 15 s
    DOCTEST_CHECK(r.error == QuotaError::None);
    return true;
})

REGISTER_TEST(claude_quota_deepseek_balance_ok_multi_currency_takes_first,
              "claude_quota: G3 DeepSeek 正常余额（多币种取表序首条，金额字符串可解析）", {
    CapturedRequest seen;
    const QuotaResult r = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(), httpResponding(&seen, 200, kDeepSeekBalanceMultiCurrency));

    DOCTEST_CHECK(r.error == QuotaError::None);
    DOCTEST_CHECK(r.errorMessage.empty());
    DOCTEST_CHECK(r.kind == QuotaKind::Balance);
    DOCTEST_CHECK(r.windows.empty());  // 钉死：Balance 型不许硬造窗口
    // 多币种取**表序首条**：cc-switch :116-138 把 balance_infos 逐条各成一行、
    // 按数组序展示（不排序、不挑币种）；DevDesk 契约只有单标量，
    // 取首条 = cc-switch 展示序首行（DeepSeek 主币种在首条，顺序稳定）
    DOCTEST_CHECK_EQ(r.balanceRemaining, 110.0);
    DOCTEST_CHECK_EQ(r.currency, std::string("CNY"));
    return true;
})

REGISTER_TEST(claude_quota_deepseek_currency_passthrough_no_conversion,
              "claude_quota: G3 DeepSeek 币种原样保留、金额不换算（number 与 string 两种形态）", {
    // number 形态（非 DeepSeek 现行格式，但 cc-switch parse_f64_field 两种都收）
    const QuotaResult usd = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200,
                       R"({"is_available":true,"balance_infos":[{"currency":"USD","total_balance":7.5}]})"));
    DOCTEST_CHECK(usd.error == QuotaError::None);
    DOCTEST_CHECK_EQ(usd.currency, std::string("USD"));  // 原样，不折成 CNY
    DOCTEST_CHECK_EQ(usd.balanceRemaining, 7.5);         // 原值，不乘汇率

    // string 形态：大数值 + 多小数位原样透传，不截断
    const QuotaResult cny = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200,
                       R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"1234.5678"}]})"));
    DOCTEST_CHECK(cny.error == QuotaError::None);
    DOCTEST_CHECK_EQ(cny.currency, std::string("CNY"));
    DOCTEST_CHECK_EQ(cny.balanceRemaining, 1234.5678);
    return true;
})

REGISTER_TEST(claude_quota_deepseek_is_available_false_is_entitlement,
              "claude_quota: G3 DeepSeek is_available=false → Entitlement（改密钥没用）", {
    // cc-switch :131-135 把 is_available=false 映射成 "Insufficient balance"
    const QuotaResult r = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200,
                       R"({"is_available":false,"balance_infos":[)"
                       R"({"currency":"CNY","total_balance":"0.00","granted_balance":"0.00","topped_up_balance":"0.00"}]})"));

    DOCTEST_CHECK(r.error == QuotaError::Entitlement);
    DOCTEST_CHECK(r.errorMessage.find("余额不足") != std::string::npos);
    // 错误通道独占（契约约定 2）：失败态不许夹带半截成功值，也不许有窗口
    DOCTEST_CHECK(r.windows.empty());
    DOCTEST_CHECK_EQ(r.balanceRemaining, 0.0);
    DOCTEST_CHECK(r.currency.empty());
    DOCTEST_CHECK(r.kind == QuotaKind::Balance);
    return true;
})

REGISTER_TEST(claude_quota_deepseek_is_available_missing_defaults_true,
              "claude_quota: G3 DeepSeek is_available 缺失按 true（cc-switch unwrap_or(true)）", {
    const QuotaResult r = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200,
                       R"({"balance_infos":[{"currency":"CNY","total_balance":"9.99"}]})"));
    DOCTEST_CHECK(r.error == QuotaError::None);  // 没有证据不判欠费
    DOCTEST_CHECK_EQ(r.balanceRemaining, 9.99);
    DOCTEST_CHECK_EQ(r.currency, std::string("CNY"));
    return true;
})

REGISTER_TEST(claude_quota_deepseek_shape_errors,
              "claude_quota: G3 DeepSeek 空 balance_infos / 结构不认识 → Shape", {
    // 空数组（cc-switch 会静默回 success+无数据；DevDesk 契约要求 Shape——
    // 「不是返回空卡」，见 quota_provider.h:46-47）
    const QuotaResult empty = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200, R"({"is_available":true,"balance_infos":[]})"));
    DOCTEST_CHECK(empty.error == QuotaError::Shape);
    DOCTEST_CHECK_FALSE(empty.errorMessage.empty());

    // 缺 balance_infos 根键
    const QuotaResult missing = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(), httpResponding(nullptr, 200, R"({"is_available":true})"));
    DOCTEST_CHECK(missing.error == QuotaError::Shape);

    // 根不是对象
    const QuotaResult notObject = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(), httpResponding(nullptr, 200, "[1,2,3]"));
    DOCTEST_CHECK(notObject.error == QuotaError::Shape);

    // 非法 JSON（拿到完整响应体后解析失败 = 确定性，cc-switch :107 同判）
    const QuotaResult badJson = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(), httpResponding(nullptr, 200, "not json at all"));
    DOCTEST_CHECK(badJson.error == QuotaError::Shape);

    // 条目缺 total_balance / currency
    const QuotaResult noTotal = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200,
                       R"({"is_available":true,"balance_infos":[{"currency":"CNY"}]})"));
    DOCTEST_CHECK(noTotal.error == QuotaError::Shape);
    const QuotaResult noCurrency = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(),
        httpResponding(nullptr, 200,
                       R"({"is_available":true,"balance_infos":[{"total_balance":"1.00"}]})"));
    DOCTEST_CHECK(noCurrency.error == QuotaError::Shape);
    return true;
})

REGISTER_TEST(claude_quota_deepseek_transport_and_http_errors,
              "claude_quota: G3 DeepSeek 传输失败/5xx/限流 → Transient；401→Auth；403→Entitlement", {
    // fetch 非 const 成员 → 必须用非 const 指针（注册表返回的 const 指针只够读元信息）
    QuotaProvider* p = module::claude::quota::createDeepSeekProvider();

    // 空缝：不许 std::bad_function_call（契约 quota_provider.h:114-116）
    const QuotaResult noSeam = p->fetch(deepseekConfig(), HttpSendFn{});
    DOCTEST_CHECK(noSeam.error == QuotaError::Transient);

    // 传输层失败（Result::fail）
    const QuotaResult netFail = p->fetch(deepseekConfig(), httpTransportFailure());
    DOCTEST_CHECK(netFail.error == QuotaError::Transient);

    // 5xx / 429
    DOCTEST_CHECK(p->fetch(deepseekConfig(), httpResponding(nullptr, 500, "{}")).error ==
                  QuotaError::Transient);
    DOCTEST_CHECK(p->fetch(deepseekConfig(), httpResponding(nullptr, 503, "{}")).error ==
                  QuotaError::Transient);
    DOCTEST_CHECK(p->fetch(deepseekConfig(), httpResponding(nullptr, 429, "{}")).error ==
                  QuotaError::Transient);

    // 401 → Auth（cc-switch :91-93 的 UNAUTHORIZED 分支；改密钥有用）
    const QuotaResult auth =
        p->fetch(deepseekConfig("sk-bad"), httpResponding(nullptr, 401, "{}"));
    DOCTEST_CHECK(auth.error == QuotaError::Auth);
    DOCTEST_CHECK(auth.errorMessage.find("401") != std::string::npos);

    // 403 → Entitlement（cc-switch 与 401 同判「鉴权失败」，但契约
    // quota_provider.h:44-46 把 403 归 Entitlement：凭据有效、产品权限没有）
    const QuotaResult forbidden = p->fetch(deepseekConfig(), httpResponding(nullptr, 403, "{}"));
    DOCTEST_CHECK(forbidden.error == QuotaError::Entitlement);
    return true;
})

REGISTER_TEST(claude_quota_deepseek_missing_key_is_not_configured,
              "claude_quota: G3 DeepSeek 缺 API key → NotConfigured（不发请求）", {
    CapturedRequest seen;
    const QuotaResult r = module::claude::quota::createDeepSeekProvider()->fetch(
        deepseekConfig(""), httpResponding(&seen, 200, kDeepSeekBalanceMultiCurrency));
    DOCTEST_CHECK(r.error == QuotaError::NotConfigured);
    DOCTEST_CHECK_FALSE(seen.called);  // 配置不全不发请求（cc-switch :427-433 同判）
    return true;
})

REGISTER_TEST(claude_quota_deepseek_never_has_windows,
              "claude_quota: G3 DeepSeek 任何形态都无窗口（Balance 契约钉死）", {
    // 这条是给后来者的护栏：DeepSeek 没有 5h/周/月窗口，任何「为凑数硬造一个窗口」
    // 的改动都会在这里红（契约 quota_provider.h:66-68）。
    struct Case {
        int status;
        const char* body;
    };
    const Case cases[] = {
        {200, kDeepSeekBalanceMultiCurrency},
        {200, R"({"is_available":false,"balance_infos":[{"currency":"CNY","total_balance":"0.00"}]})"},
        {200, R"({"is_available":true,"balance_infos":[]})"},
        {200, "not json"},
        {401, "{}"},
        {500, "{}"},
    };
    for (const Case& c : cases) {
        const QuotaResult r = module::claude::quota::createDeepSeekProvider()->fetch(
            deepseekConfig(), httpResponding(nullptr, c.status, c.body));
        DOCTEST_CHECK(r.windows.empty());
        DOCTEST_CHECK(r.kind == QuotaKind::Balance);
    }
    // 空缝形态也不许有窗口
    const QuotaResult noSeam =
        module::claude::quota::createDeepSeekProvider()->fetch(deepseekConfig(), HttpSendFn{});
    DOCTEST_CHECK(noSeam.windows.empty());
    return true;
})
