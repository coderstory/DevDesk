// tests/test_modules/test_app_now_tool.cpp
// app.now 行为契约 + 新增工具（app.now / net.fetch）的模块注册往返：
//   - app.now：无参数、本地时区 JSON 快照（iso/date/time/中文星期/epoch 毫秒）；
//     date 与系统当天一致（跨午夜防抖：执行前后取到的系统日期都在合法集）；
//     epoch_ms 与用例内系统毫秒差在分钟级内（防跑挂的时钟/溢出）。
//   - 注册往返：onStart 后 registry 出现 app.now/net.fetch 且进 allSchemasJson
//     （AgentLoop 工具表与「可用工具」列表都按注册表动态收集，零硬编码），
//     onStop 后消失。
// 跨文件前提：
//   - 工具级失败一律 Result ok + isError=true + 中文说明；Result::fail 只留给
//     编程不变量破坏。
//   - ToolRegistry 是进程级单例，不在 test_main 的 resetCaseGlobals 清单里，
//     只能靠用例内 ToolGuard 兜底摘除（显式清理会漏 fail-fast 提前 return 的路径）。
#include "doctest.h"
#include "test_support.h"

#include "core/time.h"
#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/app_now_tool.h"
#include "platform/json.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

// 用例内独立取系统本地日期（YYYY-MM-DD）：与工具同源 core::time::localTime，
// 但独立执行一次「取当前时刻 → 格式化」，做外部比对而非复用工具输出
std::string systemToday() {
    std::tm local{};
    core::time::localTime(core::time::nowUnix(), &local);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return buf;
}

// ToolGuard 不可省：ToolRegistry 是进程级单例、不在 resetCaseGlobals 清单里，
// 用例内显式 unregister 会漏掉 fail-fast 提前 return 的路径（同 test_agent_tools.cpp 口径）
struct ToolGuard {
    std::vector<std::string> names;
    ToolGuard(std::initializer_list<std::string> toolNames) : names(toolNames) {}
    ~ToolGuard() {
        for (const auto& name : names) {
            core::ToolRegistry::instance().unregisterTool(name);
        }
    }
};

} // namespace

REGISTER_TEST(app_now_returns_local_time_json,
              "app.now: 返回本地时间 JSON（date=系统当天、中文星期、epoch 毫秒）", {
    auto tool = module::agent::makeAppNowTool();
    DOCTEST_REQUIRE(tool != nullptr);
    DOCTEST_CHECK_EQ(tool->name(), std::string("app.now"));
    DOCTEST_CHECK_FALSE(tool->isDestructive());
    const std::string schema = tool->schemaJson();
    DOCTEST_CHECK(schema.find("\"name\":\"app.now\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("input_schema") != std::string::npos);

    // 执行前后各取一次系统日期：工具执行恰跨午夜时 date 落在闭区间任一天都合法
    const std::string before = systemToday();
    auto r = tool->execute(core::ToolArgs{"{}"});
    const std::string after = systemToday();
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_REQUIRE(parsed.value().isObject());
    auto obj = parsed.value().asObject();

    DOCTEST_REQUIRE(obj.get("date").isString());
    const std::string date = obj.get("date").asString();
    DOCTEST_CHECK_MESSAGE(date == before || date == after,
                          "date(" + date + ") 与系统当天(" + before + "/" +
                              after + ")不符");

    DOCTEST_REQUIRE(obj.get("weekday").isString());
    const std::string weekday = obj.get("weekday").asString();
    bool weekdayOk = false;
    for (const char* w : {"星期日", "星期一", "星期二", "星期三", "星期四",
                          "星期五", "星期六"}) {
        if (weekday == w) weekdayOk = true;
    }
    DOCTEST_CHECK_MESSAGE(weekdayOk, "weekday 非中文星期名: " + weekday);

    // iso 以 date 开头、中段嵌 time、尾部带 ±HH:MM 偏移（注：日期自带连字符，
    // 不能用 find("-") 判偏移——按尾 6 字符形状判定）
    DOCTEST_REQUIRE(obj.get("iso").isString());
    const std::string iso = obj.get("iso").asString();
    DOCTEST_CHECK(iso.find(date) == 0);
    DOCTEST_REQUIRE(obj.get("time").isString());
    const std::string timeOfDay = obj.get("time").asString();
    DOCTEST_CHECK(iso.find("T" + timeOfDay) != std::string::npos);
    DOCTEST_REQUIRE(iso.size() >= 6);
    const size_t off = iso.size() - 6;
    DOCTEST_CHECK_MESSAGE(
        (iso[off] == '+' || iso[off] == '-') && iso[off + 3] == ':' &&
            isdigit(static_cast<unsigned char>(iso[off + 1])) &&
            isdigit(static_cast<unsigned char>(iso[off + 2])) &&
            isdigit(static_cast<unsigned char>(iso[off + 4])) &&
            isdigit(static_cast<unsigned char>(iso[off + 5])),
        "iso 尾部非 ±HH:MM 偏移形状，实测: " + iso);

    // epoch_ms 与用例内系统毫秒同源（分钟级容差足够防跑挂/溢出）
    DOCTEST_REQUIRE(obj.get("epoch_ms").isNumber());
    const int64_t epochMs = obj.get("epoch_ms").asInt();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    DOCTEST_CHECK_MESSAGE(epochMs > 0 && nowMs - epochMs >= 0 &&
                              nowMs - epochMs < 60000,
                          "epoch_ms(" + std::to_string(epochMs) +
                              ") 与系统毫秒(" + std::to_string(nowMs) +
                              ")偏差过大");
    return true;
})

REGISTER_TEST(app_now_empty_args_ok,
              "app.now: 空参数可执行、schema 无参（同 app.stats 口径）", {
    auto tool = module::agent::makeAppNowTool();
    auto r = tool->execute(core::ToolArgs{""});
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    return true;
})

// 注册/注销往返：onStart 注册 app.now + net.fetch（非破坏性只读）、进 allSchemasJson
// （工具表与「可用工具」列表均按注册表动态渲染，注册即自动出现，零白名单），onStop 消失。
REGISTER_TEST(agent_module_registers_app_now_net_fetch,
              "agent_module: onStart 注册 app.now/net.fetch、onStop 注销（生命周期）", {
    test_support::TestDb tdb("agenttools", "nownet");
    test_support::FakeProfileSource profile;
    ToolGuard guard{"app.now", "net.fetch"};   // 兜底摘除（含 fail-fast 路径）
    core::ToolRegistry::instance().unregisterTool("app.now");
    core::ToolRegistry::instance().unregisterTool("net.fetch");   // 起点干净

    const std::string schemasBefore =
        core::ToolRegistry::instance().allSchemasJson();

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    std::unique_ptr<::core::Module> mod(new module::agent::AgentModule(deps));

    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.now").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("net.fetch").isFail());
    mod->onStart();
    auto now = core::ToolRegistry::instance().find("app.now");
    DOCTEST_REQUIRE(now.isOk());
    DOCTEST_CHECK_EQ(now.value()->name(), std::string("app.now"));
    DOCTEST_CHECK_FALSE(now.value()->isDestructive());
    auto net = core::ToolRegistry::instance().find("net.fetch");
    DOCTEST_REQUIRE(net.isOk());
    DOCTEST_CHECK_EQ(net.value()->name(), std::string("net.fetch"));
    DOCTEST_CHECK_FALSE(net.value()->isDestructive());

    // 注册表 → AgentLoop 工具表（allSchemasJson 动态收集）与「可用工具」列表
    // （listRegisteredTools 同源）都会自动带上新工具
    const std::string schemas =
        core::ToolRegistry::instance().allSchemasJson();
    DOCTEST_CHECK_MESSAGE(schemas.find("app.now") != std::string::npos,
                          "allSchemasJson 未收录 app.now");
    DOCTEST_CHECK_MESSAGE(schemas.find("net.fetch") != std::string::npos,
                          "allSchemasJson 未收录 net.fetch");
    DOCTEST_CHECK_MESSAGE(schemasBefore.find("app.now") == std::string::npos,
                          "起点注册表已含 app.now（前置清理失效）");

    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("app.now").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance().find("net.fetch").isFail());
    DOCTEST_CHECK(core::ToolRegistry::instance()
                      .allSchemasJson()
                      .find("net.fetch") == std::string::npos);
    return true;
})
