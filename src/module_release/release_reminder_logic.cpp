// 提醒窗纯决策逻辑实现（审计 C2 M2 抽取）：函数体自 release_reminder.cpp
// 逐行搬移，只做了签名/入参具象化，判定逻辑与常量取值未变。契约见
// release_reminder_logic.h。
#include "release_reminder_logic.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace module::release {

namespace {

void appendItemFingerprint(std::string& out, const DueItem& item) {
    out += std::to_string(static_cast<int>(item.level));
    out += '\x1f';
    out += std::to_string(item.taskId);
    out += '\x1f';
    out += item.scope;
    out += '\x1f';
    out += item.title;
    out += '\x1f';
    out += item.detail;
    out += '\x1e';
}

} // namespace

std::string buildFingerprint(const ReminderView& view) {
    std::string out;
    for (const ReminderTaskGroup& group : view.tasks) {
        out += "T";
        out += std::to_string(group.taskId);
        out += '|';
        out += group.planDate;
        out += '|';
        out += std::to_string(static_cast<int>(group.status));
        for (const std::string& envName : group.envNames) {
            out += "|E";
            out += envName;
        }
        for (const DueItem& item : group.taskItems) appendItemFingerprint(out, item);
        for (const ReminderEnvSection& section : group.envSections) {
            out += "|S";
            out += section.envName;
            for (const DueItem& item : section.items) appendItemFingerprint(out, item);
        }
        out += '\n';
    }
    return out;
}

bool hasDueItems(const std::vector<DueItem>& items) { return !items.empty(); }

// DEVDESK_RELEASE_DEMO = red / orange / yellow；非法值按未设置处理。
// 只在引擎构造时调用一次：运行期不再 getenv。
bool readDemoLevelFromEnv(RemindLevel& level) {
    const char* raw = std::getenv("DEVDESK_RELEASE_DEMO");
    if (raw == nullptr) return false;
    const std::string value(raw);
    if (value == "red") {
        level = RemindLevel::Red;
        return true;
    }
    if (value == "orange") {
        level = RemindLevel::Orange;
        return true;
    }
    if (value == "yellow") {
        level = RemindLevel::Yellow;
        return true;
    }
    return false;
}

bool watchdogLostHandleGraceElapsed(std::int64_t currentNow,
                                    std::int64_t openConfirmedAt) {
    return currentNow - openConfirmedAt > kWindowOpenGraceSeconds;
}

bool openRequestGraceElapsed(std::int64_t currentNow,
                             std::int64_t openRequestedAt) {
    return currentNow - openRequestedAt > kWindowOpenGraceSeconds;
}

std::int64_t computeSuppressUntil(std::int64_t currentNow,
                                  std::int64_t suppressUntil) {
    return std::max(suppressUntil, currentNow + kReshowSeconds);
}

bool shouldOpenReminder(std::int64_t currentNow, std::int64_t suppressUntil,
                        std::int64_t lastShownAt,
                        const std::string& lastShownFingerprint,
                        const std::string& viewFingerprint) {
    if (currentNow < suppressUntil) return false;

    // 同一批到期项（内容指纹相同）在冷却窗口内不重复弹；时钟回拨时
    // currentNow < lastShownAt，差值不可信，直接按「未冷却」处理。
    const bool withinReshowWindow =
        currentNow >= lastShownAt && currentNow - lastShownAt < kReshowSeconds;
    if (withinReshowWindow && viewFingerprint == lastShownFingerprint) return false;

    return true;
}

bool isStaleGeneration(std::uint64_t generation,
                       std::uint64_t currentGeneration) {
    return generation != currentGeneration;
}

} // namespace module::release
