// tests/test_modules/test_terminal_controller.cpp
// 三态状态机单测（技术卡 §9）：fake TerminalBackend 注入 fail/exit/wake 序列。
// 同接口双用（卡 §9：fake backend 也充当占位页开发期桩）。
#include "doctest.h"
#include "module_terminal/terminal_session_controller.h"

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
    int scrollbackCount() override { return 0; }
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

REGISTER_TEST(controllerThreeStates,
              "terminal controller: startNew→Running→exit→Exited(N)", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    DOCTEST_CHECK(c.state() == terminal::SessionState::Idle);

    c.startNew();
    DOCTEST_CHECK(c.state() == terminal::SessionState::Running);
    DOCTEST_CHECK(backend.last != nullptr);

    // generation 不变 → poll 不拉帧
    c.poll();
    DOCTEST_CHECK(c.frame().rows == 0);
    // generation 变化 → 拉帧
    backend.last->generation = 1;
    backend.last->nextFrame.rows = 10;
    backend.last->nextFrame.cols = 80;
    c.poll();
    DOCTEST_CHECK(c.frame().rows == 10);
    DOCTEST_CHECK(c.frame().cols == 80);

    // 子进程退出 → Exited(N)
    backend.last->exitedValue = true;
    backend.last->exitCodeValue = 2;
    backend.last->generation = 2;
    c.poll();
    DOCTEST_CHECK(c.state() == terminal::SessionState::Exited);
    DOCTEST_CHECK(c.exitCode() == 2);
    return true;
})

REGISTER_TEST(controllerFailRetry,
              "terminal controller: spawn 失败→Failed 可重试回 Running", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    backend.failSpawn = true;
    c.startNew();
    DOCTEST_CHECK(c.state() == terminal::SessionState::Failed);
    DOCTEST_CHECK(c.errorMessage().find("注入失败") != std::string::npos);

    backend.failSpawn = false;
    c.startNew();
    DOCTEST_CHECK(c.state() == terminal::SessionState::Running);
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
    c.startNew();
    ::unsetenv("DEVDESK_TERMINAL_SHELL");
    DOCTEST_CHECK(c.state() == terminal::SessionState::Failed);
    DOCTEST_CHECK(c.errorMessage().find("/nonexistent") == std::string::npos ||
                  c.errorMessage().size() > 0);  // 消息非空即可达（原文含核心 PTYError 描述）
    DOCTEST_CHECK(!c.errorMessage().empty());
    c.stop();
    DOCTEST_CHECK(c.state() == terminal::SessionState::Idle);
    return true;
})
#endif

REGISTER_TEST(controllerStopGuards,
              "terminal controller: write/resize 门控与 stop→Idle", {
    FakeBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);

    // 未启动：write/resize 不触达
    c.writeToSession(reinterpret_cast<const uint8_t*>("x"), 1);
    c.resizeGrid(24, 80);
    DOCTEST_CHECK(backend.last == nullptr);

    c.startNew();
    DOCTEST_CHECK(backend.last != nullptr);
    c.writeToSession(reinterpret_cast<const uint8_t*>("ls"), 2);
    // 启动时已预置 24×80（前述 resizeGrid 预置语义）→ 同值幂等跳过；改值才触达
    c.resizeGrid(30, 100);
    DOCTEST_CHECK(backend.last->writeCount == 1);
    DOCTEST_CHECK(backend.last->resizeCount == 1);
    DOCTEST_CHECK(backend.last->frame_.rows == 30);
    DOCTEST_CHECK(backend.last->frame_.cols == 100);

    const int terminatedBefore = FakeHandle::terminatedTotal;
    c.stop();
    DOCTEST_CHECK(c.state() == terminal::SessionState::Idle);
    // stop → terminate + 句柄释放（静态计数：实例析构后 backend.last 悬空禁读）
    DOCTEST_CHECK(FakeHandle::terminatedTotal == terminatedBefore + 1);
    DOCTEST_CHECK(FakeHandle::destroyedCount >= 1);
    return true;
})
