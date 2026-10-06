// tests/test_modules/test_home_quota_card.cpp
// 首页余量卡片：五态 + Pending 判据 / 色带边界 / 手动刷新唤醒 / 去配置弹窗 / 行序稳定。
//
// 本文件的断言写法约定，勿"顺手改回"更省事的写法：
// - 断言节点存在/不存在，不要退回「状态枚举 == X」——那只证明判据函数对了，
//   证明不了绘制层照它画了；枚举对照另开纯函数用例。
// - 期望值一律写字面量（列文本 "auth" / 颜色阈值 89.9 / 元素 id 字符串），
//   不要用 fields()、kQuotaProviders 这类被测对象自己的输出来构造期望，
//   那会让实现和期望一起错。
// - 「不存在」类断言必须带对照组（Normal 行必须有条）：没有对照组的
//   「不存在」断言可能只是因为根本没渲染任何东西而恒真。
// - EUI-NEO 的 dialog 关着时照样把整棵树建出来（只把 panel 的 opacity 设 0 并
//   disable），判「弹窗开着」必须看 panel 的 opacity，find != nullptr 恒真。
#include "doctest.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "app_registry.h"
#include "claude_service.h"
#include "module_home/home_quota_card.h"
#include "quota_home_bridge.h"
#include "quota/quota_provider.h"
#include "quota/quota_refresh.h"
#include "quota/quota_store.h"

#include <eui/dsl.h>

#include <cmath>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace cq = module::claude::quota;
namespace hc = module::home::quota_card;

constexpr const char* kDbPrefix = "devdesk_test_homequota_";

cq::QuotaConfigRow makeConfig(int64_t profileId, const std::string& providerId) {
    cq::QuotaConfigRow c;
    c.profileId = profileId;
    c.providerId = providerId;
    c.enabled = !providerId.empty();
    c.autoRefresh = true;
    c.intervalMinutes = 15;
    return c;
}

cq::QuotaWindow makeWindow(const char* key, double used, const char* resets) {
    cq::QuotaWindow w;
    w.key = key;
    w.usedPercent = used;
    w.resetsAt = resets;
    return w;
}

cq::QuotaResult tieredOk(std::vector<cq::QuotaWindow> windows) {
    cq::QuotaResult r;
    r.kind = cq::QuotaKind::Tiered;
    r.windows = std::move(windows);
    r.error = cq::QuotaError::None;
    return r;
}

cq::QuotaResult balanceOk(double amount, const char* currency) {
    cq::QuotaResult r;
    r.kind = cq::QuotaKind::Balance;
    r.balanceRemaining = amount;
    r.currency = currency;
    r.error = cq::QuotaError::None;
    return r;
}

cq::QuotaResult fail(cq::QuotaError kind, const std::string& msg) {
    cq::QuotaResult r;
    r.error = kind;
    r.errorMessage = msg;
    return r;
}

int64_t insertProfile(core::data::SqliteDb& db, const std::string& name) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    DOCTEST_REQUIRE(db.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
        "api_key_check, model, created_at, updated_at) VALUES ('" + name +
        "', 'https://example.invalid', 'sk-***', 'chk', 'm', " +
        std::to_string(now) + ", " + std::to_string(now) + ")").isOk());
    auto st = db.prepare("SELECT last_insert_rowid() AS id");
    DOCTEST_REQUIRE(st.isOk());
    auto rows = st.value().fetchAll();
    DOCTEST_REQUIRE(rows.isOk() && rows.value().size() == 1);
    return rows.value()[0].getInt64("id").value();
}

// composeFrame 只建树，不排版。读任何 frame 之前必须自己调 ui.layout()，
// 否则 frame 全是 0，基于 frame 的比较（如「A 的底 ≤ B 的底 + 0.5」）会恒真。
void composeFrame(::core::Module& mod, eui::Ui& ui, eui::Screen screen,
                  const char* treeId) {
    ui.begin(treeId);
    mod.compose(ui, screen);
    ui.end();
}

bool clickById(eui::Ui& ui, const std::string& id) {
    const auto* node = ui.find(id);
    if (node == nullptr || node->onClick == nullptr) return false;
    node->onClick();
    return true;
}

// kEps 取 1/255：主题色经插值/量化后与 token 存在这个量级的偏差。
// 判「等于某 token」必须用 colorsNear；判「不等于」必须用 colorsDiffer，
// 写相等性断言会因这点偏差恒红。
bool colorsNear(const eui::Color& a, const eui::Color& b) {
    constexpr float kEps = 0.004f;
    return std::fabs(a.r - b.r) <= kEps && std::fabs(a.g - b.g) <= kEps &&
           std::fabs(a.b - b.b) <= kEps;
}

bool colorsDiffer(const eui::Color& a, const eui::Color& b) {
    return !colorsNear(a, b);
}

// 种子铺七行，覆盖五态 + 余额型 + 等待首次；返回各行 profileId（按序）。
// 这些 profileId 是行序断言的基准：行 id = "home.quota.row." + profile_id。
struct SeedIds {
    int64_t unconfigured = 0;
    int64_t incomplete = 0;
    int64_t normal = 0;
    int64_t failedStale = 0;
    int64_t failedNever = 0;
    int64_t balance = 0;
    int64_t pending = 0;
};

// 出参而非返回值 + 失败即抛：DOCTEST_REQUIRE 宏自带 `return <bool>`，在 void
// 函数里编译不过（-Wreturn-mismatch = -Werror）。抛异常由 test_main 逐用例 catch，
// 记成一条 Failed —— 与 test_support.h「构造失败即抛」同一口径。
void seedSevenRows(core::data::SqliteDb& db, SeedIds& ids) {
    cq::QuotaStore store(db);
    const int64_t now = 1000;
    const auto must = [](bool ok, const char* what) {
        if (!ok) {
            throw std::runtime_error(std::string("余量卡种子落库失败: ") + what);
        }
    };

    ids.unconfigured = insertProfile(db, "P1-未配置");
    must(store.saveConfig(makeConfig(ids.unconfigured, "")).isOk(), "P1");

    ids.incomplete = insertProfile(db, "P2-配置不完整");
    must(store.saveConfig(makeConfig(ids.incomplete, "volcano")).isOk(), "P2cfg");
    must(store.applyFetchResult(ids.incomplete,
                                fail(cq::QuotaError::NotConfigured,
                                     "缺少 accessKeyId"),
                                now)
             .isOk(),
         "P2res");

    ids.normal = insertProfile(db, "P3-正常");
    must(store.saveConfig(makeConfig(ids.normal, "opencode_go")).isOk(), "P3cfg");
    must(store.applyFetchResult(
             ids.normal,
             tieredOk({makeWindow("five_hour", 62.0, ""),
                       makeWindow("weekly", 91.0, ""),
                       makeWindow("monthly", 45.0, "2026-10-10T08:00:00")}),
             now)
             .isOk(),
         "P3res");

    ids.failedStale = insertProfile(db, "P4-失败有旧值");
    must(store.saveConfig(makeConfig(ids.failedStale, "minimax_cn")).isOk(),
         "P4cfg");
    // 必须先成功一次再失败：只有「有过成功值 + 当前有错」才构成 FailedStale
    must(store.applyFetchResult(
             ids.failedStale,
             tieredOk({makeWindow("five_hour", 30.0, "")}), now)
             .isOk(),
         "P4ok");
    must(store.applyFetchResult(ids.failedStale,
                                fail(cq::QuotaError::Auth, "HTTP 401"), now + 60)
             .isOk(),
         "P4err");

    ids.failedNever = insertProfile(db, "P5-失败未成功");
    must(store.saveConfig(makeConfig(ids.failedNever, "deepseek")).isOk(),
         "P5cfg");
    must(store.applyFetchResult(ids.failedNever,
                                fail(cq::QuotaError::Auth, "HTTP 401"), now)
             .isOk(),
         "P5res");

    ids.balance = insertProfile(db, "P6-余额型");
    must(store.saveConfig(makeConfig(ids.balance, "deepseek")).isOk(), "P6cfg");
    // Balance 成功时 windows 恒空 —— 「是否成功过」只能看 kind 列，
    // 不能用 windows 空不空当判据
    must(store.applyFetchResult(ids.balance, balanceOk(12.34, "CNY"), now)
             .isOk(),
         "P6res");

    ids.pending = insertProfile(db, "P7-等待首次");
    must(store.saveConfig(makeConfig(ids.pending, "opencode_go")).isOk(),
         "P7cfg");
    // 刻意不写结果行：Pending 态 = 配好了但一次都没查过
}

}  // namespace

// quotaCardState(providerId, lastErrorKind, hasEverSucceeded) 的优先级契约：
//   providerId 为空 → Unconfigured（不看另两个入参）
//   否则有错 + 从未成功 + NotConfigured → Incomplete
//   否则有错 + 从未成功 + 其它错误    → FailedNever
//   否则有错 + 有过成功              → FailedStale
//   否则无错 + 有过成功              → Normal
//   否则无错 + 从未成功              → Pending
// lastErrorKind 的空串是「无错」：列默认值 '' 表示这行从没写过错误，
// 当成 Transient 会把刚建好的行显示成「网络失败」。
REGISTER_TEST(home_quota_state_decision_table,
              "home quota: 五态判据纯函数真值表（列文本字面量）", {
    using S = cq::QuotaCardState;
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("", "none", true)),
                     static_cast<int>(S::Unconfigured));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("", "auth", false)),
                     static_cast<int>(S::Unconfigured));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "none", true)),
                     static_cast<int>(S::Normal));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "", false)),
                     static_cast<int>(S::Pending));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "auth", true)),
                     static_cast<int>(S::FailedStale));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "not_configured", true)),
                     static_cast<int>(S::FailedStale));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("volcano", "not_configured", false)),
                     static_cast<int>(S::Incomplete));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("deepseek", "auth", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "entitlement", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "shape", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "transient", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "", true)),
                     static_cast<int>(S::Normal));
    return true;
})

REGISTER_TEST(home_quota_shows_bar_gate,
              "home quota: 只有 Normal / FailedStale 画条（假 0% 红线的代码形态）", {
    using S = cq::QuotaCardState;
    DOCTEST_CHECK(hc::showsQuotaBar(S::Normal));
    DOCTEST_CHECK(hc::showsQuotaBar(S::FailedStale));
    DOCTEST_CHECK(!hc::showsQuotaBar(S::FailedNever));
    DOCTEST_CHECK(!hc::showsQuotaBar(S::Incomplete));
    DOCTEST_CHECK(!hc::showsQuotaBar(S::Unconfigured));
    DOCTEST_CHECK(!hc::showsQuotaBar(S::Pending));
    return true;
})

REGISTER_TEST(home_quota_five_state_render,
              "home quota: 五态逐态渲染（节点级断言）", {
    test_support::TestEnv env(kDbPrefix, "five", true);
    DOCTEST_REQUIRE(env.ready);
    SeedIds ids;
    seedSevenRows(env.db->db(), ids);

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;   // 装得下 7 行，不至于把别的行挤出视口
    eui::Ui ui;
    composeFrame(*mod, ui, screen, "home_quota_five");

    const auto rowId = [](int64_t pid) {
        return "home.quota.row." + std::to_string(pid);
    };
    const auto has = [&](const std::string& id) { return ui.find(id) != nullptr; };

    DOCTEST_CHECK(has(rowId(ids.unconfigured) + ".config.bg"));
    DOCTEST_CHECK(!has(rowId(ids.unconfigured) + ".bar.five_hour"));
    DOCTEST_CHECK(!has(rowId(ids.unconfigured) + ".error"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.unconfigured) + ".status") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.unconfigured) + ".status")->text,
                     std::string("未配置余量查询"));

    DOCTEST_CHECK(has(rowId(ids.incomplete) + ".config.bg"));
    DOCTEST_CHECK(!has(rowId(ids.incomplete) + ".bar.five_hour"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.incomplete) + ".error") != nullptr);
    DOCTEST_CHECK(ui.find(rowId(ids.incomplete) + ".error")->text.find("配置不完整") !=
                  std::string::npos);

    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.five_hour.track"));
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.weekly.track"));
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.monthly.track"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".config"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".error"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".stale"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.normal) + ".bar.five_hour.pct") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.normal) + ".bar.five_hour.pct")->text,
                     std::string("62%"));
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.normal) + ".bar.weekly.pct")->text,
                     std::string("91%"));
    // 只有带 resetsAt 的窗口才画倒计时节点；不得为空的画一个「--」冒充
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.monthly.reset"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".bar.five_hour.reset"));

    DOCTEST_CHECK(has(rowId(ids.failedStale) + ".bar.five_hour.track"));
    DOCTEST_CHECK(has(rowId(ids.failedStale) + ".stale"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.failedStale) + ".error") != nullptr);
    DOCTEST_CHECK(ui.find(rowId(ids.failedStale) + ".error")->text.find("密钥无效") !=
                  std::string::npos);
    DOCTEST_CHECK(ui.find(rowId(ids.failedStale) + ".error")->text.find("可能过期") ==
                  std::string::npos);   // 「可能过期」是角标，不是错误文案

    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".bar.five_hour"));
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".bar.weekly"));
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".stale"));
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".config"));
    DOCTEST_CHECK(has(rowId(ids.failedNever) + ".error"));
    DOCTEST_CHECK(ui.find(rowId(ids.failedNever) + ".error")->text.find("密钥无效") !=
                  std::string::npos);

    DOCTEST_CHECK(!has(rowId(ids.pending) + ".bar.five_hour"));
    DOCTEST_CHECK(!has(rowId(ids.pending) + ".error"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.pending) + ".status") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.pending) + ".status")->text,
                     std::string("等待首次查询"));

    // 对照组：没有它，上面的「不存在」断言可能只是因为什么都没渲染而恒真
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.five_hour.track"));
    mod->onStop();
    return true;
})

REGISTER_TEST(home_quota_balance_success_not_misread,
              "home quota: Balance 成功 → hasEverSucceeded=true 且渲染余额行", {
    test_support::TestEnv env(kDbPrefix, "balance", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    SeedIds ids;
    seedSevenRows(sq, ids);

    cq::QuotaStore store(sq);
    auto r = store.loadResult(ids.balance);
    DOCTEST_REQUIRE_MESSAGE(r.isOk(), r.errorMessage());
    DOCTEST_CHECK(r.value().windows.empty());          // 前提：确实没有窗口
    DOCTEST_CHECK(r.value().hasEverSucceeded);        // 判据是 kind 列不是 windows
    DOCTEST_CHECK(!r.value().hasError());

    auto rows = cq::listQuotaHomeRows(sq);
    DOCTEST_REQUIRE_MESSAGE(rows.isOk(), rows.errorMessage());
    const cq::QuotaHomeRow* found = nullptr;
    for (const auto& row : rows.value()) {
        if (row.profileId == ids.balance) found = &row;
    }
    DOCTEST_REQUIRE(found != nullptr);
    DOCTEST_CHECK(found->hasEverSucceeded);
    DOCTEST_CHECK_EQ(static_cast<int>(found->state),
                     static_cast<int>(cq::QuotaCardState::Normal));
    DOCTEST_CHECK_EQ(static_cast<int>(found->kind),
                     static_cast<int>(cq::QuotaHomeKind::Balance));

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    composeFrame(*mod, ui, screen, "home_quota_balance");
    const std::string rowId = "home.quota.row." + std::to_string(ids.balance);
    DOCTEST_CHECK(ui.find(rowId + ".balance") != nullptr);
    DOCTEST_REQUIRE(ui.find(rowId + ".balance.amount") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId + ".balance.amount")->text,
                     std::string("余额 12.34 CNY"));
    DOCTEST_CHECK(ui.find(rowId + ".bar.five_hour") == nullptr);
    mod->onStop();
    return true;
})

// 阈值语义：≥90 红 / ≥70 橙 / 其余绿（边界值含在本侧）
REGISTER_TEST(home_quota_tint_boundaries,
              "home quota: 色带阈值边界 89.9 / 90.0 / 70.0（纯函数 + 节点色）", {
    using T = hc::Tint;
    DOCTEST_CHECK_EQ(static_cast<int>(hc::tintForUsedPercent(89.9)),
                     static_cast<int>(T::Orange));
    DOCTEST_CHECK_EQ(static_cast<int>(hc::tintForUsedPercent(90.0)),
                     static_cast<int>(T::Red));
    DOCTEST_CHECK_EQ(static_cast<int>(hc::tintForUsedPercent(69.9)),
                     static_cast<int>(T::Green));
    DOCTEST_CHECK_EQ(static_cast<int>(hc::tintForUsedPercent(70.0)),
                     static_cast<int>(T::Orange));
    DOCTEST_CHECK_EQ(static_cast<int>(hc::tintForUsedPercent(0.0)),
                     static_cast<int>(T::Green));
    DOCTEST_CHECK_EQ(static_cast<int>(hc::tintForUsedPercent(100.0)),
                     static_cast<int>(T::Red));

    test_support::TestEnv env(kDbPrefix, "tint", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    const int64_t pid = insertProfile(sq, "色带行");
    cq::QuotaStore store(sq);
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(pid, "opencode_go")).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        pid,
                        tieredOk({makeWindow("five_hour", 62.0, ""),
                                  makeWindow("weekly", 91.0, "")}),
                        1000)
                        .isOk());

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    composeFrame(*mod, ui, screen, "home_quota_tint");

    const auto& t = core::theme::current();
    const std::string base = "home.quota.row." + std::to_string(pid) + ".bar.";
    const auto* greenFill = ui.find(base + "five_hour.fill");
    const auto* redFill = ui.find(base + "weekly.fill");
    DOCTEST_REQUIRE(greenFill != nullptr);
    DOCTEST_REQUIRE(redFill != nullptr);
    DOCTEST_CHECK_MESSAGE(
        colorsNear(greenFill->color, t.colors.success),
        "62% 的条应**就是**主题 success 绿");
    DOCTEST_CHECK_MESSAGE(
        colorsNear(redFill->color, t.colors.error),
        "91% 的条应**就是**主题 error 红");
    // 恒真陷阱：只断言「各自等于某个 token」不够——两个 token 取反了也各自相等，
    // 必须再断言两条确实不同色
    DOCTEST_CHECK_MESSAGE(
        colorsDiffer(greenFill->color, redFill->color),
        "62% 与 91% 必须是两种不同颜色（否则色带没生效）");
    mod->onStop();
    return true;
})

REGISTER_TEST(home_quota_refresh_button_wakes_refresher,
              "home quota: [刷新] 写请求信号，且只消费一次（不是同步查询）", {
    test_support::TestEnv env(kDbPrefix, "refresh", true);
    DOCTEST_REQUIRE(env.ready);
    insertProfile(env.db->db(), "待刷新的 profile");

    // 请求位是进程级跨用例存活的；不复位的话后面那条断言测的是上一次用例的残留
    cq::quotaRefresherResetTestSeams();
    DOCTEST_CHECK(!cq::consumeQuotaImmediateRefreshRequest());

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    composeFrame(*mod, ui, screen, "home_quota_refresh_btn");

    DOCTEST_CHECK(ui.find("home.quota.refresh") != nullptr);
    DOCTEST_CHECK(!cq::consumeQuotaImmediateRefreshRequest());  // 点之前没人请求
    DOCTEST_CHECK_MESSAGE(clickById(ui, "home.quota.refresh.bg"),
                          "home.quota.refresh.bg 上必须有 onClick");
    DOCTEST_CHECK(cq::consumeQuotaImmediateRefreshRequest());
    // 请求位消费一次即清：连点两下只触发一轮
    DOCTEST_CHECK(!cq::consumeQuotaImmediateRefreshRequest());
    mod->onStop();
    cq::quotaRefresherResetTestSeams();
    return true;
})

REGISTER_TEST(home_quota_config_button_opens_g6_dialog,
              "home quota: 「去配置」经 Router 负载打开 Claude 页的余量弹窗", {
    ::core::Router router;
    test_support::ScopedAppRouter scoped(&router);

    test_support::TestEnv env(kDbPrefix, "cfgbtn", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    SeedIds ids;
    seedSevenRows(sq, ids);

    // ClaudeProxy 必须在点按钮之前注册：home 的余量卡门控走
    // router->isRegistered(ClaudeProxy)，只注册 Home 时整张卡按模块插拔语义不画，
    // 按钮根本不存在；且 navigateTo 对未注册页会回落 Home，负载挂不上 Claude 页。
    module::claude::ClaudeService svc(sq);
    module::claude::ClaudeService::setGlobal(&svc);
    std::unique_ptr<::core::Module> home(create_home_module());
    std::unique_ptr<::core::Module> claude(create_claude_module());
    router.registerModule(home.get());
    router.registerModule(claude.get());
    home->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui hui;
    composeFrame(*home, hui, screen, "home_quota_cfgbtn");

    const std::string btnId =
        "home.quota.row." + std::to_string(ids.unconfigured) + ".config.bg";
    DOCTEST_CHECK_MESSAGE(clickById(hui, btnId),
                          "未配置行的「去配置」按钮必须可点（存在且带 onClick）");
    const std::string payload =
        router.consumePendingPayload(::core::PageId::ClaudeProxy);
    DOCTEST_CHECK_EQ(payload, "action:quota_config:" +
                                  std::to_string(ids.unconfigured));
    home->onStop();

    // 刻意不调 claude->onStart()：它会起真后台刷新线程，单测没必要依赖线程时序；
    // 弹窗只依赖 ClaudeService::global() + state_.quota，与 onStart 无关。
    router.navigateTo(::core::PageId::ClaudeProxy, payload);
    eui::Screen cScreen;
    cScreen.width = 1600.0f;
    cScreen.height = 1400.0f;
    eui::Ui cui;
    composeFrame(*claude, cui, cScreen, "claude_quota_from_home");
    const auto dialogVisible = [](eui::Ui& u) {
        const auto* panel = u.find("claude.dialog.quota.panel");
        return panel != nullptr && panel->opacity > 0.5f;
    };
    DOCTEST_CHECK_MESSAGE(dialogVisible(cui),
                          "首页的「去配置」必须真的**打开**余量配置弹窗");
    // 负载一次性：consumeQuotaConfigPayload 取走即清，否则每帧都会重开
    DOCTEST_REQUIRE_MESSAGE(
        clickById(cui, "claude.dialog.quota.cancel.bg"),
        "弹窗必须有可点的「取消」（否则关不掉，一次性语义无从验证）");
    eui::Ui cui2;
    composeFrame(*claude, cui2, cScreen, "claude_quota_from_home2");
    DOCTEST_CHECK_MESSAGE(!dialogVisible(cui2), "点「取消」后弹窗应关闭");
    eui::Ui cui3;
    composeFrame(*claude, cui3, cScreen, "claude_quota_from_home3");
    DOCTEST_CHECK_MESSAGE(!dialogVisible(cui3),
                          "负载被重复消费了：弹窗自己又弹回来了");
    module::claude::ClaudeService::setGlobal(nullptr);
    return true;
})

REGISTER_TEST(home_quota_row_order_stable,
              "home quota: 行序按 profile_id 升序，刷新前后不跳", {
    test_support::TestEnv env(kDbPrefix, "order", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    // 「名称顺序」故意与「id 顺序」相反：按名字排序的实现会在这里露馅
    const int64_t z = insertProfile(sq, "AAA");
    const int64_t m = insertProfile(sq, "MMM");
    const int64_t a = insertProfile(sq, "ZZZ");
    cq::QuotaStore store(sq);
    for (int64_t pid : {z, m, a}) {
        DOCTEST_REQUIRE(store.saveConfig(makeConfig(pid, "opencode_go")).isOk());
    }
    DOCTEST_REQUIRE(store.applyFetchResult(z, tieredOk({makeWindow("weekly", 10.0, "")}), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(m, tieredOk({makeWindow("weekly", 20.0, "")}), 1000).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(a, tieredOk({makeWindow("weekly", 30.0, "")}), 1000).isOk());

    auto first = cq::listQuotaHomeRows(sq);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_REQUIRE(first.value().size() >= 3);
    DOCTEST_CHECK_EQ(first.value()[0].profileId, z);
    DOCTEST_CHECK_EQ(first.value()[1].profileId, m);
    DOCTEST_CHECK_EQ(first.value()[2].profileId, a);
    DOCTEST_CHECK_EQ(first.value()[0].profileName, std::string("AAA"));

    // 「刷新」= 改结果行（百分比变了），行序必须不变
    DOCTEST_REQUIRE(store.applyFetchResult(z, tieredOk({makeWindow("weekly", 77.0, "")}), 2000).isOk());
    auto second = cq::listQuotaHomeRows(sq);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_REQUIRE(second.value().size() == first.value().size());
    for (size_t i = 0; i < second.value().size(); ++i) {
        DOCTEST_CHECK_EQ(second.value()[i].profileId, first.value()[i].profileId);
    }
    DOCTEST_CHECK_EQ(second.value()[0].windows[0].usedPercent, 77.0);
    return true;
})

REGISTER_TEST(home_quota_formats,
              "home quota: 百分比 / 余额 / 倒计时文案（手算对照）", {
    DOCTEST_CHECK_EQ(hc::formatQuotaPercent(0.0), std::string("0.0%"));
    DOCTEST_CHECK_EQ(hc::formatQuotaPercent(9.94), std::string("9.9%"));
    DOCTEST_CHECK_EQ(hc::formatQuotaPercent(10.0), std::string("10%"));
    DOCTEST_CHECK_EQ(hc::formatQuotaPercent(62.0), std::string("62%"));
    DOCTEST_CHECK_EQ(hc::formatQuotaPercent(100.0), std::string("100%"));

    DOCTEST_CHECK_EQ(hc::formatQuotaBalance(12.34, "CNY"),
                     std::string("余额 12.34 CNY"));
    DOCTEST_CHECK_EQ(hc::formatQuotaBalance(0.0, ""), std::string("余额 0.00"));

    // 倒计时用 epoch 锚点，直接钉住解析器的日期换算：days_from_civil 差一天会在这里露馅
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("1970-01-01T00:00:01", 0),
        std::string("即将重置"));            // 差 1 秒
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("1970-01-02T00:00:00", 0),
        std::string("1 天后重置"));           // 差 86400 秒
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("1970-01-01T01:00:00", 0),
        std::string("1 小时后重置"));         // 差 3600 秒
    const int64_t now = 1767225600;  // = 2026-01-01T00:00:00Z，下面的期望按秒差手算
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("2026-01-01T00:00:40", now),
        std::string("即将重置"));            // 差 40 秒
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("2026-01-01T00:30:00", now),
        std::string("30 分钟后重置"));        // 差 30 分
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("2026-01-01T05:00:00", now),
        std::string("5 小时后重置"));         // 差 5 小时
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("2026-01-06T00:00:00", now),
        std::string("5 天后重置"));           // 差 5 天
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("2025-12-31T23:59:59", now),
        std::string("已过期"));
    // 解析不出来 → 空串（调用方据此不画节点），不得返回「--」冒充
    DOCTEST_CHECK_EQ(hc::formatQuotaResetCountdown("", now), std::string());
    DOCTEST_CHECK_EQ(hc::formatQuotaResetCountdown("明天", now), std::string());
    DOCTEST_CHECK_EQ(hc::formatQuotaResetCountdown("2026-01-01", now), std::string());
    return true;
})

REGISTER_TEST(home_quota_card_metrics_overflow,
              "home quota: 卡高随行数自适应，超出上限出滚动（overflows）", {
    std::vector<cq::QuotaHomeRow> rows;
    cq::QuotaHomeRow one;
    one.profileId = 1;
    one.state = cq::QuotaCardState::FailedNever;
    one.lastError = "密钥无效，请换一把 API Key";
    one.hasError = true;
    rows.push_back(one);
    const hc::QuotaCardMetrics m1 = hc::computeQuotaCardMetrics(rows, 1520.0f);
    DOCTEST_CHECK(!m1.overflows);
    DOCTEST_CHECK(m1.bodyHeight > 0.0f);
    const float expected =
        16.0f * 2.0f + 28.0f + 10.0f + hc::quotaRowHeight(one);
    DOCTEST_CHECK(std::fabs(m1.cardHeight - expected) < 0.01f);

    // 20 行 → 内容远超 kQuotaCardMaxBodyH → 视口封顶 + overflows
    for (int i = 2; i <= 20; ++i) {
        cq::QuotaHomeRow r = one;
        r.profileId = i;
        rows.push_back(r);
    }
    const hc::QuotaCardMetrics m20 = hc::computeQuotaCardMetrics(rows, 1520.0f);
    DOCTEST_CHECK(m20.overflows);
    DOCTEST_CHECK_EQ(m20.bodyHeight, hc::kQuotaCardMaxBodyH);
    // 40 行与 20 行同高：少了这条就只测到「20 行时恰好触顶」，测不出封顶是否生效
    std::vector<cq::QuotaHomeRow> many = rows;
    for (int i = 21; i <= 40; ++i) {
        cq::QuotaHomeRow r = one;
        r.profileId = i;
        many.push_back(r);
    }
    const hc::QuotaCardMetrics m40 = hc::computeQuotaCardMetrics(many, 1520.0f);
    DOCTEST_CHECK_EQ(m40.cardHeight, m20.cardHeight);

    const hc::QuotaCardMetrics m0 = hc::computeQuotaCardMetrics({}, 1520.0f);
    DOCTEST_CHECK(!m0.overflows);
    DOCTEST_CHECK(m0.cardHeight > 0.0f);

    // 预算小于内容时视口必须被压到预算值：首页整页无外层滚动，卡片不自己封顶
    // 就会把下方图表与按钮顶出可视区
    const hc::QuotaCardMetrics capped =
        hc::computeQuotaCardMetrics(rows, 1520.0f, 40.0f);
    DOCTEST_CHECK_EQ(capped.bodyHeight, 40.0f);
    DOCTEST_CHECK(capped.overflows);
    DOCTEST_CHECK(capped.cardHeight < m20.cardHeight);
    // 负数预算不产生负高度（NaN / 负尺寸会让 EUI 布局炸）
    const hc::QuotaCardMetrics negative =
        hc::computeQuotaCardMetrics(rows, 1520.0f, -50.0f);
    DOCTEST_CHECK(negative.bodyHeight >= 0.0f);
    DOCTEST_CHECK(negative.cardHeight > 0.0f);
    return true;
})

// 从没成功过的行即使带着窗口数据也绝不画条（假 0% 红线）。
// 必须直接调绘制函数喂危险入参、不能走库造种子：kind 列为空时 resultRowFrom
// 根本不解码 windows_json，「有窗口 + 从未成功」这个形态在库里造不出来。
// 两行除 state 外完全一致，所以条的有无只由 showsQuotaBar 解释。
REGISTER_TEST(home_quota_never_succeeded_never_draws_bar_even_with_windows,
              "home quota: 从未成功（哪怕行里带窗口数据）绝不画进度条", {
    auto rowWith = [](cq::QuotaCardState state, int64_t pid) {
        cq::QuotaHomeRow r;
        r.profileId = pid;
        r.profileName = "profile-" + std::to_string(pid);
        r.providerId = "opencode_go";
        r.providerDisplayName = "OpenCode Go";
        r.state = state;
        r.hasEverSucceeded = (state == cq::QuotaCardState::Normal ||
                              state == cq::QuotaCardState::FailedStale);
        r.hasError = (state == cq::QuotaCardState::FailedNever ||
                      state == cq::QuotaCardState::FailedStale);
        r.lastErrorKind = r.hasError ? "auth" : "none";
        r.lastError = r.hasError
                          ? std::string("密钥无效，请换一把 API Key：HTTP 401")
                          : std::string();
        cq::QuotaHomeWindow w1;
        w1.key = "five_hour"; w1.label = "五小时"; w1.usedPercent = 62.0;
        cq::QuotaHomeWindow w2;
        w2.key = "weekly"; w2.label = "本周"; w2.usedPercent = 88.0;
        r.windows = {w1, w2};  // 两行窗口数据完全一样，只有 state 不同：改这里等于废掉本用例
        return r;
    };

    const auto& t = core::theme::current();
    hc::Hooks hooks;
    hooks.refreshAll = [] {};
    hooks.openConfig = [](int64_t) {};

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    eui::Ui ui;
    ui.begin("home_quota_fakezero");
    hc::drawQuotaCard(ui, t, 1520.0f,
                      {rowWith(cq::QuotaCardState::FailedNever, 1),
                       rowWith(cq::QuotaCardState::Normal, 2)},
                      hooks, hc::kQuotaCardMaxBodyH);
    ui.end();

    DOCTEST_CHECK_MESSAGE(ui.find("home.quota.row.1.bar.five_hour") == nullptr,
                          "假 0% 红线破了：从未成功的行画出了五小时进度条");
    DOCTEST_CHECK_MESSAGE(ui.find("home.quota.row.1.bar.weekly") == nullptr,
                          "假 0% 红线破了：从未成功的行画出了本周进度条");
    DOCTEST_CHECK(ui.find("home.quota.row.1.error") != nullptr);
    DOCTEST_CHECK(ui.find("home.quota.row.1.config") == nullptr);
    // 对照组：去掉它，上面两条就只是因为「什么都没渲染」而恒真
    DOCTEST_CHECK_MESSAGE(
        ui.find("home.quota.row.2.bar.five_hour.track") != nullptr &&
            ui.find("home.quota.row.2.bar.weekly.track") != nullptr,
        "对照组：Normal 行必须画出两条（否则前两条断言是恒真的）");
    // 倒计时只在 resetsAt 非空时画（入参无 resetsAt → 不画「--」）
    DOCTEST_CHECK(ui.find("home.quota.row.2.bar.five_hour.reset") == nullptr);
    return true;
})

// 矮窗下余量卡仍要画出来，且不挤掉既有内容。
// 720 刻意低于一切真实下限（设计最小窗 1330×845，本机有效 805），目的是守住深档；
// 不可"修正"到 845 —— 那会让这条断言变成永不触发的废条。
// 首页整页挂在 scrollView 上，装不下时由滚动条吸收，所以矮窗下仍然该画卡，
// 不得以「会顶出可视区」为由整张缺席。
REGISTER_TEST(home_quota_card_visible_at_design_minimum_window,
              "home quota: 矮窗 720（守护深档；真实最小窗 1330×845/805 见探针组 K）"
              "仍画出余量卡，且既有内容不丢", {
    test_support::TestEnv env(kDbPrefix, "shortwin", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    const int64_t pid = insertProfile(sq, "高窗-正常");
    cq::QuotaStore store(sq);
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(pid, "opencode_go")).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        pid, tieredOk({makeWindow("five_hour", 30.0, "")}), 1000)
                        .isOk());

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();

    // ① 高窗（1000）：装得下 → 画
    eui::Screen tall;
    tall.width = 1600.0f;
    tall.height = 1000.0f;
    eui::Ui tallUi;
    composeFrame(*mod, tallUi, tall, "home_quota_tall");
    DOCTEST_CHECK_MESSAGE(tallUi.find("home.quota") != nullptr,
                          "1000 高（默认窗）必须画出余量卡");

    // ② 矮窗 720：也画（整页可滚，卡不再两面落空）
    eui::Screen shortScreen;
    shortScreen.width = 1200.0f;
    shortScreen.height = 720.0f;
    eui::Ui shortUi;
    composeFrame(*mod, shortUi, shortScreen, "home_quota_short");
    // 恒真陷阱：不 layout 的话所有 frame 都是 0，下面 ④ 会退化成「0 <= 0.5」永远成立
    shortUi.layout(shortScreen.width, shortScreen.height);
    DOCTEST_CHECK_MESSAGE(shortUi.find("home.quota") != nullptr,
                          "720 高（守护深档）必须画出余量卡");
    DOCTEST_CHECK(shortUi.find("home.stats") != nullptr);
    DOCTEST_CHECK(shortUi.find("home.actions") != nullptr);
    DOCTEST_CHECK(shortUi.find("home.api.chart") != nullptr);
    // ④ 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
    const auto* acts = shortUi.find("home.actions");
    const auto* root = shortUi.find("home.root");
    DOCTEST_REQUIRE(acts != nullptr);
    DOCTEST_REQUIRE(root != nullptr);
    DOCTEST_CHECK_MESSAGE(
        acts->frame.y + acts->frame.height <=
            root->frame.y + root->frame.height + 0.5f,
        "快捷按钮行底 " +
            std::to_string(acts->frame.y + acts->frame.height) +
            " 溢出滚动内容底 " +
            std::to_string(root->frame.y + root->frame.height));
    mod->onStop();
    return true;
})

REGISTER_TEST(home_quota_error_text_never_empty,
              "home quota: 有错时 last_error 恒非空（旧行读侧也兜底）", {
    test_support::TestEnv env(kDbPrefix, "errtext", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    cq::QuotaStore store(sq);

    const int64_t pid = insertProfile(sq, "空文案行");
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(pid, "deepseek")).isOk());
    // 写侧兜底：provider 传来空 errorMessage，落库时仍必须补成可显示的话
    DOCTEST_REQUIRE(store.applyFetchResult(
                        pid, fail(cq::QuotaError::Entitlement, ""), 1000)
                        .isOk());
    auto row = store.loadResult(pid);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(!row.value().lastError.empty());
    DOCTEST_CHECK(row.value().lastError.find("套餐") != std::string::npos);

    // 读侧兜底：模拟兜底上线前落下的旧行（直接把列改空）
    DOCTEST_REQUIRE(sq.exec(
        "UPDATE profile_quota_result SET last_error = '' WHERE profile_id = " +
        std::to_string(pid)).isOk());
    auto rows = cq::listQuotaHomeRows(sq);
    DOCTEST_REQUIRE(rows.isOk());
    const cq::QuotaHomeRow* found = nullptr;
    for (const auto& r : rows.value()) {
        if (r.profileId == pid) found = &r;
    }
    DOCTEST_REQUIRE(found != nullptr);
    DOCTEST_CHECK(found->hasError);
    DOCTEST_CHECK_MESSAGE(!found->lastError.empty(),
                          "读侧兜底失效：旧行会画出空白错误文案");
    DOCTEST_CHECK(found->lastError.find("套餐") != std::string::npos);
    return true;
})

REGISTER_TEST(home_quota_manual_refresh_due_rules,
              "home quota: 手动刷新绕 interval 与 auto_refresh，但不放过 enabled=0", {
    cq::QuotaConfigRow cfg;
    cfg.profileId = 1;
    cfg.providerId = "opencode_go";
    cfg.enabled = true;
    cfg.autoRefresh = true;
    cfg.intervalMinutes = 15;

    // 刚查过（未到 interval）→ 自动不放行、手动必须放行
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(cfg, 1000, 1000, false));
    DOCTEST_CHECK(cq::QuotaRefresher::isDue(cfg, 1000, 1000, true));

    // autoRefresh=0（「手动刷新档」）→ 自动不放行、手动放行
    cq::QuotaConfigRow manual = cfg;
    manual.autoRefresh = false;
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(manual, 100000, 0, false));
    DOCTEST_CHECK(cq::QuotaRefresher::isDue(manual, 100000, 0, true));

    // 总开关关着 → 手动也不该发请求
    cq::QuotaConfigRow off = cfg;
    off.enabled = false;
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(off, 100000, 0, true));

    // provider_id 空 → 手动也不发
    cq::QuotaConfigRow none = cfg;
    none.providerId.clear();
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(none, 100000, 0, true));
    return true;
})

REGISTER_TEST(home_quota_budget_threshold_is_pinned,
              "home_quota: 高度预算阈值钉死（矮窗压缩图表给余量卡让位）", {
    const float pad = 40.0f;   // spacingXXLarge，按 fontScale=1
    const float gap = 20.0f;   // spacingLarge×1.25，按 fontScale=1
    const float statH = 144.0f;

    // 宽窗：图表不压缩，余量卡拿到上限
    const auto tall = module::home::quota_card::computeHomeQuotaBudget(1000.0f, statH, pad, gap);
    DOCTEST_CHECK(tall.drawsQuotaCard);
    DOCTEST_CHECK(!tall.pageStillOverflows);
    DOCTEST_CHECK_EQ(tall.chartHeight, module::home::quota_card::kHomeChartH);
    // quotaBodyHeight 语义 = 可用空间下限，不是最终行内容高；最终高由
    // computeQuotaCardMetrics 取 min(内容高, 可用空间) 再封顶 kQuotaCardMaxBodyH。
    // 断言的是「够画」，不是「已达上限」。
    DOCTEST_CHECK(tall.quotaBodyHeight >= module::home::quota_card::kQuotaCardMinBodyH);
    const auto huge = module::home::quota_card::computeHomeQuotaBudget(2000.0f, statH, pad, gap);
    DOCTEST_CHECK(huge.quotaBodyHeight >= module::home::quota_card::kQuotaCardMaxBodyH);

    // 805pt = 本机典型客户区高（工作区 833 − 标题栏 28）
    const auto macDefault = module::home::quota_card::computeHomeQuotaBudget(805.0f, statH, pad, gap);
    DOCTEST_CHECK(macDefault.drawsQuotaCard);
    DOCTEST_CHECK(!macDefault.pageStillOverflows);
    DOCTEST_CHECK(macDefault.quotaBodyHeight >= module::home::quota_card::kQuotaCardMinBodyH);
    DOCTEST_CHECK(macDefault.chartHeight < module::home::quota_card::kHomeChartH);
    DOCTEST_CHECK(macDefault.chartHeight >= module::home::quota_card::kHomeChartMinH);

    // 滚动模式阈值 = 不含余量卡行的整页高度（fixed + kHomeChartH）：
    //   屏高 ≥ 阈值 → 装得下 → 压缩图表给余量卡让位
    //   屏高 <  阈值 → 装不下 → 滚动模式：图表一分不压 + 余量卡给到上限
    // 「装不下」不是不画卡的理由，反而正是该画卡的信号：整页挂在 scrollView 上，
    // 只有这时用户才需要滚着看，而滚的前提是得有东西可看。
    // 不可把「压缩让位」扩到装不下的档，也不可让装不下的档退回整张不画。
    const float scrollModeThreshold =
        /* fixed = */ pad * 2.0f +
        module::home::quota_card::kHomeWelcomeH + gap + statH + gap +
        module::home::quota_card::kQuotaCardChromeH + gap + gap +
        module::home::quota_card::kHomeActionsH +
        module::home::quota_card::kHomeChartH;

    // 阈值上下各差一像素：阈值必须是真边界，否则下面两条恒成立
    const auto justFits = module::home::quota_card::computeHomeQuotaBudget(
        scrollModeThreshold, statH, pad, gap);
    const auto justScrolls = module::home::quota_card::computeHomeQuotaBudget(
        scrollModeThreshold - 1.0f, statH, pad, gap);
    DOCTEST_CHECK(justFits.chartHeight < module::home::quota_card::kHomeChartH);
    DOCTEST_CHECK_EQ(justScrolls.chartHeight,
                     module::home::quota_card::kHomeChartH);
    DOCTEST_CHECK(justScrolls.drawsQuotaCard);
    DOCTEST_CHECK(!justScrolls.pageStillOverflows);
    DOCTEST_CHECK(justScrolls.quotaBodyHeight >=
                  module::home::quota_card::kQuotaCardMinBodyH);

    // 逐像素扫 600..阈值-1：滚动模式那一段每一屏都画得出余量卡
    bool allDrawBelow = true;
    for (float h = 600.0f; h <= scrollModeThreshold - 1.0f; h += 1.0f) {
        const auto b = module::home::quota_card::computeHomeQuotaBudget(
            h, statH, pad, gap);
        if (!b.drawsQuotaCard || b.pageStillOverflows ||
            b.chartHeight != module::home::quota_card::kHomeChartH) {
            allDrawBelow = false;
            break;
        }
    }
    DOCTEST_CHECK_MESSAGE(allDrawBelow,
                          "600.." + std::to_string(scrollModeThreshold - 1.0f) +
                              " 区间内存在「不画余量卡」或「图表被压」的屏高 —— "
                              "滚动模式下不该再有两面落空");

    // 720 低于一切真实下限，走的必须是滚动模式而非「压不出就整张不画」
    const auto minWindow = module::home::quota_card::computeHomeQuotaBudget(720.0f, statH, pad, gap);
    DOCTEST_CHECK(minWindow.drawsQuotaCard);
    DOCTEST_CHECK(!minWindow.pageStillOverflows);
    DOCTEST_CHECK_EQ(minWindow.chartHeight, module::home::quota_card::kHomeChartH);
    DOCTEST_CHECK(minWindow.quotaBodyHeight >=
                  module::home::quota_card::kQuotaCardMinBodyH);
    return true;
})

// 守的是行为：数据变了，compose 必须在下一次重排里反映出来。刻意不依赖任何
// 缓存机制——每帧都查也该通过，加了缓存之后仍然必须通过。
// 五步各走一条不同的写入通道，使「脏标记失效」不能靠某一条特定路径侥幸成立：
//   ① 新 profile → 裸 INSERT claude_profiles（不经 QuotaStore）
//   ② 结果更新   → QuotaStore::applyFetchResult（后台刷新器那条路）
//   ③ 配置改动   → QuotaStore::saveConfig（provider 换人）
//   ④ 数据没变时重排多次，结果必须稳定（防缓存返回半截/撕裂的行）
//   ⑤ 数据删除   → 裸 DELETE，不经任何应用层写路径
REGISTER_TEST(home_quota_snapshot_reflects_db_changes,
              "home quota: 库变了 compose 必须反映（新 profile / 结果更新 / 配置改）", {
    test_support::TestEnv env(kDbPrefix, "dirty", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    cq::QuotaStore store(sq);

    // 首行：配好 + 成功过一次（Normal 态，有条可断言）
    const int64_t p1 = insertProfile(sq, "P1-初始");
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(p1, "opencode_go")).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        p1, tieredOk({makeWindow("five_hour", 30.0, "")}), 1000)
                        .isOk());

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    const auto rowId = [](int64_t pid) {
        return "home.quota.row." + std::to_string(pid);
    };
    // 重排一帧并返回该帧的树（每次都用**新** Ui：树是幂等重建的，同一个 Ui
    // 连着 begin/end 两帧会让 find 命中上一帧的残留，掩盖「没重查」）。
    auto frame = [&]() {
        auto ui = std::make_unique<eui::Ui>();
        composeFrame(*mod, *ui, screen, "home_quota_dirty");
        return ui;
    };

    {
        auto ui = frame();
        DOCTEST_REQUIRE(ui->find(rowId(p1) + ".bar.five_hour.pct") != nullptr);
        DOCTEST_CHECK_EQ(ui->find(rowId(p1) + ".bar.five_hour.pct")->text,
                         std::string("30%"));
        // 对照组：find 对不存在的 id 确实返回空，后面的「不存在」断言才有意义
        DOCTEST_CHECK(ui->find(rowId(999999)) == nullptr);
    }

    // ① 裸 INSERT claude_profiles，不经 QuotaStore / ProfileRepository —— 应用层
    // 完全不知情，脏标记若只认某个写路径的钩子，这里就会露馅
    const int64_t p2 = insertProfile(sq, "P2-新增");
    {
        auto ui = frame();
        DOCTEST_REQUIRE_MESSAGE(
            ui->find(rowId(p2)) != nullptr,
            "库里新增了 profile，compose 却没画出来 —— 脏标记漏了新增这条路");
        DOCTEST_REQUIRE(ui->find(rowId(p2) + ".status") != nullptr);
        DOCTEST_CHECK_EQ(ui->find(rowId(p2) + ".status")->text,
                         std::string("未配置余量查询"));
    }

    // ② 后台刷新器那条路
    DOCTEST_REQUIRE(store.applyFetchResult(
                        p1, tieredOk({makeWindow("five_hour", 77.0, "")}), 2000)
                        .isOk());
    {
        auto ui = frame();
        DOCTEST_REQUIRE(ui->find(rowId(p1) + ".bar.five_hour.pct") != nullptr);
        DOCTEST_CHECK_EQ(
            ui->find(rowId(p1) + ".bar.five_hour.pct")->text,
            std::string("77%"));
    }

    // ③ provider 换人。断言必须落在 .provider 节点的**文本**上，不能读内部字段 ——
    // 换 provider 会连带改 kind / 窗口，只看字段证明不了绘制层跟着换了。
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(p1, "deepseek")).isOk());
    {
        auto ui = frame();
        DOCTEST_REQUIRE(ui->find(rowId(p1) + ".provider") != nullptr);
        DOCTEST_CHECK_MESSAGE(
            ui->find(rowId(p1) + ".provider")->text != std::string("OpenCode Go"),
            "provider 配置已换成 deepseek，compose 仍显示旧供应商名 —— "
            "脏标记漏了 saveConfig 这条路");
    }

    // ④ 「重查多少次都得对」而非「不该重查」（后者是下一个用例）：缓存返回旧值、
    // 或只更新了一半的行（新 profile 有了但百分比还是旧的），这里都会红。
    {
        auto a = frame();
        auto b = frame();
        const std::string id1 = rowId(p1);
        DOCTEST_REQUIRE(a->find(id1 + ".bar.five_hour.pct") != nullptr);
        DOCTEST_REQUIRE(b->find(id1 + ".bar.five_hour.pct") != nullptr);
        DOCTEST_CHECK_EQ(a->find(id1 + ".bar.five_hour.pct")->text,
                         b->find(id1 + ".bar.five_hour.pct")->text);
        DOCTEST_CHECK_EQ(a->find(id1 + ".provider")->text,
                         b->find(id1 + ".provider")->text);
        DOCTEST_CHECK_EQ(a->find(rowId(p2) + ".status")->text,
                         b->find(rowId(p2) + ".status")->text);
    }

    // ⑤ 裸 DELETE config 行，不经任何应用层写路径 —— 写侧钩子方案在这里会露馅，
    // DB 层版本号方案不会。这是最容易漏的一类失效路径。
    DOCTEST_REQUIRE(sq.exec("DELETE FROM profile_quota_config WHERE profile_id = " +
                            std::to_string(p2))
                        .isOk());
    {
        auto ui = frame();
        // 行不该消失：删的只是 config 行，profile 还在（桥不变量）。
        // 所以判据是状态文案回到「未配置」，不是「行不见了」。
        DOCTEST_REQUIRE(ui->find(rowId(p2) + ".status") != nullptr);
        DOCTEST_CHECK_EQ(ui->find(rowId(p2) + ".status")->text,
                         std::string("未配置余量查询"));
    }
    mod->onStop();
    return true;
})

// 判据用真实调用计数，不用「compose 里有没有出现 listQuotaHomeRows」这种源码形态
// 断言：形态断言在函数被改名 / 换调用路径时会静默失效，计数是运行时事实。
REGISTER_TEST(home_quota_snapshot_not_requery_when_unchanged,
              "home quota: 数据没变时连续 compose 不应重复查库（缓存生效）", {
    test_support::TestEnv env(kDbPrefix, "cachehit", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    cq::QuotaStore store(sq);
    const int64_t p1 = insertProfile(sq, "P1-缓存");
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(p1, "opencode_go")).isOk());
    DOCTEST_REQUIRE(store.applyFetchResult(
                        p1, tieredOk({makeWindow("five_hour", 42.0, "")}), 1000)
                        .isOk());

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1400.0f;
    const std::string id1 = "home.quota.row." + std::to_string(p1);

    // 冷启动首帧必须出数据：缓存最容易犯的错就是首帧空
    {
        auto ui = std::make_unique<eui::Ui>();
        composeFrame(*mod, *ui, screen, "home_quota_cache_cold");
        DOCTEST_REQUIRE_MESSAGE(
            ui->find(id1 + ".bar.five_hour.pct") != nullptr,
            "冷启动首帧必须画出余量条（缓存若让首帧空掉，用户看到的是空卡）");
    }

    // 连续 20 帧，数据一动不动 → 真实查库次数不得增长
    const int64_t before = cq::quotaHomeRowsQueryCountForTest();
    for (int i = 0; i < 20; ++i) {
        auto ui = std::make_unique<eui::Ui>();
        composeFrame(*mod, *ui, screen, "home_quota_cache_idle");
    }
    const int64_t after = cq::quotaHomeRowsQueryCountForTest();
    DOCTEST_CHECK_EQ(after - before, static_cast<int64_t>(0));

    // 缓存期间内容仍正确（防「不重查」退化成「返回错的东西」）
    {
        auto ui = std::make_unique<eui::Ui>();
        composeFrame(*mod, *ui, screen, "home_quota_cache_verify");
        DOCTEST_REQUIRE(ui->find(id1 + ".bar.five_hour.pct") != nullptr);
        DOCTEST_CHECK_EQ(ui->find(id1 + ".bar.five_hour.pct")->text,
                         std::string("42%"));
    }

    // 数据一变 → 必须恰好多查一次（既不能漏，也不能每帧都查）
    DOCTEST_REQUIRE(store.applyFetchResult(
                        p1, tieredOk({makeWindow("five_hour", 55.0, "")}), 2000)
                        .isOk());
    const int64_t preChange = cq::quotaHomeRowsQueryCountForTest();
    {
        auto ui = std::make_unique<eui::Ui>();
        composeFrame(*mod, *ui, screen, "home_quota_cache_after");
        DOCTEST_REQUIRE(ui->find(id1 + ".bar.five_hour.pct") != nullptr);
        DOCTEST_CHECK_MESSAGE(
            ui->find(id1 + ".bar.five_hour.pct")->text == std::string("55%"),
            "数据变了但 compose 仍显示旧值 —— 缓存脏标记没被置位");
    }
    const int64_t postChange = cq::quotaHomeRowsQueryCountForTest();
    DOCTEST_CHECK_MESSAGE(postChange - preChange == 1,
                          "数据变了一帧内应恰好重查 1 次，实测 " +
                              std::to_string(postChange - preChange) +
                              " 次（0 = 脏标记漏了；>1 = 一帧里查了多次）");

    // 变化后再空转 20 帧 → 又一次都不查（脏标记消费后必须回到静止态）
    const int64_t settled = cq::quotaHomeRowsQueryCountForTest();
    for (int i = 0; i < 20; ++i) {
        auto ui = std::make_unique<eui::Ui>();
        composeFrame(*mod, *ui, screen, "home_quota_cache_settled");
    }
    DOCTEST_CHECK_EQ(cq::quotaHomeRowsQueryCountForTest() - settled,
                     static_cast<int64_t>(0));

    mod->onStop();
    return true;
})
