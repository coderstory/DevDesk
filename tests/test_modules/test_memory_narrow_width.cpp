// tests/test_modules/test_memory_narrow_width.cpp
// need298（2026-11-18 走查 §1.3）窄窗几何单测：
// 阈值宽度 drive 到断点，断言 不重叠 / 换行 / 省略。阈值与让位宽度的
// 单一来源在 memory_module_internal.h（compose 与本测同一口径）；
// bug217 行盒预算截断（truncateByAdvance）同族做法。
// 全部纯函数，零渲染零网络。
#include "doctest.h"

#include "test_support.h"

#include "module_memory/memory_module_internal.h"

#include <string>

namespace memory = module::memory;

namespace {
// 精简版 doctest 无 Approx：几何值全部整数档常量，0.5px 容差足够。
bool approx(float a, float b) { return a - b < 0.5f && b - a < 0.5f; }
}  // namespace

// ── M1 健康格② 状态行让位「跑评测」96×28 ──

REGISTER_TEST(memory_narrow_health_status_row_clears_run_button,
              "memory need298 M1: 状态行宽让位跑评测钮，同带不叠印",
              {
    // 让位后：右缘 = 卡右 14 − 96 − 8，与按钮左缘间距恰 8
    for (float cardW : {150.0f, 214.0f, 300.0f}) {
        const float rowW = memory::healthStatusRowW(cardW, true);
        const float rowRight = 14.0f + rowW;
        const float btnLeft = cardW - 14.0f - 96.0f;
        DOCTEST_CHECK_MESSAGE(rowRight + 8.0f <= btnLeft + 0.5f,
                              "状态行右缘 + 8 隙不得越按钮左缘 cardW=" +
                                  std::to_string(cardW));
        DOCTEST_CHECK_MESSAGE(rowW > 0.0f, "状态行宽须为正 cardW=" +
                                               std::to_string(cardW));
    }
    DOCTEST_CHECK(approx(memory::healthStatusRowW(214.0f, false), 186.0f));
    return true;
});

// ── M3 健康四格 2×2 断点（#181 §8：<880 收两列）──

REGISTER_TEST(memory_narrow_health_grid_breakpoint_2x2,
              "memory need298 M3: 内容区 <880 收 2×2，格宽回升超 200",
              {
    DOCTEST_CHECK(memory::healthGridCols(880.0f) == 4);
    DOCTEST_CHECK(memory::healthGridCols(879.0f) == 2);
    DOCTEST_CHECK(memory::healthGridRows(880.0f) == 1);
    DOCTEST_CHECK(memory::healthGridRows(879.0f) == 2);
    // 挤压场景：880 宽 4 列格宽 ≈214；窄窗 460 收 2 列后 ≈226（>200 不再挤压）
    DOCTEST_CHECK(approx(memory::healthGridCardW(880.0f), 214.0f));
    DOCTEST_CHECK(memory::healthGridCardW(460.0f) > 200.0f);
    // 实占高供 composeConsole 推进 cy：2×2 态两行
    const float cardH = core::theme::scaled(memory::kHealthGridH);
    DOCTEST_CHECK(cardH * 2.0f + memory::kCardGap > cardH);
    return true;
});

// ── M4 引擎状态带警示短语不越右端按钮组 ──

REGISTER_TEST(memory_narrow_engine_band_warn_clears_button_group,
              "memory need298 M4: 警示短语可用宽止于按钮组左缘 −8，超短钳 0",
              {
    // 宽窗：短语有空间；窄窗（640，含向导钮）钳 0 → compose 不画不叠印
    DOCTEST_CHECK(memory::engineBandWarnW(1000.0f, false) > 300.0f);
    DOCTEST_CHECK(memory::engineBandWarnW(1000.0f, true) <
                  memory::engineBandWarnW(1000.0f, false));
    DOCTEST_CHECK(memory::engineBandWarnW(640.0f, true) <= 0.0f);
    DOCTEST_CHECK(memory::engineBandWarnW(640.0f, false) > 0.0f);
    return true;
});

// ── M5 批量操作条两段式 ──

REGISTER_TEST(memory_narrow_batch_bar_two_segment,
              "memory need298 M5: <740 说明独占一行 + 按钮行下一行，不再负宽叠印",
              {
    // 宽窗：单行 44，说明区右缘与「取消选择」左缘留隙
    const float wideInfoW = memory::batchInfoW(800.0f);
    DOCTEST_CHECK(approx(memory::batchBarH(800.0f), 44.0f));
    DOCTEST_CHECK(14.0f + wideInfoW + 16.0f <= 800.0f - 260.0f);
    // 窄窗：说明独占一行（全宽−28 恒正，原 w−480 会变负），按钮下沉第二行
    DOCTEST_CHECK(approx(memory::batchInfoW(400.0f), 372.0f));
    DOCTEST_CHECK(approx(memory::batchBarH(400.0f), 76.0f));
    DOCTEST_CHECK(approx(memory::batchBtnRowY(400.0f), 42.0f));
    // 行带不重叠：说明行 12~32（scaled 1.0），按钮行 42 起
    DOCTEST_CHECK(memory::batchBtnRowY(400.0f) >= 32.0f);
    return true;
});

// ── M6 工具条折行态 chips 独占第二行 ──

REGISTER_TEST(memory_narrow_toolbar_wrap_state_row_below_button,
              "memory need298 M6: 折行态 chips 第二行 y54，在「批量处理」钮底之下",
              {
    const float chipsRowY = memory::batchToolbarStateRowY(true);
    const float btnBottom = 10.0f + 32.0f;
    DOCTEST_CHECK(chipsRowY >= btnBottom);  // 不与 y10~42 的按钮同带
    DOCTEST_CHECK(approx(memory::batchToolbarStateRowY(false), 10.0f));
    return true;
});

// ── M7 已裁决卡类型盒不越时间戳列 ──

REGISTER_TEST(memory_narrow_decided_kind_box_clears_timestamp,
              "memory need298 M7: 类型盒右缘 ≤ 时间戳列左缘（原越界 6px 清零）",
              {
    for (float w : {300.0f, 460.0f, 900.0f}) {
        const float kindRight = 16.0f + memory::decidedKindBoxW(w);
        const float timeLeft = w - 190.0f;
        DOCTEST_CHECK_MESSAGE(kindRight <= timeLeft + 0.5f,
                              "类型盒不得越时间戳列 w=" + std::to_string(w));
    }
    return true;
});

// ── M9 审核 TAB 宽自适应 ──

REGISTER_TEST(memory_narrow_review_tab_width_adaptive,
              "memory need298 M9: 文字实宽+32 自适应，下限 132 保短 TAB 观感",
              {
    DOCTEST_CHECK(approx(memory::reviewTabW(90.0f), 132.0f));
    DOCTEST_CHECK(approx(memory::reviewTabW(200.0f), 232.0f));
    // 4 位待审数场景（真库 1629）：估宽必须突破 132 定宽
    DOCTEST_CHECK(memory::reviewTabW(140.0f) > 132.0f);
    return true;
});

// ── M10 触发线磁贴文字省略 ──

REGISTER_TEST(memory_narrow_trigger_tile_text_ellipsis,
              "memory need298 M10: 窄窗磁贴文字按盒宽截断补省略号",
              {
    const float tw = (400.0f - 32.0f - 16.0f) / 3.0f;  // 窄窗三列每枚 ≈117
    const float boxW = tw - core::theme::scaled(48.0f);
    DOCTEST_CHECK(boxW > 0.0f);
    const std::string cut = memory::truncateByAdvance("命中 5 次 → 候选", boxW,
                                                      core::theme::scaled(11.0f));
    DOCTEST_CHECK_MESSAGE(cut.find("\xE2\x80\xA6") != std::string::npos ||
                              cut == "命中 5 次 → 候选",
                          "磁贴文字要么原样要么带省略号，禁硬裁");
    return true;
});

// ── M11 配置组头摘要省略 ──

REGISTER_TEST(memory_narrow_group_sum_ellipsis,
              "memory need298 M11: 组头摘要窄窗截断且右缘不越 chevron",
              {
    for (float w : {400.0f, 600.0f, 1000.0f}) {
        const float sumW = memory::groupSumW(w);
        DOCTEST_CHECK(sumW > 0.0f);
        DOCTEST_CHECK(240.0f + sumW <= w - 40.0f);  // chevron x = w−40
    }
    const std::string cut =
        memory::truncateByAdvance("日 50 万 · 月软 300 万 / 硬 500 万",
                                  memory::groupSumW(400.0f),
                                  core::theme::scaled(12.0f));
    DOCTEST_CHECK(cut.find("\xE2\x80\xA6") != std::string::npos);
    return true;
});

// ── M13 提案卡摘要窄窗档 ──

REGISTER_TEST(memory_narrow_proposal_meta_narrow_tier,
              "memory need298 M13: <560 摘要 190→120，标题预算得让位",
              {
    DOCTEST_CHECK(approx(memory::proposalMetaW(560.0f), 190.0f));
    DOCTEST_CHECK(approx(memory::proposalMetaW(559.0f), 120.0f));
    // 标题预算随让位增加（hx=16 非批量、kindW=44 口径），窄窗仍有 200px+
    const float budgetNarrow =
        460.0f - 16.0f - 44.0f - memory::proposalMetaW(460.0f) - 20.0f;
    DOCTEST_CHECK(budgetNarrow > 200.0f);
    return true;
});

// ── M16 chips 估宽四字节双宽 ──

REGISTER_TEST(memory_narrow_chip_width_four_byte_wide,
              "memory need298 M16: emoji 四字节计 24，CJK 12，ASCII 6",
              {
    DOCTEST_CHECK(approx(memory::chipTextW("ab", 24.0f), 36.0f));
    DOCTEST_CHECK(approx(memory::chipTextW("待审", 24.0f), 48.0f));
    DOCTEST_CHECK(approx(memory::chipTextW("\xF0\x9F\x98\x80", 24.0f),
                         48.0f));  // U+1F600 双宽
    DOCTEST_CHECK(approx(memory::chipTextW("\xF0\x9F\x98\x80", 30.0f),
                         54.0f));  // wrap 估口径 base 30
    return true;
});

// ── M18 消耗带标题不越预算档词 ──

REGISTER_TEST(memory_narrow_usage_title_clears_phase_word,
              "memory need298 M18: 标题宽收至预算档词（x+w−240）左缘，负宽钳 0",
              {
    DOCTEST_CHECK(approx(memory::usageTitleW(1200.0f), 320.0f));
    DOCTEST_CHECK(approx(memory::usageTitleW(560.0f), 304.0f));
    for (float w : {300.0f, 560.0f, 1200.0f}) {
        const float titleRight = 16.0f + memory::usageTitleW(w);
        DOCTEST_CHECK_MESSAGE(titleRight <= w - 240.0f + 0.5f,
                              "标题右缘不得越预算档词左缘 w=" +
                                  std::to_string(w));
    }
    DOCTEST_CHECK(approx(memory::usageTitleW(200.0f), 0.0f));
    return true;
});
