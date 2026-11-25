// tests/test_modules/test_font_em_layout.cpp
// em 字号语义复核 + 各模块固定尺寸容器的文本容纳复核（横向按真实字体测量，
// 纵向按字形纵向范围 / ascent 与 em 的保守比值做算术比较）。
//
// 跨文件前提：布局常量（卡高 / chip 高 / itemH / 侧边栏 label 宽）镜像自各模块源码，
// 改源码布局必须同步改这里，否则这组断言会对着旧常量恒绿：
//   - home_module.cpp drawSolidStatCard：pad=20、labelH=16、numGap=4、numberH=34、
//     卡高 96；指标名 14（fontSizeBody）、数字 30（fontSizeStat）、说明并入同行 14。
//     卡网格 gap 8、单卡最小宽 180、最多 5 列（homeGridColumns 响应式）：
//     contentW=窗宽-侧栏 220-页内边距 64；cw=卡宽-2×20。默认窗 1600×1000 → 5 列
//     卡宽 256.8 / cw 216.8；最小窗 1000×640 → 3 列卡宽 233.3 / cw 193.3。
//     行预算：指标名/数字按 cw，合并说明（失败态）按整卡宽（池489）。
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
    const float labelSize = t.sizes.fontSizeBody;   // 14，指标名 + 合并的说明同字号
    const float numberSize = t.sizes.fontSizeStat;  // 30

    // 横向预算 = 运行时真实网格（need268 响应式列数，镜像 home_module.cpp）：
    // 统计卡恒 5 张；卡网格 gap 8 / 单卡最小宽 180 / 最多 5 列；contentW = 窗宽 −
    // 侧栏 220 − 页内边距 32×2；卡内边距 pad 20 → 内容宽 cw = 卡宽 − 40。
    // 默认窗 1600 → 5 列：卡宽 256.8、cw 216.8；最小窗 1000 → 3 列：卡宽 233.3、cw 193.3。
    // 量宽留 12px 余量（measureTextSize 与最终排版 1~2% 差），与运行时同口径。
    constexpr float kSlack = 12.0f;
    constexpr float kGridGap = 8.0f;
    constexpr float kCardMinW = 180.0f;
    constexpr int kMaxCols = 5;
    constexpr float kWindowDefault = 1600.0f;
    constexpr float kWindowMin = 1000.0f;
    auto contentWidth = [](float windowW) { return windowW - 220.0f - 64.0f; };
    auto cardWidth = [](float availableW) {
        for (int cols = kMaxCols; cols > 1; --cols) {
            const float cardW =
                (availableW - kGridGap * static_cast<float>(cols - 1)) /
                static_cast<float>(cols);
            if (cardW >= kCardMinW) {
                return cardW;
            }
        }
        return availableW;  // 窄窗单列兜底
    };
    const float cardWDefault = cardWidth(contentWidth(kWindowDefault));  // 256.8
    const float cwDefault = cardWDefault - 40.0f;                        // 216.8
    const float cwMin = cardWidth(contentWidth(kWindowMin)) - 40.0f;     // 193.3

    const std::string label = "Token 速率 (tok/s)";
    const std::string merged = label + " · 加载失败";
    const std::string badge = "加载失败";
    const float wLabel = textW(label, labelSize);
    const float wNumber = textW("999.9", numberSize, 700);
    const float wMerged = textW(merged, labelSize);
    const float wFail = textW(badge, labelSize);
    // 默认窗：指标名 / 数字走内容宽预算；合并文案（失败态）走整卡宽预算——
    // 运行时 drawSolidStatCard 的行预算=整卡宽（含内边距区，池489）。
    DOCTEST_CHECK(wLabel + kSlack <= cwDefault);
    DOCTEST_CHECK(wNumber + kSlack <= cwDefault);
    DOCTEST_CHECK(wMerged + kSlack <= cardWDefault);
    // 最小窗：合并文案放不下是既定事实（运行时退化为只显 badge，见 drawSolidStatCard）。
    // 底线 = 退化后的「加载失败」仍能整段放下——失败必须显形。
    DOCTEST_CHECK(wFail + kSlack <= cwMin);

    // CJK 宽字体压力档：本机/中文环境实测 SimSun/SimHei 的 Latin 是全角宽档，
    // 合并文案（14px=210）明显宽于产品默认栈（Segoe UI + YaHei 回退，179.9）。
    // 预算必须按最坏档校验，否则宽字体环境把「加载失败」挤进省略号（池 489）。
    // clean Windows 无这些字体文件时量宽落系统回退，数值仍在校验预算内（CI 安全）。
    for (const char* fam : {"Microsoft YaHei", "SimHei", "SimSun"}) {
        if (core::TextPrimitive::resolveFontPath(fam).empty()) {
            continue;
        }
        const float wl = core::TextPrimitive::measureTextWidth(label, fam, labelSize);
        const float wm = core::TextPrimitive::measureTextWidth(merged, fam, labelSize);
        DOCTEST_CHECK_MESSAGE(wl + kSlack <= cwDefault,
                              std::string(fam) + " label=" + std::to_string(wl));
        DOCTEST_CHECK_MESSAGE(wm + kSlack <= cardWDefault,
                              std::string(fam) + " merged=" + std::to_string(wm));
    }

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
