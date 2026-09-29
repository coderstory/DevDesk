// tests/probes/about_probe.cpp — about 页可见 UI probe（2026-09-28 真机反馈三项）
//
// 覆盖三件用户真机反馈改动：
//   1) hero 三行（版本 / 构建 / commit）删除 → hero = 图标 + 标题；
//   2) 两张卡从左右并排改上下堆叠（各占满内容区宽 900、卡距 8，本页无滚动）；
//   3) 浮环动态背景可见度上调（元素 opacity 0.18-0.28）且「真的在动」。
//
// 断言口径（真窗口后端 GLFW+OpenGL + 真 Runtime 循环，非无头）：
//   - 几何：内容块贴底（底边 = 页脚顶 − 一档间距，2026-09-29）、三行 id 不存在、
//     卡宽/卡位/footer 底边数值断言（与 test_ui_modern.cpp:uiAboutLayout 同口径，
//     双保险）；卡片几何期望值随窗口高现算 → `--size 1200x640` 顺带覆盖极矮窗
//     clamp 回 topPad 的分支；
//   - 浮环：7 个环元素存在、元素 opacity ∈ [0.18, 0.28]（字面量，防调淡回归）；
//     真帧推进（runtime.updateFrameCallback → compose 挂的 onFrame，每次
//     callback 置 composeRequested_/paintRequested_，EUI-NEO
//     core/runtime/runtime_update.h:407-413）后环元素几何变化 = 端到端「在动」；
//     静止 2s+ 后 bg 不再挂 onFrame（休眠省电设计），指针移动注入可再唤醒。
//
// --manual：窗口停在屏上（位置固定 100,100 点，便于 screencapture 按 2x 物理
// 像素裁剪）并周期注入指针移动保活漂移；此模式自己 render+present（探针常规
// 帧循环不画），供人工/截图复核观感；有界 90s 自动退出（外部按 PID 结束，
// 勿按名杀进程）。加 --dump <prefix> 则把 GL 后备缓冲读回写 <prefix>_a/_b.ppm
//（P6）——不依赖系统截屏（OpenGL 窗口 screencapture 有旧帧坑、且裸 GLFW 探针
// 窗在满屏应用下可能不被置前）。
#include <GLFW/glfw3.h>  // 必须先于 harness：让 GLFW 默认拉平台 GL 头（--dump 用 glReadPixels）
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "app_registry.h"             // create_about_module 声明
#include "core/module_interface.h"
#include "core/render/render_backend.h"  // manual 渲染：activeRenderBackend/present
#include "core/theme.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层链接桩（同 settings_dropdown_z_probe.cpp：探针不建应用级 UI）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_about_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

bool near(float a, float b) { return std::fabs(a - b) <= 0.5f; }

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    // 固定窗口位置：截图裁剪用（GLFW 屏幕坐标 = 点；Retina 2x → 物理 ×2）
    core::window::setWindowPos(window, 100, 100);

    std::unique_ptr<::core::Module> mod(create_about_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "about";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "about_probe: FAIL " << what << "\n";
            passed = false;
        }
    };
    auto frameStr = [](const core::dsl::Element* e) {
        return "(" + std::to_string(e->frame.x) + ", " +
               std::to_string(e->frame.y) + ", " +
               std::to_string(e->frame.width) + "x" +
               std::to_string(e->frame.height) + ")";
    };

    const auto& t = core::theme::current();
    constexpr float kContentW = 900.0f;   // 2026-09-29 1040→900（两卡缩一档）
    constexpr float kCardGap = 8.0f;
    constexpr float kCardH = 24.0f + 28.0f + 6.0f * 28.0f + 24.0f;  // 244
    constexpr float kFooterH = 20.0f;
    const float topPad = t.sizes.spacingXXLarge;                           // 40（仅极矮窗兜底）
    const float iconSize = t.sizes.spacingXXLarge + t.sizes.spacingLarge;  // 56
    const float heroToCards = t.sizes.spacingXLarge;                       // 24
    // 贴底对齐（2026-09-29）：内容块总高固定 624，起点 = 页脚顶 − 一档间距 − 624
    //（护栏 topPad：极矮窗退回顶部对齐）
    const float contentH = iconSize + 8.0f + 40.0f + heroToCards +
                           kCardH + kCardGap + kCardH;                    // 624
    const float footerTop = h - t.sizes.spacingXXLarge - kFooterH;
    const float contentTop = std::max(topPad, footerTop - heroToCards - contentH);
    const float cardsTop = contentTop + iconSize + 8.0f + 40.0f + heroToCards;
    const float x = std::max(0.0f, (w - kContentW) / 2.0f);

    composeFrame();

    // ---- 1) hero：图标 + 标题，三行 id 不存在 ----
    const auto* hero = ui != nullptr ? ui->find("about.hero.icon") : nullptr;
    if (hero == nullptr) {
        std::cerr << "about_probe: about.hero.icon 不存在\n";
        passed = false;
    } else {
        check(near(hero->frame.y, contentTop), "hero.frame.y " + frameStr(hero));
        check(near(hero->frame.width, iconSize), "hero 图标宽 " + frameStr(hero));
    }
    check(ui != nullptr && ui->find("about.hero.sub") == nullptr,
          "about.hero.sub 应已删除");
    check(ui != nullptr && ui->find("about.hero.build") == nullptr,
          "about.hero.build 应已删除");
    check(ui != nullptr && ui->find("about.hero.commit") == nullptr,
          "about.hero.commit 应已删除");

    // ---- 2) 卡片上下堆叠 + 内容块贴底 + footer 贴底 ----
    // 极矮窗（页脚顶 − 一档间距 − 内容块高 < topPad）走 clamp 分支：起点退回
    // topPad，此时「内容块底 = 页脚顶 − 间距」与「footer 贴页底」两条都不成立
    //（footer 被自身 max 护栏压到卡下），断言按分支取
    const bool clamped = (footerTop - heroToCards - contentH) < topPad;
    const auto* c1 = ui != nullptr ? ui->find("about.card.app") : nullptr;
    const auto* c2 = ui != nullptr ? ui->find("about.card.deps") : nullptr;
    const auto* footer = ui != nullptr ? ui->find("about.footer") : nullptr;
    if (c1 == nullptr || c2 == nullptr || footer == nullptr) {
        std::cerr << "about_probe: 卡片/页脚元素缺失\n";
        passed = false;
    } else {
        check(near(c1->frame.width, kContentW), "card.app 宽 " + frameStr(c1));
        check(near(c2->frame.width, kContentW), "card.deps 宽 " + frameStr(c2));
        check(near(c1->frame.height, kCardH), "card.app 高 " + frameStr(c1));
        check(near(c1->frame.x, x), "card.app.x " + frameStr(c1));
        check(near(c1->frame.y, cardsTop), "card.app.y " + frameStr(c1));
        check(near(c2->frame.y, c1->frame.y + c1->frame.height + kCardGap),
              "card.deps.y " + frameStr(c2));
        if (clamped) {
            // 护栏分支：起点 = topPad（无 max() 时会是负值），卡与页脚不重叠
            check(near(hero == nullptr ? -1.0f : hero->frame.y, topPad),
                  "clamp 分支 hero.frame.y " +
                      (hero != nullptr ? frameStr(hero) : std::string("null")));
            check(c2->frame.y + c2->frame.height + kCardGap <= footer->frame.y,
                  "clamp 分支卡/页脚重叠 " + frameStr(footer));
        } else {
            // 贴底对齐核心：内容块底边 = 页脚顶 − 一档间距（回退成顶部对齐即红）
            check(near(c2->frame.y + c2->frame.height, footerTop - heroToCards),
                  "内容块底边 " + std::to_string(c2->frame.y + c2->frame.height) +
                      " 期望 " + std::to_string(footerTop - heroToCards));
            check(near(footer->frame.y + footer->frame.height,
                       h - t.sizes.spacingXXLarge),
                  "footer 底边 " + frameStr(footer));
            check(footer->frame.y > c2->frame.y + c2->frame.height,
                  "footer 应在卡下 " + frameStr(footer));
        }
    }

    // ---- 3) 浮环：存在 / 可见度区间（字面量，防调淡回归）/ 在动 / 休眠 ----
    for (int i = 0; i < 7; ++i) {
        const auto* ring = ui != nullptr
                               ? ui->find("about.ring." + std::to_string(i))
                               : nullptr;
        if (ring == nullptr) {
            std::cerr << "about_probe: about.ring." << i << " 不存在\n";
            passed = false;
            continue;
        }
        check(ring->opacity >= 0.18f - 1e-6f && ring->opacity <= 0.28f + 1e-6f,
              "ring." + std::to_string(i) + " opacity " +
                  std::to_string(ring->opacity));
        check(ring->frame.x >= 0.0f && ring->frame.y >= 0.0f &&
                  ring->frame.x + ring->frame.width <= w + 0.5f &&
                  ring->frame.y + ring->frame.height <= h + 0.5f,
              "ring." + std::to_string(i) + " 越界 " + frameStr(ring));
    }
    const auto* bg = ui != nullptr ? ui->find("about.bg") : nullptr;
    const auto* ring0 = ui != nullptr ? ui->find("about.ring.0") : nullptr;
    if (bg == nullptr || ring0 == nullptr || bg->onFrame == nullptr) {
        std::cerr << "about_probe: bg/ring0 缺失或未挂 onFrame（唤醒期应有）\n";
        passed = false;
    } else {
        const float x0 = ring0->frame.x;
        const float y0 = ring0->frame.y;
        for (int f = 0; f < 20; ++f) composeFrame();  // 真帧推进（唤醒窗内）
        const auto* ring0b = ui->find("about.ring.0");
        if (ring0b == nullptr) {
            std::cerr << "about_probe: 重 compose 后 ring.0 丢失\n";
            passed = false;
        } else {
            const float moved = std::fabs(ring0b->frame.x - x0) +
                                std::fabs(ring0b->frame.y - y0);
            check(moved > 1.0f,
                  "ring.0 位移 " + std::to_string(moved) + "（应 > 1px，真在动）");
        }
        // 静止推进 2s+ → 全 settled + 超时 → 休眠（bg 不再挂 onFrame）
        for (int f = 0; f < 140; ++f) composeFrame();
        const auto* bgIdle = ui->find("about.bg");
        check(bgIdle != nullptr && bgIdle->onFrame == nullptr,
              "静止 2s+ 后 bg 应休眠（不挂 onFrame）");
        // 指针移动注入（真命中分发）→ 唤醒，重新挂 onFrame
        core::queuePointerMotion(window, w * 0.5, h * 0.4, core::PointerButtons{},
                                 core::KeyModifiers{});
        composeFrame();
        const auto* bgWake = ui->find("about.bg");
        check(bgWake != nullptr && bgWake->onFrame != nullptr,
              "指针移动后 bg 应重新挂 onFrame（唤醒路径）");
    }

    // --dump <prefix>：manual 模式下把渲染结果从 GL 后备缓冲读回写成 PPM
    //（P6，可被 PIL 读；观感复核不依赖系统截屏——OpenGL 窗口 screencapture
    // 有旧帧坑，且探针窗在满屏应用下可能不被 ordered front）
    std::string dumpPrefix;
    bool forceLightTheme = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--dump") {
            dumpPrefix = argv[i + 1];
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--light") {
            forceLightTheme = true;  // 用户实际主题是浅色档：截图按浅色出
        }
    }
    if (forceLightTheme) {
        core::theme::setLight();
    }

    if (args.manual) {
        // 截图模式：镜像 app 主循环（poll → update/compose → render → present）。
        // 注意探针常规帧循环只 update/compose 不画（probe_harness 的
        // composeModuleFrame 不 render），截图前必须真渲染，否则抓到空窗。
        // Handle 即 GLFWwindow*（core::window GLFW 后端语义；
        // nativeWindowInfo().platformWindow 在 macOS 是 NSWindow*，勿混用——
        // 直接转 GLFWwindow* 调 glfwGetFramebufferSize 会崩）
        auto* glfwWin = static_cast<GLFWwindow*>(window);
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(glfwWin, &fbW, &fbH);
        float scaleX = 1.0f, scaleY = 1.0f;
        glfwGetWindowContentScale(glfwWin, &scaleX, &scaleY);
        const float renderScale = scaleX > 0.0f ? scaleX : 1.0f;
        // 裸 GLFW 探针从终端启动时窗口可能不置前（满屏应用盖住）——显式 show+focus
        glfwShowWindow(glfwWin);
        glfwFocusWindow(glfwWin);
        int winX = 0, winY = 0;
        core::window::getWindowPos(window, winX, winY);
        std::cout << "about_probe: manual 窗口@" << winX << "," << winY
                  << " 点 / framebuffer " << fbW << "x" << fbH << " / scale "
                  << renderScale << "（周期注入指针移动保活漂移，有界 90s）\n";
        const eui::Color clearColor = core::theme::current().colors.background;
        // 渲染后端：runtime.render 走 activeRenderBackend()——不建后端/不激活时
        // 渲染是空操作（窗口全黑）。镜像 app：createRenderBackend + 每帧
        // ScopedRenderBackend 激活 + beginFrame(表面) → render → present
        auto renderBackend = core::render::createRenderBackend(window);
        if (renderBackend != nullptr && !renderBackend->initialize()) {
            std::cerr << "about_probe: renderBackend->initialize 失败\n";
            renderBackend.reset();
        }
        if (renderBackend == nullptr) {
            std::cerr << "about_probe: 无渲染后端（渲染/截图不可用）\n";
        }
        const auto dumpFrame = [fbW, fbH](const std::string& path) {
            std::vector<unsigned char> px(
                static_cast<std::size_t>(fbW) * static_cast<std::size_t>(fbH) * 3u);
            // macOS 把 OpenGL API 标 deprecated（-Werror 下直接变错）：只包住
            // 这两条调用，逐语句窄抑制（先例：src/platform/keychain_macos.cpp）。
            // 不用文件级 GL_SILENCE_DEPRECATION / 不加 target 级屏蔽——那会连带
            // 屏蔽真危险的弃用告警；__clang__ 守卫免 GCC 报 unknown pragma
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbW, fbH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            std::ofstream ofs(path, std::ios::binary);
            ofs << "P6\n" << fbW << " " << fbH << "\n255\n";
            for (int row = fbH - 1; row >= 0; --row) {  // GL 原点左下 → 翻转成图上原点
                ofs.write(reinterpret_cast<const char*>(
                              px.data() + static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(fbW) * 3u),
                          static_cast<std::streamsize>(fbW) * 3);
            }
            std::cout << "about_probe: dump " << path << "\n";
        };
        for (int i = 0; i < 90 * 60; ++i) {
            if (i % 120 == 0) {
                core::queuePointerMotion(
                    window, w * 0.4 + static_cast<double>(i % 11) * 40.0,
                    h * 0.35 + static_cast<double>(i % 7) * 50.0,
                    core::PointerButtons{}, core::KeyModifiers{});
            }
            glfwPollEvents();
            composeFrame();
            if (renderBackend) {
                core::render::ScopedRenderBackend scoped(*renderBackend);
                renderBackend->beginFrame(core::render::RenderSurface{
                    window, core::window::nativeWindowInfo(window), fbW, fbH,
                    renderScale});
                runtime.render(fbW, fbH, renderScale, clearColor);
                if (!dumpPrefix.empty() && (i == 60 || i == 240)) {
                    dumpFrame(dumpPrefix + (i == 60 ? "_a.ppm" : "_b.ppm"));
                }
                renderBackend->present();
            }
            if (!dumpPrefix.empty() && i >= 240) {
                break;  // 抖动对已取到，提前收工（有界）
            }
        }
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "about_probe: PASS（" << args.windowW << "x" << args.windowH
                  << "）\n";
    }
    return passed ? 0 : 1;
}
