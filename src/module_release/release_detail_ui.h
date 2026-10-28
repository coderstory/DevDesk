// 任务执行页（票据 06 / 07 / 08 / 10）：任务级前置、环境与服务执行清单、撤回、
// 提交（Draft→Pending）、任务状态（作废 / 恢复）。
//
// 契约：
//   - 宿主注入 ReleaseStore + 跨帧 state；本页只在 state.dirty 或 taskId 变化时读库，
//     渲染帧不查库。其它层改了任务数据（例如配置查看层保存）后必须置 state.dirty。
//   - 所有判定走 release_logic 的纯函数：进度、环境完成、任务完成、时间基准、提交校验；
//     本页不另算一套。提醒窗不在这里（release_reminder 独立负责）。
//   - 配置内容查看 / 编辑由本页自绘页内全屏编辑层（不经模块回调）；actions.viewConfig
//     保留为可选扩展口，本页不再调用。撤回 / 作废 / 恢复 / 校验清单（提交 / 独立
//     校验共用）是本页交互的一部分，本页自行绘制。
//   - 状态只跟时间与完成度走：Draft 不自动离开（仅「提交」按钮显式推进）；Pending 到
//     时间基准 → Running；Running / Pending 在全部完成后 → Done；Voided 只能手动
//     恢复。提前勾选前置不会让状态提前跳变。
//   - 服务顺序没有独立列：调整结果只记一条 scope=环境名 / step=reorder 的 TaskLog，
//     渲染时按最后一条还原；没有日志按读取顺序。不要为了「更干净」给 TaskService
//     加 sort 字段——store / types 属共享契约，本页无权改。
//   - 配置顺序写回 TaskConfig.order（upsertTaskConfig）；服务 / 配置排序只在
//     Draft / Pending 可调，执行中锁定。
//   - 二次确认前不写库；所有写失败经 Result 回显到 notice，不抛异常。
//   - 「校验」是只读动作：随时可跑、不改任务状态；不通过时标红缺失项并弹清单，
//     通过只给反馈。「提交」仍按 validateTask 结果决定是否放行。
//   - 配置内容（编辑层保存）/ 服务分支保存后（宿主置 state.dirty）重读时对比旧快照：
//     内容变了旧确认不作数——已完成的对应步骤自动撤回并记 action=revoke 日志；
//     未完成的不动。
//   - 跳过（配置项）与移除（服务 / 环境）都直接写库后置 dirty 重读；已跳过配置项的
//     编辑层只读，Done / Voided 整单只读：勾选、跳过、移除、撤回、编辑、排序全部
//     禁用；配置内容仍可查看（只读）。
#pragma once

#include "release_types.h"

#include <eui/dsl.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace module::release {

class ReleaseStore;

// 二次确认类型（None = 无弹层；ValidationReport = 校验未通过清单；
// RemoveService / RemoveEnv = 服务 / 环境移除确认）。
enum class ReleaseDetailConfirm {
    None,
    Revoke,
    VoidTask,
    RestoreTask,
    RemoveService,
    RemoveEnv,
    ValidationReport
};

// 撤回目标类型。
enum class ReleaseDetailRevokeKind { Prereq, Announce, Step, Config };

// 拖拽排序的目标类型。
enum class ReleaseDetailDragKind { None, ServiceOrder, ConfigOrder };

// 拖拽中间态（跨帧）：只记录锚点与目标插位，松手才写库。
// 反重构警告：serviceId / configKind 是落库依据，不得换成会随重绘过期的列表索引。
struct ReleaseDetailDragState {
    ReleaseDetailDragKind kind = ReleaseDetailDragKind::None;
    bool active = false;             // 超过框架 dragThreshold 才置位
    std::int64_t envId = 0;          // 服务拖拽：所属环境
    std::string envName;             // 服务拖拽：日志 scope
    std::int64_t serviceId = 0;      // 服务拖拽 = 被拖服务；配置拖拽 = 所属服务
    ConfigKind configKind = ConfigKind::Nacos;
    int sourceIndex = -1;            // 去源前在可视序列中的下标
    int targetIndex = -1;            // 去源后的插入位（0..n；-1 = 无有效目标）
    float deltaY = 0.0f;             // 按下点起的逻辑像素位移（DragEvent::totalY / scale）
};

// 页面会话态（宿主持有，跨帧存活）。
struct ReleaseDetailUiState {
    std::string taskId;             // 输入：要展示的任务 id（十进制文本）
    bool dirty = true;              // 置 true 时下一帧读库
    std::string loadedTaskId;       // 内部：已加载的 taskId（检测切换）
    std::string loadError;          // 读库失败的人类可读原因
    std::string notice;             // 最近一次操作反馈（成功 / 拒绝 / 失败）
    bool noticeIsError = false;

    Task task;
    // 内部：按时间与完成度推导出的展示状态；推进写库成功后同步到 state.task.status。
    TaskStatus displayStatus = TaskStatus::Draft;
    bool statusWriteFailed = false;  // 状态推进写库失败后不每帧重复提示

    // 未移除环境；服务 / 配置 / 步骤为全部环境汇总（removed 标志保留，逻辑层自行忽略）。
    std::vector<TaskEnv> envs;
    std::vector<TaskService> services;
    std::vector<TaskConfig> configs;
    std::vector<TaskStep> steps;
    // 任务日志：服务顺序还原的唯一依据（不逐帧读库）。写入成功后同步追加。
    std::vector<TaskLog> logs;

    // 提交校验：active 时 validationMissing 为 release_logic::validateTask 的原样输出，
    // 用于标红对应项；数据重读后按同一函数重算，不在 UI 重推规则。
    bool validationActive = false;
    std::vector<std::string> validationMissing;

    // 服务 / 配置拖拽排序中间态。
    ReleaseDetailDragState drag;

    // 二次确认弹层。
    ReleaseDetailConfirm confirm = ReleaseDetailConfirm::None;
    ReleaseDetailRevokeKind revokeKind = ReleaseDetailRevokeKind::Prereq;
    std::string revokePrereq;        // "pretest" / "cab" / "nacos" / "db"
    std::int64_t revokeServiceId = 0;
    StepKind revokeStepKind = StepKind::MergeBranch;
    ConfigKind revokeConfigKind = ConfigKind::Nacos;
    std::string revokeScope;         // 日志 scope（服务名 / 任务）
    std::string revokeStepLabel;     // 日志 step 与确认文案

    // 服务 / 环境移除二次确认的目标与文案。
    std::int64_t removeServiceId = 0;
    std::int64_t removeEnvId = 0;
    std::string removeName;          // 确认文案中的服务名 / 环境名

    // 配置编辑层（页内自绘全屏，不依赖模块注入）：editorOpen 时盖住整页。
    // editorText 是编辑缓冲（只写库不直改 state.configs，保存后置 dirty 重读，
    // 内容变化的自动撤回由 loadDetail → applyContentRevocations 对账完成）。
    bool editorOpen = false;
    std::int64_t editorServiceId = 0;
    ConfigKind editorKind = ConfigKind::Nacos;
    std::string editorText;
};

// 调用方注入的动作。viewConfig 保留为可选扩展口（历史契约）：详情页的配置查看 /
// 编辑已改为页内自绘全屏编辑层，本页不再调用 viewConfig。
// 回调参数只是调用期引用：调用方需自行拷贝再跨帧使用（本页可能随后重读数据）。
struct ReleaseDetailActions {
    std::function<void(const TaskEnv&, const TaskService&, const TaskConfig&)> viewConfig;
    // 返回任务列表：由模块注入（模块层 Esc 返回之外的新入口）。
    // 默认空 = 页头不显示「← 返回」按钮；注入后按钮执行本回调。
    std::function<void()> onBack;
};

// 页面入口：dirty 时从 store 读一次，再渲染并处理交互。
// store 必须比本页元素树活得久（模块成员）。
void composeReleaseDetailPage(eui::Ui& ui, const eui::Screen& screen,
                              ReleaseStore& store, ReleaseDetailUiState& state,
                              const ReleaseDetailActions& actions =
                                  ReleaseDetailActions{});

} // namespace module::release