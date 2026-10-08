// tests/test_modules/test_font_em_layout.cpp
// W9 字号 em 语义修复（EUI-NEO 0.7.0 fix/font-em-size）正式采用的布局复核：
//   1) em 语义确实生效——同文本宽度随 fontSize 线性且不缩水（旧 bug 下
//      Segoe UI 会被 hhea 度量缩 ~0.75）
//   2) 修复后 100% 缩放（文字比历史渲染大 ~33%）下，各模块固定尺寸容器
//      不被文本撑爆：横向用 core::TextPrimitive::measureTextWidth 以真实
//      字体实测；纵向用字形纵向范围比 r（(ascender-descender)/upem，
//      Segoe UI ≈ 1.331，取保守 1.35；纯数字无下延，ascent 比 a ≈ 1.08，
//      取保守 1.10）对布局常量做算术复核
//
// 布局常量镜像自各模块源码（改动源码时同步这里）：
//   - home_module.cpp drawStatCard：label y=16、number y=40 lineHeight 56、
//     badge 底对齐（height-48）、卡高 144、padX 16、最小窗口 1200 时 statW=208
//     （2026-09-28 内边距系统性修复：16 四周统一，替代旧 20 inset + 20/42/98
//     坐标叠加的「顶留空/底溢出」）
//   - todo_module.cpp drawChip：chip 高 22、字号 fontSizeSmall-1（UI 整改 18
//     由 18 / fontSizeSmall-3 调大）；drawCard：title y=14 lineHeight 18、
//     chip y=44、卡高 84（todo_ui_logic.h）
//   - app_sidebar.cpp 侧边栏：itemH 40、label 宽 200、brand 32px
#include "doctest.h"

#include <cmath>
#include <string>

#include "core/render/text.h"  // EUI-NEO（与 DevDesk 同命名空间 core）
#include "core/theme.h"
#include "module_todo/todo_ui_logic.h"

namespace {

// 字形纵向范围（含上下延）与 em 的比，保守值；Segoe UI 实际 1.331
constexpr float kGlyphExtentRatio = 1.35f;
// 数字/大写字母（无下延）ascent 与 em 的比，保守值；Segoe UI 实际 1.079
constexpr float kAscentRatio = 1.10f;

// 最小窗口 1200（spec §6.5.2）下 Home 统计卡的文字可用宽。
// 保守上界：实际可用 = statW 208 - 内边距 16×2 - 文本内缩 16×2 = 144；
// 152（旧 padX 28×2 口径）恒 ≥ 实际，防文本变宽回归先放宽。
constexpr float kHomeStatTextW = 152.0f;

float textW(const std::string& s, float size, int weight = 400) {
    return core::TextPrimitive::measureTextWidth(s, "", size, weight);
}

} // namespace

// ---- 1. em 语义生效（修复回归即挂：缩水 ~0.75 时 10 位数字宽 ~402 < 500） ----
REGISTER_TEST(font_em_semantics_active, "font-em: measured width follows CSS em size (no shrink)", {
    const float w10 = textW("0123456789", 10.0f);
    const float w100 = textW("0123456789", 100.0f);
    // Segoe UI 数字宽 ≈ 0.536em/位 → 100px 下 ≈ 536；缩水 bug 下 ≈ 402
    DOCTEST_CHECK(w100 >= 500.0f);
    // em 尺寸线性：100px 宽 ≈ 10 × 10px 宽
    DOCTEST_CHECK(std::fabs(w100 - w10 * 10.0f) <= 2.0f);
    return true;
})

// ---- 2. Home 统计卡（重点：48px 数字 + 11px 长 badge，最小窗口宽 152） ----
REGISTER_TEST(home_stat_card_text_fits, "font-em: home stat card text fits fixed card", {
    const auto& t = core::theme::current();
    core::theme::setFontScale(1.0f);  // 100% 默认，测完恢复
    const float labelSize = t.sizes.fontSizeSmall - 1.0f;   // 12
    const float badgeSize = t.sizes.fontSizeSmall - 2.0f;   // 11
    const float numberSize = t.sizes.fontSizeDisplay;       // 48

    // 标签（最长："Token 速率 (tok/s)"）
    DOCTEST_CHECK(textW("Token 速率 (tok/s)", labelSize) <= kHomeStatTextW);
    // 数字（① 2026-09-29 改口径：最近一次调用速率，恒 1 位小数；
    // 三位整数 + 小数位 = 5 字符是常见上限档；700 字重）
    DOCTEST_CHECK(textW("999.9", numberSize, 700) <= kHomeStatTextW);
    // badge 最坏情况（① 后不再有「累计」字样）：
    // 流式档「● 999 路生成中 · 最近一次」/ 空闲档「最近一次成功调用」
    DOCTEST_CHECK(textW("● 999 路生成中 · 最近一次", badgeSize) <=
                  kHomeStatTextW);
    DOCTEST_CHECK(textW("最近一次成功调用", badgeSize) <= kHomeStatTextW);
    DOCTEST_CHECK(textW("暂无成功调用", badgeSize) <= kHomeStatTextW);
    DOCTEST_CHECK(textW("今日修改过的笔记", badgeSize) <= kHomeStatTextW);

    // 纵向（drawStatCard 绝对定位：label 16 / number 40 / badge 底对齐
    // height-48；最小档 s=1 时 badge 顶 96 / 卡高 144）
    DOCTEST_CHECK(16.0f + kGlyphExtentRatio * labelSize <= 40.0f);   // 标签不压数字
    DOCTEST_CHECK(40.0f + kAscentRatio * numberSize <= 96.0f);       // 数字（无下延）不压 badge
    DOCTEST_CHECK(96.0f + kGlyphExtentRatio * badgeSize <= 144.0f);  // badge 不出卡
    return true;
})

// ---- 3. Todo 卡片 / 列头 / chip ----
REGISTER_TEST(todo_card_text_fits, "font-em: todo card & chips fit fixed heights", {
    const auto& t = core::theme::current();
    core::theme::setFontScale(1.0f);
    // UI 整改 18：chip 字号 10（fontSizeSmall-3）→ 12（fontSizeSmall-1）
    const float chipFont = t.sizes.fontSizeSmall - 1.0f;  // 12

    // 截止 chip 最大宽 66（drawChip 内容水平居中，超宽即视觉外溢）；
    // chip 内可用文字宽 = 66 - 20（估宽基数 20 = 左右各 10px 留白）
    DOCTEST_CHECK(textW("12-31", chipFont) <= 66.0f - 20.0f);
    // 标签 chip：宽由 chipWidth 估值决定 → 实测字形 + 两侧留白必须放得进估值
    // （字号调大而估值没跟着放大 = 裁字；估值口径见 todo_ui_logic.h）
    DOCTEST_CHECK(textW("重要", chipFont, 600) + 2.0f * 5.0f <=
                  module::todo::ui::chipWidth("重要"));
    DOCTEST_CHECK(textW("前端重构", chipFont, 600) + 2.0f * 5.0f <=
                  module::todo::ui::chipWidth("前端重构"));
    DOCTEST_CHECK(textW("frontend", chipFont, 600) + 2.0f * 5.0f <=
                  module::todo::ui::chipWidth("frontend"));
    // 详情弹窗优先级按钮（宽 100、字号 fontSizeSmall-1）文案「●高」放得下
    DOCTEST_CHECK(textW("●高", chipFont, 600) <= 100.0f);

    // 纵向（drawCard：title y=14 lineHeight 18、chip y=44；卡高 84）
    DOCTEST_CHECK(14.0f + kGlyphExtentRatio * (t.sizes.fontSizeBody - 1.0f) <= 44.0f);
    // chip 高 22（UI 整改 18：原 18）：字形纵向范围 ≤ chip 高 = 文字不被裁
    DOCTEST_CHECK(kGlyphExtentRatio * chipFont <= 22.0f);
    DOCTEST_CHECK(44.0f + 22.0f <= module::todo::ui::kCardHeight);
    // 列头（title y=16，高 56）
    DOCTEST_CHECK(16.0f + kGlyphExtentRatio * t.sizes.fontSizeBody <=
                  module::todo::ui::kColumnHeaderHeight);
    return true;
})

// ---- 4. 侧边栏（app_sidebar.cpp：itemH 40、label 宽 200、brand 32px） ----
REGISTER_TEST(sidebar_text_fits, "font-em: sidebar labels fit item height/width", {
    const auto& t = core::theme::current();
    core::theme::setFontScale(1.0f);
    // 最长显示名（Router 模块名）
    DOCTEST_CHECK(textW("Markdown 笔记", t.sizes.fontSizeBody) <= 200.0f);
    DOCTEST_CHECK(textW("Claude 代理", t.sizes.fontSizeBody) <= 200.0f);
    DOCTEST_CHECK(textW("DevDesk", t.sizes.fontSizePage, 700) <= 228.0f);
    // itemH 40 居中：整字形高 ≤ 40
    DOCTEST_CHECK(kGlyphExtentRatio * t.sizes.fontSizeBody <= 40.0f);
    return true;
})
