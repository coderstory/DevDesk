// 提醒引擎（骨架）：到期前提醒的调度入口，具体策略由后续任务填充。
#pragma once

#include <cstdint>

namespace module::release {

class ReleaseReminder {
public:
    // 由帧泵/定时器驱动；nowUnix = 当前 Unix 秒。幂等，重复调用不重复提醒。
    void tick(std::int64_t nowUnix);

private:
    std::int64_t lastTickUnix_ = 0;
};

} // namespace module::release