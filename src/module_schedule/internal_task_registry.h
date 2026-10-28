// src/module_schedule/internal_task_registry.h
// 内部任务注册表（§4.3 run_task / §14）：动作层只认 key，不认具体模块。
// app_assembly 在装配段把来源模块提供的任务回调注册进来并经 core::ServiceLocator
// 暴露；调度引擎（S4）/动作层按需 requireService<InternalTaskRegistry>() 取用。
// key 形如 "agent:<task_id>"、"release:<check>"——key 的稳定性由注册方负责，
// 本表不解析冒号语法。
//
// 线程模型：注册/注销只在启动装配段与测试体内（单线程），运行期查找只读——
// 与 core::ServiceLocator 同口径，本表内部不加锁。
#pragma once

#include "core/result.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace module::schedule {

// 任务回调：入参为该次触发携带的 action_json 原文；返回 fail = 该次 run_task
// 执行失败（由动作层决定重试/落 run 状态，本表不参与编排）。
using InternalTaskFn =
    std::function<core::Result<void>(const std::string& actionJson)>;

class InternalTaskRegistry {
public:
    // 注册任务并接管所有权。重复 key → fail(AlreadyExists) 且不替换——
    // 装配段可以重跑，但已注册的任务实现不得被静默换掉（ChannelRegistry 同款契约）。
    core::Result<void> registerTask(const std::string& key, InternalTaskFn fn);

    // 只读查找；未知 key → nullptr
    const InternalTaskFn* find(const std::string& key) const;

    // 已注册 key 快照（诊断 / 测试断言用）
    std::vector<std::string> keys() const;

    // 测试缝：清空全部注册（生产不调用）
    void resetForTest();

private:
    std::map<std::string, InternalTaskFn> tasks_;
};

} // namespace module::schedule
