// src/module_home/home_module.cpp
// Home 仪表盘模块 - 真实数据（HomeService，spec §8.4）+ 快捷按钮接线
//
// 数据：markdown_index / todo_tasks / claude_call_logs 只读聚合，
// 经 core::appDatabase() 拿共享连接（与 Markdown/Todo 模块同一装配模式）。
// 导航：core::appRouter()（main.cpp 注册），module_* 之间互不依赖；
// 跨模块动作经 Router payload 下发（"action:*" / "open:*"），由目标页消费。
#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/theme.h"
#include "home_service.h"

#include <eui/dsl.h>

#include <components/button.h>

#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace module::home {

namespace {

// 渲染单个统计卡片（stack 内子元素绝对定位，点击整卡跳转）
// numberColor/badgeColor 非空时覆盖默认配色（accent ? error : 常规）
// timerSeconds > 0 时挂一个 0 尺寸 onTimer 元素驱动本卡定期重绘
void drawStatCard(eui::Ui& ui,
                  const std::string& id,
                  float width,
                  float height,
                  const core::theme::Tokens& t,
                  const std::string& label,
                  const std::string& number,
                  const std::string& badge,
                  bool accent,
                  std::function<void()> onClick,
                  const eui::Color* numberColor = nullptr,
                  const eui::Color* badgeColor = nullptr,
                  float timerSeconds = 0.0f) {
    // 卡内布局常量随全局字号缩放派生：行高/内边距固定 px 在大字号下会
    // 裁切数字（W7 稳定化），fontSize* token 已在 theme 层缩放
    const float s = core::theme::fontScale();
    const float badgeFont = t.sizes.fontSizeSmall - 2.0f; // 11px @ scale 1
    const float padX = 28.0f * s;
    ui.stack(id)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .color(t.colors.surface)
                .hoverColor(t.colors.hover)
                .radius(t.sizes.radiusLarge)
                .border(1.0f, t.colors.borderDefault)
                .onClick(std::move(onClick))
                .build();

            ui.text(id + ".label")
                .position(padX, 20.0f * s)
                .size(width - padX * 2.0f, 16.0f * s)
                .text(label)
                .fontSize(t.sizes.fontSizeSmall - 1.0f)
                .lineHeight(16.0f * s)
                .color(t.colors.textSecondary)
                .build();

            ui.text(id + ".number")
                .position(padX, 42.0f * s)
                .size(width - padX * 2.0f, 56.0f * s)
                .text(number)
                .fontSize(t.sizes.fontSizeDisplay)
                .lineHeight(56.0f * s)
                .fontWeight(700)
                .color(numberColor ? *numberColor
                                   : (accent ? t.colors.error
                                             : t.colors.textPrimary))
                .build();

            ui.text(id + ".badge")
                .position(padX, 98.0f * s)
                .size(width - padX * 2.0f, badgeFont + 3.0f)
                .text(badge)
                .fontSize(badgeFont)
                .lineHeight(badgeFont + 3.0f)
                .color(badgeColor ? *badgeColor
                                  : (accent ? t.colors.error
                                            : t.colors.textTertiary))
                .build();

            if (timerSeconds > 0.0f) {
                // compose 不是每帧重跑（DSL.md），实时刷新靠 onTimer 周期触发
                // 整页 compose（设计 §2.6，linechart 同款机制）
                ui.stack(id + ".timer")
                    .size(0.0f, 0.0f)
                    .onTimer(timerSeconds, [] {})
                    .build();
            }
        })
        .build();
}

// 笔记显示名：标题为空时回退文件名
std::string recentNoteName(const RecentNote& note) {
    if (!note.title.empty()) return note.title;
    const auto slash = note.relativePath.find_last_of('/');
    if (slash == std::string::npos) return note.relativePath;
    return note.relativePath.substr(slash + 1);
}

// 副标题：相对时间 + 所在目录（根目录省略）
std::string recentNoteSubtitle(const RecentNote& note, std::time_t now) {
    std::string sub = formatRelativeTime(note.modifiedAt, now);
    const auto slash = note.relativePath.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        sub += " · " + note.relativePath.substr(0, slash);
    }
    return sub;
}

} // namespace

class HomeModule : public ::core::Module {
public:
    ::core::PageId pageId() const override { return ::core::PageId::Home; }
    std::string displayName() const override { return "首页"; }
    std::string iconGlyph() const override { return "🏠"; }

    // ==================== 生命周期 ====================

    void onStart() override {
        auto* db = ::core::appDatabase();
        if (db == nullptr) {
            LOG_WARN("Home 模块启动：appDatabase() 为空，仪表盘显示空态");
            // 补齐错误文案：否则 compose 拼出「数据加载失败：（空串）」
            statsError_ = "数据库未初始化（appDatabase 为空）";
            return;
        }
        service_ = std::make_unique<HomeService>(*db);
        refreshStats();
    }

    void onStop() override {
        service_.reset();
        LOG_INFO("Home 模块停止");
    }

    // 进页重查（W7）：数据只在启动查一次，从 Todo/Markdown 切回来数字
    // 不更新——Router::navigateTo 进页通知驱动重查
    void onPageEnter() override { refreshStats(); }

    // ==================== 数据 ====================

    void refreshStats() {
        if (!service_) return;
        auto r = service_->getStats(kRecentNoteLimit);
        if (r.isOk()) {
            stats_ = std::move(r.unsafeValue());
            statsLoaded_ = true;
            statsError_.clear();
            LOG_INFO("Home 仪表盘数据：今日笔记 " +
                     std::to_string(stats_.todayNotes) + " / 待办 " +
                     std::to_string(stats_.pendingTodos) + " / 逾期 " +
                     std::to_string(stats_.overdueTodos) + " / 今日 API 调用 " +
                     std::to_string(stats_.todayApiCalls) + " / 最近编辑 " +
                     std::to_string(stats_.recentNotes.size()) + " 条");
        } else {
            statsLoaded_ = false;
            statsError_ = r.errorMessage();
            LOG_ERROR("Home 仪表盘数据加载失败: " + statsError_);
        }
    }

    // ==================== 渲染 ====================

    void compose(eui::Ui& ui, const eui::Screen& screen) override {
        const auto& t = core::theme::current();
        const float pad = t.sizes.spacingXXLarge;       // 40
        const float gap = t.sizes.spacingLarge * 1.25f; // 20
        const float contentW = screen.width - pad * 2.0f;
        const float cardGap = 20.0f;
        const float statW = (contentW - cardGap * 4.0f) / 5.0f;
        // 卡高随字号缩放派生（卡内 y 布局同源缩放，大字号不裁切）
        const float statH = 120.0f * core::theme::fontScale();
        const std::time_t now = std::time(nullptr);

        // 页面背景自绘（与 settings_module 同款）：main.cpp clearColor 固定
        // 浅色，深色主题下页面必须自己铺底，否则浅色 clear 底 + 深色 token
        // 文字不可读
        ui.rect("home.pagebg")
            .size(screen.width, screen.height)
            .color(t.colors.background)
            .build();

        ui.column("home.root")
            .size(screen.width, screen.height)
            .padding(pad)
            .gap(gap)
            .content([&] {
                // ========== 顶部欢迎区 ==========
                ui.row("home.header")
                    .size(contentW, 72.0f)
                    .alignItems(eui::Align::CENTER)
                    .content([&] {
                        ui.column("home.greeting")
                            .size(std::max(280.0f, contentW - 240.0f), 72.0f)
                            .gap(6.0f)
                            .content([&] {
                                ui.text("home.h1")
                                    .size(std::max(280.0f, contentW - 240.0f), 40.0f)
                                    .text("欢迎回到 DevDesk")
                                    .fontSize(t.sizes.fontSizePage)
                                    .lineHeight(40.0f)
                                    .fontWeight(700)
                                    .color(t.colors.textPrimary)
                                    .build();
                                ui.text("home.date")
                                    .size(std::max(280.0f, contentW - 240.0f), 18.0f)
                                    .text("今天是 " + formatTodayCn(now))
                                    .fontSize(t.sizes.fontSizeSmall)
                                    .lineHeight(18.0f)
                                    .color(t.colors.textSecondary)
                                    .build();
                            })
                            .build();

                        // 右侧按钮组
                        ui.row("home.header.actions")
                            .size(212.0f, 40.0f)
                            .justifyContent(eui::Align::END)
                            .alignItems(eui::Align::CENTER)
                            .gap(t.sizes.spacingSmall)
                            .content([&] {
                                components::button(ui, "home.btn.refresh")
                                    .size(100.0f, 40.0f)
                                    .text("↻ 刷新")
                                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                                    .colors(t.colors.surfaceElevated,
                                            t.colors.hover, t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .onClick([this] { refreshStats(); })
                                    .build();

                                // 设置页 T11 落地前不可用（disabled 置灰）
                                components::button(ui, "home.btn.settings")
                                    .size(100.0f, 40.0f)
                                    .text("⚙ 设置")
                                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                                    .colors(t.colors.surfaceElevated,
                                            t.colors.hover, t.colors.active)
                                    .textColor(t.colors.textSecondary)
                                    .radius(t.sizes.radius)
                                    .border(1.0f, t.colors.borderDefault)
                                    .disabled()
                                    .build();
                            })
                            .build();
                    })
                    .build();

                // ========== 5 个统计卡片（真实数据 + Token 速率实时卡） ==========
                const bool ok = statsLoaded_;
                auto num = [&ok](int64_t v) {
                    return ok ? std::to_string(v) : std::string("--");
                };

                // Token 速率卡三态（设计 §4 方案 A）：
                //   流式中 = primary 色 + success 绿 badge；空闲/刚结束 = 灰，
                //   速率随 2s 窗口自然衰减到 0，累计定格保留
                const core::proxy_stats::Snapshot snap =
                    core::proxy_stats::snapshot();
                const bool streaming = snap.activeStreams > 0;
                const std::string sessionCn =
                    formatTokenCount(snap.sessionTokens);
                const std::string tokenBadge =
                    streaming
                        ? "● " + std::to_string(snap.activeStreams) +
                              " 路并发 · 会话累计 " + sessionCn
                        : "空闲 · 会话累计 " + sessionCn;

                ui.row("home.stats")
                    .size(contentW, statH)
                    .gap(cardGap)
                    .content([&] {
                        drawStatCard(ui, "home.stat.tokens", statW, statH, t,
                                     "Token 速率 (tok/s)",
                                     formatRateTps(snap.rateTps), tokenBadge,
                                     false,
                                     [this] { navigateTo(::core::PageId::ClaudeProxy); },
                                     streaming ? &t.colors.primary
                                               : &t.colors.textTertiary,
                                     streaming ? &t.colors.success : nullptr,
                                     0.5f);
                        drawStatCard(ui, "home.stat.notes", statW, statH, t,
                                     "今日笔记", num(stats_.todayNotes),
                                     ok ? "今日修改过的笔记" : "加载失败",
                                     false,
                                     [this] { navigateTo(::core::PageId::Markdown); });
                        drawStatCard(ui, "home.stat.todo", statW, statH, t,
                                     "待办任务", num(stats_.pendingTodos),
                                     ok ? "未完成总数" : "加载失败",
                                     false,
                                     [this] { navigateTo(::core::PageId::Todo); });
                        drawStatCard(ui, "home.stat.overdue", statW, statH, t,
                                     "逾期任务", num(stats_.overdueTodos),
                                     ok ? "已过截止时间" : "加载失败",
                                     stats_.overdueTodos > 0,
                                     [this] { navigateTo(::core::PageId::Todo); });
                        drawStatCard(ui, "home.stat.api", statW, statH, t,
                                     "API 调用", num(stats_.todayApiCalls),
                                     ok ? "今日代理调用" : "加载失败",
                                     false,
                                     [this] { navigateTo(::core::PageId::ClaudeProxy); });
                    })
                    .build();

                // ========== 最近编辑卡片（真实数据，stack 内绝对定位） ==========
                const int noteCount =
                    static_cast<int>(stats_.recentNotes.size());
                const float itemH = 44.0f;
                const float listTop = 60.0f;
                const float recentH = noteCount > 0
                                          ? listTop + noteCount * itemH + 12.0f
                                          : 108.0f;

                ui.stack("home.recent")
                    .size(contentW, recentH)
                    .content([&] {
                        ui.rect("home.recent.bg")
                            .size(contentW, recentH)
                            .color(t.colors.surface)
                            .radius(t.sizes.radiusLarge)
                            .border(1.0f, t.colors.borderDefault)
                            .build();

                        ui.text("home.recent.title")
                            .position(28.0f, 20.0f)
                            .size(contentW - 56.0f, 24.0f)
                            .text("最近编辑")
                            .fontSize(t.sizes.fontSizeLarge)
                            .lineHeight(24.0f)
                            .fontWeight(600)
                            .color(t.colors.textPrimary)
                            .build();

                        if (!ok) {
                            ui.text("home.recent.error")
                                .position(28.0f, listTop)
                                .size(contentW - 56.0f, 32.0f)
                                .text("数据加载失败：" +
                                      (statsError_.empty() ? std::string("未知错误")
                                                           : statsError_) +
                                      "（点右上角刷新重试）")
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(20.0f)
                                .wrap()
                                .color(t.colors.error)
                                .build();
                        } else if (noteCount == 0) {
                            ui.text("home.recent.empty")
                                .position(28.0f, listTop)
                                .size(contentW - 56.0f, 32.0f)
                                .text("还没有笔记。点下方「+ 新建笔记」去 Markdown 模块创建第一篇")
                                .fontSize(t.sizes.fontSizeSmall)
                                .lineHeight(20.0f)
                                .wrap()
                                .color(t.colors.textTertiary)
                                .build();
                        } else {
                            for (int i = 0; i < noteCount; ++i) {
                                const RecentNote& note = stats_.recentNotes[i];
                                const float iy = listTop + static_cast<float>(i) * itemH;
                                const auto id = std::to_string(i);
                                // 拷贝进闭包：note 是 stats_ 里的引用，
                                // refreshStats 后可能失效
                                const std::string notePath = note.relativePath;

                                // 整行可点击 → 跳转 Markdown 页并打开该笔记
                                // （Router payload："open:<相对路径>"，由
                                // Markdown 模块 compose 消费）
                                ui.rect("home.recent.row." + id)
                                    .position(12.0f, iy - 4.0f)
                                    .size(contentW - 24.0f, 40.0f)
                                    .color(t.colors.surface)
                                    .hoverColor(t.colors.hover)
                                    .radius(t.sizes.radiusSmall)
                                    .onClick([this, notePath] {
                                        navigateTo(::core::PageId::Markdown,
                                                   "open:" + notePath);
                                    })
                                    .build();
                                // 图标
                                ui.rect("home.recent.icon." + id)
                                    .position(28.0f, iy)
                                    .size(32.0f, 32.0f)
                                    .color(t.colors.surfaceElevated)
                                    .radius(t.sizes.radiusSmall)
                                    .build();
                                ui.text("home.recent.icon.text." + id)
                                    .position(28.0f, iy)
                                    .size(32.0f, 32.0f)
                                    .text("📝")
                                    .fontSize(14.0f)
                                    .lineHeight(32.0f)
                                    .horizontalAlign(eui::HorizontalAlign::Center)
                                    .build();
                                // 文件名（标题）
                                ui.text("home.recent.name." + id)
                                    .position(72.0f, iy - 1.0f)
                                    .size(contentW - 120.0f, 18.0f)
                                    .text(recentNoteName(note))
                                    .fontSize(t.sizes.fontSizeBody - 1.0f)
                                    .lineHeight(18.0f)
                                    .fontWeight(500)
                                    .color(t.colors.textPrimary)
                                    .build();
                                // 时间 + 目录
                                ui.text("home.recent.time." + id)
                                    .position(72.0f, iy + 18.0f)
                                    .size(contentW - 120.0f, 14.0f)
                                    .text(recentNoteSubtitle(note, now))
                                    .fontSize(t.sizes.fontSizeSmall - 2.0f)
                                    .lineHeight(t.sizes.fontSizeSmall + 1.0f)
                                    .color(t.colors.textTertiary)
                                    .build();
                            }
                        }
                    })
                    .build();

                // ========== 快捷操作按钮组（Router payload 跨模块指令）==========
                const float actGap = t.sizes.spacingLarge; // 16
                const float actW = (contentW - actGap * 2.0f) / 3.0f;
                ui.row("home.actions")
                    .size(contentW, 44.0f)
                    .gap(actGap)
                    .content([&] {
                        // + 新建笔记 → 跳 Markdown 并触发新建笔记弹窗
                        // （payload 由 Markdown 模块 compose 消费）
                        components::button(ui, "home.act.newnote")
                            .size(actW, 44.0f)
                            .text("+ 新建笔记")
                            .fontSize(t.sizes.fontSizeBody)
                            .colors(t.colors.primary, t.colors.primaryHover,
                                    t.colors.primaryPressed)
                            .textColor(eui::Color("#FFFFFF"))
                            .radius(t.sizes.radius)
                            .onClick([this] {
                                navigateTo(::core::PageId::Markdown,
                                           "action:new_note");
                            })
                            .build();

                        // + 新建任务 → 跳 Todo 并触发新建任务弹窗
                        components::button(ui, "home.act.newtodo")
                            .size(actW, 44.0f)
                            .text("+ 新建任务")
                            .fontSize(t.sizes.fontSizeBody)
                            .colors(t.colors.surfaceElevated, t.colors.hover,
                                    t.colors.active)
                            .textColor(t.colors.textPrimary)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .onClick([this] {
                                navigateTo(::core::PageId::Todo,
                                           "action:new_task");
                            })
                            .build();

                        // 切换 Claude Profile → Claude 模块（Profile 列表在该页）
                        components::button(ui, "home.act.claude")
                            .size(actW, 44.0f)
                            .text("📋 切换 Claude Profile")
                            .fontSize(t.sizes.fontSizeBody)
                            .colors(t.colors.surfaceElevated, t.colors.hover,
                                    t.colors.active)
                            .textColor(t.colors.textPrimary)
                            .radius(t.sizes.radius)
                            .border(1.0f, t.colors.borderDefault)
                            .onClick([this] { navigateTo(::core::PageId::ClaudeProxy); })
                            .build();
                    })
                    .build();
            })
            .build();
    }

private:
    static constexpr int kRecentNoteLimit = 5;

    void navigateTo(::core::PageId page, const std::string& payload = "") {
        auto* router = ::core::appRouter();
        if (router == nullptr) {
            LOG_WARN("Home: Router 未注册，无法跳转到 " +
                     std::string(::core::pageIdToString(page)));
            return;
        }
        router->navigateTo(page, payload);
    }

    std::unique_ptr<HomeService> service_;
    DashboardStats stats_;
    bool statsLoaded_ = false;
    std::string statsError_;
};

} // namespace module::home

extern "C" ::core::Module* create_home_module() {
    return new module::home::HomeModule();
}
