// tests/test_modules/test_terminal_web_bridge.cpp
// need#490 W4 会话桥：尺寸钳制/桌面尺寸恢复/tab 同步/退出态/设置单一真源与目录校验。
#include "doctest.h"
#include "module_terminal/terminal_session_controller.h"
#include "module_terminal/web/web_session_bridge.h"

#include <filesystem>
#include <string>
#include <vector>

#include "test_support.h"

namespace {

class BridgeFakeHandle : public platform::TerminalSessionHandle {
public:
    int generation = 0;
    int exitCodeValue = 0;
    bool exitedValue = false;
    platform::TerminalFrame frame;

    core::Result<void> write(const uint8_t*, int32_t, bool) override {
        return core::Result<void>::ok();
    }
    core::Result<void> resize(const platform::TerminalSize& size) override {
        frame.rows = size.rows;
        frame.cols = size.cols;
        return core::Result<void>::ok();
    }
    void terminate() override {}
    bool exited(int32_t* code) override {
        *code = exitCodeValue;
        return exitedValue;
    }
    platform::TerminalFrame poll() override { return frame; }
    int pendingGeneration() override { return generation; }
    int scrollbackCount() override { return 0; }
    bool scrollbackLine(int, std::string&) override { return false; }
    bool selectionText(int, int, int, int, std::string&) override { return false; }
};

class BridgeFakeBackend : public platform::TerminalBackend {
public:
    BridgeFakeHandle* last = nullptr;
    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&, const std::string&) override {
        auto h = std::make_unique<BridgeFakeHandle>();
        last = h.get();
        return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::ok(
            std::move(h));
    }
    bool available() const override { return true; }
    const char* unavailableReason() const override { return ""; }
};

struct Fixture {
    BridgeFakeBackend backend;
    terminal::SessionController controller;
    terminal::TerminalSettings settings;
    int saveCalls = 0;
    int repaintCalls = 0;
    bool saveFails = false;
    std::vector<terminal::web::WebEvent> events;

    Fixture() {
        controller.setBackend(&backend);
        bridge = std::make_unique<terminal::web::WebSessionBridge>(
            controller, &settings,
            [this](const terminal::TerminalSettings&) {
                saveCalls++;
                if (saveFails) {
                    return core::Result<void>::fail(core::ErrorCode::Internal, "注入保存失败");
                }
                return core::Result<void>::ok();
            },
            [this] { repaintCalls++; });
        bridge->setListener([this](const terminal::web::WebEvent& ev) { events.push_back(ev); });
    }

    std::unique_ptr<terminal::web::WebSessionBridge> bridge;
};

} // namespace

REGISTER_TEST(webBridgeClampBoundaries,
              "web bridge: 尺寸钳制 20–200 列 / 8–200 行（桌面路径不经此函数）", {
    using namespace terminal::web;
    auto lo = clampWebTerminalSize(1, 1);
    DOCTEST_CHECK_EQ(lo.rows, kWebMinRows);
    DOCTEST_CHECK_EQ(lo.cols, kWebMinCols);
    DOCTEST_CHECK(lo.clamped);

    auto hi = clampWebTerminalSize(1000, 1000);
    DOCTEST_CHECK_EQ(hi.rows, kWebMaxRows);
    DOCTEST_CHECK_EQ(hi.cols, kWebMaxCols);

    auto mid = clampWebTerminalSize(24, 80);
    DOCTEST_CHECK_EQ(mid.rows, 24);
    DOCTEST_CHECK_EQ(mid.cols, 80);
    DOCTEST_CHECK(!mid.clamped);

    // 任务书口径：10/1000 落 20/200
    auto task = clampWebTerminalSize(24, 10);
    DOCTEST_CHECK_EQ(task.cols, 20);
    auto task2 = clampWebTerminalSize(24, 1000);
    DOCTEST_CHECK_EQ(task2.cols, 200);
    return true;
})

REGISTER_TEST(webBridgeTabSync,
              "web bridge: tab 列表/新建/关闭/激活共享同一 SessionController", {
    using namespace terminal::web;
    Fixture f;
    const int a = f.bridge->createTab();
    const int b = f.bridge->createTab();
    DOCTEST_CHECK_EQ(a, 0);
    DOCTEST_CHECK_EQ(b, 1);
    auto tabs = f.bridge->tabs();
    DOCTEST_CHECK_EQ(tabs.size(), 2u);
    DOCTEST_CHECK(tabs[1].active);
    DOCTEST_CHECK_EQ(tabs[1].state, std::string("running"));

    DOCTEST_CHECK(f.bridge->activateTab(0));
    DOCTEST_CHECK_EQ(f.controller.activeIndex(), 0);
    DOCTEST_CHECK(f.bridge->tabs()[0].active);

    DOCTEST_CHECK(f.bridge->closeTab(0));
    DOCTEST_CHECK_EQ(f.controller.count(), 1);
    DOCTEST_CHECK_EQ(f.bridge->tabs().size(), 1u);
    return true;
})

REGISTER_TEST(webBridgeExitState,
              "web bridge: 子进程退出 → Exit 事件带 exitCode（只发一次）", {
    using namespace terminal::web;
    Fixture f;
    f.bridge->createTab();
    f.events.clear();             // createTab 自身发 Tabs，本用例只看退出事件
    f.controller.pollAll();       // 首帧记 Running
    f.bridge->pollStates();
    DOCTEST_CHECK(f.events.empty());

    f.backend.last->exitedValue = true;
    f.backend.last->exitCodeValue = 3;
    f.controller.pollAll();
    f.bridge->pollStates();
    DOCTEST_CHECK_EQ(f.events.size(), 1u);
    DOCTEST_CHECK(f.events[0].type == WebEventType::Exit);
    DOCTEST_CHECK_EQ(f.events[0].exitCode, 3);

    f.bridge->pollStates();  // 状态不再变 → 不重复发
    DOCTEST_CHECK_EQ(f.events.size(), 1u);
    return true;
})

REGISTER_TEST(webBridgeDesktopRestart,
              "web bridge: 无会话启动态 → DesktopRestart(reason=desktop_restart)", {
    using namespace terminal::web;
    Fixture f;
    f.bridge->markDesktopRestart();
    DOCTEST_CHECK_EQ(f.events.size(), 1u);
    DOCTEST_CHECK(f.events[0].type == WebEventType::DesktopRestart);
    DOCTEST_CHECK_EQ(f.events[0].reason, std::string("desktop_restart"));
    return true;
})

REGISTER_TEST(webBridgeSizeRestore,
              "web bridge: 网页 resize 钳制生效；最后订阅退出恢复桌面尺寸", {
    using namespace terminal::web;
    Fixture f;
    f.bridge->createTab();
    f.bridge->addSubscriber();
    f.bridge->observeDesktopResize(160, 50);
    f.controller.resizeActive(160, 50);

    int appliedRows = 0;
    int appliedCols = 0;
    DOCTEST_CHECK(f.bridge->webResize(1000, 1000, &appliedRows, &appliedCols));
    DOCTEST_CHECK_EQ(appliedRows, 200);
    DOCTEST_CHECK_EQ(appliedCols, 200);
    DOCTEST_CHECK_EQ(f.controller.active()->rows, 200);

    f.bridge->removeSubscriber();
    DOCTEST_CHECK_EQ(f.controller.active()->rows, 160);  // 恢复桌面 160×50，非网页 200
    DOCTEST_CHECK_EQ(f.controller.active()->cols, 50);
    return true;
})

REGISTER_TEST(webBridgeSettingsSingleSource,
              "web bridge: writeSettings 校验目录→写内存→持久化→广播/重绘（失败回滚）", {
    using namespace terminal::web;
    Fixture f;
    const std::filesystem::path dir = test_temp::tempPath("terminal_web_bridge_dir");
    std::filesystem::create_directories(dir);

    terminal::TerminalSettings next;
    next.startupDir = dir.string();
    next.shell = terminal::terminalShellOptionValues().back();
    auto ok = f.bridge->writeSettings(next);
    DOCTEST_CHECK(ok.isOk());
    DOCTEST_CHECK_EQ(f.settings.startupDir, dir.string());
    DOCTEST_CHECK_EQ(f.saveCalls, 1);
    DOCTEST_CHECK_EQ(f.repaintCalls, 1);
    DOCTEST_CHECK_EQ(f.events.size(), 1u);
    DOCTEST_CHECK(f.events[0].type == WebEventType::Settings);

    // 非法目录：拒绝且不改内存/不落库
    terminal::TerminalSettings bad = next;
    bad.startupDir = "Z:/definitely/not/exist/terminal_dir";
    auto rejected = f.bridge->writeSettings(bad);
    DOCTEST_CHECK(rejected.isFail());
    DOCTEST_CHECK_EQ(f.settings.startupDir, dir.string());
    DOCTEST_CHECK_EQ(f.saveCalls, 1);

    // 持久化失败：回滚内存
    f.saveFails = true;
    terminal::TerminalSettings next2 = next;
    next2.fontSize = 20;
    auto failed = f.bridge->writeSettings(next2);
    DOCTEST_CHECK(failed.isFail());
    DOCTEST_CHECK_EQ(f.settings.fontSize, next.fontSize);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return true;
})
