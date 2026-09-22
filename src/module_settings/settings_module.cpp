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
//   3. Claude 代理 token 只读显示 + 重新生成（stopProxy + startProxy 同端口重启
//      → 服务端生成新 token；同步重写 proxy_token.txt）
//   4. Markdown 笔记目录只读展示（改目录属于范围外，仅展示）
//   5. 调用日志保留天数（0=永久）——仅配置存取；
//      集成点：与 w3-t10-home 并行分支的日志清理任务联动，T10 清理逻辑读取
//      config.json 的 logRetentionDays 字段，接线留给集成阶段
//
// 入口说明：设置入口在 main.cpp 的常驻侧边栏（app.sidebar，spec §2/§6.5.2），
// 与其他四个模块同列。
//
// 依赖说明：module_* 之间原则上不互相依赖（防循环）；本模块单向依赖
// module_claude_proxy，仅为读取 main.cpp 注册的 ClaudeService::global() 单例
// （既有跨模块桥接模式，claude_module.cpp 同款用法），无环。
#include "settings_config.h"

#include "claude_service.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/theme.h"

#include <eui/dsl.h>

#include <components/button.h>
#include <components/dropdown.h>
#include <components/theme.h>

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

// 与 main.cpp 的 writeProxyTokenFile 保持一致（那边在匿名命名空间，无法复用；
// 重新生成 token 后必须同步重写，否则用户按文件里的旧 token 调用会 401）
void writeProxyTokenFile(const std::string& token, int port) {
    const auto path = core::paths::appDataDir() / "proxy_token.txt";
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        LOG_WARN("Settings: 无法写入 proxy_token.txt: " + path.string());
        return;
    }
    f << "# DevDesk Claude 代理凭据（自动生成，请勿提交到版本控制）\n"
      << "port=" << port << "\n"
      << "token=" << token << "\n"
      << "\n"
      << "# 用法：\n"
      << "# curl -X POST http://127.0.0.1:" << port << "/v1/messages \\\n"
      << "#   -H \"Authorization: Bearer " << token << "\" \\\n"
      << "#   -H \"Content-Type: application/json\" \\\n"
      << "#   -d '{\"model\":\"claude-sonnet-4-5\",\"max_tokens\":64,"
         "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}'\n";
    f.close();
    LOG_INFO("Settings: 代理凭据已更新: " + path.string());
}

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
        setStatus("日志保留：" + retentionLabel());
        LOG_INFO("Settings: 日志保留 → " + retentionLabel() +
                 "（清理任务由 Home 模块联动，集成阶段接线）");
    }

    void doRegenerateToken() {
        auto* svc = ::module::claude::ClaudeService::global();
        if (!svc) {
            setStatus("Claude 服务不可用", true);
            return;
        }
        const int port = svc->isProxyRunning() ? svc->proxyPort() : 8888;
        if (svc->isProxyRunning()) {
            svc->stopProxy();
        }
        // startProxy 不传 token → 服务端生成新 token（claude_service 现有 API）
        auto r = svc->startProxy("127.0.0.1", port);
        if (r.isFail()) {
            setStatus("重新生成失败：" + r.errorMessage(), true);
            LOG_ERROR("Settings: 代理 token 重新生成失败: " + r.errorMessage());
            return;
        }
        writeProxyTokenFile(svc->proxyAuthToken(), port);
        setStatus("已重新生成代理 Token（端口 " + std::to_string(port) + "）");
        LOG_INFO("Settings: 代理 token 已重新生成 port=" + std::to_string(port));
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
                        drawAppearanceCard(ui, "settings.card.appearance",
                                           contentW, 160.0f, t);
                        drawDirectoryCard(ui, "settings.card.directory",
                                          contentW, 108.0f, t);
                        drawProxyCard(ui, "settings.card.proxy", contentW,
                                      140.0f, t);
                        drawLogCard(ui, "settings.card.log", contentW, 100.0f,
                                    t);
                    })
                    .build();
            })
            .build();

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
                    .lineHeight(34.0f)
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
                    .size(w - 56.0f, 22.0f)
                    .text("Claude 代理")
                    .fontSize(t.sizes.fontSizeLarge)
                    .lineHeight(22.0f)
                    .fontWeight(600)
                    .color(t.colors.textPrimary)
                    .build();
                ui.text(id + ".desc")
                    .position(28.0f, 40.0f)
                    .size(w - 56.0f, 16.0f)
                    .text(running
                          ? "本地代理 http://127.0.0.1:" + std::to_string(port) +
                                " · 客户端调用需携带 Authorization: Bearer <token>"
                          : "本地代理未运行（启动应用时会自动启动）")
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
                    .lineHeight(40.0f)
                    .color(running ? t.colors.textSecondary : t.colors.textTertiary)
                    .build();

                components::button(ui, id + ".token.regen")
                    .position(w - 28.0f - btnW, 86.0f)
                    .size(btnW, 40.0f)
                    .text("↻ 重新生成")
                    .fontSize(fs(14.0f))
                    .colors(t.colors.primary, t.colors.primaryHover,
                            t.colors.primaryPressed)
                    .textColor(eui::Color("#FFFFFF"))
                    .radius(t.sizes.radius)
                    .onClick([this] { doRegenerateToken(); })
                    .build();
            })
            .build();
    }

    // ---------- 卡片 4：日志保留策略 ----------
    void drawLogCard(eui::Ui& ui, const std::string& id, float w, float h,
                     const core::theme::Tokens& t) {
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
                    .text("0 = 永久保留；30 天为默认。清理任务随 Home 模块联动。")
                    .fontSize(fs(12.0f))
                    .lineHeight(18.0f)
                    .color(t.colors.textTertiary)
                    .build();
            })
            .build();
    }

    // ---------- 状态 ----------
    std::unique_ptr<SettingsStore> store_;
    SettingsConfig cfg_;
    bool fontDropdownOpen_ = false;
    bool retentionDropdownOpen_ = false;
    std::string statusMessage_;
    bool statusIsError_ = false;
};

} // namespace module::settings

extern "C" ::core::Module* create_settings_module() {
    return new module::settings::SettingsModule();
}
