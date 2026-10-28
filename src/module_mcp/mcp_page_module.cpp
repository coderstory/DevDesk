// src/module_mcp/mcp_page_module.cpp
// MCP 菜单页实现：页头+服务状态行 → 工具卡片网格（ui.flow，卡定宽 340）→
// 详情弹窗（components::dialog，关闭 = 按钮 + Esc + backdrop 三通道）→
// 「试一下」（execute 后台线程 + compose 轮询收割，禁 UI 线程直调）。
// 视觉基准 = 桌面 mcp-menu-demo.html（token 对齐，图标 FA7 码点；页内开关与
// 关闭置灰不照抄——关闭态 = 模块门控整页注销）。
#include "mcp_page_module.h"

#include "core/component_theme.h"
#include "core/logging.h"
#include "core/theme.h"
#include "mcp_service_bridge.h"

#include <core/platform/platform.h>  // core::platform::requestUiUpdate（唤醒主循环）

#include <components/button.h>
#include <components/dialog.h>
#include <components/scrollview.h>
#include <components/theme.h>

#include <eui/dsl.h>

#include <algorithm>
#include <utility>

namespace module::mcp {

namespace {

constexpr float kCardW = 340.0f;   // demo 网格 minmax(340px,1fr) 的定宽档
constexpr float kCardH = 120.0f;
constexpr float kDialogW = 600.0f;
constexpr float kDialogH = 480.0f;

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

                    // 区头：标题 + 计数 chip
                    ui.row("mcp.section")
                        .size(contentW, core::theme::scaled(26.0f))
                        .alignItems(eui::Align::CENTER)
                        .gap(t.sizes.spacingSmall)
                        .content([&] {
                            ui.text("mcp.section.title")
                                .size(std::max(0.0f, contentW - 40.0f),
                                      core::theme::scaled(26.0f))
                                .text("注册的工具")
                                .fontSize(t.sizes.fontSizeLarge)
                                .fontWeight(600)
                                .color(t.colors.textPrimary)
                                .build();
                            ui.rect("mcp.section.count.bg")
                                .size(28.0f, core::theme::scaled(18.0f))
                                .radius(t.sizes.radiusPill)
                                .color(t.colors.primarySoftBg)
                                .build();
                            ui.text("mcp.section.count")
                                .position(0.0f, 0.0f)
                                .size(28.0f, core::theme::scaled(18.0f))
                                .text(std::to_string(tools.size()))
                                .fontSize(core::theme::scaled(11.0f))
                                .color(t.colors.primaryText)
                                .horizontalAlign(eui::HorizontalAlign::Center)
                                .verticalAlign(eui::VerticalAlign::Center)
                                .build();
                        })
                        .build();

                    if (tools.empty()) {
                        // 空态：极简一行文字（§4.12 禁插画/情绪化文案）
                        ui.text("mcp.empty")
                            .size(contentW, core::theme::scaled(48.0f))
                            .text("暂无已注册的 MCP 工具（工具由各模块在启动时注册）")
                            .fontSize(core::theme::scaled(13.0f))
                            .color(t.colors.textTertiary)
                            .horizontalAlign(eui::HorizontalAlign::Center)
                            .build();
                    } else {
                        ui.flow("mcp.grid")
                            .gap(t.sizes.spacingLarge)
                            .content([&] {
                                for (const auto& tool : tools) {
                                    drawToolCard(ui, tool, t, comp, svc.running);
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
                                 bool serviceRunning) {
    const std::string& name = tool->name();
    const std::string id = "mcp.card." + name;
    const bool hovered = hoveredCards_.count(name) > 0;
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

    ui.stack(id)
        .size(kCardW, kCardH)
        .content([&] {
            // 白卡 + Sm 阴影 + 描边（settings_ui_cards.cpp 卡底同款口径）
            ui.rect(id + ".bg")
                .size(kCardW, kCardH)
                .color(eui::Color{t.colors.surface.r, t.colors.surface.g,
                                  t.colors.surface.b, t.colors.surface.a})
                .radius(t.sizes.radiusLarge)
                .shadow(components::theme::shadow(comp, 3.0f, 1.0f, 0.04f, 0.02f))
                .border(1.0f, t.colors.borderDefault)
                .build();

            // 图标位：primarySoftBg 圆角底 + FA 码点
            ui.rect(id + ".icon.bg")
                .position(16.0f, 16.0f)
                .size(40.0f, 40.0f)
                .radius(t.sizes.radius)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".icon")
                .position(16.0f, 16.0f)
                .size(40.0f, 40.0f)
                .icon(eui::utf8(toolIconGlyph(name)))
                .fontSize(core::theme::scaled(16.0f))
                .color(t.colors.primary)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // 名称 + 单行描述（Ellipsis 只在 wrap(false)+maxWidth>0 生效）
            ui.text(id + ".name")
                .position(68.0f, 16.0f)
                .size(178.0f, core::theme::scaled(20.0f))
                .text(name)
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(14.0f))
                .fontWeight(600)
                .color(t.colors.textPrimary)
                .build();
            ui.text(id + ".desc")
                .position(68.0f, 40.0f)
                .size(178.0f, core::theme::scaled(16.0f))
                .text(tool->description())
                .fontSize(core::theme::scaled(12.0f))
                .color(t.colors.textTertiary)
                .maxWidth(178.0f)
                .wrap(false)
                .overflow(core::TextOverflow::Ellipsis)
                .build();

            // 状态点 + 文案（右上；服务运行态只读反映）
            ui.rect(id + ".status.dot")
                .position(254.0f, 20.0f)
                .size(8.0f, 8.0f)
                .radius(t.sizes.radiusSmall)
                .color(serviceRunning ? t.colors.success : t.colors.textDisabled)
                .build();
            ui.text(id + ".status.text")
                .position(254.0f, 32.0f)
                .size(70.0f, core::theme::scaled(14.0f))
                .text(serviceRunning ? "可用" : "未启用")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.textTertiary)
                .build();

            // 标签 chips：内置（primary 软底）/ 服务（purple 软底）
            ui.rect(id + ".tag.builtin.bg")
                .position(16.0f, 84.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .radius(t.sizes.radiusPill)
                .color(t.colors.primarySoftBg)
                .build();
            ui.text(id + ".tag.builtin")
                .position(16.0f, 84.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("内置")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.primaryText)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            ui.rect(id + ".tag.svc.bg")
                .position(62.0f, 84.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .radius(t.sizes.radiusPill)
                .color(components::theme::withOpacity(t.colors.purple, 0.12f))
                .build();
            ui.text(id + ".tag.svc")
                .position(62.0f, 84.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("服务")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.purple)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            // hover CTA：平时 opacity 0，onHover 浮现（app_sidebar 窗控钮先例的
            // .states+onHover 通道）；整卡可点进详情
            ui.row(id + ".cta")
                .position(150.0f, 84.0f)
                .size(158.0f, core::theme::scaled(18.0f))
                .alignItems(eui::Align::CENTER)
                .gap(4.0f)
                .opacity(hovered ? 1.0f : 0.0f)
                .content([&] {
                    ui.text(id + ".cta.label")
                        .size(140.0f, core::theme::scaled(18.0f))
                        .text("查看详情")
                        .fontSize(core::theme::scaled(12.0f))
                        .color(t.colors.primaryText)
                        .horizontalAlign(eui::HorizontalAlign::Right)
                        .build();
                    ui.text(id + ".cta.arrow")
                        .size(14.0f, core::theme::scaled(18.0f))
                        .icon(0xF061)  // fa-arrow-right
                        .fontSize(core::theme::scaled(11.0f))
                        .color(t.colors.primaryText)
                        .build();
                })
                .build();

            // 命中面：透明三态 + onHover/onClick（顶层，吃整卡点击）
            ui.rect(id + ".hit")
                .size(kCardW, kCardH)
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
                .color(components::theme::withOpacity(t.colors.purple, 0.12f))
                .build();
            ui.text("mcp.dialog.detail.hero.tag.svc")
                .position(134.0f, 50.0f)
                .size(40.0f, core::theme::scaled(18.0f))
                .text("服务")
                .fontSize(core::theme::scaled(11.0f))
                .color(t.colors.purple)
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
            ui.text("mcp.dialog.detail.schema.text")
                .position(28.0f, 176.0f)
                .size(w - 56.0f, 102.0f)
                .text(tool->inputSchemaJson())
                .fontFamily("monospace")
                .fontSize(core::theme::scaled(12.0f))
                .lineHeight(core::theme::scaled(17.0f))
                .color(t.colors.textSecondary)
                .wrap(true)
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
