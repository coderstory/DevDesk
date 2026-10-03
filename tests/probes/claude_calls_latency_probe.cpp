// tests/probes/claude_calls_latency_probe.cpp
// 批次 D（2026-10-02）探针：真实窗口 + 真实主循环语义下量
// 「点开「调用记录」弹窗 → 行可见」的墙钟。
//
// 与无头用例 claude_calls_latency 的分工：
//   - 无头用例量各段（派发 / 查询 / 收割）并把「后台结果就绪必须请求重绘」
//     钉成断言（回退修复即红）；
//   - 本 probe 补无头测不到的那一段：**真实帧节奏**。EUI-NEO 主循环在无动画、
//     无输入时阻塞在 glfwWaitEvents（core/app/glfw_app_main.cpp 帧末），所以
//     「那一帧什么时候来」由运行时状态（动画/脏标记/唤醒标志）决定，无头用例
//     是测试自己喂帧，看不见这件事。
//
// 帧判据逐字照抄 core/app/main_window_runtime.h runFrame 的 frameRequested：
//     paintRequested || updateRequested || frameWakeRequested || animating || 待处理指针输入
// 空闲时 glfwWaitEventsTimeout(--idle-ms) 等价 glfwWaitEvents（无事件即阻塞，
// 超时上限只为让探针必然退出；实测里它只在「没人唤醒」时才会走到）。
//
// 输出（stdout，供报告引用）：
//   click→行可见 墙钟 ms / 驱动帧数 / 阻塞等待累计 ms / 期间 animating 帧数
// 用法：./build/tests/claude_calls_latency_probe [--size 1600x1000] [--idle-ms 3000]
// GL/GLFW 头由 probe_harness 统一给（glad 路线；见 settings_cards_probes.cpp:43 同款说明）

#include "probes/probe_harness.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "core/platform/platform.h"   // consumeUiUpdate / consumeFrameRequest
#include "core/input/input_state.h"   // hasPendingPointerInput
#include "core/render/render_backend.h"   // --render：真渲染帧（量上屏段）
#include "core/theme.h"

#include "module_claude_proxy/claude_service.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <ratio>
#include <string>
#include <thread>
#include <vector>

extern "C" ::core::Module* create_claude_module();

namespace {

constexpr const char* kDbPrefix = "devdesk_probe_calls_lat_";
constexpr const char* kKeychainService = "DevDeskTestClaudeProbe";
// RAII：临时库 + 注册全局 ClaudeService（claude_probes.cpp ProbeEnv 同款）
struct ProbeEnv : test_support::TestDb {
    std::unique_ptr<module::claude::ClaudeService> svc;

    ProbeEnv() : test_support::TestDb(kDbPrefix, "") {
        svc = std::make_unique<module::claude::ClaudeService>(handle(),
                                                             kKeychainService);
        module::claude::ClaudeService::setGlobal(svc.get());
    }

    ~ProbeEnv() { module::claude::ClaudeService::setGlobal(nullptr); }
};

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}   // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    double idleWaitSeconds = 3.0;   // 等价 glfwWaitEvents 的阻塞上限（只为可退出）
    int rows = 2000;                // 调用记录行数（--rows 调档，量查询随规模的变化）
    bool wantRender = false;        // --render：真渲染帧，量「上屏」段（默认只量 compose）
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--idle-ms" && i + 1 < argc) {
            idleWaitSeconds = std::atof(argv[i + 1]) / 1000.0;
        } else if (arg == "--rows" && i + 1 < argc) {
            rows = std::atoi(argv[i + 1]);
        } else if (arg == "--render") {
            wantRender = true;
        }
    }
    if (!probe::initializeWindowBackend()) {
        return 1;
    }
    auto window = probe::makeWindow("claude_calls_latency_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    auto cfg = test_support::tempConfigFile("calls_lat_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    ProbeEnv env;
    auto prof = env.svc->createProfile("探针", "https://probe.example.com",
                                       "sk-ant-probe-lat", "m");
    if (prof.isFail()) {
        std::cerr << "probe: createProfile failed\n";
        return 1;
    }
    const int64_t pid = prof.value().id;

    // N 行调用记录（真实量级；单事务批量插，逐条 fsync 会拖到秒级）
    const int kRows = rows;
    const std::time_t base = std::time(nullptr);
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
        if (tr.isFail()) {
            std::cerr << "probe: seed failed\n";
            return 1;
        }
    }

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    const std::string pageId = "claude-proxy";
    const float frameW = static_cast<float>(args.windowW);
    const float frameH = static_cast<float>(args.windowH);

    core::dsl::Ui* ui = nullptr;
    bool rowsVisible = false;
    bool loadingVisible = false;
    bool composed = false;
    auto doCompose = [&]() {
        runtime.compose(pageId, frameW, frameH, 1.0f,
                        [&](eui::Ui& u, const eui::Screen& screen) {
                            ui = &u;
                            mod->compose(u, screen);
                            rowsVisible = u.find(
                                "claude.dialog.calls.row.0.wrap") != nullptr;
                            loadingVisible =
                                u.find("claude.dialog.calls.loading") != nullptr;
                        });
        composed = true;
    };

    // ---- 真渲染（--render）：量「上屏」里 compose 之外的那一段（GL 提交 + present）
    std::unique_ptr<core::render::RenderBackend> renderBackend;
    int fbW = 0;
    int fbH = 0;
    float renderScale = 1.0f;
    if (wantRender) {
        auto* glfwWin = static_cast<GLFWwindow*>(window);
        glfwGetFramebufferSize(glfwWin, &fbW, &fbH);
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetWindowContentScale(glfwWin, &scaleX, &scaleY);
        renderScale = scaleX > 0.0f ? scaleX : 1.0f;
        glfwShowWindow(glfwWin);
        glfwFocusWindow(glfwWin);
        renderBackend = core::render::createRenderBackend(window);
        if (renderBackend == nullptr || !renderBackend->initialize()) {
            std::cerr << "probe: 渲染后端不可用，--render 降级为只 compose\n";
            renderBackend.reset();
        }
    }
    const eui::Color clearColor = core::theme::current().colors.background;

    // ---- 主循环一帧 ----
    // 判据逐字镜像 EUI-NEO include/eui/detail/dsl_app_impl.h 的 app::update +
    // core/app/main_window_runtime.h 的 runFrame：**compose 不是每帧都发生**
    //（只在「未合成 / updateRequested / 运行时请求重组」时），这也是无头用例
    // 测不到的部分——无头是测试自己决定喂多少帧。
    struct PumpStats {
        int frames = 0;
        int composes = 0;
        int blockedFrames = 0;
        double blockedMs = 0.0;
        int animatingFrames = 0;
        int renderedFrames = 0;
        double renderMs = 0.0;
        double renderMaxMs = 0.0;
    };
    PumpStats stats;
    bool appPaintRequested = true;   // AppRunner::paintRequested 初值 = true
    auto appUpdate = [&](bool updateRequested, float dt) -> bool {
        bool changed = false;
        if (!composed) doCompose();
        if (updateRequested) {
            doCompose();
            ++stats.composes;
            changed = true;
        }
        changed = runtime.update(window, dt, 1.0f, 1.0f, true) || changed;
        if (runtime.composeRequested()) {
            runtime.requestFullPaint();
            doCompose();
            ++stats.composes;
            changed = runtime.update(window, 0.0f, 1.0f, 1.0f, true) || changed;
            changed = true;
        }
        return changed;
    };
    auto pumpOnce = [&]() {
        const bool updateRequested = ::core::platform::consumeUiUpdate();
        const bool frameWake = ::core::platform::consumeFrameRequest();
        const bool animating = runtime.isAnimating();
        const bool wantFrame = appPaintRequested || updateRequested || frameWake ||
                               animating ||
                               core::hasPendingPointerInput(window, 1.0f);
        if (animating) ++stats.animatingFrames;
        if (!wantFrame) {
            const double t0 = nowMs();
            glfwWaitEventsTimeout(idleWaitSeconds);
            stats.blockedMs += nowMs() - t0;
            ++stats.blockedFrames;
        }
        appPaintRequested = appUpdate(updateRequested || frameWake, 1.0f / 60.0f);
        ++stats.frames;
        if (appPaintRequested && renderBackend != nullptr) {
            const double t0 = nowMs();
            core::render::ScopedRenderBackend scoped(*renderBackend);
            renderBackend->beginFrame(core::render::RenderSurface{
                window, core::window::nativeWindowInfo(window), fbW, fbH,
                renderScale});
            runtime.render(fbW, fbH, renderScale, clearColor);
            renderBackend->present();
            const double dtMs = nowMs() - t0;
            stats.renderMs += dtMs;
            if (dtMs > stats.renderMaxMs) stats.renderMaxMs = dtMs;
            ++stats.renderedFrames;
            appPaintRequested = false;   // markRendered
        }
    };
    auto pumpUntil = [&](auto&& pred, double timeoutMs) -> bool {
        const double t0 = nowMs();
        while (nowMs() - t0 < timeoutMs) {
            pumpOnce();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    auto clickElement = [&](const std::string& id) -> bool {
        const core::dsl::Element* e = ui != nullptr ? ui->find(id) : nullptr;
        if (e == nullptr) {
            std::cerr << "probe: no element '" << id << "'\n";
            return false;
        }
        const float cx = e->frame.x + e->frame.width * 0.5f;
        const float cy = e->frame.y + e->frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
        return true;
    };

    // 预热：冷启动首帧（含 dataLoaded_ 冷启动同步装载）——与真机同序
    pumpUntil([&] { return ui != nullptr; }, 500.0);
    pumpOnce();
    // 切到「调用记录」详情页签（idx 4，2026-09-30 batch-fix B 起）
    if (!clickElement("claude.detail.tabs.hit.4")) return 1;
    pumpUntil([&] { return ui != nullptr && ui->find("claude.calls.viewall.hit") != nullptr; },
              1000.0);
    pumpOnce();

    // ---- 测量：点「查看全部 →」 → 行可见 ----
    if (!clickElement("claude.calls.viewall.hit")) return 1;
    const double tClick = nowMs();
    stats = PumpStats{};
    bool sawDialog = false;
    bool ok = false;
    const double budgetMs = 8000.0;
    std::vector<double> frameAtMs;
    while (nowMs() - tClick < budgetMs) {
        pumpOnce();
        frameAtMs.push_back(nowMs() - tClick);
        if (loadingVisible || rowsVisible) sawDialog = true;
        if (rowsVisible) {
            ok = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double tRows = nowMs();

    std::cout << "[probe-D] 点「查看全部 →」→ 行可见 = " << (tRows - tClick)
              << " ms" << std::endl;
    std::cout << "[probe-D] 驱动帧数 = " << stats.frames
              << "（compose " << stats.composes
              << " 次，animating 帧 " << stats.animatingFrames
              << "，阻塞等待帧 " << stats.blockedFrames
              << "，阻塞累计 " << stats.blockedMs << " ms）" << std::endl;
    std::cout << "[probe-D] 弹窗出现过 = " << (sawDialog ? "yes" : "no")
              << "，行可见 = " << (ok ? "yes" : "no") << std::endl;
    if (!frameAtMs.empty()) {
        std::cout << "[probe-D] 首帧 " << frameAtMs.front() << " ms，末帧 "
                  << frameAtMs.back() << " ms，共 " << frameAtMs.size()
                  << " 帧" << std::endl;
    }
    if (renderBackend != nullptr) {
        std::cout << "[probe-D] 渲染帧 " << stats.renderedFrames
                  << " 帧，总 " << stats.renderMs << " ms，最慢单帧 "
                  << stats.renderMaxMs << " ms（compose 之外的「上屏」段）"
                  << std::endl;
    } else {
        std::cout << "[probe-D] 未渲染（--render 未开）：上屏段只含 compose"
                  << std::endl;
    }
    std::cout << "[probe-D] 空转上限（等价 glfwWaitEvents 阻塞窗口）= "
              << idleWaitSeconds * 1000.0 << " ms" << std::endl;

    // ---- 查询本身的成本 vs 表规模（H2 的定量复核：同一两段 SQL，走后台线程）----
    {
        module::claude::ClaudeService::CallLogQuery q;
        q.limit = 20;   // kCallsPageSize（claude_ui_logic.h）
        std::atomic<long long> a{0};
        std::atomic<long long> b{0};
        std::thread th([&] {
            a.store(static_cast<long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()));
            auto r1 = env.svc->queryCalls(q);
            auto r2 = env.svc->countCalls(q);
            b.store(static_cast<long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()));
            (void)r1;
            (void)r2;
        });
        th.join();
        std::cout << "[probe-D] 表 " << kRows << " 行：queryCalls+countCalls = "
                  << static_cast<double>(b.load() - a.load()) / 1e6 << " ms"
                  << std::endl;
    }

    mod->onStop();
    test_support::cleanupProfileKey(kKeychainService, pid);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    return ok ? 0 : 1;
}
