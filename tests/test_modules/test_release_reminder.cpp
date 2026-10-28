// tests/test_modules/test_release_reminder.cpp
// 提醒窗调度/抑制/渲染零自动化断言（审计 C2 M2）：把 release_reminder.cpp
// 抽出的纯决策逻辑（release_reminder_logic）钉上契约。零窗口、零 DB、零系统
// 时钟——now / due 清单 / 状态结构全部由用例喂入，只断言纯函数结论。
//
// 每条用例注明防的具体事故：
//   ① due 清单空 → 关窗决策          防：全部做完清空清单后窗口残留不收起
//   ② due 非空 + 冷却期内 → 不开窗    防：用户关窗后 5 分钟内立即重弹
//   ③ 冷却过期 + due 非空 → 开窗      防：冷却到期该弹不弹（含应用重开补弹）
//   ④ 指纹相同 → 不重弹；变化 → 重弹  防：清单没变反复打扰 / 变了却哑火
//   ⑤ demo 三电平映射                防：演示开关错色（DEVDESK_RELEASE_DEMO）
#include "doctest.h"
#include "test_support.h"

#include "module_release/release_reminder_logic.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace rel = module::release;

rel::DueItem makeItem(rel::RemindLevel level, std::int64_t taskId,
                      const std::string& scope, const std::string& title) {
    rel::DueItem item;
    item.level = level;
    item.taskId = taskId;
    item.scope = scope;
    item.title = title;
    item.detail = "明细";
    return item;
}

rel::ReminderTaskGroup makeGroup(std::int64_t taskId,
                                 const std::string& planDate,
                                 rel::TaskStatus status,
                                 std::vector<rel::DueItem> taskItems) {
    rel::ReminderTaskGroup group;
    group.taskId = taskId;
    group.planDate = planDate;
    group.status = status;
    group.level = rel::RemindLevel::Red;
    group.envNames = {"EU"};
    group.taskItems = std::move(taskItems);
    return group;
}

rel::ReminderView makeView(std::vector<rel::DueItem> items,
                           std::vector<rel::ReminderTaskGroup> tasks) {
    rel::ReminderView view;
    view.items = std::move(items);
    view.tasks = std::move(tasks);
    return view;
}

} // namespace

// 防（审计 C2 M2）：tick 里「items.empty → closeForEmpty」的判据被改坏时，
// 全部做完后空清单不再触发关窗，全屏提醒窗残留不收起。关窗决策的输入
// hasDueItems 恒与 due 清单非空性同真同假。
REGISTER_TEST(release_reminder_empty_due_means_close,
              "release 提醒：due 清单空 → 关窗决策，非空 → 开窗候选（防：清空后窗口残留）", {
    const rel::ReminderView empty;
    DOCTEST_CHECK_FALSE(rel::hasDueItems(empty.items));

    const rel::ReminderView due = makeView(
        {makeItem(rel::RemindLevel::Red, 1, "task", "数据库工单未审批")}, {});
    DOCTEST_CHECK(rel::hasDueItems(due.items));
    return true;
})

// 防：用户关窗后冷却期内被立即重弹（suppressUntil 门失效）；或冷却到期/应用
// 重开（状态全零 = 重启后首 tick）后该弹不弹（补弹门失效）。
REGISTER_TEST(release_reminder_suppress_gate,
              "release 提醒：冷却期内不开窗、过期与重开补弹开窗（防：关窗立即重弹/该弹不弹）", {
    const std::string fingerprint = "fp-1";
    // 冷却期内：suppressUntil 在未来 → 不开窗
    DOCTEST_CHECK_FALSE(rel::shouldOpenReminder(
        /*currentNow=*/1000, /*suppressUntil=*/1300, /*lastShownAt=*/1000,
        fingerprint, fingerprint));
    // 冷却到期：suppressUntil 已过（且重弹窗也早已出窗）→ 开窗
    DOCTEST_CHECK(rel::shouldOpenReminder(100000, 2000, 1000, fingerprint,
                                          fingerprint));
    // 冷却上界恰好到期（now == suppressUntil）→ 开窗（边界不回退）
    DOCTEST_CHECK(rel::shouldOpenReminder(1300, 1300, 1000, fingerprint,
                                          fingerprint));
    // 应用重开补弹：状态全零 + 空指纹 → 开窗（首 tick 即补弹，错过不吞）
    DOCTEST_CHECK(rel::shouldOpenReminder(1000, 0, 0, "", fingerprint));
    return true;
})

// 防：同清单在重弹窗口内反复打扰（指纹护栏失效）；清单变化后却因指纹误判哑火。
// 时钟回拨（currentNow < lastShownAt）差值不可信，必须按「未冷却」处理直接开窗。
REGISTER_TEST(release_reminder_fingerprint_gate,
              "release 提醒：同指纹不重弹、指纹变化/时钟回拨重弹（防：清单没变反复打扰）", {
    const std::string fingerprint = "fp";
    // lastShownAt 距今 < 5 分钟且指纹相同 → 不重弹
    DOCTEST_CHECK_FALSE(rel::shouldOpenReminder(1000, 0, 950, fingerprint,
                                                fingerprint));
    // 同窗口内指纹变化 → 重弹
    DOCTEST_CHECK(rel::shouldOpenReminder(1000, 0, 950, fingerprint, "fp-2"));
    // 时钟回拨：currentNow < lastShownAt → 按「未冷却」处理，开窗
    DOCTEST_CHECK(rel::shouldOpenReminder(900, 0, 950, fingerprint, fingerprint));
    return true;
})

// 防：指纹串编解码失真——同内容产不同指纹（用户被重复打扰）或不同内容产同
// 指纹（变更被吞掉不重弹）。条目级字段（title/status/…）任一漏编码都会失真。
REGISTER_TEST(release_reminder_fingerprint_build,
              "release 提醒：指纹串同内容稳定、条目变化即变（防：去重键失真）", {
    const rel::ReminderView v1 = makeView(
        {makeItem(rel::RemindLevel::Red, 1, "task", "工单未审批")},
        {makeGroup(1, "2026-10-27", rel::TaskStatus::Running,
                   {makeItem(rel::RemindLevel::Red, 1, "task", "工单未审批")})});
    // 同内容 → 同指纹
    rel::ReminderView same = v1;
    DOCTEST_CHECK_EQ(rel::buildFingerprint(v1), rel::buildFingerprint(same));
    // 条目标题变化 → 指纹变化
    rel::ReminderView titleChanged = v1;
    titleChanged.tasks[0].taskItems[0].title = "上线公告未发";
    DOCTEST_CHECK_NE(rel::buildFingerprint(v1),
                     rel::buildFingerprint(titleChanged));
    // 任务状态变化 → 指纹变化
    rel::ReminderView statusChanged = v1;
    statusChanged.tasks[0].status = rel::TaskStatus::Pending;
    DOCTEST_CHECK_NE(rel::buildFingerprint(v1),
                     rel::buildFingerprint(statusChanged));
    // 空视图的指纹是确定性的空串（不因「空 == 空」误判重弹）
    const rel::ReminderView empty;
    DOCTEST_CHECK(rel::buildFingerprint(empty).empty());
    return true;
})

// 防：DEVDESK_RELEASE_DEMO 的 red/orange/yellow 映射错位时演示窗错色；
// 非法值/未设置被当演示处理时生产路径误吃假数据。
REGISTER_TEST(release_reminder_demo_level_map,
              "release 提醒：DEVDESK_RELEASE_DEMO 三电平映射（防：演示开关错色）", {
    rel::RemindLevel level = rel::RemindLevel::Yellow;
    {
        test_support::ScopedEnv env("DEVDESK_RELEASE_DEMO", "red");
        DOCTEST_CHECK(rel::readDemoLevelFromEnv(level));
        DOCTEST_CHECK(level == rel::RemindLevel::Red);
    }
    {
        test_support::ScopedEnv env("DEVDESK_RELEASE_DEMO", "orange");
        DOCTEST_CHECK(rel::readDemoLevelFromEnv(level));
        DOCTEST_CHECK(level == rel::RemindLevel::Orange);
    }
    {
        test_support::ScopedEnv env("DEVDESK_RELEASE_DEMO", "yellow");
        DOCTEST_CHECK(rel::readDemoLevelFromEnv(level));
        DOCTEST_CHECK(level == rel::RemindLevel::Yellow);
    }
    // 非法值按未设置处理：返回 false 且不写 level
    {
        test_support::ScopedEnv env("DEVDESK_RELEASE_DEMO", "green");
        const rel::RemindLevel before = level;
        DOCTEST_CHECK_FALSE(rel::readDemoLevelFromEnv(level));
        DOCTEST_CHECK(level == before);
    }
    // 未设置 → false
    {
        test_support::ScopedEnv env("DEVDESK_RELEASE_DEMO", "");
        DOCTEST_CHECK_FALSE(rel::readDemoLevelFromEnv(level));
    }
    return true;
})

// 防：看门狗宽限期算错——失配/在飞未超期被误判丢窗（正常窗被重开干扰），或
// 超期不判丢（提醒永久哑火）；冷却上界被更早的到期覆盖缩短（用户关窗冷却失效）；
// 过期代数回调误动当前窗状态（看门狗重开后旧请求搅乱新窗）。
REGISTER_TEST(release_reminder_watchdog_suppress_generation,
              "release 提醒：看门狗宽限/冷却上界/迟到代数（防：窗口失联哑火、冷却被缩短、旧回调搅局）", {
    // 句柄失配看门狗：宽限期内不判丢（恰 5s 也不丢），超过宽限期判丢
    DOCTEST_CHECK_FALSE(rel::watchdogLostHandleGraceElapsed(1005, 1000));
    DOCTEST_CHECK(rel::watchdogLostHandleGraceElapsed(1006, 1000));
    // 开窗请求在飞看门狗：同口径
    DOCTEST_CHECK_FALSE(rel::openRequestGraceElapsed(1005, 1000));
    DOCTEST_CHECK(rel::openRequestGraceElapsed(1006, 1000));
    // 冷却上界只增不减：更晚的冷却不被更早的关闭覆盖
    DOCTEST_CHECK_EQ(rel::computeSuppressUntil(1000, 900),
                     1000 + rel::kReshowSeconds);
    DOCTEST_CHECK_EQ(rel::computeSuppressUntil(1000, 1000 + rel::kReshowSeconds + 60),
                     1000 + rel::kReshowSeconds + 60);
    // 迟到代数：发起代数 != 当前代数 → 忽略；同代数 → 正常处理
    DOCTEST_CHECK(rel::isStaleGeneration(3, 4));
    DOCTEST_CHECK_FALSE(rel::isStaleGeneration(4, 4));
    return true;
})
