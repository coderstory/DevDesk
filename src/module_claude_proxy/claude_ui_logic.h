// src/module_claude_proxy/claude_ui_logic.h
// 纯逻辑单元：「不碰 Ui、不碰 DB」的自由函数 + 调用记录弹窗常量，
// 自 claude_module.cpp 匿名命名空间逐字搬出，可无头单测。
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

// 后台任务**结果就绪 → 唤醒主循环**的唯一出口：UI 主循环无动画/无输入时阻塞在
// glfwWaitEvents，后台跑完不唤醒 → 结果要等用户下一次输入才上屏。次序不能反：
// std::async 在 callable 返回之后才发布值，先唤醒后 set_value 会让 UI 抢跑、
// 结果落内存里没人上屏。把「探就绪」和「唤醒」绑在同一出口让次序可判定：seam
// 在唤醒那一刻真读一次共享状态，写反/绕过 → seam 停 false → 无头用例判红。
template <typename T>
void wakeAfterPublish(const std::shared_future<T>& published,
                      std::atomic<bool>& sawReadySeam) {
    sawReadySeam.store(published.wait_for(std::chrono::seconds(0)) ==
                           std::future_status::ready,
                       std::memory_order_relaxed);
    // 不唤醒 = 结果要等用户下一次输入；requestUiUpdate 用 core 层——
    // app::requestUpdate() 只编进 app main TU，probe/无头链接报 Undefined symbols。
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

// ---- 共享配置片段（片段不再携带 env 键；frozenKeys 家族已拆除，findSharedById 保留）----

// 片段按 id 查找（快照定位；未绑定/不存在 → nullptr）
const SharedConfig* findSharedById(const std::vector<SharedConfig>& configs,
                                   int64_t id);

// 「复制配置」序列化（纯函数，无 IO）：完整 settings.json 形态。env 域复用
// buildDirectEnv（与「运行」临时 settings 同一条管线）；非 env 域追加
// effectiveExtraSettingsJson(p.extraSettingsJson, fragmentJson) 并入顶层（片段赢）。
// fragmentJson = 所绑片段原文（空串 = 未绑定）。序列化 = stringify(pretty)。
// keyPlaceholder = 凭据键占位值（与 apiKeyHint 同一脱敏口径），**不导明文 key**
//（剪贴板是跨应用共享面）；传空串 → 导出无 token 键。序列化失败（理论不可达）
// → 返回空串。硬约束：keyPlaceholder 只允许传脱敏形态，调用方必须守住「导出
// 结果不含凭据」（tests/test_modules/test_claude_ui.cpp 有回归网用例）。
std::string buildProfileExportJson(const Profile& p,
                                   const std::string& fragmentJson,
                                   const std::string& keyPlaceholder);

// 「复制配置」toast 副文案（常量落 logic 层供无头断言）
inline constexpr const char* kCopyConfigToastSubtitle =
    "已把该 Profile 的完整 settings.json 配置（env + 高级属性）导出到剪贴板"
    "（API Key 为占位符，不含明文）";

// ---- 调用记录弹窗常量 ----
// 状态码过滤：下拉索引 → ClaudeService::CallLogQuery::statusClass 映射
inline constexpr int kCallsStatusClasses[] = {0, 2, 3, 4, 5, 9};
inline constexpr const char* kCallsStatusLabels[] = {
    "全部状态", "2xx 成功", "3xx 重定向", "4xx 客户端错误",
    "5xx 服务端错误", "错误（超时等）"};
inline constexpr int kCallsStatusCount = 6;
// 日志弹窗每页条数（审计 [11]；总设计只承诺「过滤、搜索」，条数由 UI 自定）
inline constexpr int kCallsPageSize = 20;
// 搜索框防抖窗口：每次按键顺延窗口，停手满一个窗口才派发一次（250ms = 常见档）
inline constexpr int kCallsSearchDebounceMs = 250;

} // namespace module::claude
