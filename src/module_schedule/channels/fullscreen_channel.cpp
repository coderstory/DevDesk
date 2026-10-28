// src/module_schedule/channels/fullscreen_channel.cpp
// 全屏提醒渠道（内置渠道 "fullscreen"）。
//
// 契约（与 release_reminder 同源，独立最小实现）：
//   * 聚合：多条待投 Delivery 合并进同一个独立提醒窗，不逐条开窗。
//   * 冷却：窗被用户关闭后 5 分钟内不再开窗（期间新投递继续入队，不丢）。
//   * 重启首 tick 补弹：deliver 只入队、开窗延迟到 flushToMainThread 聚合——
//     启动初期（宿主首 tick 之前）到达的投递由首次 flush 补弹，不吞。
//     跨进程重启的补弹依赖宿主在启动时把未完成的投递重新投递（store 层职责，
//     本渠道不写库）。
//   * 去重：同 dedupeKey 的投递在队列中合并为一条（同 (rule, planned_at)
//     不重复弹）；dedupeKey 为空 = 不参与合并，逐条独立。
//   * 并发：deliver 任意线程（互斥量保护队列）；flushToMainThread 与
//     notifyWindowClosed 仅主线程（触 UI）。开窗回调在锁外调用——回调可能
//     同步触发 notifyWindowClosed，锁内调用会自锁。
//   * 关窗判定：用户点「关闭」按钮 / Alt+F4 等一切窗口销毁路径统一走
//     onWindowDestroyed → notifyWindowClosed，冷却口径一致。
//
// 本波为独立置顶窗的最小实现；真全屏（无边框最大化）与视觉细化由 UI 波完成。
#include "module_schedule/channel_registry.h"

#include <components/button.h>
#include <core/window/window_backend.h>
#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <memory>
#include <mutex>
#include <vector>

#include "core/logging.h"
#include "core/time.h"

namespace module::schedule {
namespace {

constexpr std::int64_t kCooldownSeconds = 300;  // 关闭后冷却 5 分钟（§4.9）

// 主线程帧共享态：compose 每帧读 items 渲染；开窗/刷新/关窗只写 handle。
struct FrameState {
    std::vector<Delivery> items;
    core::window::Handle handle = nullptr;
    std::function<void()> onClosed;  // 最近一次开窗接线；窗口销毁时调用
};

// 生产默认开窗接线：独立置顶窗，compose 每帧渲染 FrameState::items，
// 关闭按钮走 requestWindowClose（与 Alt+F4 同路，统一由 onWindowDestroyed
// 触发 notifyClosed）。全部回调主线程。
void composeReminderWindow(eui::Ui& ui, const eui::Screen& screen,
                           const std::shared_ptr<FrameState>& frame) {
    ui.column("reminder.root")
        .position(0.0f, 0.0f)
        .size(screen.width, screen.height)
        .content([&] {
            const float pad = 24.0f;
            const float textWidth = screen.width - pad * 2.0f;
            int row = 0;
            for (const auto& item : frame->items) {
                ui.text("reminder.item." + std::to_string(row))
                    .position(pad, pad + row * 88.0f)
                    .text(item.title)
                    .fontSize(20.0f)
                    .maxWidth(textWidth)
                    .wrap(true)
                    .build();
                ++row;
                ui.text("reminder.item." + std::to_string(row))
                    .position(pad, pad + (row - 1) * 88.0f + 30.0f)
                    .text(item.body)
                    .fontSize(15.0f)
                    .maxWidth(textWidth)
                    .wrap(true)
                    .build();
                ++row;
            }
            components::button(ui, "reminder.btn.close")
                .position(pad, screen.height - 64.0f)
                .size(120.0f, 40.0f)
                .text(" 关闭")
                .onClick([frame] {
                    if (frame->handle != nullptr) {
                        core::window::requestWindowClose(frame->handle);
                    }
                })
                .build();
        })
        .build();
}

}  // namespace

class FullscreenChannel final : public Channel {
public:
    // updater 为空 = 生产默认（工厂随后用 setWindowUpdater 装真窗口接线）。
    // now 为空 = core::time::nowUnix()。
    explicit FullscreenChannel(ChannelWindowUpdater updater, ChannelNowFn now)
        : updater_(std::move(updater)),
          now_(now ? std::move(now)
                   : [] { return static_cast<std::int64_t>(core::time::nowUnix()); }) {}

    // 替换开窗/刷新回调。工厂在构造后用它装生产接线；测试注入假回调。
    void setWindowUpdater(ChannelWindowUpdater updater) {
        std::lock_guard<std::mutex> lock(mutex_);
        updater_ = std::move(updater);
    }

    std::string id() const override { return kChannelFullscreen; }

    core::Result<Receipt> deliver(const Delivery& d) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!d.dedupeKey.empty()) {
            for (auto& pending : pending_) {
                if (pending.dedupeKey == d.dedupeKey) {
                    pending = d;  // 同键合并：后到覆盖（含 level/body 更新）
                    if (windowOpen_) dirty_ = true;
                    return core::Result<Receipt>::ok(
                        Receipt{"queued", "与既有待弹条目按 dedupeKey 合并"});
                }
            }
        }
        pending_.push_back(d);
        if (windowOpen_) dirty_ = true;
        return core::Result<Receipt>::ok(Receipt{"queued", "已入聚合队列"});
    }

    // 仅主线程：聚合开窗 / 窗已开时刷新 / 冷却期保持入队。
    // 开窗回调在锁外调用（回调可能同步触发 notifyWindowClosed）。
    void flushToMainThread() override {
        std::vector<Delivery> snapshot;
        bool callUpdater = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!updater_) {
                if (!pending_.empty()) {
                    LOG_WARN("fullscreen 渠道无开窗回调，待弹 " +
                             std::to_string(pending_.size()) + " 条保持入队");
                }
                return;
            }
            if (windowOpen_) {
                if (dirty_) {
                    snapshot = pending_;  // 刷新已开窗内容
                    dirty_ = false;
                    callUpdater = true;
                }
                return;
            }
            if (pending_.empty()) return;
            const std::int64_t now = now_();
            if (now < cooldownUntil_) return;  // 冷却中：保持入队，到期后补弹
            windowOpen_ = true;
            cooldownUntil_ = 0;
            snapshot = pending_;
            callUpdater = true;
        }
        if (callUpdater) {
            updater_(snapshot, [this] { notifyWindowClosed(); });
        }
    }

    // 仅主线程：窗口销毁（用户关闭）后调用——清已读、进 5 分钟冷却
    void notifyWindowClosed() {
        std::lock_guard<std::mutex> lock(mutex_);
        windowOpen_ = false;
        pending_.clear();
        cooldownUntil_ = now_() + kCooldownSeconds;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Delivery> pending_;
    bool windowOpen_ = false;
    bool dirty_ = false;  // 窗打开期间新投递 → 下次 flush 刷新窗内容
    std::int64_t cooldownUntil_ = 0;
    ChannelWindowUpdater updater_;
    ChannelNowFn now_;
};

std::unique_ptr<Channel> makeFullscreenChannel() {
    auto channel = std::make_unique<FullscreenChannel>(nullptr, nullptr);
    const auto frame = std::make_shared<FrameState>();
    channel->setWindowUpdater(
        [frame](const std::vector<Delivery>& items,
                const std::function<void()>& notifyClosed) {
            frame->items = items;
            frame->onClosed = notifyClosed;
            if (frame->handle != nullptr) return;  // 已开窗：compose 每帧读 items 即刷新
            app::openWindow(
                app::DslWindowConfig{}
                    .title("定时提醒")
                    .pageId("schedule.reminder")
                    .windowSize(760, 520)
                    .alwaysOnTop(true)
                    .focusOnShow(true)
                    .onWindowCreated([frame](core::window::Handle handle) {
                        frame->handle = handle;
                    })
                    .onWindowDestroyed([frame](core::window::Handle handle) {
                        if (frame->handle == handle) frame->handle = nullptr;
                        if (frame->onClosed) frame->onClosed();
                    }),
                [frame](eui::Ui& ui, const eui::Screen& screen) {
                    composeReminderWindow(ui, screen, frame);
                });
        });
    return channel;
}

std::unique_ptr<Channel> makeFullscreenChannelForTest(ChannelWindowUpdater updater,
                                                      ChannelNowFn now) {
    return std::make_unique<FullscreenChannel>(std::move(updater), std::move(now));
}

}  // namespace module::schedule
