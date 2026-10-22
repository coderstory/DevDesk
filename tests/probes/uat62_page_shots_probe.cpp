// uat62_page_shots_probe.cpp — UAT62 设计系统「全页面截图批」探针（真窗口 + app::compose）
//
// 交付物：7 个 Router 页面 × 每档窗口尺寸各一张 PNG（供「看图会话」逐页对设计稿审查）。
// 运行两次（每档一次，尺寸由 --size 传入，文件名后缀 = 窗口高）：
//   uat62_page_shots_probe.exe --size 1000x640 --out <输出目录>   → 01-home-640.png … 07-about-640.png
//   uat62_page_shots_probe.exe --size 1600x1000 --out <输出目录>  → 01-home-1000.png … 07-about-1000.png
// --out 缺省 = 当前工作目录下 UAT62-screenshots/；目录不存在自动建。
//
// 构图路径：真 app::compose（自绘标题栏 + 侧边栏 + Router 当前页，与真机同一条装配链）。
// 页面按 src/app_registry.h 的 kModuleRegistry 表序遍历（7 行 = PageId 枚举序 =
// 侧边栏序），实例全部来自表内 extern "C" 工厂（真模块，非桩）。
//
// 数据态 = 探针默认空状态（设计审查对象之一，不造假数据）：MdHeadlessEnv 给
// 空笔记目录 + 已迁移空库（注册为 appDatabase）+ 关笔记目录 git 自动提交，
// 各模块 onStart 全部走「空库/空目录」默认分支。
//
// 落图不用 captureIfRequested 而直接 readFrame + writeRgbaToPng：同一设施低一层，
// 但每页落图前可在 RGBA 缓冲上做**全黑帧 / 纯色帧判据**（readFrame 已翻轴为
// top-down；翻轴本身的正确性由 screenshot_probe 的往返断言钉死，本探针不重复验）。
//
// 链接面（CMake 里与 module_toggle_sidebar_probe / h13_fluid_background_probe 同组）：
// 额外编译 src/app_sidebar.cpp + src/app_assembly.cpp + src/module_toggle_bridge.cpp。
// app::dslAppConfig() 桩 + app::requestShow/requestUpdate 真调用一次（内联定义落地）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现
#include "app_assembly.h"             // app_assembly.cpp 符号（app_sidebar 引用）
#include "app_registry.h"             // kModuleRegistry（7 行登记表 + 工厂）
#include "app_sidebar.h"              // 真 app::compose
#include "module_toggle_bridge.h"     // app_assembly 的桥符号
#include "test_support.h"             // MdHeadlessEnv / ScopedAppRouter

#include "core/module_interface.h"
#include "core/router.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

// app:: 层链接桩（dsl_app_impl.h 引用 dslAppConfig；app::compose 用真实现，
// 来自本 probe 编译的 src/app_sidebar.cpp，不桩）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_uat62_page_shots_probe");
    return config;
}
}  // namespace app

namespace {

// 输出文件名 slug（与 kModuleRegistry 表序一一对应；static_assert 钉长度）
constexpr const char* kSlugs[] = {
    "home", "markdown", "todo", "claude_proxy", "agent", "settings", "about",
};
static_assert(sizeof(kSlugs) / sizeof(kSlugs[0]) ==
                  sizeof(::app::registry::kModuleRegistry) /
                      sizeof(::app::registry::kModuleRegistry[0]),
              "kSlugs 与 kModuleRegistry 行数不一致（表序漂移）");

// 两数字符串（01..07）
std::string twoDigit(std::size_t n) {
    std::string s = std::to_string(n);
    return s.size() < 2 ? "0" + s : s;
}

// 全黑/纯色帧判据（采样 8px 网格；判据口径同 screenshot_probe ⑤）：
//  - 纯黑采样点占比 > 90% = 全黑帧（窗口没渲染 / 渲染后端没激活）
//  - 颜色桶（R/G/B 各取高 3 位）数 < 3 = 纯色帧（空窗 / 只有底色）
// 返回 true = 通过；失败时 stderr 打出两个实测值。
bool frameHasContent(const std::vector<unsigned char>& rgba, int width, int height) {
    std::size_t black = 0;
    std::size_t samples = 0;
    std::unordered_set<std::uint32_t> buckets;
    for (int y = 0; y < height; y += 8) {
        for (int x = 0; x < width; x += 8) {
            const unsigned char* px =
                rgba.data() +
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)) *
                    4u;
            ++samples;
            if (px[0] < 10 && px[1] < 10 && px[2] < 10) {
                ++black;
            }
            buckets.insert((static_cast<std::uint32_t>(px[0] >> 5)) |
                           (static_cast<std::uint32_t>(px[1] >> 5) << 3) |
                           (static_cast<std::uint32_t>(px[2] >> 5) << 6));
        }
    }
    const bool blackFrame = samples > 0 && black * 100 / samples > 90;
    const bool solidFrame = buckets.size() < 3;
    if (blackFrame || solidFrame) {
        std::cerr << "  FAIL——帧内容判据不过：黑点占比 " << black << "/"
                  << samples << "，颜色桶 " << buckets.size()
                  << (blackFrame ? "（全黑帧）" : "（纯色帧）") << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string outDir = "UAT62-screenshots";  // 缺省 = 当前工作目录下
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--out" && i + 1 < argc) {
            outDir = argv[++i];
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("uat62_page_shots", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 数据隔离（探针默认空状态）：空笔记目录 + 已迁移空库注册 appDatabase +
    // 关笔记目录 git 自动提交。全模块 onStart 吃这份空底座，不触真实用户数据。
    test_support::MdHeadlessEnv mdEnv("uat62shots_", "shots", "shots.db");

    // 7 个真模块：实例来自 kModuleRegistry 表内工厂，注册序 = 表序 = 侧边栏序
    ::core::Router router;
    std::vector<std::unique_ptr<::core::Module>> mods;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        std::unique_ptr<::core::Module> m(e.factory());
        router.registerModule(m.get());
        mods.push_back(std::move(m));
    }
    test_support::ScopedAppRouter scopedRouter(&router);
    router.startAll();

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    // 内联定义落地本 TU（见文件头说明）：GLFW 初始化后调用安全
    app::requestUpdate();
    app::requestShow();

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "uat62_page_shots: FrameCapture 建失败: "
                  << capture.error() << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "uat62_page_shots: 建输出目录失败 " << outDir << ": "
                  << ec.message() << "\n";
        router.stopAll();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 4;
    }

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    for (std::size_t i = 0; i < std::size(::app::registry::kModuleRegistry);
         ++i) {
        const auto& e = ::app::registry::kModuleRegistry[i];
        router.navigateTo(e.page);
        // 稳帧：24 帧（400ms @60fps）盖过仓库最长过渡时长——弹层透明度等由
        // animate().transition() 逐帧插值，只 poll 不 compose 树不更新，少跑会
        // 拍到半透明中间态。app::compose 每帧重建树。
        for (int f = 0; f < 24; ++f) {
            probe::composeModuleFrame(
                runtime, window, "shots", w, h,
                [&](eui::Ui& ui, const eui::Screen& screen) {
                    app::compose(ui, screen);
                });
        }
        const std::string path = outDir + "/" + twoDigit(i + 1) + "-" +
                                 kSlugs[i] + "-" +
                                 std::to_string(args.windowH) + ".png";
        std::vector<unsigned char> rgba;
        int pw = 0;
        int ph = 0;
        if (!capture.readFrame(runtime, rgba, pw, ph)) {
            passed = false;
            continue;
        }
        if (!frameHasContent(rgba, pw, ph)) {
            passed = false;
        }
        if (!probe::writeRgbaToPng(path, rgba.data(), pw, ph)) {
            passed = false;
            continue;
        }
        std::cout << "uat62_page_shots: " << path << " (" << pw << "x" << ph
                  << ")\n";
    }

    router.stopAll();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (!passed) {
        std::cerr << "uat62_page_shots: FAIL\n";
        return 1;
    }
    std::cout << "uat62_page_shots: PASS（" << args.windowW << "x"
              << args.windowH << "，7 页全落图）\n";
    return 0;
}
