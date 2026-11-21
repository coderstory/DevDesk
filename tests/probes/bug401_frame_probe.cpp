// bug401_frame_probe.cpp — need401 全模块 compose 帧耗时仪表（只测不修）
//
// 运行：bug401_frame_probe.exe > evidence/bug401-probe.log（真窗口 + 真 app::compose，
// 与真机同一条装配链：侧边栏 + Router 当前页）。
//
// 数据底座 = MdHeadlessEnv 临时目录（300 篇笔记 + 已迁移临时库注册 appDatabase），
// Todo 种子 60 任务，覆盖量级敏感模块。markdown 轮询周期经测试缝压到 1ms，使
// 「外部新增文件 → pollChanged 命中 → rescan」的同步链落在可测帧内（真机默认
// 5s 墙钟触发，同一段代码路径）。
//
// 输出口径：每页 enter=onPageEnter 同步耗时（如 Home getStats 直读库）、f1..f8 =
// 连续 compose 帧墙钟（含 event pump + runtime.compose，不含 render），steady=
// 第 4~8 帧中位数。> 50ms 帧按简报口径进分级报告。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>
#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"
#include "module_toggle_bridge.h"
#include "test_support.h"

#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_markdown/markdown_module_test_seam.h"
#include "module_todo/todo_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_bug401_frame_probe");
    return config;
}
}  // namespace app

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

constexpr const char* kSlugs[] = {
    "home",     "markdown", "git",           "todo",     "schedule",
    "claude_proxy", "agent", "memory",        "wallpaper", "mcp",
    "terminal", "release_group", "release_tasks", "release_config",
    "settings", "about",
};
static_assert(sizeof(kSlugs) / sizeof(kSlugs[0]) ==
                  sizeof(::app::registry::kModuleRegistry) /
                      sizeof(::app::registry::kModuleRegistry[0]),
              "kSlugs 与 kModuleRegistry 行数不一致（表序漂移）");

void seedNotes(const std::filesystem::path& dir, int count) {
    std::error_code ec;
    for (int i = 0; i < count; ++i) {
        const auto sub = dir / ("组" + std::to_string(i % 12));
        std::filesystem::create_directories(sub, ec);
        std::ofstream f(sub / ("note-" + std::to_string(i) + ".md"),
                        std::ios::binary);
        f << "# 笔记 " << i << "\n\n";
        for (int j = 0; j < 24; ++j) {
            f << "第 " << j << " 段内容，用于索引与 FTS。标签 #tag"
              << (i % 7) << " #组" << (i % 12) << "\n";
        }
    }
}

int seedTodos(test_support::MdHeadlessEnv& env, int count) {
    module::todo::TodoService svc(env.db->db());
    auto cols = svc.listColumns();
    if (cols.isFail() || cols.value().empty()) return 0;
    const int64_t colId = cols.value().front().id;
    int made = 0;
    for (int i = 0; i < count; ++i) {
        if (svc.createTask(colId, "任务 " + std::to_string(i)).isOk()) ++made;
    }
    return made;
}

double composeOnce(core::dsl::Runtime& runtime, core::window::Handle window,
                   float w, float h) {
    const auto t0 = Clock::now();
    probe::composeModuleFrame(
        runtime, window, "bug401", w, h,
        [](eui::Ui& ui, const eui::Screen& screen) { app::compose(ui, screen); });
    return msSince(t0);
}

double medianOf(std::vector<double> v, std::size_t from) {
    if (from >= v.size()) return 0.0;
    std::vector<double> s(v.begin() + static_cast<std::ptrdiff_t>(from), v.end());
    std::sort(s.begin(), s.end());
    const std::size_t n = s.size();
    return n % 2 ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) * 0.5;
}

void printPage(const std::string& slug, double enterMs,
               const std::vector<double>& frames) {
    std::cout << "PAGE " << slug << " enter=" << enterMs;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::cout << " f" << (i + 1) << "=" << frames[i];
    }
    const double maxF = frames.empty()
                            ? 0.0
                            : *std::max_element(frames.begin(), frames.end());
    std::cout << " max=" << maxF << " steady=" << medianOf(frames, 3) << "\n";
}

}  // namespace

int main() {
    if (!probe::initializeWindowBackend()) return 2;
    auto window = probe::makeWindow("bug401_frame_probe", 1440, 900);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::MdHeadlessEnv mdEnv("bug401_", "frames", "frames.db");
    seedNotes(mdEnv.dir, 300);

    ::core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);

    ::core::Module* mdMod = nullptr;
    for (std::size_t i = 0; i < mods.size(); ++i) {
        if (std::string(kSlugs[i]) == "markdown") mdMod = mods[i].get();
    }
    if (mdMod != nullptr) {
        // 默认 1ms（与 bug401 基线同一口径，把同步链压进可测帧）；bug402/403 起
        // 比对/rescan 在后台线程，1ms 只是「后台巡检多久一轮」，可 BUG401_POLL_MS
        // 覆盖成真机周期（5000）看稳态帧。
        int64_t pollMs = 1;
        if (const char* env = std::getenv("BUG401_POLL_MS")) {
            const long long v = std::atoll(env);
            if (v > 0) pollMs = v;
        }
        module::markdown::mdSetPollIntervalMsForTest(mdMod, pollMs);
        std::cout << "BUG401 poll_ms=" << pollMs << "\n";
    }
    const int todoSeeded = seedTodos(mdEnv, 60);

    router.startAll();
    // 外部新增文件：下一次 markdown compose 的 pollChanged 必命中 → rescan 落在帧内
    {
        std::ofstream f(mdEnv.dir / "external-new.md", std::ios::binary);
        f << "# 外部新增\n\n用于触发 pollChanged + rescan 同步链。\n";
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    app::requestUpdate();
    app::requestShow();

    const auto [w, h] = probe::windowLogicalSize(window);
    std::cout << "BUG401 notes=300 todos=" << todoSeeded << " size=" << w << "x"
              << h << "\n";
    composeOnce(runtime, window, w, h);  // 暖帧（首帧含窗口/主题初始化，不计入页表）

    for (std::size_t i = 0; i < mods.size(); ++i) {
        const auto& e = ::app::registry::kModuleRegistry[i];
        if (mods[i] == nullptr || std::string(kSlugs[i]) == "release_group") {
            std::cout << "PAGE " << kSlugs[i] << " skipped=no-instance\n";
            continue;
        }
        const auto tEnter = Clock::now();
        router.navigateTo(e.page);
        const double enterMs = msSince(tEnter);
        std::vector<double> frames;
        for (int f = 0; f < 8; ++f) {
            frames.push_back(composeOnce(runtime, window, w, h));
        }
        printPage(kSlugs[i], enterMs, frames);
    }

    // Home 二次进页（onPageEnter → refreshStats 直读库）——离开再回来，同步口径
    {
        router.navigateTo(::core::PageId::Settings);
        composeOnce(runtime, window, w, h);
        const auto t0 = Clock::now();
        router.navigateTo(::core::PageId::Home);
        const double enterMs = msSince(t0);
        std::cout << "HOME_REENTER enter=" << enterMs << "\n";
    }

    // Memory：跨过 1s 刷新节流后的首个 compose（proposals/ledger 查询 + 全量快照）
    {
        router.navigateTo(::core::PageId::Memory);
        composeOnce(runtime, window, w, h);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        const auto t0 = Clock::now();
        const double ms = composeOnce(runtime, window, w, h);
        std::cout << "MEM_REFRESH_FRAME frame=" << ms << " (sleep1100)"
                  << " total=" << msSince(t0) << "\n";
    }

    // bug402/403 语义验证：外部新增/删除笔记 → watcher 后台线程增量同步索引
    //（比对/rescan 已不在 UI 帧里跑），索引必须跟随磁盘且不靠帧驱动。
    {
        auto indexCount = [&](const char* path) -> std::string {
            auto prep = mdEnv.db->db().prepare(
                "SELECT COUNT(*) AS n FROM markdown_index WHERE path = ?");
            if (prep.isFail()) return "prepare-fail";
            auto& stmt = prep.value();
            if (stmt.bind(1, std::string(path)).isFail()) return "bind-fail";
            auto rows = stmt.fetchAll();
            if (rows.isFail() || rows.value().empty()) return "fetch-fail";
            auto v = rows.value()[0].getInt64("n");
            return v.isOk() ? std::to_string(v.value()) : "read-fail";
        };
        {
            std::ofstream f(mdEnv.dir / "external-late.md", std::ios::binary);
            f << "# 外部新增后到\n\n后台链路的语义验证。\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cout << "WATCH_ADD_INDEX count=" << indexCount("external-late.md")
                  << " (expect 1)\n";
        std::error_code rec;
        std::filesystem::remove(mdEnv.dir / "external-late.md", rec);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cout << "WATCH_DEL_INDEX count=" << indexCount("external-late.md")
                  << " (expect 0)\n";
    }

    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << "BUG401 done\n";
    return 0;
}