// src/module_release/release_schedule_source.h
// 上线任务的系统定时只读派生（S8）：作为 ScheduleProvider 的数据源，把有计划日
// 的未终结任务折算成定时视图条目。本文件只产纯数据（module_release 自有结构），
// 不 include module_schedule——模块间不可互相依赖，适配器在装配层（main.cpp）。
#pragma once

#include "module_release/release_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace module::release {

// 系统定时条目（字段语义与 schedule 的 ScheduleItemView 一一对应，类型自持）
struct ReleaseScheduleEntry {
    std::string key;        // 稳定合成键 "release:task:<id>:precheck"
    std::string title;      // announceText（空则「上线任务 #<id>」）
    std::string summary;    // 「计划上线 YYYY-MM-DD」
    std::optional<int64_t> nextFireAt;  // 计划日本地零点的 epoch 秒；日期非法 = 无值
    bool enabled = true;
    std::string sourceRoute;  // kReleaseTasksPageKey
};

// 派生规则：全量任务里滤掉终态（Done / Voided）与无 planDate 者，一条任务一项。
// 折算走 planDateElapsedMinutes（全仓唯一时间口径）：零点 epoch = nowUnix − 分钟×60。
// 读库失败降级为空列表——单个来源故障不得拖垮定时列表页。
std::vector<ReleaseScheduleEntry> releaseScheduleEntries(ReleaseStore& store,
                                                         std::int64_t nowUnix);

} // namespace module::release
