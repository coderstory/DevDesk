// src/module_schedule/channels/toast_channel.cpp
// 应用内 toast 渠道（内置渠道 "toast"）：跨页轻提示，适合失败降级。
//
// 契约（§4.9）：
//   * 单实例队列：待弹槽位只有 1，flush 前多次投递后到覆盖前到——
//     只弹最新一条，不叠加、不排队（app 层 toast 本就是单槽覆盖语义）。
//   * 自动消失由 app 层 toast（compose 每帧绘制 + 超时 clearAppNotice）负责，
//     本渠道不管理消失时机。
//   * 并发：deliver 任意线程只入队（互斥量保护槽位）；flushToMainThread
//     仅主线程调用，是槽位内容唯一落 UI（showAppNotice）的时机。
//   * 载荷裁剪：本渠道只透传 title/body；level/deeplink/runId/dedupeKey
//     不参与 toast 渲染（记录层仍可经 Receipt 回溯）。
#include "module_schedule/channel_registry.h"

#include <mutex>
#include <optional>

#include "app_sidebar.h"  // app::showAppNotice（应用级 toast，跨页可见）

namespace module::schedule {
namespace {

// 默认落点：app 级 toast。测试注入假 sink 断言主线程封送语义。
void defaultSink(const std::string& title, const std::string& body) {
    app::showAppNotice(title, body);
}

}  // namespace

class ToastChannel final : public Channel {
public:
    using ToastSink = std::function<void(const std::string&, const std::string&)>;

    // sink 为空 = 生产默认（app::showAppNotice）
    explicit ToastChannel(ToastSink sink = nullptr)
        : sink_(sink ? std::move(sink) : defaultSink) {}

    std::string id() const override { return kChannelToast; }

    core::Result<Receipt> deliver(const Delivery& d) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = d;  // 单实例：后到覆盖前到
        return core::Result<Receipt>::ok(Receipt{"queued", "已入待弹槽位"});
    }

    void flushToMainThread() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_) return;
        sink_(pending_->title, pending_->body);
        pending_.reset();
    }

private:
    mutable std::mutex mutex_;
    std::optional<Delivery> pending_;  // 槽位：1 条，flush 即清
    ToastSink sink_;
};

std::unique_ptr<Channel> makeToastChannel() {
    return std::make_unique<ToastChannel>();
}

std::unique_ptr<Channel> makeToastChannelForTest(ChannelToastSink sink) {
    return std::make_unique<ToastChannel>(std::move(sink));
}

}  // namespace module::schedule
