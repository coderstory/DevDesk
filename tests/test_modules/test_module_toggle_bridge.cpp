// tests/test_modules/test_module_toggle_bridge.cpp
// 模块开关即时生效桥（module_toggle_bridge.h）单测：函数指针缝的注入 /
// 路由 / 复位语义。handler 为纯函数指针（无捕获），用例间无悬垂风险——
// 复位纪律：每用例结束 setToggleHandler(nullptr)；若断言在复位前失败，
// 后续用例首行 set 新 handler 覆盖。
#include "doctest.h"
#include "module_toggle_bridge.h"
#include <string>

namespace {
struct Recording {
    std::string lastId; bool lastOn = false; int calls = 0;
};
Recording g_rec;
void record(const std::string& id, bool on) {
    g_rec.calls++; g_rec.lastId = id; g_rec.lastOn = on;
}
} // namespace

REGISTER_TEST(toggle_bridge_handler_roundtrip,
              "toggle_bridge: 注入 handler 后 onModuleToggled 路由到它；复位后回默认", {
    app::toggle::setToggleHandler(&record);
    app::toggle::onModuleToggled("agent", true);
    DOCTEST_CHECK_EQ(g_rec.calls, 1);
    DOCTEST_CHECK_EQ(g_rec.lastId, std::string("agent"));
    DOCTEST_CHECK(g_rec.lastOn);
    app::toggle::setToggleHandler(nullptr);   // 复位默认 no-op：不崩即通过
    app::toggle::onModuleToggled("agent", false);
    DOCTEST_CHECK_EQ(g_rec.calls, 1);         // no-op 不再进 record
    return true;
})

// ---- 第二桥（2026-10-01 惰性装配计划 T1）：claude 惰性装配缝 ----
// 同函数指针缝形态；复位纪律同行首注——每用例结束 setClaudeAssemblyHandler
// (nullptr)，断言在复位前失败时后续用例首行 set 新 handler 覆盖。
namespace {
int g_asmCalls = 0;
bool g_asmLastAutoStart = false;
void recordAssembly(bool autoStart) {
    g_asmCalls++;
    g_asmLastAutoStart = autoStart;
}
} // namespace

REGISTER_TEST(toggle_bridge_claude_assembly_seam,
              "toggle_bridge: ensureClaudeServiceAssembled 缝注入/路由/复位；默认 no-op 不崩", {
    g_asmCalls = 0;
    g_asmLastAutoStart = false;
    app::toggle::setClaudeAssemblyHandler(&recordAssembly);
    app::toggle::ensureClaudeServiceAssembled(true);
    DOCTEST_CHECK_EQ(g_asmCalls, 1);
    DOCTEST_CHECK(g_asmLastAutoStart);
    app::toggle::setClaudeAssemblyHandler(nullptr);   // 复位默认 no-op：不崩即通过
    app::toggle::ensureClaudeServiceAssembled(false);
    DOCTEST_CHECK_EQ(g_asmCalls, 1);          // no-op 不再进 record
    return true;
})
