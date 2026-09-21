// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - 完整 UI 实现 + 真实状态集成
#include "claude_service.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/time.h"

#include <eui/dsl.h>

#include <cstdio>
#include <ctime>
#include <string>

namespace module::claude {

namespace {

// 通用 profile 卡片（接受 std::string，与真实 module::claude::Profile 兼容）
void drawProfileCard(eui::Ui& ui,
                     const std::string& id,
                     float y,
                     float width,
                     const core::theme::Tokens& t,
                     const std::string& name,
                     const std::string& baseUrl,
                     const std::string& model,
                     bool active) {
    const float h = 96.0f;
    const eui::Color borderColor = active ? t.colors.primary : t.colors.borderDefault;
    const float borderW = active ? 2.0f : 1.0f;
    const eui::Color bg = active ? t.colors.primarySoftBg : t.colors.surface;

    // 卡片背景
    ui.rect(id + ".bg")
        .position(0.0f, y)
        .size(width, h)
        .color(bg)
        .radius(t.sizes.radiusLarge)
        .border(borderW, borderColor)
        .build();

    // 头像（圆形 - 用 rect 模拟）
    ui.rect(id + ".avatar")
        .position(16.0f, y + 16.0f)
        .size(40.0f, 40.0f)
        .color(t.colors.surfaceElevated)
        .radius(20.0f)
        .border(1.0f, t.colors.borderDefault)
        .build();
    // 头像首字
    const std::string initial = name.empty()
        ? std::string("?")
        : std::string(1, name[0]);
    ui.text(id + ".avatar.text")
        .position(16.0f, y + 16.0f)
        .size(40.0f, 40.0f)
        .text(initial)
        .fontSize(16.0f)
        .lineHeight(40.0f)
        .horizontalAlign(eui::HorizontalAlign::Center)
        .color(t.colors.textPrimary)
        .fontWeight(600)
        .build();

    // 名称
    ui.text(id + ".name")
        .position(64.0f, y + 18.0f)
        .size(width - 130.0f, 20.0f)
        .text(name)
        .fontSize(15.0f)
        .lineHeight(20.0f)
        .fontWeight(600)
        .color(t.colors.textPrimary)
        .build();

    if (active) {
        ui.rect(id + ".active.bg")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .color(t.colors.primary)
            .radius(t.sizes.radiusPill)
            .build();
        ui.text(id + ".active.text")
            .position(width - 60.0f, y + 16.0f)
            .size(48.0f, 20.0f)
            .text("ACTIVE")
            .fontSize(9.0f)
            .lineHeight(20.0f)
            .horizontalAlign(eui::HorizontalAlign::Center)
            .color(eui::Color("#FFFFFF"))
            .fontWeight(700)
            .build();
    }

    // base-url (mono, 12px, tertiary)
    ui.text(id + ".url")
        .position(64.0f, y + 44.0f)
        .size(width - 80.0f, 14.0f)
        .text(baseUrl)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textTertiary)
        .build();

    // model (mono, 12px)
    ui.text(id + ".model")
        .position(64.0f, y + 62.0f)
        .size(width - 80.0f, 14.0f)
        .text(std::string("model: ") + model)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .color(t.colors.textSecondary)
        .build();
}

// 格式化 unix 时间戳为 HH:MM
std::string formatHHMM(std::time_t ts) {
    if (ts <= 0) return "--:--";
    std::tm tmv{};
    core::time::localTime(ts, &tmv);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return std::string(buf);
}

// 格式化毫秒数为 "1234ms" / "1.2s"
std::string formatLatency(int64_t ms) {
    if (ms <= 0) return "-";
    if (ms < 1000) return std::to_string(static_cast<int>(ms)) + "ms";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    return std::string(buf);
}

} // namespace

class ClaudeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::ClaudeProxy; }
    std::string displayName() const override { return "Claude 切换器"; }
    std::string iconGlyph() const override { return "🔄"; }

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const float headerH = 64.0f;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge;
        const float gap = 16.0f;
        const float leftW = 400.0f;
        const float rightW = contentW - leftW - gap;

        // ---- 读真实状态（ClaudeService 全局单例，由 main.cpp 注册）----
        ClaudeService* svc = ClaudeService::global();
        const bool proxyRunning = svc ? svc->isProxyRunning() : false;
        const int runningPort = svc ? svc->proxyPort() : 0;
        const std::string portStr = runningPort > 0
            ? (":" + std::to_string(runningPort))
            : std::string(":8888");

        std::vector<Profile> profiles;
        if (svc) {
            auto listR = svc->listProfiles();
            if (listR.isOk()) profiles = std::move(listR).value();
        }

        std::vector<ClaudeService::CallLogEntry> recent;
        if (svc) {
            auto rR = svc->recentCalls(3);
            if (rR.isOk()) recent = std::move(rR).value();
        }

        Profile activeProfile;
        bool hasActive = false;
        if (svc) {
            auto aR = svc->getActiveProfile();
            if (aR.isOk()) {
                activeProfile = std::move(aR).value();
                hasActive = true;
            }
        }
        const std::string nameValue = hasActive ? activeProfile.name : std::string("(未启用)");
        const std::string modelValue = hasActive ? activeProfile.model : std::string("-");
        const std::string urlValue = hasActive ? activeProfile.baseUrl : std::string("-");
        const std::string tokenValue = hasActive
            ? (activeProfile.apiKeyHint + " [Keychain]")
            : std::string("(无激活 profile)");

        ui.column("claude.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                // ========== 顶部 Header ==========
                ui.row("claude.header")
                    .size(contentW, headerH)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        // 左侧标题 + 状态
                        ui.column("claude.title.col")
                            .size(contentW - 360.0f, headerH)
                            .gap(6.0f)
                            .content([&] {
                                ui.row("claude.title.row")
                                    .size(contentW - 360.0f, 28.0f)
                                    .gap(12.0f)
                                    .alignItems(eui::Align::CENTER)
                                    .content([&] {
                                        ui.text("claude.h1")
                                            .size(220.0f, 32.0f)
                                            .text("Claude 切换器")
                                            .fontSize(t.sizes.fontSizeTitle)
                                            .lineHeight(32.0f)
                                            .fontWeight(700)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        // 状态指示器（真实状态）
                                        const eui::Color dotColor = proxyRunning
                                            ? t.colors.success
                                            : t.colors.textTertiary;
                                        const std::string statusText = proxyRunning
                                            ? std::string("运行中 ") + portStr
                                            : std::string("已停止");
                                        ui.rect("claude.status.dot")
                                            .size(10.0f, 10.0f)
                                            .color(dotColor)
                                            .radius(5.0f)
                                            .build();
                                        ui.text("claude.status.text")
                                            .size(200.0f, 18.0f)
                                            .text(statusText)
                                            .fontSize(13.0f)
                                            .lineHeight(18.0f)
                                            .color(proxyRunning ? t.colors.success
                                                                : t.colors.textTertiary)
                                            .fontWeight(500)
                                            .build();
                                    })
                                    .build();

                                const std::string subtitle =
                                    std::string("管理多套 Claude API 配置 · 本地代理 http://127.0.0.1") +
                                    portStr;
                                ui.text("claude.subtitle")
                                    .size(contentW - 360.0f, 16.0f)
                                    .text(subtitle)
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(16.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧操作
                        ui.row("claude.actions")
                            .size(360.0f, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                // 停止代理
                                ui.rect("claude.btn.stop.bg")
                                    .size(96.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("claude.btn.stop.text")
                                    .size(96.0f, 40.0f)
                                    .text("⏸ 停止代理")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textSecondary)
                                    .build();

                                // 新建 Profile
                                ui.rect("claude.btn.new.bg")
                                    .size(140.0f, 40.0f)
                                    .color(t.colors.primary)
                                    .radius(t.sizes.radius)
                                    .build();
                                ui.text("claude.btn.new.text")
                                    .size(140.0f, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(eui::Color("#FFFFFF"))
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 主区: Profile 列表 + 详情 ==========
                ui.row("claude.body")
                    .size(contentW, bodyH)
                    .gap(gap)
                    .content([&] {
                        // ---- 左栏：Profile 列表 ----
                        ui.column("claude.profiles")
                            .size(leftW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("claude.profiles.bg")
                                    .size(leftW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("claude.profiles.overlay")
                            .size(leftW, bodyH)
                            .content([&] {
                                // 顶部栏
                                ui.text("claude.profiles.title")
                                    .position(20.0f, 20.0f)
                                    .size(leftW - 40.0f, 20.0f)
                                    .text("Profile 列表")
                                    .fontSize(15.0f)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 卡片列表（真实数据；空时显示占位）
                                const float cardY0 = 56.0f;
                                const float cardGap = 12.0f;
                                if (profiles.empty()) {
                                    ui.text("claude.profiles.empty")
                                        .position(20.0f, cardY0 + 20.0f)
                                        .size(leftW - 40.0f, 60.0f)
                                        .text("暂无 Profile，点击下方按钮新建")
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.textTertiary)
                                        .build();
                                } else {
                                    for (size_t i = 0; i < profiles.size(); ++i) {
                                        const Profile& p = profiles[i];
                                        drawProfileCard(ui, "claude.profile." + std::to_string(i),
                                                        cardY0 + static_cast<float>(i) * (96.0f + cardGap),
                                                        leftW - 24.0f, t,
                                                        p.name, p.baseUrl, p.model,
                                                        p.isActive);
                                    }
                                }

                                // 底部新建按钮
                                const float newBtnY = bodyH - 64.0f;
                                ui.rect("claude.profiles.new.bg")
                                    .position(20.0f, newBtnY)
                                    .size(leftW - 40.0f, 40.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                                ui.text("claude.profiles.new.text")
                                    .position(20.0f, newBtnY)
                                    .size(leftW - 40.0f, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(13.0f)
                                    .lineHeight(40.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .color(t.colors.textPrimary)
                                    .build();
                            })
                            .build();

                        // ---- 右栏：详情面板 + 最近调用 ----
                        ui.column("claude.detail")
                            .size(rightW, bodyH)
                            .gap(0.0f)
                            .content([&] {
                                ui.rect("claude.detail.bg")
                                    .size(rightW, bodyH)
                                    .color(t.colors.surface)
                                    .radius(t.sizes.radiusLarge)
                                    .border(1.0f, t.colors.borderDefault)
                                    .build();
                            })
                            .build();

                        ui.stack("claude.detail.overlay")
                            .size(rightW, bodyH)
                            .content([&] {
                                const float x0 = 28.0f;
                                const float y0 = 24.0f;
                                const float w0 = rightW - 56.0f;

                                // 标题
                                ui.text("claude.detail.title")
                                    .position(x0, y0)
                                    .size(w0, 24.0f)
                                    .text("当前 Profile 详情")
                                    .fontSize(18.0f)
                                    .lineHeight(24.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 字段网格（2 列）
                                const float fy0 = y0 + 44.0f;
                                const float fieldH = 56.0f;

                                // 名称（左）
                                ui.text("claude.field.name.label")
                                    .position(x0, fy0)
                                    .size((w0 - 16.0f) / 2.0f, 14.0f)
                                    .text("名称")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.name.bg")
                                    .position(x0, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f, fieldH - 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.name.value")
                                    .position(x0 + 12.0f, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f - 24.0f, fieldH - 32.0f)
                                    .text(nameValue)
                                    .fontSize(14.0f)
                                    .lineHeight(fieldH - 32.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 模型（右）
                                const float col2x = x0 + (w0 + 16.0f) / 2.0f;
                                ui.text("claude.field.model.label")
                                    .position(col2x, fy0)
                                    .size((w0 - 16.0f) / 2.0f, 14.0f)
                                    .text("模型")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.model.bg")
                                    .position(col2x, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f, fieldH - 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.model.value")
                                    .position(col2x + 12.0f, fy0 + 18.0f)
                                    .size((w0 - 16.0f) / 2.0f - 24.0f, fieldH - 32.0f)
                                    .text(modelValue)
                                    .fontSize(14.0f)
                                    .lineHeight(fieldH - 32.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // Base URL（全宽）
                                ui.text("claude.field.url.label")
                                    .position(x0, fy0 + fieldH)
                                    .size(w0, 14.0f)
                                    .text("Base URL")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.url.bg")
                                    .position(x0, fy0 + fieldH + 18.0f)
                                    .size(w0, 28.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.url.value")
                                    .position(x0 + 12.0f, fy0 + fieldH + 18.0f)
                                    .size(w0 - 24.0f, 28.0f)
                                    .text(urlValue)
                                    .fontSize(13.0f)
                                    .lineHeight(28.0f)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // Token（脱敏）
                                ui.text("claude.field.token.label")
                                    .position(x0, fy0 + fieldH + 56.0f)
                                    .size(w0, 14.0f)
                                    .text("API Key")
                                    .fontSize(11.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.rect("claude.field.token.bg")
                                    .position(x0, fy0 + fieldH + 74.0f)
                                    .size(w0, 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .border(1.0f, t.colors.borderMuted)
                                    .build();
                                ui.text("claude.field.token.value")
                                    .position(x0 + 12.0f, fy0 + fieldH + 74.0f)
                                    .size(w0 - 24.0f, 32.0f)
                                    .text(tokenValue)
                                    .fontSize(13.0f)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textSecondary)
                                    .build();

                                // 操作按钮组
                                const float actY = fy0 + fieldH + 130.0f;
                                const float actGap = 12.0f;
                                const float actW = (w0 - actGap * 3.0f) / 4.0f;
                                ui.row("claude.detail.actions")
                                    .size(w0, 36.0f)
                                    .gap(actGap)
                                    .content([&] {
                                        // 启用
                                        ui.rect("claude.act.enable.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.primary)
                                            .radius(t.sizes.radius)
                                            .build();
                                        ui.text("claude.act.enable.text")
                                            .size(actW, 36.0f)
                                            .text("启用")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(eui::Color("#FFFFFF"))
                                            .build();
                                        // 测试连通
                                        ui.rect("claude.act.test.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .build();
                                        ui.text("claude.act.test.text")
                                            .size(actW, 36.0f)
                                            .text("测试连通")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.textPrimary)
                                            .build();
                                        // 编辑
                                        ui.rect("claude.act.edit.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .build();
                                        ui.text("claude.act.edit.text")
                                            .size(actW, 36.0f)
                                            .text("编辑")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.textPrimary)
                                            .build();
                                        // 删除
                                        ui.rect("claude.act.del.bg")
                                            .size(actW, 36.0f)
                                            .color(t.colors.surfaceElevated)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .build();
                                        ui.text("claude.act.del.text")
                                            .size(actW, 36.0f)
                                            .text("删除")
                                            .fontSize(13.0f)
                                            .lineHeight(36.0f)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .color(t.colors.error)
                                            .build();
                                    })
                                    .build();

                                // 分隔线
                                ui.rect("claude.divider")
                                    .position(x0, actY + 60.0f)
                                    .size(w0, 1.0f)
                                    .color(t.colors.borderMuted)
                                    .build();

                                // 最近调用
                                ui.text("claude.calls.title")
                                    .position(x0, actY + 76.0f)
                                    .size(w0, 20.0f)
                                    .text("最近调用")
                                    .fontSize(15.0f)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 调用表头
                                const float tableY = actY + 108.0f;
                                const float colTimeX = x0;
                                const float colPathX = x0 + 80.0f;
                                const float colStatusX = x0 + w0 - 200.0f;
                                const float colDurX = x0 + w0 - 100.0f;
                                ui.text("claude.calls.h.time")
                                    .position(colTimeX, tableY)
                                    .size(72.0f, 14.0f)
                                    .text("时间")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.path")
                                    .position(colPathX, tableY)
                                    .size(w0 - 280.0f, 14.0f)
                                    .text("方法 / 路径")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.status")
                                    .position(colStatusX, tableY)
                                    .size(60.0f, 14.0f)
                                    .text("状态")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                                ui.text("claude.calls.h.dur")
                                    .position(colDurX, tableY)
                                    .size(60.0f, 14.0f)
                                    .text("耗时")
                                    .fontSize(10.0f)
                                    .lineHeight(14.0f)
                                    .color(t.colors.textTertiary)
                                    .build();

                                // 最近调用（真实数据，从 claude_call_logs 读）
                                if (recent.empty()) {
                                    ui.text("claude.calls.empty")
                                        .position(colTimeX, tableY + 24.0f)
                                        .size(w0 - 80.0f, 16.0f)
                                        .text("(暂无调用记录)")
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.textTertiary)
                                        .build();
                                } else {
                                    for (size_t i = 0; i < recent.size() && i < 3; ++i) {
                                        const ClaudeService::CallLogEntry& c = recent[i];
                                        const float rowY = tableY + 24.0f +
                                            static_cast<float>(i) * 24.0f;

                                        ui.text("claude.calls.t." + std::to_string(i))
                                            .position(colTimeX, rowY)
                                            .size(72.0f, 16.0f)
                                            .text(formatHHMM(c.startedAt))
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        const std::string mp = c.method + " " + c.path;
                                        ui.text("claude.calls.p." + std::to_string(i))
                                            .position(colPathX, rowY)
                                            .size(w0 - 280.0f, 16.0f)
                                            .text(mp)
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(t.colors.textPrimary)
                                            .build();

                                        const eui::Color statusColor =
                                            (c.statusCode >= 200 && c.statusCode < 300)
                                                ? t.colors.success :
                                            c.statusCode == 429
                                                ? t.colors.warning
                                                : t.colors.error;
                                        const std::string statusStr = c.statusCode > 0
                                            ? std::to_string(c.statusCode)
                                            : std::string("-");
                                        ui.text("claude.calls.s." + std::to_string(i))
                                            .position(colStatusX, rowY)
                                            .size(60.0f, 16.0f)
                                            .text(statusStr)
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(statusColor)
                                            .build();

                                        ui.text("claude.calls.d." + std::to_string(i))
                                            .position(colDurX, rowY)
                                            .size(60.0f, 16.0f)
                                            .text(formatLatency(c.latencyMs))
                                            .fontSize(11.0f)
                                            .lineHeight(16.0f)
                                            .color(t.colors.textSecondary)
                                            .build();
                                    }
                                }

                                // 查看全部 →
                                ui.text("claude.calls.viewall")
                                    .position(x0, tableY + 108.0f)
                                    .size(w0, 18.0f)
                                    .text("查看全部 →")
                                    .fontSize(12.0f)
                                    .lineHeight(18.0f)
                                    .color(t.colors.primary)
                                    .build();
                            })
                            .build();
                    })
                    .build();
            })
            .build();

        LOG_DEBUG("Claude 模块 compose 完成");
    }

    void onStart() override {
        LOG_INFO("Claude 模块启动");
        // 代理由 main.cpp 的 dslAppConfig() 启动（全局唯一）。
        // 这里只在尚未启动时兜底启动一次，避免模块独立运行时没有代理。
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            LOG_WARN("onStart: ClaudeService::global() 为 null，跳过代理启动");
            return;
        }
        if (svc->isProxyRunning()) {
            LOG_INFO("onStart: 代理已在运行 port=" +
                     std::to_string(svc->proxyPort()));
            return;
        }
        auto r = svc->startProxy("127.0.0.1", 8888);
        if (r.isFail()) {
            LOG_ERROR("onStart: 启动代理失败 - " + r.errorMessage());
            return;
        }
        LOG_INFO("onStart: 代理已启动 port=" + std::to_string(r.value()));
    }

    void onStop() override {
        LOG_INFO("Claude 模块停止");
        ClaudeService* svc = ClaudeService::global();
        if (svc && svc->isProxyRunning()) {
            svc->stopProxy();
            LOG_INFO("onStop: 代理已停止");
        }
    }
};

} // namespace module::claude

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}