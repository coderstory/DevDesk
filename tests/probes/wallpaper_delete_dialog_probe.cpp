// tests/probes/wallpaper_delete_dialog_probe.cpp — need356 F-02 删除确认弹窗真渲染探针
//
// 为什么必须有这个 probe（code-review 约束轴 P0）：删除确认弹窗是本需求唯一
// 破坏性交互，headless 单测只能断言「开态布尔 + 文案纯函数」，画没画出来、
// z=1200 有没有被页内 overlay 截胡、正文两行几何是否溢出/重叠，一条都没验过。
// 本 probe 用真 Runtime 渲一帧落 PNG，老板据此目检。
//
// 覆盖断言（结构 + 落图）：
//   ① 弹窗节点树在位：app.wallpaperdelete.title / .msg.name / .msg.hint /
//      .cancel / .confirm 全存在（drawDialogShell 的 id 契约）。
//   ② 标题按视频/图片分派：视频态「把当前视频移到废纸篓？」、图片态含「图片」。
//   ③ 主钮为危险变体（dangerConfirm）：confirm 节点底色 = error 系（红），
//      与普通主钮区分——破坏性动作必须视觉可辨。
//   ④ 落图两张：视频态 + 图片态（文案分派肉眼可对）。
//
// 已知边界（不在本 probe 覆盖）：托盘右键菜单本身是 EUI 独立菜单窗（经
// app::openWindow 在主循环创建），probe 自成帧循环渲不到；托盘 8 项文案/顺序
// 由 tests/test_modules/test_wallpaper_tray.cpp 的 spec 结构断言锁定
//（含 {4} 删除项文案分派 + 0xF1F8 图标码点）。真机托盘视觉归人工验收。
//
// 运行：wallpaper_delete_dialog_probe [--shot-video <png>] [--shot-image <png>]
// 无开关 = 只跑结构断言（供 --dev 内循环）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 md_scrim_probe）

#include "core/theme.h"
#include "wallpaper_delete_bridge.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

// app:: 层空桩（同 agent_dialog_probe / md_scrim_probe）：probe 不建应用级 UI，
// 帧由 probe::composeModuleFrame 直接驱动 runtime；手写 bridge 的 drawDialog
// 只依赖 core/ui_kit，不需要 app_sidebar / app_assembly 参与链接。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_wallpaper_delete_dialog_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

constexpr int kSkipExitCode = 77;

// 主钮危险色判据：不钉精确值（换肤不改语义），只要求「红通道显著高于绿/蓝」。
bool looksErrorRed(const eui::Color& c) {
    return c.r > 0.55f && c.r > c.g * 1.6f && c.r > c.b * 1.6f;
}

}  // namespace

int main(int argc, char** argv) {
    std::string shotVideo;
    std::string shotImage;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-video" && i + 1 < argc) {
            shotVideo = argv[++i];
        } else if (arg == "--shot-image" && i + 1 < argc) {
            shotImage = argv[++i];
        }
    }

    if (!probe::initializeWindowBackend()) return kSkipExitCode;
    auto window = probe::makeWindow("wallpaper_delete_dialog_probe", 1200, 800);
    if (!window) {
        probe::terminateWindowBackend();
        return kSkipExitCode;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    const auto [logicalW, logicalH] = probe::windowLogicalSize(window);
    const float w = logicalW > 1.0f ? logicalW : 1200.0f;
    const float h = logicalH > 1.0f ? logicalH : 800.0f;

    bool passed = true;

    // 注入目标 provider（弹窗标题/正文按它分派）+ 计数确认回调（probe 不执行删除；
    // 计数只用于断言「没被误触发」）。resetForTest 会清 handlers，故每个状态点前重注。
    static app::wallpaper_delete::DeleteTarget gTarget;
    static int gConfirmCalls = 0;
    const auto injectHandlers = [] {
        app::wallpaper_delete::setHandlers(
            +[]() -> app::wallpaper_delete::DeleteTarget { return gTarget; },
            +[]() { ++gConfirmCalls; });
    };
    injectHandlers();

    eui::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, "wallpaper-delete-dialog", w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      app::wallpaper_delete::drawDialog(u, screen);
                                  });
    };

    // 一个状态点：置目标 → 开弹窗 → 稳帧 → 结构断言 →（可选）落图。
    auto runState = [&](const char* label, const std::string& fileName, bool image,
                        const std::string& shotPath, const char* expectTitlePart) {
        gTarget.available = true;
        gTarget.fileName = fileName;
        gTarget.image = image;
        injectHandlers();  // 上一状态点 resetForTest 清了 handlers
        app::wallpaper_delete::openConfirm();
        if (!app::wallpaper_delete::dialogOpenForTest()) {
            std::cerr << "probe: [" << label << "] 弹窗未打开\n";
            passed = false;
            return;
        }
        composeFrame();
        for (int i = 0; i < 24; ++i) probe::pollOnce(window, runtime);

        // ① 节点树契约（drawDialogShell 的 id 约定）
        const char* required[] = {"app.wallpaperdelete.title",
                                  "app.wallpaperdelete.msg.name",
                                  "app.wallpaperdelete.msg.hint",
                                  "app.wallpaperdelete.cancel",
                                  "app.wallpaperdelete.confirm"};
        for (const char* id : required) {
            if (ui->find(id) == nullptr) {
                std::cerr << "probe: [" << label << "] 节点缺失 " << id << "\n";
                passed = false;
            }
        }

        // ② 标题按类型分派
        if (const auto* title = ui->find("app.wallpaperdelete.title")) {
            const std::string text = title->text;
            if (text.find(expectTitlePart) == std::string::npos) {
                std::cerr << "probe: [" << label << "] 标题不符预期（应含「"
                          << expectTitlePart << "」），实际=" << text << "\n";
                passed = false;
            } else {
                std::cout << "probe: [" << label << "] 标题=" << text << "\n";
            }
        }

        // ③ 主钮危险变体：底色应为 error 红
        if (const auto* confirmBg = ui->find("app.wallpaperdelete.confirm.bg")) {
            const eui::Color bg = confirmBg->color;
            if (!looksErrorRed(bg)) {
                std::cerr << "probe: [" << label << "] 主钮底色非危险红 rgba=("
                          << bg.r << "," << bg.g << "," << bg.b << "," << bg.a << ")\n";
                passed = false;
            } else {
                std::cout << "probe: [" << label << "] 主钮危险红确认\n";
            }
        } else {
            std::cerr << "probe: [" << label << "] 主钮 bg 节点缺失（按钮未渲染）\n";
            passed = false;
        }

        // ④ 落图（目录不存在先建——evidence/ 下按需求分目录，CI 首跑时为空）
        if (!shotPath.empty() && passed) {
            std::error_code dirEc;
            const auto parent = std::filesystem::path(shotPath).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent, dirEc);
            probe::FrameCapture capture(window);
            if (!capture.ok()) {
                std::cerr << "probe: [" << label << "] 帧捕获失败: " << capture.error()
                          << "\n";
                passed = false;
            } else {
                std::vector<unsigned char> rgba;
                int cw = 0;
                int ch = 0;
                if (!capture.readFrame(runtime, rgba, cw, ch, 16) ||
                    !probe::writeRgbaToPng(shotPath, rgba.data(), cw, ch)) {
                    std::cerr << "probe: [" << label << "] 落图失败\n";
                    passed = false;
                } else {
                    std::cout << "probe: [" << label << "] screenshot " << shotPath
                              << " (" << cw << "x" << ch << ")\n";
                }
            }
        }
        app::wallpaper_delete::resetForTest();
    };

    runState("video", "20260101_wallpaper_demo.mp4", false, shotVideo, "视频");
    runState("image", "20260101_wallpaper_demo.png", true, shotImage, "图片");

    // 决定性断言：只开弹窗（没点主钮）绝不允许触发确认回调——误触发 = 直接删文件。
    if (gConfirmCalls != 0) {
        std::cerr << "probe: 仅打开弹窗就触发了确认回调 " << gConfirmCalls << " 次\n";
        passed = false;
    }

    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    std::cout << (passed ? "probe: PASS" : "probe: FAIL") << "\n";
    return passed ? 0 : 1;
}
