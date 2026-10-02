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
    // 生产表（G0 尚无真实 provider）也不命中，且不炸
    DOCTEST_CHECK(module::claude::quota::detectByBaseUrl("https://api.deepseek.com") ==
                  nullptr);
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
