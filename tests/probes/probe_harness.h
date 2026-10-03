#pragma once
// probe_harness.h — DevDesk probes 基础设施（header-only，namespace probe）
// 用法：probe main 直接 include 本头，按「初始化后端 → makeWindow →
// 帧循环（pollOnce / composeModuleFrame）→ 扫描断言」编排。仅用 EUI-NEO core
// 层 API；唯一 DevDesk 侧依赖是 ui_kit 的 splash 测试缝（initializeWindowBackend
// 里禁 splash，probe dump 不该拍到 splash——compose 路径同 app_sidebar）。
//
// ==================== 帧捕获：--screenshot（进程内 glReadPixels → PNG）======
// 为什么要它：macOS 的 `screencapture` 需要「屏幕录制」TCC 授权，而 agent 会话
// 由 launchd 拉起、不在终端的进程树下 → 拿不到授权，只能给几何断言没人能看图。
// **进程内 glReadPixels 读自己的 GL 后备缓冲不需要任何系统授权**，是唯一能自动
// 化的看图路径（PNG 编码走 EUI-NEO 已 vendored 的 libpng 简化 API）。
//
// 用法（主循环任意「当前状态值得看图」处一行）：
//     auto args = probe::parseProbeArgs(argc, argv);   // 已支持 --screenshot <路径>
//     ... 跑完帧循环 / 断言 ...
//     if (!probe::captureIfRequested(args, window, runtime, &passed)) passed = false;
//     // 不传 --screenshot 时 captureIfRequested 立刻返回 true：零开销、不写文件、
//     // 不改退出码（既有 probe 行为逐字不变）。
//
// 多张图 / 想直接拿内存像素做像素断言 → 用会话对象：
//     probe::FrameCapture capture(window);                       // 建渲染后端（一次即可）
//     capture.capturePng(runtime, "a.png");                      // 渲染一帧 → 落盘
//     capture.readFrame(runtime, rgba, w, h);                    // 渲染一帧 → 内存（top-down）
//
// 两条铁律（都来自 about_probe/claude_probes 的实测教训）：
//   ① 探针常规帧循环只 update/compose **不 render**（composeModuleFrame 不画），
//      本设施自己走「beginFrame → runtime.render → glReadPixels → present」，
//      不自己画就只会拍到空窗黑图；
//   ② glReadPixels 必须在 present() **之前**取——swap 之后后备缓冲内容未定义。
//
// 头文件级依赖：libpng（`<png.h>`，简化 API png_image_write_to_file）与 glad。
// 两者都靠 tests/CMakeLists.txt 里探针 target 显式 link `PNG::PNG` 传递
// include 路径——EUI-NEO 已 vendored libpng 1.6.43 并在 `PNG::PNG` 别名导出，
// 这里不引入任何新 vendored 头。

#include "core/dsl_runtime.h"
#include "core/input/input_state.h"
#include "core/render/render_backend.h"
#include "core/theme.h"
#include "core/window/window_backend.h"

#include <ui_kit/splash.h>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
// glad（EUI-NEO OpenGL 后端同款，render 已由它 load 过 loader）。⚠ 探针 TU 里
// **不要**在本头之前 include 不带 GLFW_INCLUDE_NONE 的 <GLFW/glfw3.h> —— 那会
// 先拉进平台 GL 头，而 glad.h 头两行就是 `#error OpenGL header already
// included`（两者互斥，实测踩过）。GLFW / GL 符号一律由本头统一给。
#include <glad/glad.h>
#include <png.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <utility>

namespace probe {

/// 本工程（DevDesk）统一 GLFW + OpenGL 渲染配置，恒返回 OpenGL。
inline core::window::RenderApi configuredRenderApi() {
    return core::window::RenderApi::OpenGL;
}

// ==================== 无显示环境检测（CI runner 无 GUI 会话） ====================
//
// ── 事故（2026-10-03）────────────────────────────────────────────────
// GitHub 的 macOS runner 上跑的进程**不挂在 WindowServer 的 bootstrap
// namespace 里**，probe 建不出窗口 → 首次并入门禁的 14 个 probe 全红，
// 而本机 14/14 全绿。本文件把「环境没显示」从**红色失败**改成
// **诚实的跳过**（ctest SKIP_RETURN_CODE 77），但**绝不是**「建窗失败一律
// 跳过」——那等于把门禁关掉，真代码 bug 会长得和没显示一模一样。
//
// ── 判据 = 「金丝雀窗口 + GLFW 错误码」两段，都用裸 GLFW 判定 ──────
//
// 【为什么金丝雀能区分】金丝雀窗口由**裸 GLFW** 建（下面 probeDisplayCapability
// 里那十几行），**不碰任何 DevDesk / EUI-NEO 代码**：尺寸、标题、GL 版本全是
// 编译期常量。所以「金丝雀建不出来」这件事**在因果上不可能**由我们的代码 bug
// 造成 —— 它只反映「本进程能不能造出与 probe 同等要求的窗口」。
// 反过来：金丝雀建成了而真探针窗建不出来，那**必然**是代码 / 配置问题
// （我们的 hint、GL 版本请求、断言……），此时必须报红。两者构成正反对照。
//
// 【为什么还要看错误码】金丝雀建不出来不等于一定是环境：GLFW 的
// INVALID_VALUE / INVALID_ENUM / NOT_INITIALIZED 表示**调用方**把参数写错了
// （那是 harness 自己的 bug），OUT_OF_MEMORY 是资源问题。只有「平台/能力不可用」
// 那一族才是环境。把这一族单列出来，判据才不是恒真的。
//
// 错误码取值与语义出处（均读自 EUI-NEO/3rd/glfw/include/GLFW/glfw3.h）：
//   GLFW_PLATFORM_UNAVAILABLE 0x0001000E  glfw3.h:849「没有可用的窗口系统」
//   GLFW_PLATFORM_ERROR       0x00010008  glfw3.h:762「平台 API 调用失败」
//                                          （macOS 无 GUI 会话时 Cocoa 建窗失败走这条，
//                                            见 3rd/glfw/src/cocoa_window.m:833）
//   GLFW_API_UNAVAILABLE      0x00010006  glfw3.h:734「请求的客户端 API 不可用」
//   GLFW_VERSION_UNAVAILABLE  0x00010007  glfw3.h:751「请求的 GL 版本不支持」
//                                          （无 GPU 的 runner 只有 GDI 泛型 GL 1.1）
//   GLFW_FORMAT_UNAVAILABLE   0x00010009  glfw3.h:781「没有满足的 framebuffer 配置」
// **不**归环境管的：INVALID_VALUE / INVALID_ENUM / NOT_INITIALIZED /
// OUT_OF_MEMORY / NO_ERROR —— 一律 RealFailure（红）。

/// CTest 的「跳过」返回码约定。tests/CMakeLists.txt 里每个 probe 测试都必须配
/// SKIP_RETURN_CODE 77（由 scripts/check-probe-skip-gate.sh 守卫），否则 77
/// 会被 ctest 当成普通失败。
inline constexpr int kSkipExitCode = 77;

/// 金丝雀的探测结果。纯数据、无副作用 → 可被 screenshot_probe 的分类表断言
/// 直接喂构造值（那里不需要真窗口就能验判据不是恒真的）。
struct DisplayCapability {
    /// glfwInit() 是否成功（false 时后面两项无意义）
    bool initialized = false;
    /// 金丝雀窗口是否建成
    bool canaryCreated = false;
    /// 失败时的 GLFW 错误码；成功路径恒为 GLFW_NO_ERROR
    int glfwError = GLFW_NO_ERROR;
};

enum class DisplayVerdict {
    /// 环境具备显示能力：此后 probe 的任何失败都是代码 / 配置问题 → 必须红
    Available,
    /// 环境不具备显示能力（无 GUI 会话 / 无 GL）→ 诚实跳过（77）
    NoDisplay,
    /// 不是显示环境造成的失败（参数写错、资源不足……）→ 真失败，必须红
    RealFailure,
};

/// GLFW 错误码 → 「是否属于环境没显示/没 GL 那一族」。
/// ⚠ 这条判据**不是恒真的**：下面 screenshot_probe 的分类表断言把「真代码
/// bug 形状的错误码」逐个喂进来，断言它们**不**判成 NoDisplay。
inline bool isDisplayEnvironmentError(int code) {
    switch (code) {
        case GLFW_PLATFORM_UNAVAILABLE:
        case GLFW_PLATFORM_ERROR:
        case GLFW_API_UNAVAILABLE:
        case GLFW_VERSION_UNAVAILABLE:
        case GLFW_FORMAT_UNAVAILABLE:
            return true;
        default:
            return false;
    }
}

/// 金丝雀结果 → 三态判定。**纯函数**（不碰 GLFW 运行时），可单测。
inline DisplayVerdict classifyDisplayCapability(const DisplayCapability& cap) {
    if (!cap.initialized) {
        // 连 GLFW 都没起来：只有「平台不可用」才算环境，否则是 harness 自己的 bug
        return isDisplayEnvironmentError(cap.glfwError) ? DisplayVerdict::NoDisplay
                                                        : DisplayVerdict::RealFailure;
    }
    if (cap.canaryCreated) {
        return DisplayVerdict::Available;
    }
    return isDisplayEnvironmentError(cap.glfwError) ? DisplayVerdict::NoDisplay
                                                    : DisplayVerdict::RealFailure;
}

inline const char* displayVerdictName(DisplayVerdict v) {
    switch (v) {
        case DisplayVerdict::Available: return "Available";
        case DisplayVerdict::NoDisplay: return "NoDisplay";
        case DisplayVerdict::RealFailure: return "RealFailure";
    }
    return "?";
}

/// 金丝雀窗口名（出现在 CI 日志里，便于一眼认出这是环境探测而非真探针）
inline constexpr const char* kDisplayCanaryTitle = "devdesk-probe-display-canary";

/// 跑一次金丝雀：用**裸 GLFW** 建一个隐藏的、与 probe 同等 GL 要求的窗口。
/// 全程不引用 DevDesk / EUI-NEO 的窗口代码 —— 判据因此独立于被测代码。
/// 跑完把 hints 复位（hint 会跨 glfwCreateWindow 残留），让随后的真探针窗
/// 从干净状态开始。
inline DisplayCapability probeDisplayCapability() {
    DisplayCapability cap;
    if (glfwInit() != GLFW_TRUE) {
        const char* desc = nullptr;
        cap.glfwError = glfwGetError(&desc);
        return cap;
    }
    cap.initialized = true;

    // GL 要求与 EUI-NEO 的 configureOpenGLWindowHints（core/window/
    // window_backend.cpp:758）对齐：3.3 core。**故意不调用**那个函数 —— 一旦
    // 判据依赖被测框架自己的代码，它就不再是独立的对照实验了。
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // 探测窗不弹到屏幕上
    GLFWwindow* canary =
        glfwCreateWindow(64, 64, kDisplayCanaryTitle, nullptr, nullptr);
    if (canary != nullptr) {
        cap.canaryCreated = true;
        glfwDestroyWindow(canary);
    } else {
        const char* desc = nullptr;
        cap.glfwError = glfwGetError(&desc);
    }
    glfwDefaultWindowHints();   // 复位：hint 跨 createWindow 残留
    return cap;
}

/// 窗口后端生命周期（GLFW），成对调用。
/// 无显示环境 → 打印可进 CI 日志的原因 + 以 77 退出（ctest 记 Skipped）；
/// 其它初始化失败 → 返回 false（调用方返回非零 → ctest 记 Failed）。
inline bool initializeWindowBackend() {
    // splash 测试缝：probe 进程同样活不过 splash 2.3s 时序，禁掉后 compose
    // 路径（若后续 probe 直连 app::compose / drawSidebar）不会拍到 splash
    ::ui_kit::setSplashDisabledForTests(true);
    const DisplayCapability cap = probeDisplayCapability();
    switch (classifyDisplayCapability(cap)) {
        case DisplayVerdict::NoDisplay:
            // cerr 是 unitbuf 的，但 std::exit 不跑析构 / 不 flush 其它流 ——
            // 显式 flush，保证「为什么跳过」一定落进 ctest 日志。
            std::cerr << "PROBE_SKIP_NO_DISPLAY: 本进程没有可用的显示环境"
                         "（金丝雀窗口建不出来，glfwError=0x"
                      << std::hex << cap.glfwError << std::dec
                      << " → 判定 NoDisplay）——本 probe 需要真窗口，跳过。"
                         "在有 GUI 会话的机器上跑同一条命令即可真跑："
                         "cd <build> && ctest -L probe --output-on-failure\n";
            std::cerr.flush();
            std::exit(kSkipExitCode);
        case DisplayVerdict::RealFailure:
            std::cerr << "probe::initializeWindowBackend: 窗口后端初始化失败，"
                         "且**不**是无显示环境造成的（glfwError=0x"
                      << std::hex << cap.glfwError << std::dec
                      << "，判定 RealFailure）——这是代码问题，门禁应当报红\n";
            return false;
        case DisplayVerdict::Available:
            break;
    }
    return true;
}

inline void terminateWindowBackend() {
    glfwTerminate();
}

/// 创建探针窗口：不可缩放 + OpenGL；失败返回 nullptr。
///
/// ⚠ 金丝雀此时**已经**建成了（initializeWindowBackend 里），所以这里的失败
/// **不是**环境问题：能造出金丝雀窗却造不出这个窗，差的就是我们的 hint /
/// GL 版本请求 / 窗口参数。诊断信息里明确写出这一点，避免下次再被误判成
/// 「CI 环境问题」而放过真 bug。
inline core::window::Handle makeWindow(const std::string& title, int width, int height) {
    core::window::WindowCreateRequest request;
    request.title = title.c_str();
    request.width = width;
    request.height = height;
    request.resizable = false;
    request.renderApi = configuredRenderApi();
    core::window::Handle handle = core::window::createWindow(request);
    if (handle == nullptr) {
        const char* desc = nullptr;
        const int code = glfwGetError(&desc);
        std::cerr << "probe::makeWindow(" << title << ", " << width << "x" << height
                  << "): 窗口创建失败，glfwError=0x" << std::hex << code << std::dec
                  << "（" << (desc != nullptr ? desc : "(无描述)") << "）\n"
                  << "  ⚠ 显示环境金丝雀已建成 → 本次失败**不是**环境问题，"
                     "是代码/配置问题（探针不应跳过，应报红）\n";
    }
    return handle;
}

/// 命令行探针参数。
struct ProbeArgs {
    int frames = 3;          // 渲染帧数
    int windowW = 1200;      // 窗口宽（逻辑像素）
    int windowH = 1000;      // 窗口高
    bool manual = false;     // 手动驱动模式
    // --screenshot <路径>：非空 = 该 probe 在「当前状态点」把真实渲染帧落成 PNG
    // （见文件头「帧捕获」段）。**空 = 既有行为逐字不变**（不建渲染后端、不渲染、
    // 不写文件、不改退出码）。
    std::string screenshotPath;
    // --screenshot-seeded <路径>：**中途**状态点落图（与 screenshotPath 相互
    // 独立、两个可同时给）。存在的理由：探针收尾前常会做「破坏性收尾断言」
    // （home_probes 组 H(b) 会清空余量表再断言），退出点那张图拍到的是**收尾后**
    // 的空态；本开关让探针能在「数据还在」的中途点另拍一张。
    // 空 = 零开销，行为与从前逐字一致。
    std::string screenshotSeedPath;
    // --quota-fixture <路径>：**home_probes 专用**（其余 probe 忽略）。让余量卡
    // 种子改用一份「真机落下的 JSON」（quota_live_probe --dump 的产物），从而
    // 「真网络 → 真落库 → 真渲染 → 真落图」这条链能被一次跑通并留证据。
    // 空 = 用离线假种子（默认，门禁跑的就是这一条，见 home_probes.cpp）。
    std::string quotaFixturePath;
};

inline int parseLeadingInt(const std::string& text, int fallback) {
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            break;
        }
        value = value * 10 + (c - '0');
    }
    return value > 0 ? value : fallback;
}

/// 解析 --frames N / --size WxH / --manual / --screenshot <路径> /
/// --screenshot-seeded <路径> / --quota-fixture <路径>；未知参数忽略。
inline ProbeArgs parseProbeArgs(int argc, char* argv[]) {
    ProbeArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            args.frames = parseLeadingInt(argv[++i], args.frames);
        } else if (arg == "--size" && i + 1 < argc) {
            const std::string size = argv[++i];
            const std::size_t sep = size.find_first_of("xX");
            if (sep != std::string::npos) {
                args.windowW = parseLeadingInt(size.substr(0, sep), args.windowW);
                args.windowH = parseLeadingInt(size.substr(sep + 1), args.windowH);
            }
        } else if (arg == "--screenshot" && i + 1 < argc) {
            args.screenshotPath = argv[++i];
        } else if (arg == "--screenshot-seeded" && i + 1 < argc) {
            args.screenshotSeedPath = argv[++i];
        } else if (arg == "--quota-fixture" && i + 1 < argc) {
            args.quotaFixturePath = argv[++i];
        } else if (arg == "--manual") {
            args.manual = true;
        }
    }
    return args;
}

/// 单帧驱动：Runtime::update（输入派发 + 交互 + 布局/重绘标脏），
/// pointerScale = dpiScale = 1（逻辑 = 物理，探针窗口无缩放）。
inline bool pollOnce(core::window::Handle window,
                     core::dsl::Runtime& runtime,
                     float dt = 1.0f / 60.0f) {
    return runtime.update(window, dt, 1.0f, 1.0f);
}

/// 帧循环标准单元：先 pollOnce 再 compose（成对使用——update 不重建元素树，
/// 弹层等条件元素要 recompose 才进 DOM，requestFocus 等帧末请求也在 compose
/// 消费）。composeFn 形如 [&](Ui& ui, const Screen& screen) { ... }。
template <typename ComposeFn>
inline void composeModuleFrame(core::dsl::Runtime& runtime,
                               core::window::Handle window,
                               const std::string& pageId,
                               float logicalWidth,
                               float logicalHeight,
                               ComposeFn&& composeFn) {
    pollOnce(window, runtime);
    runtime.compose(pageId, logicalWidth, logicalHeight, std::forward<ComposeFn>(composeFn));
}

// ==================== 帧捕获（--screenshot）实现 ====================

/// GL 后备缓冲行序（左下原点）翻成 **top-down**（第 0 行 = 图顶，即 PNG 行序）。
/// readFrameRgba 在 GL 边界调它；也供断言直接喂合成像素单测翻轴
/// （见 screenshot_probe.cpp —— 不必起窗口就能证翻轴正确）。
/// 越界保护：像素数不足直接不动（宁可不动也不要静默越界读）。
inline void flipRowsToTopDown(std::vector<unsigned char>& rgba, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    if (rgba.size() < stride * static_cast<std::size_t>(height)) {
        return;
    }
    std::vector<unsigned char> scratch(stride);
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* top =
            rgba.data() + static_cast<std::size_t>(y) * stride;
        unsigned char* bottom = rgba.data() +
            static_cast<std::size_t>(height - 1 - y) * stride;
        std::memcpy(scratch.data(), top, stride);
        std::memcpy(top, bottom, stride);
        std::memcpy(bottom, scratch.data(), stride);
    }
}

/// 底层：top-down RGBA 像素（**第 0 行 = 图顶**）编码写 PNG。
/// 不碰 GL / 窗口 —— 可被断言直接喂合成像素验证，无需窗口上下文。
/// 编码走 libpng 简化 API png_image_write_to_file（EUI-NEO vendored libpng
/// 1.6.43 已带该接口），无需自己手写 deflate。
/// 失败（参数非法 / 打不开 / 写失败）→ stderr 明确诊断 + false，绝不静默假成功。
inline bool writeRgbaToPng(const std::string& path,
                           const unsigned char* rgba,
                           int width,
                           int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) {
        std::cerr << "probe::writeRgbaToPng: 非法参数 (rgba="
                  << (rgba == nullptr ? "null" : "ok") << ", " << width << "x"
                  << height << ")\n";
        return false;
    }
    png_image image{};  // libpng 要求先整体清零再填字段（version 必设）
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;  // 输入已 8bit RGBA，逐字节原样落盘
    // convert_to_8bit = 0（已是 8bit）；row_stride = 0 = 用最小连续行距；
    // colormap = NULL（真彩色）。libpng 简化 API 一次调用即建文件、写完关闭。
    if (!png_image_write_to_file(&image, path.c_str(), 0, rgba, 0, nullptr)) {
        std::cerr << "probe::writeRgbaToPng: 写 PNG 失败 '" << path << "': "
                  << image.message << "\n";
        return false;
    }
    return true;
}

/// 帧捕获会话：持有渲染后端 + 窗口 framebuffer 像素尺寸 / 内容缩放。
/// 构造 = glfwShow/Focus + createRenderBackend + initialize。
/// 一个 probe 进程建一个就够——渲染后端是重量级对象（GL 上下文 / 渲染缓存），
/// 反复建会重复初始化。
class FrameCapture {
public:
    explicit FrameCapture(core::window::Handle window) : window_(window) {
        if (window == nullptr) {
            error_ = "窗口句柄为 null";
            return;
        }
        // Handle 即 GLFWwindow*（core::window 的 GLFW 后端语义；nativeWindowInfo()
        // 在 macOS 给的是 NSWindow*，两者不可混用）
        auto* glfwWin = static_cast<GLFWwindow*>(window);
        // 裸 GLFW 探针窗可能没被置前，先 show+focus 再取尺寸（满屏应用下尤其重要）
        glfwShowWindow(glfwWin);
        glfwFocusWindow(glfwWin);
        glfwGetFramebufferSize(glfwWin, &fbWidth_, &fbHeight_);
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        glfwGetWindowContentScale(glfwWin, &scaleX, &scaleY);
        scale_ = scaleX > 0.0f ? scaleX : 1.0f;
        if (fbWidth_ <= 0 || fbHeight_ <= 0) {
            error_ = "framebuffer 尺寸非法（窗口还没映射？）";
            return;
        }
        backend_ = core::render::createRenderBackend(window);
        if (backend_ == nullptr) {
            error_ = "createRenderBackend 返回 null（渲染后端不可用）";
            return;
        }
        if (!backend_->initialize()) {
            error_ = "renderBackend->initialize() 失败";
            backend_.reset();
        }
    }

    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;
    ~FrameCapture() = default;  // 后端随句柄销毁（须早于 destroyWindow）

    /// 建会话成功 = ok()；error() 给出可直接打进报告的中文原因。
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    /// framebuffer 像素宽高（= PNG 出来的图尺寸，与逻辑窗口宽高不同：Retina 2x）
    int pixelWidth() const { return fbWidth_; }
    int pixelHeight() const { return fbHeight_; }
    float scale() const { return scale_; }

    /// 真渲染 runtime 当前一帧 → glReadPixels 读回后备缓冲 → 返回 **top-down**
    /// RGBA（已翻轴，第 0 行 = 图顶）。调用方若拿宽度请用本对象的
    /// pixelWidth/pixelHeight —— 逻辑 px × scale 才是真实像素尺寸。
    bool readFrame(core::dsl::Runtime& runtime,
                   std::vector<unsigned char>& rgba,
                   int& width,
                   int& height) {
        if (!ok()) {
            std::cerr << "probe::FrameCapture::readFrame: " << error_ << "\n";
            return false;
        }
        width = fbWidth_;
        height = fbHeight_;
        rgba.assign(static_cast<std::size_t>(width) *
                        static_cast<std::size_t>(height) * 4u,
                    0u);
        {
            // runtime.render 走 activeRenderBackend()——不激活就是空操作（黑图）
            core::render::ScopedRenderBackend scoped(*backend_);
            backend_->beginFrame(core::render::RenderSurface{
                window_, core::window::nativeWindowInfo(window_), width, height,
                scale_});
            runtime.render(width, height, scale_,
                           core::theme::current().colors.background);
            // ⚠ 必须落在 present() 之前：swap 之后后备缓冲内容未定义
            //   （about_probe.cpp:425 / claude_probes.cpp:844 同一条实测教训）
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba.data());
            glPixelStorei(GL_PACK_ALIGNMENT, 4);  // 还原默认，别影响后续渲染
            flipRowsToTopDown(rgba, width, height);
            backend_->present();
        }
        return true;
    }

    /// readFrame + writeRgbaToPng 一步到位。
    bool capturePng(core::dsl::Runtime& runtime, const std::string& path) {
        std::vector<unsigned char> rgba;
        int width = 0;
        int height = 0;
        if (!readFrame(runtime, rgba, width, height)) {
            return false;
        }
        if (!writeRgbaToPng(path, rgba.data(), width, height)) {
            return false;
        }
        std::cout << "probe: screenshot " << path << " (" << width << "x"
                  << height << ")\n";
        return true;
    }

private:
    core::window::Handle window_ = nullptr;
    std::unique_ptr<core::render::RenderBackend> backend_;
    std::string error_;
    int fbWidth_ = 0;
    int fbHeight_ = 0;
    float scale_ = 1.0f;
};

/// 一次性便捷（只拍一张的场景）：建会话 → **把过渡动画跑完** → 渲染一帧 →
/// 写 PNG → 销毁后端。
///
/// 为什么必须先跑动画：dialog 的面板透明度是 `.opacity(open_ ? 1 : 0)` 给出的
/// **过渡目标值**（EUI-NEO components/dialog.h:89），真正的不透明度由
/// `.animate(Opacity).transition()` 逐帧插值。刚 open 就 glReadPixels 会抓到
/// 半透明面板——底下整页内容透上来，截图看起来像「弹窗背景是透明的」这种
/// 产品缺陷，实际是采集时机早于动画结束。推进 24 帧（400ms @60fps）盖过
/// 仓库里最长的过渡时长，落图才是稳定态。
inline bool captureFramePng(core::window::Handle window,
                            core::dsl::Runtime& runtime,
                            const std::string& path) {
    for (int i = 0; i < 24; ++i) {
        pollOnce(window, runtime);
    }
    FrameCapture capture(window);
    return capture.capturePng(runtime, path);
}

/// --screenshot 的标准接线点。
/// **未传路径 = 立刻 return true：零开销**（不建渲染后端、不渲染、不写文件），
/// 既有 probe 行为与退出码逐字不变。传了路径就抓一张；失败时 stderr 明确诊断 +
/// 置 *passed = false（调用方据此返回非零退出码，不静默）。
inline bool captureIfRequested(const ProbeArgs& args,
                               core::window::Handle window,
                               core::dsl::Runtime& runtime,
                               bool* passed = nullptr) {
    if (args.screenshotPath.empty()) {
        return true;
    }
    const bool captured =
        captureFramePng(window, runtime, args.screenshotPath);
    if (!captured) {
        std::cerr << "probe: --screenshot 落图失败: " << args.screenshotPath
                  << "（退出码将非零：视觉证据链缺失不能当通过）\n";
        if (passed != nullptr) {
            *passed = false;
        }
    }
    return captured;
}

/// --screenshot-seeded 的标准接线点：**中途**状态点落图（语义见 ProbeArgs）。
/// 与 captureIfRequested 同款契约——未传路径 = 立刻 return true（零开销），
/// 传了但落图失败 = stderr 诊断 + 置 *passed = false（不静默）。
inline bool captureSeedIfRequested(const ProbeArgs& args,
                                   core::window::Handle window,
                                   core::dsl::Runtime& runtime,
                                   bool* passed = nullptr) {
    if (args.screenshotSeedPath.empty()) {
        return true;
    }
    const bool captured =
        captureFramePng(window, runtime, args.screenshotSeedPath);
    if (!captured) {
        std::cerr << "probe: --screenshot-seeded 落图失败: "
                  << args.screenshotSeedPath
                  << "（退出码将非零：视觉证据链缺失不能当通过）\n";
        if (passed != nullptr) {
            *passed = false;
        }
    }
    return captured;
}

namespace detail {

/// GLFW 键码 → core::InputKey。与 EUI-NEO core/window/window_input_backend.cpp
/// mapGlfwKey 保持一致（该函数是 TU 内 static 不导出，harness 自含一份）。
/// 未知键 → InputKey::Unknown（queueKeyInput 会按 Unknown 忽略按键态）。
inline core::InputKey glfwKeyToInputKey(int key) {
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::Digit0) + key - GLFW_KEY_0);
    }
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::A) + key - GLFW_KEY_A);
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F24) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::F1) + key - GLFW_KEY_F1);
    }
    if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) {
        return static_cast<core::InputKey>(
            static_cast<int>(core::InputKey::Numpad0) + key - GLFW_KEY_KP_0);
    }
    switch (key) {
        case GLFW_KEY_BACKSPACE: return core::InputKey::Backspace;
        case GLFW_KEY_TAB: return core::InputKey::Tab;
        case GLFW_KEY_ENTER: return core::InputKey::Enter;
        case GLFW_KEY_ESCAPE: return core::InputKey::Escape;
        case GLFW_KEY_SPACE: return core::InputKey::Space;
        case GLFW_KEY_INSERT: return core::InputKey::Insert;
        case GLFW_KEY_DELETE: return core::InputKey::Delete;
        case GLFW_KEY_HOME: return core::InputKey::Home;
        case GLFW_KEY_END: return core::InputKey::End;
        case GLFW_KEY_PAGE_UP: return core::InputKey::PageUp;
        case GLFW_KEY_PAGE_DOWN: return core::InputKey::PageDown;
        case GLFW_KEY_LEFT: return core::InputKey::Left;
        case GLFW_KEY_RIGHT: return core::InputKey::Right;
        case GLFW_KEY_UP: return core::InputKey::Up;
        case GLFW_KEY_DOWN: return core::InputKey::Down;
        case GLFW_KEY_PRINT_SCREEN: return core::InputKey::PrintScreen;
        case GLFW_KEY_SCROLL_LOCK: return core::InputKey::ScrollLock;
        case GLFW_KEY_PAUSE: return core::InputKey::Pause;
        case GLFW_KEY_CAPS_LOCK: return core::InputKey::CapsLock;
        case GLFW_KEY_NUM_LOCK: return core::InputKey::NumLock;
        case GLFW_KEY_LEFT_SHIFT: return core::InputKey::LeftShift;
        case GLFW_KEY_RIGHT_SHIFT: return core::InputKey::RightShift;
        case GLFW_KEY_LEFT_CONTROL: return core::InputKey::LeftControl;
        case GLFW_KEY_RIGHT_CONTROL: return core::InputKey::RightControl;
        case GLFW_KEY_LEFT_ALT: return core::InputKey::LeftAlt;
        case GLFW_KEY_RIGHT_ALT: return core::InputKey::RightAlt;
        case GLFW_KEY_LEFT_SUPER: return core::InputKey::LeftSuper;
        case GLFW_KEY_RIGHT_SUPER: return core::InputKey::RightSuper;
        case GLFW_KEY_MENU: return core::InputKey::Menu;
        case GLFW_KEY_APOSTROPHE: return core::InputKey::Apostrophe;
        case GLFW_KEY_COMMA: return core::InputKey::Comma;
        case GLFW_KEY_MINUS: return core::InputKey::Minus;
        case GLFW_KEY_PERIOD: return core::InputKey::Period;
        case GLFW_KEY_SLASH: return core::InputKey::Slash;
        case GLFW_KEY_SEMICOLON: return core::InputKey::Semicolon;
        case GLFW_KEY_EQUAL: return core::InputKey::Equal;
        case GLFW_KEY_LEFT_BRACKET: return core::InputKey::LeftBracket;
        case GLFW_KEY_BACKSLASH: return core::InputKey::Backslash;
        case GLFW_KEY_RIGHT_BRACKET: return core::InputKey::RightBracket;
        case GLFW_KEY_GRAVE_ACCENT: return core::InputKey::GraveAccent;
        case GLFW_KEY_KP_DECIMAL: return core::InputKey::NumpadDecimal;
        case GLFW_KEY_KP_DIVIDE: return core::InputKey::NumpadDivide;
        case GLFW_KEY_KP_MULTIPLY: return core::InputKey::NumpadMultiply;
        case GLFW_KEY_KP_SUBTRACT: return core::InputKey::NumpadSubtract;
        case GLFW_KEY_KP_ADD: return core::InputKey::NumpadAdd;
        case GLFW_KEY_KP_ENTER: return core::InputKey::NumpadEnter;
        case GLFW_KEY_KP_EQUAL: return core::InputKey::NumpadEqual;
        default: return core::InputKey::Unknown;
    }
}

/// modifiers 位约定与 GLFW_MOD_* 一致：SHIFT=1 / CONTROL=2 / ALT=4 / SUPER=8 /
/// CAPS_LOCK=16 / NUM_LOCK=32——可直接传 GLFW_MOD_* 常量或其按位组合。
inline core::KeyModifiers decodeKeyModifiers(int modifiers) {
    core::KeyModifiers out;
    out.shift = (modifiers & GLFW_MOD_SHIFT) != 0;
    out.control = (modifiers & GLFW_MOD_CONTROL) != 0;
    out.alt = (modifiers & GLFW_MOD_ALT) != 0;
    out.super = (modifiers & GLFW_MOD_SUPER) != 0;
    out.capsLock = (modifiers & GLFW_MOD_CAPS_LOCK) != 0;
    out.numLock = (modifiers & GLFW_MOD_NUM_LOCK) != 0;
    return out;
}

} // namespace detail

/// 完整点击：队列 press + release 后驱动一帧（按下/抬起同帧消费）。
/// button 默认左键；传 core::PointerButton::Right 即右键点击。
inline void click(core::window::Handle window,
                  core::dsl::Runtime& runtime,
                  double x,
                  double y,
                  core::PointerButton button = core::PointerButton::Left) {
    core::queuePointerButton(window, x, y, button,
                             core::PointerAction::Press, {});
    core::queuePointerButton(window, x, y, button,
                             core::PointerAction::Release, {});
    pollOnce(window, runtime);
}

/// 键盘注入：包装 core::queueKeyInput。keyCode 取 GLFW 键码（如 GLFW_KEY_ESCAPE /
/// GLFW_KEY_ENTER），内部映射为 core::InputKey。仅排队不驱动帧——调用方自行
/// pollOnce / composeModuleFrame 使事件送达（按需 Press 与 Release 隔帧成对）。
inline void queueKey(core::window::Handle window,
                     [[maybe_unused]] core::dsl::Runtime& runtime,
                     int keyCode,
                     core::KeyAction action,
                     int modifiers = 0) {
    core::KeyEvent event;
    event.key = detail::glfwKeyToInputKey(keyCode);
    event.action = action;
    event.modifiers = detail::decodeKeyModifiers(modifiers);
    core::queueKeyInput(window, event);
}

/// 滚轮注入：包装 core::queueScrollInput（dx/dy 为滚轮增量，如类 10 双栏同步的
/// -5.0/-5.0）。仅排队不驱动帧（同上）。
inline void queueScroll(core::window::Handle window,
                        [[maybe_unused]] core::dsl::Runtime& runtime,
                        double dx,
                        double dy) {
    core::queueScrollInput(window, dx, dy);
}

/// 文本注入：包装 core::queueTextInput（UTF-8 文本，如类 6(b) 长 profile 名输入）。
/// 仅排队不驱动帧（同上）。
inline void queueText(core::window::Handle window,
                      [[maybe_unused]] core::dsl::Runtime& runtime,
                      const std::string& text) {
    core::queueTextInput(window, text);
}

/// 完整左键拖拽序列：press(x0,y0) → steps 段均匀插值 move（每段 pollOnce 一帧，
/// 命中/悬停沿路径逐段推进）→ release(x1,y1)。类 11 占位条注入用。
inline void drag(core::window::Handle window,
                 core::dsl::Runtime& runtime,
                 double x0,
                 double y0,
                 double x1,
                 double y1,
                 int steps = 8) {
    const core::KeyModifiers noMods;
    const core::PointerButtons held(core::PointerButton::Left);
    core::queuePointerButton(window, x0, y0, core::PointerButton::Left,
                             core::PointerAction::Press, noMods);
    pollOnce(window, runtime);
    const int count = steps > 0 ? steps : 1;
    for (int step = 1; step <= count; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(count);
        core::queuePointerMotion(window,
                                 x0 + (x1 - x0) * t,
                                 y0 + (y1 - y0) * t,
                                 held,
                                 noMods);
        pollOnce(window, runtime);
    }
    core::queuePointerButton(window, x1, y1, core::PointerButton::Left,
                             core::PointerAction::Release, noMods);
    pollOnce(window, runtime);
}

namespace detail {

/// 深度遍历：记录 parentOf（id → 父 id，根为 ""）、id → 元素索引、叶子清单。
inline void walkElements(const core::dsl::Element& node,
                         const std::string& parentId,
                         std::unordered_map<std::string, std::string>& parentOf,
                         std::unordered_map<std::string, const core::dsl::Element*>& byId,
                         std::vector<const core::dsl::Element*>& leaves) {
    parentOf[node.id] = parentId;
    byId[node.id] = &node;
    if (node.children.empty()) {
        leaves.push_back(&node);
        return;
    }
    for (const auto& child : node.children) {
        walkElements(*child, node.id, parentOf, byId, leaves);
    }
}

inline bool framesIntersect(const core::dsl::Element& a, const core::dsl::Element& b) {
    const auto& r1 = a.frame;
    const auto& r2 = b.frame;
    return r1.x < r2.x + r2.width && r2.x < r1.x + r1.width &&
           r1.y < r2.y + r2.height && r2.y < r1.y + r1.height;
}

} // namespace detail

/// L2 弹层 z 碰撞扫描：popupId 弹层（含根链）之上不允许有帧相交的更高 z 叶子。
/// - 弹层不存在 → true（无验证对象）
/// - 弹层链上界 = max(p.subtreeMaxZIndex, p 到根链上每元素 zIndex)
/// - 遍历叶子 e：跳过 p 祖先/子孙（parentOf 判）与 opacity==0；帧相交且
///   e.subtreeMaxZIndex >= 上界 → cerr 双方 id+z 并返回 false
inline bool assertNoEqualZOverlap(core::dsl::Ui& ui, const std::string& popupId) {
    core::dsl::Element* popup = ui.find(popupId);
    if (popup == nullptr) {
        return true;
    }
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, const core::dsl::Element*> byId;
    std::vector<const core::dsl::Element*> leaves;
    for (const auto& root : ui.roots()) {
        detail::walkElements(*root, "", parentOf, byId, leaves);
    }
    int upperBound = popup->subtreeMaxZIndex;
    for (std::string id = popup->id; !id.empty(); id = parentOf[id]) {
        const auto it = byId.find(id);
        if (it != byId.end() && it->second->zIndex > upperBound) {
            upperBound = it->second->zIndex;
        }
    }
    for (const core::dsl::Element* leaf : leaves) {
        if (leaf->opacity == 0.0f) {
            continue;
        }
        bool onChain = false;  // p 祖先/自身（含根链）
        for (std::string id = popup->id; !id.empty(); id = parentOf[id]) {
            if (id == leaf->id) {
                onChain = true;
                break;
            }
        }
        if (!onChain) {  // p 子孙/自身
            for (std::string id = leaf->id; !id.empty(); id = parentOf[id]) {
                if (id == popup->id) {
                    onChain = true;
                    break;
                }
            }
        }
        if (onChain || !detail::framesIntersect(*leaf, *popup)) {
            continue;
        }
        if (leaf->subtreeMaxZIndex >= upperBound) {
            std::cerr << "probe::assertNoEqualZOverlap: leaf '" << leaf->id
                      << "' z=" << leaf->zIndex << " (subtree " << leaf->subtreeMaxZIndex
                      << ") overlaps popup '" << popup->id << "' z=" << popup->zIndex
                      << " chainUpperBound=" << upperBound << '\n';
            return false;
        }
    }
    return true;
}

/// 绘制序断言：topId 必须整子树排在 bottomId 之后绘制（= 盖在其上）。
/// 判据 = 两者的最低公共祖先（LCA）处，top 侧子级 subtreeMaxZIndex 严格
/// 大于 bottom 侧子级 subtreeMaxZIndex —— 兄弟按子树上界稳定排序
///（core/dsl.h rebuildOrderedChildren），top 侧 > bottom 侧 ⇒ top 后绘；
/// 等值 ⇒ DOM 序保持，bottom 若在 DOM 序靠后就盖住 top（本断言红）。
/// 与 assertNoEqualZOverlap 互补：后者扫描叶子级 z 碰撞（叶子 subtreeMax
/// 常为 0，抓不住「容器级等值 z」），本断言直接判 LCA 处的两个比较对象，
/// 是「弹层不被下方兄弟盖住」的主判据（settings 桌宠卡双下拉类）。
/// 父链按**指针**建（挂 id 链会在无名包装栈处断掉——对话框内部容器等
/// 不设 id，id 链到那里即终结，LCA 假阴性 "no common ancestor"）。
inline bool assertDrawnAbove(core::dsl::Ui& ui,
                             const std::string& topId,
                             const std::string& bottomId) {
    const core::dsl::Element* top = ui.find(topId);
    const core::dsl::Element* bottom = ui.find(bottomId);
    if (top == nullptr || bottom == nullptr) {
        std::cerr << "probe::assertDrawnAbove: element missing: '"
                  << (top == nullptr ? topId : bottomId) << "'\n";
        return false;
    }
    std::unordered_map<const core::dsl::Element*, const core::dsl::Element*> parentOf;
    for (const auto& root : ui.roots()) {
        std::vector<const core::dsl::Element*> stack{root.get()};
        while (!stack.empty()) {
            const core::dsl::Element* node = stack.back();
            stack.pop_back();
            for (const auto& child : node->children) {
                parentOf[child.get()] = node;
                stack.push_back(child.get());
            }
        }
    }
    auto chainOf = [&](const core::dsl::Element* leaf) {
        std::vector<const core::dsl::Element*> chain;
        for (const core::dsl::Element* node = leaf; node != nullptr;) {
            chain.push_back(node);
            const auto it = parentOf.find(node);
            node = it == parentOf.end() ? nullptr : it->second;
        }
        return chain;
    };
    const std::vector<const core::dsl::Element*> topChain = chainOf(top);
    const std::vector<const core::dsl::Element*> bottomChain = chainOf(bottom);
    // LCA = bottom 链上第一个同时出现在 top 链中的节点（同根必存在）
    std::size_t lcaTopIdx = 0;
    std::size_t lcaBottomIdx = 0;
    bool found = false;
    for (std::size_t k = 0; k < bottomChain.size() && !found; ++k) {
        const auto it = std::find(topChain.begin(), topChain.end(), bottomChain[k]);
        if (it != topChain.end()) {
            lcaBottomIdx = k;
            lcaTopIdx = static_cast<std::size_t>(it - topChain.begin());
            found = true;
        }
    }
    if (!found) {
        std::cerr << "probe::assertDrawnAbove: no common ancestor between '"
                  << topId << "' and '" << bottomId << "'\n";
        return false;
    }
    if (lcaTopIdx == 0 || lcaBottomIdx == 0) {
        std::cerr << "probe::assertDrawnAbove: '" << topId << "' / '"
                  << bottomId << "' 是祖先关系（绘制序由兄弟排序决定的对象"
                     "应是 LCA 下两侧的直接子级），调用方选错比较对象\n";
        return false;
    }
    // LCA 的直接子级 = 各自链上 LCA 的前一个节点
    const core::dsl::Element* topChild = topChain[lcaTopIdx - 1];
    const core::dsl::Element* bottomChild = bottomChain[lcaBottomIdx - 1];
    if (topChild->subtreeMaxZIndex > bottomChild->subtreeMaxZIndex) {
        return true;
    }
    auto label = [](const core::dsl::Element* e) {
        return e->id.empty() ? std::string("<unnamed>") : e->id;
    };
    std::cerr << "probe::assertDrawnAbove: '" << topId << "'（LCA 侧子级 '"
              << label(topChild) << "' subtreeMaxZ="
              << topChild->subtreeMaxZIndex << "）未压过 '" << bottomId
              << "'（LCA 侧子级 '" << label(bottomChild) << "' subtreeMaxZ="
              << bottomChild->subtreeMaxZIndex << "）——等值/更低 z 时稳定"
                 "排序保持 DOM 序，后者后绘盖住前者\n";
    return false;
}

/// RAII：临时把元素 onScrollOffsetChanged 换成计数器包装（仍转发原回调），
/// 析构时恢复原回调；元素不存在时为空操作。注意元素在下次 compose 重建
/// 树时即失效，guard 必须在对应 composeModuleFrame 之前销毁。
class SwapScrollCallback {
public:
    SwapScrollCallback(core::dsl::Ui& ui,
                       const std::string& elementId,
                       int* counter)
        : element_(ui.find(elementId)), counter_(counter) {
        if (element_ == nullptr) {
            return;
        }
        original_ = std::move(element_->onScrollOffsetChanged);
        element_->onScrollOffsetChanged = [this](float offset) {
            if (counter_ != nullptr) {
                ++(*counter_);
            }
            if (original_) {
                original_(offset);
            }
        };
    }

    ~SwapScrollCallback() {
        if (element_ != nullptr) {
            element_->onScrollOffsetChanged = std::move(original_);
        }
    }

    SwapScrollCallback(const SwapScrollCallback&) = delete;
    SwapScrollCallback& operator=(const SwapScrollCallback&) = delete;

private:
    core::dsl::Element* element_ = nullptr;
    std::function<void(float)> original_;
    int* counter_ = nullptr;
};

} // namespace probe