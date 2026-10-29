#include "module_schedule/channel_registry.h"

#include <algorithm>
#include <utility>

#include "core/logging.h"
#include "module_schedule/channels/wecom.h"

namespace module::schedule {

// ---- 内置渠道的实现见 channels/*.cpp（各自独立文件，工厂在此装配）----

std::unique_ptr<Channel> makeFullscreenChannel();
std::unique_ptr<Channel> makeToastChannel();
std::unique_ptr<Channel> makeOsNotificationChannel();
std::unique_ptr<Channel> makeSilentChannel();

std::unique_ptr<Channel> makeBuiltinChannel(const std::string& id) {
    if (id == kChannelFullscreen) return makeFullscreenChannel();
    if (id == kChannelToast) return makeToastChannel();
    if (id == kChannelOs) return makeOsNotificationChannel();
    if (id == kChannelSilent) return makeSilentChannel();
    if (id == kChannelWecom) {
        // 默认配置装配（per-row config 的装配缝仍是 registerWecomChannel）：
        // key 未配置时 deliver 即回 missing_secret 回执，不发请求
        return makeWecomChannel(WecomConfig{});
    }
    return nullptr;
}

void registerBuiltinChannels(ChannelRegistry& registry) {
    for (const char* id : {kChannelFullscreen, kChannelToast, kChannelOs,
                           kChannelSilent, kChannelWecom}) {
        auto channel = makeBuiltinChannel(id);
        if (!channel) continue;
        auto result = registry.registerChannel(std::move(channel));
        if (result.isFail()) {
            // 重复注册（宿主重复 onStart）：幂等装配，保留首个实现
            LOG_WARN("schedule 渠道重复注册被跳过：" + std::string(id) + " — " +
                     result.errorMessage());
        }
    }
}

core::Result<void> ChannelRegistry::registerChannel(
    std::unique_ptr<Channel> channel) {
    if (!channel) {
        return core::Result<void>::fail(core::ErrorCode::ValidationError,
                                        "渠道指针为空");
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& existing : channels_) {
        if (existing->id() == channel->id()) {
            return core::Result<void>::fail(core::ErrorCode::AlreadyExists,
                                            "渠道 id 已注册：" + channel->id());
        }
    }
    channels_.push_back(std::move(channel));
    return core::Result<void>::ok();
}

const Channel* ChannelRegistry::find(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& channel : channels_) {
        if (channel->id() == id) return channel.get();
    }
    return nullptr;
}

core::Result<DispatchReport> ChannelRegistry::deliver(
    const std::vector<std::string>& channelIds, const Delivery& d) {
    DispatchReport report;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& id : channelIds) {
        auto it = std::find_if(channels_.begin(), channels_.end(),
                               [&id](const auto& c) { return c->id() == id; });
        if (it == channels_.end()) {
            report.entries.push_back(
                {id, core::Result<Receipt>::fail(core::ErrorCode::NotFound,
                                                 "未注册的渠道 id：" + id)});
            continue;
        }
        report.entries.push_back({id, (*it)->deliver(d)});
    }
    return core::Result<DispatchReport>::ok(std::move(report));
}

void ChannelRegistry::flushAllToMainThread() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& channel : channels_) {
        channel->flushToMainThread();
    }
}

void ChannelRegistry::resetForTest() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    channels_.clear();
}

}  // namespace module::schedule
