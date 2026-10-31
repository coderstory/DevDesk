#pragma once
// driver_report.h — 结果报告（债-18 UI 驱动入口底层件，零 probe_harness 依赖）：
// stdout 逐步中文明细 / --report-json 结构化明细 / 退出码聚合纯函数（F-04）。
#include "platform/json.h"

#include <string>
#include <utility>
#include <vector>

namespace ui_drive {

struct StepResult {
    int index = 0;               // 1 起步骤号
    std::string type;            // 步骤 type（click/type/key/...）
    std::string target;          // 报告显示用摘要（id / 键名 / 文本截断）
    bool passed = false;
    double elapsedMs = 0.0;
    std::string hitId;           // click 命中回读实测 id（空 = 未执行回读）
    std::string screenshotPath;  // 失败现场截图 / screenshot 步骤产物
    std::string errorReason;     // 中文失败原因
};

// 退出码聚合纯函数（F-01 契约）：0 全过 | 1 有步骤失败或报告·截图落盘失败 |
// 2 场景无步骤（0 步在加载期已拦退 2，此处兜底防假绿——对齐 devdesk_tests
// 0 命中退 2 口径）。77（无显示跳过）由 main 的金丝雀直出，不经本函数。
inline int aggregateExitCode(const std::vector<StepResult>& results, bool reportWriteOk) {
    if (results.empty()) return 2;
    if (!reportWriteOk) return 1;
    for (const StepResult& r : results) {
        if (!r.passed) return 1;
    }
    return 0;
}

// stdout 逐步中文明细一行：
//   [3/7] PASS click todo.board.new（命中回读: todo.board.add.bg）
//   [5/7] FAIL 等待元素 'todo.item.x' 超时 5000ms（失败原因见 errorReason）
inline std::string formatStepLine(const StepResult& result, int totalSteps) {
    std::string line = "[" + std::to_string(result.index) + "/" +
                       std::to_string(totalSteps) + "] " +
                       (result.passed ? "PASS " : "FAIL ") + result.type;
    if (!result.target.empty()) line += " " + result.target;
    if (!result.hitId.empty()) line += "（命中回读: " + result.hitId + "）";
    if (!result.screenshotPath.empty()) line += "（截图: " + result.screenshotPath + "）";
    if (!result.passed && !result.errorReason.empty()) line += " —— " + result.errorReason;
    if (!result.passed) {
        line += " [耗时 " + std::to_string(static_cast<int>(result.elapsedMs)) + "ms]";
    }
    return line;
}

// --report-json：结构化明细（步骤号/type/状态/耗时/命中 id/截图路径/错误原因）。
inline std::string buildReportJson(const std::vector<StepResult>& results,
                                   const std::string& sceneName,
                                   int exitCode) {
    platform::json::JsonValue steps = platform::json::makeArray();
    for (const StepResult& r : results) {
        platform::json::JsonObject step;
        step.set("index", platform::json::makeInt(r.index));
        step.set("type", platform::json::makeString(r.type));
        step.set("target", platform::json::makeString(r.target));
        step.set("passed", platform::json::makeBool(r.passed));
        step.set("elapsedMs", platform::json::makeDouble(r.elapsedMs));
        step.set("hitId", platform::json::makeString(r.hitId));
        step.set("screenshot", platform::json::makeString(r.screenshotPath));
        step.set("error", platform::json::makeString(r.errorReason));
        steps.append(platform::json::JsonValue(std::move(step)));
    }
    platform::json::JsonObject report;
    report.set("scene", platform::json::makeString(sceneName));
    report.set("exitCode", platform::json::makeInt(exitCode));
    report.set("total", platform::json::makeInt(static_cast<int64_t>(results.size())));
    report.set("steps", std::move(steps));
    auto text = platform::json::stringify(platform::json::JsonValue(std::move(report)), /*pretty=*/true);
    return text.isOk() ? text.value() : std::string();
}

} // namespace ui_drive
