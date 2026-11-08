// b1_material_probe.cpp — B1 材质登记表 qa 验收探针（池 64，F-64-2/F-64-3 取证）
//
// scratch 库播种 3 条 manual 任务（manual 不进调度、不触 LLM、不触用户数据根），
// 驱动真 AgentModule 页面合成，取任务卡三态截图并在进程内做像素断言：
//   --screenshot <png>         = 常态（无指针事件）
//   --screenshot-seeded <png>  = hover（指针 motion 到首卡内，无按键）
//   --screenshot-publish <png> = pressed（左键按住首卡未抬起；拍完才 Release）
// 断言（退出码裁决，不靠人眼看图）：
//   A1 三态可辨：首卡内同一采样点 normal/hover/pressed 三值两两不同
//   A2 控制点不漂：次卡内同一采样点三张图完全一致（状态不串卡）
// stdout 打印任务卡/面板 frame（逻辑 px）与四点 RGB 采样，供报表引用。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 agent_e2e_probe）
#include "test_support.h"

#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（探针不建应用级 UI，compose 直接驱动模块页）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_b1_material_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

// LLM 腿桩：探针只验材质视觉，任务恒 manual 不运行，profile 恒失败即可（不出网）
class NoLlmProfileSource final : public core::LlmProfileSource {
public:
    core::Result<core::LlmDirectProfile> currentProfile() override {
        return core::Result<core::LlmDirectProfile>::fail(
            core::ErrorCode::NotImplemented,
            "b1_material_probe 不需要 LLM 腿（材质验收不运行任务）");
    }
};

struct Sample {
    const char* tag;
    float lx, ly;  // 逻辑 px
};

}  // namespace

int main(int argc, char* argv[]) {
    // scratch 数据根最先设：core::paths 随它走，不碰用户真实根
    const std::filesystem::path scratchRoot =
        std::filesystem::temp_directory_path() /
        ("devdesk_b1_material_root_" +
         std::to_string(static_cast<int>(getpid())));
    std::error_code ec;
    std::filesystem::remove_all(scratchRoot, ec);
    std::filesystem::create_directories(scratchRoot, ec);
    test_support::setEnvVar("DEVDESK_DATA_ROOT", scratchRoot.string().c_str());

    const auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("b1_material_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    test_support::TestDb tdb("b1_", "agentflow");
    core::data::SqliteDb& db = tdb.handle();

    // 播种 3 条 manual 任务（manual 不进调度，绝不触 LLM）
    std::int64_t idA = 0, idB = 0;
    {
        module::agent::AgentStore seed(db);
        auto mk = [&](const char* name) -> std::int64_t {
            auto r = seed.createTask(name, "b1 材质验收占位指令（不运行）",
                                     "manual", "default", 1, false);
            return r.isOk() ? r.value() : 0;
        };
        idA = mk("B1验收任务甲");
        idB = mk("B1验收任务乙");
    }
    bool passed = idA != 0 && idB != 0;
    if (!passed) {
        std::cerr << "b1_material_probe: 任务播种失败（idA=" << idA
                  << " idB=" << idB << "）\n";
    }

    NoLlmProfileSource noLlm;
    module::agent::AgentModule::Deps deps;
    deps.db = &db;
    deps.profileSource = &noLlm;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "agent";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod.compose(u, screen);
                                  });
    };

    composeFrame();
    for (int i = 0; i < 5; ++i) composeFrame();

    // 取卡 frame（逻辑 px）。采样点取卡内右下空白：行内容在卡顶部条带，
    // 底部 padding 区无子元素，指针/采样都落在 .bg 上
    auto frameOf = [&](const std::string& id, float* x, float* y, float* cw,
                       float* ch) -> bool {
        const core::dsl::Element* e = ui ? ui->find(id) : nullptr;
        if (e == nullptr) return false;
        *x = e->frame.x;
        *y = e->frame.y;
        *cw = e->frame.width;
        *ch = e->frame.height;
        return true;
    };
    float x1 = 0, y1 = 0, w1 = 0, h1 = 0, x2 = 0, y2 = 0, w2 = 0, h2 = 0;
    float px0 = 0, py0 = 0, pw = 0, ph = 0;
    const bool f1ok = frameOf("agent.task." + std::to_string(idA) + ".bg",
                              &x1, &y1, &w1, &h1);
    const bool f2ok = frameOf("agent.task." + std::to_string(idB) + ".bg",
                              &x2, &y2, &w2, &h2);
    const bool pok = frameOf("agent.tasklist.panel", &px0, &py0, &pw, &ph);
    if (!f1ok || !f2ok || !pok) {
        std::cerr << "b1_material_probe: 元素缺失 card1=" << f1ok
                  << " card2=" << f2ok << " panel=" << pok << "\n";
        std::cerr << "FAIL\n";
        probe::terminateWindowBackend();
        return 3;
    }
    std::cout << "frame card1 " << x1 << " " << y1 << " " << w1 << " " << h1
              << "\n";
    std::cout << "frame card2 " << x2 << " " << y2 << " " << w2 << " " << h2
              << "\n";
    std::cout << "frame panel " << px0 << " " << py0 << " " << pw << " " << ph
              << "\n";

    // 卡内 hover/pressed 交互点（也是像素采样点）：卡底边上方 8px 水平居中
    const Sample hoverPt{"card1.inner", x1 + w1 * 0.5f, y1 + h1 - 8.0f};
    const Sample card2Pt{"card2.inner", x2 + w2 * 0.5f, y2 + h2 - 8.0f};
    // 卡 1/卡 2 之间缝隙 = 面板底（containerSurface）采样点。B3 起任务卡是网格
    //（并排），原「两卡 y 中点」公式只在单列堆叠下落在缝里；改取「卡 1 右侧 4px ×
    // 卡 1 垂直中点」——同行时落在列间缝、异行时落在卡 1 右侧空白，两条路都必然是
    // 面板底，不会误采进卡内。
    const Sample panelGapPt{"panel.gap", x1 + w1 + 4.0f, y1 + h1 * 0.5f};
    // 页面底候选点：窗口右下角内缩（B1 判据「卡外页面底」）
    const Sample pageBgPt{"page.bg", w - 12.0f, h - 12.0f};
    const Sample pts[] = {hoverPt, card2Pt, panelGapPt, pageBgPt};

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "b1_material_probe: FrameCapture 失败: "
                  << capture.error() << "\n";
        std::cerr << "FAIL\n";
        probe::terminateWindowBackend();
        return 4;
    }
    const float scale = capture.scale();
    // 小屏 / 高 DPI：显示器装不下请求的逻辑窗高时系统会钳窗口，按请求尺寸布局的
    // 底部采样点（page.bg 在 h-12）必然落在帧外——环境不可用 → 77，不判红
    //（与 home_probes N2 同口径；池123）。
    const float actualLogicalH =
        scale > 0.0f ? static_cast<float>(capture.pixelHeight()) / scale : 0.0f;
    if (static_cast<float>(args.windowH) > actualLogicalH + 1.0f) {
        std::cerr << "b1_material_probe: 环境不支持——请求 " << args.windowW << "x"
                  << args.windowH << "，系统把窗口钳到 " << capture.pixelWidth()
                  << "x" << capture.pixelHeight() << "px（逻辑高≈"
                  << actualLogicalH << "，显示器装不下）；像素判据跳过，返回 77\n";
        mod.onStop();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 77;
    }

    auto sampleRgb = [&](const std::vector<unsigned char>& rgba, int imgW,
                         int imgH, const Sample& pt) -> std::string {
        const int px = static_cast<int>(pt.lx * scale);
        const int py = static_cast<int>(pt.ly * scale);
        // x/y 两轴都要判：只判 x 曾让底部采样点把 py 当行下标越界（vector 断言崩溃，池123）
        if (px < 0 || py < 0 || px >= imgW || py >= imgH) return "oob";
        const std::size_t base = (static_cast<std::size_t>(py) * imgW + px) * 4u;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d,%d,%d", rgba[base], rgba[base + 1],
                      rgba[base + 2]);
        return buf;
    };

    // 稳帧数：24 帧（400ms@60fps）盖过 states 色过渡
    auto settle = [&]() {
        for (int i = 0; i < 24; ++i) composeFrame();
    };

    std::string rgb[3][4];  // [normal/hover/pressed][4 点]
    bool shotOk = true;
    auto grab = [&](int stateIdx, const std::string& pngPath) {
        std::vector<unsigned char> rgba;
        int iw = 0, ih = 0;
        if (!capture.readFrame(runtime, rgba, iw, ih)) {
            std::cerr << "b1_material_probe: readFrame 失败（state=" << stateIdx
                      << "）\n";
            shotOk = false;
            return;
        }
        for (int k = 0; k < 4; ++k) {
            rgb[stateIdx][k] = sampleRgb(rgba, iw, ih, pts[k]);
        }
        if (!pngPath.empty() &&
            !probe::writeRgbaToPng(pngPath, rgba.data(), iw, ih)) {
            std::cerr << "b1_material_probe: 落图失败: " << pngPath << "\n";
            shotOk = false;
        }
    };

    // ① 常态：无任何指针事件
    settle();
    grab(0, args.screenshotPath);

    // ② hover：指针 motion 到首卡内，无按键
    const core::KeyModifiers noMods;
    core::queuePointerMotion(window, hoverPt.lx, hoverPt.ly,
                             core::PointerButtons{}, noMods);
    settle();
    grab(1, args.screenshotSeedPath);

    // ③ pressed：左键按住不抬（onClick 未触发，详情弹窗不会打开），拍完才 Release
    core::queuePointerButton(window, hoverPt.lx, hoverPt.ly,
                             core::PointerButton::Left,
                             core::PointerAction::Press, noMods);
    settle();
    grab(2, args.screenshotPublishPath);
    core::queuePointerButton(window, hoverPt.lx, hoverPt.ly,
                             core::PointerButton::Left,
                             core::PointerAction::Release, noMods);
    composeFrame();

    // A3 点击链路未被果冻劫持（池 bug232 回归守卫）：hover+press+release 后
    // 详情弹窗应入 DOM。果冻动画期 .bg 若参与命中（states() 隐式 interactive
    // 未关）会顶替 .hit 的 onClick → 本断言红而 A1 也同时红。
    for (int i = 0; i < 6; ++i) composeFrame();
    const bool a3 = ui != nullptr && ui->find("agent.dialog.detail") != nullptr;
    if (!a3) {
        std::cerr << "b1_material_probe: A3 FAIL 点击卡片未打开详情弹窗"
                     "（动画期命中被 .bg 劫持？）\n";
        passed = false;
    } else {
        std::cout << "A3 PASS 点击链路正常（详情弹窗入 DOM）\n";
    }

    if (!shotOk) passed = false;

    // 断言输出
    static const char* stateNames[3] = {"normal", "hover", "pressed"};
    for (int s = 0; s < 3; ++s) {
        for (int k = 0; k < 4; ++k) {
            std::cout << "rgb " << stateNames[s] << " " << pts[k].tag << " "
                      << rgb[s][k] << "\n";
        }
    }
    // A1 三态可辨：card1.inner 三值两两不同
    const std::string& n = rgb[0][0];
    const std::string& hv = rgb[1][0];
    const std::string& pr = rgb[2][0];
    const bool a1 = !n.empty() && n != hv && hv != pr && n != pr;
    if (!a1) {
        std::cerr << "b1_material_probe: A1 FAIL 三态不可辨 normal=" << n
                  << " hover=" << hv << " pressed=" << pr << "\n";
        passed = false;
    } else {
        std::cout << "A1 PASS 三态可辨 normal=" << n << " hover=" << hv
                  << " pressed=" << pr << "\n";
    }
    // A2 控制点不漂：card2.inner 三态一致
    const bool a2 = rgb[0][1] == rgb[1][1] && rgb[1][1] == rgb[2][1] &&
                    !rgb[0][1].empty();
    if (!a2) {
        std::cerr << "b1_material_probe: A2 FAIL 控制点漂移 card2 normal="
                  << rgb[0][1] << " hover=" << rgb[1][1]
                  << " pressed=" << rgb[2][1] << "\n";
        passed = false;
    } else {
        std::cout << "A2 PASS 控制点不漂 card2.inner=" << rgb[0][1] << "\n";
    }
    // F-64-3 佐证：面板底 vs 卡内 ≠（L1 containerSurface ≠ L2 itemSurface）
    if (rgb[0][0] != rgb[0][2]) {
        std::cout << "LAYER PASS panel.gap=" << rgb[0][2]
                  << " != card1.inner=" << rgb[0][0] << "（分层生效）\n";
    } else {
        std::cerr << "b1_material_probe: LAYER WARN panel.gap == card1.inner="
                  << rgb[0][0] << "（L1/L2 采样同色，需人工核对）\n";
    }

    mod.onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "b1_material_probe: PASS\n";
    return 0;
}
