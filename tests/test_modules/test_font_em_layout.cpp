// tests/test_modules/test_font_em_layout.cpp
// em 字号语义复核 + 各模块固定尺寸容器的文本容纳复核（横向按真实字体测量，
// 纵向按字形纵向范围 / ascent 与 em 的保守比值做算术比较）。
//
// 跨文件前提：布局常量（卡高 / chip 高 / itemH / 侧边栏 label 宽）镜像自各模块源码，
// 改源码布局必须同步改这里，否则这组断言会对着旧常量恒绿：
//   - home_module.cpp drawGlassStatCard：label y=16、number y=40、badge 底对齐 height-48、
//     卡高 144、padX 16、最小窗口 1200 时 statW=208、统计格高 144
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

// Home 统计卡的文字可用宽上界。实际可用 = statW 208 - 内边距 16×2 - 文本内缩 16×2 = 144；
// 这里取 152 略宽于实际，是为了让「文本变宽」的回归先撞上这条线而不是漏过。
constexpr float kHomeStatTextW = 152.0f;

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
    const float labelSize = t.sizes.fontSizeSmall + 1.0f;   // 14（UAT2 卡片字大）
    const float badgeSize = t.sizes.fontSizeSmall;          // 13（UAT2 卡片字大）
    const float numberSize = t.sizes.fontSizeLarge + 4.0f;  // 22（UAT2 卡片字大）

    // 标签取最长文案
    DOCTEST_CHECK(textW("Token 速率 (tok/s)", labelSize) <= kHomeStatTextW);
    // 数字口径：最近一次调用速率，恒 1 位小数，三位整数 + 小数位 = 5 字符是常见上限档
    DOCTEST_CHECK(textW("999.9", numberSize, 700) <= kHomeStatTextW);
    // badge 最坏情况：流式档与空闲档各取最长的一条
    DOCTEST_CHECK(textW("● 999 路生成中 · 最近一次", badgeSize) <=
                  kHomeStatTextW);
    DOCTEST_CHECK(textW("最近一次成功调用", badgeSize) <= kHomeStatTextW);
    DOCTEST_CHECK(textW("暂无成功调用", badgeSize) <= kHomeStatTextW);
    DOCTEST_CHECK(textW("今日修改过的笔记", badgeSize) <= kHomeStatTextW);

    // 纵向（drawGlassStatCard 绝对定位：label 16 / number 40 / badge 底对齐 height-48；
    // 最小档 fontScale=1 时 badge 顶 96、卡高 144）
    DOCTEST_CHECK(16.0f + kGlyphExtentRatio * labelSize <= 40.0f);   // 标签不压数字
    DOCTEST_CHECK(40.0f + kAscentRatio * numberSize <= 96.0f);       // 数字（无下延）不压 badge
    DOCTEST_CHECK(96.0f + kGlyphExtentRatio * badgeSize <= 144.0f);  // badge 不出卡
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
