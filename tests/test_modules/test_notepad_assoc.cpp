// tests/test_modules/test_notepad_assoc.cpp
// 「记事本文件关联」纯逻辑单测（F-18，need263 批 4 棒 B；接管退役的
// test_md_assoc_logic.cpp，矩阵契约不变）：
//   · 状态字四态矩阵（enabled × everEnabled × 聚合回读归属 = 12 组合全遍历）
//   · 自愈决策矩阵 + 幂等 + 修复闭环
//   · 多扩展聚合/未生效摘要纯逻辑（F-18 逐扩展化新增面）
//   · SettingsStore 持久化往返（notepadAssoc* 两字段，DB 键沿 F-10 字面量）
//   · 卡高契约三档 + 文字档随行数派生
// 判定规则来源 = notepad_assoc_logic.h 头注释（方案卡 §十）。
// 断言回退即红：四态推导改坏任一档 / 决策矩阵改坏任一格 / 聚合改坏任一档 /
// 持久化丢键都会挂。写法约定同 test_settings.cpp（REGISTER_TEST + DOCTEST_CHECK）。
// 系统触达链路（registry roundtrip / spy / unregister 幂等）归
// tests/test_platform/test_file_association.cpp（平台层用例）。
#include "doctest.h"
#include "test_support.h"

#include "notepad_assoc_logic.h"
#include "settings_config.h"

#include "core/database.h"

#include <string>
#include <vector>

namespace {

using module::settings::NotepadAssocHealAction;
using module::settings::NotepadAssocStatusKind;
using AssocState = platform::file_association::AssocState;

constexpr const char* kPrefix = "devdesk_test_notepad_assoc_";

} // namespace

// ── F-18：状态字四态推导（enabled × everEnabled × 聚合归属 = 12 组合全遍历）──
REGISTER_TEST(notepad_assoc_status_matrix, "notepad_assoc: 状态字四态矩阵（12 组合+文案契约）", {
    auto kind = [](bool e, bool ev, AssocState s) {
        return module::settings::notepadAssocStatusKind(e, ev, s);
    };
    // 关 × 从未开过 → 无状态字（与归属无关：关态不消费回读）
    DOCTEST_CHECK(kind(false, false, AssocState::Ours) == NotepadAssocStatusKind::None);
    DOCTEST_CHECK(kind(false, false, AssocState::Others) == NotepadAssocStatusKind::None);
    DOCTEST_CHECK(kind(false, false, AssocState::Unknown) == NotepadAssocStatusKind::None);
    // 关 × 曾开过 → 已停止自动维护
    DOCTEST_CHECK(kind(false, true, AssocState::Ours) == NotepadAssocStatusKind::MaintenanceStopped);
    DOCTEST_CHECK(kind(false, true, AssocState::Others) == NotepadAssocStatusKind::MaintenanceStopped);
    DOCTEST_CHECK(kind(false, true, AssocState::Unknown) == NotepadAssocStatusKind::MaintenanceStopped);
    // 开 × 归本应用 → 已关联（everEnabled 不参与开态推导）
    DOCTEST_CHECK(kind(true, false, AssocState::Ours) == NotepadAssocStatusKind::Linked);
    DOCTEST_CHECK(kind(true, true, AssocState::Ours) == NotepadAssocStatusKind::Linked);
    // 开 × 非本应用 / 查询失败 → 未生效（Unknown=按需修复档，呈现与 Others 同档）
    DOCTEST_CHECK(kind(true, false, AssocState::Others) == NotepadAssocStatusKind::NotEffective);
    DOCTEST_CHECK(kind(true, true, AssocState::Others) == NotepadAssocStatusKind::NotEffective);
    DOCTEST_CHECK(kind(true, false, AssocState::Unknown) == NotepadAssocStatusKind::NotEffective);
    DOCTEST_CHECK(kind(true, true, AssocState::Unknown) == NotepadAssocStatusKind::NotEffective);
    // 文案契约：None → 空串（卡片按空判断不建状态字元素）；其余三态非空
    DOCTEST_CHECK(std::string(module::settings::notepadAssocStatusLabel(NotepadAssocStatusKind::None)).empty());
    DOCTEST_CHECK(std::string(module::settings::notepadAssocStatusLabel(NotepadAssocStatusKind::Linked)) == "已关联");
    DOCTEST_CHECK(std::string(module::settings::notepadAssocStatusLabel(NotepadAssocStatusKind::NotEffective)) == "未生效");
    DOCTEST_CHECK(std::string(module::settings::notepadAssocStatusLabel(NotepadAssocStatusKind::MaintenanceStopped)) == "已停止自动维护");
    return true;
})

// ── F-18：启动自愈决策矩阵（6 组合）+ 幂等 + 修复闭环 ──
REGISTER_TEST(notepad_assoc_heal_matrix, "notepad_assoc: 自愈决策矩阵 + 幂等 + 修复闭环", {
    auto act = [](bool e, AssocState s) {
        return module::settings::notepadAssocHealAction(e, s);
    };
    // 关 × 任意 = Skip（整个检查跳过）
    DOCTEST_CHECK(act(false, AssocState::Ours) == NotepadAssocHealAction::Skip);
    DOCTEST_CHECK(act(false, AssocState::Others) == NotepadAssocHealAction::Skip);
    DOCTEST_CHECK(act(false, AssocState::Unknown) == NotepadAssocHealAction::Skip);
    // 开 × ours = 不动；开 × others / unknown（按需修复档）= 修复一次
    DOCTEST_CHECK(act(true, AssocState::Ours) == NotepadAssocHealAction::Skip);
    DOCTEST_CHECK(act(true, AssocState::Others) == NotepadAssocHealAction::Repair);
    DOCTEST_CHECK(act(true, AssocState::Unknown) == NotepadAssocHealAction::Repair);
    // 幂等（纯函数）：同输入重复决策结果一致
    for (int i = 0; i < 3; ++i) {
        DOCTEST_CHECK(act(true, AssocState::Others) == NotepadAssocHealAction::Repair);
        DOCTEST_CHECK(act(true, AssocState::Ours) == NotepadAssocHealAction::Skip);
    }
    // 修复闭环：Repair → （写入幂等）回读归本 → 再决策 = Skip
    //（模拟修复序列；真写入由 platform 注入缝用例覆盖，此处锁决策链）
    AssocState afterRepair = AssocState::Ours;
    DOCTEST_CHECK(act(true, afterRepair) == NotepadAssocHealAction::Skip);
    return true;
})

// ── F-18：多扩展聚合归属（aggregateAssocStates 纯逻辑，任一未生效=整体未生效）──
REGISTER_TEST(notepad_assoc_aggregate, "notepad_assoc: 多扩展聚合（全 ours/含 others/含 unknown/空表）", {
    using platform::file_association::aggregateAssocStates;
    // 全 Ours → Ours（唯一「已关联」档）
    DOCTEST_CHECK(aggregateAssocStates({}) == AssocState::Unknown);  // 空表 ≠ 已生效
    DOCTEST_CHECK(aggregateAssocStates({AssocState::Ours}) == AssocState::Ours);
    DOCTEST_CHECK(aggregateAssocStates({AssocState::Ours, AssocState::Ours,
                                        AssocState::Ours}) == AssocState::Ours);
    // 任一 Unknown → Unknown（查询失败优先于 Others：按需修复档成因）
    DOCTEST_CHECK(aggregateAssocStates({AssocState::Ours, AssocState::Unknown}) ==
                  AssocState::Unknown);
    DOCTEST_CHECK(aggregateAssocStates({AssocState::Others, AssocState::Unknown}) ==
                  AssocState::Unknown);
    // 无 Unknown 但含 Others → Others
    DOCTEST_CHECK(aggregateAssocStates({AssocState::Ours, AssocState::Others}) ==
                  AssocState::Others);
    DOCTEST_CHECK(aggregateAssocStates({AssocState::Others}) == AssocState::Others);
    return true;
})

// ── F-18：未生效摘要行（引导带数据源，逐扩展配对收集）──
REGISTER_TEST(notepad_assoc_failing_summary, "notepad_assoc: 未生效摘要（配对/截断/全 ours 空）", {
    const std::vector<std::string> exts = {".a", ".b", ".c", ".d", ".e", ".f"};
    const auto ours = AssocState::Ours;
    const auto other = AssocState::Others;
    // 全 Ours → 空串（不渲染引导行）
    DOCTEST_CHECK(module::settings::notepadAssocFailingSummary(
                      exts, {ours, ours, ours, ours, ours, ours}).empty());
    // 尺寸不齐（快照缺失）= 按配对下限安全收集，不越界
    DOCTEST_CHECK(module::settings::notepadAssocFailingSummary(exts, {}).empty());
    // 部分 Others：计数 + 逐项列出，保持入参顺序
    const std::string s2 = module::settings::notepadAssocFailingSummary(
        exts, {ours, other, ours, other, ours, ours});
    DOCTEST_CHECK(s2 == "2 项未生效：.b .d");
    // 超 4 项截断补省略号
    const std::string s5 = module::settings::notepadAssocFailingSummary(
        exts, {other, other, other, other, other, ours});
    DOCTEST_CHECK(s5 == "5 项未生效：.a .b .c .d …");
    // Unknown 同档计入（非 Ours 即未生效）
    const std::string su = module::settings::notepadAssocFailingSummary(
        {".x", ".y"}, {ours, AssocState::Unknown});
    DOCTEST_CHECK(su == "1 项未生效：.y");
    return true;
})

// ── F-18：mac 文字指引组装（摘要 + 平台指引单一来源）──
REGISTER_TEST(notepad_assoc_mac_guide_text, "notepad_assoc: mac 指引组装（含/不含摘要）", {
    const std::string bare = module::settings::notepadAssocMacGuideText("");
    DOCTEST_CHECK(bare.find("Finder") != std::string::npos);
    DOCTEST_CHECK(bare.find("全部更改") != std::string::npos);
    const std::string withSummary =
        module::settings::notepadAssocMacGuideText("2 项未生效：.b .d");
    DOCTEST_CHECK(withSummary.rfind("2 项未生效：.b .d。", 0) == 0);
    DOCTEST_CHECK(withSummary.find("Finder") != std::string::npos);
    return true;
})

// ── F-18：SettingsStore 持久化往返（两字段独立落库；DB 键沿 F-10 字面量）──
REGISTER_TEST(notepad_assoc_store_roundtrip, "notepad_assoc: 持久化往返 + 默认值（含关+曾开过跨重启）", {
    test_support::TestEnv env(kPrefix, "roundtrip", true);
    module::settings::SettingsStore store(::core::appDatabase());

    // 默认：关 + 从未开过（键缺失 → 默认 false）
    const auto fresh = store.load();
    DOCTEST_CHECK_FALSE(fresh.notepadAssocEnabled);
    DOCTEST_CHECK_FALSE(fresh.notepadAssocEverEnabled);

    // 开：两字段同时落盘 → 读回 true/true
    {
        module::settings::SettingsConfig cfg;
        cfg.notepadAssocEnabled = true;
        cfg.notepadAssocEverEnabled = true;
        DOCTEST_REQUIRE(store.save(cfg).isOk());
    }
    auto on = store.load();
    DOCTEST_CHECK(on.notepadAssocEnabled);
    DOCTEST_CHECK(on.notepadAssocEverEnabled);

    // 关：enabled 翻 false、ever 保持 true → 读回 false/true
    //（「关 + 曾开过 = 已停止自动维护」的判据必须跨重启成立）
    {
        module::settings::SettingsConfig cfg = on;
        cfg.notepadAssocEnabled = false;
        DOCTEST_REQUIRE(store.save(cfg).isOk());
    }
    auto off = store.load();
    DOCTEST_CHECK_FALSE(off.notepadAssocEnabled);
    DOCTEST_CHECK(off.notepadAssocEverEnabled);
    DOCTEST_CHECK(module::settings::notepadAssocStatusKind(
                      off.notepadAssocEnabled, off.notepadAssocEverEnabled,
                      AssocState::Unknown) == NotepadAssocStatusKind::MaintenanceStopped);

    // 两字段独立持久化：ever=false、enabled=true 也不串档
    {
        module::settings::SettingsConfig cfg;
        cfg.notepadAssocEnabled = true;
        cfg.notepadAssocEverEnabled = false;
        DOCTEST_REQUIRE(store.save(cfg).isOk());
    }
    auto mixed = store.load();
    DOCTEST_CHECK(mixed.notepadAssocEnabled);
    DOCTEST_CHECK_FALSE(mixed.notepadAssocEverEnabled);
    return true;
})

// ── 卡高契约（承接 md 卡设计文档 §3.5：引导行行盒动态增高，卡高同源重算）──
REGISTER_TEST(notepad_assoc_card_height, "notepad_assoc: 卡高三档（无引导 108 / mac 文字 140 / Win 按钮 152）", {
    DOCTEST_CHECK(module::settings::notepadAssocCardHeight(false, true) ==
                  module::settings::kNotepadAssocCardHeight);
    DOCTEST_CHECK(module::settings::notepadAssocCardHeight(true, false) ==
                  module::settings::kNotepadAssocCardHeightTextGuide);
    DOCTEST_CHECK(module::settings::notepadAssocCardHeight(true, true) ==
                  module::settings::kNotepadAssocCardHeightBtnGuide);
    return true;
})

// ── 文字档卡高随实测行数派生（承接 md 卡 review P2-1）──
REGISTER_TEST(notepad_assoc_card_height_lines, "notepad_assoc: 文字档卡高随行数派生（行数下限 2、参考几何 140 不变）", {
    using namespace module::settings;
    // 参考几何（100% 档两行）= 140 不变（设计 §2.3/§7 契约）
    DOCTEST_CHECK(notepadAssocCardHeight(true, false, 2) ==
                  kNotepadAssocCardHeightTextGuide);
    // 第三行 = +一个行高（kNotepadAssocGuideLineH=16）
    DOCTEST_CHECK(notepadAssocCardHeight(true, false, 3) ==
                  kNotepadAssocCardHeightTextGuide + kNotepadAssocGuideLineH);
    DOCTEST_CHECK(notepadAssocCardHeight(true, false, 4) ==
                  kNotepadAssocCardHeightTextGuide + 2.0f * kNotepadAssocGuideLineH);
    // 行数钳下限 2（不足两行按两行格律，不缩卡）
    DOCTEST_CHECK(notepadAssocCardHeight(true, false, 1) ==
                  kNotepadAssocCardHeightTextGuide);
    DOCTEST_CHECK(notepadAssocCardHeight(true, false, 0) ==
                  kNotepadAssocCardHeightTextGuide);
    // 按钮档/无引导档不受行数影响
    DOCTEST_CHECK(notepadAssocCardHeight(true, true, 5) == kNotepadAssocCardHeightBtnGuide);
    DOCTEST_CHECK(notepadAssocCardHeight(false, false, 5) == kNotepadAssocCardHeight);
    return true;
})

// ── F-18：生产/测试配置多扩展形状（单一真源 + 测试红线隔离）──
REGISTER_TEST(notepad_assoc_config_shape, "notepad_assoc: production/test 配置形状（26 扩展含 .md；test 双隔离扩展）", {
    const auto prod = platform::file_association::productionConfig();
    DOCTEST_CHECK_EQ(prod.progId, std::string("DevDesk.notepad"));
    DOCTEST_CHECK_EQ(prod.extensions.size(), core::notepad::notepadExtensions().size());
    bool hasMd = false;
    bool hasCpp = false;
    for (const auto& e : prod.extensions) {
        if (e == ".md") hasMd = true;
        if (e == ".cpp") hasCpp = true;
    }
    DOCTEST_CHECK(hasMd);    // D-7：单一入口维护全部扩展名含 .md
    DOCTEST_CHECK(hasCpp);
    // 测试红线：测试配置必须是测试专用键（禁触真实关联）
    const auto test = platform::file_association::testConfig();
    DOCTEST_CHECK_EQ(test.progId, std::string("DevDeskTest.notepad"));
    DOCTEST_CHECK_EQ(test.extensions.size(), size_t{2});
    DOCTEST_CHECK_EQ(test.extensions[0], std::string(".mdtest"));
    DOCTEST_CHECK_EQ(test.extensions[1], std::string(".notepadtest"));
    return true;
})
