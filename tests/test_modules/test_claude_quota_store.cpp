// tests/test_modules/test_claude_quota_store.cpp
// G5：余量两表存储 + 后台定时刷新契约
// （计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G5）
//
// 覆盖：
//   - windows_json 编解码**往返**（空数组 / 多窗口 / 小数不截断 / 坏输入容错）
//   - 两表 CRUD（配置行 / 结果行；无行 → NotFound 而非默认行）
//   - 畸形 usedPercent 一律**丢该窗口**（缺/类型错/越界），绝不落成假 0%
//   - 失败韧性：Transient 保留上次成功值；鉴权类立即透出
//   - **「从未成功过」不写假 0%**：kind 列空 + 数据列保持初始值
//   - 定时器到点触发（**注入时钟 + 毫秒级 tick**，绝不等 15 分钟）
//   - 就绪缝（**由落库事实驱动**：读不回那一行就判红）
//   - 线程退出不挂死；**在途请求时 stopAll 会等它跑完**（退出延迟的真实口径）
//   - secrets_json 经 platform::credential 存取 + protect 失败不留半截行
//   - applyFetchResult 并发不丢更新（读—改—写同锁）
#include "doctest.h"
#include "test_support.h"

#include <core/platform/platform.h>   // consumeUiUpdate（唤醒位读取，同批次 D 用例）

#include "module_claude_proxy/profile_repository.h"
#include "platform/credential_protection.h"
#include "quota/quota_credential.h"
#include "quota/quota_error_text.h"
#include "quota/quota_refresh.h"
#include "quota/quota_store.h"

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using module::claude::ProfileRepository;
using module::claude::quota::QuotaConfigRow;
using module::claude::quota::QuotaCredentialSource;
using module::claude::quota::QuotaError;
using module::claude::quota::QuotaKind;
using module::claude::quota::QuotaProvider;
using module::claude::quota::QuotaRefreshOptions;
using module::claude::quota::QuotaRefresher;
using module::claude::quota::QuotaResult;
using module::claude::quota::QuotaResultRow;
using module::claude::quota::QuotaStore;
using module::claude::quota::QuotaWindow;
using module::claude::quota::HttpSendFn;
using module::claude::quota::ProviderConfig;
using module::claude::quota::ProviderField;

// ---- 假 provider：fetch 返回值由构造驱动，可数调用次数 ----
class ScriptedProvider final : public QuotaProvider {
public:
    explicit ScriptedProvider(QuotaResult result) : result_(std::move(result)) {}

    std::string id() const override { return "scripted"; }
    std::string displayName() const override { return "脚本供应商"; }
    std::vector<ProviderField> fields() const override { return {}; }

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) const override {
        calls.fetch_add(1);
        lastBaseUrl = cfg.baseUrl;
        (void)http;   // 本 provider 不发请求（零真实网络）
        return result_;
    }

    // fetch 是 const 成员（评审 m2），所以这两个探针成员必须可写
    mutable std::atomic<int> calls{0};
    mutable std::string lastBaseUrl;

private:
    QuotaResult result_;
};

// 假凭据来源之一：**转发**给 provider（定时器用例要的就是「provider 被调了几次」，
// 且结果由 provider 决定）。
//
// ⚠ 评审 M4：曾经有个叫 `ScriptedCredentialSource` 的假件，构造参数收一个
// QuotaResult 却被**完全忽略**（恒 return provider.fetch(...)）——名字承诺
// 「可编排结果」、行为是「恒转发」。本批侥幸全绿只因两处恰好都传 okTiered()。
// G6 一旦拿它验「凭据失败怎么落库」，它会返回 provider 的成功值、用例照样绿。
// 故拆成两个**各名副其实**的假件：本件转发、另一个（下方）才真的按脚本返回。
class ForwardingCredentialSource final : public QuotaCredentialSource {
public:
    QuotaResult resolveAndFetch(int64_t profileId, const QuotaProvider& provider,
                                const HttpSendFn& http) override {
        ++calls;
        lastProfileId = profileId;
        return provider.fetch(ProviderConfig{}, http);
    }

    std::atomic<int> calls{0};
    int64_t lastProfileId = 0;
};

// 假凭据来源之二：**按构造时给的脚本返回**，绝不碰 provider（连 fetch 都不调）。
// 「凭据层自己失败」这条路径只有这个假件能测——转发型假件永远拿不到
// 「凭据解析失败但 provider 是好的」这种结局。
class ScriptedCredentialSource final : public QuotaCredentialSource {
public:
    explicit ScriptedCredentialSource(QuotaResult result)
        : result_(std::move(result)) {}

    QuotaResult resolveAndFetch(int64_t profileId, const QuotaProvider& provider,
                                const HttpSendFn& http) override {
        ++calls;
        lastProfileId = profileId;
        return result_;   // 构造参数**真的**被读（评审 M4 的修法）
    }

    std::atomic<int> calls{0};
    int64_t lastProfileId = 0;

private:
    QuotaResult result_;
};

// 可注入时钟：秒为单位，手动拨。
struct FakeClock {
    int64_t now = 1000;
    int64_t operator()() const { return now; }
};

QuotaResult okTiered() {
    QuotaResult r;
    r.kind = QuotaKind::Tiered;
    r.windows = {QuotaWindow{"five_hour", 62.5, "2026-10-03T12:00:00Z"},
                 QuotaWindow{"weekly", 30.25, ""}};
    r.planName = "Pro";
    return r;
}

QuotaResult failure(QuotaError kind, const std::string& message) {
    QuotaResult r;
    r.error = kind;
    r.errorMessage = message;
    return r;
}

}  // namespace

// ============ windows_json 编解码往返（单点纪律的核心）============

REGISTER_TEST(claude_quota_windows_json_roundtrip,
              "claude_quota_store: windows_json 往返（空数组 / 多窗口 / 小数不截断）", {
    // 空数组往返：空 → "[]" → 空。**不是空串**——空串在库里读回会被当成
    // 「没查过」，与「查到了、零窗口」混淆。
    DOCTEST_CHECK_EQ(module::claude::quota::encodeQuotaWindows({}),
                     std::string("[]"));
    auto emptyBack = module::claude::quota::decodeQuotaWindows("[]");
    DOCTEST_REQUIRE(emptyBack.isOk());
    DOCTEST_CHECK(emptyBack.value().empty());

    // 多窗口 + 小数百分比不截断（quota_provider.h 不变量 3：量纲由 provider 归一，
    // 这里只管**存得下、读得回**）
    std::vector<QuotaWindow> original{
        QuotaWindow{"five_hour", 62.5, "2026-10-03T12:00:00Z"},
        QuotaWindow{"weekly", 30.25, ""},
        QuotaWindow{"monthly", 0.125, "2026-11-01T00:00:00Z"},
    };
    const std::string json = module::claude::quota::encodeQuotaWindows(original);
    auto back = module::claude::quota::decodeQuotaWindows(json);
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_REQUIRE_EQ(back.value().size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        DOCTEST_CHECK_EQ(back.value()[i].key, original[i].key);
        DOCTEST_CHECK_EQ(back.value()[i].usedPercent, original[i].usedPercent);
        DOCTEST_CHECK_EQ(back.value()[i].resetsAt, original[i].resetsAt);
    }

    // 坏输入容错（展示面不因一行坏 JSON 崩）：空串 / 非 JSON / 非数组 → 空列表
    auto bad1 = module::claude::quota::decodeQuotaWindows("");
    DOCTEST_REQUIRE(bad1.isOk());
    DOCTEST_CHECK(bad1.value().empty());
    auto bad2 = module::claude::quota::decodeQuotaWindows("{不是 json");
    DOCTEST_REQUIRE(bad2.isOk());
    DOCTEST_CHECK(bad2.value().empty());
    auto bad3 = module::claude::quota::decodeQuotaWindows("{\"key\":\"x\"}");
    DOCTEST_REQUIRE(bad3.isOk());
    DOCTEST_CHECK(bad3.value().empty());
    // 元素非对象 / 无规范键 → 跳过该元素（其余保留）
    auto mixed = module::claude::quota::decodeQuotaWindows(
        "[{\"key\":\"weekly\",\"usedPercent\":12},\"坏元素\",{\"usedPercent\":9}]");
    DOCTEST_REQUIRE(mixed.isOk());
    DOCTEST_REQUIRE_EQ(mixed.value().size(), 1u);
    DOCTEST_CHECK_EQ(mixed.value()[0].key, std::string("weekly"));
    return true;
})

REGISTER_TEST(claude_quota_windows_json_rejects_malformed_percent,
              "claude_quota_store: 畸形 usedPercent 丢该窗口（缺/类型错/越界），绝不落成 0%", {
    // B1：曾经「不是 number 就沿用默认值 0.0 并保留窗口」——那正是本模块
    // 第一条不变量要杜绝的假 0%（G7 会把 0 画成绿的「几乎没用满」条）。
    // 口径 = **丢该元素**：spec §9 要求越界输入「被拒或记 error」，
    // 不是 clamp 后断言落在 [0,100]（那是恒真断言）。
    // 每例都断言该窗口**不在**输出里。
    const char* bad[] = {
        R"([{"key":"weekly"}])",                              // 缺 usedPercent
        R"([{"key":"weekly","usedPercent":"62.5"}])",        // 字符串而非数字
        R"([{"key":"weekly","usedPercent":250}])",            // 越界上
        R"([{"key":"weekly","usedPercent":-5}])",             // 越界下
    };
    for (const char* json : bad) {
        auto decoded = module::claude::quota::decodeQuotaWindows(json);
        DOCTEST_REQUIRE(decoded.isOk());
        DOCTEST_CHECK_MESSAGE(decoded.value().empty(),
                              std::string("畸形 usedPercent 被静默采纳成 0%：") + json);
    }
    // 边界值 0 与 100 是**合法**的（已用 0% / 已用 100%），不许被这条规则误杀
    for (const char* edge : {R"([{"key":"weekly","usedPercent":0}])",
                             R"([{"key":"weekly","usedPercent":100}])"}) {
        auto decoded = module::claude::quota::decodeQuotaWindows(edge);
        DOCTEST_REQUIRE(decoded.isOk());
        DOCTEST_REQUIRE_EQ(decoded.value().size(), 1u);
        DOCTEST_CHECK_EQ(decoded.value()[0].key, std::string("weekly"));
    }
    // 混合：坏窗口被丢，好窗口**仍要留下**（不能整表丢弃）
    auto mixed = module::claude::quota::decodeQuotaWindows(
        R"([{"key":"five_hour","usedPercent":62.5},)"
        R"({"key":"weekly","usedPercent":"坏"},)"
        R"({"key":"monthly","usedPercent":900}])");
    DOCTEST_REQUIRE(mixed.isOk());
    DOCTEST_REQUIRE_EQ(mixed.value().size(), 1u);
    DOCTEST_CHECK_EQ(mixed.value()[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(mixed.value()[0].usedPercent, 62.5);
    // **整数百分比不许变成 0**：platform::json 把整数存成 Int（intVal，
    // dblVal 留 0），只调 asDouble() 会把 `"usedPercent": 62` 读成 0%——
    // 又一条假 0% 的路。上游给整数值是很正常的写法，故单钉一例。
    auto intPct = module::claude::quota::decodeQuotaWindows(
        R"([{"key":"weekly","usedPercent":62}])");
    DOCTEST_REQUIRE(intPct.isOk());
    DOCTEST_REQUIRE_EQ(intPct.value().size(), 1u);
    DOCTEST_CHECK_EQ(intPct.value()[0].usedPercent, 62.0);
    // 编码侧同样要能写回整数（encode 用 makeDouble，往返不因 62 vs 62.0 走偏）
    auto intRoundTrip = module::claude::quota::decodeQuotaWindows(
        module::claude::quota::encodeQuotaWindows({QuotaWindow{"weekly", 62.0, ""}}));
    DOCTEST_REQUIRE(intRoundTrip.isOk());
    DOCTEST_REQUIRE_EQ(intRoundTrip.value().size(), 1u);
    DOCTEST_CHECK_EQ(intRoundTrip.value()[0].usedPercent, 62.0);
    return true;
})

REGISTER_TEST(claude_quota_windows_json_special_chars_survive,
              "claude_quota_store: windows_json 里带引号 / 中文 / 反斜杠的键值往返无损", {
    std::vector<QuotaWindow> original{
        QuotaWindow{"weekly", 12.5, "带\"引号\"和\\反斜杠的 ISO 串"},
    };
    const std::string json = module::claude::quota::encodeQuotaWindows(original);
    auto back = module::claude::quota::decodeQuotaWindows(json);
    DOCTEST_REQUIRE(back.isOk());
    DOCTEST_REQUIRE_EQ(back.value().size(), 1u);
    DOCTEST_CHECK_EQ(back.value()[0].resetsAt, original[0].resetsAt);
    return true;
})

// ============ 两表 CRUD ============

REGISTER_TEST(claude_quota_store_config_crud,
              "claude_quota_store: 配置行 写→读→改→列→删 全链一致", {
    test_support::TestDb tdb("quotastore", "cfg");
    QuotaStore store(tdb.handle());

    // 无行 → NotFound（**不是**返回一行默认值：「没配过」与「配了但字段全空」
    // 在 UI 上是两种引导）
    auto missing = store.loadConfig(42);
    DOCTEST_CHECK(missing.isFail());
    DOCTEST_CHECK(missing.errorCode() == core::ErrorCode::NotFound);

    QuotaConfigRow row;
    row.profileId = 42;
    row.providerId = "volcano";
    row.extraJson = R"({"accessKeyId":"AK-1"})";
    row.secretsJson = R"({"secretAccessKey":"SK-1"})";
    row.enabled = true;
    row.autoRefresh = true;
    row.intervalMinutes = 15;
    DOCTEST_REQUIRE(store.saveConfig(row).isOk());

    auto loaded = store.loadConfig(42);
    DOCTEST_REQUIRE(loaded.isOk());
    DOCTEST_CHECK_EQ(loaded.value().providerId, std::string("volcano"));
    DOCTEST_CHECK_EQ(loaded.value().extraJson, row.extraJson);
    DOCTEST_CHECK_EQ(loaded.value().secretsJson, row.secretsJson);
    DOCTEST_CHECK(loaded.value().enabled);
    DOCTEST_CHECK(loaded.value().autoRefresh);
    DOCTEST_CHECK_EQ(loaded.value().intervalMinutes, 15);

    // 改：整行覆盖
    QuotaConfigRow edited = loaded.value();
    edited.providerId = "deepseek";
    edited.intervalMinutes = 30;
    edited.enabled = false;
    DOCTEST_REQUIRE(store.saveConfig(edited).isOk());
    auto reloaded = store.loadConfig(42);
    DOCTEST_REQUIRE(reloaded.isOk());
    DOCTEST_CHECK_EQ(reloaded.value().providerId, std::string("deepseek"));
    DOCTEST_CHECK_EQ(reloaded.value().intervalMinutes, 30);
    DOCTEST_CHECK(!reloaded.value().enabled);

    // 列：按 profile_id 升序、结果确定
    QuotaConfigRow other;
    other.profileId = 7;
    other.providerId = "opencode_go";
    DOCTEST_REQUIRE(store.saveConfig(other).isOk());
    auto all = store.listConfigs();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), 2u);
    DOCTEST_CHECK_EQ(all.value()[0].profileId, int64_t(7));
    DOCTEST_CHECK_EQ(all.value()[1].profileId, int64_t(42));

    // 删：存在 → ok；再删 → NotFound
    DOCTEST_REQUIRE(store.deleteConfig(42).isOk());
    DOCTEST_CHECK(store.loadConfig(42).isFail());
    DOCTEST_CHECK(store.deleteConfig(42).isFail());
    return true;
})

REGISTER_TEST(claude_quota_store_result_crud_roundtrip,
              "claude_quota_store: 结果行 落库→读回 逐列一致（含 Balance 型无窗口）", {
    test_support::TestDb tdb("quotastore", "res");
    QuotaStore store(tdb.handle());

    DOCTEST_CHECK(store.loadResult(5).isFail());   // 从没查过

    QuotaResult tiered = okTiered();
    DOCTEST_REQUIRE(store.applyFetchResult(5, tiered, 1700000000).isOk());
    auto row = store.loadResult(5);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().hasEverSucceeded);
    DOCTEST_CHECK(!row.value().neverSucceeded());
    DOCTEST_CHECK(row.value().kind == QuotaKind::Tiered);
    DOCTEST_REQUIRE_EQ(row.value().windows.size(), 2u);
    DOCTEST_CHECK_EQ(row.value().windows[0].key, std::string("five_hour"));
    DOCTEST_CHECK_EQ(row.value().windows[0].usedPercent, 62.5);
    DOCTEST_CHECK_EQ(row.value().windows[1].key, std::string("weekly"));
    DOCTEST_CHECK_EQ(row.value().planName, std::string("Pro"));
    DOCTEST_CHECK_EQ(row.value().fetchedAt, int64_t(1700000000));
    DOCTEST_CHECK(!row.value().hasError());

    // Balance 型：windows 恒空但**确实成功过**——hasEverSucceeded 必须为 true。
    // 判据若写成「windows 空 = 从未成功」，这里会被误判成未查过（假 0% 的温床）。
    QuotaResult balance;
    balance.kind = QuotaKind::Balance;
    balance.balanceRemaining = 12.5;
    balance.currency = "CNY";
    DOCTEST_REQUIRE(store.applyFetchResult(6, balance, 1700000001).isOk());
    auto bRow = store.loadResult(6);
    DOCTEST_REQUIRE(bRow.isOk());
    DOCTEST_CHECK(bRow.value().hasEverSucceeded);
    DOCTEST_CHECK(bRow.value().kind == QuotaKind::Balance);
    DOCTEST_CHECK(bRow.value().windows.empty());
    DOCTEST_CHECK_EQ(bRow.value().balanceRemaining, 12.5);
    DOCTEST_CHECK_EQ(bRow.value().currency, std::string("CNY"));

    auto all = store.listResults();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), 2u);

    DOCTEST_REQUIRE(store.deleteResult(5).isOk());
    DOCTEST_CHECK(store.loadResult(5).isFail());
    DOCTEST_CHECK(store.deleteResult(5).isFail());
    return true;
})

// ============ 失败韧性 ============

REGISTER_TEST(claude_quota_store_transient_keeps_last_success,
              "claude_quota_store: Transient 保留上次成功值 + 只写 last_error", {
    test_support::TestDb tdb("quotastore", "transient");
    QuotaStore store(tdb.handle());

    DOCTEST_REQUIRE(store.applyFetchResult(3, okTiered(), 1000).isOk());
    // 网络抖一下：不该让用户的余量条整个消失
    DOCTEST_REQUIRE(store.applyFetchResult(
                        3, failure(QuotaError::Transient, "网络超时"), 2000).isOk());

    auto row = store.loadResult(3);
    DOCTEST_REQUIRE(row.isOk());
    // 数据列**原样保留**（G7 画旧条 + 「可能过期」角标）
    DOCTEST_CHECK(row.value().hasEverSucceeded);
    DOCTEST_REQUIRE_EQ(row.value().windows.size(), 2u);
    DOCTEST_CHECK_EQ(row.value().windows[0].usedPercent, 62.5);
    DOCTEST_CHECK_EQ(row.value().planName, std::string("Pro"));
    // 错误两列已写（fetched_at 也推进：到期判定从「上次尝试」起算，
    // 否则一次持续失败会每片都重试 = 重试风暴）
    DOCTEST_CHECK(row.value().hasError());
    DOCTEST_CHECK(row.value().lastErrorKind == QuotaError::Transient);
    DOCTEST_CHECK_EQ(row.value().lastError, std::string("网络超时"));
    DOCTEST_CHECK_EQ(row.value().fetchedAt, int64_t(2000));

    // 下一次成功：清空错误两列
    DOCTEST_REQUIRE(store.applyFetchResult(3, okTiered(), 3000).isOk());
    auto healed = store.loadResult(3);
    DOCTEST_REQUIRE(healed.isOk());
    DOCTEST_CHECK(!healed.value().hasError());
    DOCTEST_CHECK(healed.value().lastError.empty());
    return true;
})

REGISTER_TEST(claude_quota_store_auth_error_surfaces_immediately,
              "claude_quota_store: 鉴权类错误立即透出（last_error_kind 分类保真）", {
    test_support::TestDb tdb("quotastore", "auth");
    QuotaStore store(tdb.handle());

    DOCTEST_REQUIRE(store.applyFetchResult(4, okTiered(), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        4, failure(QuotaError::Auth, "密钥无效"), 2000).isOk());
    auto row = store.loadResult(4);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().hasError());
    DOCTEST_CHECK(row.value().lastErrorKind == QuotaError::Auth);
    DOCTEST_CHECK_EQ(row.value().lastError, std::string("密钥无效"));
    // 有旧值 → UI 仍可显示旧条 + 明确错误文案（G7「失败（有过成功值）」）
    DOCTEST_CHECK(row.value().hasEverSucceeded);

    // 四种错误分类逐个往返（UI 据 last_error_kind 选文案，分类不能串）
    const std::pair<QuotaError, const char*> cases[] = {
        {QuotaError::Auth, "auth"},
        {QuotaError::Entitlement, "entitlement"},
        {QuotaError::Shape, "shape"},
        {QuotaError::NotConfigured, "not_configured"},
        {QuotaError::Transient, "transient"},
    };
    for (const auto& c : cases) {
        DOCTEST_CHECK_EQ(std::string(module::claude::quota::quotaErrorName(c.first)),
                         std::string(c.second));
        DOCTEST_CHECK(module::claude::quota::quotaErrorFromName(c.second) == c.first);
        DOCTEST_REQUIRE(store.applyFetchResult(
                            4, failure(c.first, "x"), 3000).isOk());
        auto r = store.loadResult(4);
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(r.value().lastErrorKind == c.first);
    }
    // 「none」是成功态的落库值，且**不是**空串（空串 = 这行还没写过）
    DOCTEST_CHECK(module::claude::quota::quotaErrorFromName("none") == QuotaError::None);
    DOCTEST_CHECK(module::claude::quota::quotaErrorFromName("") == QuotaError::None);
    // 认不出的文本 → Transient（**不**回落 None：把失败读成成功是假象）
    DOCTEST_CHECK(module::claude::quota::quotaErrorFromName("??") == QuotaError::Transient);
    return true;
})

REGISTER_TEST(claude_quota_store_never_succeeded_writes_no_fake_zero,
              "claude_quota_store: 从未成功过不写假 0%（kind 空 + 数据列保持初始值）", {
    test_support::TestDb tdb("quotastore", "never");
    QuotaStore store(tdb.handle());

    // **第一次就失败**（此前无任何成功值）：这一行必须能被 G7 识别成
    // 「失败且从未成功」→ 只渲染错误行，绝不画 0% 条。
    DOCTEST_REQUIRE(store.applyFetchResult(
                        9, failure(QuotaError::Shape, "响应结构不认识"), 1000).isOk());
    auto row = store.loadResult(9);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().neverSucceeded());
    DOCTEST_CHECK(!row.value().hasEverSucceeded);
    DOCTEST_CHECK(row.value().hasError());
    DOCTEST_CHECK(row.value().lastErrorKind == QuotaError::Shape);
    // 关键：数据列是**初始值**，不是「查到了 0%」
    DOCTEST_CHECK(row.value().windows.empty());
    DOCTEST_CHECK_EQ(row.value().balanceRemaining, 0.0);
    DOCTEST_CHECK(row.value().currency.empty());
    DOCTEST_CHECK(row.value().planName.empty());

    // 库里的原始行：kind 与 windows_json 两列都是空串（G7 若直读库也判得出）
    auto raw = tdb.handle().prepare(
        "SELECT kind, windows_json FROM profile_quota_result WHERE profile_id = 9");
    DOCTEST_REQUIRE(raw.isOk());
    auto rawRows = raw.value().fetchAll();
    DOCTEST_REQUIRE(rawRows.isOk());
    DOCTEST_REQUIRE_EQ(rawRows.value().size(), 1u);
    DOCTEST_CHECK_EQ(rawRows.value()[0].getString("kind").valueOr("?"), std::string(""));
    DOCTEST_CHECK_EQ(rawRows.value()[0].getString("windows_json").valueOr("?"),
                     std::string(""));
    return true;
})

// ============ 定时器到点触发（注入时钟 + 毫秒级 tick）============

REGISTER_TEST(claude_quota_refresher_fires_when_due,
              "claude_quota_refresher: enabled+auto_refresh 且到点才刷（注入时钟，绝不等 15 分钟）", {
    test_support::TestDb tdb("quotarefresh", "due");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    // 三个 profile：到期 / 未启用 / 未到点
    QuotaConfigRow due;
    due.profileId = 1; due.providerId = "scripted"; due.enabled = true;
    due.autoRefresh = true; due.intervalMinutes = 15;
    QuotaConfigRow disabled = due;
    disabled.profileId = 2; disabled.enabled = false;
    QuotaConfigRow notYet = due;
    notYet.profileId = 3;
    DOCTEST_REQUIRE(store.saveConfig(due).isOk());
    DOCTEST_REQUIRE(store.saveConfig(disabled).isOk());
    DOCTEST_REQUIRE(store.saveConfig(notYet).isOk());

    ScriptedProvider provider{okTiered()};
    ForwardingCredentialSource creds;
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 1;
    opts.sliceMs = 1;
    opts.scanIntervalSec = 0;          // 每片都扫（测试专用短间隔）
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string& id) -> QuotaProvider* {
                                 return id == "scripted" ? &provider : nullptr;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    // 首轮：profile 1 从未查过（fetched_at = 0）→ 立即到期
    auto first = refresher.refreshNow();
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_REQUIRE_EQ(first.value().size(), 3u);
    DOCTEST_CHECK(!first.value()[0].skipped);      // profile 1：刷了
    DOCTEST_CHECK(first.value()[0].persisted);
    DOCTEST_CHECK(first.value()[1].skipped);       // profile 2：未启用
    DOCTEST_CHECK(first.value()[2].persisted);     // profile 3：首轮同样到期
    DOCTEST_CHECK_EQ(provider.calls.load(), 2);

    // 立刻再跑一轮：都还没到 15 分钟 → 一个都不该刷
    auto second = refresher.refreshNow();
    DOCTEST_REQUIRE(second.isOk());
    for (const auto& r : second.value()) DOCTEST_CHECK(r.skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), 2);

    // 把注入时钟拨到 15 分钟之后 → 到期
    clock.now += 15 * 60 + 1;
    auto third = refresher.refreshNow();
    DOCTEST_REQUIRE(third.isOk());
    DOCTEST_CHECK(!third.value()[0].skipped);
    DOCTEST_CHECK(third.value()[1].skipped);       // profile 2 未启用：再久也不刷
    DOCTEST_CHECK(!third.value()[2].skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), 4);

    // 结果真落库了（不是只调了 provider）
    auto row = store.loadResult(1);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().hasEverSucceeded);
    DOCTEST_REQUIRE_EQ(row.value().windows.size(), 2u);
    DOCTEST_CHECK_EQ(creds.lastProfileId, int64_t(3));   // 按 profile_id 升序，最后是 3
    return true;
})

REGISTER_TEST(claude_quota_refresher_publishes_before_wake,
              "claude_quota_refresher: 刷新成功后确实发了唤醒（落库 → 唤醒，顺序在源码里）", {
    test_support::TestDb tdb("quotarefresh", "order");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1; cfg.providerId = "scripted"; cfg.enabled = true;
    cfg.autoRefresh = true; cfg.intervalMinutes = 15;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    ScriptedProvider provider{okTiered()};
    ForwardingCredentialSource creds;
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; },
                             QuotaRefreshOptions{});

    module::claude::quota::quotaRefresherResetTestSeams();
    // 消费掉可能存在的残留唤醒位，只看本次刷新发出的
    while (::core::platform::consumeUiUpdate()) {}
    auto round = refresher.refreshNow();
    DOCTEST_REQUIRE(round.isOk());

    // ⚠ **这条用例原来断言的那个量已被删除**（静态审计 M1）。
    // 原文断言 `quotaRefresherWakeSawReadyForTest()`，声称「唤醒那一刻读共享
    // 状态以证明结果已就绪」。实测它**恒为 true**：`applyFetchResult` 刚写完的
    // 行，紧接着 `loadResult` 走同一个 SqliteDb、同一把 recursive_mutex、
    // 中间无别的写者 → **必然**读得回。两次变异：
    //   · 退回无条件 set_value        → quota 167 例全绿
    //   · rowReadable 硬编码 false     → **仍然全绿**（缝永远红也抓不到）
    // 那条注释还写着「于是 DB 写坏了 / 行读不回来会让缝真的变红」，而实测红不了。
    // → 恒真的假守卫，比原来那条更隐蔽，故整条删除（含 wakeSawReady_ /
    //   quotaRefresherWakeSawReadyForTest() / 那行断言）。
    //
    // 现在本用例守的是**真的会变的量**：
    //   ① 刷新成功后确实发了唤醒（consumeUiUpdate 为真）——删掉 wakeAfterPublish
    //      这一行立刻红；
    //   ② 唤醒次数恰好 1（不是 0、不是 2）——删掉 fetch_add 立刻红；
    //   ③ 落库确实发生了（round 里的 persisted）——它承载了「先落库」这半边。
    // 「先落库、后唤醒」的**次序**由同一线程的语句次序保证（直线代码、无分支、
    // 编译器不得重排有 data dependence 的语句），没有可交错的点，因此**写不出
    // 让缝变红的场景** —— 硬造一条只会得到第二条恒真的假守卫。
    // 批次 D 那个 bug 的真正成因是「落库失败路径仍在发唤醒」，由
    // `if (persisted.isFail()) return out;` 覆盖，那一条**有**测试
    // （applyFetchResult 失败时推进 lastSuccessAt，静态审计变异 6 实测变红）。
    DOCTEST_CHECK_MESSAGE(
        ::core::platform::consumeUiUpdate(),
        "刷新完成后无人请求重绘：首页余量卡要等用户下一次动鼠标才更新");
    DOCTEST_CHECK_EQ(module::claude::quota::quotaRefresherWakeCountForTest(),
                     int64_t(1));
    DOCTEST_REQUIRE_EQ(round.value().size(), 1u);
    DOCTEST_CHECK_MESSAGE(round.value()[0].persisted,
                          "本轮结果未落库 —— 「先落库」这半边没了，"
                          "而它正是「后唤醒」成立的前提");
    // 落库之后行确实可读（这是「就绪」的**结果**判据，不是唤醒那一刻的缝）：
    // 它会在「落库根本没发生 / 落到了别的 profile」时变红。
    auto persisted = store.loadResult(1);
    DOCTEST_REQUIRE(persisted.isOk());
    DOCTEST_CHECK_MESSAGE(persisted.value().hasEverSucceeded,
                          "落库后读不回该行：唤醒发出去了但 UI 收割会读到旧值");
    return true;
})

REGISTER_TEST(claude_quota_refresher_thread_stops_without_hang,
              "claude_quota_refresher: 后台线程 stopAll 有界返回（不挂死、不泄漏）", {
    test_support::TestDb tdb("quotarefresh", "stop");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1; cfg.providerId = "scripted"; cfg.enabled = true;
    cfg.autoRefresh = true; cfg.intervalMinutes = 15;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    ScriptedProvider provider{okTiered()};
    ForwardingCredentialSource creds;
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 5;   // 一轮 = 5 × 20ms = 100ms
    opts.sliceMs = 20;
    opts.scanIntervalSec = 0;
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    refresher.start();
    // 线程确实在跑：轮询（有界，不用长 sleep）到第一次刷为止
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (provider.calls.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    DOCTEST_CHECK_MESSAGE(provider.calls.load() > 0,
                          "start() 后后台线程没有刷任何一轮（定时器没起）");

    // stopAll 必须在**远小于一个扫描周期**内返回：sleep 切片的设计意义就在
    // 这——「一次睡到底」的话这里要等 60 秒。
    const auto t0 = std::chrono::steady_clock::now();
    refresher.stopAll();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    DOCTEST_CHECK_MESSAGE(elapsed.count() < 2000,
                          "stopAll 挂死：退出会等整个扫描周期（sleep 切片失效）");

    // 幂等：再调一次不崩、不重复 join
    refresher.stopAll();
    // 析构再兜底一次（~QuotaRefresher 内 stopAll）——析构时还 joinable 即
    // std::terminate，本用例能活着返回即证明线程已收干
    return true;
})

REGISTER_TEST(claude_quota_refresher_unknown_provider_is_not_configured,
              "claude_quota_refresher: 未注册的 provider 落 NotConfigured（不静默跳过）", {
    test_support::TestDb tdb("quotarefresh", "unknown");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1; cfg.providerId = "写错的供应商"; cfg.enabled = true;
    cfg.autoRefresh = true;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    ForwardingCredentialSource creds;
    QuotaRefresher refresher(store, creds,
                             [](const std::string&) -> QuotaProvider* { return nullptr; },
                             HttpSendFn{}, [&clock] { return clock.now; },
                             QuotaRefreshOptions{});
    auto round = refresher.refreshNow();
    DOCTEST_REQUIRE(round.isOk());
    DOCTEST_REQUIRE_EQ(round.value().size(), 1u);
    DOCTEST_CHECK(!round.value()[0].skipped);
    DOCTEST_CHECK(round.value()[0].error == QuotaError::NotConfigured);

    auto row = store.loadResult(1);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().neverSucceeded());   // 绝不写假 0%
    DOCTEST_CHECK(row.value().lastErrorKind == QuotaError::NotConfigured);
    DOCTEST_CHECK_EQ(creds.calls.load(), 0);       // 没解析出 provider 就不该发查询
    return true;
})

REGISTER_TEST(claude_quota_is_due_predicate,
              "claude_quota_refresher: 到期判定的四条边界（关 / 未配 / 从未查 / 间隔）", {
    QuotaConfigRow cfg;
    cfg.profileId = 1;
    cfg.providerId = "p";
    cfg.enabled = true;
    cfg.autoRefresh = true;
    cfg.intervalMinutes = 15;

    DOCTEST_CHECK(QuotaRefresher::isDue(cfg, 1000, 0));        // 从未查过
    DOCTEST_CHECK(!QuotaRefresher::isDue(cfg, 1000, 1000));   // 刚查过
    DOCTEST_CHECK(!QuotaRefresher::isDue(cfg, 1000 + 899, 1000));   // 差 1 秒
    DOCTEST_CHECK(QuotaRefresher::isDue(cfg, 1000 + 900, 1000));    // 正好 15 分钟

    QuotaConfigRow off = cfg;
    off.enabled = false;
    DOCTEST_CHECK(!QuotaRefresher::isDue(off, 100000, 0));
    QuotaConfigRow noAuto = cfg;
    noAuto.autoRefresh = false;
    DOCTEST_CHECK(!QuotaRefresher::isDue(noAuto, 100000, 0));
    QuotaConfigRow unconfigured = cfg;
    unconfigured.providerId.clear();
    DOCTEST_CHECK(!QuotaRefresher::isDue(unconfigured, 100000, 0));
    // interval <= 0 = 到点即刷（每次扫描都算到期）
    QuotaConfigRow every = cfg;
    every.intervalMinutes = 0;
    DOCTEST_CHECK(QuotaRefresher::isDue(every, 1000, 1000));
    return true;
})

// ============ 凭据来源缝（G6 的接入点）============

REGISTER_TEST(claude_quota_credential_source_seam_is_swappable,
              "claude_quota_credential_source: 缝可替换（G6 只需换实现，不改本批代码）", {
    test_support::TestDb tdb("quotacred", "seam");
    QuotaStore store(tdb.handle());
    ProfileRepository repo(tdb.handle(), "QuotaSeamTestService");

    auto profile = repo.createProfile("余量档案", "https://api.example.com",
                                      "sk-test-0123456789", "model-x");
    DOCTEST_REQUIRE(profile.isOk());
    const int64_t pid = profile.value().id;

    QuotaConfigRow cfg;
    cfg.profileId = pid;
    cfg.providerId = "scripted";
    cfg.enabled = true;
    cfg.autoRefresh = true;
    cfg.extraJson = R"({"accessKeyId":"AK-9"})";
    cfg.secretsJson = R"({"secretAccessKey":"SK-9"})";
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    // 默认实现：读本 profile 自己的凭据（E1 的 api_key_encrypted）+ 配置表
    module::claude::quota::ProfileCredentialSource source(repo, store);
    ScriptedProvider provider{okTiered()};
    auto result = source.resolveAndFetch(pid, provider, HttpSendFn{});
    DOCTEST_CHECK(result.error == QuotaError::None);
    DOCTEST_CHECK_EQ(provider.calls.load(), 1);
    DOCTEST_CHECK_EQ(provider.lastBaseUrl, std::string("https://api.example.com"));
    const int callsAfterDefault = provider.calls.load();

    // 换成另一个实现（G6 的 cc-switch 源就是这一步）——缝的存在性证据：
    // 同样的虚接口，用例塞的实现立刻生效，本批代码一行没改。
    // 这次特意注入一个**与 provider 不同的结局**：provider 是成功的，
    // 但这个源说「凭据过期」。若假件又偷偷转发 provider（评审 M4 的原缺陷），
    // 下面这条 Auth 断言会立刻红——这就是「假件不假」的证据。
    ScriptedCredentialSource replacement{failure(QuotaError::Auth, "凭据过期")};
    auto viaReplacement = replacement.resolveAndFetch(pid, provider, HttpSendFn{});
    DOCTEST_CHECK(viaReplacement.error == QuotaError::Auth);
    DOCTEST_CHECK_EQ(viaReplacement.errorMessage, std::string("凭据过期"));
    DOCTEST_CHECK_EQ(replacement.lastProfileId, pid);
    DOCTEST_CHECK_EQ(replacement.calls.load(), 1);
    // 转发型假件则相反：结果由 provider 决定，provider 调几次就是几次
    ForwardingCredentialSource forwarder;
    auto viaForwarder = forwarder.resolveAndFetch(pid, provider, HttpSendFn{});
    DOCTEST_CHECK(viaForwarder.error == QuotaError::None);
    DOCTEST_CHECK_EQ(provider.calls.load(), callsAfterDefault + 1);

    // 没选供应商（provider_id 空串，G6 的「不配置」选项）→ NotConfigured
    //（**不是** fail()）：可预料的失败结局要带中文文案进 last_error，不是异常。
    // 配置行**存在**但 provider_id 为空，仍属「未配置」——漏这一判就会拿着没选
    // 供应商的 profile 去 fetch，出来的错误文案答非所问。
    QuotaConfigRow unconfigured;
    unconfigured.profileId = pid;
    unconfigured.providerId.clear();
    DOCTEST_REQUIRE(store.saveConfig(unconfigured).isOk());
    const int callsBefore = provider.calls.load();
    auto notCfg = source.resolveAndFetch(pid, provider, HttpSendFn{});
    DOCTEST_CHECK(notCfg.error == QuotaError::NotConfigured);
    DOCTEST_CHECK(!notCfg.errorMessage.empty());
    // 未配置 → 一个请求都不该发（拿没选供应商的 profile 去 fetch 是白跑一趟）
    DOCTEST_CHECK_EQ(provider.calls.load(), callsBefore);
    return true;
})

REGISTER_TEST(claude_quota_credential_source_reads_extra_and_secrets,
              "claude_quota_credential_source: extra/secrets 合并进 fields（secrets 优先）", {
    // 覆盖「配了额外字段」的 provider（火山 AK/SK）走的路径。
    class FieldCapturingProvider final : public QuotaProvider {
    public:
        std::string id() const override { return "fields"; }
        std::string displayName() const override { return "收字段"; }
        std::vector<ProviderField> fields() const override { return {}; }
        QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn&) const override {
            seen = cfg.fields;
            return QuotaResult{};
        }
        mutable std::map<std::string, std::string> seen;   // fetch 是 const 成员
    };

    test_support::TestDb tdb("quotacred", "fields");
    QuotaStore store(tdb.handle());
    ProfileRepository repo(tdb.handle(), "QuotaFieldsTestService");
    auto profile = repo.createProfile("字段档案", "https://api.volc.example",
                                      "sk-abc", "m");
    DOCTEST_REQUIRE(profile.isOk());
    const int64_t pid = profile.value().id;

    QuotaConfigRow cfg;
    cfg.profileId = pid;
    cfg.providerId = "volcano";
    cfg.extraJson = R"({"accessKeyId":"AK-1","region":"cn-beijing"})";
    cfg.secretsJson = R"({"secretAccessKey":"SK-1","accessKeyId":"AK-覆盖"})";
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    module::claude::quota::ProfileCredentialSource source(repo, store);
    FieldCapturingProvider provider;
    source.resolveAndFetch(pid, provider, HttpSendFn{});

    DOCTEST_CHECK_EQ(provider.seen.size(), 3u);
    DOCTEST_CHECK_EQ(provider.seen.at("accessKeyId"), std::string("AK-覆盖"));
    DOCTEST_CHECK_EQ(provider.seen.at("secretAccessKey"), std::string("SK-1"));
    DOCTEST_CHECK_EQ(provider.seen.at("region"), std::string("cn-beijing"));
    return true;
})

REGISTER_TEST(claude_quota_credential_source_drops_non_string_extra,
              "claude_quota_credential_source: extra_json 里的数字/布尔/对象一律丢（口径钉死）", {
    // m5：flatMapFromJson 静默丢非字符串值，此前无测试。口径 = **丢，不报错**
    //（理由见 quota_credential.cpp 该函数注释：fields 的契约是
    // map<string,string>；把数字 stringify 塞进去会让 provider 收到「看着合法、
    // 实则类型不对」的值，报错指向错误方向。丢 = 少一个必填字段 = provider
    // 说「这个字段没填」，那是对的）。
    test_support::TestDb tdb("quotacred", "nonstr");
    QuotaStore store(tdb.handle());
    ProfileRepository repo(tdb.handle(), "QuotaNonStringTestService");
    auto profile = repo.createProfile("混合字段", "https://api.mix.example",
                                      "sk-abc", "m");
    DOCTEST_REQUIRE(profile.isOk());
    const int64_t pid = profile.value().id;

    QuotaConfigRow cfg;
    cfg.profileId = pid;
    cfg.providerId = "fields";
    cfg.extraJson = R"({"good":"值","num":42,"flag":true,"obj":{"a":1}})";
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    class CapturingProvider final : public QuotaProvider {
    public:
        std::string id() const override { return "fields"; }
        std::string displayName() const override { return "收字段"; }
        std::vector<ProviderField> fields() const override { return {}; }
        QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn&) const override {
            seen = cfg.fields;
            return QuotaResult{};
        }
        mutable std::map<std::string, std::string> seen;
    };

    module::claude::quota::ProfileCredentialSource source(repo, store);
    CapturingProvider provider;
    source.resolveAndFetch(pid, provider, HttpSendFn{});

    DOCTEST_CHECK_EQ(provider.seen.size(), 1u);          // 只剩那个字符串
    DOCTEST_CHECK_EQ(provider.seen.at("good"), std::string("值"));
    DOCTEST_CHECK(provider.seen.find("num") == provider.seen.end());
    DOCTEST_CHECK(provider.seen.find("flag") == provider.seen.end());
    DOCTEST_CHECK(provider.seen.find("obj") == provider.seen.end());
    return true;
})

REGISTER_TEST(claude_quota_store_secrets_roundtrip_through_credential_protection,
              "claude_quota_store: secrets_json 经 platform::credential 存取（明文进、明文出）", {
    // M3：本批把 secrets_json 接上 protect/unprotect（与 api_key_encrypted
    // 复用同一对函数）——Windows 上写入即 DPAPI 密文，不给「按字面实现 DDL
    // 就落成明文」留缝。POSIX 上 protect 是恒等（保护靠 sqlite_db.cpp 每次
    // open 收紧 db/-wal/-shm 到 0600），故本用例在两平台都成立：
    // 结构体里始终是明文，调用方不感知存储形态。
    test_support::TestDb tdb("quotastore", "secrets");
    QuotaStore store(tdb.handle());
    const std::string secrets = R"({"secretAccessKey":"值-带-引号\"和\\反斜杠"})";

    QuotaConfigRow row;
    row.profileId = 1;
    row.providerId = "volcano";
    row.extraJson = R"({"accessKeyId":"AK"})";
    row.secretsJson = secrets;
    DOCTEST_REQUIRE(store.saveConfig(row).isOk());

    auto loaded = store.loadConfig(1);
    DOCTEST_REQUIRE(loaded.isOk());
    // 往返无损：调用方拿到的仍是明文 JSON 原文
    DOCTEST_CHECK_EQ(loaded.value().secretsJson, secrets);
    DOCTEST_CHECK_EQ(loaded.value().extraJson, row.extraJson);   // 非密文列不被动
    return true;
})

REGISTER_TEST(claude_quota_store_secrets_protect_failure_writes_nothing,
              "claude_quota_store: protect 失败时不写行（不留「行在、secret 没加密」中间态）", {
    // M3 的失败路径：注入一个 protect 失败的假后端（credential_protection.h
    // 的 Backend 注入缝，CPX-14 同款），断言 saveConfig **失败且库里没有行**。
    // 「建了行但 secret 没存进去」是比「根本没建」更难诊断的中间态。
    test_support::TestDb tdb("quotastore", "protectfail");

    platform::credential::Backend failing{};
    failing.protect = [](const std::string&) {
        return core::Result<std::string>::fail(core::ErrorCode::Internal,
                                               "注入的 protect 失败");
    };
    failing.unprotect = [](const std::string& s) { return core::Result<std::string>::ok(s); };
    const platform::credential::Backend* prev =
        platform::credential::setBackendForTest(&failing);

    QuotaStore store(tdb.handle());
    QuotaConfigRow row;
    row.profileId = 77;
    row.providerId = "volcano";
    row.secretsJson = R"({"secretAccessKey":"SK"})";
    const auto saved = store.saveConfig(row);
    platform::credential::setBackendForTest(prev);   // 复位后端（RAII 兜底）

    DOCTEST_CHECK(saved.isFail());
    DOCTEST_CHECK(store.loadConfig(77).isFail());   // 没有留下半截行
    return true;
})

REGISTER_TEST(claude_quota_store_apply_fetch_result_is_atomic_under_concurrency,
              "claude_quota_store: 并发 applyFetchResult 不丢更新（Transient 不覆盖并发成功）", {
    // M2：韧性依赖的「读回上次成功值 → 搬进新行 → 写回」是复合操作，锁只覆盖
    // 写的话非原子 —— 后台 ticker（T）读到旧行后，G6 的手动刷新（S）写进新数据，
    // T 恢复执行就把旧行整体盖回去（静默丢数据，还会挂「可能过期」角标看起来
    // 像正常行为）。
    //
    // 判据不能是「两次都成功」这种弱判据（两种锁法都满足 = 恒真）。这里用一个
    // **只增不减的版本号**把丢更新变成可判定的不变式，且版本号由 **DB 里的行
    // 自己**给出，不依赖任何线程内的共享变量：
    //   - S（手动刷新）每次成功写，planName = "S<全局递增版本 v>"
    //   - T（ticker）只写 Transient，把读到的 planName 原样搬回
    // 不变式：DB 里的 planName 版本**永不回退**。T 每次落库后回读，只要发现
    // 本次落库让版本号比「本次落库前读到的那个」小，就说明它盖掉了别人的新写。
    //
    // ⚠ 这里刻意**不用**「跟另一个线程的原子变量比」——那个变量在写成功之后才
    // 更新，与 DB 写入之间有一个极窄窗口，会让正确实现也偶发误报（实测 6000
    // 轮就有假阳性）。改成「写前读到的版本 vs 写后落库的版本」——两端都来自
    // 同一次调用内、同一把锁保护的读，与实现无关，判据稳定。
    test_support::TestDb tdb("quotastore", "concurrent");
    QuotaStore store(tdb.handle());

    // ⚠ 迭代数与线程数是**调出来的**：竞态窗口只有「读回返回」到「INSERT 拿锁」
    // 之间几条指令，采样点不够就撞不上（实测 2 线程 × 400 轮 0 命中；8 ticker
    // × 2500 迭代 6 次只中 2 次）。8 ticker × 8000 迭代是实测**两个方向都稳定**
    // 的最小档位：修复在位 6/6 绿、撤掉锁 6/6 红。代价是本用例约 7 秒。
    // 之所以不降档：一个假绿的并发用例比没有用例更糟——它会让后续批次以为
    // 「并发安全已被守住」。
    constexpr int kIters = 8000;
    std::atomic<int> version{0};
    std::atomic<int> lostUpdates{0};
    std::atomic<int> firstBad{-1};
    std::atomic<int> firstBadPrev{-1};

    // 从 "S<v>" 解析版本号；不是该形态（非 S 写的）→ -1
    auto versionOf = [](const std::string& planName) -> int {
        if (planName.size() < 2 || planName[0] != 'S') return -1;
        return std::atoi(planName.c_str() + 1);
    };

    // T（ticker）：只写 Transient；每次落库后回读校验版本没回退
    auto tickerLoop = [&] {
        for (int i = 0; i < kIters; ++i) {
            const auto before = store.loadResult(5);
            if (!store.applyFetchResult(
                    5, failure(QuotaError::Transient, "抖动"), 3000).isOk()) {
                continue;
            }
            const auto after = store.loadResult(5);
            if (after.isFail() || !after.value().hasError()) continue;
            const int v = versionOf(after.value().planName);
            if (v < 0) continue;   // 还没被 S 写过，无从比较
            const int prev = before.isOk() ? versionOf(before.value().planName) : -1;
            // 本次落库让版本回退了 = 盖掉了别人的新写（丢更新）
            if (prev >= 0 && v < prev) {
                if (lostUpdates.fetch_add(1) == 0) {
                    firstBad.store(v);
                    firstBadPrev.store(prev);
                }
            }
        }
    };

    // S（手动刷新）：每次成功写把版本推进
    std::thread success([&] {
        for (int i = 0; i < kIters; ++i) {
            const int v = version.fetch_add(1) + 1;
            QuotaResult r;
            r.kind = QuotaKind::Tiered;
            r.windows = {QuotaWindow{"five_hour", 42.0, ""}};
            r.planName = "S" + std::to_string(v);
            if (store.applyFetchResult(5, r, 2000).isOk()) version.store(v);
        }
    });
    std::vector<std::thread> tickers;
    for (int t = 0; t < 8; ++t) tickers.emplace_back(tickerLoop);
    success.join();
    for (auto& th : tickers) th.join();

    DOCTEST_CHECK_MESSAGE(lostUpdates.load() == 0,
        "读—改—写非原子：Transient 分支带着过期数据盖掉了并发写入的成功数据"
        "（静默数据丢失）。命中 " + std::to_string(lostUpdates.load()) +
        " 次，首次 S" + std::to_string(firstBadPrev.load()) +
        " 被盖回 S" + std::to_string(firstBad.load()));

    auto row = store.loadResult(5);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().hasEverSucceeded);
    DOCTEST_CHECK(!row.value().windows.empty());
    return true;
})

REGISTER_TEST(claude_quota_store_last_success_at_only_advances_on_success,
              "claude_quota_store: last_success_at 只在成功时推进（数据时间 ≠ 尝试时间）", {
    // m1：fetched_at 成败都推进（定时器用它，防重试风暴），last_success_at
    // 只在成功时推进（G7 显示「数据时间」用它）。合成一列会让 G7 在一次失败
    // 后显示「15 分钟前的数字 + 刚刚查询过」这种自相矛盾的话。
    test_support::TestDb tdb("quotastore", "successat");
    QuotaStore store(tdb.handle());

    DOCTEST_REQUIRE(store.applyFetchResult(1, okTiered(), 1000).isOk());
    auto first = store.loadResult(1);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_CHECK_EQ(first.value().fetchedAt, int64_t(1000));
    DOCTEST_CHECK_EQ(first.value().lastSuccessAt, int64_t(1000));

    // Transient 失败：尝试时间推进，**数据时间不动**
    DOCTEST_REQUIRE(store.applyFetchResult(
                        1, failure(QuotaError::Transient, "抖动"), 2000).isOk());
    auto afterFail = store.loadResult(1);
    DOCTEST_REQUIRE(afterFail.isOk());
    DOCTEST_CHECK_EQ(afterFail.value().fetchedAt, int64_t(2000));       // 尝试=2000
    DOCTEST_CHECK_EQ(afterFail.value().lastSuccessAt, int64_t(1000));   // 数据仍是 1000

    // 鉴权类失败同理（数据同样不动）
    DOCTEST_REQUIRE(store.applyFetchResult(
                        1, failure(QuotaError::Auth, "密钥无效"), 3000).isOk());
    auto afterAuth = store.loadResult(1);
    DOCTEST_REQUIRE(afterAuth.isOk());
    DOCTEST_CHECK_EQ(afterAuth.value().lastSuccessAt, int64_t(1000));

    // 再成功：两列一起推进
    DOCTEST_REQUIRE(store.applyFetchResult(1, okTiered(), 4000).isOk());
    auto healed = store.loadResult(1);
    DOCTEST_REQUIRE(healed.isOk());
    DOCTEST_CHECK_EQ(healed.value().fetchedAt, int64_t(4000));
    DOCTEST_CHECK_EQ(healed.value().lastSuccessAt, int64_t(4000));
    return true;
})

REGISTER_TEST(claude_quota_refresher_scan_throttle_branch,
              "claude_quota_refresher: scanIntervalSec=60 的节流分支（生产默认路径）", {
    // m3：唯一跑线程的用例设 scanIntervalSec=0，生产默认 60 秒那条
    // `continue` 分支从未被执行过。本用例跑线程 + 拨时钟，覆盖它。
    test_support::TestDb tdb("quotarefresh", "throttle");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1; cfg.providerId = "scripted"; cfg.enabled = true;
    cfg.autoRefresh = true; cfg.intervalMinutes = 0;   // 每次扫描都算到期
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    ScriptedProvider provider{okTiered()};
    ForwardingCredentialSource creds;
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 1;
    opts.sliceMs = 2;
    opts.scanIntervalSec = 60;    // ← 生产默认：这条节流分支
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> const QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    refresher.start();
    // 等到第一轮扫完（时钟没动，所以只应扫一次）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (provider.calls.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    DOCTEST_REQUIRE(provider.calls.load() >= 1);
    const int afterFirst = provider.calls.load();

    // 时钟**不动** → 节流分支应持续 continue，一轮都不该多刷
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    DOCTEST_CHECK_MESSAGE(provider.calls.load() == afterFirst,
                          "时钟没前进却仍在扫描：scanIntervalSec 节流失效");

    // 时钟拨过 60 秒 → 又扫一轮
    clock.now += 61;
    const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (provider.calls.load() == afterFirst &&
           std::chrono::steady_clock::now() < deadline2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    DOCTEST_CHECK_MESSAGE(provider.calls.load() > afterFirst,
                          "拨过 scanIntervalSec 后没有再扫一轮");
    refresher.stopAll();
    return true;
})

REGISTER_TEST(claude_quota_refresher_stop_waits_for_inflight_fetch,
              "claude_quota_refresher: stopAll 会等在途请求跑完（退出延迟口径的实测）", {
    // m4：头注释曾声称「最多等一片」，那只对**睡眠态**成立。线程正在
    // refreshNow() 里发请求时，join 会等到那一轮跑完。本用例用一个会
    // sleep 住的假 provider 钉住这个口径——**不是**断言「立刻返回」。
    test_support::TestDb tdb("quotarefresh", "inflight");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1; cfg.providerId = "slow"; cfg.enabled = true;
    cfg.autoRefresh = true; cfg.intervalMinutes = 0;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    class SlowProvider final : public QuotaProvider {
    public:
        std::string id() const override { return "slow"; }
        std::string displayName() const override { return "慢供应商"; }
        std::vector<ProviderField> fields() const override { return {}; }
        QuotaResult fetch(const ProviderConfig&, const HttpSendFn&) const override {
            entered.store(true);
            // 模拟一次 400ms 的真实请求（provider timeoutMs 量级 15s，
            // 这里取小值让用例快）
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            return okTiered();
        }
        mutable std::atomic<bool> entered{false};
    };

    SlowProvider provider;
    ForwardingCredentialSource creds;
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 1;
    opts.sliceMs = 2;
    opts.scanIntervalSec = 0;
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> const QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    refresher.start();
    // 等请求真的发出去（进入 fetch 体内）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!provider.entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    DOCTEST_REQUIRE(provider.entered.load());

    // 在途时 stopAll：应**等**这一轮跑完（而不是立刻返回把请求腰斩）
    const auto t0 = std::chrono::steady_clock::now();
    refresher.stopAll();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    DOCTEST_CHECK_MESSAGE(elapsed.count() >= 100,
                          "在途请求被腰斩：stopAll 立刻返回，线程仍在发请求");
    // 但也不能挂死：远小于 provider 的 timeoutMs 量级
    DOCTEST_CHECK_MESSAGE(elapsed.count() < 10000, "stopAll 在在途请求上挂死");
    // 等完了 = 结果真落库了（没有半截状态）
    DOCTEST_CHECK(store.loadResult(1).isOk());
    return true;
})

// ==================== G8：失败韧性收口 =================================
//
// spec §5.3 的四条收尾，本段逐条钉死。前三条此前**只有纯函数级**的覆盖
// （claude_quota_is_due_predicate 测 isDue、claude_quota_store_auth_error…
// 测落库），缺的是**整条链路**上的行为：失败后到底会不会重打供应商、
// 一个 profile 炸了会不会带倒整轮、四类错误会不会被「保留旧值」吞掉。
// 这里补的正是那三条「链路级」缺口。

// ---- 假凭据来源之三：**指定 profile 抛异常**，其余照常转发 ----
// 「单个 profile 失败不许中断整轮」这条只有它能测：另两个假件都不抛。
class SelectivelyThrowingCredentialSource final : public QuotaCredentialSource {
public:
    explicit SelectivelyThrowingCredentialSource(int64_t throwForProfileId)
        : throwFor_(throwForProfileId) {}

    QuotaResult resolveAndFetch(int64_t profileId, const QuotaProvider& provider,
                                const HttpSendFn& http) override {
        ++calls;
        if (profileId == throwFor_) {
            throw std::runtime_error("该 profile 的凭据层炸了（注入）");
        }
        return provider.fetch(ProviderConfig{}, http);
    }

    std::atomic<int> calls{0};

private:
    int64_t throwFor_;
};

// 四类「非 Transient」错误的公共核对：last_error / last_error_kind 都写对，
// 且**旧数据仍在**（错误不被保留逻辑吞掉）。
// 返回 false 时由调用方用例判红——用例名带错误分类名，失败时一眼看出是哪类坏了。
bool checkErrorClassNotMasked(QuotaStore& store, int64_t profileId, QuotaError kind,
                              const char* label) {
    std::string why;
    auto row = store.loadResult(profileId);
    if (row.isFail()) {
        why = "loadResult 失败";
    } else if (row.value().lastErrorKind != kind) {
        why = "lastErrorKind 不等于该分类";
    } else if (row.value().lastError.empty()) {
        why = "last_error 为空串（UI 会画出空白错误行 = 静默失败面）";
    } else if (row.value().lastErrorKind == QuotaError::Transient) {
        why = "被降级成 Transient（会挂「可能过期」角标掩盖真因）";
    } else if (!row.value().hasEverSucceeded) {
        why = "有旧值却被抹掉了（hasEverSucceeded=false）";
    } else if (row.value().windows.size() != 2u) {
        why = "旧窗口被抹掉了";
    } else if (row.value().windows[0].usedPercent != 62.5) {
        why = "旧窗口数值被改写";
    } else if (row.value().lastSuccessAt != 1000) {
        why = "lastSuccessAt 被失败推进了（数据时间与数据本身自相矛盾）";
    }
    if (!why.empty()) {
        std::cerr << "FAIL [" << label << "] " << why << std::endl;
        return false;
    }
    return true;
}

REGISTER_TEST(claude_quota_refresher_failure_advances_attempt_clock,
              "claude_quota_refresher: 失败也推进 fetched_at —— 持续失败不重试风暴", {
    test_support::TestDb tdb("quotarefresh", "failclock");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1;
    cfg.providerId = "scripted";
    cfg.enabled = true;
    cfg.autoRefresh = true;
    cfg.intervalMinutes = 15;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    // provider **一直失败**（网络一直抖 / 上游一直 5xx）
    ScriptedProvider provider{failure(QuotaError::Transient, "网络超时")};
    ForwardingCredentialSource creds;
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 1;
    opts.sliceMs = 1;
    opts.scanIntervalSec = 0;   // 每片都扫：模拟后台 tick 不停地来
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    auto first = refresher.refreshNow();
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_REQUIRE_EQ(first.value().size(), 1u);
    DOCTEST_CHECK(!first.value()[0].skipped);
    DOCTEST_CHECK(first.value()[0].persisted);
    DOCTEST_CHECK_EQ(provider.calls.load(), 1);

    // 失败**也**写了 fetched_at（到期判定从「上次尝试」起算，见 quota_store.h）
    auto row = store.loadResult(1);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().fetchedAt, clock.now);

    // 🔴 关键断言：网络持续失败时，间隔内**不许**再打供应商。
    // 若 fetched_at 只在成功时推进（拿到的是 0 / 旧值），这里每分钟都会重打一次
    // → provider.calls 从 1 涨到 6，重试风暴。
    for (int minute = 1; minute <= 5; ++minute) {
        clock.now += 60;   // 每分钟扫一次，合计 5 分钟 < 15 分钟间隔
        auto again = refresher.refreshNow();
        DOCTEST_REQUIRE(again.isOk());
        DOCTEST_CHECK(again.value()[0].skipped);
    }
    DOCTEST_CHECK_EQ(provider.calls.load(), 1);

    // 防线不是「永不重试」，是「按间隔重试」：越过 15 分钟后重新到期
    clock.now += 15 * 60;
    auto due = refresher.refreshNow();
    DOCTEST_REQUIRE(due.isOk());
    DOCTEST_CHECK(!due.value()[0].skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), 2);
    return true;
})

REGISTER_TEST(claude_quota_refresher_one_bad_profile_does_not_break_round,
              "claude_quota_refresher: 单个 profile 抛异常，其余照常刷新落库", {
    test_support::TestDb tdb("quotarefresh", "onebad");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    for (int64_t id = 1; id <= 3; ++id) {
        QuotaConfigRow cfg;
        cfg.profileId = id;
        cfg.providerId = "scripted";
        cfg.enabled = true;
        cfg.autoRefresh = true;
        cfg.intervalMinutes = 15;
        DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());
    }

    ScriptedProvider provider{okTiered()};
    // profile 2 抛异常；1 与 3 正常转发
    SelectivelyThrowingCredentialSource creds(2);
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 1;
    opts.sliceMs = 1;
    opts.scanIntervalSec = 0;
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    auto round = refresher.refreshNow();
    DOCTEST_REQUIRE(round.isOk());
    // 三个 profile 都有产出（一个都没被整轮吞掉）
    DOCTEST_REQUIRE_EQ(round.value().size(), 3u);
    // 炸的那个被标成 skipped + 有原因，且**没有**被记成成功
    DOCTEST_CHECK(round.value()[1].skipped);
    DOCTEST_CHECK(!round.value()[1].persisted);
    DOCTEST_CHECK(!round.value()[1].skipReason.empty());
    // 前后两个照常刷新落库 —— 这才是「整轮不中断」的真判据
    DOCTEST_CHECK(round.value()[0].persisted);
    DOCTEST_CHECK(round.value()[2].persisted);
    // provider 只被正常的两个 profile 调了 2 次（炸的那个没走到 fetch）
    DOCTEST_CHECK_EQ(provider.calls.load(), 2);

    // 落库事实：1 / 3 有结果，2 没有
    DOCTEST_CHECK(store.loadResult(1).isOk());
    DOCTEST_CHECK(!store.loadResult(2).isOk());
    DOCTEST_CHECK(store.loadResult(3).isOk());
    return true;
})

REGISTER_TEST(claude_quota_store_auth_not_masked_by_retention,
              "claude_quota_store: Auth 立即透出（旧值保留但错误不被吞）", {
    test_support::TestDb tdb("quotastore", "maskauth");
    QuotaStore store(tdb.handle());
    DOCTEST_REQUIRE(store.applyFetchResult(1, okTiered(), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        1, failure(QuotaError::Auth, "密钥无效"), 2000).isOk());
    return checkErrorClassNotMasked(store, 1, QuotaError::Auth, "Auth");
})

REGISTER_TEST(claude_quota_store_entitlement_not_masked_by_retention,
              "claude_quota_store: Entitlement 立即透出（旧值保留但错误不被吞）", {
    test_support::TestDb tdb("quotastore", "maskent");
    QuotaStore store(tdb.handle());
    DOCTEST_REQUIRE(store.applyFetchResult(1, okTiered(), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        1, failure(QuotaError::Entitlement, "套餐过期"), 2000).isOk());
    return checkErrorClassNotMasked(store, 1, QuotaError::Entitlement, "Entitlement");
})

REGISTER_TEST(claude_quota_store_shape_not_masked_by_retention,
              "claude_quota_store: Shape 立即透出（旧值保留但错误不被吞）", {
    test_support::TestDb tdb("quotastore", "maskshape");
    QuotaStore store(tdb.handle());
    DOCTEST_REQUIRE(store.applyFetchResult(1, okTiered(), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        1, failure(QuotaError::Shape, "响应结构不认识"), 2000).isOk());
    return checkErrorClassNotMasked(store, 1, QuotaError::Shape, "Shape");
})

REGISTER_TEST(claude_quota_store_not_configured_not_masked_by_retention,
              "claude_quota_store: NotConfigured 立即透出（旧值保留但错误不被吞）", {
    test_support::TestDb tdb("quotastore", "masknotcfg");
    QuotaStore store(tdb.handle());
    DOCTEST_REQUIRE(store.applyFetchResult(1, okTiered(), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        1, failure(QuotaError::NotConfigured, "配置不完整"), 2000).isOk());
    return checkErrorClassNotMasked(store, 1, QuotaError::NotConfigured, "NotConfigured");
})

REGISTER_TEST(claude_quota_store_error_headline_points_at_the_right_fix,
              "claude_quota_store: 四类错误的头条文案各自指向正确处置（不把用户引向错方向）", {
    // 「文案把用户引向错误方向」是本批明确要找的缺陷类型。四类错误的处置方向
    // 各不相同，把 Entitlement 说成「换密钥」或把 Transient 说成「套餐过期」
    // 都会让用户白折腾。这里逐类钉死 headline 的**关键词**。
    const auto headline = [](QuotaError kind) {
        return module::claude::quota::quotaErrorHeadline(kind, std::string());
    };
    // Auth → 让用户换密钥
    DOCTEST_CHECK(headline(QuotaError::Auth).find("换一把") != std::string::npos);
    DOCTEST_CHECK(headline(QuotaError::Auth).find("重试") == std::string::npos);
    // Entitlement → 明说换密钥没用、得续费（minimax 套餐过期走的就是这条）
    DOCTEST_CHECK(headline(QuotaError::Entitlement).find("换密钥没用") != std::string::npos);
    DOCTEST_CHECK(headline(QuotaError::Entitlement).find("续费") != std::string::npos);
    DOCTEST_CHECK(headline(QuotaError::Entitlement).find("换一把") == std::string::npos);
    // Transient → 才轮到「重试」
    DOCTEST_CHECK(headline(QuotaError::Transient).find("重试") != std::string::npos);
    // Shape → 是「供应商改了接口」，不是「你的网络有问题」
    DOCTEST_CHECK(headline(QuotaError::Shape).find("结构") != std::string::npos);
    DOCTEST_CHECK(headline(QuotaError::Shape).find("重试") == std::string::npos);
    // NotConfigured → 去补配置，不是重试
    DOCTEST_CHECK(headline(QuotaError::NotConfigured).find("补全") != std::string::npos);
    DOCTEST_CHECK(headline(QuotaError::NotConfigured).find("重试") == std::string::npos);
    // None 不该有文案（成功态）
    DOCTEST_CHECK(headline(QuotaError::None).empty());
    return true;
})

REGISTER_TEST(claude_quota_production_scan_interval_is_single_source,
              "claude_quota_refresh: 扫描间隔只有一个真相源（结构默认值 == 承诺给用户的秒数）", {
    // G6 裁决 8 的欠账兑现：以前 toast 文案与 QuotaRefreshOptions 各写一个字面量
    // 60，改一边另一边不动。现在两边都从 kQuotaProductionScanIntervalSec 取。
    // 本用例把那个常量钉在 60 上：改它是产品决策（用户承诺的自动查询延迟变了），
    // 必须连本用例一起改，避免无声漂移。
    DOCTEST_CHECK_EQ(module::claude::quota::kQuotaProductionScanIntervalSec, 60);
    // 结构默认值真的取自那个常量（不是又抄了一遍 60）
    const QuotaRefreshOptions defaults;
    DOCTEST_CHECK_EQ(defaults.scanIntervalSec,
                     module::claude::quota::kQuotaProductionScanIntervalSec);
    // 用例注入的 0 仍能覆盖（测试专用短间隔这条路没被常量锁死）
    QuotaRefreshOptions injected;
    injected.scanIntervalSec = 0;
    DOCTEST_CHECK_EQ(injected.scanIntervalSec, 0);
    return true;
})

REGISTER_TEST(claude_quota_refresher_manual_path_bypasses_interval_and_auto_refresh,
              "claude_quota_refresher: 手动刷新绕过 interval 与 auto_refresh（G6 L1 的唤醒通道终点）", {
    // G6 遗留 L1「保存后不即时刷新」的收口：G7 给首页 [刷新] 按钮加了**唤醒通道**
    //（进程级 atomic 请求位 + tick 线程每片查），tick 拿到位后走
    // refreshNow(manual=true)。本用例盯的就是那个终点——**不改**它，不要求它
    // 同步 refreshNow（后者是 15s×N 的真网络查询，会冻 UI）。
    //
    // 这里的配置刻意设成「自动刷新关 + 刚查过」：这两种情况下
    // refreshNow(false) 一定不刷，于是「手动通道有没有真的开一路」无所遁形。
    test_support::TestDb tdb("quotarefresh", "manual");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1;
    cfg.providerId = "scripted";
    cfg.enabled = true;
    cfg.autoRefresh = false;        // ← 手动通道必须无视它
    cfg.intervalMinutes = 15;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    ScriptedProvider provider{okTiered()};
    ForwardingCredentialSource creds;
    QuotaRefreshOptions opts;
    opts.slicesPerRound = 1;
    opts.sliceMs = 1;
    opts.scanIntervalSec = 0;
    QuotaRefresher refresher(store, creds,
                             [&provider](const std::string&) -> QuotaProvider* {
                                 return &provider;
                             },
                             HttpSendFn{}, [&clock] { return clock.now; }, opts);

    // 自动通道：auto_refresh=false → 一条都不刷
    auto autoRound = refresher.refreshNow(false);
    DOCTEST_REQUIRE(autoRound.isOk());
    DOCTEST_CHECK(autoRound.value()[0].skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), 0);

    // 手动通道：无视 auto_refresh 与 interval，真的发一次
    auto manualRound = refresher.refreshNow(true);
    DOCTEST_REQUIRE(manualRound.isOk());
    DOCTEST_CHECK(!manualRound.value()[0].skipped);
    DOCTEST_CHECK(manualRound.value()[0].persisted);
    DOCTEST_CHECK_EQ(provider.calls.load(), 1);

    // 结果真落库（不是只调了 provider）
    auto row = store.loadResult(1);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(row.value().hasEverSucceeded);

    // 手动刷新**不**把 auto_refresh 打开：再走自动通道依然不刷
    auto afterManual = refresher.refreshNow(false);
    DOCTEST_REQUIRE(afterManual.isOk());
    DOCTEST_CHECK(afterManual.value()[0].skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), 1);

    // 两道闸仍然生效：enabled=false 的档，手动通道也**不**该强行刷。
    // 注意手动一轮会**同时**重刷所有「enabled 且配了 provider」的档（profile 1
    // 也算），所以这里断言的是「调用次数只 +1」——如果 enabled=false 那道闸
    // 失效，次数会 +2（profile 1 与 2 各一次）。
    QuotaConfigRow off = cfg;
    off.profileId = 2;
    off.enabled = false;
    DOCTEST_REQUIRE(store.saveConfig(off).isOk());
    const int callsBefore = provider.calls.load();
    auto manualOff = refresher.refreshNow(true);
    DOCTEST_REQUIRE(manualOff.isOk());
    DOCTEST_REQUIRE_EQ(manualOff.value().size(), 2u);
    DOCTEST_CHECK(manualOff.value()[1].skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), callsBefore + 1);   // 只有 profile 1

    // provider_id 为空的档同样不该被强行查询
    QuotaConfigRow none = cfg;
    none.profileId = 3;
    none.providerId.clear();
    DOCTEST_REQUIRE(store.saveConfig(none).isOk());
    const int callsBeforeNone = provider.calls.load();
    auto manualNone = refresher.refreshNow(true);
    DOCTEST_REQUIRE(manualNone.isOk());
    DOCTEST_REQUIRE_EQ(manualNone.value().size(), 3u);
    DOCTEST_CHECK(manualNone.value()[2].skipped);
    DOCTEST_CHECK_EQ(provider.calls.load(), callsBeforeNone + 1);  // 只有 profile 1
    return true;
})
