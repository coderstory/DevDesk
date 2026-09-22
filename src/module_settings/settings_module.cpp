// src/module_settings/settings_module.cpp
// P13 设置页模块（spec §8.5 / 任务 T11）
//
// 设置项：
//   1. 主题切换（深色/浅色）——core::theme::setDark/setLight，compose 每帧读
//      current()，切换后下一帧全页生效（日志自证：applyTheme 有 LOG_INFO）；
//      窗口底色（clearColor）在启动时跟随持久化主题，运行中切换重启后生效
//      （EUI-NEO 无运行时改 clearColor 的 API，见 main.cpp 注释）
//   2. 字号缩放——core::theme::setFontScale 全局生效：token 层缩放让所有
//      经 t.sizes.fontSize* 取字号的模块零改动受益；本页非 token 的硬编码
//      小字号经 fs() 辅助函数跟随同一缩放
//   3. Claude 代理：随应用启动开关（Phase B 硬性需求 1，默认关）+ token 只读
//      显示 + 重新生成（确认弹窗后 stopProxy + startProxy 同端口重启 → 服务端
//      生成新 token；代理未运行时按钮置灰，不允许顺带拉起代理）
//      + Claude Code 接管状态（Phase D UI 接入，数据源 ClaudeService）
//   4. Markdown 笔记目录只读展示（改目录属于范围外，仅展示）
//   5. 调用日志保留天数（0=永久）——切换档位立即执行一次滚动清理并
//      反馈「已清理 N 条」（core::pruneCallLogs，经 appDatabase() 共享
//      连接；启动时 main.cpp 仍会按持久化配置再清一次，幂等）
//
// 入口说明：设置入口在 main.cpp 的常驻侧边栏（app.sidebar，spec §2/§6.5.2），
// 与其他四个模块同列。
//
// 依赖说明：module_* 之间原则上不互相依赖（防循环）；本模块单向依赖
// module_claude_proxy，仅为读取 main.cpp 注册的 ClaudeService::global() 单例
// （既有跨模块桥接模式，claude_module.cpp 同款用法），无环。
#include "settings_config.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dialog.h>
#include <components/dropdown.h>
#include <components/switch.h>
#include <components/theme.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace module::settings {

namespace {

// 字号缩放档位（%）
constexpr int kFontPresets[] = {85, 100, 115, 130};
constexpr int kFontPresetCount = 4;
const char* kFontPresetLabels[] = {"小 85%", "标准 100%", "大 115%", "特大 130%"};

// 日志保留档位（天；0 = 永久）
constexpr int kRetentionPresets[] = {0, 7, 30, 90, 180, 365};
constexpr int kRetentionPresetCount = 6;

int nearestIndex(int value, const int* presets, int count) {
    int best = 0;
    int bestDist = 1 << 30;
    for (int i = 0; i < count; ++i) {
        const int d = std::abs(presets[i] - value);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// 代理凭据文件写入已收敛到 module_claude_proxy 的
// module::claude::writeProxyTokenFile（claude_service.h），本模块直接复用
//（此前这里与 main.cpp 各有一份复制粘贴实现）

} // namespace

class SettingsModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Settings; }
    std::string displayName() const override { return "设置"; }
    std::string iconGlyph() const override { return eui::utf8(0xF013); }  // fa-gear

    // ============ 生命周期 ============

    void onStart() override {
        store_ = std::make_unique<SettingsStore>(::core::paths::configFile());
        cfg_ = store_->load();
        // 启动即应用持久化主题 + 字号缩放（main.cpp 的 dslAppConfig 也做过
        // 同样恢复——这里幂等重放，保证模块独立使用时行为一致）
        if (cfg_.darkTheme) {
            ::core::theme::setDark();
        } else {
            ::core::theme::setLight();
        }
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        LOG_INFO("Settings 模块启动（主题=" + ::core::theme::currentThemeName() +
                 ", 字号=" + std::to_string(cfg_.fontScalePercent) + "%" +
                 ", 日志保留=" + retentionLabel() + "）");
    }

    void onStop() override {
        store_.reset();
        LOG_INFO("Settings 模块停止");
    }

    // ============ 设置变更 ============

    void applyTheme(bool dark) {
        if (dark) {
            ::core::theme::setDark();
        } else {
            ::core::theme::setLight();
        }
        cfg_.darkTheme = dark;
        persist();
        setStatus("主题已切换：" + ::core::theme::currentThemeName());
        LOG_INFO("Settings: 主题切换为 " + ::core::theme::currentThemeName() +
                 "（下一帧全页生效）");
    }

    void applyFontPreset(int index) {
        if (index < 0 || index >= kFontPresetCount) return;
        cfg_.fontScalePercent = kFontPresets[index];
        // 写入 core::theme 全局缩放：token 读取层生效，所有模块零改动受益
        ::core::theme::setFontScale(
            static_cast<float>(cfg_.fontScalePercent) / 100.0f);
        persist();
        setStatus("字号缩放：" + std::to_string(cfg_.fontScalePercent) +
                  "%（下一帧全局生效）");
        LOG_INFO("Settings: 字号缩放 → " + std::to_string(cfg_.fontScalePercent) +
                 "%（core::theme token 层全局缩放）");
    }

    void applyRetentionPreset(int index) {
        if (index < 0 || index >= kRetentionPresetCount) return;
        cfg_.logRetentionDays = kRetentionPresets[index];
        persist();
        // 0 = 永久：跳过清理；否则切档立即执行一次滚动清理并反馈条数
        //（此前只在启动时读档清理一次，改档要重启才生效）
        if (cfg_.logRetentionDays == 0) {
            setStatus("日志保留：永久保留（不清理历史日志）");
            LOG_INFO("Settings: 日志保留 → 永久保留（跳过清理）");
            return;
        }
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            setStatus("日志保留：" + retentionLabel() +
                      "（数据库未就绪，重启后生效）");
            LOG_WARN("Settings: appDatabase 未注册，跳过立即清理");
            return;
        }
        auto pruned = ::core::pruneCallLogs(*db, cfg_.logRetentionDays);
        if (pruned.isOk()) {
            setStatus("日志保留：" + retentionLabel() + "，已清理 " +
                      std::to_string(pruned.unsafeValue()) + " 条历史日志");
            LOG_INFO("Settings: 日志保留 → " + retentionLabel() + "，已清理 " +
                     std::to_string(pruned.unsafeValue()) + " 条");
        } else {
            setStatus("日志保留：" + retentionLabel() +
                      "（清理失败：" + pruned.errorMessage() + "）", true);
            LOG_ERROR("Settings: 立即清理失败: " + pruned.errorMessage());
        }
    }

    // 请求重新生成 token：代理未运行 → 不可生成（token 随 startProxy 生成，
    // 没有「预生成」概念，也不允许顺带把代理拉起来——Phase B 修的怪行为）；
    // 运行中 → 先弹确认（一键作废在用 token）
    void requestRegenerateToken() {
        auto* svc = ::module::claude::ClaudeService::global();
        if (!svc) {
            setStatus("Claude 服务不可用", true);
            return;
        }
        if (!svc->isProxyRunning()) {
            setStatus("代理未运行，无法重新生成（Token 随代理启动时生成）", true);
            LOG_INFO("Settings: 重新生成被拒——代理未运行（不拉起代理）");
            return;
        }
        regenConfirmOpen_ = true;
    }

    // 确认弹窗「确认重新生成」回调：stop + start 同端口重启（服务端生成
    // 新 token），并同步重写 proxy_token.txt
    void doRegenerateToken() {
        auto* svc = ::module::claude::ClaudeService::global();
        if (!svc) {
            setStatus("Claude 服务不可用", true);
            return;
        }
        if (!svc->isProxyRunning()) {  // 双保险：弹窗期间代理可能已被停止
            setStatus("代理未运行，无法重新生成", true);
            return;
        }
        const int port = svc->proxyPort();
        svc->stopProxy();
        // startProxy 不传 token → 服务端生成新 token（claude_service 现有 API）
        auto r = svc->startProxy("127.0.0.1", port);
        if (r.isFail()) {
            setStatus("重新生成失败：" + r.errorMessage(), true);
            LOG_ERROR("Settings: 代理 token 重新生成失败: " + r.errorMessage());
            return;
        }
        ::module::claude::writeProxyTokenFile(svc->proxyAuthToken(), port);
        setStatus("已重新生成代理 Token（端口 " + std::to_string(port) + "）");
        LOG_INFO("Settings: 代理 token 已重新生成 port=" + std::to_string(port));
    }

    // 「随应用启动代理」开关（provider-config Phase B 硬性需求 1）
    void applyAutoStart(bool on) {
        cfg_.claudeProxyAutoStart = on;
        persist();
        setStatus(on ? "代理将随应用启动（下次启动生效）"
                     : "代理不再随应用启动（下次启动生效）");
        LOG_INFO("Settings: claudeProxyAutoStart → " +
                 std::string(on ? "true" : "false"));
    }

    void persist() {
        if (!store_) return;
        auto r = store_->save(cfg_);
        if (r.isFail()) {
            setStatus("配置保存失败：" + r.errorMessage(), true);
            LOG_ERROR("Settings: 配置保存失败: " + r.errorMessage());
        }
    }

    void setStatus(const std::string& msg, bool isError = false) {
        statusMessage_ = msg;
        statusIsError_ = isError;
    }

    // ============ UI ============

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;  // 40
        const float contentW = screen.width - pad * 2.0f;

        // 页面背景自绘（main.cpp clearColor 固定浅色，深色主题下页面必须自己铺底，
        // 否则浅色 clear 底 + 深色 token 文字不可读——其他模块不铺底是既有现状）
        ui.rect("settings.pagebg")
            .size(screen.width, screen.height)
            .color(t.colors.background)
            .build();

        const float headerH = 64.0f;
        ui.column("settings.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(t.sizes.spacingLarge)
            .content([&] {
                drawHeader(ui, contentW, headerH, t);
                ui.column("settings.cards")
                    .size(contentW, screen.height - pad * 2.0f - headerH -
                                        t.sizes.spacingLarge)
                    .gap(t.sizes.spacingLarge)
                    .content([&] {
                        // 卡片纵向游标（与 column 布局的 padding/gap 逐项对应），
                        // 用于计算日志卡下拉字段的绝对 Y —— 判断弹层向下展开
                        // 是否会超出窗口底部（超出则改向上展开，EUI-NEO openUp）
                        const float gapY = t.sizes.spacingLarge;
                        float cardY = pad + headerH + gapY;
                        drawAppearanceCard(ui, "settings.card.appearance",
                                           contentW, 160.0f, t);
                        cardY += 160.0f + gapY;
                        drawDirectoryCard(ui, "settings.card.directory",
                                          contentW, 108.0f, t);
                        cardY += 108.0f + gapY;
                        // 代理卡高度动态：接管状态行 + 上层覆盖警告行（§6.3）
                        {
                            auto* svc =
                                ::module::claude::ClaudeService::global();
                            const bool managed =
                                svc && svc->isClaudeCodeManaged();
                            size_t warnLines = 0;
                            if (managed && svc) {
                                warnLines = std::min<size_t>(
                                    svc->lastSyncWarnings().size(), 2);
                            }
                            const float proxyH = 140.0f + 28.0f +
                                (managed ? 20.0f : 0.0f) +
                                static_cast<float>(warnLines) * 16.0f;
                            drawProxyCard(ui, "settings.card.proxy", contentW,
                                          proxyH, t);
                            cardY += proxyH + gapY;
                        }
                        drawLogCard(ui, "settings.card.log", contentW, 100.0f,
                                    t, cardY, screen.height);
                    })
                    .build();
            })
            .build();

        // 重新生成 Token 确认弹窗（画在最上层；仿 claude_module 删除确认弹窗）
        drawRegenConfirmDialog(ui, screen, t);

        LOG_DEBUG("Settings 模块 compose 完成");
    }

private:
    // 非 token 的硬编码字号跟随全局缩放（单一来源 core::theme::fontScale；
    // token 字号已在 theme 层缩放，不要再经 fs() 包一层，避免双重缩放）
    float fs(float base) const {
        return base * ::core::theme::fontScale();
    }

    std::string retentionLabel() const {
        return cfg_.logRetentionDays == 0
                   ? "永久保留"
                   : std::to_string(cfg_.logRetentionDays) + " 天";
    }

    // ---------- 顶部 Header ----------
    void drawHeader(eui::Ui& ui, float contentW, float headerH,
                    const core::theme::Tokens& t) {
        ui.row("settings.header")
            .size(contentW, headerH)
            .alignItems(eui::Align::CENTER)
            .content([&] {
                ui.column("settings.header.text")
                    .size(contentW, headerH)
                    .gap(2.0f)
                    .content([&] {
                        ui.text("settings.h1")
                            .size(contentW, 34.0f)
                            .text("设置")
                            .fontSize(t.sizes.fontSizePage)
                            .lineHeight(34.0f)
                            .fontWeight(700)
                            .color(t.colors.textPrimary)
                            .build();
                        const std::string sub = statusMessage_.empty()
                            ? "所有变更即时保存到 config.json · 左侧侧边栏切换页面"
                            : statusMessage_;
                        ui.text("settings.subtitle")
                            .size(contentW, 20.0f)
                            .text(sub)
                            .fontSize(t.sizes.fontSizeSmall)
                            .lineHeight(20.0f)
                            .color(statusIsError_ ? t.colors.error
                                                  : t.colors.textSecondary)
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 卡片公共背景 ----------
    void cardFrame(eui::Ui& ui, const std::string& id, float w, float h,
                   const core::theme::Tokens& t) {
        ui.rect(id + ".bg")
            .position(0.0f, 0.0f)
            .size(w, h)
            .color(t.colors.surface)
            .radius(t.sizes.radiusLarge)
            .border(1.0f, t.colors.borderDefault)
            .build();
    }

    // ---------- 卡片 1：外观（主题 + 字号） ----------
    // zIndex(30)：字号下拉的弹出层要盖过下方卡片（zIndex 只在兄弟间排序，
    // 弹层在卡片子树内，需整卡抬层）
    void drawAppearanceCard(eui::Ui& ui, const std::string& id, float w,
                            float h, const core::theme::Tokens& t) {
        const auto compTheme = t.isDark ? components::theme::dark()
                                        : components::theme::light();
        ui.stack(id)
            .size(w, h)
            .zIndex(30)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 18.0f)
                    .size(w - 56.0f, 24.0f)
                    .text("外观")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(24.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".desc")
                    .position(28.0f, 44.0f)
                    .size(w - 56.0f, 18.0f)
                    .text("主题即时生效（窗口底色重启后跟随）；字号缩放全局生效")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();

                // 左：主题切换
                ui.text(id + ".theme.label")
                    .position(28.0f, 72.0f)
                    .size(200.0f, 16.0f)
                    .text("主题")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textSecondary)
                    .build();
                const bool dark = cfg_.darkTheme;
                components::button(ui, id + ".theme.dark")
                    .position(28.0f, 94.0f)
                    .size(130.0f, 40.0f)
                    .text("🌙 深色")
                    .fontSize(fs(14.0f))
                    .colors(dark ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                            t.colors.hover, t.colors.active)
                    .textColor(dark ? t.colors.primary : t.colors.textSecondary)
                    .radius(t.sizes.radius)
                    .border(1.0f, dark ? t.colors.primary : t.colors.borderDefault)
                    .onClick([this] { applyTheme(true); })
                    .build();
                components::button(ui, id + ".theme.light")
                    .position(170.0f, 94.0f)
                    .size(130.0f, 40.0f)
                    .text("☀ 浅色")
                    .fontSize(fs(14.0f))
                    .colors(!dark ? t.colors.primarySoftBg : t.colors.surfaceElevated,
                            t.colors.hover, t.colors.active)
                    .textColor(!dark ? t.colors.primary : t.colors.textSecondary)
                    .radius(t.sizes.radius)
                    .border(1.0f, !dark ? t.colors.primary : t.colors.borderDefault)
                    .onClick([this] { applyTheme(false); })
                    .build();

                // 右：字号缩放
                ui.text(id + ".font.label")
                    .position(380.0f, 72.0f)
                    .size(240.0f, 16.0f)
                    .text("字号缩放（正文基准 15px）")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textSecondary)
                    .build();
                ui.stack(id + ".font.ddwrap")
                    .position(380.0f, 94.0f)
                    .size(240.0f, 40.0f)
                    .content([&] {
                        components::dropdown(ui, id + ".font.dd")
                            .size(240.0f, 40.0f)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kFontPresetLabels,
                                kFontPresetLabels + kFontPresetCount))
                            .selected(nearestIndex(cfg_.fontScalePercent,
                                                   kFontPresets, kFontPresetCount))
                            .open(fontDropdownOpen_)
                            .onOpenChange([this](bool v) { fontDropdownOpen_ = v; })
                            .onChange([this](int idx) { applyFontPreset(idx); })
                            .build();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 卡片 2：Markdown 笔记目录（只读） ----------
    void drawDirectoryCard(eui::Ui& ui, const std::string& id, float w,
                           float h, const core::theme::Tokens& t) {
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("Markdown 笔记目录（只读）")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                const std::filesystem::path mdDir =
                    ::core::paths::markdownDir();
                std::error_code ec;
                const bool exists =
                    std::filesystem::exists(mdDir, ec) && !ec;
                ui.rect(id + ".path.bg")
                    .position(28.0f, 46.0f)
                    .size(w - 56.0f, 34.0f)
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderMuted)
                    .build();
                ui.text(id + ".path.value")
                    .position(40.0f, 46.0f)
                    .size(w - 80.0f, 34.0f)
                    .text(mdDir.string() + (exists ? "" : "（尚未创建）"))
                    .fontSize(fs(13.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(t.colors.textSecondary)
                    .build();
                ui.text(id + ".hint")
                    .position(28.0f, 86.0f)
                    .size(w - 56.0f, 14.0f)
                    .text("修改笔记目录属于后续功能，当前仅展示")
                    .fontSize(fs(11.0f))
                    .lineHeight(14.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 卡片 3：Claude 代理 token ----------
    void drawProxyCard(eui::Ui& ui, const std::string& id, float w, float h,
                       const core::theme::Tokens& t) {
        ui.stack(id)
            .size(w, h)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                auto* svc = ::module::claude::ClaudeService::global();
                const bool running = svc && svc->isProxyRunning();
                const int port = running ? svc->proxyPort() : 0;
                const std::string tokenText = running
                    ? svc->proxyAuthToken()
                    : std::string("（代理未运行）");

                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(240.0f, 22.0f)
                    .text("Claude 代理")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                // 随应用启动代理（Phase B 硬性需求 1，默认关）——放卡片右上，
                // 不新增卡片高度（SwitchBuilder 无 position，套 stack 定位，
                // 与字号下拉同款包装）
                const auto compTheme = t.isDark ? components::theme::dark()
                                                : components::theme::light();
                ui.stack(id + ".autostart.wrap")
                    .position(w - 28.0f - 240.0f, 12.0f)
                    .size(240.0f, 28.0f)
                    .content([&] {
                        components::toggleSwitch(ui, id + ".autostart")
                            .size(240.0f, 28.0f)
                            .text("随应用启动代理")
                            .fontSize(fs(13.0f))
                            .checked(cfg_.claudeProxyAutoStart)
                            .theme(compTheme)
                            .onChange([this](bool v) { applyAutoStart(v); })
                            .build();
                    })
                    .build();

                ui.text(id + ".desc")
                    .position(28.0f, 40.0f)
                    .size(w - 56.0f, 16.0f)
                    .text(running
                          ? "本地代理 http://127.0.0.1:" + std::to_string(port) +
                                " · 客户端调用需携带 Authorization: Bearer <token>"
                          : "本地代理未运行（默认不随应用启动，可在 Claude 页手动启动）")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textTertiary)
                    .build();

                ui.text(id + ".token.label")
                    .position(28.0f, 64.0f)
                    .size(240.0f, 16.0f)
                    .text("认证 Token（只读）")
                    .fontSize(fs(12.0f))
                    .lineHeight(16.0f)
                    .color(t.colors.textSecondary)
                    .build();
                const float btnW = 140.0f;
                ui.rect(id + ".token.bg")
                    .position(28.0f, 86.0f)
                    .size(w - 56.0f - btnW - 12.0f, 40.0f)
                    .color(t.colors.surfaceElevated)
                    .radius(t.sizes.radiusSmall)
                    .border(1.0f, t.colors.borderMuted)
                    .build();
                ui.text(id + ".token.value")
                    .position(40.0f, 86.0f)
                    .size(w - 80.0f - btnW - 12.0f, 40.0f)
                    .text(tokenText)
                    .fontSize(fs(13.0f))
                    .verticalAlign(eui::VerticalAlign::Center)
                    .color(running ? t.colors.textSecondary : t.colors.textTertiary)
                    .build();

                // 未运行时置灰（token 随 startProxy 生成，无「预生成」概念；
                // 且绝不允许此按钮顺带拉起代理——Phase B）
                components::button(ui, id + ".token.regen")
                    .position(w - 28.0f - btnW, 86.0f)
                    .size(btnW, 40.0f)
                    .text("↻ 重新生成")
                    .fontSize(fs(14.0f))
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .disabled(!running)
                    .onClick([this] { requestRegenerateToken(); })
                    .build();

                // Claude Code 接管状态（Phase D，§6.3）：sidecar 是否存在推导，
                // 让还原机制对用户可见；上层覆盖警告（§5.3 检测命中时）逐行展示
                const bool managed = svc && svc->isClaudeCodeManaged();
                ui.text(id + ".takeover.label")
                    .position(28.0f, 134.0f)
                    .size(150.0f, 18.0f)
                    .text("Claude Code 接管")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textSecondary)
                    .build();
                const std::string ccPath =
                    ::module::claude::settings_sync::defaultPaths()
                        .settingsFile.string();
                ui.text(id + ".takeover.value")
                    .position(190.0f, 134.0f)
                    .size(w - 190.0f - 28.0f, 18.0f)
                    .text(managed
                          ? ("● 已接管 " + ccPath)
                          : std::string("○ 未接管（启动代理后自动接管，停止时还原）"))
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(managed ? t.colors.success : t.colors.textTertiary)
                    .build();
                if (managed && svc) {
                    const auto& warns = svc->lastSyncWarnings();
                    // ponytail: 最多展示 2 条（卡片高度约束），更多警告在
                    // 日志（LOG_WARN 全量）可查
                    const size_t shown =
                        std::min<size_t>(warns.size(), 2);
                    for (size_t i = 0; i < shown; ++i) {
                        ui.text(id + ".takeover.warn." + std::to_string(i))
                            .position(28.0f, 154.0f + static_cast<float>(i) * 16.0f)
                            .size(w - 56.0f, 16.0f)
                            .text("⚠ " + warns[i])
                            .fontSize(fs(11.0f))
                            .lineHeight(16.0f)
                            .color(t.colors.warning)
                            .build();
                    }
                }
            })
            .build();
    }

    // ---------- 卡片 4：日志保留策略 ----------
    // cardTopY/screenH：日志卡是页面最后一张卡，字段贴近窗口底部；
    // 估算弹层高度（与 dropdown 内部一致：菜单项 34 × 6 项 + 上下 padding 6×2）
    // 判断向下展开是否出界，出界则 openUp 向上展开（修复菜单被窗体裁剪）
    void drawLogCard(eui::Ui& ui, const std::string& id, float w, float h,
                     const core::theme::Tokens& t, float cardTopY,
                     float screenH) {
        const auto compTheme = t.isDark ? components::theme::dark()
                                        : components::theme::light();
        ui.stack(id)
            .size(w, h)
            .zIndex(30)
            .content([&] {
                cardFrame(ui, id, w, h, t);
                ui.text(id + ".title")
                    .position(28.0f, 16.0f)
                    .size(w - 56.0f, 22.0f)
                    .text("调用日志保留")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();

                // 档位说明（0 = 永久）；存的是原始天数 int，T10 清理任务按值读取
                static const char* kRetentionLabels[] = {
                    "永久保留", "7 天", "30 天", "90 天", "180 天", "365 天"};
                ui.stack(id + ".ddwrap")
                    .position(28.0f, 48.0f)
                    .size(240.0f, 40.0f)
                    .content([&] {
                        // 字段绝对底边 = 卡顶 + 卡内 y 偏移(48) + 字段高(40)
                        const float fieldBottomY = cardTopY + 48.0f + 40.0f;
                        const float popupGap = 8.0f;   // metrics spacing.compact
                        const float popupH =
                            static_cast<float>(kRetentionPresetCount) *
                                34.0f + 12.0f;  // menuItem × 6 + small×2
                        const bool openUp =
                            fieldBottomY + popupGap + popupH > screenH - 16.0f;
                        components::dropdown(ui, id + ".dd")
                            .size(240.0f, 40.0f)
                            .theme(compTheme)
                            .items(std::vector<std::string>(
                                kRetentionLabels,
                                kRetentionLabels + kRetentionPresetCount))
                            .selected(nearestIndex(cfg_.logRetentionDays,
                                                   kRetentionPresets,
                                                   kRetentionPresetCount))
                            .open(retentionDropdownOpen_)
                            .openUp(openUp)
                            .onOpenChange(
                                [this](bool v) { retentionDropdownOpen_ = v; })
                            .onChange(
                                [this](int idx) { applyRetentionPreset(idx); })
                            .build();
                    })
                    .build();
                ui.text(id + ".hint")
                    .position(288.0f, 60.0f)
                    .size(w - 316.0f, 18.0f)
                    .text("0 = 永久保留；30 天为默认。切换档位立即清理一次。")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 重新生成 Token 确认弹窗 ----------
    // 一键作废在用 token 是破坏性操作，先确认再执行（provider-config Phase B；
    // W9 批次「删除确认同款」需求并入此处）
    void drawRegenConfirmDialog(eui::Ui& ui, const eui::Screen& screen,
                                const core::theme::Tokens& t) {
        const float w = 460.0f;
        const float h = 210.0f;
        const auto compTheme = t.isDark ? components::theme::dark()
                                        : components::theme::light();
        components::dialog(ui, "settings.dialog.regen")
            .open(regenConfirmOpen_)
            .screen(screen.width, screen.height)
            .size(w, h)
            .theme(compTheme)
            .onOpenChange([this](bool v) { regenConfirmOpen_ = v; })
            .content([&] {
                ui.text("settings.dialog.regen.title")
                    .position(28.0f, 24.0f)
                    .size(w - 56.0f, 28.0f)
                    .text("重新生成代理 Token？")
                    .fontSize(20.0f)
                    .lineHeight(28.0f)
                    .fontWeight(700)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text("settings.dialog.regen.msg")
                    .position(28.0f, 58.0f)
                    .size(w - 56.0f, 60.0f)
                    .text("将立即作废当前正在使用的 Token（同端口重启代理生成新 "
                          "Token）。正在调用代理的客户端需要更新凭据后才能继续。")
                    .fontSize(t.sizes.fontSizeSmall)
                    .lineHeight(20.0f)
                    .color(t.colors.textSecondary)
                    .build();

                components::button(ui, "settings.dialog.regen.cancel")
                    .position(28.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("取消")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover,
                            t.colors.active)
                    .textColor(t.colors.textPrimary)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] { regenConfirmOpen_ = false; })
                    .build();
                components::button(ui, "settings.dialog.regen.confirm")
                    .position(28.0f + (w - 68.0f) * 0.5f + 12.0f, h - 60.0f)
                    .size((w - 68.0f) * 0.5f, 40.0f)
                    .text("确认重新生成")
                    .fontSize(14.0f)
                    .colors(t.colors.surfaceElevated, t.colors.hover,
                            t.colors.active)
                    .textColor(t.colors.error)
                    .radius(t.sizes.radius)
                    .border(1.0f, t.colors.borderDefault)
                    .onClick([this] {
                        regenConfirmOpen_ = false;
                        doRegenerateToken();
                    })
                    .build();
            })
            .build();
    }

    // ---------- 状态 ----------
    std::unique_ptr<SettingsStore> store_;
    SettingsConfig cfg_;
    bool fontDropdownOpen_ = false;
    bool retentionDropdownOpen_ = false;
    bool regenConfirmOpen_ = false;       // 重新生成 token 确认弹窗
    std::string statusMessage_;
    bool statusIsError_ = false;
};

} // namespace module::settings

extern "C" ::core::Module* create_settings_module() {
    return new module::settings::SettingsModule();
}
