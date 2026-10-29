// src/module_schedule/channels/os_notification.cpp
// OS 通知渠道（内置渠道 "os"）：系统级通知（macOS 通知中心 / Windows 托盘 NIF_INFO）。
//
// 契约边界（§4.9，全部由 platform::notify facade 兑现，本渠道只做载荷映射）：
//   * 标题 63 / 正文 255（UTF-16 码元）截断与换行清洗 —— facade。
//   * 去重 2s：id = Delivery.dedupeKey，facade 按 id+内容 2 秒窗口去重 ——
//     去重键必须透传，否则同 (rule, planned_at) 的重复投递会穿透为重复通知。
//   * 限速 3 条突发 / 1 条每秒 —— facade 令牌桶。
//   * 无点击动作：OS 通知不挂任何点击行为，deeplink 被本渠道忽略。
//   * 并发：deliver 任意线程直调 platform::notify::send（facade 自身任意线程安全）。
//   * 失败语义：环境类失败（权限拒绝/限速/后端不可用）映射为 Receipt.status，
//     不返回 Result::fail；只有载荷非法（title/body 清洗后为空）才透传 fail。
#include "module_schedule/channel_registry.h"

#include <algorithm>

#include "platform/notification.h"

namespace module::schedule {

class OsNotificationChannel final : public Channel {
public:
    std::string id() const override { return kChannelOs; }

    core::Result<Receipt> deliver(const Delivery& d) override {
        platform::notify::Notification notification;
        notification.id = d.dedupeKey;  // 去重键透传：facade 2s 窗口按 id+内容去重
        notification.title = d.title;
        notification.body = d.body;
        notification.sound = true;
        // Delivery.level（0 信息/1 成功/2 警告/3 错误）与 notify::Level 枚举序对齐
        const int clamped = std::clamp(d.level, 0, 3);
        notification.level = static_cast<platform::notify::Level>(clamped);
        // deeplink 故意不映射：OS 通知无点击动作（§4.9）

        auto result = platform::notify::send(notification);
        if (result.isFail()) {
            return core::Result<Receipt>::fail(result.errorCode(),
                                               result.errorMessage());
        }
        const auto& receipt = result.value();
        return core::Result<Receipt>::ok(Receipt{
            platform::notify::statusToString(receipt.status),
            receipt.detail.empty() ? "系统通知投递完成" : receipt.detail});
    }
};

std::unique_ptr<Channel> makeOsNotificationChannel() {
    return std::make_unique<OsNotificationChannel>();
}

}  // namespace module::schedule
