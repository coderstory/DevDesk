// src/module_git/git_views.cpp
#include "git_views.h"

#include "git_colors.h"

#include <components/input.h>
#include <components/theme.h>
#include <components/tooltip.h>
#include <components/virtuallist.h>
#include <core/component_theme.h>

#include <algorithm>
#include <string>
#include <utility>

namespace module::git {

namespace {

using core::theme::scaled;
using core::theme::Tokens;

// ---- UI 方案 A §2/§4 尺寸常量（字面量基准，经 scaled() 随全局字号缩放）----
constexpr float kTopBarH = 48.0f;
constexpr float kModeBarH = 40.0f;
constexpr float kGroupHeaderH = 32.0f;
constexpr float kPathBarH = 36.0f;
constexpr float kTreeRowH = 28.0f;
constexpr float kListRowH = 28.0f;
constexpr float kIndentStep = 16.0f;   // 树每层缩进
constexpr float kIndentBase = 12.0f;
constexpr float kBadgeSize = 16.0f;
constexpr float kNarrowBreakpoint = 1100.0f;  // §9：≤1100 档左 200/中 240
constexpr float kLeftColWide = 260.0f;
constexpr float kLeftColNarrow = 200.0f;
constexpr float kMidColWide = 280.0f;
constexpr float kMidColNarrow = 240.0f;
// §4/§5.5/§5.6 代码面（等宽 13px、行高 22px；diff 行号 12px、hunk 头/折叠条 24px）
constexpr float kCodeRowH = 22.0f;          // 内容/ diff 代码行高
constexpr float kDiffSlotH = 24.0f;         // diff virtualList 定高槽（行画槽内 22px，
                                            // hunk 头/折叠条满槽——EUI 定高约束）
constexpr float kContentGutterW = 44.0f;    // 内容行号列宽
constexpr float kDiffFoldW = 16.0f;         // diff 折叠符列宽
constexpr float kDiffNoW = 40.0f;           // diff 旧/新行号列宽
constexpr float kDiffFileHeaderH = 28.0f;   // diff 文件头高
constexpr float kCodeGutterPad = 8.0f;      // 行号右 padding

// 文本宽度估算（wtabTextWidth 同式：CJK 0.43 / ASCII 0.56 × 字号 + 1.6 基宽；
// 偏大只多留白，用于固定头内元素定位，不做逐字截断）
float textWidthEstimate(const std::string& text, float fontSize) {
    float w = fontSize * 1.6f;
    for (unsigned char c : text) {
        w += (c >= 0x80) ? fontSize * 0.43f : fontSize * 0.56f;
    }
    return w;
}

// 等宽代码宽估算（F-07⑥ 横滚内容宽；ASCII 0.62em，CJK 按 2 格 1.1em；
// 偏大只多留白——横滚尾部多出的空白不影响「不换行错位」验收）
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

// 下划线式 Tab（§5.2 模式切换条 / §5.4 视图切换：激活=主色 2px 底线；方案 A 明确
// Tab 形态禁实心按钮——ui_kit::wtab 为块面式，按方案 A 自绘下划线式）
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

// ---- 空态页（§5.7）----
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
        .text("只读查看器：浏览文件、查看变更与 diff，不做任何修改。")
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

// ---- 顶栏（§5.1 + §5.1a 全局搜索框 need:14 增补，决策① v2 反转落点）----

// 搜索框几何（320×32，弹性间隔位=路径右侧、「切换仓库」左侧；浮层锚同源）
std::pair<float, float> searchBoxGeometry(float w) {
    const float searchW = scaled(320.0f);
    const float searchH = scaled(32.0f);
    const float switchW = textWidthEstimate("切换仓库", scaled(14.0f)) + scaled(24.0f);
    const float refreshW = textWidthEstimate("↻ 刷新", scaled(14.0f)) + scaled(24.0f);
    const float x = w - scaled(16.0f) - refreshW - scaled(12.0f) - switchW -
                    scaled(12.0f) - searchW;
    return {x, (kTopBarH - searchH) * 0.5f};
}

void composeTopBar(eui::Ui& ui, float w, GitPageController& c, const Tokens& t) {
    ui.rect("git.top.bg").size(w, kTopBarH).color(t.colors.surface).build();
    ui.rect("git.top.border")
        .position(0.0f, kTopBarH)
        .size(w, 1.0f)
        .color(t.colors.borderDefault)
        .build();

    const float padX = scaled(16.0f);
    const std::string name = c.repoDisplayName();
    const float nameW = textWidthEstimate(name, scaled(15.0f));
    ui.text("git.top.name")
        .position(padX, 0.0f)
        .size(nameW + scaled(8.0f), kTopBarH)
        .text(name)
        .fontSize(scaled(15.0f))
        .fontWeight(600)
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textPrimary)
        .build();

    float x = padX + nameW + scaled(8.0f) + scaled(12.0f);
    const auto [searchBoxX, searchBoxY] = searchBoxGeometry(w);
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
    // placeholder 13px textTertiary；Enter 提交 / ESC 显式取消 / 清空=取消同路）
    const float searchW = scaled(320.0f);
    const float searchH = scaled(32.0f);
    components::input(ui, "git.search.input")
        .position(searchBoxX, searchBoxY)
        .size(searchW, searchH)
        .value(c.searchDraft())
        .placeholder("搜索全仓内容…")
        .fontSize(scaled(13.0f))
        .theme(core::theme::componentTokens(t))
        .onChange([&c](const std::string& v) {
            c.setSearchDraft(v);
            c.markSearchEdited();
        })
        .onEnter([&c] { c.submitSearchNow(); })
        .onCancel([&c] { c.cancelSearch(); })
        .build();
    // 右侧指示：扫描中 ↻（pulse 驱动持续出帧；§5.1a「旋转圈」EUI 无旋转原语，
    // 落地为 pulse 字形——参数同档 12px textTertiary）；有草稿时 ×（清空+取消）
    const float badgeX = searchBoxX + searchW - scaled(22.0f);
    if (c.searchState() == GitPageController::GitSearchState::Scanning) {
        ui.text("git.search.spin")
            .position(badgeX, searchBoxY)
            .size(scaled(20.0f), searchH)
            .icon(0xF021)
            .fontSize(scaled(12.0f))
            .opacity(skeletonPulseOpacity())
            .onFrame([](float) {})
            .horizontalAlign(eui::HorizontalAlign::Center)
            .verticalAlign(eui::VerticalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
    } else if (!c.searchDraft().empty()) {
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

// ---- 树行渲染（§5.2；行盒 = （行, contentWidth）纯函数）----
void drawTreeRow(eui::Ui& ui, const std::string& rowId, const TreeRow& row,
                 float contentWidth, GitPageController& c, const Tokens& t,
                 const GitColors& gc) {
    const float rowH = scaled(kTreeRowH);
    ui.stack(rowId)
        .size(contentWidth, rowH)
        .content([&] {
            const float indent = scaled(kIndentBase + kIndentStep * row.depth);
            const bool selected =
                row.kind == TreeRowKind::File &&
                c.selectedPath() == row.relPath && !c.rightShowsDiff();
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
            // 展开符：目录 ▶/▼（懒加载中省略号占位；叶/降级行同宽空占）
            const float arrowW = scaled(16.0f);
            const bool showArrow = row.kind == TreeRowKind::Directory;
            if (showArrow) {
                const char* glyph = row.loading ? "…" : (row.expanded ? "▼" : "▶");
                ui.text(rowId + ".arrow")
                    .position(x, 0.0f)
                    .size(arrowW, rowH)
                    .text(glyph)
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            }
            x += arrowW;

            if (row.kind == TreeRowKind::Error) {
                // 行内错误行 + 重试（§5.2）
                ui.text(rowId + ".err")
                    .position(x, 0.0f)
                    .size(contentWidth - x - scaled(48.0f), rowH)
                    .text("无法读取：" + row.errorText)
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
                        [&c, rel = row.relPath] { c.retryDir(rel); });
                return;
            }

            // 名称（变更行着状态色）
            const bool hasStatus = row.status.has_value();
            eui::Color nameColor = t.colors.textPrimary;
            if (hasStatus) {
                switch (*row.status) {
                    case platform::git::StatusKind::New: nameColor = gc.added; break;
                    case platform::git::StatusKind::Modified: nameColor = gc.modified; break;
                    case platform::git::StatusKind::Deleted: nameColor = gc.deleted; break;
                    case platform::git::StatusKind::Renamed: nameColor = gc.renamed; break;
                    case platform::git::StatusKind::Untracked: nameColor = gc.untracked; break;
                    default: break;
                }
            }
            if (row.kind == TreeRowKind::EmptyDir) {
                ui.text(rowId + ".name")
                    .position(x, 0.0f)
                    .size(contentWidth - x - scaled(12.0f), rowH)
                    .text("空目录")
                    .fontSize(scaled(12.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
                return;
            }
            const float nameW =
                contentWidth - x - scaled(12.0f) -
                (row.kind == TreeRowKind::NoAccess || row.kind == TreeRowKind::Subrepo
                     ? scaled(64.0f)
                     : (hasStatus ? scaled(16.0f) : 0.0f));
            ui.text(rowId + ".name")
                .position(x, 0.0f)
                .size(std::max(scaled(20.0f), nameW), rowH)
                .text(row.name)
                .fontSize(scaled(13.0f))
                .verticalAlign(eui::VerticalAlign::Center)
                .color(row.kind == TreeRowKind::NoAccess ? t.colors.textTertiary
                                                         : nameColor)
                .build();

            // 超长名截断 + 悬浮全名（PRD §5 目录树成功格边界；EUI tooltip 组件，
            // hoverOpacityFrom(source) 驱动显隐；气泡 zIndex 逃逸行栈）
            if (textWidthEstimate(row.name, scaled(13.0f)) > nameW) {
                components::tooltip(ui, rowId + ".name.tip")
                    .theme(core::theme::componentTokens(t))
                    .source(rowId + ".name")
                    .value(row.name)
                    .anchor(x + std::max(scaled(20.0f), nameW) * 0.5f,
                            rowH * 0.5f)
                    .bounds(contentWidth, rowH)
                    .minWidth(120.0f)
                    .maxWidth(420.0f)
                    .zIndex(40)
                    .build();
            }

            // 降级标注（无权限/子仓库）
            if (row.kind == TreeRowKind::NoAccess) {
                ui.text(rowId + ".tag")
                    .position(contentWidth - scaled(76.0f), 0.0f)
                    .size(scaled(64.0f), rowH)
                    .text("不可访问")
                    .fontSize(scaled(12.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            } else if (row.kind == TreeRowKind::Subrepo) {
                ui.text(rowId + ".tag")
                    .position(contentWidth - scaled(76.0f), 0.0f)
                    .size(scaled(64.0f), rowH)
                    .text("子仓库")
                    .fontSize(scaled(12.0f))
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textTertiary)
                    .build();
            } else if (hasStatus) {
                // 变更行右缘 6px 同色圆点（着色+点双通道，PRD §6）
                ui.rect(rowId + ".dot")
                    .position(contentWidth - scaled(14.0f), (rowH - scaled(6.0f)) * 0.5f)
                    .size(scaled(6.0f), scaled(6.0f))
                    .color(nameColor)
                    .radius(3.0f)
                    .build();
            }

            // 点击：目录=展开/折叠；文件=选中切内容视图；降级行无反应
            if (row.kind == TreeRowKind::Directory) {
                hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, rowH,
                        [&c, rel = row.relPath] { c.toggleDir(rel); });
            } else if (row.kind == TreeRowKind::File) {
                hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, rowH,
                        [&c, rel = row.relPath] { c.selectTreeFile(rel); });
            }
        })
        .build();
}

// ---- 左栏（§5.2）----
void composeLeftColumn(eui::Ui& ui, float x, float w, float y, float h,
                       GitPageController& c, const Tokens& t) {
    ui.rect("git.left.bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.sidebarBg)
        .build();
    ui.rect("git.left.border")
        .position(x + w, y)
        .size(1.0f, h)
        .color(t.colors.borderDefault)
        .build();

    // 模式切换条（两 Tab 各 50% 等分，下划线式）
    const auto mode = c.tree().mode();
    const float tabW = w * 0.5f;
    underlineTab(ui, "git.left.tab.all", "全部文件",
                 mode == GitTreeModel::Mode::AllFiles, x, y, tabW, scaled(kModeBarH),
                 t, [&c] { c.setTreeMode(GitTreeModel::Mode::AllFiles); });
    underlineTab(ui, "git.left.tab.changes", "仅修改",
                 mode == GitTreeModel::Mode::ChangesOnly, x + tabW, y, tabW,
                 scaled(kModeBarH), t,
                 [&c] { c.setTreeMode(GitTreeModel::Mode::ChangesOnly); });
    ui.rect("git.left.modebar.border")
        .position(x, y + scaled(kModeBarH))
        .size(w, 1.0f)
        .color(t.colors.borderDefault)
        .build();

    const float listY = y + scaled(kModeBarH) + scaled(4.0f);
    const float listH = h - scaled(kModeBarH) - scaled(8.0f);
    const auto& rows = c.visibleRows();  // 成员引用：行闭包布局期回调仍有效
    const bool rootLoaded =
        mode == GitTreeModel::Mode::ChangesOnly || c.tree().rootLoaded();

    if (mode == GitTreeModel::Mode::AllFiles && !rootLoaded) {
        // 根层懒加载中：骨架行（§5.8 左栏 6 条）
        float sy = listY + scaled(8.0f);
        float widths[] = {0.62f, 0.78f, 0.70f, 0.85f, 0.66f, 0.74f};
        int depthSteps[] = {0, 1, 1, 2, 0, 1};
        for (int i = 0; i < 6; ++i) {
            const float sw = (w - scaled(24.0f)) * widths[i];
            skeletonRow(ui, "git.left.skel." + std::to_string(i),
                        x + scaled(12.0f) +
                            scaled(kIndentStep * depthSteps[i]),
                        sy, sw, scaled(16.0f), t);
            sy += scaled(24.0f);
        }
        return;
    }
    if (rows.empty()) {
        const char* emptyText =
            mode == GitTreeModel::Mode::ChangesOnly ? "工作区无变更" : "无可显示文件";
        ui.text("git.left.empty")
            .position(x, listY)
            .size(w, scaled(40.0f))
            .text(emptyText)
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        return;
    }
    components::virtualList(ui, "git.tree.list")
        .position(x, listY)
        .size(w, listH)
        .itemCount(static_cast<int64_t>(rows.size()))
        .rowHeight(scaled(kTreeRowH))
        .step(scaled(kTreeRowH))
        .overscanViewports(1.0f)
        .theme(core::theme::componentTokens(t))
        .offset(c.treeScrollOffset())
        .onChange([&c](float v) { c.setTreeScrollOffset(v); })
        .row([&](eui::Ui& inner, const std::string& rowId, int64_t index,
                 float contentWidth, float rowHeight) {
            (void)rowHeight;
            const auto idx = static_cast<size_t>(index);
            if (idx >= rows.size()) return;
            drawTreeRow(inner, rowId, rows[idx], contentWidth, c, t,
                        gitColors(t));
        })
        .build();
}

// ---- 中栏条目行（§5.3）----
void drawChangeRow(eui::Ui& ui, const std::string& rowId, const ChangeRow& row,
                   float contentWidth, GitPageController& c, const Tokens& t,
                   const GitColors& gc) {
    const float rowH = scaled(kListRowH);
    ui.stack(rowId)
        .size(contentWidth, rowH)
        .content([&] {
            const bool selected = c.selectedPath() == row.path && c.rightShowsDiff();
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
            const float padX = scaled(12.0f);
            // 状态徽标 16×16（A/M/D/R 彩底彩字；U 灰底灰字描边）
            eui::Color badgeBg = t.colors.surfaceElevated;
            eui::Color badgeFg = gc.untracked;
            switch (row.kind) {
                case platform::git::StatusKind::New: badgeBg = gc.addedBg; badgeFg = gc.added; break;
                case platform::git::StatusKind::Modified: badgeBg = gc.modifiedBg; badgeFg = gc.modified; break;
                case platform::git::StatusKind::Deleted: badgeBg = gc.deletedBg; badgeFg = gc.deleted; break;
                case platform::git::StatusKind::Renamed: badgeBg = gc.renamedBg; badgeFg = gc.renamed; break;
                default: break;
            }
            ui.rect(rowId + ".badge")
                .position(padX, (rowH - scaled(kBadgeSize)) * 0.5f)
                .size(scaled(kBadgeSize), scaled(kBadgeSize))
                .color(badgeBg)
                .radius(4.0f)
                .border(row.kind == platform::git::StatusKind::Untracked
                            ? 1.0f
                            : 0.0f,
                        t.colors.borderDefault)
                .build();
            ui.text(rowId + ".badge.text")
                .position(padX, (rowH - scaled(kBadgeSize)) * 0.5f)
                .size(scaled(kBadgeSize), scaled(kBadgeSize))
                .text(std::string(1, statusBadgeLetter(row.kind)))
                .fontSize(scaled(10.0f))
                .fontWeight(700)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .color(badgeFg)
                .build();

            const float textX = padX + scaled(kBadgeSize) + scaled(8.0f);
            const float textW = contentWidth - textX - scaled(12.0f);
            // 重命名两行式压缩档（主行新名 13px + 副行旧名 11px，共 28px 行高制；
            // 方案 A 40px 两行需变高行 virtualList——EUI 虚拟列表定高，压缩排版）
            if (!row.oldPath.empty()) {
                ui.text(rowId + ".path")
                    .position(textX, 0.0f)
                    .size(textW, scaled(16.0f))
                    .text(row.path)
                    .fontSize(scaled(13.0f))
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(rowId + ".oldpath")
                    .position(textX, scaled(15.0f))
                    .size(textW, scaled(12.0f))
                    .text("← 原 " + row.oldPath)
                    .fontSize(scaled(11.0f))
                    .color(t.colors.textTertiary)
                    .build();
            } else {
                ui.text(rowId + ".path")
                    .position(textX, 0.0f)
                    .size(textW, rowH)
                    .text(row.path)
                    .fontSize(scaled(13.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textPrimary)
                    .build();
            }
            hitRect(ui, rowId + ".hit", 0.0f, 0.0f, contentWidth, rowH,
                    [&c, p = row.path] { c.selectChange(p); });
        })
        .build();
}

// ---- 中栏（§5.3）----
void composeMiddleColumn(eui::Ui& ui, float x, float w, float y, float h,
                         GitPageController& c, const Tokens& t) {
    ui.rect("git.mid.bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.surface)
        .build();
    ui.rect("git.mid.border")
        .position(x, y)
        .size(1.0f, h)
        .color(t.colors.borderDefault)
        .build();

    const auto& rows = c.changeRows();
    if (c.statusLoading() && rows.empty()) {
        // 加载态：5 条骨架行（§5.3 加载态）
        float sy = y + scaled(12.0f);
        float widths[] = {0.90f, 0.70f, 0.80f, 0.60f, 0.75f};
        for (int i = 0; i < 5; ++i) {
            skeletonRow(ui, "git.mid.skel." + std::to_string(i),
                        x + scaled(12.0f), sy, (w - scaled(24.0f)) * widths[i],
                        scaled(20.0f), t);
            sy += scaled(28.0f);
        }
        return;
    }
    if (!c.statusError().empty() && rows.empty()) {
        ui.text("git.mid.error")
            .position(x + scaled(12.0f), y + scaled(12.0f))
            .size(w - scaled(24.0f), scaled(60.0f))
            .text("无法获取变更：" + c.statusError())
            .fontSize(scaled(13.0f))
            .lineHeight(scaled(20.0f))
            .wrap(true)
            .color(t.colors.errorText)
            .build();
        return;
    }
    if (rows.empty()) {
        // 空态（§5.3）：主文案 + 节流说明
        ui.text("git.mid.empty")
            .position(x, y + h * 0.4f)
            .size(w, scaled(20.0f))
            .text("工作区无变更")
            .fontSize(scaled(13.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textSecondary)
            .build();
        ui.text("git.mid.empty.hint")
            .position(x, y + h * 0.4f + scaled(24.0f))
            .size(w, scaled(18.0f))
            .text("保存文件后列表会自动更新（1s 节流）")
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        return;
    }

    // 混排行模型：组头 + 条目行（composePump 已物化为成员，行闭包布局期回调仍有效）
    const auto& items = c.changeListItems();
    const size_t changedN = c.changedCount();
    const size_t untrackedN = c.untrackedCount();

    const float rowH = scaled(kListRowH);
    const float headerH = scaled(kGroupHeaderH);
    // 定高行制：组头行 = rowH 槽内画 32px 头（EUI virtualList 定高约束）
    components::virtualList(ui, "git.changes.list")
        .position(x + 1.0f, y)
        .size(w - 1.0f, h)
        .itemCount(static_cast<int64_t>(items.size()))
        .rowHeight(rowH)
        .step(rowH)
        .overscanViewports(1.0f)
        .theme(core::theme::componentTokens(t))
        .row([&](eui::Ui& inner, const std::string& rowId, int64_t index,
                 float contentWidth, float vh) {
            (void)vh;
            const auto idx = static_cast<size_t>(index);
            if (idx >= items.size()) return;
            const GitPageController::ChangeListItem& item = items[idx];
            if (item.header) {
                const bool isChanged = item.rowIndex == 0;
                inner.rect(rowId + ".hdbg")
                    .size(contentWidth, headerH)
                    .color(t.colors.surface)
                    .build();
                inner.rect(rowId + ".hdborder")
                    .position(0.0f, headerH)
                    .size(contentWidth, 1.0f)
                    .color(t.colors.borderMuted)
                    .build();
                inner.text(rowId + ".hdtext")
                    .position(scaled(12.0f), 0.0f)
                    .size(contentWidth - scaled(24.0f), headerH)
                    .text(isChanged ? GitChangesModel::groupHeader(ChangeGroup::Changed, changedN)
                                    : GitChangesModel::groupHeader(ChangeGroup::Untracked, untrackedN))
                    .fontSize(scaled(12.0f))
                    .fontWeight(600)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .build();
                return;
            }
            drawChangeRow(inner, rowId, rows[item.rowIndex], contentWidth, c, t,
                          gitColors(t));
        })
        .build();
}

// ---- 内容视图（§5.5：行号列 44px + 代码 runs；行槽 22px）----
void drawContentRow(eui::Ui& ui, const std::string& rowId, size_t lineIdx,
                    float contentWidth, const GitPageController& c,
                    const Tokens& t, const GitColors& gc) {
    const float rowH = scaled(kCodeRowH);
    ui.stack(rowId)
        .size(contentWidth, rowH)
        .content([&] {
            // F-08② 命中行常亮底色（--c-selected-bg 同款；无淡出动画——推迟项）
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

// ---- 内容视图体（§5.5/§7：四态占位 + 错误重试 + 行列表）----
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
        // 四态占位文案（§5.5）：主文案 14px --c-text-2 + 副文案 12px --c-text-3
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

// ---- diff 视图行（§5.6：四列 16/40/40/flex；行 22px 画 24px 槽内）----
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

// ---- diff 视图体（§5.6/§7：文件头 + hunk 折叠 + 巨 diff 行模型分块）----
void composeDiffBody(eui::Ui& ui, float x, float y, float w, float h,
                     GitPageController& c, const Tokens& t, const GitColors& gc) {
    const auto st = c.diffState();
    if (st == RightPaneState::None) return;

    // 文件头 28px（加载中照常画，§7 状态表「骨架+文件头照常」）
    const std::string headerText = c.diffModel().headerText().empty()
                                       ? c.diffPath()
                                       : c.diffModel().headerText();
    ui.rect("git.diff.header")
        .position(x, y)
        .size(w, scaled(kDiffFileHeaderH))
        .color(gc.diffGutterBg)
        .build();
    ui.rect("git.diff.header.border")
        .position(x, y + scaled(kDiffFileHeaderH))
        .size(w, 1.0f)
        .color(t.colors.borderMuted)
        .build();
    ui.text("git.diff.header.text")
        .position(x + scaled(12.0f), y)
        .size(w - scaled(24.0f), scaled(kDiffFileHeaderH))
        .text(headerText)
        .fontFamily("monospace")
        .fontSize(scaled(12.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(t.colors.textSecondary)
        .build();

    const float bodyY = y + scaled(kDiffFileHeaderH);
    const float bodyH = h - scaled(kDiffFileHeaderH);

    if (st == RightPaneState::Loading) {
        float sy = bodyY + scaled(12.0f);
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
        ui.text("git.diff.err.main")
            .position(x, bodyY + bodyH * 0.4f)
            .size(w, scaled(20.0f))
            .text("无法计算差异")
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.errorText)
            .build();
        ui.text("git.diff.err.reason")
            .position(x, bodyY + bodyH * 0.4f + scaled(26.0f))
            .size(w, scaled(18.0f))
            .text(c.diffError())
            .fontSize(scaled(12.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        const float btnW = 88.0f;
        secondaryButton(ui, "git.diff.retry", "重试",
                        x + (w - btnW) * 0.5f, bodyY + bodyH * 0.4f + scaled(52.0f),
                        btnW, scaled(32.0f), t, [&c] { c.retryDiff(); });
        return;
    }

    // Ready：untracked 超大占位（bug#18，F-11③ 同文案）/ 二进制占位 /
    // 无内容变更占位 / unified 行列表
    if (!c.diffModel().placeholder().empty()) {
        ui.text("git.diff.placeholder")
            .position(x, bodyY + bodyH * 0.4f)
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
            .position(x, bodyY + bodyH * 0.4f)
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
            .position(x, bodyY + bodyH * 0.4f)
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
        .position(x, bodyY)
        .size(w, bodyH)
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

// ---- 右栏（§5.4：路径条 + 视图切换 + 内容/diff 真身）----
void composeRightColumn(eui::Ui& ui, float x, float w, float y, float h,
                        GitPageController& c, const Tokens& t) {
    ui.rect("git.right.bg")
        .position(x, y)
        .size(w, h)
        .color(t.colors.surface)
        .build();

    // 路径条 36px：左路径右切换（内容｜diff，diff Tab 仅选中文件有变更时出现）
    ui.rect("git.right.pathbar.border")
        .position(x, y + scaled(kPathBarH))
        .size(w, 1.0f)
        .color(t.colors.borderDefault)
        .build();
    const std::string shown = c.selectedPath().empty() ? c.repoPathUtf8()
                                                       : c.selectedPath();
    ui.text("git.right.path")
        .position(x + scaled(12.0f), y)
        .size(w - scaled(220.0f), scaled(kPathBarH))
        .text(shown)
        .fontSize(scaled(12.0f))
        .verticalAlign(eui::VerticalAlign::Center)
        .color(c.selectedPath().empty() ? t.colors.textTertiary : t.colors.textSecondary)
        .build();

    const float tabH = scaled(kPathBarH);
    const float tabW = textWidthEstimate("内容", scaled(13.0f)) + scaled(20.0f);
    const float contentTabX = x + w - scaled(16.0f) - tabW * 2.0f - scaled(12.0f);
    underlineTab(ui, "git.right.tab.content", "内容", !c.rightShowsDiff(),
                 contentTabX, y, tabW, tabH, t, [&c] {
                     if (!c.selectedPath().empty()) {
                         c.selectTreeFile(c.selectedPath());
                     }
                 });
    if (c.selectedHasChange()) {
        underlineTab(ui, "git.right.tab.diff", "diff", c.rightShowsDiff(),
                     contentTabX + tabW + scaled(12.0f), y, tabW, tabH, t,
                     [&c] { c.selectChange(c.selectedPath()); });
    }

    // 视图体（批次三真身：N8 diff / N6 内容）
    const float bodyY = y + scaled(kPathBarH);
    const float bodyH = h - scaled(kPathBarH);
    const GitColors gc = gitColors(t);
    if (c.selectedPath().empty()) {
        ui.text("git.right.body.hint")
            .position(x, bodyY + bodyH * 0.4f)
            .size(w, scaled(20.0f))
            .text("在左侧选择文件查看内容与变更")
            .fontSize(scaled(14.0f))
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(t.colors.textTertiary)
            .build();
        return;
    }
    if (c.rightShowsDiff()) {
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
    const auto [boxX, boxY] = searchBoxGeometry(w);
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
                // 错误块（中栏错误块参数缩档）：danger-tint 底 + 主文案 + 原因
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
                    // 点击结果行 → F-08② 定位（浮层保持开，渐进出果价值）
                    inner.rect(rowId + ".hit")
                        .size(contentWidth, rowH)
                        .color(eui::Color{0.0f, 0.0f, 0.0f, 0.0f})
                        .onClick([&c, &results, idx] { c.jumpToHit(results[idx]); })
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
    const bool narrow = w < kNarrowBreakpoint;
    const float leftW = narrow ? kLeftColNarrow : kLeftColWide;
    const float midW = narrow ? kMidColNarrow : kMidColWide;
    const float bodyY = kTopBarH;

    composeTopBar(ui, w, c, t);
    composeLeftColumn(ui, 0.0f, leftW, bodyY, h - bodyY, c, t);
    composeMiddleColumn(ui, leftW, midW, bodyY, h - bodyY, c, t);
    composeRightColumn(ui, leftW + midW, w - leftW - midW, bodyY, h - bodyY, c, t);
    // 全局搜索结果浮层（need:14）：compose 在各栏之后 = z 序最上（scrim 同层）
    composeSearchPopup(ui, w, h, c, t);
}

} // namespace module::git
