// 上线弹层：新建任务（票据 04）。
//
// 契约：
//   - 日期选择用 EUI-NEO 的 components::datePicker（月/日/年三列滚轮），不自绘月历。
//   - 「一天一个任务」按计划日期算：同一天已有非作废任务时直接打开既有任务，不新建；
//     作废任务不占当天名额（可再建）。
//   - 非常规发布日（非周二/周四）必须经过一次二次确认；取消只关确认层，回到日期选择。
//   - 确认后写入任务 + 每个选中环境的快照（环境名/时间窗/服务清单与服务类型/默认分支）；
//     快照字段在这里按基础配置填充，之后基础配置怎么改都不回灌已建任务。
//   - 所有落库错误经 Result 回传并显示在弹层内，不抛异常、不静默吞错。
#pragma once

#include "release_types.h"

#include <eui/dsl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace module::release {

// 数据层由 release_store.h 定义（并行落地的契约见票据）；头文件只用到引用，
// 前向声明可让本头与数据层实现解耦（测试文件不被迫拉入 SQLite 头）。
class ReleaseStore;

// 新建日期的纯决策（无 IO / 无 UI，可无头单测）。
enum class ReleaseNewTaskDateAction {
    InvalidDate,        // 非法日期：不允许创建
    Create,             // 周二/周四：直接建
    ConfirmIrregular,   // 其他日子：先弹二次确认
    OpenExisting,       // 同日已有非作废任务：打开既有，不新建
};

struct ReleaseNewTaskDateDecision {
    ReleaseNewTaskDateAction action = ReleaseNewTaskDateAction::InvalidDate;
    std::int64_t existingTaskId = 0;  // 仅 OpenExisting 有意义
};

ReleaseNewTaskDateDecision releaseDecideNewTaskDate(const std::string& planDate,
                                                    const std::vector<Task>& tasks);

// 弹层会话态（宿主持有，跨帧存活）。只有 open 由调用方置位；
// 打开沿（open false→true）由绘制函数自己清缓存并加载基础配置。
struct ReleaseNewTaskDialogState {
    bool open = false;
    bool wasOpen = false;             // 内部：打开沿检测
    bool dataLoaded = false;          // 内部：本次会话是否已加载
    bool pickerOpen = false;
    bool irregularConfirmOpen = false;
    bool irregularConfirmed = false;  // 本次会话已确认「非常规发布日」
    std::string planDate;             // YYYY-MM-DD（picker 的提交值）
    int year = 0;
    int month = 0;
    int day = 0;
    std::vector<std::int64_t> selectedEnvIds;
    std::string error;
    std::vector<EnvDef> envs;
    std::vector<ServiceDef> services;
};

// 新建任务弹层。openTask 由调用方注入：同日已有任务或创建成功后跳执行页。
// 数据经 store.findTaskByDate / listEnvs / listServices 读取；确认后调用
// store.createTask + store.addTaskEnv 落库。
void drawNewTaskDialog(eui::Ui& ui, const eui::Screen& screen,
                       ReleaseStore& store,
                       ReleaseNewTaskDialogState& state,
                       const std::function<void(std::int64_t taskId)>& openTask);

// 其余弹层由后续票据实现；保留声明避免并行开发期调用点断裂。
void drawFullConfigEditorDialog(eui::Ui& ui);
void drawWithdrawConfirmDialog(eui::Ui& ui);

} // namespace module::release