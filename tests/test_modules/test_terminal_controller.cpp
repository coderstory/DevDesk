// tests/test_modules/test_terminal_controller.cpp
// 多会话管理单测（need241 v2）：fake TerminalBackend 注入 fail/exit/wake 序列 +
// 多会话建/切/关路由语义。
#include "doctest.h"
#include "module_terminal/module_terminal.h"
#include "module_terminal/terminal_search.h"
#include "module_terminal/terminal_text_util.h"
#include "module_terminal/terminal_quit_guard.h"
#include "module_terminal/terminal_session_controller.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

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
    // bug249 输出/退出唤醒缝：与平台后端同形状（reader 线程调一次 → 宿主出这一帧）
    std::function<void()> notify_;
    int notifyCalls = 0;
    void setOutputNotify(std::function<void()> notify) override { notify_ = std::move(notify); }
    bool supportsOutputNotify() const override { return true; }
    void emitOutput() {  // 模拟 reader 线程：新输出/子进程退出后唤醒一次
        notifyCalls++;
        if (notify_) notify_();
    }
};

class FakeBackend : public platform::TerminalBackend {
public:
    bool failSpawn = false;
    FakeHandle* last = nullptr;

    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&, const std::string&) override {
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
    // 计数走静态累计：closeSession 已 reset handle → 上面的 h2 是悬垂指针，
    // 读它等于读已释放内存（曾在本机与 CI 上随机红/绿）。
    const int terminatedBefore = FakeHandle::terminatedTotal;
    c.closeSession(1);
    DOCTEST_CHECK(c.count() == 1);
    DOCTEST_CHECK(c.activeIndex() == 0);
    DOCTEST_CHECK(c.at(0)->titleNumber == 2);
    DOCTEST_CHECK(FakeHandle::terminatedTotal == terminatedBefore + 1);

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

// bug249/bug305（2026-11-18）：出帧策略。有 Running 会话之前用常驻 onFrame 泵兜底
// （EUI 契约 onFrame 每帧置 composeRequested → 空闲也 60fps 空转）；平台提供输出通知
// 路径后必须按需出帧（泵不挂）。无 Running 会话（含 exit 收割后）零帧。
REGISTER_TEST(terminalFramePumpPolicy,
              "terminal frames: 有输出通知路径不挂常驻泵；无 Running 会话零帧（bug249/305）", {
    DOCTEST_CHECK(terminal::needsFramePump(false, false) == false); // 空态零帧
    DOCTEST_CHECK(terminal::needsFramePump(false, true) == false);  // 空态零帧（有通知）
    DOCTEST_CHECK(terminal::needsFramePump(true, false) == true);   // 无通知路径→兜底泵
    DOCTEST_CHECK(terminal::needsFramePump(true, true) == false);   // 按需出帧：泵必须不挂
    return true;
})

// 唤醒链路：reader 输出/退出 → 唤醒出口（生产=core::platform::requestUiUpdate）；
// idle 反复 pollAll 不请求帧（空闲零出帧）；退出收割后 state=Exited 且无 Running。
REGISTER_TEST(controllerOutputWakeExit,
              "terminal controller: 输出/退出唤醒一次；idle 零请求；exit 后无 Running", {
    FakeBackend backend;
    terminal::SessionController c;
    int wakes = 0;
    c.setBackend(&backend);
    c.setOutputWake([&wakes] { wakes++; });
    c.startSession();
    DOCTEST_CHECK(c.outputNotifyAvailable());
    // idle（会话 Running、后端无新输出）：反复 pollAll 不产生任何帧请求
    for (int i = 0; i < 5; i++) c.pollAll();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    c.pollAll();
    DOCTEST_CHECK(wakes == 0);
    // reader 产出新输出：每批一次唤醒，消费（pollAll）后不续帧
    backend.last->emitOutput();
    DOCTEST_CHECK(wakes == 1);
    c.pollAll();
    DOCTEST_CHECK(wakes == 1);
    DOCTEST_CHECK(backend.last->notifyCalls == 1);
    // 子进程退出：后端 exited + 唤醒一次 → pollAll 迁移 Exited → 无 Running → 无泵
    backend.last->exitedValue = true;
    backend.last->emitOutput();
    c.pollAll();
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Exited);
    bool anyRunning = false;
    for (int i = 0; i < c.count(); i++) {
        if (c.at(i)->state == terminal::SessionState::Running) anyRunning = true;
    }
    DOCTEST_CHECK(anyRunning == false);
    DOCTEST_CHECK(terminal::needsFramePump(anyRunning, c.outputNotifyAvailable()) == false);
    DOCTEST_CHECK(terminal::needsIdleTick(anyRunning, c.outputNotifyAvailable()) == false);
    return true;
})

// bug305/306（2026-11-18）：Running 期间的低频自唤醒（有通知路径时）。它是「光标闪烁
// 相位」与「子进程退出巡检」的唯一帧源（ConPTY 退出不断管道，pollAll 靠这一帧发现退
// 出），且必须 ≤2fps——不得回到 60fps 常驻出帧（bug249 验收口径）。
REGISTER_TEST(terminalIdleTickPolicy,
              "terminal frames: Running+通知路径挂低频自唤醒（≤2fps），退出后零帧（bug305/306）",
              {
    DOCTEST_CHECK(terminal::needsIdleTick(false, true) == false);  // 空态/退出后零帧
    DOCTEST_CHECK(terminal::needsIdleTick(false, false) == false); // 空态零帧
    DOCTEST_CHECK(terminal::needsIdleTick(true, true) == true);    // 通知路径→低频帧
    DOCTEST_CHECK(terminal::needsIdleTick(true, false) == false);  // 无通知路径走常驻泵
    // 帧率硬上限：≤2fps（1000/550 ≈ 1.8 帧/s；60fps 的 16ms 档必须不成立）
    DOCTEST_CHECK(terminal::kIdleFrameTickMs >= 500);
    DOCTEST_CHECK(terminal::kIdleFrameTickMs <= 1000);
    // 两路永不并存：Running 时必有（且只有）一路帧源，否则闪烁/退出巡检断供
    DOCTEST_CHECK(!(terminal::needsFramePump(true, true) && terminal::needsIdleTick(true, true)));
    DOCTEST_CHECK((terminal::needsFramePump(true, false) || terminal::needsIdleTick(true, false)) ==
                  true);
    return true;
})

// ── 池#365：终端历史搜索纯函数 ─────────────────────────────────────────
namespace {
// ASCII 行 → cells 网格（行间按最长行补空格对齐，防 cols 越行读越界）
platform::TerminalFrame frameFromLines(const std::vector<std::string>& lines) {
    platform::TerminalFrame f;
    size_t cols = 0;
    for (const auto& l : lines) cols = std::max(cols, l.size());
    f.rows = static_cast<int>(lines.size());
    f.cols = static_cast<int>(cols);
    for (const auto& l : lines) {
        for (size_t i = 0; i < cols; ++i) {
            platform::TerminalCell c;
            c.cp = i < l.size() ? static_cast<uint32_t>(
                                      static_cast<unsigned char>(l[i]))
                                : static_cast<uint32_t>(' ');
            f.cells.push_back(c);
        }
    }
    return f;
}
} // namespace

REGISTER_TEST(terminalSearchCollectMatches,
              "terminal search: collectMatchLines 历史行+屏上行/大小写不敏感/空查询（池#365）",
              {
    using terminal::collectMatchLines;
    const std::vector<std::string> sb = {"hello world", "foo bar", "Hello again"};
    auto scrollbackLine = [&sb](int idx, std::string& out) {
        if (idx < 0 || idx >= static_cast<int>(sb.size())) return false;
        out = sb[static_cast<size_t>(idx)];
        return true;
    };
    const platform::TerminalFrame frame =
        frameFromLines({"tail line", "FOO tail"});
    const int sbCount = 3;
    const int total = sbCount + 2;  // 全局行号空间：0..2 历史 + 3..4 屏上

    // 历史行 + 大小写不敏感（hello / Hello 同命中）
    auto m1 = collectMatchLines("hello", total, sbCount, scrollbackLine, frame);
    DOCTEST_REQUIRE(m1.size() == 2);
    DOCTEST_CHECK(m1[0] == 0 && m1[1] == 2);
    // 历史小写 / 屏上大写同命中
    auto m2 = collectMatchLines("foo", total, sbCount, scrollbackLine, frame);
    DOCTEST_REQUIRE(m2.size() == 2);
    DOCTEST_CHECK(m2[0] == 1 && m2[1] == 4);
    // 屏上行内容来自 frame cells（rowSliceText 拼串）
    auto m3 = collectMatchLines("tail", total, sbCount, scrollbackLine, frame);
    DOCTEST_REQUIRE(m3.size() == 2);
    DOCTEST_CHECK(m3[0] == 3 && m3[1] == 4);
    // 空查询 / 无命中 → 空
    DOCTEST_CHECK(collectMatchLines("", total, sbCount, scrollbackLine, frame).empty());
    DOCTEST_CHECK(collectMatchLines("zzz", total, sbCount, scrollbackLine, frame).empty());
    // 历史行取不到（false）→ 空串参与，不中断整表扫描
    auto none = [](int, std::string&) { return false; };
    DOCTEST_CHECK(
        collectMatchLines("foo", 3, 3, none, platform::TerminalFrame{}).empty());
    return true;
})

REGISTER_TEST(terminalSearchScrollCenter,
              "terminal search: scrollOffsetToCenter 居中跳转与两端钳制（池#365）", {
    using terminal::scrollOffsetToCenter;
    // 100 行总量、视口 10 行、maxOffset=90：命中 50 → 顶行 45 → 偏移 45（居中）
    DOCTEST_CHECK_EQ(scrollOffsetToCenter(50, 100, 10, 90), 45);
    // 底部命中 → 负偏移钳 0（贴底）
    DOCTEST_CHECK_EQ(scrollOffsetToCenter(99, 100, 10, 90), 0);
    // 顶部命中 → 钳 maxOffset（滚到最老）
    DOCTEST_CHECK_EQ(scrollOffsetToCenter(0, 100, 10, 90), 90);
    // 无溢出（视口 ≥ 总量）→ 恒 0
    DOCTEST_CHECK_EQ(scrollOffsetToCenter(3, 5, 10, 0), 0);
    return true;
})

// ── bug406：后台建会话（进页帧不被 ConPTY 创建阻塞）───────────────────
namespace {
// spawn 可被闸门挂起的后端：用于断言 onPageEnter / startSessionAsync 在 spawn 完成前
// 返回（同步实现在此必挂死，即测试自身的红灯）。
class GateBackend : public platform::TerminalBackend {
public:
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;  // true=放行 spawn
    std::atomic<int> spawns{0};

    core::Result<std::unique_ptr<platform::TerminalSessionHandle>> spawn(
        const std::string&, const platform::TerminalSize&, const std::string&) override {
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [this] { return open; });
        }
        spawns.fetch_add(1);
        std::unique_ptr<platform::TerminalSessionHandle> h(new FakeHandle());
        return core::Result<std::unique_ptr<platform::TerminalSessionHandle>>::ok(std::move(h));
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mu);
            open = true;
        }
        cv.notify_all();
    }
    bool available() const override { return true; }
    const char* unavailableReason() const override { return ""; }
};

// UI 线程等价循环：生产由 compose → pollAll 收割；测试手动驱动（上限防挂死）
void waitSettled(terminal::SessionController& c, int index) {
    for (int i = 0; i < 400; i++) {
        c.pollAll();
        const terminal::Session* s = c.at(index);
        if (s != nullptr && s->state != terminal::SessionState::Starting) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace

// bug406（池#406，实测进页 14.4ms）：onPageEnter 不能同步建 ConPTY 会话。骨架态 =
// 会话已占位（Starting，页面出「正在启动终端…」）+ 句柄未接入；就绪后同帧接网格。
REGISTER_TEST(moduleTerminalEnterAsyncNonBlocking,
              "terminal module: onPageEnter 不等 spawn（Starting 骨架）；后台就绪转 Running（bug406）", 
              {
    GateBackend backend;
    terminal::TerminalModule m(&backend);
    m.onPageEnter(); // spawn 被闸门挂住 —— 同步实现会在本行挂死
    auto& c = m.controllerForTest();
    DOCTEST_CHECK(m.sessionCount() == 1);  // 占位即计数（骨架可渲染）
    DOCTEST_CHECK(c.at(0)->titleNumber == 1);
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Starting);
    DOCTEST_CHECK(c.at(0)->handle == nullptr); // 句柄尚未接入（同步路径的根本差异）
    DOCTEST_CHECK(c.at(0)->error.empty());

    backend.release();
    waitSettled(c, 0);
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Running);
    DOCTEST_CHECK(c.at(0)->handle != nullptr);

    // 幂等：Starting / Running 都是「已有会话」，二次进页不新增
    m.onPageEnter();
    DOCTEST_CHECK(m.sessionCount() == 1);
    m.onStop();
    DOCTEST_CHECK(m.sessionCount() == 0);
    return true;
})

// 失败必须可见（不静默）：后台 spawn 失败 → Failed 态 + error 文案（走既有覆盖卡）
REGISTER_TEST(moduleTerminalEnterAsyncFailVisible,
              "terminal module: 后台建会话失败 → Failed 带错误文案（不静默，bug406）", {
    FakeBackend backend;
    backend.failSpawn = true;
    terminal::TerminalModule m(&backend);
    m.onPageEnter();
    auto& c = m.controllerForTest();
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Starting); // 先骨架
    waitSettled(c, 0);
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Failed);
    DOCTEST_CHECK(c.at(0)->error.find("注入失败") != std::string::npos);
    DOCTEST_CHECK(c.at(0)->handle == nullptr);
    return true;
})

// 建会话期间被关闭：后台结果丢弃、句柄不接入（不留孤儿进程，不因索引平移错位）
REGISTER_TEST(controllerAsyncCloseDropsResult,
              "terminal controller: 建会话中途关闭 → 后台结果丢弃（bug406）", {
    GateBackend backend;
    terminal::SessionController c;
    c.setBackend(&backend);
    c.startSessionAsync();
    DOCTEST_CHECK(c.at(0)->state == terminal::SessionState::Starting);
    c.closeSession(0);
    DOCTEST_CHECK(c.empty());

    backend.release();
    for (int i = 0; i < 400 && backend.spawns.load() == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    DOCTEST_CHECK(backend.spawns.load() == 1);
    c.pollAll(); // 收割：无同位会话 → 丢弃
    DOCTEST_CHECK(c.empty());
    DOCTEST_CHECK(c.activeIndex() == -1);
    return true;
})

// Starting 必须有帧源：否则后台结果只能在用户下一次交互时才被收割（失败态不可见）；
// 且只能走低频档，不能把常驻 60fps 泵拉回来
REGISTER_TEST(terminalStartingKeepsFrameFlow,
              "terminal frames: Starting 进低频帧档、不进常驻泵档（bug406）", {
    // 仅 Starting：通知能力未知也要有帧（worker 唤醒是主路，低频帧兜底）
    DOCTEST_CHECK(terminal::needsStartingFrameTick(true, false, false) == true);
    DOCTEST_CHECK(terminal::needsStartingFrameTick(true, false, true) == true);
    // Starting + Running：沿用原文口径（有通知→低频帧；无通知→常驻泵兜底）
    DOCTEST_CHECK(terminal::needsStartingFrameTick(true, true, true) == true);
    // 无 Starting 时口径不变：无 Running → 零帧（空闲/收起零出帧不动）
    DOCTEST_CHECK(terminal::needsStartingFrameTick(false, false, true) == false);
    DOCTEST_CHECK(terminal::needsStartingFrameTick(false, false, false) == false);
    DOCTEST_CHECK(terminal::needsStartingFrameTick(false, true, true) == true);
    return true;
})

REGISTER_TEST(terminalSearchMatchColumns,
              "terminal search: 子串命中列区间（ASCII 多命中 / 大小写 / CJK 宽字 / 无命中）", {
    using terminal::findMatchColumns;
    // ASCII：两处命中，列区间升序
    auto ascii = findMatchColumns("abc ABC xabc", "abc");
    DOCTEST_CHECK_EQ(ascii.size(), 3u);
    if (ascii.size() == 3u) {
        DOCTEST_CHECK_EQ(ascii[0].colBegin, 0);
        DOCTEST_CHECK_EQ(ascii[0].colEnd, 3);
        DOCTEST_CHECK_EQ(ascii[1].colBegin, 4);
        DOCTEST_CHECK_EQ(ascii[1].colEnd, 7);
        DOCTEST_CHECK_EQ(ascii[2].colBegin, 9);
        DOCTEST_CHECK_EQ(ascii[2].colEnd, 12);
    }
    // CJK：宽字按 2 列计——"杭州" 占 4 列
    auto cjk = findMatchColumns("x杭州杭州", "杭州");
    DOCTEST_CHECK_EQ(cjk.size(), 2u);
    if (cjk.size() == 2u) {
        DOCTEST_CHECK_EQ(cjk[0].colBegin, 1);
        DOCTEST_CHECK_EQ(cjk[0].colEnd, 5);
        DOCTEST_CHECK_EQ(cjk[1].colBegin, 5);
        DOCTEST_CHECK_EQ(cjk[1].colEnd, 9);
    }
    // 空查询 / 无命中 / 查询比行还长
    DOCTEST_CHECK_TRUE(findMatchColumns("abc", "").empty());
    DOCTEST_CHECK_TRUE(findMatchColumns("abc", "zzz").empty());
    DOCTEST_CHECK_TRUE(findMatchColumns("ab", "abc").empty());
    return true;
})

REGISTER_TEST(terminalTextWordBounds,
              "terminal text util: 双击选词列区间（ASCII 词/CJK/标点/空白）", {
    using terminal::textDisplayColumns;
    using terminal::wordBoundsAt;
    // "foo bar(x)"：foo=0..3、bar=4..7、(=7
    auto w1 = wordBoundsAt("foo bar(x)", 1);
    DOCTEST_CHECK_EQ(w1.first, 0);
    DOCTEST_CHECK_EQ(w1.second, 3);
    auto w2 = wordBoundsAt("foo bar(x)", 5);
    DOCTEST_CHECK_EQ(w2.first, 4);
    DOCTEST_CHECK_EQ(w2.second, 7);
    // 标点处按单列
    auto w3 = wordBoundsAt("foo bar(x)", 7);
    DOCTEST_CHECK_EQ(w3.first, 7);
    DOCTEST_CHECK_EQ(w3.second, 8);
    // CJK：宽字每字 2 列，"杭州" = 0..4；第二列落在同一字
    DOCTEST_CHECK_EQ(textDisplayColumns("杭x"), 3);
    auto w4 = wordBoundsAt("杭州 x", 2);
    DOCTEST_CHECK_EQ(w4.first, 0);
    DOCTEST_CHECK_EQ(w4.second, 4);
    auto w5 = wordBoundsAt("杭州 x", 5);
    DOCTEST_CHECK_EQ(w5.first, 5);
    DOCTEST_CHECK_EQ(w5.second, 6);
    // 空串/越界
    auto w6 = wordBoundsAt("", 3);
    DOCTEST_CHECK_EQ(w6.first, 3);
    DOCTEST_CHECK_EQ(w6.second, 4);
    return true;
})

REGISTER_TEST(terminalQuitConfirmDecision,
              "terminal quit guard: 只有 Running 会话需要关闭/退出确认", {
    using terminal::stateNeedsCloseConfirm;
    DOCTEST_CHECK_TRUE(stateNeedsCloseConfirm(terminal::SessionState::Running));
    DOCTEST_CHECK_FALSE(stateNeedsCloseConfirm(terminal::SessionState::Idle));
    DOCTEST_CHECK_FALSE(stateNeedsCloseConfirm(terminal::SessionState::Starting));
    DOCTEST_CHECK_FALSE(stateNeedsCloseConfirm(terminal::SessionState::Exited));
    DOCTEST_CHECK_FALSE(stateNeedsCloseConfirm(terminal::SessionState::Failed));
    return true;
})
