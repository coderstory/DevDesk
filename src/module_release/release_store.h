// 上线域数据访问层：基础配置（服务/环境）+ 任务及其快照（环境/服务/配置/步骤/日志）。
//
// 契约：
//   - 错误一律 Result 返回，不抛异常；SQL 参数一律绑定，不拼接调用方输入。
//   - 任务快照与基础配置解耦：改/删基础配置（ServiceDef / EnvDef）不回灌已建任务。
//   - 「移除」一律置 removed 标志，不物理删已进任务的环境/服务（历史留痕）。
//   - 排序：服务/环境按 sort、配置项按 order、任务按 planDate 降序、日志按 at 升序。
//   - 任务列表分页在 SQL 侧完成（listTasksPage 的 LIMIT/OFFSET）；调用方不得
//     先读全表再在内存切片。
//   - 枚举列以文本存库（schema.h 约定）；读回非法文本按 ValidationError 失败，
//     不回落默认值——否则损坏数据会被当成正常任务展示。
#pragma once

#include "core/result.h"
#include "core/sqlite_db.h"
#include "release_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace module::release {

class ReleaseStore {
public:
    explicit ReleaseStore(core::data::SqliteDb& db);

    core::Result<std::vector<ServiceDef>> listServices();
    // id == 0 新增（返回新 id），否则按 id 更新（id 不存在 → NotFound）
    core::Result<std::int64_t> upsertService(const ServiceDef& def);
    core::Result<void> deleteService(std::int64_t id);

    core::Result<std::vector<EnvDef>> listEnvs();
    // 含 serviceIds 关联表写入：先整体清空该环境的关联，再按 def.serviceIds 重建
    core::Result<std::int64_t> upsertEnv(const EnvDef& def);
    core::Result<void> deleteEnv(std::int64_t id);

    // 全量读取，仅供内部（提醒引擎、执行页）与测试使用。
    // 列表页禁止用它：一次读全表再在内存切片会让分页形同虚设，请用 listTasksPage。
    core::Result<std::vector<Task>> listTasks();
    // 列表页按需取页：SQL LIMIT/OFFSET，排序口径同 listTasks（plan_date DESC, id DESC）。
    // offset/limit 为负 → ValidationError；limit == 0 → 空页。
    core::Result<std::vector<Task>> listTasksPage(int offset, int limit);
    // 任务总数，供列表页判断「是否还有更多」。
    // 计入已作废任务（列表页仍展示作废卡），与 findTaskByDate「作废不占当天名额」
    // 的口径刻意不同：后者是同日唯一性，前者要跟列表可见条数一致。
    core::Result<int> countTasks();
    // 同计划日、非 voided 的任务（作废不占当天名额）；未命中 → ok(nullopt)
    core::Result<std::optional<Task>> findTaskByDate(const std::string& planDate);
    core::Result<std::int64_t> createTask(const Task& task);
    // 新建任务的原子入口：一个事务内完成建任务 + 写入所有环境快照及其服务实例。
    // 任一环境失败 → 整体回滚，不产生任何残留行（旧的两步走会残留 Draft 死单）。
    // 入参 envs[i].taskId 一律被忽略并绑定本次新建任务；envs 与 servicesPerEnv
    // 必须等长，否则事务前直接 ValidationError。
    core::Result<std::int64_t> createTaskWithEnvs(
        const Task& task,
        const std::vector<TaskEnv>& envs,
        const std::vector<std::vector<TaskService>>& servicesPerEnv);
    core::Result<void> updateTask(const Task& task);

    core::Result<std::vector<TaskEnv>> listTaskEnvs(std::int64_t taskId);
    // services 的 id / taskEnvId 由本方法落库生成（入参值被忽略）
    core::Result<std::int64_t> addTaskEnv(const TaskEnv& env,
                                          const std::vector<TaskService>& services);
    core::Result<void> removeTaskEnv(std::int64_t taskEnvId);

    core::Result<std::vector<TaskService>> listTaskServices(std::int64_t taskEnvId);
    core::Result<void> updateTaskService(const TaskService& svc);
    core::Result<void> removeTaskService(std::int64_t taskServiceId);

    core::Result<std::vector<TaskConfig>> listTaskConfigs(std::int64_t taskServiceId);
    core::Result<void> upsertTaskConfig(const TaskConfig& cfg);

    core::Result<std::vector<TaskStep>> listTaskSteps(std::int64_t taskServiceId);
    // 一条 SQL 拉整个任务的全部步骤（经 release_task_service / release_task_env
    // 两级 JOIN 归集），供提醒引擎一次装载后按 taskServiceId 分发，消除
    // 「每服务一条查询」的 N+1 扫描。
    core::Result<std::vector<TaskStep>> listTaskStepsByTask(std::int64_t taskId);
    // doneAt == 0 表示撤回（该步未完成），已有行置 0，不删行
    core::Result<void> setStepDone(std::int64_t taskServiceId, StepKind kind,
                                   std::int64_t doneAt);

    core::Result<void> appendLog(const TaskLog& log);
    core::Result<std::vector<TaskLog>> listLogs(std::int64_t taskId);

private:
    core::data::SqliteDb& db_;
};

} // namespace module::release