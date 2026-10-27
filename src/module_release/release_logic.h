// 上线域纯逻辑（骨架）：无 IO、无 UI 依赖，可无头单测。
#pragma once

#include "release_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace module::release {

// 任务字段校验；失败时 error 填人类可读原因
bool validateTask(const ReleaseTask& task, std::string& error);

// 距到期天数（向上取整；已过期/无记录返回 <= 0）
int daysUntilExpiry(const ReleaseTask& task, std::int64_t nowUnix);

// 步骤完成百分比（0..100；空列表 = 0）
int progressPercent(const std::vector<Step>& steps);

} // namespace module::release