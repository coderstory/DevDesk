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

// 格式化毫秒数为 "1234ms" / "1.2s"
std::string formatLatency(int64_t ms);

// 按字节截断 UTF-8 串（截点回退到字符边界，不把多字节字符切成半个）
std::string truncateUtf8(const std::string& s, size_t maxBytes);

// 模型映射摘要（详情面板一行式）：分母取自槽位表（不手写清单与硬编码 /6）
std::string modelsSummary(const Profile& p);

// 「导入当前配置」名称建议：BASE_URL host；提取不到 →「导入 <日期>」；用户可改
std::string suggestImportName(const std::string& baseUrl);

// 共享配置片段

// 片段按 id 查找（快照定位；未绑定/不存在 → nullptr）
const SharedConfig* findSharedById(const std::vector<SharedConfig>& configs,
                                   int64_t id);

// 「复制配置」序列化（纯函数，无 IO）：完整 settings.json 形态；env 复用
// buildDirectEnv，非 env 域 = 生效 JSON（片段赢）并入顶层。不导明文 key；失败 → 空串。
std::string buildProfileExportJson(const Profile& p,
                                   const std::string& fragmentJson,
                                   const std::string& keyPlaceholder);

// 「复制配置」toast 副文案（常量落 logic 层供无头断言）
inline constexpr const char* kCopyConfigToastSubtitle =
    "已把该 Profile 的完整 settings.json 配置（env + 高级属性）导出到剪贴板"
    "（API Key 为占位符，不含明文）";

// 调用记录弹窗常量
// 状态码过滤：下拉索引 → ClaudeService::CallLogQuery::statusClass 映射
inline constexpr int kCallsStatusClasses[] = {0, 2, 3, 4, 5, 9};
inline constexpr const char* kCallsStatusLabels[] = {
    "全部状态", "2xx 成功", "3xx 重定向", "4xx 客户端错误",
    "5xx 服务端错误", "错误（超时等）"};
inline constexpr int kCallsStatusCount = 6;
// 日志弹窗每页条数
inline constexpr int kCallsPageSize = 20;
// 搜索框防抖窗口：每次按键顺延窗口，停手满一个窗口才派发一次（250ms = 常见档）
inline constexpr int kCallsSearchDebounceMs = 250;

} // namespace module::claude
