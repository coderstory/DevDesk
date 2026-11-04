// schedule.runs：定时任务执行历史只读查询工具（need 219 F-01）。
// 数据源 = ScheduleStore::listRuns（schedule_runs 表），零新存储；schedule 模块
// 自注册进 core::ToolRegistry（todo_tools 同款模式），模块禁用 → 工具随之消失。
#pragma once

#include <memory>

#include "core/tool.h"
#include "module_schedule/schedule_store.h"

namespace module::schedule {

std::shared_ptr<core::ITool> makeScheduleRunsTool(ScheduleStore& store);

} // namespace module::schedule
