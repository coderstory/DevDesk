// ui_driver_main.cpp — 债-18「UI 自动化驱动入口」CLI（方案卡 §5 F-01/F-08）。
//
// 用法：
//   devdesk_ui_driver --script <scene.json> [--report-json <path>] [--out-dir <dir>]
//                     [--keep-going] [--size WxH]
//   devdesk_ui_driver --dump-ids [--size WxH]     # 当帧元素树清单（场景编写辅助）
//
// 退出码：0 全过 | 1 有步骤失败或报告·截图落盘失败 | 2 用法/场景非法（含 0 步）
//        | 77 无显示环境诚实跳过（金丝雀判定，ctest SKIP_RETURN_CODE）。
//
// 环境组装（F-08）：进程恒用临时数据根（DataRootEnv，绝不碰真实 ~/DevDesk /
// %APPDATA%\DevDesk）+ 已 migrate 临时库注册 appDatabase + 真 kModuleRegistry
// 全表 Router（uat62_page_shots_probe 同款装配）+ 真 app::compose（app 级导航 =
// 真点侧边栏条目，module_toggle_sidebar_probe 同款链路）。
// app::dslAppConfig() 需桩（main.cpp 不在链接面）；app::requestShow/requestUpdate
// 真调一次——GLFW 初始化后安全（只置原子标志 + postEmptyEvent），同时强制
// dsl_app_impl.h 的内联定义落地本 TU（module_toggle_sidebar_probe 同款理由）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "app_assembly.h"          // buildGatedEntries / dispatchModuleToggle
#include "app_registry.h"
#include "app_sidebar.h"
#include "module_toggle_bridge.h"  // app::toggle::setToggleHandler
#include "settings_config.h"       // SettingsStore（门控注册读 enabledModules）
#include "test_support.h"
#include "ui_driver/driver_dump.h"
#include "ui_driver/driver_report.h"
#include "ui_driver/driver_schema.h"
#include "ui_driver/driver_steps.h"

#include "core/module_interface.h"
#include "core/router.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层链接桩（dsl_app_impl.h 引用 dslAppConfig；app::compose 用真实现，
// 来自本可执行编译的 src/app_sidebar.cpp，不桩）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_ui_driver");
    return config;
}
}  // namespace app

namespace {

struct CliArgs {
    std::string scriptPath;
    std::string reportJsonPath;
    std::string outDir = "ui-drive-shots";
    bool dumpIds = false;
    bool keepGoing = false;
    bool hasSize = false;
    int windowW = 0;
    int windowH = 0;
};

const char* kUsage =
    "用法：devdesk_ui_driver --script <scene.json> [--report-json <path>]\n"
    "                       [--out-dir <dir>] [--keep-going] [--size WxH]\n"
    "  或：devdesk_ui_driver --dump-ids [--size WxH]\n"
    "退出码：0 全过 | 1 步骤失败或落盘失败 | 2 用法/场景非法 | 77 无显示跳过";

CliArgs parseCli(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--script" && i + 1 < argc) {
            args.scriptPath = argv[++i];
        } else if (arg == "--report-json" && i + 1 < argc) {
            args.reportJsonPath = argv[++i];
        } else if (arg == "--out-dir" && i + 1 < argc) {
            args.outDir = argv[++i];
        } else if (arg == "--dump-ids") {
            args.dumpIds = true;
        } else if (arg == "--keep-going") {
            args.keepGoing = true;
        } else if (arg == "--size" && i + 1 < argc) {
            const std::string size = argv[++i];
            const std::size_t sep = size.find_first_of("xX");
            if (sep != std::string::npos) {
                args.windowW = std::atoi(size.substr(0, sep).c_str());
                args.windowH = std::atoi(size.substr(sep + 1).c_str());
                args.hasSize = args.windowW > 0 && args.windowH > 0;
            }
        }
    }
    return args;
}

// app 组装（main.cpp 装配链的驱动复刻）：真 kModuleRegistry 全表**实例** +
// buildGatedEntries 门控**注册**（enabledModules 决定侧边栏条目，settings-toggle
// 场景的种子证明依赖这一点）+ toggle 桥 handler（真点开关 → 注册/注销生效）。
// uat62_page_shots_probe 全表直注册是截图专用形态，不模拟门控，勿照抄。
namespace {

::core::Router* g_router = nullptr;
std::vector<std::unique_ptr<::core::Module>>* g_mods = nullptr;

constexpr std::size_t kRegistrySize =
    sizeof(::app::registry::kModuleRegistry) /
    sizeof(::app::registry::kModuleRegistry[0]);

// ModuleLookup：PageId → 已实例化模块（main.cpp moduleForPage 同款，裸函数指针）
::core::Module* lookupForDriver(::core::PageId page) {
    if (g_mods == nullptr) return nullptr;
    for (std::size_t i = 0; i < kRegistrySize; ++i) {
        if (::app::registry::kModuleRegistry[i].page == page) {
            return (*g_mods)[i].get();
        }
    }
    return nullptr;
}

// handler 形态同生产接线（module_toggle_sidebar_probe 同款）：settings 卡片开关 →
// 桥 → 这里 → dispatchModuleToggle
void driverToggleHandler(const std::string& moduleId, bool on) {
    if (g_router != nullptr) {
        app::assembly::dispatchModuleToggle(moduleId, on, *g_router,
                                            &lookupForDriver);
    }
}

}  // namespace

// 返回 false = 门控后无可注册模块（异常态）
bool assembleApp(::core::Router& router,
                 std::vector<std::unique_ptr<::core::Module>>& mods) {
    // 全表实例化（工厂可能返回 nullptr：平台能力不支持，如非 Windows 的 wallpaper）
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        mods.push_back(std::move(m));
    }
    // 门控注册：SettingsStore 从 appDatabase 读 enabledModules（种子已预置），
    // 恒注册行必含、门控行 moduleEnabled 才含（main.cpp / app_assembly 同款）
    const module::settings::SettingsStore store(::core::appDatabase());
    const auto gated = app::assembly::buildGatedEntries(
        store.load(), ::app::registry::kModuleRegistry, kRegistrySize);
    std::size_t registered = 0;
    for (const auto& entry : gated) {
        for (std::size_t i = 0; i < kRegistrySize; ++i) {
            if (::app::registry::kModuleRegistry[i].page == entry.page) {
                if (mods[i] != nullptr) {
                    router.registerModule(mods[i].get());
                    ++registered;
                }
                break;
            }
        }
    }
    g_mods = &mods;
    g_router = &router;
    app::toggle::setToggleHandler(&driverToggleHandler);
    return registered > 0;
}

int cleanupAndReturn(::core::Router& router,
                     std::vector<std::unique_ptr<::core::Module>>& mods,
                     core::dsl::Runtime& runtime,
                     core::window::Handle window,
                     int code) {
    // 收尾顺序对齐 uat62_page_shots_probe：stopAll → runtime → 窗口 → 后端；
    // 模块实例随 main 作用域析构（stopAll 已停生命周期）。桥 handler 置空防
    // 悬垂（module_toggle_sidebar_probe 同款收尾）。
    (void)mods;
    app::toggle::setToggleHandler(nullptr);
    g_router = nullptr;
    g_mods = nullptr;
    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    return code;
}

}  // namespace

int main(int argc, char* argv[]) {
    const CliArgs args = parseCli(argc, argv);
    if ((args.scriptPath.empty() && !args.dumpIds) ||
        (!args.scriptPath.empty() && args.dumpIds)) {
        std::cerr << kUsage << "\n";
        return 2;
    }

    // 场景加载在开窗之前：场景非法（退出码 2 档）不值得起真窗
    ui_drive::Scene scene;
    std::string sceneName;
    if (!args.scriptPath.empty()) {
        auto loaded = ui_drive::loadSceneFile(args.scriptPath);
        if (loaded.isFail()) {
            std::cerr << "ui_driver: 场景非法（退出码 2）——" << loaded.errorMessage() << "\n";
            return 2;
        }
        scene = std::move(loaded.value());
        sceneName = std::filesystem::path(args.scriptPath).filename().string();
    }

    // 金丝雀：无显示环境 → initializeWindowBackend 内部 std::exit(77)
    //（ctest SKIP_RETURN_CODE 记 Skipped）；RealFailure → 1（代码问题必须红）
    if (!probe::initializeWindowBackend()) return 1;

    const int windowW = args.hasSize ? args.windowW : scene.settings.windowWidth;
    const int windowH = args.hasSize ? args.windowH : scene.settings.windowHeight;
    auto window = probe::makeWindow("devdesk_ui_driver", windowW, windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 数据前置（F-08）：临时数据根 + 临时库注册 appDatabase。进程结束即弃。
    test_support::DataRootEnv dataRoot("uidrv_", "root");
    test_support::TestEnv env("uidrv_", "db", /*registerAppDb=*/true);

    // 场景种子（settings.seed.config）：当场读回校验，预置失败 fail-fast
    for (const auto& [ns, text] : scene.settings.seedConfigs) {
        if (!test_support::seedAppConfigJson(ns.c_str(), text)) {
            std::cerr << "ui_driver: 种子预置失败（settings.seed.config 命名空间 '"
                      << ns << "'）——中止（退出码 1）\n";
            probe::terminateWindowBackend();
            return 1;
        }
    }

    ::core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    if (!assembleApp(router, mods)) {
        std::cerr << "ui_driver: kModuleRegistry 全表实例化失败（无可运行模块）\n";
        probe::terminateWindowBackend();
        return 2;
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    // 内联定义落地本 TU：GLFW 初始化后调用安全（见文件头说明）
    app::requestUpdate();
    app::requestShow();

    const float w = static_cast<float>(windowW);
    const float h = static_cast<float>(windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFn = [&](eui::Ui& u, const eui::Screen& screen) {
        ui = &u;
        app::compose(u, screen);
    };

    // --dump-ids：compose 数帧（稳态）→ 打印当帧元素树 → 退出 0
    if (args.dumpIds) {
        for (int i = 0; i < 3; ++i) {
            probe::composeModuleFrame(runtime, window, "app", w, h, composeFn);
        }
        for (const ui_drive::DumpRow& row : ui_drive::collectDumpRows(*ui)) {
            std::cout << ui_drive::formatDumpRow(row) << "\n";
        }
        return cleanupAndReturn(router, mods, runtime, window, 0);
    }

    // --script：解释器逐步骤执行
    std::filesystem::create_directories(args.outDir);
    ui_drive::DriverEnv driverEnv;
    driverEnv.window = window;
    driverEnv.runtime = &runtime;
    driverEnv.windowW = w;
    driverEnv.windowH = h;
    driverEnv.outDir = args.outDir;
    driverEnv.keepGoing = args.keepGoing || scene.settings.keepGoing;
    driverEnv.defaultTimeoutMs = scene.settings.defaultWaitTimeoutMs;
    driverEnv.composeFn = composeFn;

    ui_drive::SceneRunner runner(driverEnv);
    // 首帧 compose 先行（runner 内步骤从「树已就位」态开始）。Ui 对象地址随
    // Runtime 存活而稳定（compose 重建的是元素树，不是 Ui），attachUi 一次即可。
    probe::composeModuleFrame(runtime, window, "app", w, h, composeFn);
    runner.attachUi(ui);
    ui_drive::RunOutcome outcome = runner.run(scene, sceneName);

    // --report-json：结构化明细落盘（失败 → 退出码 1，证据链缺失不静默）
    if (!args.reportJsonPath.empty()) {
        const std::string json =
            ui_drive::buildReportJson(outcome.results, sceneName, outcome.exitCode);
        std::ofstream report(args.reportJsonPath, std::ios::binary | std::ios::trunc);
        if (!report || !report.write(json.data(), static_cast<std::streamsize>(json.size()))) {
            std::cerr << "ui_driver: 报告落盘失败: " << args.reportJsonPath
                      << "（退出码 1）\n";
            outcome.exitCode = 1;
        }
    }

    std::cout << "ui_driver: 退出码 " << outcome.exitCode << "（"
              << outcome.results.size() << " 步）\n";
    return cleanupAndReturn(router, mods, runtime, window, outcome.exitCode);
}
