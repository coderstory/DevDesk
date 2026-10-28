// 上线域纯逻辑：无 IO、无 UI、无系统时钟依赖（now 由调用方传入）。
// 所有判定只读入参快照——不在这里查库/读全局，否则提醒与完成判定无法无头单测。
// 反重构警告：dueItems 的条目级别与到期时刻是提醒引擎/弹窗的外部契约；
// 不得为了「少弹几条」把多项合并成一项，也不得改变红/橙/黄的触发时刻。
#pragma once

#include "release_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace module::release {

// 常规发布日 = 周二 / 周四。isoDate 形如 "YYYY-MM-DD"；格式错、月日越界、
// 不存在的日期（如 2 月 30 日）一律 false——不抛异常，也不猜「最近的有效日」。
bool isRegularReleaseDay(const std::string& isoDate);

// 严格日历合法性（与 isRegularReleaseDay 同一解析实现）：仅判「YYYY-MM-DD 且
// 该日期真实存在」，不含周二/周四判定。供数据层写入口校验 planDate。
bool isValidIsoDate(const std::string& isoDate);

// 任务时间基准 = 未移除环境的最早开始分钟（当天 0..1439）；无有效环境返回 -1。
// 跨零点环境（end <= start）只影响窗口归属，不改变其「开始时刻」的取值。
std::int64_t taskBaseline(const std::vector<TaskEnv>& envs);

// 计划日零点起的本地流逝分钟——全仓库唯一的时间换算口径（提醒侧、执行页、
// 状态推导都必须调本函数，禁止各自另起「日历日差 × 1440」等口径）。
//   用本地时区把 planDate 的零点折成 epoch 秒（mktime，DST 切换日也取该日真实零点），
//   再取 (nowUnix − 零点) / 60（向负无穷取整）。跨零点继续累计，可 > 1440；
//   计划日之前为负。不读系统时钟（nowUnix 由调用方传入）；
//   日期非法或本地时间不可表示时返回 false，且不写 outMinute。
bool planDateElapsedMinutes(const std::string& planDate, std::int64_t nowUnix,
                            std::int64_t& outMinute);

// 列表展示状态：只按时间推导、只用于显示，不写库（库中状态仍由提交 / 作废 /
// 恢复动作维护；本函数是无 IO / 不读系统时钟的纯函数，nowUnix 由调用方传入）。
//   Done / Voided 原样返回（终态不随时间回流）。
//   Draft 恒为 Draft：提交这个动作才改变状态；已过基准的未提交任务由提醒条目
//     「任务未提交」负责警示，状态栏不显示「执行中」。
//   Running 恒为 Running（不回退）。
//   Pending：nowMinute = 计划日零点起的本地流逝分钟（换算一律走
//     planDateElapsedMinutes：本地时区零点起算，DST 日也不偏移；跨零点可 > 1440，
//     计划日之前为负），与 taskBaseline 同口径比较：
//     nowMinute < 时间基准 → Pending；nowMinute >= 时间基准 → Running。
//   无有效环境（基准 -1）或计划日期非法 → 没有时间依据，保持库值不猜。
TaskStatus deriveStatus(const Task& task,
                        const std::vector<TaskEnv>& envs,
                        std::int64_t nowUnix);

// 到期聚合（提醒引擎唯一判据）：返回「此刻已到期但未完成」的全部条目。
//   nowMinute = 计划日零点起的分钟数；跨零点继续累计，可 > 1440。
//   任务 Done / Voided → 空（已完成不收债；已作废停止提醒）。
//   Draft 且未到基准 → 空（配置中不打扰）；Draft 已过基准 → 末尾追加红「任务未提交」。
//   基准 −60 已到：提测单 / CAB / 适用工单 每项未完成各一条橙。
//   基准已到且公告未发 → 一条黄。
//   环境开始已到且该环境未完成 → 每环境一条红「部署未完成」。
//     完成判定与 envCompleted 同源：每个未移除服务的 3 个固定步骤（缺行按未完成）
//     + 期望配置槽位（后端固定 5 项；缺行按未完成、跳过视为关闭）。服务全部
//     移除的环境 0/0 视为完成，不产出部署条目。合并分支进度参与完成判定，
//     但没有独立提醒条目。
//   工单适用性 = 仅统计未移除环境下的未移除服务：任务内存在该 kind 的
//     未跳过且内容非空的配置（没内容 = 不欠工单）。
// 条目顺序：任务级前置 → 公告 → 各环境部署（按入参顺序）→ 未提交（最后）。
std::vector<DueItem> dueItems(const Task& task,
                              const std::vector<TaskEnv>& envs,
                              const std::vector<TaskService>& services,
                              const std::vector<TaskConfig>& configs,
                              const std::vector<TaskStep>& steps,
                              int nowMinute);

// 兼容入口：不提供步骤数据（等价于空 steps——全部步骤按未完成处理，只要环境
// 仍有未移除服务就报红，与旧契约行为一致）。详情页已迁移到带 steps 的入口，
// 剩余调用方仅为旧测试（test_release_logic.cpp）——新代码一律走带 steps 的
// 入口，否则步骤已全部做完的环境仍会持续报红。
std::vector<DueItem> dueItems(const Task& task,
                              const std::vector<TaskEnv>& envs,
                              const std::vector<TaskService>& services,
                              const std::vector<TaskConfig>& configs,
                              int nowMinute);

// 一窗多档时的窗色：红 > 橙 > 黄；空集返回黄（调用方应先判空再展示）。
RemindLevel highestLevel(const std::vector<DueItem>& items);

// 提交前完整性校验：返回人类可读的缺失项描述（空 = 通过）。
//   未移除服务分支为空 → 一条；未跳过配置内容为空 → 一条。
//   已移除服务及其配置不参与；跳过项不参与。不校验分支真伪 / 内容对错。
std::vector<std::string> validateTask(const std::vector<TaskService>& services,
                                      const std::vector<TaskConfig>& configs);

// 服务类型的期望配置项数（进度与完成判定的分母基准）：Backend 固定 5 项
// （nacos / db / cron / menu / gateway），前端（PcWeb / MobileWeb）为 0。
// 与库中实际配置行数无关——缺行按未完成计，多行按 kind 去重不重复计 done。
int expectedConfigs(ServiceKind kind);

// 进度 = 每个未移除服务的 3 个固定步骤（合并分支 / 部署 / 验证）+ 期望配置数
// （expectedConfigs：后端固定 5 项，前端 0 项）。分母与库中行数无关：步骤行 /
// 配置行缺失一律计入 total 并按未完成计；配置槽位「已跳过」视为关闭（计 done）。
// done 以 doneAt != 0 计；同 kind 多行按槽位去重，done 不会超过 total。
struct Progress {
    int done = 0;
    int total = 0;
};

// 环境进度；removed 环境与服务全被移除的环境均为 0/0。
Progress envProgress(const TaskEnv& env,
                     const std::vector<TaskService>& services,
                     const std::vector<TaskConfig>& configs,
                     const std::vector<TaskStep>& steps);

// 任务进度 = 全部未移除环境的进度之和 + 任务级前置（提测 / CAB / 适用工单 / 公告）。
Progress taskProgress(const Task& task,
                      const std::vector<TaskEnv>& envs,
                      const std::vector<TaskService>& services,
                      const std::vector<TaskConfig>& configs,
                      const std::vector<TaskStep>& steps);

// 环境完成 = 该环境进度已满（done == total）。
//   服务全被移除 / 环境无服务 → 0/0 → 完成；removed 环境不欠账 → 完成。
//   TaskStep 中 kind=Config 的行不参与（配置以 TaskConfig.doneAt 记账，避免双重记账）。
//   配置槽位口径与 envProgress 相同：期望配置缺行按未完成计、跳过视为关闭。
bool envCompleted(const TaskEnv& env,
                  const std::vector<TaskService>& services,
                  const std::vector<TaskConfig>& configs,
                  const std::vector<TaskStep>& steps);

// 任务完成 = 任务进度已满（所有未移除环境完成 + 任务级前置全部完成）。
// 只做事实判定，不看 Task.status（状态流转由调用方按时间推进）。
bool taskCompleted(const Task& task,
                   const std::vector<TaskEnv>& envs,
                   const std::vector<TaskService>& services,
                   const std::vector<TaskConfig>& configs,
                   const std::vector<TaskStep>& steps);

} // namespace module::release