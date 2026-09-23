// src/module_claude_proxy/claude_module.cpp
// Claude 配置切换器模块 - 完整 UI + 操作按钮全量接线（真实 ClaudeService）
//
// 接线清单（W5）：
//   新建/编辑 Profile（dialog 表单）→ createProfile / updateProfile
//   启用 → setActiveProfile；删除（确认 dialog）→ deleteProfile
//   测试连通 → testConnection；代理开关 → startProxy / stopProxy
//   每次操作后靠下一帧 compose 重新拉取数据（本模块 compose 直读 service）
#include "claude_service.h"
#include "provider_config.h"
#include "settings_sync.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "core/time.h"
#include "core/component_theme.h"
#include "platform/json.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/dropdown.h>
#include <components/input.h>
#include <components/scrollview.h>
#include <components/tabs.h>
#include <components/theme.h>
#include <components/toast.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace module::claude {

namespace {

// 格式化毫秒数为 "1234ms" / "1.2s"
std::string formatLatency(int64_t ms) {
    if (ms <= 0) return "-";
    if (ms < 1000) return std::to_string(static_cast<int>(ms)) + "ms";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
    return std::string(buf);
}

// 按字节截断 UTF-8 串（审计 [11]：错误行尾附 error 摘要，过长会挤爆行宽）。
// 截点回退到字符边界，不把多字节字符切成半个（乱码方块）
std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "…";
}

// 调用记录弹窗状态码过滤（审计 [11]，spec §8.3 第 6 条「支持过滤、搜索」）：
// 下拉索引 → ClaudeService::CallLogQuery::statusClass 映射
constexpr int kCallsStatusClasses[] = {0, 2, 3, 4, 5, 9};
const char* const kCallsStatusLabels[] = {
    "全部状态", "2xx 成功", "3xx 重定向", "4xx 客户端错误",
    "5xx 服务端错误", "错误（超时等）"};
constexpr int kCallsStatusCount = 6;
// 日志弹窗每页条数（审计 [11]；总设计只承诺「过滤、搜索」，条数由 UI 自定）
constexpr int kCallsPageSize = 20;

// 模型映射摘要（§6.2 详情面板一行式）：「默认 <model> · 槽位已配 N/6」
std::string modelsSummary(const Profile& p) {
    const ModelsConfig mc = effectiveModelsConfig(p.modelsConfigJson, p.model);
    const ModelSlot slots[] = {mc.def, mc.sonnet, mc.opus,
                               mc.haiku, mc.fable, mc.subagent};
    int configured = 0;
    for (const auto& s : slots) {
        if (!s.model.empty()) ++configured;
    }
    if (mc.def.model.empty()) {
        return configured > 0
            ? ("（默认未配）· 槽位已配 " + std::to_string(configured) + "/6")
            : std::string("-");
    }
    return "默认 " + mc.def.model + " · 槽位已配 " +
           std::to_string(configured) + "/6";
}

// 「导入当前配置」名称建议：BASE_URL host（剥 scheme/路径/端口），
// 提取不到（无 BASE_URL）→「导入 <日期>」；用户可改
std::string suggestImportName(const std::string& baseUrl) {
    size_t start = baseUrl.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = baseUrl.find('/', start);
    if (end == std::string::npos) end = baseUrl.size();
    std::string host = baseUrl.substr(start, end - start);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) host.resize(colon);
    if (!host.empty()) return "导入 " + host;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    core::time::localTime(t, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return "导入 " + std::string(buf);
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
        const auto themeTokens = core::theme::componentTokens(t);
        const float pad = t.sizes.spacingXXLarge;        // 40
        const float contentW = screen.width - pad * 2.0f;
        const float contentH = screen.height - pad * 2.0f;
        const std::time_t now = std::time(nullptr);   // 调用时间列「今天/昨天」判定
        const float headerH = 64.0f;
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

        // 引导条（§6.4）：代理未运行且本会话从未启动过时显示（内存态一次性）
        if (proxyRunning) proxyEverSeen_ = true;
        const bool showGuide = !proxyRunning && !proxyEverSeen_;
        const float bodyH = contentH - headerH - t.sizes.spacingLarge -
                            (showGuide ? 30.0f : 0.0f);

        // 页面底色 = 主窗口 clearColor（随主题/窗口效果档运行时跟随，
        // 磨砂 Phase B/D）——不自铺不透明底，半透/磨砂档主体才生效

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
        const std::string modelValue = shown ? modelsSummary(*shown)
                                             : std::string("-");
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
                                    .text(proxyRunning ? "停止代理" : "启动代理")
                                    .icon(proxyRunning ? 0xF04C : 0xF04B)  // fa-pause / fa-play
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .iconColor(t.colors.textSecondary)
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
                                    .textColor(t.colors.onPrimary)
                                    .radius(t.sizes.radius)
                                    .onClick([this] { openCreateForm(); })
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // 引导条（§6.4）：代理未运行且本会话从未启动过时一次性提示接管语义
                //（ⓘ 换 FA：Icon 字体不能与正文混排，图标/文字拆两个元素）
                if (showGuide) {
                    ui.row("claude.guide.wrap")
                        .size(contentW, 18.0f)
                        .gap(6.0f)
                        .alignItems(eui::Align::CENTER)
                        .content([&] {
                            ui.text("claude.guide.icon")
                                .size(14.0f, 18.0f)
                                .icon(0xF05A)  // fa-circle-info
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
                                .color(t.colors.textSecondary)
                                .build();
                            ui.text("claude.guide.takeover")
                                .size(contentW - 20.0f, 18.0f)
                                .text("启动代理后将自动接管 Claude Code 配置（" +
                                      settings_sync::defaultPaths().settingsFile.string() +
                                      "），停止代理时自动还原；其余字段原样保留")
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(18.0f)
                                .color(t.colors.textSecondary)
                                .build();
                        })
                        .build();
                }

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
                                    // 左栏 Profile 列表 = 结构性面板 → background
                                    .color(t.colors.background)
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
                                    // 卡片列表套 scrollView（产品审计 2026-09-23
                                    // [26]）：此前固定 y=56+i*108 平铺，profile 数
                                    // 一多就画出左栏边界、叠在底部按钮上且无法滚
                                    // 动。仿 Todo 列内滚动 / settings 卡片区。
                                    // 滚动区底 = 底部按钮（bodyH-64）上方 12px
                                    const float scrollH = std::max(
                                        0.0f, bodyH - 64.0f - 12.0f - cardY0);
                                    // contentKey = profile id 序列 hash（仿 md.tree，
                                    // W7 教训：数量相同但内容替换也要失效重测）
                                    size_t listKey = 0;
                                    for (const auto& p : profiles) {
                                        listKey = listKey * 1315423911u +
                                                  static_cast<size_t>(p.id);
                                    }
                                    components::scrollView(ui, "claude.profiles.scroll")
                                        .position(0.0f, cardY0)
                                        .size(leftW - 24.0f, scrollH)
                                        .gap(cardGap)
                                        .step(96.0f + cardGap)
                                        .theme(themeTokens)
                                        .contentKey("claude.profiles.scroll." +
                                                    std::to_string(listKey))
                                        .offset(profilesScrollOffset_)
                                        .onChange([this](float v) {
                                            profilesScrollOffset_ = v;
                                        })
                                        .content([&](eui::Ui& inner, float contentWidth,
                                                     float viewportHeight) {
                                            (void)viewportHeight;
                                            for (size_t i = 0; i < profiles.size(); ++i) {
                                                const std::string cardId =
                                                    "claude.profile." + std::to_string(i);
                                                // 行包 stack（position 只在 stack 内
                                                // 生效，仿 todo 列卡片）
                                                inner.stack(cardId + ".wrap")
                                                    .size(contentWidth, 96.0f)
                                                    .content([&] {
                                                        drawProfileCard(
                                                            inner, cardId, 0.0f,
                                                            contentWidth, t, profiles[i]);
                                                    })
                                                    .build();
                                            }
                                        })
                                        .build();
                                }

                                // 底部按钮：新建 + 导入当前配置（读
                                // ~/.claude/settings.json 预填新建表单）
                                const float btnW = (leftW - 40.0f - 12.0f) * 0.5f;
                                components::button(ui, "claude.profiles.new")
                                    .position(20.0f, bodyH - 64.0f)
                                    .size(btnW, 40.0f)
                                    .text("+ 新建 Profile")
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textPrimary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { openCreateForm(); })
                                    .build();
                                components::button(ui, "claude.profiles.import")
                                    .position(20.0f + btnW + 12.0f, bodyH - 64.0f)
                                    .size(btnW, 40.0f)
                                    .text("导入当前配置")
                                    .icon(0xF019)  // fa-download
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .colors(t.colors.surfaceElevated, t.colors.hover,
                                            t.colors.active)
                                    .textColor(t.colors.textPrimary)
                                    .iconColor(t.colors.textPrimary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { doImportCurrentConfig(); })
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
                                    // 右栏详情面板 = 结构性面板 → background
                                    .color(t.colors.background)
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

                                // 模型映射摘要（右，§6.2）
                                const float col2x = x0 + (w0 + 16.0f) / 2.0f;
                                ui.text("claude.field.model.label")
                                    .position(col2x, fy0)
                                    .size((w0 - 16.0f) / 2.0f, 14.0f)
                                    .text("模型映射")
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

                                // Claude Code 接管状态（Phase D，数据源 service）
                                const bool ccManaged = svc && svc->isClaudeCodeManaged();
                                const auto& ccWarnings =
                                    svc ? svc->lastSyncWarnings()
                                        : std::vector<std::string>();
                                const std::string ccPath =
                                    settings_sync::defaultPaths()
                                        .settingsFile.string();
                                ui.text("claude.field.takeover.value")
                                    .position(x0, fy0 + fieldH + 110.0f)
                                    .size(w0, 16.0f)
                                    .text(ccManaged
                                          ? ("● Claude Code 已接管 " + ccPath)
                                          : std::string("○ Claude Code 未接管"
                                                        "（启动代理后自动接管，"
                                                        "停止时还原）"))
                                    .fontSize(11.0f)
                                    .lineHeight(16.0f)
                                    .color(ccManaged ? t.colors.success
                                                     : t.colors.textTertiary)
                                    .build();
                                // 上层覆盖警告（§5.3 检测命中时；首条 + 计数）
                                //（⚠ 换 FA：图标/文字拆两个元素）
                                if (ccManaged && !ccWarnings.empty()) {
                                    ui.text("claude.field.takeover.warn.icon")
                                        .position(x0, fy0 + fieldH + 126.0f)
                                        .size(14.0f, 16.0f)
                                        .icon(0xF071)  // fa-triangle-exclamation
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.warning)
                                        .build();
                                    ui.text("claude.field.takeover.warn")
                                        .position(x0 + 18.0f, fy0 + fieldH + 126.0f)
                                        .size(w0 - 18.0f, 16.0f)
                                        .text(ccWarnings.front() +
                                              (ccWarnings.size() > 1
                                                   ? "（共 " +
                                                         std::to_string(
                                                             ccWarnings.size()) +
                                                         " 条，详见设置页）"
                                                   : ""))
                                        .fontSize(11.0f)
                                        .lineHeight(16.0f)
                                        .color(t.colors.warning)
                                        .build();
                                }

                                // 操作按钮组（全量接线）
                                const float actY = fy0 + fieldH +
                                    (ccManaged && !ccWarnings.empty()
                                         ? 152.0f : 130.0f);
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
                                            .textColor(t.colors.onPrimary)
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
                                            .text(formatCallTime(c.startedAt, now))
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
                                    // 命中区底 = 所在详情面板底（background）
                                    .color(t.colors.background)
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
                .color(t.colors.onPrimary)
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

        // model（default 槽位，未配则旧 model 列 fallback——§6.2 列表卡片）
        ui.text(id + ".model")
            .position(64.0f, y + 62.0f)
            .size(width - 80.0f, 14.0f)
            .text(std::string("model: ") +
                  effectiveModelsConfig(p.modelsConfigJson, p.model).def.model)
            .fontSize(11.0f)
            .lineHeight(14.0f)
            .color(t.colors.textSecondary)
            .build();
    }

    // ============ 新建/编辑 Profile 表单弹窗（tabs 三页，§6.1） ============
    void openCreateForm() {
        formEditId_ = 0;   // 0 = 新建
        formTab_ = 0;
        formName_.clear();
        formUrl_.clear();
        formKey_.clear();
        formError_.clear();
        formKeyHint_.clear();
        formImportWarn_.clear();
        formModels_ = ModelsConfig{};
        formEnvRows_.clear();
        resetImportState();
        formOpen_ = true;
    }

    void openEditForm(const Profile& p) {
        formEditId_ = p.id;
        formTab_ = 0;
        formName_ = p.name;
        formUrl_ = p.baseUrl;
        formKey_.clear();   // 留空 = 不换 key（updateProfile 语义）
        formError_.clear();
        formKeyHint_ = p.apiKeyHint;
        formImportWarn_.clear();
        // 槽位：models_config + 旧 model 列 fallback（§4.2 兼容读取）
        formModels_ = effectiveModelsConfig(p.modelsConfigJson, p.model);
        // env 行：非法 JSON 读路径容错为空表（不炸）
        formEnvRows_.clear();
        auto ee = parseExtraEnv(p.extraEnvJson);
        if (ee.isOk()) {
            for (const auto& [k, v] : ee.value()) {
                formEnvRows_.emplace_back(k, v);
            }
        }
        resetImportState();
        formOpen_ = true;
    }

    void resetImportState() {
        importText_.clear();
        importReady_ = false;
        importError_.clear();
        importPreview_ = ImportedConfig{};
    }

    // 导入解析（纯函数消费，Phase D）：失败只写红字，不动表单
    void doParseImport() {
        importError_.clear();
        importReady_ = false;
        auto r = parseImportedEnv(importText_);
        if (r.isFail()) {
            importError_ = r.errorMessage();
            return;
        }
        importPreview_ = std::move(r).value();
        importReady_ = true;
    }

    // 确认导入：按解析结果填充三页表单（BASE_URL/token → 基础页，
    // 槽位非空覆盖，env 键同名替换 / 新键追加）
    void doApplyImport() {
        if (!importReady_) return;
        if (!importPreview_.baseUrl.empty()) formUrl_ = importPreview_.baseUrl;
        if (!importPreview_.authToken.empty()) {
            formKey_ = importPreview_.authToken;
        }
        auto mergeSlot = [this](ModelSlot ModelsConfig::* m) {
            const ModelSlot& src = importPreview_.models.*m;
            if (!src.model.empty()) formModels_.*m = src;
        };
        mergeSlot(&ModelsConfig::def);
        mergeSlot(&ModelsConfig::sonnet);
        mergeSlot(&ModelsConfig::opus);
        mergeSlot(&ModelsConfig::haiku);
        mergeSlot(&ModelsConfig::fable);
        mergeSlot(&ModelsConfig::subagent);
        for (const auto& [k, v] : importPreview_.extraEnv) {
            bool replaced = false;
            for (auto& row : formEnvRows_) {
                if (row.first == k) {
                    row.second = v;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) formEnvRows_.emplace_back(k, v);
        }
        importReady_ = false;
        importText_.clear();
        formError_.clear();   // 导入成功清掉旧校验错误
    }

    // ============ 导入当前配置（一键读 settings.json → 预填新建表单）============
    // 等价于用户手动「从 cc-switch / settings.json 粘贴导入」但全自动：
    // 读用户级 settings.json → parseImportedEnv → 打开新建弹窗并预填
    // （基础页 BASE_URL/Token、模型映射页槽位、高级 env 页 + 预览标记）。
    // 接管态（sidecar 快照存在）时 settings.json 里是代理托管值
    // （BASE_URL=127.0.0.1:8888）而非原始上游 → 弹窗内给警告，不阻断。
    void doImportCurrentConfig() {
        // 路径解析：service 注入优先（生产 = main 注入的 defaultPaths，
        // 测试 = 临时目录注入），未注入（service 为 null 的防御分支）回落
        // defaultPaths()——与设置页/引导条的路径口径一致
        settings_sync::Paths paths = settings_sync::defaultPaths();
        ClaudeService* svc = ClaudeService::global();
        if (svc && !svc->settingsSyncPaths().settingsFile.empty()) {
            paths = svc->settingsSyncPaths();
        }

        // 读文件：不存在 → toast，不弹窗
        std::error_code ec;
        if (!std::filesystem::exists(paths.settingsFile, ec) || ec) {
            showToast("导入失败",
                      "未找到 ~/.claude/settings.json（" +
                          paths.settingsFile.string() + "）",
                      true);
            return;
        }
        std::ifstream in(paths.settingsFile, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string content = ss.str();
        // 整段 settings.json 没有 env 键 = 没有可导入的配置（区别于粘贴导入
        // 认裸 env 对象的形状启发：文件只会是 settings.json 形状），给明确
        // 提示而非共享解析器的「键名非法: permissions」误报
        if (auto doc = platform::json::parse(content);
            doc.isOk() && doc.value().isObject() &&
            !doc.value().asObject().has("env")) {
            showToast("导入失败",
                      "settings.json 没有 env 块，无配置可导入", true);
            return;
        }
        auto parsed = parseImportedEnv(content);
        if (parsed.isFail()) {
            showToast("导入失败",
                      "settings.json 解析失败: " + parsed.errorMessage(), true);
            return;
        }

        // 全自动「粘贴导入」：新建表单 → 解析结果 → 应用 → 名称建议
        openCreateForm();
        importPreview_ = std::move(parsed).value();
        importReady_ = true;
        doApplyImport();       // 预填三页表单（结束时清 importReady_）
        importReady_ = true;   // 保留高级 env 页预览标记（识别数 + 警告角标）
        formName_ = suggestImportName(importPreview_.baseUrl);

        // 接管态警告：sidecar 存在 → 导入的是代理配置而非原始上游
        if (settings_sync::hasPendingSnapshot(paths)) {
            formImportWarn_ =
                "当前是接管态，导入的是代理配置而非原始上游"
                "（BASE_URL=127.0.0.1:8888）；建议先停止代理再导入";
        }
    }

    // 表单 → models_config / extra_env JSON（全空 = 空串，保持「未配置」语义）
    bool serializeFormData(std::string* modelsJson, std::string* envJson) {
        const ModelSlot slots[] = {formModels_.def, formModels_.sonnet,
                                   formModels_.opus, formModels_.haiku,
                                   formModels_.fable, formModels_.subagent};
        bool anySlot = false;
        for (const auto& s : slots) {
            if (!s.model.empty() || !s.name.empty()) { anySlot = true; break; }
        }
        if (anySlot) {
            auto r = serializeModelsConfig(formModels_);
            if (r.isFail()) {
                formError_ = "模型配置序列化失败: " + r.errorMessage();
                return false;
            }
            *modelsJson = r.value();
        }
        std::map<std::string, std::string> envMap;   // 空键行跳过
        for (const auto& row : formEnvRows_) {
            if (row.first.empty()) continue;
            envMap[row.first] = row.second;   // 同名后行胜（map 语义）
        }
        if (!envMap.empty()) {
            auto r = serializeExtraEnv(envMap);
            if (r.isFail()) {
                formError_ = "env 序列化失败: " + r.errorMessage();
                return false;
            }
            *envJson = r.value();
        }
        return true;
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
        std::string modelsJson;
        std::string envJson;
        if (!serializeFormData(&modelsJson, &envJson)) return;

        if (formEditId_ == 0) {
            // 新建：API Key 必填
            if (formKey_.empty()) {
                formError_ = "新建 Profile 必须填写 API Key";
                return;
            }
            auto r = svc->createProfile(formName_, formUrl_, formKey_,
                                        formModels_.def.model);
            if (r.isFail()) {
                formError_ = r.errorMessage();
                return;
            }
            // createProfile 无槽位/env 参数（最小 diff 不动签名），二段补写
            if (!modelsJson.empty() || !envJson.empty()) {
                Profile p = r.value();
                p.model = formModels_.def.model;
                p.modelsConfigJson = modelsJson;
                p.extraEnvJson = envJson;
                auto u = svc->updateProfile(p);
                if (u.isFail()) {
                    formError_ = "已创建，但模型/env 写入失败: " +
                                 u.errorMessage();
                    return;
                }
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
            p.model = formModels_.def.model;   // 旧列与 default 槽位同步（§4.2）
            p.modelsConfigJson = modelsJson;
            p.extraEnvJson = envJson;
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

    // ---- 基础页（§6.1 线框）----
    void drawFormBasicTab(eui::Ui& ui, float w,
                          const core::theme::Tokens& t,
                          const components::theme::ThemeColorTokens& themeTokens,
                          bool isEdit) {
        ui.text("claude.form.basic.name.label")
            .position(28.0f, 104.0f).size(w - 56.0f, 16.0f)
            .text("名称").fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.form.name")
            .position(28.0f, 122.0f).size(w - 56.0f, 40.0f)
            .value(formName_).placeholder("例如：官方 / 中转 A")
            .fontSize(14.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { formName_ = v; })
            .build();

        ui.text("claude.form.basic.url.label")
            .position(28.0f, 174.0f).size(w - 56.0f, 16.0f)
            .text("Base URL").fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.dialog.form.url")
            .position(28.0f, 192.0f).size(w - 56.0f, 40.0f)
            .value(formUrl_).placeholder("https://api.anthropic.com")
            .fontSize(14.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { formUrl_ = v; })
            .build();

        ui.text("claude.form.basic.key.label")
            .position(28.0f, 244.0f).size(w - 56.0f, 16.0f)
            .text("API Key（存入 Keychain，不落数据库）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        const std::string keyPlaceholder = isEdit
            ? ("留空 = 保持现有 Key（" + formKeyHint_ + "）")
            : std::string("sk-ant-...");
        components::input(ui, "claude.dialog.form.key")
            .position(28.0f, 262.0f).size(w - 56.0f, 40.0f)
            .value(formKey_).placeholder(keyPlaceholder)
            .fontSize(14.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { formKey_ = v; })
            .build();

        ui.text("claude.form.basic.hint.icon")
            .position(28.0f, 316.0f).size(14.0f, 18.0f)
            .icon(0xF05A).fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.basic.hint")
            .position(46.0f, 316.0f).size(w - 74.0f, 18.0f)
            .text("模型映射和高级 env 在另外两个标签页配置")
            .fontSize(12.0f).lineHeight(18.0f)
            .color(t.colors.textTertiary).build();

        if (!formError_.empty()) {
            ui.text("claude.dialog.form.error")
                .position(28.0f, 340.0f).size(w - 56.0f, 18.0f)
                .text(formError_).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.error).build();
        }

        // 「导入当前配置」接管态警告（不阻断，仅提示；⚠ 图标 + 文字两元素）
        if (!formImportWarn_.empty()) {
            ui.text("claude.dialog.form.importwarn.icon")
                .position(28.0f, 362.0f).size(14.0f, 18.0f)
                .icon(0xF071).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.warning).build();
            ui.text("claude.dialog.form.importwarn")
                .position(46.0f, 362.0f).size(w - 74.0f, 36.0f)
                .text(formImportWarn_).fontSize(12.0f).lineHeight(18.0f)
                .color(t.colors.warning).build();
        }
    }

    // ---- 模型映射页（6 槽位 × model/name，§6.1 线框）----
    void drawFormModelsTab(eui::Ui& ui, float w,
                            const core::theme::Tokens& t,
                            const components::theme::ThemeColorTokens& themeTokens) {
        struct SlotRow {
            const char* label;
            ModelSlot ModelsConfig::*slot;
        };
        static const SlotRow kRows[] = {
            {"默认 (MODEL)", &ModelsConfig::def},
            {"Sonnet",      &ModelsConfig::sonnet},
            {"Opus",        &ModelsConfig::opus},
            {"Haiku",       &ModelsConfig::haiku},
            {"Fable",       &ModelsConfig::fable},
            {"Subagent",    &ModelsConfig::subagent},
        };

        // 表头
        ui.text("claude.form.slots.h.slot")
            .position(28.0f, 104.0f).size(110.0f, 14.0f)
            .text("槽位").fontSize(11.0f).lineHeight(14.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.h.model")
            .position(150.0f, 104.0f).size(330.0f, 14.0f)
            .text("上游模型名（发往上游）").fontSize(11.0f).lineHeight(14.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.h.name")
            .position(492.0f, 104.0f).size(240.0f, 14.0f)
            .text("显示名（CC 界面）").fontSize(11.0f).lineHeight(14.0f)
            .color(t.colors.textTertiary).build();

        for (size_t i = 0; i < 6; ++i) {
            const float y = 126.0f + static_cast<float>(i) * 52.0f;
            const SlotRow& row = kRows[i];
            const ModelSlot& slot = formModels_.*row.slot;
            const std::string idx = std::to_string(i);
            ui.text("claude.form.slot.label." + idx)
                .position(28.0f, y + 10.0f).size(110.0f, 16.0f)
                .text(row.label).fontSize(12.0f).lineHeight(16.0f)
                .color(t.colors.textSecondary).build();
            components::input(ui, "claude.form.slot." + idx + ".model")
                .position(150.0f, y).size(330.0f, 36.0f)
                .value(slot.model).placeholder("fangzhou/glm-5.3[1M]")
                .fontSize(13.0f).theme(themeTokens)
                .onChange([this, m = row.slot](const std::string& v) {
                    (formModels_.*m).model = v;
                })
                .build();
            // 显示名留空 → placeholder 渲染派生预览（§6.1）
            const std::string derived = deriveDisplayName(slot.model);
            components::input(ui, "claude.form.slot." + idx + ".name")
                .position(492.0f, y).size(240.0f, 36.0f)
                .value(slot.name)
                .placeholder(derived.empty() ? "（自动派生）" : derived)
                .fontSize(13.0f).theme(themeTokens)
                .onChange([this, m = row.slot](const std::string& v) {
                    (formModels_.*m).name = v;
                })
                .build();
        }

        ui.text("claude.form.slots.hint1.icon")
            .position(28.0f, 446.0f).size(14.0f, 16.0f)
            .icon(0xF05A).fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.hint1")
            .position(46.0f, 446.0f).size(w - 74.0f, 16.0f)
            .text("显示名留空时自动剥除模型名的 [1M]/[200K] 等上下文标记")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        ui.text("claude.form.slots.hint2")
            .position(28.0f, 464.0f).size(w - 56.0f, 16.0f)
            .text("    槽位留空 = 不写入该 env 键，Claude Code 用内置映射")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
    }

    // ---- 高级 env 页（键值编辑器 + cc-switch 粘贴导入，§6.1 线框）----
    // ponytail: 可见行数上限 5（760×600 弹窗高度约束），超出部分保存在
    // 状态里全量保存；需要编辑超长列表时再引入 scrollView
    void drawFormEnvTab(eui::Ui& ui, float w,
                        const core::theme::Tokens& t,
                        const components::theme::ThemeColorTokens& themeTokens) {
        constexpr size_t kMaxVisibleRows = 5;

        ui.text("claude.form.env.label")
            .position(28.0f, 104.0f).size(w - 56.0f, 16.0f)
            .text("额外环境变量（原样写入 settings.json env 块）")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();

        const size_t visible =
            formEnvRows_.size() < kMaxVisibleRows ? formEnvRows_.size()
                                                  : kMaxVisibleRows;
        for (size_t i = 0; i < visible; ++i) {
            const float y = 124.0f + static_cast<float>(i) * 38.0f;
            const std::string idx = std::to_string(i);
            components::input(ui, "claude.form.env.row." + idx + ".key")
                .position(28.0f, y).size(220.0f, 32.0f)
                .value(formEnvRows_[i].first).placeholder("ENV_KEY")
                .fontSize(12.0f).theme(themeTokens)
                .onChange([this, i](const std::string& v) {
                    formEnvRows_[i].first = v;
                })
                .build();
            components::input(ui, "claude.form.env.row." + idx + ".value")
                .position(258.0f, y).size(414.0f, 32.0f)
                .value(formEnvRows_[i].second).placeholder("值（字符串）")
                .fontSize(12.0f).theme(themeTokens)
                .onChange([this, i](const std::string& v) {
                    formEnvRows_[i].second = v;
                })
                .build();
            components::button(ui, "claude.form.env.row." + idx + ".del")
                .position(682.0f, y).size(50.0f, 32.0f)
                .icon(0xF00D)  // fa-xmark（✕ 换 FA）
                .fontSize(12.0f)
                .colors(t.colors.surfaceElevated, t.colors.hover,
                        t.colors.active)
                .textColor(t.colors.error)
                .iconColor(t.colors.error)
                .radius(t.sizes.radiusSmall)
                .border(1.0f, t.colors.borderDefault)
                .onClick([this, i] {
                    if (i < formEnvRows_.size()) {
                        formEnvRows_.erase(formEnvRows_.begin() +
                                           static_cast<long>(i));
                    }
                })
                .build();
        }

        // 底部：添加按钮 + 溢出说明
        components::button(ui, "claude.form.env.add")
            .position(28.0f, 314.0f).size(110.0f, 30.0f)
            .text("+ 添加变量").fontSize(12.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { formEnvRows_.emplace_back(); })
            .build();
        if (formEnvRows_.size() > kMaxVisibleRows) {
            ui.text("claude.form.env.overflow")
                .position(150.0f, 320.0f).size(w - 150.0f - 28.0f, 16.0f)
                .text("另有 " + std::to_string(formEnvRows_.size() -
                                               kMaxVisibleRows) +
                      " 项已保存（保存时全量写入，列表仅显示前 " +
                      std::to_string(kMaxVisibleRows) + " 项）")
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
        }

        // ---- 粘贴导入（cc-switch / settings.json）----
        ui.text("claude.form.import.label")
            .position(28.0f, 356.0f).size(w - 56.0f, 16.0f)
            .text("── 从 cc-switch / settings.json 粘贴导入 ──")
            .fontSize(11.0f).lineHeight(16.0f)
            .color(t.colors.textTertiary).build();
        components::input(ui, "claude.form.import.text")
            .position(28.0f, 376.0f).size(w - 56.0f, 82.0f)
            .value(importText_)
            .placeholder(R"({ "env": { "ANTHROPIC_BASE_URL": "...", ... } }  粘贴整段 settings.json 或仅 env 对象)")
            .multiline(true).scrollbar(true)
            .fontSize(12.0f).theme(themeTokens)
            .onChange([this](const std::string& v) { importText_ = v; })
            .build();

        components::button(ui, "claude.form.import.parse")
            .position(28.0f, 468.0f).size(110.0f, 32.0f)
            .text("解析导入").fontSize(12.0f)
            .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
            .textColor(t.colors.textPrimary)
            .radius(t.sizes.radiusSmall)
            .border(1.0f, t.colors.borderDefault)
            .onClick([this] { doParseImport(); })
            .build();
        if (importReady_) {
            components::button(ui, "claude.form.import.apply")
                .position(148.0f, 468.0f).size(110.0f, 32.0f)
                .text("应用到表单").fontSize(12.0f)
                .colors(t.colors.primary, t.colors.primaryHover,
                        t.colors.primaryPressed)
                .textColor(t.colors.onPrimary)
                .radius(t.sizes.radiusSmall)
                .onClick([this] { doApplyImport(); })
                .build();
        }

        // 解析结果：失败红字 / 成功预览（槽位/env 识别数 + 别名/未识别提示）
        //（✕/⚠ 换 FA：图标/文字拆两个元素）
        if (!importError_.empty()) {
            ui.text("claude.form.import.error.icon")
                .position(270.0f, 468.0f).size(13.0f, 16.0f)
                .icon(0xF00D).fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.error).build();
            ui.text("claude.form.import.error")
                .position(286.0f, 468.0f).size(w - 286.0f - 28.0f, 48.0f)
                .text(importError_).fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.error).build();
        } else if (importReady_) {
            ui.text("claude.form.import.preview")
                .position(270.0f, 468.0f).size(w - 270.0f - 28.0f, 16.0f)
                .text("识别：槽位 " + std::to_string(importPreview_.recognizedSlots) +
                      "/6 · env " + std::to_string(importPreview_.extraEnv.size()) +
                      " 项 · 别名映射 " +
                      std::to_string(importPreview_.aliasMappedKeys.size()) +
                      " · 未识别 " +
                      std::to_string(importPreview_.unrecognizedKeys.size()))
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textSecondary).build();
            // 警告角标透传（§4.3 规则 6）：别名映射 / 未识别键 / 解析警告，
            // 最多 3 条（弹窗高度约束——再往下会压到底部按钮；全量信息在
            // 保存后的 settings.json 与日志可查）
            std::vector<std::string> notes;
            for (size_t i = 0; i < importPreview_.aliasMappedKeys.size() && i < 2; ++i) {
                notes.push_back("已映射为官方变量: " +
                                importPreview_.aliasMappedKeys[i]);
            }
            for (size_t i = 0;
                 i < importPreview_.unrecognizedKeys.size() && notes.size() < 3;
                 ++i) {
                notes.push_back("未识别键将透传: " +
                                importPreview_.unrecognizedKeys[i]);
            }
            for (size_t i = 0;
                 i < importPreview_.warnings.size() && notes.size() < 3; ++i) {
                notes.push_back(importPreview_.warnings[i]);
            }
            for (size_t i = 0; i < notes.size(); ++i) {
                ui.text("claude.form.import.note.icon." + std::to_string(i))
                    .position(270.0f, 486.0f + static_cast<float>(i) * 15.0f)
                    .size(13.0f, 15.0f)
                    .icon(0xF071).fontSize(10.0f).lineHeight(15.0f)
                    .color(t.colors.warning).build();
                ui.text("claude.form.import.note." + std::to_string(i))
                    .position(286.0f, 486.0f + static_cast<float>(i) * 15.0f)
                    .size(w - 286.0f - 28.0f, 15.0f)
                    .text(notes[i]).fontSize(10.0f).lineHeight(15.0f)
                    .color(t.colors.warning).build();
            }
        } else {
            ui.text("claude.form.import.hint.icon")
                .position(270.0f, 468.0f).size(13.0f, 16.0f)
                .icon(0xF05A).fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
            ui.text("claude.form.import.hint")
                .position(286.0f, 468.0f).size(w - 286.0f - 28.0f, 32.0f)
                .text("BASE_URL/AUTH_TOKEN 归基础页，模型键归模型映射页，"
                      "其余进入上表")
                .fontSize(11.0f).lineHeight(16.0f)
                .color(t.colors.textTertiary).build();
        }
    }

    void drawProfileFormDialog(eui::Ui& ui, const eui::Screen& screen,
                               const core::theme::Tokens& t,
                               const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 760.0f;
        const float h = 600.0f;
        const bool isEdit = (formEditId_ != 0);

        components::dialog(ui, "claude.dialog.form")
            .open(formOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { formOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.form.title")
                    .position(28.0f, 20.0f)
                    .size(w - 56.0f, 26.0f)
                    .text(isEdit ? "编辑 Profile" : "新建 Profile")
                    .fontSize(20.0f)
                    .lineHeight(26.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // tabs（§6.1：基础 / 模型映射 / 高级 env）。
                // TabsBuilder 无 position，套 stack 定位（字号下拉同款包装）
                ui.stack("claude.form.tabs.wrap")
                    .position(28.0f, 52.0f)
                    .size(w - 56.0f, 34.0f)
                    .content([&] {
                        components::tabs(ui, "claude.form.tabs")
                            .size(w - 56.0f, 34.0f)
                            .items({"基础", "模型映射", "高级 env"})
                            .selected(formTab_)
                            .fontSize(13.0f)
                            .theme(themeTokens)
                            .onChange([this](int idx) { formTab_ = idx; })
                            .build();
                    })
                    .build();

                if (formTab_ == 0) {
                    drawFormBasicTab(ui, w, t, themeTokens, isEdit);
                } else if (formTab_ == 1) {
                    drawFormModelsTab(ui, w, t, themeTokens);
                } else {
                    drawFormEnvTab(ui, w, t, themeTokens);
                }

                // 错误信息（基础页之外也要能看到校验错误）
                if (!formError_.empty() && formTab_ != 0) {
                    ui.text("claude.dialog.form.error")
                        .position(28.0f, h - 104.0f)
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
                    .textColor(t.colors.onPrimary)
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
    // 产品审计 2026-09-23 [11]：spec §8.3 第 6 条承诺「支持过滤、搜索」，
    // 此前是 recentCalls(20) 写死的 20 行静态列表。现走 CallLogQuery：
    // 搜索框（path/method 模糊）+ 状态码下拉（2xx/3xx/4xx/5xx/错误）+
    // 分页（每页 kCallsPageSize 条，页内 scrollView 兜住小窗口）。
    std::string reloadCallsPage() {
        // 拉取当前过滤条件下的一页 + 总匹配数；返回错误信息（空串 = 成功）。
        // 失败时清空当页（弹窗不留旧过滤条件的残留行）
        allCalls_.clear();
        callsTotalMatches_ = 0;
        ClaudeService* svc = ClaudeService::global();
        if (!svc) return "服务未就绪（ClaudeService 未注册）";
        ClaudeService::CallLogQuery q;
        q.search = callsSearch_;
        q.statusClass =
            kCallsStatusClasses[std::clamp(callsStatusIdx_, 0, kCallsStatusCount - 1)];
        q.limit = kCallsPageSize;
        q.offset = callsPage_ * kCallsPageSize;
        auto r = svc->queryCalls(q);
        if (r.isFail()) return r.errorMessage();
        allCalls_ = std::move(r).value();
        auto c = svc->countCalls(q);
        if (c.isOk()) callsTotalMatches_ = c.value();
        return {};
    }

    // 过滤条件变化：回第一页重查（搜索框逐字符触发，本地 SQLite 直查可承受）
    void applyCallsFilter() {
        callsPage_ = 0;
        callsScrollOffset_ = 0;
        reloadCallsPage();
    }

    void openCallsDialog() {
        callsPage_ = 0;
        callsScrollOffset_ = 0;
        std::string err = reloadCallsPage();
        if (!err.empty()) {
            showToast("加载失败", err, true);
            return;
        }
        callsDialogOpen_ = true;
    }

    void drawCallsDialog(eui::Ui& ui, const eui::Screen& screen,
                         const core::theme::Tokens& t,
                         const components::theme::ThemeColorTokens& themeTokens) {
        const float w = 720.0f;
        const float h = 560.0f;
        const int rowH = 22;

        components::dialog(ui, "claude.dialog.calls")
            .open(callsDialogOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(themeTokens)
            .onOpenChange([this](bool v) { callsDialogOpen_ = v; })
            .content([&] {
                ui.text("claude.dialog.calls.title")
                    .position(28.0f, 20.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("调用记录")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();

                // ---- 工具行：搜索框 + 状态码过滤 + 匹配总数 ----
                components::input(ui, "claude.dialog.calls.search")
                    .position(28.0f, 58.0f)
                    .size(260.0f, 32.0f)
                    .value(callsSearch_)
                    .placeholder("搜索路径 / 方法…")
                    .fontSize(12.0f)
                    .theme(themeTokens)
                    .onChange([this](const std::string& v) {
                        callsSearch_ = v;
                        applyCallsFilter();
                    })
                    .onEnter([this] {
                        // Enter/Esc 清空（EUI-NEO input 把 Escape 映射到 onEnter，
                        // 与 todo.search 交互一致）
                        if (!callsSearch_.empty()) {
                            callsSearch_.clear();
                            applyCallsFilter();
                        }
                    })
                    .build();

                // dropdown 无 position 接口 → 包定位 stack（仿 settings 下拉）
                ui.stack("claude.dialog.calls.status.ddwrap")
                    .position(298.0f, 58.0f)
                    .size(150.0f, 32.0f)
                    .content([&] {
                        components::dropdown(ui, "claude.dialog.calls.status.dd")
                            .size(150.0f, 32.0f)
                            .theme(themeTokens)
                            .items(std::vector<std::string>(
                                kCallsStatusLabels,
                                kCallsStatusLabels + kCallsStatusCount))
                            .selected(callsStatusIdx_)
                            .open(callsStatusDdOpen_)
                            .onOpenChange([this](bool v) { callsStatusDdOpen_ = v; })
                            .onChange([this](int idx) {
                                callsStatusIdx_ =
                                    std::clamp(idx, 0, kCallsStatusCount - 1);
                                applyCallsFilter();
                            })
                            .build();
                    })
                    .build();

                ui.text("claude.dialog.calls.count")
                    .position(460.0f, 58.0f)
                    .size(w - 460.0f - 28.0f, 32.0f)
                    .text(std::to_string(callsTotalMatches_) + " 条匹配")
                    .fontSize(12.0f)
                    .lineHeight(32.0f)
                    .horizontalAlign(eui::HorizontalAlign::Right)
                    .color(t.colors.textTertiary)
                    .build();

                // ---- 列表区：当页行 + scrollView（440 > 视口时出滚动条） ----
                if (allCalls_.empty()) {
                    ui.text("claude.dialog.calls.empty")
                        .position(28.0f, 112.0f)
                        .size(w - 56.0f, 20.0f)
                        .text("(无匹配的调用记录)")
                        .fontSize(t.sizes.fontSizeSmall)
                        .lineHeight(20.0f)
                        .color(t.colors.textTertiary)
                        .build();
                } else {
                    // contentKey = 当页 id 序列 hash（行替换也失效重测，W7 教训）
                    size_t rowsKey = 0;
                    for (const auto& c : allCalls_) {
                        rowsKey = rowsKey * 1315423911u + static_cast<size_t>(c.id);
                    }
                    const std::time_t now = std::time(nullptr);
                    const float scrollY = 100.0f;
                    const float scrollBottom = h - 60.0f - 12.0f;  // 底部按钮上方
                    components::scrollView(ui, "claude.dialog.calls.scroll")
                        .position(28.0f, scrollY)
                        .size(w - 56.0f, scrollBottom - scrollY)
                        .gap(0.0f)
                        .step(static_cast<float>(rowH))
                        .theme(themeTokens)
                        .contentKey("claude.dialog.calls.scroll." +
                                    std::to_string(rowsKey) + "." +
                                    std::to_string(allCalls_.size()))
                        .offset(callsScrollOffset_)
                        .onChange([this](float v) { callsScrollOffset_ = v; })
                        .content([&](eui::Ui& inner, float contentWidth,
                                     float viewportHeight) {
                            (void)viewportHeight;
                            for (size_t i = 0; i < allCalls_.size(); ++i) {
                                const ClaudeService::CallLogEntry& c = allCalls_[i];
                                const std::string rowId =
                                    "claude.dialog.calls.row." + std::to_string(i);
                                // 行包 stack（position 只在 stack 内生效，仿 todo）
                                inner.stack(rowId + ".wrap")
                                    .size(contentWidth, static_cast<float>(rowH))
                                    .content([&] {
                                        std::string line =
                                            formatCallTime(c.startedAt, now) + "  " +
                                            c.method + " " + c.path + "  " +
                                            (c.statusCode > 0
                                                 ? std::to_string(c.statusCode)
                                                 : std::string("-")) +
                                            "  " + formatLatency(c.latencyMs);
                                        // 无状态码行（超时/网络错误）行尾附错误摘要，
                                        // 截 48 字节防挤爆行宽
                                        if (c.statusCode <= 0 && !c.error.empty()) {
                                            line += "  " + truncateUtf8(c.error, 48);
                                        }
                                        const eui::Color rowColor =
                                            (c.statusCode >= 200 && c.statusCode < 300)
                                                ? t.colors.textSecondary
                                                : t.colors.error;
                                        inner.text(rowId)
                                            .position(0.0f, 2.0f)
                                            .size(contentWidth, 18.0f)
                                            .text(line)
                                            .fontSize(12.0f)
                                            .lineHeight(18.0f)
                                            .color(rowColor)
                                            .build();
                                    })
                                    .build();
                            }
                        })
                        .build();
                }

                // ---- 底部：翻页 + 页码 + 关闭 ----
                const int pages = std::max(
                    1, (callsTotalMatches_ + kCallsPageSize - 1) / kCallsPageSize);
                const bool hasNext =
                    (callsPage_ + 1) * kCallsPageSize < callsTotalMatches_;
                components::button(ui, "claude.dialog.calls.prev")
                    .position(28.0f, h - 60.0f)
                    .size(88.0f, 40.0f)
                    .text("← 上一页")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .disabled(callsPage_ == 0)
                    .onClick([this] {
                        if (callsPage_ == 0) return;
                        --callsPage_;
                        callsScrollOffset_ = 0;
                        reloadCallsPage();
                    })
                    .build();
                components::button(ui, "claude.dialog.calls.next")
                    .position(128.0f, h - 60.0f)
                    .size(88.0f, 40.0f)
                    .text("下一页 →")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .disabled(!hasNext)
                    .onClick([this] {
                        if ((callsPage_ + 1) * kCallsPageSize >= callsTotalMatches_) return;
                        ++callsPage_;
                        callsScrollOffset_ = 0;
                        reloadCallsPage();
                    })
                    .build();
                ui.text("claude.dialog.calls.page")
                    .position(228.0f, h - 60.0f)
                    .size(332.0f, 40.0f)
                    .text("第 " + std::to_string(callsPage_ + 1) + " / " +
                          std::to_string(pages) + " 页")
                    .fontSize(12.0f)
                    .lineHeight(40.0f)
                    .color(t.colors.textSecondary)
                    .build();
                components::button(ui, "claude.dialog.calls.close")
                    .position(w - 28.0f - 120.0f, h - 60.0f)
                    .size(120.0f, 40.0f)
                    .text("关闭")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover, t.colors.active)
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
            // 图标走 toast 自带 Icon 字体通道（⚠/✓ 前缀混排正文不可行）
            .icon(toastIsError_ ? 0xF071 : 0xF058)  // fa-triangle-exclamation / fa-circle-check
            .title(toastTitle_)
            .message(toastMessage_)
            .onAutoDismiss([this] { toastOpen_ = false; })
            .onDismiss([this] { toastOpen_ = false; })
            .build();
    }

    void showToast(const std::string& title, const std::string& msg, bool isError) {
        toastTitle_ = title;
        toastMessage_ = msg;
        toastIsError_ = isError;
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
        // 产品审计 2026-09-23 [1]（§7 安全第 5 条）：删除激活 Profile 且代理
        // 运行中 → service 层已停代理并还原 settings.json，proxyStopped 出参
        // 告知这里，toast 明示（否则用户只见「已删除」，不知道 Claude Code
        // 接入配置已被还原）
        bool proxyStopped = false;
        auto r = svc->deleteProfile(id, &proxyStopped);
        if (r.isFail()) {
            showToast("删除失败", r.errorMessage(), true);
            return;
        }
        if (selectedId_ == id) selectedId_ = 0;
        if (proxyStopped) {
            showToast("已删除（代理已停止）",
                      "删除的是激活 Profile，已停止代理并还原 Claude Code 配置",
                      false);
        } else {
            showToast("已删除", "Profile 已删除（含 Keychain 条目）", false);
        }
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

    // 新建/编辑表单（tabs 三页，§6.1）
    bool formOpen_ = false;
    int64_t formEditId_ = 0;   // 0 = 新建，非 0 = 编辑该 id
    int formTab_ = 0;          // 0 基础 / 1 模型映射 / 2 高级 env
    std::string formName_;
    std::string formUrl_;
    std::string formKey_;
    std::string formKeyHint_;  // 编辑时显示现有 key 脱敏提示
    std::string formError_;
    std::string formImportWarn_;  // 导入当前配置的接管态警告（弹窗内展示，不阻断）
    ModelsConfig formModels_;  // 6 槽位（默认模型 = def 槽位，旧 formModel_ 并入）
    std::vector<std::pair<std::string, std::string>> formEnvRows_;  // env 键值行

    // cc-switch 粘贴导入（高级 env 页）
    std::string importText_;       // 多行粘贴框原文
    bool importReady_ = false;     // 解析成功待应用
    std::string importError_;      // 解析失败信息（红字）
    ImportedConfig importPreview_; // 解析结果预览

    // 引导条一次性标记（§6.4：会话内代理从未启动过才显示；内存态不持久化）
    bool proxyEverSeen_ = false;

    // 删除确认
    bool deleteConfirmOpen_ = false;
    int64_t deleteTargetId_ = 0;
    std::string deleteTargetName_;

    // 完整调用记录弹窗（审计 [11]：搜索/状态码过滤/翻页共用以下查询态）
    bool callsDialogOpen_ = false;
    std::vector<ClaudeService::CallLogEntry> allCalls_;
    std::string callsSearch_;        // 搜索框文本（path/method 模糊）
    int callsStatusIdx_ = 0;         // 状态下拉索引（→ kCallsStatusClasses）
    bool callsStatusDdOpen_ = false; // 状态下拉展开态
    int callsPage_ = 0;              // 当前页（0-based）
    int callsTotalMatches_ = 0;      // 满足过滤的总条数（countCalls）
    float callsScrollOffset_ = 0.0f; // 列表区滚动位置（翻页时归零）

    // Profile 列表滚动位置（审计 [26]：卡片超界可滚，不再压底部按钮）
    float profilesScrollOffset_ = 0.0f;

    // Toast
    bool toastOpen_ = false;
    std::string toastTitle_;
    std::string toastMessage_;
    bool toastIsError_ = false;

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
