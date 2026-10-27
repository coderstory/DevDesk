// 提醒引擎（骨架空实现）：策略待后续任务。
#include "release_reminder.h"

namespace module::release {

void ReleaseReminder::tick(std::int64_t nowUnix) { lastTickUnix_ = nowUnix; }

} // namespace module::release