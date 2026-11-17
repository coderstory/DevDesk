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

// ---- 顶栏（§3 48px：折叠展开钮(条件) + 仓名 + 路径 + 搜索 + 切换仓库 + 刷新）----
// 设计 §3 分支 chip：platform/git 无分支查询接口（数据面零改动硬约束），演绎省略。

// 搜索框几何（320×32，窄窗 240；弹性间隔位=路径右侧、「切换仓库」左侧）
std::pair<float, float> searchBoxGeometry(float w, bool narrow) {
    const float searchW = scaled(narrow ? 240.0f : 320.0f);
    const float searchH = scaled(32.0f);
    const float switchW = textWidthEstimate("切换仓库", scaled(14.0f)) + scaled(24.0f);
    const float refreshW = textWidthEstimate("↻ 刷新", scaled(14.0f)) + scaled(24.0f);
    const float x = w - scaled(16.0f) - refreshW - scaled(12.0f) - switchW -
                    scaled(12.0f) - searchW;
    return {x, (kTopBarH - searchH) * 0.5f};
}

void composeTopBar(eui::Ui& ui, float w, bool narrow, GitPageController& c,
                   const Tokens& t) {
    ui.rect("git.top.bg").size(w, kTopBarH).color(t.colors.surface).build();
    ui.rect("git.top.border")
        .position(0.0f, kTopBarH)
        .size(w, 1.0f)
        .color(t.colors.borderDefault)
        .build();

    const float padX = scaled(16.0f);
    float x = padX;
    // 折叠展开钮（§3：仅左栏折叠时显示，为顶栏第一个元素 32×32）
    if (c.listCollapsed()) {
        const float btnY = (kTopBarH - scaled(32.0f)) * 0.5f;
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
        .position(x, 0.0f)
        .size(nameW + scaled(8.0f), kTopBarH)
        .text(name)
        .fontSize(scaled(15.0f))
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();

    x += nameW + scaled(8.0f) + scaled(12.0f);
    const auto [searchBoxX, searchBoxY] = searchBoxGeometry(w, narrow);
    const float pathMaxW = scaled(360.0f);
    ui.text("git.top.path")
        .position(x, 0.0f)
        .size(std::min(pathMaxW, std::max(scaled(40.0f), searchBoxX - x - scaled(12.0f))),
              kTopBarH)
        .text(c.repoPathUtf8())
        .fontSize(scaled(12.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .build();

    // 全局搜索框（§5.1a need:14：320×32、radius 6、底 sidebarBg 降档区分、
    // placeholder 13px textTertiary；Enter 提交 / ESC 显式取消 / 清空=取消同路）。
    // inset 让出左侧放大镜位（组件 inset 左右对称，右缘由 ×/扫描圈叠盖——
    // 同 agent.dialog.tools.search 先例）
    const float searchW = scaled(narrow ? 240.0f : 320.0f);
    const float searchH = scaled(32.0f);
    components::input(ui, "git.search.input")
        .position(searchBoxX, searchBoxY)
        .size(searchW, searchH)
        .value(c.searchDraft())
        .placeholder("搜索全仓内容…")
        .fontSize(scaled(13.0f))
        .inset(scaled(30.0f))
        .theme(core::theme::componentTokens(t))
        .onChange([&c](const std::string& v) {
            c.setSearchDraft(v);
            c.markSearchEdited();
        })
        .onEnter([&c] { c.submitSearchNow(); })
        .onCancel([&c] { c.cancelSearch(); })
        .build();
    // 左侧放大镜（§5.1a「放大镜 16px 描边 --c-text-3」；纯装饰 pointer 穿透，
    // FA5 search 0xF002——同 agent 搜索框先例）
    ui.text("git.search.icon")
        .position(searchBoxX + scaled(9.0f), searchBoxY)
        .size(scaled(16.0f), searchH)
        .icon(::eui::utf8(0xF002))
        .fontSize(scaled(13.0f))
        .lineHeight(searchH)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textTertiary)
        .hitTestMode(::core::dsl::HitTestMode::None)
        .build();
    // 右侧指示（§5.1a 两行并列——扫描圈与清除 × 并存不互斥，P2-6）：
    // × 恒居右缘（有草稿即显，点击=清空+取消，扫描中同此路）；扫描圈让位 ×
    // 左侧一档（pulse 驱动持续出帧；「旋转圈」EUI 无旋转原语，落地为 pulse 字形）
    const float badgeX = searchBoxX + searchW - scaled(22.0f);
    const bool hasDraft = !c.searchDraft().empty();
    if (c.searchState() == GitPageController::GitSearchState::Scanning) {
        const float spinX = hasDraft ? badgeX - scaled(22.0f) : badgeX;
        ui.text("git.search.spin")
            .position(spinX, searchBoxY)
            .size(scaled(20.0f), searchH)
            .icon(kGlyphSpinner)
            .fontSize(scaled(12.0f))
            .opacity(skeletonPulseOpacity())
            .onFrame([](float) {})
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
    }
    if (hasDraft) {
        ui.text("git.search.clear")
            .position(badgeX, searchBoxY)
            .size(scaled(20.0f), searchH)
            .icon(0xF00D)
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        hitRect(ui, "git.search.clear.hit", badgeX, searchBoxY, scaled(22.0f),
                searchH, [&c] { c.cancelSearch(); });
    }

    // 右侧按钮组：[切换仓库] [↻ 刷新]（次按钮 32px 高）
    const float btnH = scaled(32.0f);
    const float btnY = (kTopBarH - btnH) * 0.5f;
    const float switchW = textWidthEstimate("切换仓库", scaled(14.0f)) + scaled(24.0f);
    const float refreshW = textWidthEstimate("↻ 刷新", scaled(14.0f)) + scaled(24.0f);
    const float refreshX = w - scaled(16.0f) - refreshW;
    const float switchX = refreshX - scaled(12.0f) - switchW;
    secondaryButton(ui, "git.top.refresh", "↻ 刷新", refreshX, btnY, refreshW, btnH,
                    t, [&c] { c.refresh(); });
    secondaryButton(ui, "git.top.switch", "切换仓库", switchX, btnY, switchW, btnH,
                    t, [&c] { c.switchRepo(); });
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
            const float indent = scaled(kIndentBase) + indentStep * row.depth;

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
                const bool untracked =
                    row.status == platform::git::StatusKind::Untracked;
                // §4.4：hover/按压降级省略（EUI 无 hover 驱动）；不选中不进 diff
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
                // 目录名（untracked 行 name 已带 '/' 后缀，模型烘焙）
                const bool hasCount =
                    row.childCount > 0 && !untracked;  // R4：untracked 行不显计数
                const float nameW =
                    contentWidth - x - scaled(12.0f) -
                    (untracked ? scaled(kBadgeSize + 4.0f) : 0.0f) -
                    (hasCount ? scaled(36.0f) : 0.0f);
                ui.text(rowId + ".name")
                    .position(x, 0.0f)
                    .size(std::max(scaled(20.0f), nameW), rowH)
                    .text(row.name)
                    .fontSize(scaled(13.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textPrimary)
                    .build();
                // U 徽标（§4.6：目录行=目录名后 4px）
                if (untracked) {
                    drawBadge(ui, rowId + ".badge",
                              x + textWidthEstimate(row.name, scaled(13.0f)) +
                                  scaled(4.0f),
                              (rowH - scaled(kBadgeSize)) * 0.5f,
                              platform::git::StatusKind::Untracked, t, gc);
                }
                // 子树变更文件计数（§4.4：右锚 12px）
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
                // 超长目录名 tooltip（need14 口径）
                if (textWidthEstimate(row.name, scaled(13.0f)) > nameW) {
                    components::tooltip(ui, rowId + ".name.tip")
                        .theme(core::theme::componentTokens(t))
                        .source(rowId + ".name")
                        .value(row.dirRel.empty() ? row.relPath : row.dirRel)
                        .anchor(x + std::max(scaled(20.0f), nameW) * 0.5f,
                                rowH * 0.5f)
                        .bounds(contentWidth, rowH)
                        .minWidth(120.0f)
                        .maxWidth(420.0f)
                        .zIndex(40)
                        .build();
                }
                // D3-A：点击=展开/收起，永不进 diff（untracked=lazy 枚举触发点）
                const std::string rel = row.relPath;
                if (untracked) {
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
                // §6.2：>500 截断尾行「还有 N 个文件 · 在搜索中查看全部」
                float x = indent + scaled(14.0f);
                ui.text(rowId + ".text")
                    .position(x, 0.0f)
                    .size(contentWidth - x - scaled(12.0f), rowH)
                    .text("还有 " + std::to_string(row.remainCount) +
                          " 个文件 · 在搜索中查看全部")
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
            float x = std::max(scaled(kIndentBase), indent);
            if (row.status.has_value()) {
                drawBadge(ui, rowId + ".badge", x,
                          (rowH - scaled(kBadgeSize)) * 0.5f, *row.status, t, gc);
            }
            x += scaled(kBadgeSize) + scaled(8.0f);
            // 文件名：树形态=短名；平铺形态=短名+目录弱化段（§4.5/§4.7）
            const float trailingW =
                flat ? scaled(60.0f) : scaled(12.0f);
            const float nameW =
                std::max(scaled(24.0f), contentWidth - x - trailingW);
            ui.text(rowId + ".name")
                .position(x, 0.0f)
                .size(nameW, rowH)
                .text(row.name)
                .fontSize(scaled(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textPrimary)
                .build();
            if (flat) {
                // 平铺：文件名后目录弱化段 12px textTertiary（§4.5 路径弱化段）
                std::string dirSeg;
                if (row.relPath.size() > row.name.size() + 1) {
                    dirSeg = row.relPath.substr(
                        0, row.relPath.size() - row.name.size() - 1);
                }
                if (!dirSeg.empty()) {
                    ui.text(rowId + ".dir")
                        .position(x + textWidthEstimate(row.name, scaled(13.0f)) +
                                      scaled(8.0f),
                                  0.0f)
                        .size(std::max(scaled(20.0f),
                                       contentWidth - x -
                                           textWidthEstimate(row.name, scaled(13.0f)) -
                                           scaled(20.0f)),
                              rowH)
                        .text(dirSeg)
                        .fontSize(scaled(12.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
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
            } else if (textWidthEstimate(row.relPath, scaled(13.0f)) > nameW) {
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
    const float collapseBtnX = x + w - scaled(8.0f) - btnSize;
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

    const float listY = y + headH;
    const float listH = h - headH;
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
            // F-08② 命中行常亮底色（--c-selected-bg 同款；无淡出动画——need:14
            // 推迟项先例，D7-A diff 侧同口径）
            if (c.contentHighlightLine() == static_cast<int>(lineIdx) + 1) {
                ui.rect(rowId + ".hlbg")
                    .size(contentWidth, rowH)
                    .color(t.colors.active)
                    .build();
            }
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
    // F-08② 内容锚滚动（need:14）：jumpToHit 记 pendingContentLine_，Ready 后
    // 按行盒几何现算目标 offset 一次性赋值（锚=行号 → 目标滚动位；clue#14：
    // 禁 setScrollOffset 偏移量直设——下方 .offset() 状态绑定是唯一滚动通道）
    if (c.pendingContentLine() && !lines.empty()) {
        const int lineNo =
            std::clamp(*c.pendingContentLine(), 1, static_cast<int>(lines.size()));
        c.clearPendingContentLine();
        const float rowH2 = scaled(kCodeRowH);
        const float maxOffset =
            std::max(0.0f, static_cast<float>(lines.size()) * rowH2 - h);
        c.setContentScrollOffset(std::clamp(static_cast<float>(lineNo - 1) * rowH2 -
                                                h * 0.35f,
                                            0.0f, maxOffset));
    }
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
            // D7-A diff 命中行常亮底色（newNo 匹配；need:14 推迟项先例同口径）
            const auto pendingNo = line.newNo;
            const bool highlighted =
                c.diffHighlightLine() > 0 && pendingNo.has_value() &&
                *pendingNo == c.diffHighlightLine();
            if (highlighted) {
                ui.rect(rowId + ".hlbg")
                    .size(contentWidth, rowH)
                    .color(t.colors.active)
                    .build();
            }
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
    const float padX = scaled(16.0f);
    const bool searchView = c.searchContentView();
    const auto selStatus = c.selectedRowStatus();

    // 头部（§5.2 徽标 + 路径条 + 统计 + tab；§6.8 搜索独立视图=仅路径+「内容」）
    float hx = x + padX;
    if (!searchView && selStatus.has_value()) {
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
    const float tabZoneW = searchView ? tabW : tabW * 2 + scaled(16.0f);
    const float pathW = std::max(
        scaled(40.0f),
        w - (hx - x) - padX - statsW - tabZoneW - scaled(16.0f));
    ui.text("git.right.head.path")
        .position(hx, y)
        .size(pathW, headH)
        .text(pathText)
        .fontFamily("monospace")
        .fontSize(scaled(13.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
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
    if (searchView) {
        // §6.8 独立内容视图：「内容」恒激活无切换（文字钮非 tab 语义）
        underlineTab(ui, "git.right.head.tab.content.only", "内容", true,
                     x + w - padX - tabW, y, tabW, headH, t, nullptr);
    } else {
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
    if (searchView) {
        // §6.8：命中∉变更列表 → 独立内容视图（头部仅路径已画）
        composeContentBody(ui, x, bodyY, w, bodyH, c, t, gc);
    } else if (c.statusLoading() && rows.empty()) {
        composeRightEmpty(ui, x, bodyY, w, bodyH, kGlyphCodeBranch,
                          "选择左侧文件查看差异", "正在扫描变更…", t);
    } else if (rows.empty() && c.statusError().empty()) {
        // 工作区干净空态（§5.7：图标+主/副文案+[刷新]；空仓同构文案演绎统一，
        // platform/git 无提交数查询接口）
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

// ---- 全局搜索结果浮层（need:14 §7.2 四态增补；锚定搜索框下方）----
// None / 收起 → 不显示；Scanning=进度头 + 渐进出果；Done=计数头 + 结果/空态；
// Error=错误块。失焦收起：浮层外全屏透明 scrim（点击浮层外=收起，扫描不取消）。
void composeSearchPopup(eui::Ui& ui, float w, float h, GitPageController& c,
                        const Tokens& t) {
    using SearchState = GitPageController::GitSearchState;
    const SearchState st = c.searchState();
    if (st == SearchState::None || !c.searchPopupVisible()) return;

    const auto& results = c.searchResults();
    const float popupW = std::min(scaled(560.0f), w - scaled(32.0f));
    const bool narrow = w < kNarrowBreakpoint;
    const auto [boxX, boxY] = searchBoxGeometry(w, narrow);
    (void)boxY;
    float px = boxX;
    if (px + popupW > w - scaled(16.0f)) px = w - scaled(16.0f) - popupW;
    const float py = kTopBarH + scaled(4.0f);

    const float headerH = scaled(32.0f);
    const float rowH = scaled(28.0f);
    float bodyH;
    if (st == SearchState::Error) {
        bodyH = scaled(96.0f);
    } else if (results.empty()) {
        bodyH = scaled(72.0f);
    } else {
        bodyH = std::min(scaled(448.0f), static_cast<float>(results.size()) * rowH);
    }
    const float popupH = headerH + bodyH;

    // 失焦收起（扫描不取消：后台继续出果，spinner 指示，再点框看进度）：
    // 三面 scrim 带（左/右/下，从顶栏底缘起）——顶栏条不盖，搜索框保持可点
    //（refocus/清除 ×/切仓均可用；顶栏点击不关浮层）
    const float bandTop = kTopBarH;
    hitRect(ui, "git.search.scrim.left", 0.0f, bandTop, std::max(0.0f, px), h - bandTop,
            [&c] { c.setSearchPopupVisible(false); });
    hitRect(ui, "git.search.scrim.right", px + popupW, bandTop,
            std::max(0.0f, w - px - popupW), h - bandTop,
            [&c] { c.setSearchPopupVisible(false); });
    hitRect(ui, "git.search.scrim.bottom", px, py + popupH,
            std::min(popupW, std::max(0.0f, w - px)), std::max(0.0f, h - py - popupH),
            [&c] { c.setSearchPopupVisible(false); });

    ui.stack("git.search.popup")
        .position(px, py)
        .size(popupW, popupH)
        .zIndex(50)
        .content([&] {
            ui.rect("git.search.popup.bg")
                .size(popupW, popupH)
                .color(t.colors.surface)
                .radius(8.0f)
                .border(1.0f, t.colors.borderDefault)
                .build();
            // 计数头：扫描态「正在扫描… x/y 文件」；终态「N 处命中 · M 个文件」
            //（truncated 追加细化提示；增量 flush 期间扫描头即随结果更新）
            std::string headText;
            if (st == SearchState::Scanning) {
                headText = "正在扫描… " + std::to_string(c.searchScanned()) + "/" +
                           std::to_string(c.searchTotalFiles()) + " 文件";
            } else if (st == SearchState::Error) {
                headText = "搜索失败";
            } else {
                headText = std::to_string(results.size()) + " 处命中 · " +
                           std::to_string(c.searchFileCount()) + " 个文件";
                if (c.searchTruncated()) {
                    headText += " · 结果过多，仅显示前 200——请细化关键词";
                }
            }
            ui.text("git.search.popup.head")
                .position(scaled(12.0f), 0.0f)
                .size(popupW - scaled(24.0f), headerH)
                .text(headText)
                .fontSize(scaled(12.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(t.colors.textSecondary)
                .build();
            ui.rect("git.search.popup.hborder")
                .position(0.0f, headerH - 1.0f)
                .size(popupW, 1.0f)
                .color(t.colors.borderMuted)
                .build();

            const float bodyY = headerH;
            if (st == SearchState::Error) {
                // 错误块（列表卡错误块参数缩档）：danger-tint 底 + 主文案 + 原因
                ui.rect("git.search.popup.errbg")
                    .position(scaled(12.0f), bodyY + scaled(12.0f))
                    .size(popupW - scaled(24.0f), bodyH - scaled(24.0f))
                    .color(components::theme::withOpacity(t.colors.error, 0.12f))
                    .radius(6.0f)
                    .build();
                ui.text("git.search.popup.errmain")
                    .position(scaled(24.0f), bodyY + scaled(22.0f))
                    .size(popupW - scaled(48.0f), scaled(20.0f))
                    .text("无法完成搜索")
                    .fontSize(scaled(13.0f))
                    .color(t.colors.errorText)
                    .build();
                ui.text("git.search.popup.errreason")
                    .position(scaled(24.0f), bodyY + scaled(44.0f))
                    .size(popupW - scaled(48.0f), scaled(18.0f))
                    .text(c.searchError())
                    .fontSize(scaled(12.0f))
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }
            if (results.empty()) {
                ui.text("git.search.popup.empty")
                    .position(0.0f, bodyY)
                    .size(popupW, bodyH)
                    .text(st == SearchState::Scanning ? "正在扫描，结果将逐步显示"
                                                      : "未找到匹配内容")
                    .fontSize(scaled(13.0f))
                    .horizontalAlign(eui::HorizontalAlign::Center)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .build();
                return;
            }
            // 结果行（virtualList 定高 28px 槽内压缩双行）：上行 relPath:行号
            // 11px textTertiary（moreInFile 追加提示）；下行 preview 12px
            // textPrimary、match 段 primary runs（≤200 条 UI 线程现算）
            components::virtualList(ui, "git.search.results")
                .position(0.0f, bodyY)
                .size(popupW, bodyH)
                .itemCount(static_cast<int64_t>(results.size()))
                .rowHeight(rowH)
                .step(rowH)
                .overscanViewports(1.0f)
                .theme(core::theme::componentTokens(t))
                .offset(c.searchPopupScroll())
                .onChange([&c](float v) { c.setSearchPopupScroll(v); })
                .row([&](eui::Ui& inner, const std::string& rowId, int64_t index,
                         float contentWidth, float vh) {
                    (void)vh;
                    const auto idx = static_cast<size_t>(index);
                    if (idx >= results.size()) return;
                    const SearchHit& hit = results[idx];
                    std::string pathLine = hit.relPath + ":" + std::to_string(hit.line);
                    if (hit.moreInFile) pathLine += " · 此文件还有更多";
                    inner.text(rowId + ".path")
                        .position(scaled(12.0f), scaled(2.0f))
                        .size(contentWidth - scaled(24.0f), scaled(13.0f))
                        .text(pathLine)
                        .fontSize(scaled(11.0f))
                        .verticalAlign(eui::VerticalAlign::Center)
                        .color(t.colors.textTertiary)
                        .build();
                    std::vector<eui::TextRun> runs;
                    if (hit.matchLen > 0 &&
                        hit.matchStart + hit.matchLen <= hit.preview.size()) {
                        runs.push_back({static_cast<int>(hit.matchStart),
                                        static_cast<int>(hit.matchStart + hit.matchLen),
                                        t.colors.primary});
                    }
                    auto previewText = inner.text(rowId + ".preview")
                                           .position(scaled(12.0f), scaled(14.0f))
                                           .size(contentWidth - scaled(24.0f), scaled(14.0f))
                                           .text(hit.preview)
                                           .fontSize(scaled(12.0f))
                                           .verticalAlign(eui::VerticalAlign::Center)
                                           .color(t.colors.textPrimary);
                    if (!runs.empty()) previewText.runs(runs);
                    previewText.build();
                    // 点击结果行 → F-08② 定位（浮层保持开，渐进出果价值）。
                    // P2-4：results 是 controller 活引用——命中区注册与点击派发间
                    // results 可被清（ESC 取消/新扫描重置），派发时重查边界防 OOB
                    inner.rect(rowId + ".hit")
                        .size(contentWidth, rowH)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .onClick([&c, &results, idx] {
                            if (idx < results.size()) c.jumpToHit(results[idx]);
                        })
                        .build();
                })
                .build();
        })
        .build();
    (void)h;
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

    // 两栏构图（§1：页边 12 + 左卡 320 可折叠 + gap 12 + 右卡余宽）
    const float bodyY = kTopBarH + kPageMargin;
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

    // 全局搜索结果浮层（need:14）：compose 在两卡之后 = z 序最上（scrim 同层）
    composeSearchPopup(ui, w, h, c, t);
}

} // namespace module::git
