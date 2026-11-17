// src/module_git/git_views.cpp
// Git 页两栏视图 compose（need:291 重设计；设计文档 docs/design/2026-11-17-git管理器重设计.md）：
//   左卡=变更列表（320 可折叠，树/平铺双态+两组头+untracked lazy 行）；
//   右卡=diff 常驻主区（头部 44px 粘顶：徽标+路径+统计+差异/内容 tab）。
// 渲染纪律（clue#22）：所有行盒/命中矩形 = （行模型, 宽度）的纯函数，compose
// measure pass 每帧两跑幂等；零矩形缓存。
#include "git_views.h"

#include "git_colors.h"

#include <components/input.h>
#include <components/theme.h>
#include <components/tooltip.h>
#include <components/virtuallist.h>
#include <core/component_theme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace module::git {

namespace {

using core::theme::scaled;
using core::theme::Tokens;

// ---- 设计 §1/§4/§5 尺寸常量（字面量基准，经 scaled() 随全局字号缩放）----
constexpr float kTopBarH = 48.0f;
constexpr float kPageMargin = 12.0f;          // §1 页边距 12（双侧 24）
constexpr float kCardGap = 12.0f;             // §1 两卡间 gap 12 显式常量
constexpr float kNarrowBreakpoint = 1200.0f;  // §9：<1200 左卡 280/缩进 12/搜索 240
constexpr float kListCardW = 320.0f;          // §4.1 左卡定宽
constexpr float kListCardWNarrow = 280.0f;    // §9 窄窗档
constexpr float kListHeaderH = 36.0f;         // §4.2 列头（粘顶）
constexpr float kGroupHeaderH = 32.0f;        // §4.3 组头
constexpr float kRowSlotH = 32.0f;            // virtualList 定高槽：组头满槽、
                                              // 行画槽顶 28px（EUI 定高约束）
constexpr float kRowH = 28.0f;                // §4.4/§4.5 目录/文件行高
constexpr float kRightHeaderH = 44.0f;        // §5.2 diff 头部（粘顶）
constexpr float kIndentStep = 16.0f;          // §4.4 每级缩进（窄窗 12）
constexpr float kIndentStepNarrow = 12.0f;
constexpr float kIndentBase = 12.0f;
constexpr float kBadgeSize = 16.0f;
constexpr float kIconBtnSize = 24.0f;         // §4.2 列头图标钮
// §5.3/§5.4 代码面（等宽 13px、行高 22px；四列网格 折叠24｜旧48｜新48｜代码 flex）
constexpr float kCodeRowH = 22.0f;
constexpr float kDiffSlotH = 24.0f;           // diff virtualList 定高槽
constexpr float kContentGutterW = 48.0f;      // §5.4 内容行号列宽
constexpr float kDiffFoldW = 24.0f;           // §5.3 折叠符列宽
constexpr float kDiffNoW = 48.0f;             // §5.3 旧/新行号列宽
constexpr float kCodeGutterPad = 8.0f;        // 行号右 padding
constexpr float kCardRadius = 16.0f;          // §1.2 两卡 radius 16

// FontAwesome（设计 §11 映射）
constexpr unsigned kGlyphFolderTree = 0xF802;       // fa-folder-tree
constexpr unsigned kGlyphList = 0xF03A;             // fa-list
constexpr unsigned kGlyphCollapse = 0xF100;         // fa-angle-double-left
constexpr unsigned kGlyphFolder = 0xF07B;           // fa-folder
constexpr unsigned kGlyphFolderOpen = 0xF07C;       // fa-folder-open
constexpr unsigned kGlyphSpinner = 0xF021;          // fa-refresh（旋转圈降级 pulse）
constexpr unsigned kGlyphCircleCheck = 0xF058;      // fa-circle-check
constexpr unsigned kGlyphCodeBranch = 0xF126;       // fa-code-branch
constexpr unsigned kGlyphCircleExclaim = 0xF06A;    // fa-circle-exclamation
constexpr unsigned kGlyphCaretDown = 0xF107;        // fa-angle-down（切换仓库下拉 §11）
constexpr unsigned kGlyphCheck = 0xF00C;            // fa-check（当前仓标记 §3.1）
constexpr unsigned kGlyphXmark = 0xF00D;            // fa-xmark（过滤清除 × §11）

// 文本宽度估算（wtabTextWidth 同式：CJK 0.43 / ASCII 0.56 × 字号 + 1.6 基宽；
// 偏大只多留白，用于固定头内元素定位，不做逐字截断）
float textWidthEstimate(const std::string& text, float fontSize) {
    float w = fontSize * 1.6f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? fontSize * 0.43f : fontSize * 0.56f;
    }
    return w;
}

// 等宽代码宽估算（F-07⑥ 横滚内容宽；每显示列 0.62em。charLen=显示列数——
// 模型层非 ASCII 起始序列按 2 列（need:14 P1-1：mono 字体 CJK 实测 ~1.0em/字，
// 码点口径对纯 CJK 行欠 58%，120 字行实量 1560px vs 模型 988px 行尾不可达），
// 双格保守入模 → 恒偏大只多留白，横滚尾部空白不影响「不换行错位」验收）
float monoLineWidth(size_t charLen, float fontSize) {
    return charLen * fontSize * 0.62f + fontSize * 1.6f;
}

// 超长名单行省略号截断（need:299 §4.4/§4.5 钉死落地；bug#292 同款列宽预算：
// 每显示列 0.62em、非 ASCII 起始字节记 2 列——CJK 实测 ~1.0em 恒偏大安全侧；
// CJK 按码点/UTF-8 序列边界截断，禁切半个多字节字符）。返回带「…」的显示串，
// truncated=true 时调用方须挂 tooltip 全文。
std::pair<std::string, bool> ellipsisFit(const std::string& text, float fontSize,
                                         float budgetW) {
    if (textWidthEstimate(text, fontSize) <= budgetW) return {text, false};
    const float colW = fontSize * 0.62f;
    const float budget = budgetW - fontSize * 0.62f - fontSize * 0.4f;  // 留「…」+安全边
    float acc = 0.0f;
    size_t keep = 0;
    while (keep < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[keep]);
        const size_t len = (ch < 0x80) ? 1
                           : ((ch & 0xE0) == 0xC0) ? 2
                           : ((ch & 0xF0) == 0xE0) ? 3 : 4;
        if (keep + len > text.size()) break;  // 防御：残缺 UTF-8 尾
        const float wCols = (ch < 0x80) ? colW : colW * 2.0f;
        if (acc + wCols > budget) break;
        acc += wCols;
        keep += len;
    }
    if (keep == 0) return {std::string("…"), true};
    return {text.substr(0, keep) + "…", true};
}

// 骨架 pulse（UI-A §8：opacity 0.45↔1、1.2s ease-in-out alternate infinite；
// 三角波+半余弦缓动近似，全骨架同相位）
float skeletonPulseOpacity() {
    using namespace std::chrono;
    const double t =
        duration<double>(steady_clock::now().time_since_epoch()).count();
    const double phase = std::fmod(t, 1.2) / 1.2;
    const double tri = phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0;
    const double eased = 0.5 - 0.5 * std::cos(tri * 3.14159265358979323846);
    return static_cast<float>(0.45 + 0.55 * eased);
}

// 透明命中层 + 文本 + 可选底色的行内小元件（按钮/Tab 共用骨架）
void hitRect(eui::Ui& ui, const std::string& id, float x, float y, float w,
             float h, const std::function<void()>& onClick) {
    auto hit = ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f});
    if (onClick) hit.onClick(onClick);
    hit.build();
}

// 次按钮（§5.1：高 32、底 surfaceElevated、radius 6、无边框无阴影）
void secondaryButton(eui::Ui& ui, const std::string& id, const std::string& label,
                     float x, float y, float w, float h, const Tokens& t,
                     const std::function<void()>& onClick) {
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            ui.rect(id + ".bg")
                .size(w, h)
                .color(t.colors.surfaceElevated)
                .radius(t.sizes.radiusSmall)
                .build();
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(scaled(14.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
        })
        .build();
    hitRect(ui, id + ".hit", x, y, w, h, onClick);
}

// 主按钮（§5.7 空态页：绿填充白字，唯一主按钮；validating = disabled）
void primaryButton(eui::Ui& ui, const std::string& id, const std::string& label,
                   float x, float y, float w, float h, const Tokens& t,
                   const std::function<void()>& onClick, bool disabled) {
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            ui.rect(id + ".bg")
                .size(w, h)
                .color(disabled ? t.colors.surfaceElevated : t.colors.primary)
                .radius(t.sizes.radiusSmall)
                .build();
            ui.text(id + ".text")
                .size(w, h)
                .text(disabled ? "校验中…" : label)
                .fontSize(scaled(14.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(disabled ? t.colors.textTertiary : t.colors.onPrimary)
                .build();
        })
        .build();
    if (!disabled) {
        hitRect(ui, id + ".hit", x, y, w, h, onClick);
    }
}

// 骨架行（§5.8：底 borderMuted、radius 4 + pulse 动效 UI-A §8）
void skeletonRow(eui::Ui& ui, const std::string& id, float x, float y, float w,
                 float h, const Tokens& t) {
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(t.colors.borderMuted)
        .radius(4.0f)
        .opacity(skeletonPulseOpacity())
        // 骨架在屏期间持续出帧驱动 pulse（骨架卸下后元素消失，帧循环自然停）
        .onFrame([](float) {})
        .build();
}

// 下划线式 Tab（§5.2 视图切换：激活=主色 2px 底线；方案 A 明确 Tab 形态禁实心
// 按钮——ui_kit::wtab 为块面式，按方案 A 自绘下划线式）
void underlineTab(eui::Ui& ui, const std::string& id, const std::string& label,
                  bool active, float x, float y, float w, float h,
                  const Tokens& t, const std::function<void()>& onClick) {
    ui.stack(id)
        .position(x, y)
        .size(w, h)
        .content([&] {
            ui.text(id + ".text")
                .size(w, h)
                .text(label)
                .fontSize(scaled(13.0f))
                .fontWeight(active ? 500 : 400)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? t.colors.textPrimary : t.colors.textSecondary)
                .build();
            if (active) {
                ui.rect(id + ".underline")
                    .position(0.0f, h - 2.0f)
                    .size(w, 2.0f)
                    .color(t.colors.primary)
                    .build();
            }
        })
        .build();
    hitRect(ui, id + ".hit", x, y, w, h, onClick);
}

// 图标钮（§4.2 列头 segmented/折叠钮：24×24；激活=底 selected + 图标 text1，
// 未激活=底透明 + 图标 text3。EUI 无 hover 驱动原语，hover 档降级省略）
void iconButton(eui::Ui& ui, const std::string& id, unsigned glyph, bool active,
                float x, float y, float size, const Tokens& t,
                const std::function<void()>& onClick) {
    ui.stack(id)
        .position(x, y)
        .size(size, size)
        .content([&] {
            if (active) {
                ui.rect(id + ".bg")
                    .size(size, size)
                    .color(t.colors.active)
                    .radius(6.0f)
                    .build();
            }
            ui.text(id + ".glyph")
                .size(size, size)
                .icon(glyph)
                .fontSize(scaled(12.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(active ? t.colors.textPrimary : t.colors.textTertiary)
                .build();
        })
        .build();
    hitRect(ui, id + ".hit", x, y, size, size, onClick);
}

// ---- 空态页（§5.7 / v1 沿用）----
void composeEmptyPage(eui::Ui& ui, const eui::Screen& screen,
                      GitPageController& c, const Tokens& t) {
    const float w = screen.width;
    const float h = screen.height;
    ui.rect("git.empty.bg").size(w, h).color(t.colors.background).build();

    const float blockW = std::min(420.0f, w);
    const float blockX = (w - blockW) * 0.5f;
    float y = h * 0.32f;

    ui.text("git.empty.title")
        .position(blockX, y)
        .size(blockW, scaled(24.0f))
        .text("查看你的 Git 仓库")
        .fontSize(scaled(16.0f))
        .fontWeight(600)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textPrimary)
        .build();
    y += scaled(28.0f);
    ui.text("git.empty.desc")
        .position(blockX, y)
        .size(blockW, scaled(20.0f))
        .text("只读查看器：浏览变更与 diff，不做任何修改。")
        .fontSize(scaled(13.0f))
        .lineHeight(scaled(18.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
    y += scaled(30.0f);

    // 记忆仓失效 / 校验失败错误条（记忆失效带第二行清除说明）
    if (const auto& err = c.emptyError()) {
        const float barH =
            err->detail.empty() ? scaled(44.0f) : scaled(64.0f);
        ui.rect("git.empty.error")
            .position(blockX, y)
            .size(blockW, barH)
            .color(components::theme::withOpacity(t.colors.error, 0.12f))
            .radius(t.sizes.radiusSmall)
            .build();
        ui.text("git.empty.error.title")
            .position(blockX + scaled(16.0f), y + scaled(8.0f))
            .size(blockW - scaled(32.0f), scaled(20.0f))
            .text(err->title)
            .fontSize(scaled(13.0f))
            .color(t.colors.errorText)
            .build();
        if (!err->detail.empty()) {
            ui.text("git.empty.error.detail")
                .position(blockX + scaled(16.0f), y + scaled(30.0f))
                .size(blockW - scaled(32.0f), scaled(20.0f))
                .text(err->detail)
                .fontSize(scaled(12.0f))
                .color(t.colors.textTertiary)
                .build();
        }
        y += barH + scaled(16.0f);
    }

    const float btnW = 160.0f;
    primaryButton(ui, "git.empty.pick", "选择仓库目录",
                  blockX + (blockW - btnW) * 0.5f, y, btnW, scaled(36.0f), t,
                  [&c] { c.beginPickRepo(); }, c.validating());
}

// ---- 顶栏（§3 need:299 重排 48px：折叠展开钮(条件)+仓名+路径480+…+[切换仓库▾][刷新]；
//      全局搜索框已删除，归 #300）----
// 设计 §3 分支 chip：platform/git 无分支查询接口（数据面零改动硬约束），演绎省略。

// 切换仓库按钮右缘（need:299 §3.1：下拉右缘=按钮右缘对齐；按钮组=右锚两枚次钮）
float switchButtonRightEdge(float w) {
    const float cardW = w - kPageMargin * 2.0f;
    const float refreshW = textWidthEstimate("↻ 刷新", scaled(14.0f)) + scaled(24.0f);
    return kPageMargin + cardW - scaled(12.0f) - refreshW - scaled(12.0f);
}

void composeTopBar(eui::Ui& ui, float w, bool narrow, GitPageController& c,
                   const Tokens& t) {
    (void)narrow;  // 搜索框删除后窄窗档只剩路径宽度自适应（§9）
    // bug#297（老板令「一个圆角一个直角」）：顶栏改卡片形态与下方两卡同语言——
    // 白卡+1px 边+radius 16+页边 12；卡内距 scaled(12) 与两卡内容列统一对齐
    const float cardX = kPageMargin;
    const float cardW = w - kPageMargin * 2.0f;
    ui.rect("git.top.bg")
        .position(cardX, kPageMargin)
        .size(cardW, kTopBarH)
        .color(t.colors.surface)
        .radius(kCardRadius)
        .border(1.0f, t.colors.borderDefault)
        .build();

    const float inner = scaled(12.0f);  // bug#297：三卡统一卡内距（内容列对齐网格）
    float x = cardX + inner;
    // 折叠展开钮（§3：仅左栏折叠时显示，为顶栏第一个元素 32×32）
    if (c.listCollapsed()) {
        const float btnY = kPageMargin + (kTopBarH - scaled(32.0f)) * 0.5f;
        ui.stack("git.top.expand")
            .position(x, btnY)
            .size(scaled(32.0f), scaled(32.0f))
            .content([&] {
                ui.text("git.top.expand.glyph")
                    .size(scaled(32.0f), scaled(32.0f))
                    .icon(kGlyphList)
                    .fontSize(scaled(14.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .build();
            })
            .build();
        hitRect(ui, "git.top.expand.hit", x, btnY, scaled(32.0f), scaled(32.0f),
                [&c] { c.toggleListCollapsed(); });
        x += scaled(32.0f) + scaled(8.0f);
    }

    const std::string name = c.repoDisplayName();
    const float nameW = textWidthEstimate(name, scaled(15.0f));
    ui.text("git.top.name")
        .position(x, kPageMargin)
        .size(nameW + scaled(8.0f), kTopBarH)
        .text(name)
        .fontSize(scaled(15.0f))
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();

    // 完整路径（need:299：搜索框删除后空间富余，max 360→480；单行省略号+tooltip
    // 全路径——§3 禁硬裁切，ellipsisFit 码点预算截断）
    x += nameW + scaled(8.0f) + scaled(12.0f);
    const float switchRight = switchButtonRightEdge(w);
    const float pathMaxW = scaled(480.0f);
    const float pathW = std::min(
        pathMaxW, std::max(scaled(40.0f), switchRight - scaled(12.0f) - x));
    const auto [shownPath, pathTruncated] =
        ellipsisFit(c.repoPathUtf8(), scaled(12.0f), pathW);
    ui.text("git.top.path")
        .position(x, kPageMargin)
        .size(pathW, kTopBarH)
        .text(shownPath)
        .fontSize(scaled(12.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    if (pathTruncated) {
        components::tooltip(ui, "git.top.path.tip")
            .theme(core::theme::componentTokens(t))
            .source("git.top.path")
            .value(c.repoPathUtf8())
            .anchor(x + pathW * 0.5f, kPageMargin + kTopBarH * 0.5f)
            .bounds(pathW, kTopBarH)
            .minWidth(120.0f)
            .maxWidth(480.0f)
            .zIndex(40)
            .build();
    }

    // 右侧按钮组：[切换仓库 ▾] [↻ 刷新]（次按钮 32px 高；卡内右距=inner 统一网格；
    // 切换仓库=下拉触发器 need:299 §3.1，原「迁移空态页」动线由下拉「浏览目录…」承接）
    const float btnH = scaled(32.0f);
    const float btnY = kPageMargin + (kTopBarH - btnH) * 0.5f;
    const float switchW =
        textWidthEstimate("切换仓库", scaled(14.0f)) + scaled(24.0f) + scaled(14.0f);
    const float refreshW = textWidthEstimate("↻ 刷新", scaled(14.0f)) + scaled(24.0f);
    const float refreshX = cardX + cardW - inner - refreshW;
    const float switchX = refreshX - scaled(12.0f) - switchW;
    secondaryButton(ui, "git.top.refresh", "↻ 刷新", refreshX, btnY, refreshW, btnH,
                    t, [&c] { c.refresh(); });
    secondaryButton(ui, "git.top.switch", "切换仓库", switchX, btnY, switchW, btnH,
                    t, [&c] { c.toggleRepoDropdown(); });
    // 文案右侧 fa-angle-down 12px textSecondary（§3；纯装饰 pointer 穿透，命中归按钮）
    ui.text("git.top.switch.caret")
        .position(switchX + switchW - scaled(16.0f), btnY)
        .size(scaled(14.0f), btnH)
        .icon(kGlyphCaretDown)
        .fontSize(scaled(12.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .hitTestMode(::core::dsl::HitTestMode::None)
        .build();
}

// ---- 切换仓库历史下拉（need:299 §3.1/§6.10；临时浮层 z50，不改页面构图）----
void composeRepoDropdown(eui::Ui& ui, float w, float h, GitPageController& c,
                         const Tokens& t) {
    if (!c.repoDropdownVisible()) return;
    const auto& entries = c.recentRepoEntries();

    // 几何：宽 min(360, 页宽−24)；右缘=按钮右缘对齐，越左越界钳页边 12；
    // 上缘=顶栏卡下缘+4
    const float popupW = std::min(scaled(360.0f), w - scaled(24.0f));
    const float switchRight = switchButtonRightEdge(w);
    float px = switchRight - popupW;
    if (px < scaled(12.0f)) px = scaled(12.0f);
    const float py = kPageMargin + kTopBarH + scaled(4.0f);
    const float labelH = entries.empty() ? 0.0f : scaled(28.0f);
    const float browseH = scaled(32.0f);
    const float popupH = labelH + static_cast<float>(entries.size()) * scaled(40.0f) +
                         1.0f + browseH;

    // scrim（浮层外透明命中带；顶栏条不盖——切换仓库钮保持可点=toggle 收起）
    const float bandTop = kPageMargin + kTopBarH;
    hitRect(ui, "git.repo.dd.scrim.left", 0.0f, bandTop, std::max(0.0f, px),
            h - bandTop, [&c] { c.setRepoDropdownVisible(false); });
    hitRect(ui, "git.repo.dd.scrim.right", px + popupW, bandTop,
            std::max(0.0f, w - px - popupW), h - bandTop,
            [&c] { c.setRepoDropdownVisible(false); });
    hitRect(ui, "git.repo.dd.scrim.bottom", px, py + popupH,
            std::min(popupW, std::max(0.0f, w - px)),
            std::max(0.0f, h - py - popupH),
            [&c] { c.setRepoDropdownVisible(false); });

    ui.stack("git.repo.dd")
        .position(px, py)
        .size(popupW, popupH)
        .zIndex(50)
        .content([&] {
            ui.rect("git.repo.dd.bg")
                .size(popupW, popupH)
                .color(t.colors.surface)
                .radius(8.0f)
                .border(1.0f, t.colors.borderDefault)
                .shadow(core::Shadow{true, {0.0f, 6.0f}, 20.0f, 0.0f,
                                     eui::Color{0.0f, 0.0f, 0.0f, 0.08f}})
                .build();
            float y = 0.0f;
            if (labelH > 0.0f) {
                // 节标「最近仓库」（仅历史非空时显示）
                ui.text("git.repo.dd.label")
                    .position(scaled(12.0f), y)
                    .size(popupW - scaled(24.0f), labelH)
                    .text("最近仓库")
                    .fontSize(scaled(11.0f))
                    .fontWeight(500)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                y += labelH;
            }
            // 仓行（两行式：仓名 13/500 + 路径 11/16；当前仓 ✓；失效行置灰+「已失效」
            // 仍可点击重开 §6.10②）。LRU 上限 8 → 无内滚档（§3.1 >8 内滚不可达）
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                const float rowY = y;
                const std::string idx = std::to_string(i);
                const eui::Color nameColor =
                    e.exists ? t.colors.textPrimary : t.colors.textTertiary;
                const eui::Color pathColor = t.colors.textTertiary;
                ui.text("git.repo.dd.row." + idx + ".name")
                    .position(scaled(12.0f), rowY + scaled(5.0f))
                    .size(popupW - scaled(24.0f) - scaled(56.0f), scaled(18.0f))
                    .text(e.name)
                    .fontSize(scaled(13.0f))
                    .fontWeight(500)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(nameColor)
                    .build();
                const auto [shownRepoPath, repoPathTrunc] =
                    ellipsisFit(e.path, scaled(11.0f), popupW - scaled(24.0f) - scaled(56.0f));
                ui.text("git.repo.dd.row." + idx + ".path")
                    .position(scaled(12.0f), rowY + scaled(21.0f))
                    .size(popupW - scaled(24.0f) - scaled(56.0f), scaled(16.0f))
                    .text(shownRepoPath)
                    .fontSize(scaled(11.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(pathColor)
                    .build();
                if (repoPathTrunc || !e.exists) {
                    components::tooltip(ui, "git.repo.dd.row." + idx + ".tip")
                        .theme(core::theme::componentTokens(t))
                        .source("git.repo.dd.row." + idx + ".path")
                        .value(e.path + (e.exists ? std::string() : std::string("（已失效）")))
                        .anchor(scaled(12.0f) + (popupW - scaled(24.0f)) * 0.5f,
                                rowY + scaled(20.0f))
                        .bounds(popupW, scaled(40.0f))
                        .minWidth(120.0f)
                        .maxWidth(480.0f)
                        .zIndex(51)
                        .build();
                }
                if (e.current) {
                    // 当前仓标记：行右缘 fa-check 12px 主色（§3.1）
                    ui.text("git.repo.dd.row." + idx + ".check")
                        .position(popupW - scaled(28.0f), rowY)
                        .size(scaled(16.0f), scaled(40.0f))
                        .icon(kGlyphCheck)
                        .fontSize(scaled(12.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.primary)
                        .hitTestMode(::core::dsl::HitTestMode::None)
                        .build();
                } else if (!e.exists) {
                    // 失效仓行：右缘「已失效」11px danger-text（仍可点击=尝试重开）
                    ui.text("git.repo.dd.row." + idx + ".stale")
                        .position(popupW - scaled(64.0f), rowY)
                        .size(scaled(52.0f), scaled(40.0f))
                        .text("已失效")
                        .fontSize(scaled(11.0f))
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.errorText)
                        .hitTestMode(::core::dsl::HitTestMode::None)
                        .build();
                }
                hitRect(ui, "git.repo.dd.row." + idx + ".hit", 0.0f, rowY, popupW,
                        scaled(40.0f),
                        [&c, p = e.path] { c.openRecentRepo(p); });
                y += scaled(40.0f);
            }
            if (labelH > 0.0f) {
                // 分隔线（节标/仓行清单与「浏览目录…」之间）
                ui.rect("git.repo.dd.divider")
                    .position(0.0f, y)
                    .size(popupW, 1.0f)
                    .color(t.colors.borderMuted)
                    .build();
                y += 1.0f;
            }
            // 操作行「浏览目录…」=打开系统目录选择器（平台 API）
            ui.text("git.repo.dd.browse.icon")
                .position(scaled(12.0f), y)
                .size(scaled(16.0f), browseH)
                .icon(kGlyphFolderOpen)
                .fontSize(scaled(12.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .hitTestMode(::core::dsl::HitTestMode::None)
                .build();
            ui.text("git.repo.dd.browse")
                .position(scaled(34.0f), y)
                .size(popupW - scaled(46.0f), browseH)
                .text("浏览目录…")
                .fontSize(scaled(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
            hitRect(ui, "git.repo.dd.browse.hit", 0.0f, y, popupW, browseH, [&c] {
                c.setRepoDropdownVisible(false);
                c.beginPickRepo();
            });
        })
        .build();
}

// ---- 变更列表行渲染（§4.3~§4.6；行盒 = （行, contentWidth）纯函数）----
// expanded 派生：下一行 depth 更深 = 本目录已展开（行模型序列自证，免模型查询）
bool rowExpandedAt(const std::vector<ChangeListRow>& rows, size_t idx) {
    return idx + 1 < rows.size() && rows[idx + 1].depth > rows[idx].depth;
}

// 状态徽标 16×16（浅染底 + 状态字母；§4.5/§4.6）
void drawBadge(eui::Ui& ui, const std::string& id, float x, float y,
               platform::git::StatusKind kind, const Tokens& t,
               const GitColors& gc) {
    eui::Color bg = t.colors.surfaceElevated;
    eui::Color fg = t.colors.textSecondary;
    switch (kind) {
        case platform::git::StatusKind::New: bg = gc.addedBg; fg = gc.added; break;
        case platform::git::StatusKind::Modified: bg = gc.modifiedBg; fg = gc.modified; break;
        case platform::git::StatusKind::Deleted: bg = gc.deletedBg; fg = gc.deleted; break;
        case platform::git::StatusKind::Renamed: bg = gc.renamedBg; fg = gc.renamed; break;
        case platform::git::StatusKind::Untracked: bg = gc.untrackedBg; fg = gc.untracked; break;
        default: break;
    }
    ui.rect(id)
        .position(x, y)
        .size(scaled(kBadgeSize), scaled(kBadgeSize))
        .color(bg)
        .radius(3.0f)
        .build();
    ui.text(id + ".text")
        .position(x, y)
        .size(scaled(kBadgeSize), scaled(kBadgeSize))
        .text(std::string(1, statusBadgeLetter(kind)))
        .fontSize(scaled(10.0f))
        .fontWeight(700)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(fg)
        .build();
}

// 目录行 / 文件行 / 加载 / 截断尾行 / 错误行（§4.3~§4.6 + §6.2 边界）
// indentStepPx：缩进步长（窄窗档 12、常规 16，§9——compose 侧换算传入）
void drawChangeListRow(eui::Ui& ui, const std::string& rowId,
                       const ChangeListRow& row, size_t idx, bool flat,
                       float indentStepPx, float contentWidth,
                       GitPageController& c, const Tokens& t, const GitColors& gc) {
    const float slotH = scaled(kRowSlotH);
    const float rowH = scaled(kRowH);
    const float indentStep = indentStepPx;

    ui.stack(rowId)
        .size(contentWidth, slotH)
        .content([&] {
            // 深嵌套缩进钳制（need:299 §4.4 钉死）：名区最小 96px——indent =
            // min(基值+步进×depth, 行宽−12(右距)−16(徽标位)−8(gap)−96)；超限后更
            // 深层级不再增加缩进（视觉同位，层级靠 tooltip 全路径区分），禁横向溢出
            const float indentClamp = std::max(
                scaled(kIndentBase),
                contentWidth - scaled(12.0f + 16.0f + 8.0f + 96.0f));
            const float indent =
                std::min(scaled(kIndentBase) + indentStep * row.depth, indentClamp);

            if (row.kind == ChangeListRow::Kind::GroupHeader) {
                // 组头 32px（§4.3：箭头 + 「已暂存 (N)」13/600；点击折叠）
                const bool collapsed = c.listModel().isGroupCollapsed(row.group);
                ui.rect(rowId + ".bg")
                    .size(contentWidth, scaled(kGroupHeaderH))
                    .color(t.colors.surface)
                    .build();
                ui.rect(rowId + ".border")
                    .position(0.0f, scaled(kGroupHeaderH))
                    .size(contentWidth, 1.0f)
                    .color(t.colors.borderMuted)
                    .build();
                ui.text(rowId + ".arrow")
                    .position(scaled(12.0f), 0.0f)
                    .size(scaled(14.0f), scaled(kGroupHeaderH))
                    .text(collapsed ? "▶" : "▼")
                    .fontSize(scaled(10.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                ui.text(rowId + ".text")
                    .position(scaled(28.0f), 0.0f)
                    .size(contentWidth - scaled(40.0f), scaled(kGroupHeaderH))
                    .text(row.name)  // 「已暂存 (N)」模型烘焙（计数=组内文件行数）
                    .fontSize(scaled(13.0f))
                    .fontWeight(600)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textPrimary)
                    .build();
                hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth,
                        scaled(kGroupHeaderH),
                        [&c, g = row.group] { c.toggleGroup(g); });
                return;
            }

            // 行体统一画在槽顶 kRowH 内（组头外全部 28px 行）
            if (row.kind == ChangeListRow::Kind::Dir) {
                const bool expanded = rowExpandedAt(c.listRows(), idx);
                // 文件夹原则令（2026-11-17）：目录行=纯结构装置——不带状态徽标，
                // childCount=子树文件行数（纯结构信息）；不可选中、不进 diff（D3-A）
                float x = indent;
                const float arrowW = scaled(14.0f);
                ui.text(rowId + ".arrow")
                    .position(x, 0.0f)
                    .size(arrowW, rowH)
                    .text(expanded ? "▼" : "▶")
                    .fontSize(scaled(10.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                x += arrowW;
                ui.text(rowId + ".icon")
                    .position(x, 0.0f)
                    .size(scaled(16.0f), rowH)
                    .icon(expanded ? kGlyphFolderOpen : kGlyphFolder)
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                x += scaled(18.0f);
                // 目录名（need:299：单行省略号字符截断，禁硬裁切——ellipsisFit
                // 码点预算；右界=计数左缘−8，徽标位已随原则令移除）
                const bool hasCount = row.childCount > 0;
                const float nameW = std::max(
                    scaled(20.0f),
                    contentWidth - x - scaled(12.0f) - (hasCount ? scaled(36.0f) : 0.0f));
                const auto [shownName, nameTruncated] =
                    ellipsisFit(row.name, scaled(13.0f), nameW);
                ui.text(rowId + ".name")
                    .position(x, 0.0f)
                    .size(nameW, rowH)
                    .text(shownName)
                    .fontSize(scaled(13.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textPrimary)
                    .build();
                // 子树文件行计数（§4.4：右锚 12px；lazy 未枚举=0 不显）
                if (hasCount) {
                    ui.text(rowId + ".count")
                        .position(contentWidth - scaled(12.0f) - scaled(32.0f), 0.0f)
                        .size(scaled(32.0f), rowH)
                        .text(std::to_string(row.childCount))
                        .fontSize(scaled(12.0f))
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                }
                // 省略发生时 tooltip 全路径（need:299 §4.4）
                if (nameTruncated) {
                    components::tooltip(ui, rowId + ".name.tip")
                        .theme(core::theme::componentTokens(t))
                        .source(rowId + ".name")
                        .value(row.dirRel.empty() ? row.relPath : row.dirRel)
                        .anchor(x + nameW * 0.5f, rowH * 0.5f)
                        .bounds(contentWidth, rowH)
                        .minWidth(120.0f)
                        .maxWidth(420.0f)
                        .zIndex(40)
                        .build();
                }
                // D3-A：点击=展开/收起，永不进 diff（lazy 派生目录=lazy 枚举触发点）
                const std::string rel = row.relPath;
                if (row.lazy) {
                    hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, rowH,
                            [&c, rel] { c.toggleUntrackedDir(rel); });
                } else {
                    hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, rowH,
                            [&c, rel] { c.toggleDir(rel); });
                }
                return;
            }

            if (row.kind == ChangeListRow::Kind::Loading) {
                // §4.4 加载行：旋转圈（pulse 降级）+「正在读取…」（>1s 阈值由
                // worker 实际耗时天然满足：常态 <20ms 不出 Loading 帧）
                float x = indent + scaled(14.0f);
                ui.text(rowId + ".spin")
                    .position(x, 0.0f)
                    .size(scaled(16.0f), rowH)
                    .icon(kGlyphSpinner)
                    .fontSize(scaled(12.0f))
                    .opacity(skeletonPulseOpacity())
                    .onFrame([](float) {})
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                x += scaled(20.0f);
                ui.text(rowId + ".text")
                    .position(x, 0.0f)
                    .size(contentWidth - x - scaled(12.0f), rowH)
                    .text("正在读取…")
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }

            if (row.kind == ChangeListRow::Kind::Truncate) {
                // §6.2：>500 截断尾行「还有 N 个文件」（need:299：搜索引导随 #300 删）
                float x = indent + scaled(14.0f);
                ui.text(rowId + ".text")
                    .position(x, 0.0f)
                    .size(contentWidth - x - scaled(12.0f), rowH)
                    .text("还有 " + std::to_string(row.remainCount) + " 个文件")
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }

            if (row.kind == ChangeListRow::Kind::Error) {
                // §6.2：枚举失败子区错误行 + 重试（目录保持展开）
                float x = indent + scaled(14.0f);
                ui.text(rowId + ".err")
                    .position(x, 0.0f)
                    .size(contentWidth - x - scaled(60.0f), rowH)
                    .text("无法读取目录：" + row.errorText)
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.errorText)
                    .build();
                const float retryW = textWidthEstimate("重试", scaled(12.0f)) + scaled(8.0f);
                ui.text(rowId + ".retry")
                    .position(contentWidth - scaled(12.0f) - retryW, 0.0f)
                    .size(retryW, rowH)
                    .text("重试")
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primaryText)
                    .build();
                hitRect(ui, rowId + ".retry.hit",
                        contentWidth - scaled(12.0f) - retryW, 0.0f, retryW, rowH,
                        [&c, rel = row.dirRel] { c.retryUntrackedDir(rel); });
                return;
            }

            // ---- 文件行（§4.5）----
            const bool selected = c.selectedPath() == row.relPath;
            if (selected) {
                ui.rect(rowId + ".selbg")
                    .size(contentWidth, rowH)
                    .color(t.colors.active)
                    .build();
                ui.rect(rowId + ".selbar")
                    .size(2.0f, rowH)
                    .color(t.colors.primary)
                    .build();
            }
            float x = indent;
            // 徽标锚位=缩进后固定 16px 位，不随名长移动（§4.5）；徽标只落文件行
            if (row.status.has_value()) {
                drawBadge(ui, rowId + ".badge", x,
                          (rowH - scaled(kBadgeSize)) * 0.5f, *row.status, t, gc);
            }
            x += scaled(kBadgeSize) + scaled(8.0f);
            // 超长名三段边界（need:299 §4.5 钉死）：①先饿平铺路径段（可至 0 隐藏）
            // 再饿名区至 96px 下限；②省略发生=tooltip 全路径；③CJK 码点截断
            const float avail = contentWidth - x - scaled(12.0f);
            const float naturalW =
                textWidthEstimate(row.name, scaled(13.0f)) + scaled(8.0f);
            const float nameW =
                std::max(scaled(96.0f), std::min(naturalW, std::max(scaled(96.0f), avail)));
            const float segW =
                flat ? std::max(0.0f, avail - nameW - scaled(8.0f)) : 0.0f;
            const auto [shownName, nameTruncated] =
                ellipsisFit(row.name, scaled(13.0f), nameW);
            ui.text(rowId + ".name")
                .position(x, 0.0f)
                .size(nameW, rowH)
                .text(shownName)
                .fontSize(scaled(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
            if (flat) {
                // 平铺：文件名后目录弱化段 12px textTertiary（§4.5；可被长名挤压
                // 至 0 隐藏——先饿次序①）
                std::string dirSeg;
                if (row.relPath.size() > row.name.size() + 1) {
                    dirSeg = row.relPath.substr(
                        0, row.relPath.size() - row.name.size() - 1);
                }
                if (!dirSeg.empty() && segW >= scaled(20.0f)) {
                    const auto [shownSeg, segTrunc] =
                        ellipsisFit(dirSeg, scaled(12.0f), segW);
                    ui.text(rowId + ".dir")
                        .position(x + nameW + scaled(8.0f), 0.0f)
                        .size(segW, rowH)
                        .text(shownSeg)
                        .fontSize(scaled(12.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .hitTestMode(::core::dsl::HitTestMode::None)
                        .build();
                    (void)segTrunc;
                }
            }
            // 超长名/R 行 tooltip（§4.5：R 行 hover 完整「old → new」等宽 12px）
            if (!row.oldPath.empty()) {
                components::tooltip(ui, rowId + ".rename.tip")
                    .theme(core::theme::componentTokens(t))
                    .source(rowId + ".name")
                    .value(row.oldPath + " → " + row.relPath)
                    .anchor(x + nameW * 0.5f, rowH * 0.5f)
                    .bounds(contentWidth, rowH)
                    .minWidth(120.0f)
                    .maxWidth(480.0f)
                    .zIndex(40)
                    .build();
            } else if (nameTruncated) {
                components::tooltip(ui, rowId + ".name.tip")
                    .theme(core::theme::componentTokens(t))
                    .source(rowId + ".name")
                    .value(row.relPath)
                    .anchor(x + nameW * 0.5f, rowH * 0.5f)
                    .bounds(contentWidth, rowH)
                    .minWidth(120.0f)
                    .maxWidth(420.0f)
                    .zIndex(40)
                    .build();
            }
            // 主动线：文件行点击 → diff（F-05；untracked 子文件行同链路）
            hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, rowH,
                    [&c, p = row.relPath] { c.selectChange(p); });
        })
        .build();
}

// ---- 左卡：变更列表（§4；320 可折叠，列头 36 + 独立内滚）----
void composeListCard(eui::Ui& ui, float x, float y, float w, float h,
                     float indentStepPx, GitPageController& c, const Tokens& t) {
    // 卡容器（§4.1：白卡 + 细边 + radius 16）
    ui.rect("git.list.bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.surface)
        .radius(kCardRadius)
        .border(1.0f, t.colors.borderDefault)
        .build();

    // 列头 36px（§4.2：右锚 树/平铺 segmented + 折叠钮）
    const float headH = scaled(kListHeaderH);
    const float btnSize = scaled(kIconBtnSize);
    const float btnY = (headH - btnSize) * 0.5f;
    const float collapseBtnX = x + w - scaled(12.0f) - btnSize;  // bug#297：列头钮右距统一 12
    const float flatBtnX = collapseBtnX - scaled(4.0f) - btnSize;
    const float treeBtnX = flatBtnX - btnSize;
    const bool treeMode = c.viewMode() == ChangeViewMode::Tree;
    iconButton(ui, "git.list.btn.tree", kGlyphFolderTree, treeMode, treeBtnX,
               y + btnY, btnSize, t,
               [&c] { c.setViewMode(ChangeViewMode::Tree); });
    iconButton(ui, "git.list.btn.flat", kGlyphList, !treeMode, flatBtnX,
               y + btnY, btnSize, t,
               [&c] { c.setViewMode(ChangeViewMode::Flat); });
    iconButton(ui, "git.list.btn.collapse", kGlyphCollapse, false, collapseBtnX,
               y + btnY, btnSize, t, [&c] { c.toggleListCollapsed(); });
    ui.rect("git.list.header.border")
        .position(x, y + headH)
        .size(w, 1.0f)
        .color(t.colors.borderMuted)
        .build();

    // 过滤行 32px（§4.2b need:299：纯输入框无按钮，粘顶列头之下；老板令逐字
    // 「目录树加一个搜索功能 只需要一个输入框 不需要按钮」；全路径子串匹配 §4.9）
    const float filterH = scaled(32.0f);
    {
        const float inputH = scaled(28.0f);
        const float inputY = y + headH + (filterH - inputH) * 0.5f;
        const float inputX = x + scaled(12.0f);
        const float inputW = w - scaled(24.0f);
        components::input(ui, "git.list.filter")
            .position(inputX, inputY)
            .size(inputW, inputH)
            .value(c.filterDraft())
            .placeholder("过滤文件名…")
            .fontSize(scaled(12.0f))
            .theme(core::theme::componentTokens(t))
            .onChange([&c](const std::string& v) { c.setFilterDraft(v); })
            .onCancel([&c] { c.clearFilter(); })  // 输入框内 Esc=清空（§6.9）
            .build();
        // 清除 ×（有输入即显；点击=清空+恢复全列表 §4.2b；无输入隐藏）
        if (!c.filterDraft().empty()) {
            const float clearX = inputX + inputW - scaled(22.0f);
            ui.text("git.list.filter.clear")
                .position(clearX, inputY)
                .size(scaled(20.0f), inputH)
                .icon(kGlyphXmark)
                .fontSize(scaled(12.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            hitRect(ui, "git.list.filter.clear.hit", clearX, inputY, scaled(22.0f),
                    inputH, [&c] { c.clearFilter(); });
        }
        ui.rect("git.list.filter.border")
            .position(x, y + headH + filterH)
            .size(w, 1.0f)
            .color(t.colors.borderMuted)
            .build();
    }

    const float listY = y + headH + filterH;
    const float listH = h - headH - filterH;
    const auto& rows = c.listRows();  // 成员引用：行闭包布局期回调仍有效

    if (c.statusLoading() && rows.empty()) {
        // 加载态（§4.8）：组头骨架 ×1 + 行骨架 ×6（无 shimmer，同高不跳版）
        float sy = listY + scaled(8.0f);
        skeletonRow(ui, "git.list.skel.header", x + scaled(12.0f), sy,
                    (w - scaled(24.0f)) * 0.5f, scaled(20.0f), t);
        sy += scaled(32.0f);
        float widths[] = {0.90f, 0.70f, 0.80f, 0.60f, 0.75f, 0.66f};
        for (int i = 0; i < 6; ++i) {
            skeletonRow(ui, "git.list.skel." + std::to_string(i),
                        x + scaled(12.0f), sy, (w - scaled(24.0f)) * widths[i],
                        scaled(20.0f), t);
            sy += scaled(28.0f);
        }
        return;
    }
    if (!c.statusError().empty() && rows.empty()) {
        // 扫描失败错误块（§4.8：不冒充空态；重试绕节流闸）
        const float blockX = x + scaled(12.0f);
        const float blockW = w - scaled(24.0f);
        float by = listY + scaled(16.0f);
        ui.text("git.list.err.icon")
            .position(blockX, by)
            .size(scaled(20.0f), scaled(20.0f))
            .icon(kGlyphCircleExclaim)
            .fontSize(scaled(14.0f))
            .color(t.colors.error)
            .build();
        by += scaled(24.0f);
        ui.text("git.list.err.main")
            .position(blockX, by)
            .size(blockW, scaled(20.0f))
            .text("无法获取变更")
            .fontSize(scaled(13.0f))
            .fontWeight(600)
            .color(t.colors.errorText)
            .build();
        by += scaled(22.0f);
        ui.text("git.list.err.reason")
            .position(blockX, by)
            .size(blockW, scaled(36.0f))
            .text(c.statusError())
            .fontSize(scaled(12.0f))
            .lineHeight(scaled(18.0f))
            .wrap(true)
            .color(t.colors.textTertiary)
            .build();
        by += scaled(40.0f);
        secondaryButton(ui, "git.list.err.retry", "重试", blockX, by,
                        scaled(72.0f), scaled(28.0f), t,
                        [&c] { c.retryStatus(); });
        return;
    }
    if (rows.empty()) {
        if (c.filterActive()) {
            // 过滤无匹配空态（§4.8 need:299：与「工作区干净」空态严格区分，
            // 两行画面；仅过滤词非空时出现）
            ui.text("git.list.filter.empty.main")
                .position(x, listY + listH * 0.4f)
                .size(w, scaled(20.0f))
                .text("无匹配文件")
                .fontSize(scaled(13.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
            ui.text("git.list.filter.empty.sub")
                .position(x, listY + listH * 0.4f + scaled(24.0f))
                .size(w, scaled(18.0f))
                .text("试试更短关键词，或点 × 清空")
                .fontSize(scaled(12.0f))
                .horizontalAlign(eui::HorizontalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            return;
        }
        // 空态（§4.8：组头全隐藏 + 弱提示一行；主空态画面由右卡承担 §5.7）
        ui.text("git.list.empty")
            .position(x, listY + listH * 0.4f)
            .size(w, scaled(20.0f))
            .text("没有未提交变更")
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        return;
    }

    // 行序列 virtualList（定高槽 32：组头满槽、行画 28px）
    const bool flat = c.viewMode() == ChangeViewMode::Flat;
    components::virtualList(ui, "git.changes.list")
        .position(x + 1.0f, listY)
        .size(w - 2.0f, listH)
        .itemCount(static_cast<int64_t>(rows.size()))
        .rowHeight(scaled(kRowSlotH))
        .step(scaled(kRowSlotH))
        .overscanViewports(1.0f)
        // 滚动条视觉全无（老板口径 §1.1：滚动保留、滚动条不显示）——纵向条
        // 隐藏，滚轮照常；横向条是 diff/内容专用（F-07⑥ 拖拽回归依赖，另处）
        .scrollbarWidth(0.0f)
        .scrollbarGap(0.0f)
        .theme(core::theme::componentTokens(t))
        .offset(c.listScrollOffset())
        .onChange([&c](float v) { c.setListScrollOffset(v); })
        .row([&](eui::Ui& inner, const std::string& rowId, int64_t index,
                 float contentWidth, float vh) {
            (void)vh;
            const auto idx = static_cast<size_t>(index);
            if (idx >= rows.size()) return;
            drawChangeListRow(inner, rowId, rows[idx], idx, flat, indentStepPx,
                              contentWidth, c, t, gitColors(t));
        })
        .build();
}

// ---- 内容视图行（§5.4：行号列 48px + 代码 runs；行槽 22px）----
void drawContentRow(eui::Ui& ui, const std::string& rowId, size_t lineIdx,
                    float contentWidth, const GitPageController& c,
                    const Tokens& t, const GitColors& gc) {
    const float rowH = scaled(kCodeRowH);
    ui.stack(rowId)
        .size(contentWidth, rowH)
        .content([&] {
            const float gutterW = scaled(kContentGutterW);
            ui.rect(rowId + ".gutter")
                .size(gutterW, rowH)
                .color(gc.diffGutterBg)
                .build();
            ui.rect(rowId + ".gutter.border")
                .position(gutterW, 0.0f)
                .size(1.0f, rowH)
                .color(t.colors.borderMuted)
                .build();
            ui.text(rowId + ".no")
                .position(0.0f, 0.0f)
                .size(gutterW - scaled(kCodeGutterPad), rowH)
                .text(std::to_string(lineIdx + 1))
                .fontFamily("monospace")
                .fontSize(scaled(12.0f))
                .horizontalAlign(eui::HorizontalAlign::Right)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textTertiary)
                .build();
            // F-07⑥ 真横滚：代码文本按行内横向偏移整体平移（行号列固定不随滚），
            // 元素宽 = 全文虚拟宽 → 超长行不换行不错位；clip 盒从沟槽右缘起裁，
            // 平移后的文本不会盖回行号列
            const float codeFixedX = gutterW + scaled(6.0f);
            const float clipW = std::max(scaled(20.0f), contentWidth - codeFixedX);
            ui.stack(rowId + ".codeclip")
                .position(codeFixedX, 0.0f)
                .size(clipW, rowH)
                .clip()
                .content([&] {
                    auto text = ui.text(rowId + ".code")
                        .position(-c.contentHOffset(), 0.0f)
                        .size(std::max(clipW,
                                       monoLineWidth(c.content().maxLineLen,
                                                     scaled(13.0f))),
                               rowH)
                        .text(c.content().lines[lineIdx])
                        .fontFamily("monospace")
                        .fontSize(scaled(13.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textPrimary);
                    // 高亮 runs（load 时按当时主题算好的行内偏移；无 runs = 默认色）
                    const auto& allRuns = c.contentRuns();
                    if (lineIdx < allRuns.size() && !allRuns[lineIdx].empty()) {
                        text.runs(allRuns[lineIdx]);
                    }
                    text.build();
                })
                .build();
        })
        .build();
}

// ---- 内容视图体（§5.4/§7：四态占位 + 错误重试 + 行列表）----
void composeContentBody(eui::Ui& ui, float x, float y, float w, float h,
                        GitPageController& c, const Tokens& t,
                        const GitColors& gc) {
    const auto st = c.contentState();
    if (st == RightPaneState::None) return;
    if (st == RightPaneState::Loading) {
        float sy = y + scaled(12.0f);
        float widths[] = {0.70f, 0.95f, 0.82f, 0.88f};
        for (int i = 0; i < 4; ++i) {
            skeletonRow(ui, "git.content.skel." + std::to_string(i),
                        x + scaled(12.0f), sy, (w - scaled(24.0f)) * widths[i],
                        scaled(16.0f), t);
            sy += scaled(26.0f);
        }
        return;
    }
    if (st == RightPaneState::Error) {
        ui.text("git.content.err.main")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text("无法读取文件")
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.errorText)
            .build();
        ui.text("git.content.err.reason")
            .position(x, y + h * 0.4f + scaled(26.0f))
            .size(w, scaled(18.0f))
            .text(c.contentError())
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        const float btnW = 88.0f;
        secondaryButton(ui, "git.content.retry", "重试",
                        x + (w - btnW) * 0.5f, y + h * 0.4f + scaled(52.0f), btnW,
                        scaled(32.0f), t, [&c] { c.retryContent(); });
        return;
    }
    if (st == RightPaneState::Placeholder) {
        // 四态占位文案（§5.4）：主文案 14px --c-text-2 + 副文案 12px --c-text-3
        const char* mainText = "无法显示";
        const char* subText = "";
        switch (c.content().state) {
        case ContentState::Empty:
            mainText = "空文件";
            subText = "文件无内容。";
            break;
        case ContentState::Binary:
            mainText = "二进制文件";
            subText = "不预览二进制内容。";
            break;
        case ContentState::Oversize:
            mainText = "文件过大，未加载内容（>1 MiB）";
            subText = "仅预览 1 MiB 以内的文本文件。";
            break;
        case ContentState::BadEncoding:
            mainText = "暂不支持该编码";
            subText = "文件不是有效的 UTF-8 文本。";
            break;
        default: break;
        }
        ui.text("git.content.ph.main")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text(mainText)
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        ui.text("git.content.ph.sub")
            .position(x, y + h * 0.4f + scaled(26.0f))
            .size(w, scaled(18.0f))
            .text(subText)
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        return;
    }

    // Ready：行号列 + 代码 runs（virtualList 定高 22px）。F-07⑥ 真横滚：虚拟
    // 内容宽 = 最长行等宽估算；超宽出底部横向滚动条（行内整体平移不换行）
    const auto& lines = c.content().lines;
    const float codeX = scaled(kContentGutterW) + scaled(6.0f);
    const float virtualRowW =
        codeX + std::max(w - codeX,
                         monoLineWidth(c.content().maxLineLen, scaled(13.0f)));
    components::virtualList(ui, "git.content.list")
        .position(x, y)
        .size(w, h)
        .itemCount(static_cast<int64_t>(lines.size()))
        .rowHeight(scaled(kCodeRowH))
        .step(scaled(kCodeRowH))
        .overscanViewports(1.0f)
        .scrollbarWidth(0.0f)  // 滚动条视觉全无（§1.1 纵向条；横向条保留 F-07⑥）
        .scrollbarGap(0.0f)
        .theme(core::theme::componentTokens(t))
        .offset(c.contentScrollOffset())
        .onChange([&c](float v) { c.setContentScrollOffset(v); })
        .contentWidth(virtualRowW)
        .hOffset(c.contentHOffset())
        .onHChange([&c](float v) { c.setContentHOffset(v); })
        .row([&](eui::Ui& inner, const std::string& rowId, int64_t index,
                 float contentWidth, float vh) {
            (void)vh;
            const auto idx = static_cast<size_t>(index);
            if (idx >= lines.size()) return;
            drawContentRow(inner, rowId, idx, contentWidth, c, t, gc);
        })
        .build();
}

// ---- diff 视图行（§5.3：四列 24/48/48/flex；行 22px 画 24px 槽内）----
void drawDiffRow(eui::Ui& ui, const std::string& rowId, const DiffRow& row,
                 float contentWidth, GitPageController& c, const Tokens& t,
                 const GitColors& gc) {
    const float slotH = scaled(kDiffSlotH);
    ui.stack(rowId)
        .size(contentWidth, slotH)
        .content([&] {
            if (row.kind == DiffRow::Kind::HunkHeader) {
                // hunk 头：24px 满槽；底 --diff-hunk-bg；折叠符 ▼/▶ 12px；头 11px 等宽
                ui.rect(rowId + ".bg")
                    .size(contentWidth, slotH)
                    .color(gc.diffHunkBg)
                    .build();
                const bool folded = c.diffModel().folded(row.hunkIndex);
                ui.text(rowId + ".fold")
                    .position(scaled(4.0f), 0.0f)
                    .size(scaled(kDiffFoldW), slotH)
                    .text(folded ? "▶" : "▼")
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                ui.text(rowId + ".header")
                    .position(scaled(kDiffFoldW + kDiffNoW * 2 + 8.0f), 0.0f)
                    .size(std::max(scaled(40.0f),
                                   contentWidth - scaled(kDiffFoldW + kDiffNoW * 2 + 8.0f)),
                          slotH)
                    .text(c.diffModel().patch().hunks[row.hunkIndex].header)
                    .fontFamily("monospace")
                    .fontSize(scaled(11.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .build();
                hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, slotH,
                        [&c, h = row.hunkIndex] { c.diffModel().toggleHunk(h); });
                return;
            }
            if (row.kind == DiffRow::Kind::FoldBar) {
                // 折叠条：居中「··· 展开早期未变更行（N 行）」；底 --c-btn-2-bg
                ui.rect(rowId + ".bg")
                    .size(contentWidth, slotH)
                    .color(t.colors.surfaceElevated)
                    .build();
                const size_t n =
                    c.diffModel().patch().hunks[row.hunkIndex].lines.size();
                ui.text(rowId + ".text")
                    .position(0.0f, 0.0f)
                    .size(contentWidth, slotH)
                    .text("··· 展开早期未变更行（" + std::to_string(n) + " 行）")
                    .fontSize(scaled(11.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, slotH,
                        [&c, h = row.hunkIndex] { c.diffModel().toggleHunk(h); });
                return;
            }

            // diff 行：增/删整行浅底（22px 行高），行号列按 origin 显隐
            const auto& line =
                c.diffModel().patch().hunks[row.hunkIndex].lines[row.lineIndex];
            const float rowH = scaled(kCodeRowH);
            const float oldX = scaled(kDiffFoldW);
            const float newX = scaled(kDiffFoldW + kDiffNoW);
            const float codeX = scaled(kDiffFoldW + kDiffNoW * 2);
            if (line.origin == '+') {
                ui.rect(rowId + ".bg")
                    .size(contentWidth, rowH)
                    .color(gc.diffAddedBg)
                    .build();
            } else if (line.origin == '-') {
                ui.rect(rowId + ".bg")
                    .size(contentWidth, rowH)
                    .color(gc.diffRemovedBg)
                    .build();
            }
            const bool hasOld = line.oldNo.has_value();
            const bool hasNew = line.newNo.has_value();
            if (hasOld) {
                ui.text(rowId + ".oldno")
                    .position(oldX, 0.0f)
                    .size(scaled(kDiffNoW - kCodeGutterPad), rowH)
                    .text(std::to_string(*line.oldNo))
                    .fontFamily("monospace")
                    .fontSize(scaled(12.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(line.origin == '-' ? t.colors.errorText
                                              : t.colors.textTertiary)
                    .build();
            }
            if (hasNew) {
                ui.text(rowId + ".newno")
                    .position(newX, 0.0f)
                    .size(scaled(kDiffNoW - kCodeGutterPad), rowH)
                    .text(std::to_string(*line.newNo))
                    .fontFamily("monospace")
                    .fontSize(scaled(12.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(line.origin == '+' ? t.colors.primaryText
                                              : t.colors.textTertiary)
                    .build();
            }
            // F-07⑥ 真横滚：代码文本按行内横向偏移整体平移（行号/折叠列固定），
            // clip 盒从内容列左缘起裁，平移文本不盖回行号列
            ui.stack(rowId + ".codeclip")
                .position(codeX, 0.0f)
                .size(std::max(scaled(20.0f), contentWidth - codeX), rowH)
                .clip()
                .content([&] {
                    ui.text(rowId + ".code")
                        .position(-c.diffHOffset(), 0.0f)
                        .size(std::max(scaled(20.0f),
                                       monoLineWidth(c.diffModel().maxLineLen(),
                                                     scaled(13.0f))),
                              rowH)
                        .text(line.content)
                        .fontFamily("monospace")
                        .fontSize(scaled(13.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textPrimary)
                        .build();
                })
                .build();
        })
        .build();
}

// ---- diff 视图体（§5.3/§5.6：头部信息在右卡 44px 头部；此体只画画布）----
void composeDiffBody(eui::Ui& ui, float x, float y, float w, float h,
                     GitPageController& c, const Tokens& t, const GitColors& gc) {
    const auto st = c.diffState();
    if (st == RightPaneState::None) return;

    if (st == RightPaneState::Loading) {
        float sy = y + scaled(12.0f);
        float widths[] = {0.74f, 0.92f, 0.68f, 0.85f};
        for (int i = 0; i < 4; ++i) {
            skeletonRow(ui, "git.diff.skel." + std::to_string(i),
                        x + scaled(12.0f), sy, (w - scaled(24.0f)) * widths[i],
                        scaled(16.0f), t);
            sy += scaled(26.0f);
        }
        return;
    }
    if (st == RightPaneState::Error) {
        // §5.6 错误块：禁静默空/禁「无内容变更」误导文案（bug#290 根治口径）
        ui.text("git.diff.err.main")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text("无法加载差异")
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.errorText)
            .build();
        ui.text("git.diff.err.reason")
            .position(x, y + h * 0.4f + scaled(26.0f))
            .size(w, scaled(18.0f))
            .text(c.diffError())
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        const float btnW = 88.0f;
        secondaryButton(ui, "git.diff.retry", "重试",
                        x + (w - btnW) * 0.5f, y + h * 0.4f + scaled(52.0f),
                        btnW, scaled(32.0f), t, [&c] { c.retryDiff(); });
        return;
    }

    // Ready：untracked 超大占位（bug#18，F-11③ 同文案）/ 竞态占位（N5 显式
    // 文案+自动刷新）/ 二进制占位 / 空文件空态 / unified 行列表
    if (!c.diffModel().placeholder().empty()) {
        ui.text("git.diff.placeholder")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text(c.diffModel().placeholder())
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        return;
    }
    if (c.diffModel().binary()) {
        ui.text("git.diff.binary")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text("二进制文件，未显示内容差异")
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        return;
    }
    if (c.diffModel().isEmpty()) {
        ui.text("git.diff.empty")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text("无内容变更")
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        return;
    }
    const auto& rows = c.diffModel().rows();
    // F-07⑥ 真横滚：diff 行虚拟宽 = 最长行等宽估算（超宽出底部横向滚动条）
    const float codeX = scaled(kDiffFoldW + kDiffNoW * 2);
    const float virtualRowW =
        codeX + std::max(w - codeX,
                         monoLineWidth(c.diffModel().maxLineLen(), scaled(13.0f)));
    components::virtualList(ui, "git.diff.list")
        .position(x, y)
        .size(w, h)
        .itemCount(static_cast<int64_t>(rows.size()))
        .rowHeight(scaled(kDiffSlotH))
        .step(scaled(kDiffSlotH))
        .overscanViewports(1.0f)
        .scrollbarWidth(0.0f)  // 滚动条视觉全无（§1.1 纵向条；横向条保留 F-07⑥）
        .scrollbarGap(0.0f)
        .theme(core::theme::componentTokens(t))
        .offset(c.diffScrollOffset())
        .onChange([&c](float v) { c.setDiffScrollOffset(v); })
        .contentWidth(virtualRowW)
        .hOffset(c.diffHOffset())
        .onHChange([&c](float v) { c.setDiffHOffset(v); })
        .row([&](eui::Ui& inner, const std::string& rowId, int64_t index,
                 float contentWidth, float vh) {
            (void)vh;
            const auto idx = static_cast<size_t>(index);
            if (idx >= rows.size()) return;
            drawDiffRow(inner, rowId, rows[idx], contentWidth, c, t, gc);
        })
        .build();
}

// ---- 右卡空态块（§5.5/§5.7：线性图标 + 主文案 + 副文案 + 可选就近操作）----
void composeRightEmpty(eui::Ui& ui, float x, float y, float w, float h,
                       unsigned glyph, const std::string& mainText,
                       const std::string& subText, const Tokens& t,
                       GitPageController* c = nullptr) {
    const float blockW = std::min(420.0f, w);
    const float blockX = x + (w - blockW) * 0.5f;
    float by = y + h * 0.32f;
    ui.text("git.right.empty.icon")
        .position(blockX + (blockW - scaled(24.0f)) * 0.5f, by)
        .size(scaled(24.0f), scaled(24.0f))
        .icon(glyph)
        .fontSize(scaled(20.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    by += scaled(36.0f);
    ui.text("git.right.empty.main")
        .position(blockX, by)
        .size(blockW, scaled(20.0f))
        .text(mainText)
        .fontSize(scaled(14.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textSecondary)
        .build();
    by += scaled(26.0f);
    ui.text("git.right.empty.sub")
        .position(blockX, by)
        .size(blockW, scaled(18.0f))
        .text(subText)
        .fontSize(scaled(12.0f))
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textTertiary)
        .build();
    if (c != nullptr) {
        by += scaled(30.0f);
        secondaryButton(ui, "git.right.empty.action", "↻ 刷新",
                        blockX + (blockW - scaled(96.0f)) * 0.5f, by,
                        scaled(96.0f), scaled(32.0f), t, [c] { c->refresh(); });
    }
}

// ---- 右卡：diff 主区（§5：头部 44px 粘顶 + 视图体）----
void composeDiffCard(eui::Ui& ui, float x, float y, float w, float h,
                     GitPageController& c, const Tokens& t) {
    ui.rect("git.right.bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.surface)
        .radius(kCardRadius)
        .border(1.0f, t.colors.borderDefault)
        .build();

    const float headH = scaled(kRightHeaderH);
    const float padX = scaled(12.0f);  // bug#297：卡内距统一 12（与顶栏/左卡内容列对齐）
    const auto selStatus = c.selectedRowStatus();

    // 头部（§5.2 徽标 + 路径条 + 统计 + tab）
    float hx = x + padX;
    if (selStatus.has_value()) {
        drawBadge(ui, "git.right.head.badge", hx,
                  y + (headH - scaled(kBadgeSize)) * 0.5f, *selStatus, t,
                  gitColors(t));
        hx += scaled(kBadgeSize) + scaled(8.0f);
    }
    // 路径条：等宽 13px；rename 显「old → new」（diffModel.headerText 既有口径）
    std::string pathText = c.selectedPath().empty() ? std::string() : c.selectedPath();
    if (c.diffState() == RightPaneState::Ready && !c.diffModel().headerText().empty()) {
        pathText = c.diffModel().headerText();
    }
    // 统计 +N/−N（§5.2：diff 体量唯一摘要位；右锚 tab 前预留位现算）
    size_t added = 0;
    size_t removed = 0;
    if (c.diffState() == RightPaneState::Ready) {
        for (const auto& hunk : c.diffModel().patch().hunks) {
            for (const auto& l : hunk.lines) {
                if (l.origin == '+') ++added;
                else if (l.origin == '-') ++removed;
            }
        }
    }
    const float tabW = scaled(52.0f);
    const float statsW = (added > 0 || removed > 0)
                             ? textWidthEstimate("+" + std::to_string(added) + " −" +
                                                     std::to_string(removed),
                                                 scaled(12.0f)) + scaled(8.0f)
                             : 0.0f;
    const float tabZoneW = tabW * 2 + scaled(16.0f);
    const float pathW = std::max(
        scaled(40.0f),
        w - (hx - x) - padX - statsW - tabZoneW - scaled(16.0f));
    // bug#292（F-11）：超长路径省略（显示列宽预算逐码点截断+「…」）+ tooltip 全文
    // （包装栈内局部坐标=列表行 tooltip 同款口径；等宽列宽 0.62em/列、非 ASCII
    // 起始字节记 2 列——CJK 实测 ~1.0em 恒偏大安全侧）
    std::string shownPath;
    bool pathTruncated = false;
    if (!pathText.empty()) {
        const float colW = scaled(13.0f) * 0.62f;
        const float budget =
            pathW - scaled(13.0f) * 1.6f - scaled(12.0f);  // 基宽+右安全边
        float acc = 0.0f;
        size_t i = 0;
        while (i < pathText.size()) {
            const unsigned char ch = static_cast<unsigned char>(pathText[i]);
            const size_t len = (ch < 0x80) ? 1
                               : ((ch & 0xE0) == 0xC0) ? 2
                               : ((ch & 0xF0) == 0xE0) ? 3 : 4;
            const float wCols = (ch < 0x80) ? colW : colW * 2.0f;
            if (acc + wCols > budget) {
                pathTruncated = true;
                break;
            }
            shownPath.append(pathText, i, len);
            acc += wCols;
            i += len;
        }
        if (pathTruncated) shownPath += "…";
    }
    ui.stack("git.right.head.pathwrap")
        .position(hx, y)
        .size(pathW, headH)
        .content([&] {
            ui.text("git.right.head.path")
                .position(0.0f, 0.0f)
                .size(pathW, headH)
                .text(shownPath)
                .fontFamily("monospace")
                .fontSize(scaled(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
            if (pathTruncated) {
                components::tooltip(ui, "git.right.head.path.tip")
                    .theme(core::theme::componentTokens(t))
                    .source("git.right.head.path")
                    .value(pathText)
                    .anchor(pathW * 0.5f, headH * 0.5f)
                    .bounds(pathW, headH)
                    .minWidth(120.0f)
                    .maxWidth(420.0f)
                    .zIndex(40)
                    .build();
            }
        })
        .build();
    // 统计（路径后 8px：+N 绿 / −N 红；单元素 + runs 分色，§5.2）
    if (statsW > 0.0f) {
        const float statsX = x + w - padX - tabZoneW - scaled(16.0f) - statsW;
        const std::string addText = "+" + std::to_string(added);
        const std::string delText = "−" + std::to_string(removed);
        std::vector<eui::TextRun> statRuns;
        statRuns.push_back({0, static_cast<int>(addText.size()), t.colors.primaryText});
        statRuns.push_back({static_cast<int>(addText.size() + 1),
                            static_cast<int>(addText.size() + 1 + delText.size()),
                            t.colors.errorText});
        auto statsText = ui.text("git.right.head.stats")
            .position(statsX, y)
            .size(statsW, headH)
            .text(addText + " " + delText)
            .fontFamily("monospace")
            .fontSize(scaled(12.0f))
            .verticalAlign(eui::VerticalAlign::Center);
        statsText.runs(statRuns);
        statsText.build();
    }
    // tab（右锚；§5.2 激活=底部 2px 主色下划线）
    {
        const float contentTabX = x + w - padX - tabW;
        const float diffTabX = contentTabX - scaled(16.0f) - tabW;
        underlineTab(ui, "git.right.head.tab.content", "内容",
                     c.rightTab() == GitPageController::RightTab::Content,
                     contentTabX, y, tabW, headH, t,
                     [&c] { c.setRightTab(GitPageController::RightTab::Content); });
        underlineTab(ui, "git.right.head.tab.diff", "差异",
                     c.rightTab() == GitPageController::RightTab::Diff,
                     diffTabX, y, tabW, headH, t,
                     [&c] { c.setRightTab(GitPageController::RightTab::Diff); });
    }
    ui.rect("git.right.head.border")
        .position(x, y + headH)
        .size(w, 1.0f)
        .color(t.colors.borderDefault)
        .build();

    // 视图体（diff 常驻主视图 / 内容次视图 / 空态全集）
    const float bodyY = y + headH;
    const float bodyH = h - headH;
    const GitColors gc = gitColors(t);
    const auto& rows = c.listRows();
    if (c.statusLoading() && rows.empty()) {
        composeRightEmpty(ui, x, bodyY, w, bodyH, kGlyphCodeBranch,
                          "选择左侧文件查看差异", "正在扫描变更…", t);
    } else if (rows.empty() && c.statusError().empty() && !c.filterActive()) {
        // 工作区干净空态（§5.7：图标+主/副文案+[刷新]；空仓同构文案演绎统一，
        // platform/git 无提交数查询接口）。need299 打磨轮修订：过滤无匹配
        // （rows 被滤空但工作区非干净）不得到此冒充干净空态（§4.9 过滤=仅
        // 视图过滤），落 §5.5 未选文件空态——filterActive 判定隔离两空态
        composeRightEmpty(ui, x, bodyY, w, bodyH, kGlyphCircleCheck,
                          "工作区很干净", "文件变更后会出现在这里", t, &c);
    } else if (c.selectedPath().empty()) {
        // 未选文件空态（§5.5）
        composeRightEmpty(ui, x, bodyY, w, bodyH, kGlyphCodeBranch,
                          "选择左侧文件查看差异", "点击变更列表中的文件行", t);
    } else if (c.rightTab() == GitPageController::RightTab::Diff) {
        composeDiffBody(ui, x, bodyY, w, bodyH, c, t, gc);
    } else {
        composeContentBody(ui, x, bodyY, w, bodyH, c, t, gc);
    }

    // F-13③ 过期条「内容已过期，点击重载」：覆盖绘制在视图体顶部（不进布局 →
    // 出现/消失零滚动跳动），点击重载当前打开内容/diff + status
    if (c.staleBarVisible()) {
        const float barH = scaled(28.0f);
        ui.stack("git.right.stale")
            .position(x, bodyY)
            .size(w, barH)
            .zIndex(30)
            .content([&] {
                ui.rect("git.right.stale.bg")
                    .size(w, barH)
                    .color(components::theme::withOpacity(t.colors.primary, 0.14f))
                    .build();
                ui.rect("git.right.stale.border")
                    .position(0.0f, barH - 1.0f)
                    .size(w, 1.0f)
                    .color(t.colors.borderDefault)
                    .build();
                ui.text("git.right.stale.text")
                    .size(w, barH)
                    .text("内容已过期，点击重载")
                    .fontSize(scaled(12.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.primaryText)
                    .build();
            })
            .build();
        hitRect(ui, "git.right.stale.hit", x, bodyY, w, barH,
                [&c] { c.reloadStale(); });
    }
}

} // namespace

void composeGitPage(eui::Ui& ui, const eui::Screen& screen,
                    GitPageController& c, const core::theme::Tokens& t) {
    c.composePump();
    if (c.phase() == GitPagePhase::Empty) {
        composeEmptyPage(ui, screen, c, t);
        return;
    }
    const float w = screen.width;
    const float h = screen.height;
    const bool narrow = w < kNarrowBreakpoint;  // §9：<1200 左卡 280

    composeTopBar(ui, w, narrow, c, t);

    // 两栏构图（§1：页边 12 + 顶栏卡 12..60 + gap 12 + 左卡 320 可折叠 + gap 12 + 右卡余宽）
    // bug#297：顶栏卡片化后 bodyY 含顶栏卡自身页边（12+48+12=72）
    const float bodyY = kPageMargin + kTopBarH + kPageMargin;
    const float bodyH = h - bodyY - kPageMargin;
    float listW = narrow ? kListCardWNarrow : kListCardW;
    if (c.listCollapsed()) listW = 0.0f;  // §6.5 折叠=宽 0 隐藏，右卡满宽
    if (listW > 0.0f) {
        composeListCard(ui, kPageMargin, bodyY, listW, bodyH,
                        scaled(narrow ? kIndentStepNarrow : kIndentStep), c, t);
    }
    const float rightX = kPageMargin + listW + (listW > 0.0f ? kCardGap : 0.0f);
    const float rightW = w - rightX - kPageMargin;
    composeDiffCard(ui, rightX, bodyY, rightW, bodyH, c, t);

    // 切换仓库历史下拉（need:299 §3.1）：compose 在两卡之后 = z 序最上（scrim 同层；
    // 临时浮层不改页面构图 §1.1）
    composeRepoDropdown(ui, w, h, c, t);
}

} // namespace module::git
