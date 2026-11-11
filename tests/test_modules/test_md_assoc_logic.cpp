// tests/test_modules/test_md_assoc_logic.cpp
// 「关联 .md 文件」纯逻辑单测（F-10 状态字四态矩阵 + F-11 自愈决策矩阵）+
// SettingsStore 持久化往返（mdAssocEnabled/mdAssocEverEnabled 两键）。
// 判定规则来源 = md_assoc_logic.h 头注释（方案卡 §二.F-07/F-10/F-11）。
// 断言回退即红：四态推导改坏任一档 / 决策矩阵改坏任一格 / 持久化丢键都会挂。
// 写法约定同 test_settings.cpp（REGISTER_TEST + DOCTEST_CHECK）。
#include "doctest.h"
#include "test_support.h"

#include "md_assoc_logic.h"
#include "settings_config.h"

#include "core/database.h"

#include <string>

namespace {

using module::settings::MdAssocHealAction;
using module::settings::MdAssocStatusKind;
using AssocState = platform::file_association::AssocState;

constexpr const char* kPrefix = "devdesk_test_md_assoc_";

} // namespace

// ── F-10：状态字四态推导（enabled × everEnabled × 回读归属 = 8 组合全遍历）──
REGISTER_TEST(md_assoc_status_matrix, "md_assoc: 状态字四态矩阵（8 组合+文案契约）", {
    auto kind = [](bool e, bool ev, AssocState s) {
        return module::settings::mdAssocStatusKind(e, ev, s);
    };
    // 关 × 从未开过 → 无状态字（与归属无关：关态不消费回读）
    DOCTEST_CHECK(kind(false, false, AssocState::Ours) == MdAssocStatusKind::None);
    DOCTEST_CHECK(kind(false, false, AssocState::Others) == MdAssocStatusKind::None);
    DOCTEST_CHECK(kind(false, false, AssocState::Unknown) == MdAssocStatusKind::None);
    // 关 × 曾开过 → 已停止自动维护（D-4：不注销，仅停自愈）
    DOCTEST_CHECK(kind(false, true, AssocState::Ours) == MdAssocStatusKind::MaintenanceStopped);
    DOCTEST_CHECK(kind(false, true, AssocState::Others) == MdAssocStatusKind::MaintenanceStopped);
    DOCTEST_CHECK(kind(false, true, AssocState::Unknown) == MdAssocStatusKind::MaintenanceStopped);
    // 开 × 归本应用 → 已关联（everEnabled 不参与开态推导）
    DOCTEST_CHECK(kind(true, false, AssocState::Ours) == MdAssocStatusKind::Linked);
    DOCTEST_CHECK(kind(true, true, AssocState::Ours) == MdAssocStatusKind::Linked);
    // 开 × 非本应用 / 查询失败 → 未生效（Unknown=按需修复档，呈现与 Others 同档）
    DOCTEST_CHECK(kind(true, false, AssocState::Others) == MdAssocStatusKind::NotEffective);
    DOCTEST_CHECK(kind(true, true, AssocState::Others) == MdAssocStatusKind::NotEffective);
    DOCTEST_CHECK(kind(true, false, AssocState::Unknown) == MdAssocStatusKind::NotEffective);
    DOCTEST_CHECK(kind(true, true, AssocState::Unknown) == MdAssocStatusKind::NotEffective);
    // 文案契约：None → 空串（卡片按空判断不建状态字元素）；其余三态非空
    DOCTEST_CHECK(std::string(module::settings::mdAssocStatusLabel(MdAssocStatusKind::None)).empty());
    DOCTEST_CHECK(std::string(module::settings::mdAssocStatusLabel(MdAssocStatusKind::Linked)) == "已关联");
    DOCTEST_CHECK(std::string(module::settings::mdAssocStatusLabel(MdAssocStatusKind::NotEffective)) == "未生效");
    DOCTEST_CHECK(std::string(module::settings::mdAssocStatusLabel(MdAssocStatusKind::MaintenanceStopped)) == "已停止自动维护");
    return true;
})

// ── F-11：启动自愈决策矩阵（6 组合）+ 幂等 + 修复闭环 ──
REGISTER_TEST(md_assoc_heal_matrix, "md_assoc: 自愈决策矩阵 + 幂等 + 修复闭环", {
    auto act = [](bool e, AssocState s) {
        return module::settings::mdAssocHealAction(e, s);
    };
    // 关 × 任意 = Skip（D3：整个检查跳过）
    DOCTEST_CHECK(act(false, AssocState::Ours) == MdAssocHealAction::Skip);
    DOCTEST_CHECK(act(false, AssocState::Others) == MdAssocHealAction::Skip);
    DOCTEST_CHECK(act(false, AssocState::Unknown) == MdAssocHealAction::Skip);
    // 开 × ours = 不动（D5）；开 × others / unknown（按需修复档）= 修复一次（D6）
    DOCTEST_CHECK(act(true, AssocState::Ours) == MdAssocHealAction::Skip);
    DOCTEST_CHECK(act(true, AssocState::Others) == MdAssocHealAction::Repair);
    DOCTEST_CHECK(act(true, AssocState::Unknown) == MdAssocHealAction::Repair);
    // 幂等（纯函数）：同输入重复决策结果一致
    for (int i = 0; i < 3; ++i) {
        DOCTEST_CHECK(act(true, AssocState::Others) == MdAssocHealAction::Repair);
        DOCTEST_CHECK(act(true, AssocState::Ours) == MdAssocHealAction::Skip);
    }
    // 修复闭环：Repair → （写入幂等）回读归本 → 再决策 = Skip
    //（模拟 D6→D7 序列；真写入由 platform 注入缝用例覆盖，此处锁决策链）
    AssocState afterRepair = AssocState::Ours;
    DOCTEST_CHECK(act(true, afterRepair) == MdAssocHealAction::Skip);
    return true;
})

// ── F-10：SettingsStore 持久化往返（两字段独立落库）──
REGISTER_TEST(md_assoc_store_roundtrip, "md_assoc: 持久化往返 + 默认值（含 D-4 关+曾开过跨重启）", {
    test_support::TestEnv env(kPrefix, "roundtrip", true);
    module::settings::SettingsStore store(::core::appDatabase());

    // 默认：关 + 从未开过（键缺失 → 默认 false）
    const auto fresh = store.load();
    DOCTEST_CHECK_FALSE(fresh.mdAssocEnabled);
    DOCTEST_CHECK_FALSE(fresh.mdAssocEverEnabled);

    // 开：两字段同时落盘 → 读回 true/true
    {
        module::settings::SettingsConfig cfg;
        cfg.mdAssocEnabled = true;
        cfg.mdAssocEverEnabled = true;
        DOCTEST_REQUIRE(store.save(cfg).isOk());
    }
    auto on = store.load();
    DOCTEST_CHECK(on.mdAssocEnabled);
    DOCTEST_CHECK(on.mdAssocEverEnabled);

    // 关（D-4 不注销）：enabled 翻 false、ever 保持 true → 读回 false/true
    //（「关 + 曾开过 = 已停止自动维护」的判据必须跨重启成立）
    {
        module::settings::SettingsConfig cfg = on;
        cfg.mdAssocEnabled = false;
        DOCTEST_REQUIRE(store.save(cfg).isOk());
    }
    auto off = store.load();
    DOCTEST_CHECK_FALSE(off.mdAssocEnabled);
    DOCTEST_CHECK(off.mdAssocEverEnabled);
    DOCTEST_CHECK(module::settings::mdAssocStatusKind(
                      off.mdAssocEnabled, off.mdAssocEverEnabled,
                      AssocState::Unknown) == MdAssocStatusKind::MaintenanceStopped);

    // 两字段独立持久化：ever=false、enabled=true 也不串档
    {
        module::settings::SettingsConfig cfg;
        cfg.mdAssocEnabled = true;
        cfg.mdAssocEverEnabled = false;
        DOCTEST_REQUIRE(store.save(cfg).isOk());
    }
    auto mixed = store.load();
    DOCTEST_CHECK(mixed.mdAssocEnabled);
    DOCTEST_CHECK_FALSE(mixed.mdAssocEverEnabled);
    return true;
})

// ── 卡高契约（设计文档 §3.5：引导行行盒动态增高，卡高同源重算）──
REGISTER_TEST(md_assoc_card_height, "md_assoc: 卡高三档（无引导 48 / mac 文字 66 / Win 按钮 78）", {
    DOCTEST_CHECK(module::settings::mdAssocCardHeight(false, true) ==
                  module::settings::kMdAssocCardHeight);
    DOCTEST_CHECK(module::settings::mdAssocCardHeight(true, false) ==
                  module::settings::kMdAssocCardHeightTextGuide);
    DOCTEST_CHECK(module::settings::mdAssocCardHeight(true, true) ==
                  module::settings::kMdAssocCardHeightBtnGuide);
    return true;
})
