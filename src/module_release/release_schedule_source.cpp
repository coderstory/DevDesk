// src/module_release/release_schedule_source.cpp
#include "release_schedule_source.h"

#include <utility>

#include "module_release/release_logic.h"
#include "module_release/release_module.h"

namespace module::release {

std::vector<ReleaseScheduleEntry> releaseScheduleEntries(ReleaseStore& store,
                                                         std::int64_t nowUnix) {
    std::vector<ReleaseScheduleEntry> out;
    const auto tasks = store.listTasks();
    if (tasks.isFail()) return out;
    out.reserve(tasks.value().size());
    for (const auto& task : tasks.value()) {
        if (task.status == TaskStatus::Done || task.status == TaskStatus::Voided)
            continue;
        if (task.planDate.empty()) continue;
        ReleaseScheduleEntry entry;
        entry.key = "release:task:" + std::to_string(task.id) + ":precheck";
        entry.title = task.announceText.empty() ? "上线任务 #" + std::to_string(task.id)
                                                : task.announceText;
        entry.summary = "计划上线 " + task.planDate;
        std::int64_t minute = 0;
        if (planDateElapsedMinutes(task.planDate, nowUnix, minute))
            entry.nextFireAt = nowUnix - minute * 60;
        entry.enabled = true;
        entry.sourceRoute = kReleaseTasksPageKey;
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace module::release
