// tests/test_modules/test_module_toggle_bridge.cpp
// 模块开关两条函数指针缝的注入 / 路由 / 复位语义。
// 跨文件前提：
//   - 两条 handler 槽是进程级全局，跨用例、跨 TU 存活。test_main 的 clean-room 清单
//     每用例前无条件清槽，那只是最后一道防线；用例内仍须各自复位，否则前一个用例
//     残留的桩会污染后一个用例的调用计数断言。
//   - 复位必须走 RAII，不能写在用例尾部：doctest 断言失败即 `return false`（fail-fast），
//     尾部语句被跳过，桩就留在那儿给同进程后续用例。
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

// 复位守卫：任何退出路径都把**两条桥**的槽一并回厂。不能改成「用例尾部显式复位」——
// 断言在复位前 fail-fast 早退时尾部被跳过，桩残留给后续用例 / 后续 TU。
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
    DOCTEST_CHECK_EQ(g_rec.calls, 1);
    return true;
})

// 第二桥：claude 惰性装配缝
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
    // 中途复位本身就是被断言的一部分（验 no-op 语义），不是冗余：删了就不再覆盖
    // 「装好后可安全回厂」这条契约
    app::toggle::setClaudeAssemblyHandler(nullptr);   // 复位默认 no-op：不崩即通过
    app::toggle::ensureClaudeServiceAssembled(false);
    DOCTEST_CHECK_EQ(g_asmCalls, 1);
    return true;
})
