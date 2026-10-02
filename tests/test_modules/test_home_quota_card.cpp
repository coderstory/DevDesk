// tests/test_modules/test_home_quota_card.cpp
// G7：首页余量卡片 —— 五态 / 假 0% 红线 / 色带边界 / 手动刷新唤醒 / 去配置弹窗 /
// 行序稳定。
//
// 三条测试纪律在本文件的具体落法：
//   · **断言节点存在/不存在**，不拿「状态枚举 == X」当呈现证据（那只能证明
//     判据函数对了，证明不了绘制层照它画了）；枚举对照另开一条纯函数用例。
//   · **期望值是字面量**（列文本 "auth" / 颜色阈值 89.9 / 元素 id 字符串），
//     不用 fields()、kQuotaProviders 这类被测对象自己的输出来构造期望。
//   · 假 0% 红线那条**必带对照组**（Normal 行必须有条）：没有对照组的
//     「不存在」断言可能是因为根本没渲染任何东西而恒真。
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

// 造一个 provider 已选、结果 = 指定的行
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

// 成功一次（Tiered）
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

// 无头 compose 一帧（同 test_home / test_claude_ui 同款）
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

// 同色判定（>1/255 视为不同）：断言「条的颜色**就是**主题的某个 token」
// 要的是「相等」，不是「不相等」——用 colorsDiffer 写这种断言会恒红。
bool colorsNear(const eui::Color& a, const eui::Color& b) {
    constexpr float kEps = 0.004f;
    return std::fabs(a.r - b.r) <= kEps && std::fabs(a.g - b.g) <= kEps &&
           std::fabs(a.b - b.b) <= kEps;
}

bool colorsDiffer(const eui::Color& a, const eui::Color& b) {
    return !colorsNear(a, b);
}

// 七行种子（覆盖五态 + 余额型 + 等待首次），返回各行 profileId（按序）
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
// 函数里编译不过（-Wreturn-mismatch = -Werror）。抛异常由 test_main 逐用例
// catch，记成一条 Failed —— 与 test_support.h「构造失败即抛」同一口径。
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
    // 先成功一次（留下有效数据），再失败 → 「旧值 + 可能过期」
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
    // Balance 成功时 windows 恒空 —— 这正是「不能用 windows 空不空当判据」的原因
    must(store.applyFetchResult(ids.balance, balanceOk(12.34, "CNY"), now)
             .isOk(),
         "P6res");

    ids.pending = insertProfile(db, "P7-等待首次");
    must(store.saveConfig(makeConfig(ids.pending, "opencode_go")).isOk(),
         "P7cfg");
    // 无结果行 = 配好了但一次都没查过
}

}  // namespace

// ==================== ① 五态判据（纯函数，字面量真值表）====================
REGISTER_TEST(home_quota_state_decision_table,
              "home quota: 五态判据纯函数真值表（列文本字面量）", {
    using S = cq::QuotaCardState;
    // 1 未配置：provider_id 空（无论有无历史成功值 / 错误）
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("", "none", true)),
                     static_cast<int>(S::Unconfigured));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("", "auth", false)),
                     static_cast<int>(S::Unconfigured));
    // 2 正常：无错 + 有过成功值
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "none", true)),
                     static_cast<int>(S::Normal));
    // 3 等待首次：无错 + 从未成功
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "", false)),
                     static_cast<int>(S::Pending));
    // 4 失败有旧值：任何错误 + 有过成功值（含鉴权类与配置类）
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "auth", true)),
                     static_cast<int>(S::FailedStale));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "not_configured", true)),
                     static_cast<int>(S::FailedStale));
    // 5 配置不完整：NotConfigured + 从未成功
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("volcano", "not_configured", false)),
                     static_cast<int>(S::Incomplete));
    // 6 失败未成功：其余错误 + 从未成功（三类错误各一条）
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("deepseek", "auth", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "entitlement", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "shape", false)),
                     static_cast<int>(S::FailedNever));
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "transient", false)),
                     static_cast<int>(S::FailedNever));
    // 空串必须当「无错」：列默认值 '' 的含义是「这行从没写过」，
    // 若被当成 Transient 会把刚建好的行显示成「网络失败」
    DOCTEST_CHECK_EQ(static_cast<int>(cq::quotaCardState("opencode_go", "", true)),
                     static_cast<int>(S::Normal));
    return true;
})

// ==================== ② 「画不画条」判据（六态逐条）====================
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

// ==================== ③ 五态逐态的呈现（节点存在 / 不存在）====================
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

    // ---- 未配置：去配置按钮在、无条、无错误行 ----
    DOCTEST_CHECK(has(rowId(ids.unconfigured) + ".config.bg"));
    DOCTEST_CHECK(!has(rowId(ids.unconfigured) + ".bar.five_hour"));
    DOCTEST_CHECK(!has(rowId(ids.unconfigured) + ".error"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.unconfigured) + ".status") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.unconfigured) + ".status")->text,
                     std::string("未配置余量查询"));

    // ---- 配置不完整：去配置按钮在、**无条**、错误行是配置类文案 ----
    DOCTEST_CHECK(has(rowId(ids.incomplete) + ".config.bg"));
    DOCTEST_CHECK(!has(rowId(ids.incomplete) + ".bar.five_hour"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.incomplete) + ".error") != nullptr);
    DOCTEST_CHECK(ui.find(rowId(ids.incomplete) + ".error")->text.find("配置不完整") !=
                  std::string::npos);

    // ---- 正常：三个窗口条都在、无去配置、无错误行 ----
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.five_hour.track"));
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.weekly.track"));
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.monthly.track"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".config"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".error"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".stale"));
    // 百分比按窗口分别渲染（62 / 91 / 45）
    DOCTEST_REQUIRE(ui.find(rowId(ids.normal) + ".bar.five_hour.pct") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.normal) + ".bar.five_hour.pct")->text,
                     std::string("62%"));
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.normal) + ".bar.weekly.pct")->text,
                     std::string("91%"));
    // 有 resetsAt 的窗口才画倒计时节点；没有的不画（不画「--」冒充）
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.monthly.reset"));
    DOCTEST_CHECK(!has(rowId(ids.normal) + ".bar.five_hour.reset"));

    // ---- 失败（有过成功值）：旧条 + 「可能过期」角标 + 按 kind 选的错误文案 ----
    DOCTEST_CHECK(has(rowId(ids.failedStale) + ".bar.five_hour.track"));
    DOCTEST_CHECK(has(rowId(ids.failedStale) + ".stale"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.failedStale) + ".error") != nullptr);
    DOCTEST_CHECK(ui.find(rowId(ids.failedStale) + ".error")->text.find("密钥无效") !=
                  std::string::npos);
    DOCTEST_CHECK(ui.find(rowId(ids.failedStale) + ".error")->text.find("可能过期") ==
                  std::string::npos);   // 「可能过期」是角标，不是错误文案

    // ---- 失败（从未成功）：**只渲染错误行**，无条、无角标、无去配置 ----
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".bar.five_hour"));
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".bar.weekly"));
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".stale"));
    DOCTEST_CHECK(!has(rowId(ids.failedNever) + ".config"));
    DOCTEST_CHECK(has(rowId(ids.failedNever) + ".error"));
    DOCTEST_CHECK(ui.find(rowId(ids.failedNever) + ".error")->text.find("密钥无效") !=
                  std::string::npos);

    // ---- 等待首次：无条、无错误行、状态文案 ----
    DOCTEST_CHECK(!has(rowId(ids.pending) + ".bar.five_hour"));
    DOCTEST_CHECK(!has(rowId(ids.pending) + ".error"));
    DOCTEST_REQUIRE(ui.find(rowId(ids.pending) + ".status") != nullptr);
    DOCTEST_CHECK_EQ(ui.find(rowId(ids.pending) + ".status")->text,
                     std::string("等待首次查询"));

    // ---- 对照组（防「不存在」断言恒真）：正常行必须有条 ----
    DOCTEST_CHECK(has(rowId(ids.normal) + ".bar.five_hour.track"));
    mod->onStop();
    return true;
})

// ==================== ④ Balance 成功不被「windows 空」误判 ====================
REGISTER_TEST(home_quota_balance_success_not_misread,
              "home quota: Balance 成功 → hasEverSucceeded=true 且渲染余额行", {
    test_support::TestEnv env(kDbPrefix, "balance", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    SeedIds ids;
    seedSevenRows(sq, ids);

    // 数据侧：Balance 成功行的 windows 恒空、kind 列非空
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

    // 呈现侧：余额行在、周期条不在
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

// ==================== ⑤ 色带边界（≥90 红 / ≥70 橙 / 其余绿）====================
REGISTER_TEST(home_quota_tint_boundaries,
              "home quota: 色带阈值边界 89.9 / 90.0 / 70.0（纯函数 + 节点色）", {
    using T = hc::Tint;
    // 89.9 属橙档（≥70），绿/橙边界在 70.0，橙/红边界在 90.0
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

    // 节点色：三个窗口 62（绿）/ 91（红）/ 45（绿）分别落在不同色档
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
    // 色带确实生效：两条不是同一个颜色（只断言「各自等于某个 token」不够——
    // 两个 token 取反了也各自相等）
    DOCTEST_CHECK_MESSAGE(
        colorsDiffer(greenFill->color, redFill->color),
        "62% 与 91% 必须是两种不同颜色（否则色带没生效）");
    mod->onStop();
    return true;
})

// ==================== ⑥ [刷新] 按钮 = 唤醒后台刷新（G6 欠账通道）====================
REGISTER_TEST(home_quota_refresh_button_wakes_refresher,
              "home quota: [刷新] 写请求信号，且只消费一次（不是同步查询）", {
    test_support::TestEnv env(kDbPrefix, "refresh", true);
    DOCTEST_REQUIRE(env.ready);
    insertProfile(env.db->db(), "待刷新的 profile");

    // 前置：请求位必须先是干净的（否则这条断言测的是上一次残留）
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
    // 点完立刻有请求位（**不**等一轮，也**不**当场发 HTTP）
    DOCTEST_CHECK(cq::consumeQuotaImmediateRefreshRequest());
    // 请求位只消费一次：重复消费不会再触发一轮（否则连点两下刷两遍）
    DOCTEST_CHECK(!cq::consumeQuotaImmediateRefreshRequest());
    mod->onStop();
    cq::quotaRefresherResetTestSeams();
    return true;
})

// ==================== ⑦ 「去配置」按钮 → 打开 G6 的余量弹窗（跨模块全链）====================
REGISTER_TEST(home_quota_config_button_opens_g6_dialog,
              "home quota: 「去配置」经 Router 负载打开 Claude 页的余量弹窗", {
    ::core::Router router;
    test_support::ScopedAppRouter scoped(&router);

    test_support::TestEnv env(kDbPrefix, "cfgbtn", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    SeedIds ids;
    seedSevenRows(sq, ids);

    // 前半段：首页点「去配置」→ Router 上出现本页负载
    // ⚠ 必须把模块注册进 Router：home 的 `moduleRegistered(ClaudeProxy)` 走
    // `router->isRegistered()`，空 Router 会让整张余量卡**整张不画**（门控按
    // 模块插拔语义就是这样的），点不到按钮不是按钮的锅。
    module::claude::ClaudeService svc(sq);
    module::claude::ClaudeService::setGlobal(&svc);
    std::unique_ptr<::core::Module> home(create_home_module());
    std::unique_ptr<::core::Module> claude(create_claude_module());
    // ⚠ **两个都要在点按钮之前注册**：home 的余量卡门控走
    // `router->isRegistered(ClaudeProxy)`，只注册 Home 时整张卡按模块插拔
    // 语义不画（那是设计，不是 bug）。且 navigateTo 对未注册页会回落 Home，
    // 负载根本挂不上 Claude 页。
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

    // 后半段：把负载挂回 Claude 页 → compose 消费 → G6 的弹窗出现。
    // **不调 claude->onStart()**：onStart 会起真后台刷新线程（本仓种子 profile
    // 无凭据，虽不至于真发请求，但让单测依赖线程时序没有好处）；弹窗只依赖
    // ClaudeService::global() + state_.quota，与 onStart 无关。
    router.navigateTo(::core::PageId::ClaudeProxy, payload);
    eui::Screen cScreen;
    cScreen.width = 1600.0f;
    cScreen.height = 1400.0f;
    eui::Ui cui;
    composeFrame(*claude, cui, cScreen, "claude_quota_from_home");
    // ⚠ **判「弹窗开着」不能只看 find != nullptr**：EUI-NEO 的 dialog 组件
    // 关着时照样把整棵树建出来（只把 panel 的 opacity 设 0 并 disable），
    // 所以 `find(...) != nullptr` 在开与关两种情况下都成立 = 恒真断言。
    // 真正可判的量是 panel 的 opacity（dialog.h:89 visible = open ? 1 : 0）。
    const auto dialogVisible = [](eui::Ui& u) {
        const auto* panel = u.find("claude.dialog.quota.panel");
        return panel != nullptr && panel->opacity > 0.5f;
    };
    DOCTEST_CHECK_MESSAGE(dialogVisible(cui),
                          "首页的「去配置」必须真的**打开** G6 的余量配置弹窗");
    // 负载**一次性**：关掉弹窗后再 compose，弹窗不得自己再弹回来
    //（若 consumeQuotaConfigPayload 不是取走即清，每帧都会重开）。
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

// ==================== ⑧ 行序稳定（刷新不重排）====================
REGISTER_TEST(home_quota_row_order_stable,
              "home quota: 行序按 profile_id 升序，刷新前后不跳", {
    test_support::TestEnv env(kDbPrefix, "order", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    // 故意让「名称顺序」与「id 顺序」相反：靠名字排序的实现会在这里露馅
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

// ==================== ⑨ 纯函数：百分比 / 余额 / 倒计时 ====================
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

    // 倒计时基准：**epoch 锚点**直接钉住解析器的日期换算（days_from_civil
    // 差一天就会在这里露馅，而不是等到某天的倒计时默默差 24 小时）。
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("1970-01-01T00:00:01", 0),
        std::string("即将重置"));            // 差 1 秒
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("1970-01-02T00:00:00", 0),
        std::string("1 天后重置"));           // 差 86400 秒
    DOCTEST_CHECK_EQ(
        hc::formatQuotaResetCountdown("1970-01-01T01:00:00", 0),
        std::string("1 小时后重置"));         // 差 3600 秒
    // 真实日期基准（2026-01-01T00:00:00Z = 1767225600），期望按秒差手算
    const int64_t now = 1767225600;
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
    // 解析不出来 → 空串（调用方据此不画节点），绝不返回「--」冒充
    DOCTEST_CHECK_EQ(hc::formatQuotaResetCountdown("", now), std::string());
    DOCTEST_CHECK_EQ(hc::formatQuotaResetCountdown("明天", now), std::string());
    DOCTEST_CHECK_EQ(hc::formatQuotaResetCountdown("2026-01-01", now), std::string());
    return true;
})

// ==================== ⑩ 卡片几何：行多时滚动、不无限增高 ====================
REGISTER_TEST(home_quota_card_metrics_overflow,
              "home quota: 卡高随行数自适应，超出上限出滚动（overflows）", {
    std::vector<cq::QuotaHomeRow> rows;
    // 单行：标题 + 一条错误行
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

    // 20 行 → 内容远超 288 上限 → 视口封顶 + overflows
    for (int i = 2; i <= 20; ++i) {
        cq::QuotaHomeRow r = one;
        r.profileId = i;
        rows.push_back(r);
    }
    const hc::QuotaCardMetrics m20 = hc::computeQuotaCardMetrics(rows, 1520.0f);
    DOCTEST_CHECK(m20.overflows);
    DOCTEST_CHECK_EQ(m20.bodyHeight, hc::kQuotaCardMaxBodyH);
    // 40 行与 20 行同高 —— 封顶真的生效（否则卡高会随 profile 数线性长）
    std::vector<cq::QuotaHomeRow> many = rows;
    for (int i = 21; i <= 40; ++i) {
        cq::QuotaHomeRow r = one;
        r.profileId = i;
        many.push_back(r);
    }
    const hc::QuotaCardMetrics m40 = hc::computeQuotaCardMetrics(many, 1520.0f);
    DOCTEST_CHECK_EQ(m40.cardHeight, m20.cardHeight);

    // 空列表：卡高只剩外壳（不塌成 0）
    const hc::QuotaCardMetrics m0 = hc::computeQuotaCardMetrics({}, 1520.0f);
    DOCTEST_CHECK(!m0.overflows);
    DOCTEST_CHECK(m0.cardHeight > 0.0f);

    // 窗口剩余空间小于内容时：视口被压到 maxBodyHeight（首页整页无外层滚动，
    // 卡片必须自己封顶，否则会把下方图表与按钮顶出可视区）
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

// ==================== ⑭ 假 0% 红线（强化版：真能变红的那条）====================
// ⚠ 五态那条用例里「失败未成功行没有 .bar 节点」**单独看是空的**：那种行的
// windows 本来就空，绘制层就算无条件画条也会因为「没窗口可遍历」而不画——
// 它在「把画条判据改成恒真」的变异下仍然是绿的，等于没钉（实测过：真恒真）。
//
// 顺带查明：走库这条路也造不出「有窗口 + 从未成功」的形态——G5 的
// `resultRowFrom`（quota_store.cpp:113）在 kind 列为空时**根本不解码**
// windows_json。所以要钉住渲染层，就**直接调绘制函数、喂它危险的入参**：
// 一行 state=FailedNever 但**带 2 个窗口**，一行同款窗口但 state=Normal 作对照。
// 这样「画不画条」这个决定只可能来自 showsQuotaBar，没有别的因子能替代它。
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
        r.windows = {w1, w2};   // ⚠ 两行窗口数据**完全一样**，只有 state 不同
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

    // 从未成功：两个窗口都不得出现（哪怕入参里带着 62% / 88%）
    DOCTEST_CHECK_MESSAGE(ui.find("home.quota.row.1.bar.five_hour") == nullptr,
                          "假 0% 红线破了：从未成功的行画出了五小时进度条");
    DOCTEST_CHECK_MESSAGE(ui.find("home.quota.row.1.bar.weekly") == nullptr,
                          "假 0% 红线破了：从未成功的行画出了本周进度条");
    DOCTEST_CHECK(ui.find("home.quota.row.1.error") != nullptr);
    DOCTEST_CHECK(ui.find("home.quota.row.1.config") == nullptr);
    // 对照组：同样两个窗口、state=Normal → 必须画条
    DOCTEST_CHECK_MESSAGE(
        ui.find("home.quota.row.2.bar.five_hour.track") != nullptr &&
            ui.find("home.quota.row.2.bar.weekly.track") != nullptr,
        "对照组：Normal 行必须画出两条（否则前两条断言是恒真的）");
    // 倒计时只在 resetsAt 非空时画（入参无 resetsAt → 不画「--」）
    DOCTEST_CHECK(ui.find("home.quota.row.2.bar.five_hour.reset") == nullptr);
    return true;
})

// ==================== ⑬ 窗口太矮时整张不画（不挤掉既有内容）====================
// 首页整页**没有外层滚动容器**：余量卡若不按窗口剩余高度封顶，会把下方的
// 图表卡与快捷按钮行顶出可视区（那是既有功能，本批不能弄坏它）。故窗口装不下
// 一行余量行时**整张不画**。
REGISTER_TEST(home_quota_card_hidden_when_window_too_short,
              "home quota: 窗口装不下一行余量行时整张卡不画（保既有页面）", {
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

    // 高窗（1000）：装得下 → 画
    eui::Screen tall;
    tall.width = 1600.0f;
    tall.height = 1000.0f;
    eui::Ui tallUi;
    composeFrame(*mod, tallUi, tall, "home_quota_tall");
    DOCTEST_CHECK_MESSAGE(tallUi.find("home.quota") != nullptr,
                          "1000 高（默认窗）必须画出余量卡");

    // 矮窗（720 = 设计最小窗高）：装不下 → 整张不画，但既有统计网格照旧在
    eui::Screen shortScreen;
    shortScreen.width = 1200.0f;
    shortScreen.height = 720.0f;
    eui::Ui shortUi;
    composeFrame(*mod, shortUi, shortScreen, "home_quota_short");
    DOCTEST_CHECK_MESSAGE(shortUi.find("home.quota") == nullptr,
                          "720 高时余量卡必须整张缺席（否则顶掉快捷按钮行）");
    DOCTEST_CHECK(shortUi.find("home.stats") != nullptr);
    DOCTEST_CHECK(shortUi.find("home.actions") != nullptr);
    mod->onStop();
    return true;
})

// ==================== ⑪ 错误文案恒非空（写侧兜底 + 读侧兜底）====================
REGISTER_TEST(home_quota_error_text_never_empty,
              "home quota: 有错时 last_error 恒非空（旧行读侧也兜底）", {
    test_support::TestEnv env(kDbPrefix, "errtext", true);
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    cq::QuotaStore store(sq);

    const int64_t pid = insertProfile(sq, "空文案行");
    DOCTEST_REQUIRE(store.saveConfig(makeConfig(pid, "deepseek")).isOk());
    // provider 忘了填 errorMessage —— 落库仍必须给出可显示的话
    DOCTEST_REQUIRE(store.applyFetchResult(
                        pid, fail(cq::QuotaError::Entitlement, ""), 1000)
                        .isOk());
    auto row = store.loadResult(pid);
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK(!row.value().lastError.empty());
    DOCTEST_CHECK(row.value().lastError.find("套餐") != std::string::npos);

    // 读侧兜底：模拟「兜底之前落下的旧行」——直接把列改空
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

// ==================== ⑫ 手动刷新绕过 interval / auto_refresh，但仍受 enabled 闸 ====================
REGISTER_TEST(home_quota_manual_refresh_due_rules,
              "home quota: 手动刷新绕 interval 与 auto_refresh，但不放过 enabled=0", {
    cq::QuotaConfigRow cfg;
    cfg.profileId = 1;
    cfg.providerId = "opencode_go";
    cfg.enabled = true;
    cfg.autoRefresh = true;
    cfg.intervalMinutes = 15;

    // 刚查过（不自动到期）→ 手动必须放行
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(cfg, 1000, 1000, false));
    DOCTEST_CHECK(cq::QuotaRefresher::isDue(cfg, 1000, 1000, true));

    // autoRefresh=0（「手动刷新档」）→ 自动不放行、手动放行
    cq::QuotaConfigRow manual = cfg;
    manual.autoRefresh = false;
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(manual, 100000, 0, false));
    DOCTEST_CHECK(cq::QuotaRefresher::isDue(manual, 100000, 0, true));

    // 总开关关着 → 手动也不该发请求（没开的档不该被强行查询）
    cq::QuotaConfigRow off = cfg;
    off.enabled = false;
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(off, 100000, 0, true));

    // provider_id 空 → 手动也不发
    cq::QuotaConfigRow none = cfg;
    none.providerId.clear();
    DOCTEST_CHECK(!cq::QuotaRefresher::isDue(none, 100000, 0, true));
    return true;
})
// ============ G8：首页高度预算（纯函数）============
//
// G7 遗留 1 的欠账：1200×720（设计最小窗）下看不到余量卡。这段把原先内联在
// compose lambda 里、谁也测不到的预算算式提成纯函数，并把**阈值钉死**——
// 「矮窗下画不画」从此是一条会红的断言，不是心算。
REGISTER_TEST(home_quota_budget_threshold_is_pinned,
              "home_quota: 高度预算阈值钉死（矮窗压缩图表给余量卡让位）", {
    // 本机 theme 的实际取值（home_module.cpp:336-338 / 346：spacingXXLarge=40、
    // spacingLarge×1.25=20、统计格 144×fontScale）。这里按 fontScale=1 取值。
    const float pad = 40.0f;
    const float gap = 20.0f;
    const float statH = 144.0f;

    // ---- ① 宽窗：图表不压缩，余量卡拿到上限 ----
    const auto tall = module::home::quota_card::computeHomeQuotaBudget(1000.0f, statH, pad, gap);
    DOCTEST_CHECK(tall.drawsQuotaCard);
    DOCTEST_CHECK(!tall.pageStillOverflows);
    DOCTEST_CHECK_EQ(tall.chartHeight, module::home::quota_card::kHomeChartH);
    // quotaBodyHeight 是**可用空间**，不是最终行内容高；行内容高由
    // computeQuotaCardMetrics 取 min(内容高, 可用空间) 并封顶
    // kQuotaCardMaxBodyH。所以这里断言的是「够画」，不是「已达上限」。
    DOCTEST_CHECK(tall.quotaBodyHeight >= module::home::quota_card::kQuotaCardMinBodyH);
    // 真·很高时可用空间确实超过封顶值（封顶那条分支真的会被走到）
    const auto huge = module::home::quota_card::computeHomeQuotaBudget(2000.0f, statH, pad, gap);
    DOCTEST_CHECK(huge.quotaBodyHeight >= module::home::quota_card::kQuotaCardMaxBodyH);

    // ---- ② 本机典型客户区高（805pt：工作区 833 − 标题栏 28）----
    // 这是**修复前余量卡完全不画**的高度区间：原算式 reserve=750，805−750=55
    // < 96。现在图表被压到给余量卡让位，余量卡照常画出。
    const auto macDefault = module::home::quota_card::computeHomeQuotaBudget(805.0f, statH, pad, gap);
    DOCTEST_CHECK(macDefault.drawsQuotaCard);
    DOCTEST_CHECK(!macDefault.pageStillOverflows);
    DOCTEST_CHECK(macDefault.quotaBodyHeight >= module::home::quota_card::kQuotaCardMinBodyH);
    DOCTEST_CHECK(macDefault.chartHeight < module::home::quota_card::kHomeChartH);
    DOCTEST_CHECK(macDefault.chartHeight >= module::home::quota_card::kHomeChartMinH);

    // ---- ③ 临界点前后：阈值必须是**确定的**，不能是「差不多」----
    // 逐像素扫 700..900，找到「最矮仍画得出」的那一屏，并断言它就是纯函数的边界
    float lowestDrawing = 0.0f;
    for (float h = 700.0f; h <= 900.0f; h += 1.0f) {
        const auto b = module::home::quota_card::computeHomeQuotaBudget(h, statH, pad, gap);
        if (b.drawsQuotaCard) lowestDrawing = (lowestDrawing == 0.0f) ? h : lowestDrawing;
    }
    DOCTEST_CHECK(lowestDrawing > 0.0f);
    // 边界上下各差一像素，判定必须翻转（证明这是一个真阈值，不是恒真）
    const auto justAbove = module::home::quota_card::computeHomeQuotaBudget(
        lowestDrawing, statH, pad, gap);
    const auto justBelow = module::home::quota_card::computeHomeQuotaBudget(
        lowestDrawing - 1.0f, statH, pad, gap);
    DOCTEST_CHECK(justAbove.drawsQuotaCard);
    DOCTEST_CHECK(!justBelow.drawsQuotaCard);
    DOCTEST_CHECK(justBelow.pageStillOverflows);
    // 再矮一屏：压缩**换不来**余量卡（连 kHomeChartMinH 都腾不出下限），
    // 于是图表保持完整高度 —— 宁可画不出余量卡，也不把图表压成一条缝。
    // 这条与 ④ 是同一个语义的两个取样点。
    const auto wayBelow = module::home::quota_card::computeHomeQuotaBudget(
        lowestDrawing - 1.0f, statH, pad, gap);
    DOCTEST_CHECK(!wayBelow.drawsQuotaCard);
    DOCTEST_CHECK_EQ(wayBelow.chartHeight, module::home::quota_card::kHomeChartH);

    // ---- ④ 设计最小窗 1200×720：仍是「本页装不下」，且**图表不许被压** ----
    // 首页没有外层滚动容器，这是本批**未解**的那一段（见 report-G8 遗留）：
    // 要在 720 也画出余量卡，得给首页加外层 scrollView，那不是纯函数能解决的。
    //
    // 关键：本窗高下压缩图表**换不来任何余量卡**（压到下限仍只有 70 < 96），
    // 所以预算必须让图表保持完整高度 —— 否则是把用户本来看得懂的图表压成一条缝
    // 换零收益。test_ui_modern 的「Home Token 分布」几何用例（跑在 1200×720、
    // 峰值柱高钉死 84）就是这条的外部守门人。
    const auto minWindow = module::home::quota_card::computeHomeQuotaBudget(720.0f, statH, pad, gap);
    DOCTEST_CHECK(!minWindow.drawsQuotaCard);
    DOCTEST_CHECK(minWindow.pageStillOverflows);
    DOCTEST_CHECK_EQ(minWindow.chartHeight, module::home::quota_card::kHomeChartH);
    return true;
})
