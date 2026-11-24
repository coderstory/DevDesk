// tests/test_modules/test_usage_alert.cpp
// 需-9 用量告警定向用例：纯判定（日/月两档阈值 + 每档每日一次节流 + 跨档独立）
// 与阈值/闸状态持久化往返。假日键注入（dayKey = YYYYMMDD），零真时钟依赖；
// 持久化用例走 DEVDESK_DATA_ROOT 隔离根（resetCaseGlobals 出厂已设构建树内
// test_data），先落 seed 防其它用例写过的 panel_settings.json 串味。
#include "doctest.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/paths.h"
#include "module_memory/engine/telemem_config.h"
#include "module_memory/panel/settings_store.h"
#include "module_memory/proposals/usage_alert.h"

using module::memory::panel::MemoryPanelSettings;
using module::memory::proposals::UsageAlertFire;
using module::memory::proposals::UsageAlertGate;
using module::memory::proposals::UsageAlertThresholds;
using module::memory::proposals::UsageAlertTier;
using module::memory::proposals::evaluateUsageAlert;
using module::memory::proposals::usageAlertBody;
using module::memory::proposals::usageAlertTitle;

namespace {

const long long kDay1 = 20261029;
const long long kDay2 = 20261030;

bool hasTier(const std::vector<UsageAlertFire>& fires, UsageAlertTier tier) {
    for (const auto& f : fires) {
        if (f.tier == tier) return true;
    }
    return false;
}

}  // namespace

REGISTER_TEST(usage_alert_day_fire_once,
              "F-01 日档：越线击发一条，文案含用量/阈值/档位/估算口径", {
    UsageAlertThresholds th;  // 默认：日 50 万 / 软 200 万 / 硬 300 万
    UsageAlertGate day, soft, hard;
    // 未越线：不击发且武装
    auto quiet = evaluateUsageAlert(th, 400000, 1000000, kDay1, day, soft, hard);
    DOCTEST_CHECK_TRUE(quiet.empty());
    DOCTEST_CHECK_TRUE(day.armed);
    // 越线：击发日档一条
    auto fires = evaluateUsageAlert(th, 612345, 1000000, kDay1, day, soft, hard);
    DOCTEST_REQUIRE_EQ(fires.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(fires[0].tier, UsageAlertTier::Day);
    DOCTEST_CHECK_EQ(fires[0].usageTokens, 612345);
    DOCTEST_CHECK_EQ(fires[0].thresholdTokens, th.dayTokens);
    const auto title = usageAlertTitle(fires[0].tier);
    const auto body = usageAlertBody(fires[0]);
    DOCTEST_CHECK_TRUE(title.find("日档") != std::string::npos);
    DOCTEST_CHECK_TRUE(body.find("612345") != std::string::npos);
    DOCTEST_CHECK_TRUE(body.find("500000") != std::string::npos);
    DOCTEST_CHECK_TRUE(body.find("估算") != std::string::npos);
    return true;
})

REGISTER_TEST(usage_alert_day_boundary_equal,
              "F-01 边界：恰好等于阈值也算越线（含恰好等于）", {
    UsageAlertThresholds th;
    UsageAlertGate day, soft, hard;
    auto fires = evaluateUsageAlert(th, 500000, 1000000, kDay1, day, soft, hard);
    DOCTEST_CHECK_EQ(fires.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(fires[0].tier, UsageAlertTier::Day);
    // 月档同口径：恰好等于软档 → 击发软档
    UsageAlertGate day2, soft2, hard2;
    auto m = evaluateUsageAlert(th, 0, 2000000, kDay1, day2, soft2, hard2);
    DOCTEST_CHECK_EQ(m.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(m[0].tier, UsageAlertTier::MonthSoft);
    return true;
})

REGISTER_TEST(usage_alert_same_day_throttle,
              "F-02 节流：同档同日重复越线只响一次（阈值上调再降回也不加响）", {
    UsageAlertThresholds th;
    UsageAlertGate day, soft, hard;
    (void)evaluateUsageAlert(th, 600000, 1000000, kDay1, day, soft, hard);
    // 同日用量再涨：不重响
    auto again = evaluateUsageAlert(th, 700000, 1000000, kDay1, day, soft, hard);
    DOCTEST_CHECK_TRUE(again.empty());
    // 同日阈值上调越过用量（解除）再降回越线：当日已响过，仍不响
    th.dayTokens = 800000;
    (void)evaluateUsageAlert(th, 700000, 1000000, kDay1, day, soft, hard);
    DOCTEST_CHECK_TRUE(day.armed);  // 回落解除 → 重新武装
    th.dayTokens = 500000;
    auto reFire = evaluateUsageAlert(th, 700000, 1000000, kDay1, day, soft, hard);
    DOCTEST_CHECK_TRUE(reFire.empty());  // 每档每日一次底线
    return true;
})

REGISTER_TEST(usage_alert_next_day_rearm,
              "F-03 跨日复位：次日用量回落再越线 → 再提醒", {
    UsageAlertThresholds th;
    UsageAlertGate day, soft, hard;
    (void)evaluateUsageAlert(th, 600000, 1000000, kDay1, day, soft, hard);
    // 次日：今日账面自然归零（回落解除）
    (void)evaluateUsageAlert(th, 30000, 1000000, kDay2, day, soft, hard);
    DOCTEST_CHECK_TRUE(day.armed);
    // 次日再越线 → 再响
    auto fires = evaluateUsageAlert(th, 650000, 1000000, kDay2, day, soft, hard);
    DOCTEST_REQUIRE_EQ(fires.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(fires[0].tier, UsageAlertTier::Day);
    return true;
})

REGISTER_TEST(usage_alert_month_tiers_independent,
              "F-04 月档独立于日档各自置位；软→硬升级各自击发", {
    UsageAlertThresholds th;
    UsageAlertGate day, soft, hard;
    // 同一次判定：日档与月档（软档）同时越线 → 各响一条
    auto both = evaluateUsageAlert(th, 600000, 2100000, kDay1, day, soft, hard);
    DOCTEST_CHECK_EQ(both.size(), std::size_t{2});
    DOCTEST_CHECK_TRUE(hasTier(both, UsageAlertTier::Day));
    DOCTEST_CHECK_TRUE(hasTier(both, UsageAlertTier::MonthSoft));
    const auto softTitle = usageAlertTitle(UsageAlertTier::MonthSoft);
    DOCTEST_CHECK_TRUE(softTitle.find("月档") != std::string::npos);
    DOCTEST_CHECK_TRUE(softTitle.find("软档") != std::string::npos);
    // 月档文案含「估算」
    UsageAlertFire softFire{UsageAlertTier::MonthSoft, 2100000, 2000000};
    DOCTEST_CHECK_TRUE(usageAlertBody(softFire).find("估算") !=
                       std::string::npos);
    // 硬档文案：档位 + 检索不受影响 + 估算
    UsageAlertGate day2, soft2, hard2;
    auto hf = evaluateUsageAlert(th, 0, 3000000, kDay1, day2, soft2, hard2);
    DOCTEST_REQUIRE_EQ(hf.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(hf[0].tier, UsageAlertTier::MonthHard);
    const auto hardTitle = usageAlertTitle(UsageAlertTier::MonthHard);
    DOCTEST_CHECK_TRUE(hardTitle.find("硬档") != std::string::npos);
    DOCTEST_CHECK_TRUE(usageAlertBody(hf[0]).find("检索不受影响") !=
                       std::string::npos);
    // 软档→硬档升级（软档已响过的闸上）：硬档新越线仍击发
    UsageAlertGate day3, soft3, hard3;
    (void)evaluateUsageAlert(th, 0, 2100000, kDay1, day3, soft3, hard3);  // 软档响
    auto up = evaluateUsageAlert(th, 0, 3100000, kDay1, day3, soft3, hard3);
    DOCTEST_REQUIRE_EQ(up.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(up[0].tier, UsageAlertTier::MonthHard);
    return true;
})

REGISTER_TEST(usage_alert_month_still_over_no_refire,
              "F-03/§3.2 月档越线未解除次日不重响；回落再进位重新武装", {
    UsageAlertThresholds th;
    UsageAlertGate day, soft, hard;
    (void)evaluateUsageAlert(th, 0, 2100000, kDay1, day, soft, hard);  // 软档响
    // 次日仍在软档区间：不重响
    auto still = evaluateUsageAlert(th, 0, 2200000, kDay2, day, soft, hard);
    DOCTEST_CHECK_TRUE(still.empty());
    // 用量回落到软档下：重新武装
    (void)evaluateUsageAlert(th, 0, 1900000, kDay2, day, soft, hard);
    DOCTEST_CHECK_TRUE(soft.armed);
    // 再进位越线：再响
    auto refire = evaluateUsageAlert(th, 0, 2050000, kDay2, day, soft, hard);
    DOCTEST_REQUIRE_EQ(refire.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(refire[0].tier, UsageAlertTier::MonthSoft);
    return true;
})

REGISTER_TEST(usage_alert_hard_jump_covers_soft,
              "月档跳升硬档：软档不另响（硬档文案覆盖），硬档解除后回软档区间不补响", {
    UsageAlertThresholds th;
    UsageAlertGate day, soft, hard;
    // 一步跳进硬档：只响硬档
    auto jump = evaluateUsageAlert(th, 0, 3100000, kDay1, day, soft, hard);
    DOCTEST_REQUIRE_EQ(jump.size(), std::size_t{1});
    DOCTEST_CHECK_EQ(jump[0].tier, UsageAlertTier::MonthHard);
    DOCTEST_CHECK_FALSE(soft.armed);  // 软档被硬档覆盖，保持已越过态
    // 硬档阈值上调越过用量（解除硬档），仍在软档区间：软档未解除不补响
    th.monthHardTokens = 5000000;
    auto back = evaluateUsageAlert(th, 0, 3100000, kDay2, day, soft, hard);
    DOCTEST_CHECK_TRUE(back.empty());
    DOCTEST_CHECK_TRUE(hard.armed);
    return true;
})

REGISTER_TEST(usage_alert_disabled_tier,
              "阈值 ≤0 = 该档关闭：不击发也不动闸", {
    UsageAlertThresholds th;
    th.dayTokens = 0;
    th.monthSoftTokens = 0;
    th.monthHardTokens = 0;
    UsageAlertGate day, soft, hard;
    auto fires = evaluateUsageAlert(th, 9000000, 9000000, kDay1, day, soft, hard);
    DOCTEST_CHECK_TRUE(fires.empty());
    DOCTEST_CHECK_TRUE(day.armed);
    DOCTEST_CHECK_TRUE(soft.armed);
    DOCTEST_CHECK_TRUE(hard.armed);
    return true;
})

REGISTER_TEST(usage_alert_settings_round_trip,
              "F-05 阈值与闸状态持久化往返 + 旧文件缺键回默认", {
    // 前置：隔离根内落 seed（防其它用例写过的 panel_settings.json 串味）
    MemoryPanelSettings seed;
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(seed).isOk());
    // 改动三档阈值 + 击发态闸 → 落盘 → 重读全等
    MemoryPanelSettings s;
    s.usageAlertsEnabled = false;  // need:380 B-5 开关随设置往返
    s.usageDayAlertTokens = 123456;
    s.usageSoftBudgetTokens = 2345678;
    s.usageHardBudgetTokens = 3456789;
    s.usageDayGate.armed = false;
    s.usageDayGate.lastFireDay = 20261030;
    s.usageSoftGate.armed = true;
    s.usageSoftGate.lastFireDay = 20261029;
    s.usageHardGate.armed = false;
    s.usageHardGate.lastFireDay = 20261028;
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(s).isOk());
    auto back = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_FALSE(back.usageAlertsEnabled);
    DOCTEST_CHECK_EQ(back.usageDayAlertTokens, 123456);
    DOCTEST_CHECK_EQ(back.usageSoftBudgetTokens, 2345678);
    DOCTEST_CHECK_EQ(back.usageHardBudgetTokens, 3456789);
    DOCTEST_CHECK_FALSE(back.usageDayGate.armed);
    DOCTEST_CHECK_EQ(back.usageDayGate.lastFireDay, 20261030);
    DOCTEST_CHECK_TRUE(back.usageSoftGate.armed);
    DOCTEST_CHECK_EQ(back.usageSoftGate.lastFireDay, 20261029);
    DOCTEST_CHECK_FALSE(back.usageHardGate.armed);
    DOCTEST_CHECK_EQ(back.usageHardGate.lastFireDay, 20261028);
    // 旧文件无自定义 → 全默认（开关开 + bug490 默认 1000 万 / 5000 万 / 1 亿，闸
    // 全武装）；need:380 B-5 迁移口径：已自定义的阈值键原样读入继续生效（上行往返已证）
    MemoryPanelSettings legacy;
    legacy.sanitizeEnabled = false;  // 只改旧键，阈值保持出厂默认
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(legacy).isOk());
    auto dflt = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_EQ(dflt.usageDayAlertTokens, 10000000);
    DOCTEST_CHECK_EQ(dflt.usageSoftBudgetTokens, 50000000);
    DOCTEST_CHECK_EQ(dflt.usageHardBudgetTokens, 100000000);
    DOCTEST_CHECK_TRUE(dflt.usageAlertsEnabled);
    DOCTEST_CHECK_TRUE(dflt.usageDayGate.armed);
    DOCTEST_CHECK_EQ(dflt.usageDayGate.lastFireDay, 0);
    DOCTEST_CHECK_TRUE(dflt.usageSoftGate.armed);
    DOCTEST_CHECK_TRUE(dflt.usageHardGate.armed);
    return true;
})

// 防：bug480——settings 文件路径经 path::string()（Windows ACP）喂 fopen，非 ACP 可
// 表示字符目录下存取全失败。U+1F600 在 ACP=936 不可表示，红在旧写法、绿在 fs::path 直收。
REGISTER_TEST(memory_panel_settings_non_ascii_path_round_trip,
              "bug480 非 ACP 可表示字符路径下设置存读往返", {
    namespace fs = std::filesystem;
    test_support::TempDir dir("mem_panel_bug480");
    const fs::path root = dir.path / fs::path(L"mem-\U0001F600");
    const auto u8 = root.u8string();
    test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                     std::string(u8.begin(), u8.end()));
    MemoryPanelSettings s;
    s.mcpSearchEnabled = false;
    s.usageDayAlertTokens = 424242;
    s.llmProfileName = "bug480-emoji-path";
    DOCTEST_CHECK(module::memory::panel::savePanelSettings(s).isOk());
    auto back = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_EQ(back.usageDayAlertTokens, 424242);
    DOCTEST_CHECK_EQ(back.llmProfileName, std::string("bug480-emoji-path"));
    DOCTEST_CHECK_FALSE(back.mcpSearchEnabled);
    return true;
})

// 防：bug490——记忆 token 预算上调（日 1000 万 / 月软 5000 万 / 月硬 1 亿）后的
// 存量与边界：缺键取新默认；三键精确等于旧默认（savePanelSettings 全量写下的未
// 自定义值）迁移到新默认；自定义值原样保留；≤0 关闭语义不得被迁移成正值。
// 回退迁移或误迁移非正数都会红。
REGISTER_TEST(usage_alert_budget_upgrade_migration,
              "bug490 预算上调：缺键新默认 / 旧默认迁移 / 自定义与关闭语义保留", {
    namespace fs = std::filesystem;
    test_support::TempDir dir("mem_panel_bug490");
    const auto u8 = dir.path.u8string();
    test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                     std::string(u8.begin(), u8.end()));
    const fs::path file =
        module::memory::engine::memoryDataDir() / "panel_settings.json";
    // ① 最老文件三键缺失 → 新默认
    {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        DOCTEST_REQUIRE(f.is_open());
        f << "{\"sanitizeEnabled\":false}";
        f.close();
    }
    auto missing = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_EQ(missing.usageDayAlertTokens, 10000000);
    DOCTEST_CHECK_EQ(missing.usageSoftBudgetTokens, 50000000);
    DOCTEST_CHECK_EQ(missing.usageHardBudgetTokens, 100000000);
    // ② 三键等于旧默认（视为未自定义）→ 迁移为新默认
    MemoryPanelSettings oldDefaults;
    oldDefaults.usageDayAlertTokens = 1000000;
    oldDefaults.usageSoftBudgetTokens = 3000000;
    oldDefaults.usageHardBudgetTokens = 5000000;
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(oldDefaults).isOk());
    auto migrated = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_EQ(migrated.usageDayAlertTokens, 10000000);
    DOCTEST_CHECK_EQ(migrated.usageSoftBudgetTokens, 50000000);
    DOCTEST_CHECK_EQ(migrated.usageHardBudgetTokens, 100000000);
    // ③ 自定义值 → 原样保留
    MemoryPanelSettings custom;
    custom.usageDayAlertTokens = 123456;
    custom.usageSoftBudgetTokens = 2345678;
    custom.usageHardBudgetTokens = 3456789;
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(custom).isOk());
    auto kept = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_EQ(kept.usageDayAlertTokens, 123456);
    DOCTEST_CHECK_EQ(kept.usageSoftBudgetTokens, 2345678);
    DOCTEST_CHECK_EQ(kept.usageHardBudgetTokens, 3456789);
    // ④ ≤0 = 关闭语义，不得被迁移成正值
    MemoryPanelSettings off;
    off.usageDayAlertTokens = 0;
    off.usageSoftBudgetTokens = -1;
    off.usageHardBudgetTokens = -1000000;
    DOCTEST_REQUIRE(module::memory::panel::savePanelSettings(off).isOk());
    auto offBack = module::memory::panel::loadPanelSettings();
    DOCTEST_CHECK_EQ(offBack.usageDayAlertTokens, 0);
    DOCTEST_CHECK_EQ(offBack.usageSoftBudgetTokens, -1);
    DOCTEST_CHECK_EQ(offBack.usageHardBudgetTokens, -1000000);
    return true;
})
