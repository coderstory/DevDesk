// tests/test_modules/test_terminal_controller.cpp
// 多会话管理单测（need241 v2）：fake TerminalBackend 注入 fail/exit/wake 序列 +
// 多会话建/切/关路由语义。
#include "doctest.h"
#include "module_terminal/module_terminal.h"
#include "module_terminal/terminal_session_controller.h"

#include <cstring>

namespace {

class FakeHandle : public platform::TerminalSessionHandle {
public:
    static int destroyedCount;
    static int terminatedTotal;  // 静态累计：实例析构后仍可断言
    ~FakeHandle() override { destroyedCount++; }
    int generation = 0;
    int exitCodeValue = 0;
    bool exitedValue = false;
    int writeCount = 0;
    int resizeCount = 0;
    int terminateCount = 0;
    platform::TerminalFrame nextFrame;
    platform::TerminalFrame frame_;
    int scrollbackN = 0;

    core::Result<void> write(const uint8_t*, int32_t, bool) override {
        writeCount++;
        return core::Result<void>::ok();
    }
    core::Result<void> resize(const platform::TerminalSize& size) override {
        resizeCount++;
        frame_.rows = size.rows;
        frame_.cols = size.cols;
        return core::Result<void>::ok();
    }
    void terminate() override {
        terminateCount++;
        terminatedTotal++;
    }
    bool exited(int32_t* code) override {
        *code = exitCodeValue;
        return exitedValue;
    }
    platform::TerminalFrame poll() override { return nextFrame; }
    int pendingGeneration() override { return generation; }
    int scrollbackCount() override { return scrollbackN; }
    bool scrollbackLine(int, std::string&) override { return false; }
    bool selectionText(int, int, int, int, std::string&) override { return false; }
};

class FakeBackend : public platform::TerminalBackend {
public:
    bool failSpawn = false;
    FakeHandle* last = nullptr;

    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&) override {
        if (failSpawn) {
            return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::fail(
                core::ErrorCode::Internal, "spawn 注入失败");
        }
        auto h = std::make_unique<FakeHandle>();
        last = h.get();
        return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::ok(
            std::move(h));
    }
    bool available() const override { return true; }
    const char* unavailableReason() const override { return ""; }
};

int FakeHandle::destroyedCount = 0;
int FakeHandle::terminatedTotal = 0;

} // namespace

REGISTER_TEST(controllerSessionLifecycle,
              "terminal controller: startSession→Running→exit→Exited(N)（单会话链路）", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    DOCTEST_CHECK(c.empty());
    DOCTEST_CHECK(c.activeIndex() == -1);

    const int idx = c.startSession();
    DOCTEST_CHECK(idx == 0);
    DOCTEST_CHECK(c.activeIndex() == 0);
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Running);
    DOCTEST_CHECK(backend.last != nullptr);
    DOCTEST_CHECK(c.at(0)->titleNumber == 1);

    // generation 不变 → poll 不拉帧
    c.pollAll();
    DOCTEST_CHECK(c.at(0)->frame.rows == 0);
    // generation 变化 → 拉帧
    backend.last->generation = 1;
    backend.last->nextFrame.rows = 10;
    backend.last->nextFrame.cols = 80;
    c.pollAll();
    DOCTEST_CHECK(c.at(0)->frame.rows == 10);
    DOCTEST_CHECK(c.at(0)->frame.cols == 80);

    // 子进程退出 → Exited(N)
    backend.last->exitedValue = true;
    backend.last->exitCodeValue = 2;
    backend.last->generation = 2;
    c.pollAll();
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Exited);
    DOCTEST_CHECK(c.at(0)->exitCode == 2);
    return true;
})

REGISTER_TEST(controllerFailThenStart,
              "terminal controller: spawn 失败会话 Failed 占位，后续会话不受牵连", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    backend.failSpawn = true;
    const int bad = c.startSession();
    DOCTEST_CHECK(bad == 0); // 失败也占位（TAB 圆点红 + 覆盖卡错误详情）
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Failed);
    DOCTEST_CHECK(c.at(0)->error.find("注入失败") != std::string::npos);

    backend.failSpawn = false;
    const int good = c.startSession();
    DOCTEST_CHECK(good == 1);
    DOCTEST_CHECK(c.activeIndex() == 1);
    DOCTEST_CHECK(c.at(1)->state == terminal::SessionState::Running);
    DOCTEST_CHECK(c.at(1)->titleNumber == 2); // 序号递增不复用
    return true;
})

REGISTER_TEST(controllerMultiSession,
              "terminal controller: 多会话切换/关闭右邻优先/关末位空态/scrollback 按会话记忆", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    c.startSession();
    c.startSession();
    c.startSession();
    DOCTEST_CHECK(c.count() == 3);
    DOCTEST_CHECK(c.activeIndex() == 2);

    // 每会话独立 scrollback 偏移记忆
    c.at(0)->scrollOffset = 5;
    c.at(1)->scrollOffset = 0;
    c.at(2)->scrollOffset = 3;
    c.setActive(0);
    c.setActive(1);
    c.setActive(2);
    DOCTEST_CHECK(c.at(0)->scrollOffset == 5);
    DOCTEST_CHECK(c.at(2)->scrollOffset == 3);

    // 写路由只达激活会话
    FakeHandle* h1 = backend.last;  // 会话 2 的句柄（startSession 顺序尾）
    c.setActive(1);
    c.writeToActive(reinterpret_cast<const uint8_t*>("ls"), 2);
    DOCTEST_CHECK(h1->writeCount == 0);

    // 关闭激活会话（idx1）→ 右邻（原 idx2）成为新激活
    FakeHandle* h2 = h1;
    c.closeSession(1);
    DOCTEST_CHECK(c.count() == 2);
    DOCTEST_CHECK(c.activeIndex() == 1);          // 右邻左移一位
    DOCTEST_CHECK(c.at(1)->titleNumber == 3);     // 原「终端 3」
    DOCTEST_CHECK(FakeHandle::terminatedTotal >= 1);

    // 关闭末位 → 激活左邻
    c.closeSession(1);
    DOCTEST_CHECK(c.count() == 1);
    DOCTEST_CHECK(c.activeIndex() == 0);

    // 关最后一个 → 空态
    const int destroyedBefore = FakeHandle::destroyedCount;
    c.closeSession(0);
    DOCTEST_CHECK(c.empty());
    DOCTEST_CHECK(c.activeIndex() == -1);
    DOCTEST_CHECK(FakeHandle::destroyedCount == destroyedBefore + 1);
    (void)h2;
    return true;
})

REGISTER_TEST(controllerCloseInactiveKeepsFocus,
              "terminal controller: 关非激活 TAB 不换焦点（左邻删=索引平移/右邻删=不动）"
              "+ setActive→write 正向路由", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    c.startSession();
    FakeHandle* h0 = backend.last;
    c.startSession();
    FakeHandle* h1 = backend.last;
    c.startSession();
    FakeHandle* h2 = backend.last;
    DOCTEST_CHECK(c.activeIndex() == 2);

    // setActive(1) → write 正向路由：只达会话 1
    c.setActive(1);
    c.writeToActive(reinterpret_cast<const uint8_t*>("a"), 1);
    DOCTEST_CHECK(h1->writeCount == 1);
    DOCTEST_CHECK(h0->writeCount == 0);
    DOCTEST_CHECK(h2->writeCount == 0);

    // 关非激活左邻（idx0）：焦点会话不变，仅索引随删左移一位
    c.closeSession(0);
    DOCTEST_CHECK(c.count() == 2);
    DOCTEST_CHECK(c.activeIndex() == 0);
    DOCTEST_CHECK(c.at(0)->titleNumber == 2); // 焦点会话=原「终端 2」（h1）
    c.writeToActive(reinterpret_cast<const uint8_t*>("b"), 1);
    DOCTEST_CHECK(h1->writeCount == 2);       // 仍路由原会话 1
    DOCTEST_CHECK(h2->writeCount == 0);

    // 关非激活右邻（现 idx1）：焦点不动
    c.closeSession(1);
    DOCTEST_CHECK(c.count() == 1);
    DOCTEST_CHECK(c.activeIndex() == 0);
    DOCTEST_CHECK(c.at(0)->titleNumber == 2);
    DOCTEST_CHECK(h2->terminateCount == 1);

    // 关激活末位 → 空态
    c.closeSession(0);
    DOCTEST_CHECK(c.empty());
    DOCTEST_CHECK(c.activeIndex() == -1);
    return true;
})

REGISTER_TEST(controllerRestartAndGuards,
              "terminal controller: restartSession 回 Running；无会话时写/resize 静默", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);

    // 无会话：write/resize 静默丢弃
    c.writeToActive(reinterpret_cast<const uint8_t*>("x"), 1);
    c.resizeActive(24, 80);
    DOCTEST_CHECK(backend.last == nullptr);

    c.startSession();
    c.resizeActive(30, 100);
    DOCTEST_CHECK(backend.last->resizeCount == 1);
    DOCTEST_CHECK(backend.last->frame_.rows == 30);
    DOCTEST_CHECK(backend.last->frame_.cols == 100);

    // Exited → restart 回 Running（同会话序号不换）；P3-1：回滚偏移/余量随重启清零
    const int title = c.at(0)->titleNumber;
    c.at(0)->scrollOffset = 7;
    c.at(0)->scrollRemainder = 2.5f;
    backend.last->exitedValue = true;
    backend.last->exitCodeValue = 1;
    backend.last->generation = 9;
    c.pollAll();
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Exited);
    c.restartSession(0);
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Running);
    DOCTEST_CHECK(c.at(0)->titleNumber == title);
    DOCTEST_CHECK(c.at(0)->scrollOffset == 0);
    DOCTEST_CHECK(c.at(0)->scrollRemainder == 0.0f);
    DOCTEST_CHECK(FakeHandle::terminatedTotal >= 1);
    return true;
})

#if defined(__APPLE__)
// F-07 失败注入（决策 8）：DEVDESK_TERMINAL_SHELL=/nonexistent → spawn 失败 → Failed
// 卡消息可达（真 mac 后端链路：env 注入 → resolveDefaultShell → 核心 PTYError）。
REGISTER_TEST(controllerMacSpawnInjection,
              "terminal controller: mac 真后端失败注入 env → Failed 消息可达", {
    ::setenv("DEVDESK_TERMINAL_SHELL", "/nonexistent", 1);
    FakeBackend realOwner;  // 占位满足编译面；真后端直接取单例
    terminal::SessionController c;
    c.setBackend(platform::terminalBackendMac());
    c.startSession();
    ::unsetenv("DEVDESK_TERMINAL_SHELL");
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Failed);
    DOCTEST_CHECK(!c.at(0)->error.empty());  // 消息非空即可达（原文含核心 PTYError 描述）
    c.closeSession(0);
    DOCTEST_CHECK(c.empty());
    return true;
})
#endif

// bug247（老板 2026-11-05：打开软件别默认创建终端）：onStart 不建会话（开机不占
// ConPTY）；onPageEnter 建首个且幂等；onStop 清空。会话 spawn 失败也占位数 1
//（Failed 占位语义），故计数断言与机器可用性解耦。
REGISTER_TEST(moduleTerminalNoSessionAtStart,
              "terminal module: onStart 不建会话；onPageEnter 建首个（bug247）", {
    // 复审 P2：注入 fake backend——缺省构造即接平台真后端，onPageEnter 会在 doctest
    // 里真实 spawn pwsh（ConPTY 进程泄漏 + 测试变慢）
    FakeBackend backend;
    terminal::TerminalModule m(&backend);
    DOCTEST_CHECK(m.sessionCount() == 0); // 未进页：0
    m.onStart();
    DOCTEST_CHECK(m.sessionCount() == 0); // 回归锚：开机不建
    m.onPageEnter();
    DOCTEST_CHECK(m.sessionCount() == 1); // 进页自动建第一个
    m.onPageEnter();
    DOCTEST_CHECK(m.sessionCount() == 1); // 幂等：不重复建
    m.onStop();
    DOCTEST_CHECK(m.sessionCount() == 0); // 停用清空
    return true;
})
