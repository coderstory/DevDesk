// tests/test_modules/test_claude_calls_latency.cpp
// 调用记录弹窗的分段耗时实测 + 「后台结果就绪必须唤醒主循环」钉板（弹窗路径 + 页面级刷新路径各一条用例）。
// 跨文件前提：
//   - 计时缝与次序缝由 claude_ui_logs.cpp / claude_ui_state.cpp 文件末尾 extern "C" 导出，声明见下。
//   - EUI-NEO 的两个帧唤醒标志是进程全局原子量，只读消费即可断言「请求了重绘」，不需要 GLFW 初始化。
//   - 无头进程不跑主循环；本文件按「没有唤醒信号就没有下一帧」建模，等价 glfwWaitEvents 阻塞。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "claude_ui_logic.h"
#include "claude_ui_state.h"   // runDataRefresh / DataRefresh（争用负载）
#include "core/database.h"
#include "core/input/input_types.h"   // EUI-NEO core::TextInputEvent
#include "core/module_interface.h"
#include "core/sqlite_db.h"
#include "core/theme.h"
#include "core/platform/platform.h"

#include <eui/dsl.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" ::core::Module* create_claude_module();
// claude_ui_logs.cpp 文件末尾同名导出
extern "C" int claude_calls_query_dispatches_for_test();
extern "C" unsigned long long claude_calls_query_last_thread_for_test();
// 同文件导出的四个 steady_clock 纳秒时刻（0 = 未发生）：派发点 / worker 启动 / worker 查完 / 结果进快照
extern "C" long long claude_calls_query_dispatch_ns_for_test();
extern "C" long long claude_calls_query_worker_start_ns_for_test();
extern "C" long long claude_calls_query_worker_done_ns_for_test();
extern "C" long long claude_calls_query_harvest_ns_for_test();
// 次序缝：唤醒发出那一刻结果共享状态是否已就绪（true = 先就绪后唤醒）。弹窗路径与页面路径各一条同名导出
extern "C" bool claude_calls_query_wake_saw_ready_for_test();
extern "C" bool claude_data_refresh_wake_saw_ready_for_test();

namespace {

constexpr const char* kTestKeychainService = "DevDeskTestClaudeModule";
constexpr const char* kDbPrefix = "devdesk_test_claude_lat_";

// 临时库 + 注册全局 ClaudeService：析构必须摘掉全局单例，否则同进程后续用例读悬垂指针
struct TestEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    TestEnv() : test_support::TestDb(kDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(handle(),
                                                             kTestKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~TestEnv() { module::claude::ClaudeService::setGlobal(nullptr); }
};

long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double msOf(long long ns) { return static_cast<double>(ns) / 1e6; }

}   // namespace

REGISTER_TEST(claude_calls_dialog_latency_segments,
              "claude_calls_latency: 调用记录弹窗分段耗时 + 后台结果唤醒主循环", {
    TestEnv env;
    auto p = env.svc->createProfile("延迟实测", "https://lat.example.com",
                                    "sk-ant-latdlg-01", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;
    const std::time_t base = std::time(nullptr);

    // 2000 行（真实量级：代理每条转发调用落一行）。单事务批量插入不可拆成逐条提交——那走
    // WAL + synchronous=FULL，2000 次 fsync 会把用例拖到秒级。
    constexpr int kRows = 2000;
    {
        auto tr = env.handle().transaction([&]() -> ::core::Result<void> {
            for (int i = 0; i < kRows; ++i) {
                module::claude::ClaudeService::CallLogEntry e;
                e.profileId = pid;
                e.method = (i % 7 == 0) ? "GET" : "POST";
                e.path = (i % 5 == 0) ? "/v1/complete" : "/v1/messages";
                e.statusCode = (i % 11 == 0) ? 429 : 200;
                e.latencyMs = 100 + (i % 300);
                e.startedAt = base - i;
                auto r = env.svc->logCall(e);
                if (r.isFail()) return r;
            }
            return ::core::Result<void>::ok();
        });
        DOCTEST_REQUIRE(tr.isOk());
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_calls_lat");
        mod->compose(ui, screen);
        ui.end();
    };
    auto click = [](eui::Ui& ui, const std::string& id) -> bool {
        const auto* node = ui.find(id);
        if (node == nullptr || node->onClick == nullptr) {
            std::cerr << "missing clickable: " << id << std::endl;
            return false;
        }
        node->onClick();
        return true;
    };
    auto report = [](const char* seg, double ms, const char* note) {
        std::cout << "【批次D 实测】" << seg << " = " << ms << " ms " << note
                  << std::endl;
    };

    // 「调用记录」= 详情面板第 5 页签（idx 4）；首帧走 dataLoaded_ 同步装载一次，与真机同序
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(click(ui, "claude.detail.tabs.hit.4"));
    }

    // 测量前必须排空唤醒标志：两个标志进程全局，上一例 / 上一次派发的残留会让 ③ 的断言恒真
    auto drainWake = [] {
        ::core::platform::consumeUiUpdate();
        ::core::platform::consumeFrameRequest();
    };
    drainWake();
    const int q0 = claude_calls_query_dispatches_for_test();
    const long long done0 = claude_calls_query_worker_done_ns_for_test();

    // ① 打开动作（点「查看全部 →」）→ 弹窗首帧
    const long long t_click = nowNs();
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_REQUIRE(click(ui, "claude.calls.viewall.hit"));
    }
    // 自检：开弹窗这一帧还没有查询在飞，不可能被 worker 的唤醒污染，所以这两条恒绿——它们
    // 证明的是「排空的标志位确实干净」。不可改成「派发帧之后仍为 false」：2000 行库查询
    // 只要 0.25ms，worker 常在同一帧的 compose 里就跑完并发出唤醒，那个断言会 flaky。
    DOCTEST_CHECK_MESSAGE(!::core::platform::consumeUiUpdate(),
                          "开弹窗帧不该请求重绘");
    DOCTEST_CHECK_MESSAGE(!::core::platform::consumeFrameRequest(),
                          "开弹窗帧不该请求帧");
    // ② 弹窗首帧：显示「加载中…」占位 + 同帧派发后台查询（0 帧延迟）
    const long long t_first_frame_start = nowNs();
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.loading") != nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.0.wrap") == nullptr);
    }
    const long long t_first_frame_end = nowNs();
    DOCTEST_CHECK_EQ(claude_calls_query_dispatches_for_test(), q0 + 1);
    const long long t_dispatch = claude_calls_query_dispatch_ns_for_test();
    DOCTEST_CHECK(t_dispatch >= t_click);

    // 等后台查询实跑完（**不 compose**：compose 会把结果收进快照，那是上屏帧那一段）
    long long worker_start = 0;
    long long worker_done = 0;
    for (int i = 0; i < 5000; ++i) {   // 5s 上限（负载机器上留足余量）
        worker_start = claude_calls_query_worker_start_ns_for_test();
        worker_done = claude_calls_query_worker_done_ns_for_test();
        if (worker_done != done0 && worker_done != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DOCTEST_REQUIRE_MESSAGE(worker_done != done0 && worker_done != 0,
                            "后台查询未在 ~5s 内跑完");
    DOCTEST_CHECK(worker_start >= t_dispatch);

    // ③ 仿真主循环：没有唤醒信号就「不出帧」（等价 glfwWaitEvents 阻塞）。后台结果就绪必须发
    //    一次唤醒，否则主循环等不到事件、数据早在内存里也没人 recompose。
    bool woke = false;
    const long long t_wait_start = nowNs();
    while (nowNs() - t_wait_start < 3000000000LL) {
        if (::core::platform::consumeUiUpdate() ||
            ::core::platform::consumeFrameRequest()) {
            woke = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const long long t_woke = nowNs();
    DOCTEST_CHECK_MESSAGE(
        woke,
        "后台结果就绪后无人请求重绘：主循环停在 glfwWaitEvents 等不到事件，"
        "结果要等用户下一次动鼠标/敲键才上屏（= 用户主诉的「加载特别慢」）");
    // 次序断言：唤醒发出那一刻结果必须**已经就绪**。「先唤醒、后就绪」是原症状的复发形态
    // ——std::async 在 callable 返回之后才发布值，UI 抢跑会把唤醒位消费掉、脏标记已清 →
    // 结果没人再上屏；而这个窗口只有纳秒级，只测最终结果抓不到，故由 worker 在唤醒前真探一次共享状态。
    DOCTEST_CHECK_MESSAGE(
        claude_calls_query_wake_saw_ready_for_test(),
        "唤醒先于结果就绪：UI 可能抢跑消费唤醒位 → 结果不上屏（次序必须是"
        "「先 set_value 发布、后 requestUiUpdate」）");

    // ④ 上屏帧：被唤醒后的那一帧把结果搬进快照并渲染 2000 行的第 1 页
    const long long t_visible_start = nowNs();
    {
        eui::Ui ui;
        compose(ui);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.loading") == nullptr);
        DOCTEST_CHECK(ui.find("claude.dialog.calls.row.0.wrap") != nullptr);
        const auto* cnt = ui.find("claude.dialog.calls.count");
        DOCTEST_REQUIRE(cnt != nullptr);
        DOCTEST_CHECK(cnt->text == std::string("2000 条匹配"));
    }
    const long long t_visible_end = nowNs();
    const long long t_harvest = claude_calls_query_harvest_ns_for_test();
    DOCTEST_CHECK(t_harvest >= worker_done);
    DOCTEST_CHECK(claude_calls_query_last_thread_for_test() !=
                  static_cast<unsigned long long>(
                      std::hash<std::thread::id>{}(std::this_thread::get_id())));

    report("① 打开动作→弹窗首帧 compose", msOf(t_first_frame_start - t_click),
           "(点击后到首帧 compose 起点)");
    report("② 首帧 compose 墙钟", msOf(t_first_frame_end - t_first_frame_start),
           "(弹窗整棵树 + 「加载中…」占位，零 IO)");
    report("③ 派发→worker 启动", msOf(worker_start - t_dispatch),
           "(std::async 线程唤醒)");
    report("④ worker 启动→查完", msOf(worker_done - worker_start),
           "(queryCalls + countCalls，表 2000 行)");
    report("⑤ 查完→主循环被唤醒", msOf(t_woke - worker_done),
           "(修复后即时；修复前无信号 = 等价 ∞)");
    report("⑥ 唤醒→结果上屏帧 compose",
           msOf(t_visible_end - t_visible_start), "(收割快照 + 渲染第 1 页 20 行)");
    report("= 端到端（点击→首帧→派发→上屏）", msOf(t_visible_end - t_click),
           "(仿真主循环：无唤醒信号不出帧；不含真机 GPU 渲染)");

    // 单连接争用对照：负载 = Claude 页 compose 每 200ms 节流派发的同一函数 runDataRefresh
    //（listProfiles / getActiveProfile / listSharedConfigs / recentCalls(30)），在另一线程全速跑，
    // 量弹窗那两条 SQL 被拉长多少（= 单连接互斥的贡献上界）。
    module::claude::ClaudeService::CallLogQuery q;
    q.limit = module::claude::kCallsPageSize;
    const auto timedQuery = [&]() -> double {
        std::atomic<long long> a{0};
        std::atomic<long long> b{0};
        std::thread th([&] {
            a.store(nowNs());
            auto r1 = env.svc->queryCalls(q);
            auto r2 = env.svc->countCalls(q);
            b.store(nowNs());
            (void)r1;
            (void)r2;
        });
        th.join();
        return msOf(b.load() - a.load());
    };
    double idleTotal = 0.0;
    constexpr int kIdleRuns = 20;
    for (int i = 0; i < kIdleRuns; ++i) idleTotal += timedQuery();
    std::atomic<bool> loadStop{false};
    std::thread loadThread([&] {
        while (!loadStop.load()) {
            module::claude::DataRefresh r =
                module::claude::runDataRefresh(env.svc.get(), true, true);
            (void)r;
        }
    });
    double busyTotal = 0.0;
    constexpr int kBusyRuns = 20;
    for (int i = 0; i < kBusyRuns; ++i) busyTotal += timedQuery();
    loadStop.store(true);
    loadThread.join();
    report("⑦ 查询耗时（空闲，均值）", idleTotal / kIdleRuns, "(同两条 SQL)");
    report("⑦ 查询耗时（另线程全速跑页刷新，均值）", busyTotal / kBusyRuns,
           "(单连接互斥的贡献上界)");

    mod->onStop();
    test_support::cleanupProfileKey(kTestKeychainService, pid);
    return true;
})

// 页面级后台刷新（profile 列表 / 最近调用快照）的同一条唤醒契约，代码在
// claude_ui_state.cpp runDataRefresh 末尾。与弹窗路径同一病，且**没有**弹窗打开时的
// 0.16s 过渡动画兜底，是「页面内容加载很慢」更直接的来源。
// 回退即红：删掉那句 requestUiUpdate，本用例在 3s 窗口内拿不到唤醒信号（派发帧本身
// 不置位，已在本用例内自检）。
REGISTER_TEST(claude_page_refresh_wakes_ui,
              "claude_calls_latency: 页面级后台刷新完成唤醒主循环（headless）", {
    TestEnv env;
    auto p = env.svc->createProfile("刷新唤醒", "https://wake.example.com",
                                    "sk-ant-wake-01", "m");
    DOCTEST_REQUIRE(p.isOk());
    const int64_t pid = p.value().id;

    std::unique_ptr<::core::Module> mod(create_claude_module());
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    auto compose = [&](eui::Ui& ui) {
        ui.begin("claude_page_wake");
        mod->compose(ui, screen);
        ui.end();
    };

    // 冷启动首帧：dataLoaded_ 走同步装载一次（此后刷新全走后台）
    {
        eui::Ui ui;
        compose(ui);
    }
    ::core::platform::consumeUiUpdate();
    ::core::platform::consumeFrameRequest();
    // 自检：此刻唤醒标志干净——冷启动那一次是**同步**装载（调用点就在 compose 里，早已返回），
    // 它请求的重绘已被上面 consume 掉，于是后面观察到的唤醒只可能来自「本次后台刷新完成」。
    // 不可断言「派发帧之后仍未置位」：页刷新只要 ~1ms，worker 常在同一帧的 compose 里就跑完
    // 并发出唤醒，那个断言会 flaky。
    DOCTEST_CHECK_MESSAGE(!::core::platform::consumeUiUpdate(),
                          "起始唤醒标志应干净");
    DOCTEST_CHECK_MESSAGE(!::core::platform::consumeFrameRequest(),
                          "起始帧标志应干净");

    // 进页标脏 → 下一帧 compose 派发一次后台刷新
    mod->onPageEnter();
    {
        eui::Ui ui;
        compose(ui);
    }
    // 此处**不能**再 consume 一次当「清位」：本页刷新只要 ~0.2ms，worker 常在上一帧
    // compose 期间就跑完并发出唤醒，清位会把那一下唤醒吃掉，等待窗必然超时（实测过的
    // 假红）。排空只放在派发**之前**，之后到等待之间没有任何置位来源：compose 路径不置位。

    bool woke = false;
    const long long t0 = nowNs();
    while (nowNs() - t0 < 3000000000LL) {
        if (::core::platform::consumeUiUpdate() ||
            ::core::platform::consumeFrameRequest()) {
            woke = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << "【批次D 实测】页面刷新完成→主循环被唤醒 = "
              << msOf(nowNs() - t0) << " ms" << std::endl;
    DOCTEST_CHECK_MESSAGE(
        woke, "页面级后台刷新完成后无人请求重绘：列表/最近调用快照要等下一次"
              "用户输入才上屏");
    // 次序断言：同调用记录路径——先就绪、后唤醒
    DOCTEST_CHECK_MESSAGE(
        claude_data_refresh_wake_saw_ready_for_test(),
        "页面刷新唤醒先于结果就绪（次序必须是「先发布、后唤醒」）");

    mod->onStop();
    test_support::cleanupProfileKey(kTestKeychainService, pid);
    return true;
})
