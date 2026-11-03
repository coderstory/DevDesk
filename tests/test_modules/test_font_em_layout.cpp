// tests/test_modules/test_font_em_layout.cpp
// em 字号语义复核 + 各模块固定尺寸容器的文本容纳复核（横向按真实字体测量，
// 纵向按字形纵向范围 / ascent 与 em 的保守比值做算术比较）。
//
// 跨文件前提：布局常量（卡高 / chip 高 / itemH / 侧边栏 label 宽）镜像自各模块源码，
// 改源码布局必须同步改这里，否则这组断言会对着旧常量恒绿：
//   - home_module.cpp drawSolidStatCard：pad=20、labelH=16、numGap=4、numberH=34、
//     卡高 96；指标名 14（fontSizeBody）、数字 30（fontSizeStat）、说明并入同行 14。
//     5 卡等分：卡宽=(contentW-4×20)/5，cw=卡宽-2×20；contentW=窗宽-侧栏 220-页 padding 64。
//     默认窗 1600×1000 → cw≈207.2；最小窗 1000×640 → cw≈87.2（主力程序 main.cpp:767）
//   - todo_module.cpp drawChip：chip 高 22、字号 fontSizeSmall-1；
//     drawCard：title y=14、chip y=44、卡高 kCardHeight、列头 title y=16
//   - app_sidebar.cpp：itemH 40、label 宽 200、brand 56px（UAT5 logo 48 行高 56）
#include "doctest.h"

#include <cmath>
#include <string>

#include "core/render/text.h"  // EUI-NEO（与 DevDesk 同命名空间 core）
#include "core/theme.h"
#include "module_todo/todo_ui_logic.h"

namespace {

// 字形纵向范围（含上下延）/ em 的保守上界，Segoe UI 实际 1.331
constexpr float kGlyphExtentRatio = 1.35f;
// 纯数字/大写字母（无下延）ascent / em 的保守上界，Segoe UI 实际 1.079
constexpr float kAscentRatio = 1.10f;

float textW(const std::string& s, float size, int weight = 400) {
    return core::TextPrimitive::measureTextWidth(s, "", size, weight);
}

} // namespace

REGISTER_TEST(font_em_semantics_active, "font-em: measured width follows CSS em size (no shrink)", {
    const float w10 = textW("0123456789", 10.0f);
    const float w100 = textW("0123456789", 100.0f);
    // 阈值 500 的来历：Segoe UI 数字宽 ≈ 0.536em/位 → 100px 下 ≈ 536；
    // 若 measureTextWidth 又被 hhea 度量缩了 ~0.75，就只剩 ≈ 402 而本条转红。
    DOCTEST_CHECK(w100 >= 500.0f);
    // em 尺寸线性：100px 宽 ≈ 10 × 10px 宽
    DOCTEST_CHECK(std::fabs(w100 - w10 * 10.0f) <= 2.0f);
    return true;
})

REGISTER_TEST(home_stat_card_text_fits, "font-em: home stat card text fits fixed card", {
    const auto& t = core::theme::current();
    core::theme::setFontScale(1.0f);  // 100% 是本文件的复核基准；测完由后续用例各自复位
    // 字号 = drawSolidStatCard 真实渲染尺寸（旧用例按 12/13px 量宽、预算 152，
    // 是卡高 144 时代的错位常量——真机早就不显形、用例仍绿，池117 实证）。
    const float labelSize = t.sizes.fontSizeBody;   // 14，指标名 + 合并的说明同字号
    const float numberSize = t.sizes.fontSizeStat;  // 30

    // 水平预算（与运行时 measureTextSize + 12px 余量同口径）：
    constexpr float kHomeStatTextWDefault = 207.2f - 12.0f;  // 默认窗 cw
    constexpr float kHomeStatTextWMin = 87.2f - 12.0f;       // 最小窗 cw
    const float wLabel = textW("Token 速率 (tok/s)", labelSize);
    const float wNumber = textW("999.9", numberSize, 700);
    const float wMerged = textW("Token 速率 (tok/s) · 加载失败", labelSize);
    const float wFail = textW("加载失败", labelSize);
    // 默认窗：指标名 / 数字 / 合并说明三条都必须整段放下（正常运行态与失败态全覆盖）
    DOCTEST_CHECK(wLabel <= kHomeStatTextWDefault);
    DOCTEST_CHECK(wNumber <= kHomeStatTextWDefault);
    DOCTEST_CHECK(wMerged <= kHomeStatTextWDefault);
    // 最小窗：合并文案放不下是既定事实（运行时退化为只显 badge，见 drawSolidStatCard）。
    // 底线 = 退化后的「加载失败」仍能整段放下——失败必须显形。
    DOCTEST_CHECK(wFail <= kHomeStatTextWMin);

    // 纵向（drawSolidStatCard 绝对定位：textTop=(96-40-54)/2=1、numberY=21；
    // 卡高 96、numberH=34 → 数字盒 21..55）
    DOCTEST_CHECK(1.0f + kGlyphExtentRatio * labelSize <= 21.0f);    // 标签不压数字
    DOCTEST_CHECK(21.0f + kAscentRatio * numberSize <= 55.0f);       // 数字（无下延）不出行盒
    return true;
})

REGISTER_TEST(todo_card_text_fits, "font-em: todo card & chips fit fixed heights", {
    const auto& t = core::theme::current();
    core::theme::setFontScale(1.0f);
    const float chipFont = t.sizes.fontSizeSmall - 1.0f;  // 12

    // 截止 chip：内容水平居中，超出 chip 高 66 即视觉外溢；文字两侧各留 10px
    DOCTEST_CHECK(textW("12-31", chipFont) <= 66.0f - 20.0f);
    // 标签 chip 的宽度由 chipWidth 估值决定 → 实测字形 + 两侧留白必须放得进估值。
    // 字号调大而估值没跟着放大就表现为裁字。
    DOCTEST_CHECK(textW("重要", chipFont, 600) + 2.0f * 5.0f <=
                  module::todo::ui::chipWidth("重要"));
    DOCTEST_CHECK(textW("前端重构", chipFont, 600) + 2.0f * 5.0f <=
                  module::todo::ui::chipWidth("前端重构"));
    DOCTEST_CHECK(textW("frontend", chipFont, 600) + 2.0f * 5.0f <=
                  module::todo::ui::chipWidth("frontend"));
    // 详情弹窗优先级按钮（宽 100）
    DOCTEST_CHECK(textW("●高", chipFont, 600) <= 100.0f);

    // 纵向（drawCard：title y=14、chip y=44、卡高 kCardHeight）
    DOCTEST_CHECK(14.0f + kGlyphExtentRatio * (t.sizes.fontSizeBody - 1.0f) <= 44.0f);
    DOCTEST_CHECK(kGlyphExtentRatio * chipFont <= 22.0f);  // chip 高 22，字形不被裁
    DOCTEST_CHECK(44.0f + 22.0f <= module::todo::ui::kCardHeight);
    // 列头 title y=16
    DOCTEST_CHECK(16.0f + kGlyphExtentRatio * t.sizes.fontSizeBody <=
                  module::todo::ui::kColumnHeaderHeight);
    return true;
})

REGISTER_TEST(sidebar_text_fits, "font-em: sidebar labels fit item height/width", {
    const auto& t = core::theme::current();
    core::theme::setFontScale(1.0f);
    DOCTEST_CHECK(textW("Markdown 笔记", t.sizes.fontSizeBody) <= 200.0f);
    DOCTEST_CHECK(textW("Claude 代理", t.sizes.fontSizeBody) <= 200.0f);
    DOCTEST_CHECK(textW("DevDesk", t.sizes.fontSizePage, 700) <= 228.0f);
    // itemH 40 居中：整字形高 ≤ 40
    DOCTEST_CHECK(kGlyphExtentRatio * t.sizes.fontSizeBody <= 40.0f);
    return true;
})
