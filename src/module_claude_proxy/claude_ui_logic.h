// src/module_claude_proxy/claude_ui_logic.h
// 纯逻辑单元：「不碰 Ui、不碰 DB」的自由函数 + 调用记录弹窗常量，可无头单测。
#pragma once

#include "claude_service.h"

#include <core/platform/platform.h>   // core::platform::requestUiUpdate

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>

namespace module::claude {

// 后台任务结果就绪 → 唤醒主循环的唯一出口（UI 阻塞在 glfwWaitEvents，不唤醒则结果等下次输入才上屏）。
// 次序不能反：async 先发布值后返回，先唤醒后发布会让 UI 抢跑；seam 在唤醒时真读共享状态，写反 → 无头用例判红。
template <typename T>
void wakeAfterPublish(const std::shared_future<T>& published,
                      std::atomic<bool>& sawReadySeam) {
    sawReadySeam.store(published.wait_for(std::chrono::seconds(0)) ==
                           std::future_status::ready,
                       std::memory_order_relaxed);
    // 用 core 层 requestUiUpdate（app::requestUpdate 只编进 app main TU，probe/无头链接不过）。
    core::platform::requestUiUpdate();
}

// 模型配置摘要（详情面板一行式）：分母取自槽位表（不手写清单与硬编码 /6）
std::string modelsSummary(const Profile& p);

// 「复制配置」序列化（纯函数，无 IO）：settings.json 形态；env 复用
// buildDirectEnv。不导明文 key；失败 → 空串。
std::string buildProfileExportJson(const Profile& p,
                                   const std::string& keyPlaceholder);

// 「复制配置」toast 副文案（常量落 logic 层供无头断言）
inline constexpr const char* kCopyConfigToastSubtitle =
    "已把该 Profile 的 settings.json env 配置导出到剪贴板"
    "（API Key 为占位符，不含明文）";

} // namespace module::claude
