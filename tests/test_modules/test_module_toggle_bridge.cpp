// tests/test_modules/test_module_toggle_bridge.cpp
// 模块开关即时生效桥（module_toggle_bridge.h）单测：函数指针缝的注入 /
// 路由 / 复位语义。handler 为纯函数指针（无捕获），用例间无悬垂风险——
// 复位三层：① 用例体内显式复位；② 断言在复位前失败时由作用域守卫兜底
//（BridgeHandlerGuard，两条桥都回厂，审计 SYS-04）；③ test_main clean-room
// 清单第 14 项每用例前无条件清两条槽（进程级函数指针的最终防线）。
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

// 复位守卫（审计 SYS-04）：doctest 断言失败即 `return false`（fail-fast），
// 写在用例尾部的复位会被跳过 → 桩残留到后续用例 / 后续 TU。守卫放析构，
// 任何退出路径都把**两条桥**的槽一并回厂（形态照 test_app_assembly 的
// LazyAssemblyGuard）。test_main 的 clean-room 清单第 14 项是另一层兜底
//（每用例前无条件清槽）。
struct BridgeHandlerGuard {
    BridgeHandlerGuard() = default;   // 声明 deleted 拷贝会抑制隐式默认构造，显式补回
    ~BridgeHandlerGuard() {
        app::toggle::setToggleHandler(nullptr);
        app::toggle::setClaudeAssemblyHandler(nullptr);
    }
    BridgeHandlerGuard(const BridgeHandlerGuard&) = delete;
    BridgeHandlerGuard& operator=(const BridgeHandlerGuard&) = delete;
};
} // namespace

REGISTER_TEST(toggle_bridge_handler_roundtrip,
              "toggle_bridge: 注入 handler 后 onModuleToggled 路由到它；复位后回默认", {
    BridgeHandlerGuard handlerGuard;   // 先于任何断言构造：fail-fast 早退也回厂
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
// 同函数指针缝形态；复位纪律见文件头三层（本文件两条用例共用
// BridgeHandlerGuard——任一条 fail-fast 都不会把桩留给后续用例 / 后续 TU）。
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
    BridgeHandlerGuard handlerGuard;   // 先于任何断言构造
    g_asmCalls = 0;
    g_asmLastAutoStart = false;
    app::toggle::setClaudeAssemblyHandler(&recordAssembly);
    app::toggle::ensureClaudeServiceAssembled(true);
    DOCTEST_CHECK_EQ(g_asmCalls, 1);
    DOCTEST_CHECK(g_asmLastAutoStart);
    // 中途复位 = 被断言的一部分（验 no-op 语义），保留；守卫仍兜 fail-fast 早退
    app::toggle::setClaudeAssemblyHandler(nullptr);   // 复位默认 no-op：不崩即通过
    app::toggle::ensureClaudeServiceAssembled(false);
    DOCTEST_CHECK_EQ(g_asmCalls, 1);          // no-op 不再进 record
    return true;
})
