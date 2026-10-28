// src/module_schedule/channels/silent_channel.cpp
// 静默渠道（内置渠道 "silent"）：空投递。
//
// 契约（§4.9）：
//   * 任何载荷一律 ok(Receipt{"ok"})，无失败路径、无副作用。
//   * 运行/投递记录由 store 层负责（schedule_run / schedule_delivery），
//     本渠道不写库、不触 UI、不联网——别给它加"顺手记一笔"的逻辑。
//   * 用途：静默执行类动作仍走统一分发链，落记录但零打扰。
#include "module_schedule/channel_registry.h"

namespace module::schedule {
namespace {

class SilentChannel final : public Channel {
public:
    std::string id() const override { return kChannelSilent; }

    core::Result<Receipt> deliver(const Delivery&) override {
        return core::Result<Receipt>::ok(Receipt{"ok", "空投递，记录由 store 层负责"});
    }
};

}  // namespace

std::unique_ptr<Channel> makeSilentChannel() {
    return std::make_unique<SilentChannel>();
}

}  // namespace module::schedule
