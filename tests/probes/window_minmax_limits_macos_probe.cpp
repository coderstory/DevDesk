// window_minmax_limits_macos_probe.cpp — 窗口 minSize/maxSize 原生传导门禁探针
//（DevDesk need269；macOS 专探：_macos_probe 后缀 = 仅 APPLE 编，tests/CMakeLists.txt
//  平台门控，Windows 不编译不登记）
//
// 为什么断言面是 NSWindow contentMin/MaxSize 而不是「程序化缩窗被钳」：
// macOS AppKit 对程序化 setContentSize **不**强制 contentMin/MaxSize（实测
// 2026-11-20：createWindow 请求 200x100 + min 720x480，创建后与程序化
// setWindowSize 后 glfwGetWindowSize 均原样 200x100，可见/隐藏窗一致）——limits
// 只在用户拖拽路径生效，而真机拖拽交互禁 computer-use。离屏探针可自动化断言的
// 是「四字段真实落到原生窗口」：core::window::nativeWindowInfo().platformWindow
// （=glfwGetCocoaWindow 的 NSWindow*）→ objc_msgSend 读 contentMinSize /
// contentMaxSize，即 GLFW _glfwSetWindowSizeLimitsCocoa 的落点状态
//（3rd/glfw/src/cocoa_window.m:1101：min 不约束=(0,0)，max 不约束=写 DBL_MAX、
// AppKit float 位宽落 FLT_MAX）。
// Windows 侧 WM_GETMINMAXINFO 连程序化 resize 一起钳，彼侧可另断「程序化缩窗被
// 钳回下限」（_win_probe 变体；本机 mac 未写未验，清单外建议）。
//
//   A DSL→请求数据链：DslWindowConfig.minSize/maxSize builder → openWindow 入队 →
//     consumeWindowRequests 四字段（含负值归 0；与 EUI-NEO pet_window_config 同
//     断言面，DevDesk 侧经同一 impl 头复证消费端视角）
//   B 原生传导：createWindow 带四约束字段 + 低于下限的请求尺寸 → 原生窗口
//     contentMinSize=(720,480) / contentMaxSize=(1920,1080)；无约束对照窗读出
//     (0,0)/(DBL_MAX,DBL_MAX)（= 不传导，零回归基线）
// 运行：window_minmax_limits_macos_probe（窗口机制探针，无渲染帧，不支持
// --screenshot）。
#include "probes/probe_harness.h"

#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（openWindow=入队）

#include <cfloat>
#include <CoreGraphics/CGGeometry.h>  // CGSize / CGFloat（NSSize 同构）
#include <objc/message.h>
#include <objc/objc.h>
#include <iostream>
#include <string>
#include <vector>

// app:: 层桩（probe 不建应用级 UI；同 notepad_probe 口径）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_minmax_limits_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

bool fail(const std::string& what) {
    std::cerr << "window_minmax_limits_macos_probe: FAIL — " << what << "\n";
    return false;
}

bool check(bool ok, const std::string& what) {
    if (!ok) {
        return fail(what);
    }
    return true;
}

// objc_msgSend 的 CGSize 返回位（arm64 ABI：16 字节结构走 d0/d1 寄存器，经函数
// 指针类型声明即正确收值；同 window_visibility_macos.cpp 的纯 C++ 翻译单元口径，
// 不 import AppKit）
using SizeMsgSend = CGSize (*)(id, SEL);
const SizeMsgSend sizeMsgSend = reinterpret_cast<SizeMsgSend>(objc_msgSend);

bool sizeEquals(CGSize actual, double w, double h, const std::string& what) {
    return check(actual.width == w && actual.height == h,
                 what + "（实得 " + std::to_string(actual.width) + "x" +
                     std::to_string(actual.height) + "）");
}

// 读原生窗口的尺寸约束状态
bool nativeLimits(core::window::Handle window, CGSize& minSize, CGSize& maxSize,
                  const std::string& what) {
    const core::window::NativeWindowInfo info = core::window::nativeWindowInfo(window);
    if (!check(info.platformWindow != nullptr, what + ": nativeWindowInfo 未取到 NSWindow")) {
        return false;
    }
    const id nsWindow = static_cast<id>(info.platformWindow);
    minSize = sizeMsgSend(nsWindow, sel_registerName("contentMinSize"));
    maxSize = sizeMsgSend(nsWindow, sel_registerName("contentMaxSize"));
    return true;
}

// A 段：DSL 数据链（复用 EUI-NEO pet_window_config 的断言面）
bool dslRequestChain() {
    app::openWindow(app::DslWindowConfig{}.windowSize(200, 100).minSize(720, 480).maxSize(1920, 1080),
                    [](eui::Ui&, const eui::Screen&) {});
    std::vector<app::DslWindowRequest> bounded = app::consumeWindowRequests();
    if (!check(bounded.size() == 1, "A: bounded 请求队列应恰 1 条")) {
        return false;
    }
    if (!check(bounded[0].width == 200 && bounded[0].height == 100,
               "A: 请求尺寸应保持 200x100（约束不改请求尺寸，钳制归后端）")) {
        return false;
    }
    if (!check(bounded[0].minWidth == 720 && bounded[0].minHeight == 480 &&
                   bounded[0].maxWidth == 1920 && bounded[0].maxHeight == 1080,
               "A: minSize/maxSize 应透传到 DslWindowRequest 四字段")) {
        return false;
    }

    // 未设上限 = 不约束（0），负值在转换层归 0（与后端「>0 才生效」判据对齐）
    app::openWindow(app::DslWindowConfig{}.minSize(-5, -1), [](eui::Ui&, const eui::Screen&) {});
    std::vector<app::DslWindowRequest> neg = app::consumeWindowRequests();
    if (!check(neg.size() == 1, "A: negative 请求队列应恰 1 条")) {
        return false;
    }
    if (!check(neg[0].minWidth == 0 && neg[0].minHeight == 0 && neg[0].maxWidth == 0 &&
                   neg[0].maxHeight == 0,
               "A: 负值/未设应归 0（= 不约束）")) {
        return false;
    }
    return true;
}

// B 段：原生传导（GLFW size limits 生效路径，NSWindow 落点状态）
bool nativeLimitsPropagation() {
    // 约束窗：请求 200x100（低于下限）+ min 720x480 / max 1920x1080
    core::window::WindowCreateRequest req;
    req.title = "need269_min_clamp";
    req.width = 200;
    req.height = 100;
    req.minWidth = 720;
    req.minHeight = 480;
    req.maxWidth = 1920;
    req.maxHeight = 1080;
    req.resizable = true;
    // 默认隐藏建窗（不弹屏不抢焦点，离屏探针纪律；见 probeWindowVisible 注释）
    req.visible = probe::probeWindowVisible();
    req.focusOnShow = req.visible;
    req.renderApi = probe::configuredRenderApi();
    core::window::Handle limited = core::window::createWindow(req);
    if (!check(limited != nullptr, "B: 约束窗创建失败（金丝雀已过 → 非环境问题）")) {
        return false;
    }

    CGSize minSize = {0, 0};
    CGSize maxSize = {0, 0};
    bool ok = nativeLimits(limited, minSize, maxSize, "B: 约束窗") &&
              sizeEquals(minSize, 720, 480, "B: 约束窗 contentMinSize 应为 720x480") &&
              sizeEquals(maxSize, 1920, 1080, "B: 约束窗 contentMaxSize 应为 1920x1080");
    core::window::destroyWindow(limited);
    if (!ok) {
        return false;
    }

    // 对照窗：不传约束 = 无限制（零回归基线：GLFW_DONT_CARE → cocoa 落 (0,0) 与
    // (DBL_MAX,DBL_MAX)，见 cocoa_window.m:1101）
    core::window::WindowCreateRequest plain;
    plain.title = "need269_unbounded_control";
    plain.width = 200;
    plain.height = 100;
    plain.resizable = true;
    plain.visible = probe::probeWindowVisible();
    plain.focusOnShow = plain.visible;
    plain.renderApi = probe::configuredRenderApi();
    core::window::Handle control = core::window::createWindow(plain);
    if (!check(control != nullptr, "B: 对照窗创建失败")) {
        return false;
    }
    ok = nativeLimits(control, minSize, maxSize, "B: 对照窗") &&
         sizeEquals(minSize, 0, 0, "B: 对照窗 contentMinSize 应为 (0,0)=不约束") &&
         sizeEquals(maxSize, FLT_MAX, FLT_MAX,
                    "B: 对照窗 contentMaxSize 应为不约束（GLFW 写 DBL_MAX，AppKit "
                    "内部 float 位宽落 FLT_MAX——2026-11-20 实测锚）");
    core::window::destroyWindow(control);
    return ok;
}

}  // namespace

int main() {
    if (!probe::initializeWindowBackend()) {
        return 1;
    }

    bool passed = dslRequestChain();
    // A 段失败不阻断 B 段：两段独立取证，报告里都出
    const bool nativeOk = nativeLimitsPropagation();
    passed = passed && nativeOk;

    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "window_minmax_limits_macos_probe: FAIL\n";
        return 1;
    }
    std::cout << "window_minmax_limits_macos_probe: PASS（A DSL→请求四字段透传；"
                 "B 原生窗口 contentMin/MaxSize 落点 = 720x480 / 1920x1080，"
                 "对照窗不传导基线成立）\n";
    return 0;
}
