// 一次 AI 草稿生成的自洽机制（T7b）——线程 + epoch + 锁纪律。
// worker 只触碰自己的 done 原子与下方槽位字段（done release 前 / reap acquire 后
// 构成 happens-before，槽位字段不加锁）。请求不可中断（DirectLlmClient 不加
// SendOptions，已定案）：「取消」= 弹窗关闭把 epoch 失效，请求自然跑完后结果
// 在 reap 处丢弃。onStop join 先于 llm_.reset()。
//
// 本头文件还承载 agent 模块跨 TU 共享的触发类型常量与轻量 helper：
// composeTaskDialog、applyDraftPrefill、列表/详情渲染等多处使用，
// 原在 agent_module.cpp 匿名命名空间，拆分后迁出以消除 TU 边界。

#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "task_draft.h"

namespace module::agent {

// ---- 草稿 Job 结构 / 阶段 ----

struct DraftJob {
    std::string input;      // 用户一句话（值拷贝，worker 不摸 UI 草稿成员）
    std::string modelSlot;  // 发起时的槽位键（值拷贝，防主线程并发改 editSlot_）
    int epoch = 0;          // 发起时的会话代次；收割时不匹配 = 弹窗曾关闭/已重开，结果丢弃
    std::thread thread;
    std::atomic<bool> done{false};
    bool ok = false;          // requestTaskDraft + parseTaskDraft 双双成功
    std::string errorText;    // !ok 时的失败原因（给子弹窗错误行）
    TaskDraftResult parsed;   // ok 时的解析+校验结果
};

// 子弹窗阶段：Idle = 待输入；Generating = 请求在途；Done = 已预填（notice 行可见）；
// Failed = 错误行可见。全部仅主线程读写（worker 只写 DraftJob 槽位）。
enum class DraftPhase { Idle, Generating, Done, Failed };

// ---- 触发类型常量 ----

// daily = 每天（不指定时刻）：当天首次打开后的第一个调度周期跑一次，见 Scheduler::tickNow
// once = 闹钟（一次性绝对时刻，trigger_config.once_at）：到点派发一次后自动停用
// 新类型一律追加在末尾：既有下标是编辑回填 / 测试断言的契约，插队会改写历史任务的类型
inline constexpr const char* kTriggerKeys[] = {"manual",   "schedule", "resident",
                                               "event",    "daily",    "once"};
inline constexpr const char* kTriggerLabels[] = {"手动", "定时", "常驻",
                                                 "事件", "每天", "闹钟"};
inline constexpr int kTriggerCount = 6;
// 六类触发的表单语义提示：与 kTriggerLabels 同下标，一行讲清「这是什么/怎么填」。
// 「每天」的隐藏语义（首次打开后的首个调度周期，不是固定时刻）必须写明，否则
// 新用户按直觉理解成闹钟；定时/闹钟两类的 y=212 槽平时被预览行占用（预览是
// 已算出的下次触发/错误，信息量更大），预览为空时才轮到本提示。
inline constexpr const char* kTriggerHints[] = {
    "手动：只在你点「启动」时运行，不自动调度",
    "定时：按 cron 周期自动运行（分 时 日 月 周），需应用保持开启",
    "常驻：不自动运行，点列表「入队」立即执行一轮，适合值守类任务",
    "事件：勾选的待办/笔记事件发生时自动运行",
    "每天：当天首次打开应用后的首个调度周期运行一次（需应用保持运行）",
    "闹钟：到指定时刻运行一次后自动停用，错过不补"};

inline constexpr int kTriggerScheduleIdx = 1;
inline constexpr int kTriggerEventIdx = 3;
inline constexpr int kTriggerOnceIdx = 5;

// 事件触发可选事件表：名字 = core::event 常量（唯一真源），label 是表单文案。
// 顺序即 editEvents_ 下标——两表必须同长，加事件只改这里 + editEvents_ 容量
inline constexpr const char* kEventNames[] = {
    "todo.created",
    "todo.completed",
    "md.saved",
};
inline constexpr const char* kEventLabels[] = {"新建待办", "完成待办", "保存笔记"};
inline constexpr int kEventCount = 3;

// ---- 共享 helper 函数（inline，零开销） ----

// 触发类型键 → kTriggerKeys 下标；未知类型返回 0（手动）
inline int triggerIndexOf(const std::string& triggerType) {
    for (int i = 0; i < kTriggerCount; ++i) {
        if (triggerType == kTriggerKeys[i]) return i;
    }
    return 0;
}

// 展示用文案：已知类型给中文标签，未知类型原样回显（不伪装成「手动」）
inline std::string triggerLabelOf(const std::string& triggerType) {
    for (int i = 0; i < kTriggerCount; ++i) {
        if (triggerType == kTriggerKeys[i]) return kTriggerLabels[i];
    }
    return triggerType;
}

// 顿号连接（预填摘要用；空表 = 空串）
inline std::string joinCnList(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += "、";
        out += parts[i];
    }
    return out;
}

} // namespace module::agent