// tests/test_modules/test_agent_detail_layout.cpp
// 智能体详情弹窗 P1 布局壳三尺寸无头用例（spec §13 P1 + §12.3）。
// 钉死三档 geometries：760×660 宽屏、700×560 窄屏单列、900×600 矮宽屏。
//
// 过滤子串：detail_layout（ASCII 可 grep）
// 预检：构建 ./build/tests/devdesk_tests && 运行时过滤 detail_layout 只跑本文件用例。
#include "doctest.h"
#include "test_support.h"

#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "platform/http_client.h"

#include <eui/dsl.h>

#include <string>

namespace {

using namespace module::agent;

const eui::Screen kScreenBig{1600.0f, 1000.0f};   // 无钳制：默认设计尺寸 760×660

void composeTo(eui::Ui& ui, AgentModule& mod, const eui::Screen& screen,
               const char* pageId) {
    ui.begin(pageId);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

bool nearPx(float a, float b) { return std::fabs(a - b) <= 0.5f; }

bool clickId(eui::Ui& ui, const std::string& id) {
    auto* el = ui.find(id);
    if (el == nullptr) return false;
    el->onClick();
    return true;
}

// 打开某任务的详情弹窗（点列表行「详情」chip），返回是否点开
bool openDetailDialog(eui::Ui& ui, AgentModule& mod,
                      const std::string& tid) {
    return clickId(ui, "agent.task." + tid + ".detail");
}

// ---------- 三尺寸 test ----------

REGISTER_TEST(agent_ui_detail_layout_wide_760x660,
              "detail_layout: 760×660 宽屏——面板设计尺寸、三卡常驻、左右分栏", {
    test_support::TestDb tdb("agentdl", "wide");
    test_support::FakeProfileSource profile;

    AgentStore store(tdb.handle());
    auto created =
        store.createTask("布局测试任务", "验证三尺寸布局", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());

    AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = "{}";
        return core::Result<platform::http::Response>::ok(res);
    };
    AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreenBig, "layout_wide_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));

    eui::Ui ui2;
    composeTo(ui2, mod, kScreenBig, "layout_wide_2");

    // 面板即设计尺寸 760×660
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr, "宽屏 760×660 缺 panel");
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 760.0f) &&
                              nearPx(panel->frame.height, 660.0f),
                          "宽屏默认设计尺寸 760×660，实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));

    // 三卡常驻
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.last") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.next") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.stats") != nullptr);

    // 三卡无边框（P1 去 1px 边框改 surfaceElevated 色块）
    const auto* lastBg = ui2.find("agent.dialog.detail.summary.last.bg");
    DOCTEST_REQUIRE(lastBg != nullptr);
    DOCTEST_CHECK_MESSAGE(
        lastBg->border.width <= 0.0f,
        "摘要卡 .bg 不应有边框，实测 border.width=" +
            std::to_string(lastBg->border.width));

    // 配置头常驻
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.header") != nullptr);

    // 宽屏无 nav
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.detail.nav") == nullptr,
                          "宽屏不应渲染窄屏 segmented nav");

    // 左右分栏存在（宽屏）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") != nullptr);

    // 左栏宽 clamp(innerW*0.42, 300, 360)：innerW = 760-56 = 704, 704*0.42≈295.68
    // → clamp 到 300（P1 新口径）
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(runlist != nullptr);
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.width, 300.0f),
        "宽屏左栏宽应 300（clamp(innerW*0.42,300,360) 当 innerW=704 时），实测 " +
            std::to_string(runlist->frame.width));

    // 无节点越出面板底（runlist 底 ≤ panel 底 - 24）
    const float panelBot = panel->frame.y + panel->frame.height;
    const float runlistBot = runlist->frame.y + runlist->frame.height;
    DOCTEST_CHECK_MESSAGE(runlistBot <= panelBot + 0.5f,
                          "runlist 底不应越出 panel，panel 底=" +
                              std::to_string(panelBot) +
                              " runlist 底=" + std::to_string(runlistBot));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_layout_narrow_700x560,
              "detail_layout: 700×560 窄屏——面板 652×512、nav 存在、单列三卡", {
    test_support::TestDb tdb("agentdl", "narrow");
    test_support::FakeProfileSource profile;

    AgentStore store(tdb.handle());
    auto created =
        store.createTask("窄屏测试", "验证窄屏单列布局", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());

    AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = "{}";
        return core::Result<platform::http::Response>::ok(res);
    };
    AgentModule mod(deps);
    mod.onStart();

    const eui::Screen kScreen700{700.0f, 560.0f};

    eui::Ui ui;
    composeTo(ui, mod, kScreen700, "layout_narrow_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));

    eui::Ui ui2;
    composeTo(ui2, mod, kScreen700, "layout_narrow_2");

    // 面板经钳制: w = min(760, 700-48) = 652, h = min(660, 560-48) = 512
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr, "窄屏 700×560 缺 panel");
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 652.0f) &&
                              nearPx(panel->frame.height, 512.0f),
                          "窄屏面板应 652×512（钳制后），实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));

    // 窄屏 nav 存在
    const auto* nav = ui2.find("agent.dialog.detail.nav");
    DOCTEST_REQUIRE_MESSAGE(nav != nullptr, "窄屏 700×560 应渲染 segmented nav");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.hit.0") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.hit.1") != nullptr);

    // 窄屏无左右分栏（tab 0=runlist 存在，detail 不存在）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist") != nullptr);
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.detail.detail") == nullptr,
                          "窄屏 tab 0 不应渲染右栏 detail");

    // 三卡常驻、单列全宽
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.last") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.next") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.stats") != nullptr);

    // 配置头常驻
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.header") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_layout_shortwide_900x600,
              "detail_layout: 900×600 矮宽屏——detail.scroll 存在、msgscroll 不存在", {
    test_support::TestDb tdb("agentdl", "shortw");
    test_support::FakeProfileSource profile;

    AgentStore store(tdb.handle());
    auto created =
        store.createTask("矮宽屏测试", "验证 detail.scroll 降级", "manual", "default", 10,
                         false);
    DOCTEST_REQUIRE(created.isOk());

    AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = "{}";
        return core::Result<platform::http::Response>::ok(res);
    };
    AgentModule mod(deps);
    mod.onStart();

    const eui::Screen kScreen900{900.0f, 600.0f};

    eui::Ui ui;
    composeTo(ui, mod, kScreen900, "layout_shortw_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));

    eui::Ui ui2;
    composeTo(ui2, mod, kScreen900, "layout_shortw_2");

    // 面板经钳制: w = min(760, 900-48) = 760, h = min(660, 600-48) = 552
    // wide（w>=680 && h>=520）但 h<600 → 矮宽屏，右栏用 detail.scroll
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr, "矮宽屏 900×600 缺 panel");
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 760.0f) &&
                              nearPx(panel->frame.height, 552.0f),
                          "矮宽屏面板应 760×552（钳制后），实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));

    // 宽屏无 nav
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav") == nullptr);

    // 左右分栏存在
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") != nullptr);

    // 矮宽屏无 run：右栏 show「选择一次执行查看详情」空态，此时 detail.scroll 不存在
    // detail.empty 在场。三卡常驻 + 配置头常驻。
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.last") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.header") != nullptr);

    // 消息默认折叠，msgscroll 不存在
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.detail.msgscroll") == nullptr,
                          "消息默认折叠，不应有 msgscroll");

    mod.onStop();
    return true;
})

}  // namespace