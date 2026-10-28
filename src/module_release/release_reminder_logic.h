// 提醒窗纯决策逻辑（审计 C2 M2 抽取）：从 release_reminder.cpp 的 Impl 中抽出的
// 可无头测试部分——零窗口、零 DB、零 IO、零系统时钟（now / 状态全部由调用方传入）。
// release_reminder.cpp 的调度 / 抑制 / 渲染逐点调用本文件的函数：本文件是这些
// 行为的唯一实现处，改这里 = 改生产行为。
//
// 契约（与抽取前行为逐点一致）：
//   - 指纹：同一到期聚合（任务/环境/条目全同）产同一指纹串；条目级字段
//     （level/taskId/scope/title/detail）逐字段编码，\x1f 分字段、\x1e 分条目。
//   - 开/关窗：due 清单空 → 关窗（hasDueItems 为 false）；非空才走开窗判据。
//   - 冷却门：用户关窗后 kReshowSeconds 秒内不重弹（computeSuppressUntil 只增
//     不减；「内容变空」的自动关窗不进入冷却）。
//   - 重弹窗护栏：冷却窗口内同指纹不重弹；指纹变化立即重弹；时钟回拨
//     （currentNow < lastShownAt）差值不可信，直接按「未冷却」处理。
//   - 补弹/应用重开：状态全零（lastShownAt=0 / suppressUntil=0 / 指纹空）时
//     shouldOpenReminder 恒为 true——重启后首个 tick 即补弹，错过不吞。
//   - 看门狗：开窗请求在飞 / 句柄已确认后失配，超过 kWindowOpenGraceSeconds
//     判定失败允许重试，避免提醒永久哑火。
//   - 迟到代数：过期代数的创建/销毁回调一律忽略（isStaleGeneration）。
//   - demo：DEVDESK_RELEASE_DEMO = red / orange / yellow 映射三档提醒色；只在
//     引擎构造时解析一次，非法值 / 未设置按未设置处理（不写 level、返回 false）。
#pragma once

#include "release_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace module::release {

// 用户关闭后的冷却时长
inline constexpr std::int64_t kReshowSeconds = 5 * 60;
// 开窗请求的确认宽限期：onWindowCreated 未在此时长内确认句柄即视为创建
// 失败（EUI-NEO openWindow 只是入队，失败时创建/销毁回调都不触发）。
inline constexpr std::int64_t kWindowOpenGraceSeconds = 5;

// 一个环境分组（scope 非 "task" 的到期条目）。
struct ReminderEnvSection {
    std::string envName;
    std::vector<DueItem> items;
};

// 一个任务的展示分组：任务级条目 + 各环境条目；条目顺序沿用 dueItems 的输出。
struct ReminderTaskGroup {
    std::int64_t taskId = 0;
    std::string planDate;
    TaskStatus status = TaskStatus::Draft;
    RemindLevel level = RemindLevel::Yellow;
    std::vector<std::string> envNames;  // 任务全部未移除环境（展示上下文）
    std::vector<DueItem> taskItems;
    std::vector<ReminderEnvSection> envSections;
};

struct ReminderView {
    std::vector<ReminderTaskGroup> tasks;
    std::vector<DueItem> items;
    std::string fingerprint;
    RemindLevel level = RemindLevel::Yellow;
};

// 到期聚合 → 指纹串（「清单没变不重弹」的判据）：只读 view.tasks（任务/环境/
// 条目全同则指纹相同）；空视图的指纹为空串。
std::string buildFingerprint(const ReminderView& view);

// due 清单非空 = true。调用方以 false 执行关窗决策（全部做完就不弹，已开的窗
// 也要收掉）；开窗判据（shouldOpenReminder）只应在 true 之后评估。
bool hasDueItems(const std::vector<DueItem>& items);

// DEVDESK_RELEASE_DEMO = red / orange / yellow → true 并写出对应档位；
// 未设置或取值非法 → false 且不写 level。
bool readDemoLevelFromEnv(RemindLevel& level);

// 看门狗①：开窗状态与句柄失配（句柄确认后丢失）超过宽限期 → 判丢，允许清态
// 重试。只做时间判据；windowOpen / 句柄为空的失配前提由调用方判断。
bool watchdogLostHandleGraceElapsed(std::int64_t currentNow,
                                    std::int64_t openConfirmedAt);

// 看门狗②：开窗请求在飞但句柄迟迟未确认，超过宽限期 → 判定创建失败，允许
// 下一轮重开。只做时间判据；「请求在飞」（openRequestedAt != 0）由调用方判断。
bool openRequestGraceElapsed(std::int64_t currentNow,
                             std::int64_t openRequestedAt);

// 用户关窗后的冷却上界：max(现值, 关窗时刻 + kReshowSeconds)——只增不减，
// 已有更晚的冷却（更近的一次关闭）不被更早的值覆盖。
std::int64_t computeSuppressUntil(std::int64_t currentNow,
                                  std::int64_t suppressUntil);

// 补弹/重弹最终判据（due 非空时调用）：冷却门（currentNow < suppressUntil 不弹）
// + 同指纹重弹窗护栏（lastShownAt 起的冷却窗口内同指纹不弹；指纹变化 / 时钟
// 回拨一律按「未冷却」处理）。true = 开窗。
bool shouldOpenReminder(std::int64_t currentNow, std::int64_t suppressUntil,
                        std::int64_t lastShownAt,
                        const std::string& lastShownFingerprint,
                        const std::string& viewFingerprint);

// 迟到代数：回调捕获的发起代数 != 当前代数 → 忽略（看门狗放弃旧请求重开后，
// 旧请求的窗口创建/销毁不得搅乱当前窗状态）。
bool isStaleGeneration(std::uint64_t generation,
                       std::uint64_t currentGeneration);

} // namespace module::release
