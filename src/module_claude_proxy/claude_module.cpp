// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - 完整 UI + 操作按钮全量接线（真实 ClaudeService）
//
// 接线清单（W5）：
//   新建/编辑 Profile（dialog 表单）→ createProfile / updateProfile
//   启用 → setActiveProfile；删除（确认 dialog）→ deleteProfile
//   测试连通 → testConnection；代理开关 → startProxy / stopProxy
//   每次操作后靠下一帧 compose 重新拉取数据（本模块 compose 直读 service）
#include "claude_service.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/time.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/input.h>
#include <components/theme.h>
#include <components/toast.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace module::claude {

namespace {

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
    std::string iconGlyph() const override { return eui::utf8(0xF021); }  // fa-arrows-rotate

    ~ClaudeModule() override {
        // 后台测试线程必须先收（最坏阻塞到 HTTP 超时 15s，避免悬垂 this）
        if (testThread_.joinable()) testThread_.join();
    }

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const auto themeTokens = t.isDark ? components::theme::dark()
                                          : components::theme::light();
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
            : std::string("");   // 未运行时不显示假端口（副标题另有提示）

        // 页面背景自绘（仿 settings_module：main.cpp clearColor 固定浅色，
        // 深色主题下页面必须自己铺底，否则浅色 clear 底 + 深色 token 文字不可读）
        ui.rect("claude.pagebg")
            .size(screen.width, screen.height)
            .color(t.colors.background)
            .build();

        // 错误态（仿 Home statsError_）：加载失败不静默吞错显示假空态
        std::string profilesError;
        std::string callsError;

        std::vector<Profile> profiles;
        if (!svc) {
            profilesError = "服务未就绪（ClaudeService 未注册）";
        } else {
            auto listR = svc->listProfiles();
            if (listR.isOk()) {
                profiles = std::move(listR).value();
            } else {
                profilesError = listR.errorMessage();
                LOG_ERROR("Profile 列表加载失败: " + profilesError);
            }
        }

        std::vector<ClaudeService::CallLogEntry> recent;
        if (!svc) {
            callsError = "服务未就绪（ClaudeService 未注册）";
        } else {
            auto rR = svc->recentCalls(3);
            if (rR.isOk()) {
                recent = std::move(rR).value();
            } else {
                callsError = rR.errorMessage();
                LOG_ERROR("调用记录加载失败: " + callsError);
            }
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

        // 详情面板展示的 profile：优先用户选中的，其次激活的
        const Profile* shown = nullptr;
        for (const auto& p : profiles) {
            if (p.id == selectedId_) { shown = &p; break; }
        }
        if (!shown && hasActive) shown = &activeProfile;

        const std::string nameValue = shown ? shown->name : std::string("(未启用)");
        const std::string modelValue = shown ? shown->model : std::string("-");
        const std::string urlValue = shown ? shown->baseUrl : std::string("-");
        const std::string tokenValue = shown
            ? (shown->apiKeyHint + " [Keychain]")
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
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .lineHeight(18.0f)
                                            .color(proxyRunning ? t.colors.success
                                                                : t.colors.textTertiary)
                                            .fontWeight(500)
                                            .build();
                                    })
                                    .build();

                                // 代理未运行时不显示假地址，改为可操作的提示
                                const std::string subtitle = proxyRunning
                                    ? (std::string("管理多套 Claude API 配置 · 本地代理 http://127.0.0.1") +
                                       portStr)
                                    : std::string("管理多套 Claude API 配置 · 代理已停止，点右上角启动");
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
                                // 停止/启动代理（真实开关）
                                components::button(ui, "claude.btn.proxy")
                                    .size(110.0f, 40.0f)
                                    .text(proxyRunning ? "⏸ 停止代理" : "▶ 启动代理")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { doToggleProxy(); })
                                    .build();

                                // 新建 Profile
                                components::button(ui, "claude.btn.new")
                                    .size(140.0f, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.primary, t.colors.primaryHover,
                                            t.colors.primaryPressed)
                                    .textColor(eui::Color("#FFFFFF"))
                                    .radius(t.sizes.radius)
                                    .onClick([this] { openCreateForm(); })
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
                                    .fontSize(t.sizes.fontSizeBody)
                                    .lineHeight(20.0f)
                                    .fontWeight(600)
                                    .color(t.colors.textPrimary)
                                    .build();

                                // 卡片列表（真实数据；空时显示占位）
                                const float cardY0 = 56.0f;
                                const float cardGap = 12.0f;
                                if (!profilesError.empty()) {
                                    ui.text("claude.profiles.error")
                                        .position(20.0f, cardY0 + 20.0f)
                                        .size(leftW - 40.0f, 60.0f)
                                        .text("Profile 列表加载失败：" + profilesError)
                                        .fontSize(12.0f)
                                        .lineHeight(20.0f)
                                        .color(t.colors.error)
                                        .build();
                                } else if (profiles.empty()) {
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
                                        drawProfileCard(ui,
                                                        "claude.profile." + std::to_string(i),
                                                        cardY0 + static_cast<float>(i) *
                                                            (96.0f + cardGap),
                                                        leftW - 24.0f, t, profiles[i]);
                                    }
                                }

                                // 底部新建按钮
                                components::button(ui, "claude.profiles.new")
                                    .position(20.0f, bodyH - 64.0f)
                                    .size(leftW - 40.0f, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textPrimary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { openCreateForm(); })
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
                                    .fontSize(t.sizes.fontSizeLarge)
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
                                    .fontSize(t.sizes.fontSizeSmall)
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
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(32.0f)
                                    .color(t.colors.textSecondary)
                                    .build();

                                // 操作按钮组（全量接线）
                                const float actY = fy0 + fieldH + 130.0f;
                                const float actGap = 12.0f;
                                const float actW = (w0 - actGap * 3.0f) / 4.0f;
                                ui.row("claude.detail.actions")
                                    .size(w0, 36.0f)
                                    .gap(actGap)
                                    .content([&] {
                                        const int64_t shownId = shown ? shown->id : 0;
                                        const Profile shownCopy = shown
                                            ? *shown : Profile{};

                                        // 启用（设为激活）
                                        components::button(ui, "claude.act.enable")
                                            .size(actW, 36.0f)
                                            .text("启用")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.primary,
                                                    t.colors.primaryHover,
                                                    t.colors.primaryPressed)
                                            .textColor(eui::Color("#FFFFFF"))
                                            .radius(t.sizes.radius)
                                            .onClick([this, shownId] {
                                                if (shownId == 0) {
                                                    showToast("启用失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                doSetActive(shownId);
                                            })
                                            .build();
                                        // 测试连通
                                        components::button(ui, "claude.act.test")
                                            .size(actW, 36.0f)
                                            .text("测试连通")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownCopy] {
                                                if (shownCopy.id == 0) {
                                                    showToast("测试连通失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                doTestConnection(shownCopy);
                                            })
                                            .build();
                                        // 编辑（预填现有值）
                                        components::button(ui, "claude.act.edit")
                                            .size(actW, 36.0f)
                                            .text("编辑")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.textPrimary)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownCopy] {
                                                if (shownCopy.id == 0) {
                                                    showToast("编辑失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                openEditForm(shownCopy);
                                            })
                                            .build();
                                        // 删除（确认 dialog）
                                        components::button(ui, "claude.act.del")
                                            .size(actW, 36.0f)
                                            .text("删除")
                                            .fontSize(t.sizes.fontSizeSmall)
                                            .colors(t.colors.surfaceElevated,
                                                    t.colors.hover, t.colors.active)
                                            .textColor(t.colors.error)
                                            .radius(t.sizes.radius)
                                            .border(1.0f, t.colors.borderDefault)
                                            .onClick([this, shownCopy] {
                                                if (shownCopy.id == 0) {
                                                    showToast("删除失败",
                                                              "请先新建或选择一个 Profile",
                                                              true);
                                                    return;
                                                }
                                                deleteTargetId_ = shownCopy.id;
                                                deleteTargetName_ = shownCopy.name;
                                                deleteConfirmOpen_ = true;
                                            })
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
                                    .fontSize(t.sizes.fontSizeBody)
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
                                if (!callsError.empty()) {
                                    ui.text("claude.calls.error")
                                        .position(colTimeX, tableY + 24.0f)
                                        .size(w0 - 80.0f, 16.0f)
                                        .text("调用记录加载失败：" + callsError)
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.error)
                                        .build();
                                } else if (recent.empty()) {
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

                                // 查看全部 →（可点击：弹完整调用记录）
                                ui.rect("claude.calls.viewall.hit")
                                    .position(x0, tableY + 104.0f)
                                    .size(w0, 24.0f)
                                    .color(t.colors.surface)
                                    .hoverColor(t.colors.hover)
                                    .radius(t.sizes.radiusSmall)
                                    .onClick([this] { openCallsDialog(); })
                                    .build();
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

        // 弹窗（画在最上层）
        drawProfileFormDialog(ui, screen, t, themeTokens);
        drawDeleteConfirmDialog(ui, screen, t, themeTokens);
        drawCallsDialog(ui, screen, t, themeTokens);
        harvestTestResult();  // 后台「测试连通」结果回 UI（每帧轮询）
        drawToast(ui, screen, themeTokens);

        LOG_DEBUG("Claude 模块 compose 完成");
    }

    void onStart() override {
        LOG_INFO("Claude 模块启动");
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            LOG_WARN("onStart: ClaudeService::global() 为 null");
            return;
        }
        // 启动时孤儿密钥清理（一次扫描，删除无对应 profile 的 keychain 条目）
        auto orphan = svc->cleanupOrphanKeychainEntries();
        if (orphan.isFail()) {
            LOG_WARN("onStart: 孤儿 Keychain 清理失败 - " + orphan.errorMessage());
        }
        // 硬性需求 1（provider-config Phase B）：代理默认不启动，不再兜底
        // startProxy——由 main.cpp 按设置页「随应用启动」门控，或用户点本页
        // 右上角按钮手动启动；这里只做状态日志
        if (svc->isProxyRunning()) {
            LOG_INFO("onStart: 代理已在运行 port=" +
                     std::to_string(svc->proxyPort()));
        } else {
            LOG_INFO("onStart: 代理未运行（默认不自动启动，等待用户手动启动）");
        }
    }

    void onStop() override {
        LOG_INFO("Claude 模块停止");
        ClaudeService* svc = ClaudeService::global();
        if (svc && svc->isProxyRunning()) {
            svc->stopProxy();
            LOG_INFO("onStop: 代理已停止");
        }
    }

private:
    // ============ Profile 卡片（点击选中，选中/激活高亮） ============
    void drawProfileCard(eui::Ui& ui,
                         const std::string& id,
                         float y,
                         float width,
                         const core::theme::Tokens& t,
                         const Profile& p) {
        const float h = 96.0f;
        const bool selected = (p.id == selectedId_);
        const eui::Color borderColor = (p.isActive || selected)
            ? t.colors.primary : t.colors.borderDefault;
        const float borderW = p.isActive ? 2.0f : 1.0f;
        const eui::Color bg = p.isActive ? t.colors.primarySoftBg : t.colors.surface;

        // 卡片背景（可点击 → 选中后详情面板跟随）
        ui.rect(id + ".bg")
            .position(0.0f, y)
            .size(width, h)
            .states(bg, t.colors.hover, t.colors.active)
            .radius(t.sizes.radiusLarge)
            .border(borderW, borderColor)
            .onClick([this, pid = p.id] { selectedId_ = pid; })
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
        const std::string initial = p.name.empty()
            ? std::string("?")
            : std::string(1, p.name[0]);
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
            .text(p.name)
            .fontSize(t.sizes.fontSizeBody)
            .lineHeight(20.0f)
            .fontWeight(600)
            .color(t.colors.textPrimary)
            .build();

        if (p.isActive) {
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
            .text(p.baseUrl)
            .fontSize(11.0f)
            .lineHeight(14.0f)
            .color(t.colors.textTertiary)
            .build();

        // model (mono, 12px)
        ui.text(id + ".model")
            .position(64.0f, y + 62.0f)
            .size(width - 80.0f, 14.0f)
            .text(std::string("model: ") + p.model)
            .fontSize(11.0f)
            .lineHeight(14.0f)
            .color(t.colors.textSecondary)
            .build();
    }

    // ============ 新建/编辑 Profile 表单弹窗 ============
    void openCreateForm() {
        formEditId_ = 0;   // 0 = 新建
        formName_.clear();
        formUrl_.clear();
        formModel_.clear();
        formKey_.clear();
        formError_.clear();
        formKeyHint_.clear();
        formOpen_ = true;
    }

    void openEditForm(const Profile& p) {
        formEditId_ = p.id;
        formName_ = p.name;
        formUrl_ = p.baseUrl;
        formModel_ = p.model;
        formKey_.clear();   // 留空 = 不换 key（updateProfile 语义）
        formError_.clear();
        formKeyHint_ = p.apiKeyHint;
        formOpen_ = true;
    }

    void submitForm() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            formError_ = "服务未就绪（ClaudeService 未注册）";
            return;
        }
        if (formName_.empty() || formUrl_.empty()) {
            formError_ = "名称和 Base URL 不能为空";
            return;
        }
        if (formEditId_ == 0) {
            // 新建：API Key 必填
            if (formKey_.empty()) {
                formError_ = "新建 Profile 必须填写 API Key";
                return;
            }
            auto r = svc->createProfile(formName_, formUrl_, formKey_, formModel_);
            if (r.isFail()) {
                formError_ = r.errorMessage();
                return;
            }
            formOpen_ = false;
            selectedId_ = r.value().id;
            showToast("已创建", "Profile「" + formName_ + "」已保存", false);
        } else {
            // 编辑：先取现有行（保留 keychain 引用等字段），再覆盖表单值
            auto got = svc->getProfile(formEditId_);
            if (got.isFail()) {
                formError_ = got.errorMessage();
                return;
            }
            Profile p = std::move(got).value();
            p.name = formName_;
            p.baseUrl = formUrl_;
            p.model = formModel_;
            p.apiKeyPlain = formKey_;   // 空 = 不换 key
            auto r = svc->updateProfile(p);
            if (r.isFail()) {
                formError_ = r.errorMessage();
                return;
            }
            formOpen_ = false;
            showToast("已保存", "Profile「" + formName_ + "」已更新", false);
        }
    }

    void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 520.0f;
        const float h = 440.0f;
        const bool isEdit = (formEditId_ != 0);

        components::dialog(ui, "claude.dialog.form")
            .open(formOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { formOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.form.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text(isEdit ? "编辑 Profile" : "新建 Profile")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // 名称
                ui.text("claude.dialog.form.name.label")
                    .position(28.0f, 62.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("名称")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "claude.dialog.form.name")
                    .position(28.0f, 80.0f)
                    .size(w - 56.0f, 40.0f)
                    .value(formName_)
                    .placeholder("例如：官方 / 中转 A")
                    .fontSize(14.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { formName_ = v; })
                    .build();

                // Base URL
                ui.text("claude.dialog.form.url.label")
                    .position(28.0f, 132.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("Base URL")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "claude.dialog.form.url")
                    .position(28.0f, 150.0f)
                    .size(w - 56.0f, 40.0f)
                    .value(formUrl_)
                    .placeholder("https://api.anthropic.com")
                    .fontSize(14.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { formUrl_ = v; })
                    .build();

                // 模型
                ui.text("claude.dialog.form.model.label")
                    .position(28.0f, 202.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("默认模型")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                components::input(ui, "claude.dialog.form.model")
                    .position(28.0f, 220.0f)
                    .size(w - 56.0f, 40.0f)
                    .value(formModel_)
                    .placeholder("claude-sonnet-4-5")
                    .fontSize(14.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { formModel_ = v; })
                    .build();

                // API Key
                ui.text("claude.dialog.form.key.label")
                    .position(28.0f, 272.0f)
                    .size(w - 56.0f, 16.0f)
                    .text("API Key（存入 Keychain，不落数据库）")
                    .fontSize(11.0f)
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();
                const std::string keyPlaceholder = isEdit
                    ? ("留空 = 保持现有 Key（" + formKeyHint_ + "）")
                    : std::string("sk-ant-...");
                components::input(ui, "claude.dialog.form.key")
                    .position(28.0f, 290.0f)
                    .size(w - 56.0f, 40.0f)
                    .value(formKey_)
                    .placeholder(keyPlaceholder)
                    .fontSize(14.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) { formKey_ = v; })
                    .build();

                // 错误信息（Result 的中文错误）
                if (!formError_.empty()) {
                    ui.text("claude.dialog.form.error")
                        .position(28.0f, 340.0f)
                        .size(w - 56.0f, 18.0f)
                        .text(formError_)
                        .fontSize(12.0f)
                        .lineHeight(18.0f)
                        .color(t.colors.error)
                        .build();
                }

                // 底部按钮
                components::button(ui, "claude.dialog.form.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { formOpen_ = false; })
                    .build();
                components::button(ui, "claude.dialog.form.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text(isEdit ? "保存" : "创建")
                    .fontSize(14.0f)
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] { submitForm(); })
                    .build();
            })
            .build();
    }

    // ============ 删除确认弹窗 ============
    void drawDeleteConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                                 const core::theme::Tokens& t,
                                 const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 420.0f;
        const float h = 190.0f;

        components::dialog(ui, "claude.dialog.delete")
            .open(deleteConfirmOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { deleteConfirmOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.delete.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("确认删除？")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("claude.dialog.delete.msg")
                    .position(28.0f, 58.0f)
                    .size(w - 56.0f, 40.0f)
                    .text("将删除 Profile「" + deleteTargetName_ +
                          "」及其 Keychain 中的 API Key，不可恢复。")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();

                components::button(ui, "claude.dialog.delete.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { deleteConfirmOpen_ = false; })
                    .build();
                components::button(ui, "claude.dialog.delete.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("确认删除")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.error)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        const int64_t id = deleteTargetId_;
                        deleteConfirmOpen_ = false;
                        doDelete(id);
                    })
                    .build();
            })
            .build();
    }

    // ============ 完整调用记录弹窗（「查看全部」入口） ============
    void openCallsDialog() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            showToast("加载失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        auto r = svc->recentCalls(20);
        if (r.isFail()) {
            showToast("加载失败", r.errorMessage(), true);
            return;
        }
        allCalls_ = std::move(r).value();
        callsDialogOpen_ = true;
    }

    void drawCallsDialog(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 720.0f;
        const float h = 560.0f;

        components::dialog(ui, "claude.dialog.calls")
            .open(callsDialogOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { callsDialogOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.calls.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("调用记录（最近 20 条）")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                if (allCalls_.empty()) {
                    ui.text("claude.dialog.calls.empty")
                        .position(28.0f, 72.0f)
                        .size(w - 56.0f, 20.0f)
                        .text("(暂无调用记录)")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                    for (size_t i = 0; i < allCalls_.size(); ++i) {
                        const ClaudeService::CallLogEntry& c = allCalls_[i];
                        const float rowY = 68.0f + static_cast<float>(i) * 22.0f;
                        const std::string line =
                            formatHHMM(c.startedAt) + "  " + c.method + " " +
                            c.path + "  " +
                            (c.statusCode > 0 ? std::to_string(c.statusCode)
                                              : std::string("-")) +
                            "  " + formatLatency(c.latencyMs);
                        const eui::Color rowColor =
                            (c.statusCode >= 200 && c.statusCode < 300)
                                ? t.colors.textSecondary
                                : t.colors.error;
                        ui.text("claude.dialog.calls.row." + std::to_string(i))
                            .position(28.0f, rowY)
                            .size(w - 56.0f, 18.0f)
                            .text(line)
                            .fontSize(12.0f)
                            .lineHeight(18.0f)
                            .color(rowColor)
                            .build();
                    }
                }

                components::button(ui, "claude.dialog.calls.close")
                    .position(28.0f, h - 60.0f)
                    .size(w - 56.0f, 40.0f)
                    .text("关闭")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover,
                            t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { callsDialogOpen_ = false; })
                    .build();
            })
            .build();
    }

    // ============ Toast 反馈 ============
    void drawToast(eui::Ui& ui, const eui::Screen& screen,
                   const components::theme::ThemeColorTokens& themeTokens) {
        components::toast(ui, "claude.toast")
            .theme(themeTokens)
            .screen(screen.width, screen.height)
            .visible(toastOpen_)
            .duration(3.5f)
            .title(toastTitle_)
            .message(toastMessage_)
            .onAutoDismiss([this] { toastOpen_ = false; })
            .onDismiss([this] { toastOpen_ = false; })
            .build();
    }

    void showToast(const std::string& title, const std::string& msg, bool isError) {
        toastTitle_ = isError ? ("⚠ " + title) : ("✓ " + title);
        toastMessage_ = msg;
        toastOpen_ = true;
    }

    // ============ 操作（全部真接 ClaudeService） ============

    void doToggleProxy() {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) {
            showToast("代理操作失败", "服务未就绪（ClaudeService 未注册）", true);
            return;
        }
        if (svc->isProxyRunning()) {
            svc->stopProxy();
            showToast("代理已停止", "本地代理已停止监听", false);
            return;
        }
        auto r = svc->startProxy("127.0.0.1", 8888);
        if (r.isFail()) {
            showToast("启动代理失败", r.errorMessage(), true);
            return;
        }
        // 手动启动是 Phase B 后的主启动路径，凭据文件同步重写
        //（此前只有 main 自启 / 设置页重生成两条路径写）
        writeProxyTokenFile(svc->proxyAuthToken(), r.value());
        showToast("代理已启动",
                  "监听 http://127.0.0.1:" + std::to_string(r.value()), false);
    }

    void doSetActive(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        auto r = svc->setActiveProfile(id);
        if (r.isFail()) {
            showToast("启用失败", r.errorMessage(), true);
            return;
        }
        selectedId_ = id;
        showToast("已启用", "该 Profile 已设为激活（ACTIVE）", false);
    }

    void doDelete(int64_t id) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        auto r = svc->deleteProfile(id);
        if (r.isFail()) {
            showToast("删除失败", r.errorMessage(), true);
            return;
        }
        if (selectedId_ == id) selectedId_ = 0;
        showToast("已删除", "Profile 已删除（含 Keychain 条目）", false);
    }

    void doTestConnection(const Profile& p) {
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return;
        // 先收上一轮结果（页面不可见时 compose 不跑，会在这里积累）
        harvestTestResult();
        if (testThread_.joinable()) {
            showToast("测试进行中", "请等待上一轮测试完成", false);
            return;
        }
        // ponytail 升级路径已落地：testConnection 是同步 HTTP（timeout 15s），
        // 在 UI 线程直接调最坏冻结界面 15s → 挪后台线程，立即 toast「测试中」，
        // 结果经原子标志回 compose 每帧收割（模块析构时 join 防悬垂）
        showToast("测试中", "正在请求 " + p.baseUrl + "/v1/models，最长 15 秒…", false);
        testThread_ = std::thread([this, p, svc] {
            auto r = svc->testConnection(p);
            if (r.isFail()) {
                testResultTitle_ = "连通失败";
                testResultMsg_ = r.errorMessage();
                testResultIsError_ = true;
            } else {
                const int code = r.value().statusCode;
                if (code >= 200 && code < 300) {
                    testResultTitle_ = "连通成功";
                    testResultMsg_ = "上游返回 HTTP " + std::to_string(code) + "，API 可用";
                    testResultIsError_ = false;
                } else {
                    testResultTitle_ = "连通异常";
                    testResultMsg_ = "上游返回 HTTP " + std::to_string(code) +
                                      "（检查 API Key 或 Base URL）";
                    testResultIsError_ = true;
                }
            }
            testDone_.store(true, std::memory_order_release);
        });
    }

    // 收割后台测试结果：线程跑完后 join 并弹结果 toast（compose 每帧调）
    void harvestTestResult() {
        if (!testThread_.joinable()) return;
        if (!testDone_.load(std::memory_order_acquire)) return;  // 还在跑
        testThread_.join();
        testDone_.store(false);
        showToast(testResultTitle_, testResultMsg_, testResultIsError_);
    }

    // ============ 状态 ============

    int64_t selectedId_ = 0;   // 0 = 跟随激活 profile

    // 新建/编辑表单
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    std::string formName_;
    std::string formUrl_;
    std::string formModel_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;

    // 删除确认
    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;

    // 完整调用记录弹窗
    bool callsDialogOpen_ = false;
    std::vector<ClaudeService::CallLogEntry> allCalls_;

    // Toast
    bool toastOpen_ = false;
    std::string toastTitle_;
    std::string toastMessage_;

    // 测试连通后台线程（结果经 testDone_ 原子标志回 compose 收割；
    // 普通成员的发布顺序由 release/acquire + join 保证）
    std::thread testThread_;
    std::atomic<bool> testDone_{false};
    std::string testResultTitle_;
    std::string testResultMsg_;
    bool testResultIsError_ = false;
};

} // namespace module::claude

extern "C" ::core::Module* create_claude_module() {
    return new module::claude::ClaudeModule();
}
