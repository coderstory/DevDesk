// src/module_mcp/mcp_page_module.cpp
// MCP 菜单页实现：页头+服务状态行 → 工具卡片网格（ui.flow，列宽自适应 340–420）→
// 详情弹窗（components::dialog，关闭 = 按钮 + Esc + backdrop 三通道）→
// 「试一下」（execute 后台线程 + compose 轮询收割，禁 UI 线程直调）。
// 视觉基准 = 桌面 mcp-menu-demo.html（token 对齐，图标 FA7 码点；页内开关与
// 关闭置灰不照抄——关闭态 = 模块门控整页注销）。
#include "mcp_page_module.h"

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/theme.h"
#include "mcp_service_bridge.h"
#include "memory_mcp_bridge.h"

#include <core/platform/platform.h>  // core::platform::requestUiUpdate（唤醒主循环）

#include <components/button.h>
#include <components/dialog.h>
#include <components/scrollview.h>
#include <components/switch.h>
#include <components/theme.h>

#include <core/render/text.h>  // core::TextPrimitive::measureTextWidth

#include <eui/dsl.h>

#include <ui_kit/empty_state.h>

#include <algorithm>
#include <utility>

namespace module::mcp {

namespace {

// 工具卡列宽算法（2026-11-01 F-13）：
// 原为定宽 340，1600 窗内只排 3 张（1020px），右侧空 210px。现在列宽随内容宽
// 自适应：列数按「能放下多少列最小宽」取，卡宽 = (内容宽 - 列间距×(列数-1)) / 列数。
// 区间锁 340–420：太窄描述挤、太宽一行字飘。
constexpr float kCardMinW = 340.0f;
constexpr float kCardMaxW = 420.0f;
constexpr int kCardMaxCols = 5;
constexpr float kCardH = 120.0f;
constexpr float kDialogW = 600.0f;
constexpr float kDialogH = 480.0f;

// 卡内横向常量
constexpr float kCardPad = 16.0f;       // 卡内边距
constexpr float kCardIcon = 40.0f;      // 图标位
constexpr float kCardTextX = 68.0f;     // 文本列起点（图标右侧 12）
constexpr float kStatusGap = 16.0f;     // 文本列右端 ↔ 状态列左端（F-01 判据 ≥16）
constexpr float kChipW = 40.0f;         // 标签 chip 宽
constexpr float kChipH = 18.0f;
constexpr float kCtaW = 120.0f;         // hover CTA 宽（右对齐）

// 工具卡图标：按名字族派发 FA7 Free-Solid 码点（抓取=globe / 服务信息=server /
// 其余=cube）；码点均在 EUI-NEO 内置 FA7 整字体覆盖内。
unsigned int toolIconGlyph(const std::string& name) {
    if (name.find("fetch") != std::string::npos ||
        name.find("web") != std::string::npos) {
        return 0xF0AC;  // fa-globe
    }
    if (name.find("server") != std::string::npos ||
        name.find("info") != std::string::npos) {
        return 0xF233;  // fa-server
    }
    return 0xF1B2;  // fa-cube
}

// 结果区文案单源：运行中 / 尚未运行 / 成功前缀 / 失败前缀（测试按前缀断言）。
std::string resultDisplayText(bool running, bool hasResult, bool isError,
                              const std::string& text) {
    if (running) return "运行中…";
    if (!hasResult) return "（尚未运行）";
    return std::string(isError ? "失败：\n" : "成功：\n") + text;
}

// 列宽算法：先取「放得下多少列最小宽」的列数，再用剩余空间把列宽撑开；
// 撑过上限（420）就减列重算，避免宽窗里卡片被拉成横条。
std::pair<int, float> gridColumns(float contentW, float gap) {
    if (contentW <= 0.0f) return {1, kCardMinW};
    int cols = 1;
    for (int n = 1; n <= kCardMaxCols; ++n) {
        if (contentW >= n * kCardMinW + (n - 1) * gap) cols = n;
    }
    float cardW = (contentW - gap * (cols - 1)) / cols;
    if (cardW > kCardMaxW) {
        cols = std::max(1, static_cast<int>(
                               (contentW + gap) / (kCardMaxW + gap)));
        cardW = (contentW - gap * (cols - 1)) / cols;
    }
    return {cols, cardW};
}

} // namespace

McpPageModule::McpPageModule() : McpPageModule(::core::mcp::globalMcpTools()) {}

McpPageModule::McpPageModule(::core::mcp::McpToolRegistry& registry)
    : registry_(&registry) {}

McpPageModule::~McpPageModule() { joinTryThread(); }

::core::PageId McpPageModule::pageId() const { return ::core::PageId::Mcp; }

std::string McpPageModule::displayName() const { return "MCP"; }

std::string McpPageModule::shortName() const { return "MCP"; }

// fa-plug (U+F1E6) 的 UTF-8 字节；须与 app_registry 表行 iconGlyph 列同值。
std::string McpPageModule::iconGlyph() const { return "\xEF\x87\xA6"; }

void McpPageModule::compose(eui::Ui& ui, const eui::Screen& screen) {
    harvestTryRun();
    const auto& t = core::theme::current();
    const auto comp = core::theme::componentTokensWeChat(t);
    const float pad = t.sizes.spacingLarge * 2.0f;  // 32：页面外边距与他页同档
    const auto tools = registry_->list();           // 每帧快照（单锁，量小）

    // 服务状态行数据源：app::mcp 桥只读快照（本页不提供开关，开关在设置页）。
    const app::mcp::LiveStatus svc = app::mcp::status();

    // ⚠ 上游坑（home_module 同款）：column 流式子元素只看流式游标，flow 子元素
    // 必须显式 size()。整页挂 scrollView 矮窗滚动吸收。
    components::scrollView(ui, "mcp.scroll")
        .size(screen.width, screen.height)
        .theme(comp)
        .content([&](eui::Ui& ui, float innerW, float innerH) {
            (void)innerH;
            const float contentW = std::max(0.0f, innerW - pad * 2.0f);
            ui.column("mcp.root")
                .width(innerW)
                .height(core::SizeValue::wrapContent())
                .padding(pad, 0.0f, pad, t.sizes.spacingXLarge)
                .gap(t.sizes.spacingLarge)
                .content([&] {
                    ui.text("mcp.title")
                        .size(contentW, core::theme::scaled(32.0f))
                        .text("MCP 服务")
                        .fontSize(t.sizes.fontSizeTitle)
                        .fontWeight(600)
                        .color(t.colors.textPrimary)
                        .build();
                    ui.text("mcp.subtitle")
                        .size(contentW, core::theme::scaled(20.0f))
                        .text("将 DevDesk 内置能力以标准 MCP 协议（Streamable HTTP）"
                              "暴露给本机客户端；仅监听回环地址，不对局域网开放。")
                        .fontSize(core::theme::scaled(12.5f))
                        .color(t.colors.textTertiary)
                        .build();

                    // 服务状态行：静态状态点（pulse 动画明确不做）
                    ui.row("mcp.status")
                        .size(contentW, core::theme::scaled(20.0f))
                        .alignItems(eui::Align::CENTER)
                        .gap(t.sizes.spacingSmall)
                        .content([&] {
                            ui.rect("mcp.status.dot")
                                .size(8.0f, 8.0f)
                                .radius(t.sizes.radiusSmall)
                                .color(svc.running ? t.colors.success
                                                   : t.colors.textDisabled)
                                .build();
                            const std::string statusText =
                                svc.running
                                    ? "运行中 · 127.0.0.1:" +
                                          std::to_string(svc.port)
                                    : (svc.lastError.empty()
                                           ? "已停止"
                                           : "已停止 · " + svc.lastError);
                            ui.text("mcp.status.text")
                                .size(std::max(0.0f, contentW - 16.0f),
                                      core::theme::scaled(20.0f))
                                .text(statusText)
                                .fontSize(core::theme::scaled(12.5f))
                                .color(svc.running ? t.colors.textSecondary
                                                   : t.colors.textTertiary)
                                .build();
                        })
                        .build();

                    // 「记忆」模块行：向 MCP 贡献 memory_search 工具的模块开关
                    // （关 = 立即注销工具；经 app::memory_mcp 桥操作，不触 module_memory）
                    drawMemoryModuleRow(ui, contentW, t, comp);

                    // 区头：标题 + 计数 chip
                    // 2026-11-01 F-13：原标题占满 contentW-40，chip 被推到页面最右，
                    // 与左侧标题相距 700px，关联性为零。改为「量出标题实际宽度 →
                    // chip 紧随其后」，行内间距固定 8px。
                    const std::string sectionTitle = "注册的工具";
                    const float titleW = core::TextPrimitive::measureTextWidth(
                        sectionTitle, "", t.sizes.fontSizeLarge, 600);
                    const std::string countText = std::to_string(tools.size());
                    const float countW =
                        core::TextPrimitive::measureTextWidth(
                            countText, "", core::theme::scaled(11.0f), 400) +
                        14.0f;

                    ui.row("mcp.section")
                        .size(contentW, core::theme::scaled(26.0f))
                        .alignItems(eui::Align::CENTER)
                        .gap(8.0f)
                        .content([&] {
                            ui.text("mcp.section.title")
                                .size(titleW, core::theme::scaled(26.0f))
                                .text(sectionTitle)
                                .fontSize(t.sizes.fontSizeLarge)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.rect("mcp.section.count.bg")
                                .size(countW, core::theme::scaled(18.0f))
                                .radius(t.sizes.radiusPill)
                                .color(t.colors.primarySoftBg)
                                .build();
                            ui.text("mcp.section.count")
                                .size(countW, core::theme::scaled(18.0f))
                                .text(countText)
                                .fontSize(core::theme::scaled(11.0f))
                                .color(t.colors.primaryText)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        })
                        .build();

                    if (tools.empty()) {
                        // 空态：走全站统一组件（图标 + 主句 + 引导句 + 居中）
                        // 2026-11-01 F-14：原为单行居中文字，无图标无引导。
                        ui_kit::EmptyStateOptions emptyOpts;
                        emptyOpts.message = "暂无已注册的 MCP 工具";
                        emptyOpts.guide =
                            "工具由各模块在启动时注册；启用「记忆」等模块后，"
                            "对应工具会自动出现在这里。";
                        emptyOpts.icon = 0xF1B2;  // fa-cube
                        ui_kit::emptyState(ui, "mcp.empty", emptyOpts, 0.0f,
                                           0.0f, contentW,
                                           core::theme::scaled(220.0f), t);
                    } else {
                        // 网格：列宽随内容宽自适应（340–420），填满可用宽度，
                        // 不再在右侧留大片空白（2026-11-01 F-13）。
                        const auto [cols, cardW] =
                            gridColumns(contentW, t.sizes.spacingLarge);
                        (void)cols;
                        ui.flow("mcp.grid")
                            .width(contentW)
                            .gap(t.sizes.spacingLarge)
                            .content([&] {
                                for (const auto& tool : tools) {
                                    drawToolCard(ui, tool, t, comp, svc.running,
                                                 cardW);
                                }
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();

    drawDetailDialog(ui, screen, t, comp);
}

void McpPageModule::drawToolCard(eui::Ui& ui,
                                 const std::shared_ptr<core::mcp::IMcpTool>& tool,
                                 const core::theme::Tokens& t,
                                 const components::theme::ThemeColorTokens& comp,
                                 bool serviceRunning,
                                 float cardWidth) {
    const std::string& name = tool->name();
    const std::string id = "mcp.card." + name;
    const bool hovered = hoveredCards_.count(name) > 0;
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

    // 2026-11-01 自适应列宽下的卡内布局（全部由 cardWidth 推导，不再写死坐标）：
    //   [图标 40][文本列 …… 16px ……][● 状态]  ← 状态组右对齐
    //   [内置][服务]                        [查看详情 ›]
    const std::string statusText = serviceRunning ? "可用" : "未启用";
    const float statusTextW = core::TextPrimitive::measureTextWidth(
        statusText, "", core::theme::scaled(11.0f), 400);
    const float statusTextX = cardWidth - kCardPad - statusTextW;
    const float statusDotX = statusTextX - 12.0f - 8.0f;
    const float textW = std::max(0.0f, statusDotX - kCardTextX - kStatusGap);

    ui.stack(id)
        .size(cardWidth, kCardH)
        .content([&] {
            // 白卡 + Sm 阴影 + 描边（settings_ui_cards.cpp 卡底同款口径）
            ui.rect(id + ".bg")
                .size(cardWidth, kCardH)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, t.colors.surface.a})
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(comp, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            // 图标位：primarySoftBg 圆角底 + FA 码点
            ui.rect(id + ".icon.bg")
                .position(kCardPad, kCardPad)
                .size(kCardIcon, kCardIcon)
                .radius(t.sizes.radius)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".icon")
                .position(kCardPad, kCardPad)
                .size(kCardIcon, kCardIcon)
                .icon(eui::utf8(toolIconGlyph(name)))
                .fontSize(core::theme::scaled(16.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // 名称 + 单行描述（Ellipsis 只在 wrap(false)+maxWidth>0 生效）
            ui.text(id + ".name")
                .position(kCardTextX, kCardPad)
                .size(textW, core::theme::scaled(20.0f))
                .text(name)
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(14.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(id + ".desc")
                .position(kCardTextX, kCardPad + 24.0f)
                .size(textW, core::theme::scaled(16.0f))
                .text(tool->description())
                .fontSize(core::theme::scaled(12.0f))
                .color(t.colors.textTertiary)
                .maxWidth(textW)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();

            // 状态点 + 文案（右上；服务运行态只读反映）
            // 2026-11-01 F-01：状态组整体右对齐，与描述列右端保持 ≥16px 间距
            // （原固定 x=254、描述宽 178，间距只有 8px，长描述末尾的「…」与
            //   「未启用」粘成一句话）。
            ui.rect(id + ".status.dot")
                .position(statusDotX, kCardPad + 4.0f)
                .size(8.0f, 8.0f)
                .radius(t.sizes.radiusSmall)
                .color(serviceRunning ? t.colors.success : t.colors.textDisabled)
                .build();
            ui.text(id + ".status.text")
                .position(statusTextX, kCardPad + 14.0f)
                .size(statusTextW, core::theme::scaled(14.0f))
                .text(statusText)
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.textTertiary)
                .build();

            // 标签 chips：内置（primary 软底）/ 服务（中性灰底）
            // 2026-11-01 F-12/R6：服务 chip 原用 purple 软底 + purple 字——purple
            // 是用户可选的强调色槽位，借用为类别标签色会让微信绿白体系里凭空冒出
            // 唯一的紫色。改中性灰底 + 主色字。
            ui.rect(id + ".tag.builtin.bg")
                .position(kCardPad, 84.0f)
                .size(kChipW, kChipH)
                .radius(t.sizes.radiusPill)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".tag.builtin")
                .position(kCardPad, 84.0f)
                .size(kChipW, kChipH)
                .text("内置")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            const float svcChipX = kCardPad + kChipW + 6.0f;
            ui.rect(id + ".tag.svc.bg")
                .position(svcChipX, 84.0f)
                .size(kChipW, kChipH)
                .radius(t.sizes.radiusPill)
                .color(t.colors.borderMuted)
                .build();
            ui.text(id + ".tag.svc")
                .position(svcChipX, 84.0f)
                .size(kChipW, kChipH)
                .text("服务")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // hover CTA：平时 opacity 0，onHover 浮现（app_sidebar 窗控钮先例的
            // .states+onHover 通道）；整卡可点进详情。右对齐随卡宽自适应。
            const float ctaX = cardWidth - kCardPad - kCtaW;
            ui.row(id + ".cta")
                .position(ctaX, 84.0f)
                .size(kCtaW, kChipH)
                .alignItems(eui::Align::CENTER)
                .gap(4.0f)
                .opacity(hovered ? 1.0f : 0.0f)
                .content([&] {
                    ui.text(id + ".cta.label")
                        .size(kCtaW - 18.0f, kChipH)
                        .text("查看详情")
                        .fontSize(core::theme::scaled(12.0f))
                        .color(t.colors.primaryText)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .build();
                    ui.text(id + ".cta.arrow")
                        .size(14.0f, kChipH)
                        .icon(0xF061)  // fa-arrow-right
                        .fontSize(core::theme::scaled(11.0f))
                        .color(t.colors.primaryText)
                        .build();
                })
                .build();

            // 命中面：透明三态 + onHover/onClick（顶层，吃整卡点击）
            ui.rect(id + ".hit")
                .size(cardWidth, kCardH)
                .states(kTransparent, kTransparent, kTransparent)
                .onHover([this, name](bool h) {
                    if (h) {
                        hoveredCards_.insert(name);
                    } else {
                        hoveredCards_.erase(name);
                    }
                })
                .onClick([this, name] { openDetail(name); })
                .build();
        })
        .build();
}

// 「记忆」模块行：全宽轻量行卡（状态行同构；不用 340 网格卡，为后续模块行
// 自然堆叠留形）。开关态每帧从桥读——setEnabled 失败时注册态未变，下一帧自动
// 回弹，无需额外状态回写；反馈红字占用说明行槽位，行盒几何不变。
void McpPageModule::drawMemoryModuleRow(
    eui::Ui& ui, float contentW, const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& comp) {
    const std::string id = "mcp.module.memory";
    constexpr float kRowH = 64.0f;
    constexpr float kPad = 12.0f;
    constexpr float kIconSize = 40.0f;
    constexpr float kSwitchW = 44.0f;
    constexpr float kSwitchH = 24.0f;
    // 说明/标题区：图标右侧到开关左侧（130% 字号档靠 Ellipsis 兜底）
    const float textW = std::max(
        0.0f, contentW - kPad - kIconSize - 12.0f - kSwitchW - kPad);

    ui.stack(id)
        .size(contentW, kRowH)
        .content([&] {
            ui.rect(id + ".bg")
                .size(contentW, kRowH)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, t.colors.surface.a})
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .build();
            ui.rect(id + ".icon.bg")
                .position(kPad, kPad)
                .size(kIconSize, kIconSize)
                .radius(t.sizes.radius)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".icon")
                .position(kPad, kPad)
                .size(kIconSize, kIconSize)
                .icon(eui::utf8(0xF5DC))  // fa-brain，与 app_registry 记忆行同码点
                .fontSize(core::theme::scaled(16.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text(id + ".title")
                .position(kPad + kIconSize + 12.0f, kPad)
                .size(textW, core::theme::scaled(18.0f))
                .text("记忆")
                .fontSize(core::theme::scaled(14.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            const bool hasError = !memorySaveError_.empty();
            ui.text(id + ".desc")
                .position(kPad + kIconSize + 12.0f, kPad + core::theme::scaled(22.0f))
                .size(textW, core::theme::scaled(16.0f))
                .text(hasError ? "保存失败：" + memorySaveError_
                               : "将本机长期记忆以 memory_search 工具开放给 MCP 客户端查询")
                .fontSize(core::theme::scaled(12.0f))
                .color(hasError ? t.colors.errorText : t.colors.textTertiary)
                .maxWidth(textW)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();
            components::toggleSwitch(ui, id + ".toggle")
                .position(contentW - kPad - kSwitchW, (kRowH - kSwitchH) * 0.5f)
                .size(kSwitchW, kSwitchH)
                .checked(app::memory_mcp::status().switchOn)
                .theme(comp)
                .onChange([this](bool on) {
                    const auto r = app::memory_mcp::setEnabled(on);
                    memorySaveError_ =
                        r.isFail() ? r.errorMessage() : std::string();
                })
                .build();
        })
        .build();
}

void McpPageModule::drawDetailDialog(
    eui::Ui& ui, const eui::Screen& screen, const core::theme::Tokens& t,
    const components::theme::ThemeColorTokens& comp) {
    const bool running = tryRunning();
    bool hasResult = false;
    bool isError = false;
    std::string resultText;
    {
        std::lock_guard<std::mutex> lk(tryMutex_);
        hasResult = !tryResultText_.empty();
        isError = tryError_.load(std::memory_order_acquire);
        resultText = tryResultText_;
    }

    components::dialog(ui, "mcp.dialog.detail")
        .open(detailOpen())
        .screen(screen.width, screen.height)
        .size(kDialogW, kDialogH)
        .theme(comp)
        .onOpenChange([this](bool v) {
            if (!v) closeDetail();  // backdrop 点击 / 关闭钮 / Esc 三通道同汇
        })
        .content([&] {
            if (!detailTool_) return;
            // keep-alive 本帧副本：弹窗开着时工具被注销也不会悬垂
            const auto tool = detailTool_;
            const float w = kDialogW;
            const float h = kDialogH;

            // hero：图标位 + 名称（mono）+ 标签行
            ui.rect("mcp.dialog.detail.hero.icon.bg")
                .position(20.0f, 16.0f)
                .size(52.0f, 52.0f)
                .radius(t.sizes.radiusLarge)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text("mcp.dialog.detail.hero.icon")
                .position(20.0f, 16.0f)
                .size(52.0f, 52.0f)
                .icon(eui::utf8(toolIconGlyph(tool->name())))
                .fontSize(core::theme::scaled(20.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.text("mcp.dialog.detail.hero.name")
                .position(88.0f, 20.0f)
                .size(w - 108.0f, core::theme::scaled(24.0f))
                .text(tool->name())
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(18.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.rect("mcp.dialog.detail.hero.tag.builtin.bg")
                .position(88.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .radius(t.sizes.radiusPill)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text("mcp.dialog.detail.hero.tag.builtin")
                .position(88.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("内置")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.rect("mcp.dialog.detail.hero.tag.svc.bg")
                .position(134.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .radius(t.sizes.radiusPill)
                .color(t.colors.borderMuted)
                .build();
            ui.text("mcp.dialog.detail.hero.tag.svc")
                .position(134.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("服务")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // 描述（可换行）
            ui.text("mcp.dialog.detail.desc")
                .position(20.0f, 88.0f)
                .size(w - 40.0f, 48.0f)
                .text(tool->description())
                .fontSize(core::theme::scaled(13.0f))
                .lineHeight(core::theme::scaled(19.0f))
                .color(t.colors.textSecondary)
                .wrap(true)
                .build();

            // inputSchemaJson：monospace 展示块（浅底 + muted 边）
            ui.text("mcp.dialog.detail.schema.title")
                .position(20.0f, 148.0f)
                .size(w - 40.0f, core::theme::scaled(16.0f))
                .text("参数 schema（JSON）")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.textTertiary)
                .build();
            ui.rect("mcp.dialog.detail.schema.box")
                .position(20.0f, 168.0f)
                .size(w - 40.0f, 118.0f)
                .radius(t.sizes.radius)
                .color(t.colors.background)
                .border(1.0f, t.colors.borderMuted)
                .build();
            // 不变量：schema 文本按节点框 clip，折行溢出不越 schema.box 压「运行结果」标题。
            ui.text("mcp.dialog.detail.schema.text")
                .position(28.0f, 176.0f)
                .size(w - 56.0f, 102.0f)
                .text(tool->inputSchemaJson())
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(17.0f))
                .color(t.colors.textSecondary)
                .wrap(true)
                .clip()
                .build();

            // 结果区：成功/失败前缀区分；运行中占位
            ui.text("mcp.dialog.detail.result.title")
                .position(20.0f, 298.0f)
                .size(w - 40.0f, core::theme::scaled(16.0f))
                .text("运行结果")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.textTertiary)
                .build();
            const bool resultEmpty = !running && !hasResult;
            ui.rect("mcp.dialog.detail.result.box")
                .position(20.0f, 318.0f)
                .size(w - 40.0f, h - 318.0f - 64.0f)
                .radius(t.sizes.radius)
                .color(t.colors.background)
                .border(1.0f, t.colors.borderMuted)
                .build();
            ui.text("mcp.dialog.detail.result.text")
                .position(28.0f, 326.0f)
                .size(w - 56.0f, h - 326.0f - 64.0f - 16.0f)
                .text(resultDisplayText(running, hasResult, isError, resultText))
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(17.0f))
                .color(isError ? t.colors.errorText
                               : (resultEmpty ? t.colors.textTertiary
                                              : t.colors.textSecondary))
                .wrap(true)
                .build();

            // 操作行：「试一下」（运行中禁用）+「关闭」
            components::button(ui, "mcp.dialog.detail.btn.try")
                .theme(comp, true)
                .position(w - 20.0f - 96.0f, h - 52.0f)
                .size(96.0f, 32.0f)
                .text(running ? "运行中…" : "试一下")
                .fontSize(core::theme::scaled(13.0f))
                .disabled(running)
                .onClick([this] { startTryRun(); })
                .build();
            components::button(ui, "mcp.dialog.detail.btn.close")
                .theme(comp, false)
                .position(w - 20.0f - 96.0f - 8.0f - 72.0f, h - 52.0f)
                .size(72.0f, 32.0f)
                .text("关闭")
                .fontSize(core::theme::scaled(13.0f))
                .onClick([this] { closeDetail(); })
                .build();
        })
        .build();
}

bool McpPageModule::onKeyEvent(const eui::KeyEvent& event) {
    if (event.action != eui::KeyAction::Press &&
        event.action != eui::KeyAction::Repeat) {
        return false;
    }
    if (event.key == eui::InputKey::Escape && detailOpen()) {
        closeDetail();
        return true;
    }
    return false;
}

void McpPageModule::onStop() {
    // 先停后注销的 join 收束点：在途 execute 完整跑完（web_fetch 自限 10s），
    // 不硬杀不悬垂
    joinTryThread();
    tryRunning_.store(false, std::memory_order_release);
}

void McpPageModule::openDetail(const std::string& name) {
    auto found = registry_->find(name);
    if (found.isFail()) return;  // 工具已被注销：不开空弹窗
    detailTool_ = found.value();
    // 换工具打开：清上一工具的残留结果（同工具重开保留在途/已完成态）
    if (tryToolName_ != name && !tryRunning()) {
        std::lock_guard<std::mutex> lk(tryMutex_);
        tryResultText_.clear();
        tryDone_.store(false, std::memory_order_release);
        tryError_.store(false, std::memory_order_release);
        tryToolName_.clear();
    }
}

void McpPageModule::closeDetail() { detailTool_.reset(); }

void McpPageModule::startTryRun() {
    if (!detailTool_ || tryRunning()) return;
    if (tryThread_.joinable()) {
        tryThread_.join();  // 上一轮已完成未收割的先收（防御重复点击竞态）
    }
    const auto tool = detailTool_;  // keep-alive：注销瞬间不悬垂
    tryToolName_ = tool->name();
    tryRunning_.store(true, std::memory_order_release);
    tryDone_.store(false, std::memory_order_release);
    tryError_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(tryMutex_);
        tryResultText_.clear();
    }
    tryThread_ = std::thread([this, tool] {
        auto r = tool->execute("{}");
        bool err = false;
        std::string text;
        if (r.isFail()) {
            err = true;
            text = r.errorMessage();
        } else {
            err = r.value().isError;
            text = r.value().text;
        }
        {
            std::lock_guard<std::mutex> lk(tryMutex_);
            tryResultText_ = std::move(text);
        }
        tryError_.store(err, std::memory_order_release);
        tryDone_.store(true, std::memory_order_release);
        // worker 置 done 后主动要帧，不等用户输入（settings 后台任务同口径）
        core::platform::requestUiUpdate();
    });
}

void McpPageModule::harvestTryRun() {
    if (!tryThread_.joinable()) return;
    if (!tryDone_.load(std::memory_order_acquire)) return;
    tryThread_.join();  // join 即 happens-before，之后读 tryResultText_ 安全
    tryRunning_.store(false, std::memory_order_release);
}

void McpPageModule::joinTryThread() {
    if (tryThread_.joinable()) {
        tryThread_.join();
    }
}

} // namespace module::mcp

extern "C" ::core::Module* create_mcp_page_module() {
    return new module::mcp::McpPageModule();
}
