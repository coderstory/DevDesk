// tests/test_modules/test_ui_driver_report.cpp
// 债-18 UI 驱动入口：driver_report.h 无头单测——退出码聚合 + 明细格式化 +
// JSON 报告往返。
#include "doctest.h"
#include "ui_driver/driver_report.h"

#include "platform/json.h"

using namespace ui_drive;

namespace {

StepResult makeResult(int index, bool passed) {
    StepResult r;
    r.index = index;
    r.type = "click";
    r.target = "todo.btn.add";
    r.passed = passed;
    r.elapsedMs = 12.5;
    return r;
}

} // namespace

REGISTER_TEST(uiDriverReportAggregateAllPass, "uidrv/report: 全过 → 0", {
    std::vector<StepResult> results = {makeResult(1, true), makeResult(2, true)};
    DOCTEST_CHECK_EQ(aggregateExitCode(results, /*reportWriteOk=*/true), 0);
    return true;
})

REGISTER_TEST(uiDriverReportAggregateAnyFail, "uidrv/report: 有步骤失败 → 1", {
    std::vector<StepResult> results = {makeResult(1, true), makeResult(2, false)};
    DOCTEST_CHECK_EQ(aggregateExitCode(results, true), 1);
    return true;
})

REGISTER_TEST(uiDriverReportAggregateWriteFail, "uidrv/report: 报告/截图落盘失败 → 1（即使全过）", {
    std::vector<StepResult> results = {makeResult(1, true)};
    DOCTEST_CHECK_EQ(aggregateExitCode(results, /*reportWriteOk=*/false), 1);
    return true;
})

REGISTER_TEST(uiDriverReportAggregateZeroSteps, "uidrv/report: 0 步 → 2（防假绿兜底）", {
    std::vector<StepResult> results;
    DOCTEST_CHECK_EQ(aggregateExitCode(results, true), 2);
    return true;
})

REGISTER_TEST(uiDriverReportFormatStepLine, "uidrv/report: stdout 逐步中文明细格式", {
    StepResult pass = makeResult(3, true);
    pass.hitId = "todo.btn.add.bg";
    const std::string passLine = formatStepLine(pass, 7);
    DOCTEST_CHECK(passLine.find("[3/7]") != std::string::npos);
    DOCTEST_CHECK(passLine.find("PASS") != std::string::npos);
    DOCTEST_CHECK(passLine.find("click") != std::string::npos);
    DOCTEST_CHECK(passLine.find("todo.btn.add") != std::string::npos);
    DOCTEST_CHECK(passLine.find("命中回读: todo.btn.add.bg") != std::string::npos);

    StepResult fail = makeResult(5, false);
    fail.type = "wait";
    fail.target = "todo.item.x";
    fail.errorReason = "等待 'exists' 超时 5000ms";
    fail.screenshotPath = "shots/fail-5.png";
    const std::string failLine = formatStepLine(fail, 7);
    DOCTEST_CHECK(failLine.find("[5/7]") != std::string::npos);
    DOCTEST_CHECK(failLine.find("FAIL") != std::string::npos);
    DOCTEST_CHECK(failLine.find("等待 'exists' 超时 5000ms") != std::string::npos);
    DOCTEST_CHECK(failLine.find("shots/fail-5.png") != std::string::npos);
    return true;
})

REGISTER_TEST(uiDriverReportJsonRoundTrip, "uidrv/report: --report-json 结构化明细可解析往返", {
    std::vector<StepResult> results = {makeResult(1, true), makeResult(2, false)};
    results[1].errorReason = "断言失败：元素 'a' 不存在";
    const std::string json = buildReportJson(results, "demo.json", 1);
    DOCTEST_CHECK_FALSE(json.empty());
    auto parsed = platform::json::parse(json);
    DOCTEST_CHECK(parsed.isOk());
    DOCTEST_CHECK(parsed.value().isObject());
    const platform::json::JsonObject obj = parsed.value().asObject();
    DOCTEST_CHECK(obj.get("scene").asString() == "demo.json");
    DOCTEST_CHECK(obj.get("exitCode").asInt() == 1);
    DOCTEST_CHECK(obj.get("total").asInt() == 2);
    DOCTEST_CHECK(obj.get("steps").isArray());
    DOCTEST_CHECK(obj.get("steps").size() == 2);
    const platform::json::JsonObject first = obj.get("steps")[0].asObject();
    DOCTEST_CHECK(first.get("index").asInt() == 1);
    DOCTEST_CHECK(first.get("passed").asBool());
    DOCTEST_CHECK(first.get("type").asString() == "click");
    const platform::json::JsonObject second = obj.get("steps")[1].asObject();
    DOCTEST_CHECK_FALSE(second.get("passed").asBool());
    DOCTEST_CHECK(second.get("error").asString().find("断言失败") != std::string::npos);
    return true;
})
