// 待办到期提醒（need453）：挂在调度引擎唯一 1Hz 泵上（ScheduleEngine 每 tick 末
// 回调），内部按 kScanIntervalSec 节流扫 todo_tasks，到期未完成的任务经既有渠道
// 注册表（默认 toast）聚合投递。
//
// 为什么不依赖 module_todo：模块隔离禁令（module_* 互不 include，
// check-module-isolation.sh 断言 A），故直接读 todo_tasks/todo_columns 表。
// 跨模块读表先例 = module_agent/app_stats_tool.cpp；表契约（列名与列范围口径）
// 以 core/overdue.h 为唯一载体，schema 变更在那头文件同步。
//
// 去重：进程内账（taskId → 已提醒的 dueAt）——同任务同截止时刻只投一次；账
// 每轮按「当前到期集」重建，完成/改期/删除的任务自动出账；超出单轮上限的任务
// 不记账，下轮继续投（不静默丢）。进程重启账清零 → 已到期任务首轮补报一次
// （有意：重启后用户仍需被提醒；落库要新表+迁移，本期不做）。
//
// 并发契约：只在 tick 线程（主线程）调用，无锁。
#pragma once

#include "core/sqlite_db.h"
#include "module_schedule/channel_registry.h"

#include <cstdint>
#include <map>

namespace module::schedule {

class TodoDueWatcher {
public:
    TodoDueWatcher(core::data::SqliteDb& db, ChannelRegistry& channels);

    // 引擎每 tick 调用；内部节流到每 kScanIntervalSec 扫一次（时钟回拨重开窗口）
    void tick(int64_t nowUnix);

    // 测试缝：清空节流时刻与提醒账
    void resetForTest();

    static constexpr int kScanIntervalSec = 60;
    // 单条聚合通知最多带几个任务（toast 单槽覆盖，必须合成一条；超出的下轮补投）
    static constexpr int kMaxTasksPerNotice = 5;
    // 正文最多列出的标题数，超出以「等 N 个任务已到期」收尾
    static constexpr int kMaxTitlesInBody = 3;

private:
    void scan(int64_t nowUnix);

    core::data::SqliteDb& db_;
    ChannelRegistry& channels_;
    int64_t lastScanAt_ = 0;
    std::map<int64_t, int64_t> notified_;  // taskId → 已提醒的 dueAt
};

}  // namespace module::schedule