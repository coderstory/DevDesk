// tests/test_modules/test_claude_quota_store.cpp
// G5：余量两表存储 + 后台定时刷新契约
// （计划 docs/superpowers/plans/2026-10-02-quota-implementation-plan.md §G5）
//
// 覆盖：
//   - windows_json 编解码**往返**（空数组 / 多窗口 / 小数不截断 / 坏输入容错）
//   - 两表 CRUD（配置行 / 结果行；无行 → NotFound 而非默认行）
//   - 失败韧性：Transient 保留上次成功值；鉴权类立即透出
//   - **「从未成功过」不写假 0%**：kind 列空 + 数据列保持初始值
//   - 定时器到点触发（**注入时钟 + 毫秒级 tick**，绝不等 15 分钟）
//   - **先就绪、后唤醒**次序（缝在唤醒那一刻读共享状态，不是抄源码顺序）
//   - 线程退出不挂死（stopAll 有界返回）
#include "doctest.h"
#include "test_support.h"

#include <core/platform/platform.h>   // consumeUiUpdate（唤醒位读取，同批次 D 用例）

#include "module_claude_proxy/profile_repository.h"
#include "quota/quota_credential.h"
#include "quota/quota_refresh.h"
#include "quota/quota_store.h"

#include <atomic>
#include <chrono>
#include <map>
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

    QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn& http) override {
        ++calls;
        lastBaseUrl = cfg.baseUrl;
        (void)http;   // 本 provider 不发请求（零真实网络）
        return result_;
    }

    std::atomic<int> calls{0};
    std::string lastBaseUrl;

private:
    QuotaResult result_;
};

// ---- 假凭据来源：不经 ProfileRepository，直接给固定结果（G6 的缝的替身）----
class ScriptedCredentialSource final : public QuotaCredentialSource {
public:
    explicit ScriptedCredentialSource(QuotaResult result)
        : result_(std::move(result)) {}

    QuotaResult resolveAndFetch(int64_t profileId, QuotaProvider& provider,
                                const HttpSendFn& http) override {
        ++calls;
        lastProfileId = profileId;
        return provider.fetch(ProviderConfig{}, http);
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
    ScriptedCredentialSource creds{okTiered()};
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
              "claude_quota_refresher: 先就绪后唤醒（缝在唤醒那一刻读共享状态）", {
    test_support::TestDb tdb("quotarefresh", "order");
    QuotaStore store(tdb.handle());
    FakeClock clock;

    QuotaConfigRow cfg;
    cfg.profileId = 1; cfg.providerId = "scripted"; cfg.enabled = true;
    cfg.autoRefresh = true; cfg.intervalMinutes = 15;
    DOCTEST_REQUIRE(store.saveConfig(cfg).isOk());

    ScriptedProvider provider{okTiered()};
    ScriptedCredentialSource creds{okTiered()};
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

    // 唤醒确实发了（结果就绪后没人请求重绘 = 主循环停在 glfwWaitEvents，
    // 内容要等用户下一次动鼠标才上屏）
    DOCTEST_CHECK_MESSAGE(
        ::core::platform::consumeUiUpdate(),
        "刷新完成后无人请求重绘：首页余量卡要等用户下一次动鼠标才更新");
    DOCTEST_CHECK_EQ(module::claude::quota::quotaRefresherWakeCountForTest(),
                     int64_t(1));
    // **次序断言**（本用例的判据）：唤醒发出那一刻结果必须**已经就绪**。
    // 「先唤醒、后就绪」是批次 D 那个间歇复现的 bug 的形态——UI 抢跑消费掉
    // 唤醒位、脏标记已清 → 结果落在内存里没人再上屏。窗口只有纳秒级，
    // 光看最终结果抓不到，所以缝在唤醒那一刻真读一次共享状态。
    DOCTEST_CHECK_MESSAGE(
        module::claude::quota::quotaRefresherWakeSawReadyForTest(),
        "唤醒先于结果就绪：UI 可能抢跑消费唤醒位 → 结果不上屏（次序必须是"
        "「先落库、后 requestUiUpdate」）");
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
    ScriptedCredentialSource creds{okTiered()};
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

    ScriptedCredentialSource creds{okTiered()};
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

    // 换成另一个实现（G6 的 cc-switch 源就是这一步）——缝的存在性证据：
    // 同样的虚接口，用例塞的实现立刻生效，本批代码一行没改。
    ScriptedCredentialSource replacement{okTiered()};
    auto viaReplacement = replacement.resolveAndFetch(pid, provider, HttpSendFn{});
    DOCTEST_CHECK(viaReplacement.error == QuotaError::None);
    DOCTEST_CHECK_EQ(replacement.lastProfileId, pid);

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
        QuotaResult fetch(const ProviderConfig& cfg, const HttpSendFn&) override {
            seen = cfg.fields;
            return QuotaResult{};
        }
        std::map<std::string, std::string> seen;
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
