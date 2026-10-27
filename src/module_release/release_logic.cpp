// 上线域纯逻辑（骨架实现）：只落最小可判定语义，后续任务按需求扩展。
#include "release_logic.h"

#include <cstddef>

namespace module::release {

bool validateTask(const ReleaseTask& task, std::string& error) {
    if (task.title.empty()) {
        error = "任务标题不能为空";
        return false;
    }
    if (task.serviceId.empty()) {
        error = "必须选择服务";
        return false;
    }
    if (task.environmentId.empty()) {
        error = "必须选择环境";
        return false;
    }
    return true;
}

int daysUntilExpiry(const ReleaseTask& task, std::int64_t nowUnix) {
    constexpr std::int64_t kSecondsPerDay = 86400;
    if (task.expiresAtUnix <= 0) return 0;
    const std::int64_t remain = task.expiresAtUnix - nowUnix;
    if (remain <= 0) return 0;
    return static_cast<int>((remain + kSecondsPerDay - 1) / kSecondsPerDay);
}

int progressPercent(const std::vector<Step>& steps) {
    if (steps.empty()) return 0;
    std::size_t done = 0;
    for (const auto& step : steps) {
        if (step.done) ++done;
    }
    return static_cast<int>(done * 100 / steps.size());
}

} // namespace module::release