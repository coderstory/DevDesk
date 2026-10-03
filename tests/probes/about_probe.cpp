// tests/probes/about_probe.cpp — about 页可见 UI probe（2026-09-28/29 真机反馈）
//
// 覆盖三件用户真机反馈改动：
//   1) hero 三行（版本 / 构建 / commit）删除 → hero = 图标 + 标题；
//   2) 两张卡从左右并排改上下堆叠（各占满内容区宽 900、卡距 8，本页无滚动）；
//   3) 动态光晕（2026-09-29「又简陋又不明显 几乎看不到」→ 方案 A：光斑同心
//      圆叠层 + 利萨如漂移 + 呼吸 + 聚光扫过带 + 霓虹 shadow；同日二次反馈
//      「光团缩小一点，然后多生成几个」→ 6 光斑 / 直径 240-400 / 每球 3 层）。
//
// 断言口径（真窗口后端 GLFW+OpenGL + 真 Runtime 循环，非无头）：
//   - 几何：内容块贴底（底边 = 页脚顶 − 一档间距，2026-09-29）、三行 id 不存在、
//     卡宽/卡位/footer 底边数值断言（与 test_ui_modern.cpp:uiAboutLayout 同口径，
//     双保险）；卡片几何期望值随窗口高现算 → `--size 1200x640` 顺带覆盖极矮窗
//     clamp 回 topPad 的分支；
//   - 光晕：球数/层数/直径区间（字面量 6 / 3 / 240-400）、元素闸门
//     （6×3 层 + 2 扫带半 = 20 ≤ 20）、层 alpha 剖面（内 0.15 / 外 0.05，
//     ±35% 呼吸区间）、最外圈霓虹 shadow(blur 48 / primary@0.35)、
//     扫带峰值 alpha 0.05 + 倾角 18°（字面量，防调淡/退化回归）；
//     真帧推进（runtime.updateFrameCallback → compose 挂的 onTimer，每 tick 置
//     composeRequested_/paintRequested_，EUI-NEO core/runtime/runtime_update.h:380-403）
//     后光斑元素几何变化 = 端到端「在动」；空闲 120s 后 bg 不再挂 onTimer
//     （休眠省电设计），指针移动注入可再唤醒。
//
// --manual：窗口停在屏上（位置固定 100,100 点，便于 screencapture 按 2x 物理
// 像素裁剪）并周期注入指针移动保活漂移；此模式自己 render+present（探针常规
// 帧循环不画），供人工/截图复核观感；有界 90s 自动退出（外部按 PID 结束，
// 勿按名杀进程）。加 --dump <prefix> 则把 GL 后备缓冲读回写 <prefix>_a/_b.ppm
//（P6）——不依赖系统截屏（OpenGL 窗口 screencapture 有旧帧坑、且裸 GLFW 探针
// 窗在满屏应用下可能不被置前）；两帧按**动画时间**间隔 1.2s 取样（真机口径
// 「同一位置间隔 1.2s 抓两帧」），不靠迭代计数猜。
#include "probes/probe_harness.h"  // GL/GLFW 头由 harness 给（glad 路线；见 settings_cards_probes.cpp:43 同款说明）
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "app_registry.h"             // create_about_module 声明
#include "core/module_interface.h"
#include "core/render/render_backend.h"  // manual 渲染：activeRenderBackend/present
#include "core/theme.h"
#include "module_about/about_anim.h"  // 光晕参数（probe 与单测同源断言）

#include <algorithm>
#include <cmath>
#include <cstddef>
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

bool nearEq(float a, float b) { return std::fabs(a - b) <= 0.5f; }

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
        check(nearEq(hero->frame.y, contentTop), "hero.frame.y " + frameStr(hero));
        check(nearEq(hero->frame.width, iconSize), "hero 图标宽 " + frameStr(hero));
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
        check(nearEq(c1->frame.width, kContentW), "card.app 宽 " + frameStr(c1));
        check(nearEq(c2->frame.width, kContentW), "card.deps 宽 " + frameStr(c2));
        check(nearEq(c1->frame.height, kCardH), "card.app 高 " + frameStr(c1));
        check(nearEq(c1->frame.x, x), "card.app.x " + frameStr(c1));
        check(nearEq(c1->frame.y, cardsTop), "card.app.y " + frameStr(c1));
        check(nearEq(c2->frame.y, c1->frame.y + c1->frame.height + kCardGap),
              "card.deps.y " + frameStr(c2));
        if (clamped) {
            // 护栏分支：起点 = topPad（无 max() 时会是负值），卡与页脚不重叠
            check(nearEq(hero == nullptr ? -1.0f : hero->frame.y, topPad),
                  "clamp 分支 hero.frame.y " +
                      (hero != nullptr ? frameStr(hero) : std::string("null")));
            check(c2->frame.y + c2->frame.height + kCardGap <= footer->frame.y,
                  "clamp 分支卡/页脚重叠 " + frameStr(footer));
        } else {
            // 贴底对齐核心：内容块底边 = 页脚顶 − 一档间距（回退成顶部对齐即红）
            check(nearEq(c2->frame.y + c2->frame.height, footerTop - heroToCards),
                  "内容块底边 " + std::to_string(c2->frame.y + c2->frame.height) +
                      " 期望 " + std::to_string(footerTop - heroToCards));
            check(nearEq(footer->frame.y + footer->frame.height,
                       h - t.sizes.spacingXXLarge),
                  "footer 底边 " + frameStr(footer));
            check(footer->frame.y > c2->frame.y + c2->frame.height,
                  "footer 应在卡下 " + frameStr(footer));
        }
    }

    // ---- 3) 动态光晕：元素闸门 / alpha 剖面 / 霓虹 shadow / 扫带 / 在动 / 休眠 ----
    // 字面量口径（与 test_ui_modern.cpp:uiAboutGlow 同源但独立复述一遍：
    // 探针跑真窗口后端 + 真 Runtime 节拍，单测跑无头 compose，两边都要红）
    if (ui == nullptr) {
        std::cerr << "about_probe: ui 为空，光晕断言无法执行\n";
        passed = false;
    } else {
        using Anim = module::about::AboutAnimState;
        constexpr float kInnerAlphaMin = 0.15f * 0.65f;   // 0.15 × (1 ∓ 0.35)
        constexpr float kInnerAlphaMax = 0.15f * 1.35f;
        constexpr float kOuterAlphaMin = 0.05f * 0.65f;
        constexpr float kOuterAlphaMax = 0.05f * 1.35f;
        // 口径字面量（与单测同源、独立复述一遍；改回 3 球 / 6 层 / 380-620
        // 任一项都会红）
        constexpr int kWantBlobs = 6;
        constexpr int kWantLayers = 3;
        constexpr int kElementGate = 20;
        constexpr float kWantDiameterMin = 240.0f;
        constexpr float kWantDiameterMax = 400.0f;
        auto& probeAnim = ui->state<module::about::AboutAnimState>("about.anim");
        check(Anim::kBlobCount == kWantBlobs,
              "光斑数 " + std::to_string(Anim::kBlobCount) + "（应 6）");
        check(Anim::kLayersPerBlob == kWantLayers,
              "每球层数 " + std::to_string(Anim::kLayersPerBlob) + "（应 3）");
        for (int b = 0; b < Anim::kBlobCount; ++b) {
            const float d = probeAnim.blobs[static_cast<std::size_t>(b)].radius *
                            2.0f;
            check(d >= kWantDiameterMin - 1e-3f &&
                      d <= kWantDiameterMax + 1e-3f,
                  "blob" + std::to_string(b) + " 直径 " + std::to_string(d) +
                      "（应 240-400）");
        }
        int drawn = 0;
        for (int b = 0; b < Anim::kBlobCount; ++b) {
            for (int k = 0; k < Anim::kLayersPerBlob; ++k) {
                const auto* layer = ui->find("about.glow." + std::to_string(b) +
                                             "." + std::to_string(k));
                if (layer == nullptr) {
                    std::cerr << "about_probe: about.glow." << b << "." << k
                              << " 不存在\n";
                    passed = false;
                    continue;
                }
                ++drawn;
                const float alpha = layer->color.a;
                if (k == 0) {
                    check(alpha >= kInnerAlphaMin - 1e-4f &&
                              alpha <= kInnerAlphaMax + 1e-4f,
                          "光斑内层 alpha " + std::to_string(alpha));
                } else if (k == Anim::kLayersPerBlob - 1) {
                    check(alpha >= kOuterAlphaMin - 1e-4f &&
                              alpha <= kOuterAlphaMax + 1e-4f,
                          "光斑外层 alpha " + std::to_string(alpha));
                    // 最外圈霓虹边：shadow(blur 48, offset 0, primary@0.35)
                    check(layer->shadow.enabled, "最外圈应挂 shadow");
                    check(nearEq(layer->shadow.blur, 48.0f),
                          "霓虹 shadow blur " + std::to_string(layer->shadow.blur));
                    check(nearEq(layer->shadow.color.a, 0.35f),
                          "霓虹 shadow alpha " +
                              std::to_string(layer->shadow.color.a));
                    check(std::fabs(layer->shadow.offset.x) < 0.01f &&
                              std::fabs(layer->shadow.offset.y) < 0.01f,
                          "霓虹 shadow 应 offset 0");
                } else {
                    // 内层柔边 shadow（blur 56）：层数 6→3 后台阶更高，
                    // 靠这条把「单位像素 alpha 梯度不升」钉住（改回 40 即红）
                    check(layer->shadow.enabled, "内层应挂柔边 shadow");
                    check(nearEq(layer->shadow.blur, 56.0f),
                          "柔边 shadow blur " +
                              std::to_string(layer->shadow.blur) + "（应 56）");
                }
            }
        }
        for (int half = 0; half < 2; ++half) {
            const auto* band =
                ui->find("about.sweep." + std::to_string(half));
            if (band == nullptr) {
                std::cerr << "about_probe: about.sweep." << half << " 不存在\n";
                passed = false;
                continue;
            }
            ++drawn;
            check(band->gradient.enabled, "扫带应启用渐变");
            const float peak =
                std::max(band->gradient.start.a, band->gradient.end.a);
            check(nearEq(peak, 0.05f),
                  "扫带峰值 alpha " + std::to_string(peak) + "（应 0.05）");
            check(nearEq(band->transform.rotate, 0.31415926f),
                  "扫带倾角 " + std::to_string(band->transform.rotate) +
                      "（应 18°）");
        }
        // 元素闸门 ≤ 20（光斑 6×3 + 扫带 2 = 20；球数翻倍靠层数减半守住）
        check(drawn == kWantBlobs * kWantLayers + 2,
              "光晕元素数 " + std::to_string(drawn) + "（应 20）");
        check(drawn <= kElementGate,
              "元素闸门 ≤ 20，实为 " + std::to_string(drawn));
        check(ui->find("about.ring.0") == nullptr, "旧版浮环应已下线");

        const auto* bg = ui->find("about.bg");
        const auto* layer0 = ui->find("about.glow.0.0");
        if (bg == nullptr || layer0 == nullptr || bg->onTimer == nullptr) {
            std::cerr << "about_probe: bg/glow0 缺失或未挂 onTimer（唤醒期应有）\n";
            passed = false;
        } else {
            check(nearEq(bg->timerSeconds, 1.0f / 30.0f),
                  "驱动节拍 " + std::to_string(bg->timerSeconds) + "（应 1/30 s）");
            const float x0 = layer0->frame.x;
            const float y0 = layer0->frame.y;
            for (int f = 0; f < 90; ++f) composeFrame();  // 3s 节拍（唤醒期内）
            const auto* layer0b = ui->find("about.glow.0.0");
            if (layer0b == nullptr) {
                std::cerr << "about_probe: 重 compose 后 glow.0.0 丢失\n";
                passed = false;
            } else {
                const float moved = std::fabs(layer0b->frame.x - x0) +
                                    std::fabs(layer0b->frame.y - y0);
                check(moved > 1.0f,
                      "glow.0.0 位移 " + std::to_string(moved) +
                          "（应 > 1px，真在动）");
            }
            // 空闲 120s+ → 休眠（bg 不再挂 onTimer）。探针 dt = 1/60、节拍
            // 1/30 → 每 2 帧推进一 tick，故迭代数 ×2（另加 8 tick 余量）
            const int idleTicks = static_cast<int>(Anim::kWakeHoldSeconds /
                                                   Anim::kFrameSeconds) + 8;
            for (int f = 0; f < idleTicks * 2; ++f) composeFrame();
            const auto* bgIdle = ui->find("about.bg");
            check(bgIdle != nullptr && bgIdle->onTimer == nullptr,
                  "空闲 120s 后 bg 应休眠（不挂 onTimer）");
            // 指针移动注入（真命中分发）→ 唤醒，重新挂 onTimer
            core::queuePointerMotion(window, w * 0.5, h * 0.4,
                                     core::PointerButtons{}, core::KeyModifiers{});
            composeFrame();
            const auto* bgWake = ui->find("about.bg");
            check(bgWake != nullptr && bgWake->onTimer != nullptr,
                  "指针移动后 bg 应重新挂 onTimer（唤醒路径）");
        }
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
        // --dump 采样点：先跑满 warmup 帧再取 —— --light 是运行期切主题，
        // 元素颜色有 transition，切完立刻取样会把「主题过渡动画」算进抖动
        //（实测：浅色档不 warmup 时 ROI 均值虚高到 72/255，全是过渡帧）。
        // warmup 后按**动画时间**（onTimer 节拍推进的 anim.time，1 tick = 1/30s）
        // 取一对、间隔 1.2s——真机验收口径「同一位置、间隔 1.2s 抓两帧」，
        // 不靠迭代计数猜（渲染节奏与动画节奏解耦）
        constexpr int kDumpWarmupFrames = 120;  // ≈2s 墙钟（60fps 渲染循环）
        float dumpTimeA = -1.0f;
        const auto animClock = [&]() -> float {
            return ui->state<module::about::AboutAnimState>("about.anim").time;
        };
        for (int i = 0; i < 90 * 60; ++i) {
            // 固定点注入：只为把 bg.onMove 的唤醒窗续上（光晕要一直动），
            // **坐标恒定**——变坐标会在卡片上扫过 hover 态，把 hover 反馈算进
            // 抖动测量里（假高）。点选在 hero 上方空白（页面内、内容区之上）
            if (i % 120 == 0) {
                core::queuePointerMotion(window, w * 0.5, 20.0,
                                         core::PointerButtons{},
                                         core::KeyModifiers{});
            }
            glfwPollEvents();
            composeFrame();
            if (renderBackend) {
                core::render::ScopedRenderBackend scoped(*renderBackend);
                renderBackend->beginFrame(core::render::RenderSurface{
                    window, core::window::nativeWindowInfo(window), fbW, fbH,
                    renderScale});
                runtime.render(fbW, fbH, renderScale, clearColor);
                // dump 必须在 present() **之前**：swap 之后后备缓冲是未定义内容
                //（旧版在 present 前 dump；2026-09-29 一次改到 present 后，抓到的
                // _a 整帧全黑、差分虚高到 45/255 —— 假绿/假红都危险）
                bool dumpDone = false;
                if (!dumpPrefix.empty() && i >= kDumpWarmupFrames) {
                    const float now = animClock();
                    if (dumpTimeA < 0.0f) {
                        dumpFrame(dumpPrefix + "_a.ppm");
                        dumpTimeA = now;
                    } else if (now >= dumpTimeA + 1.2f) {
                        dumpFrame(dumpPrefix + "_b.ppm");
                        std::cout << "about_probe: 帧对间隔 " << (now - dumpTimeA)
                                  << "s 动画时间\n";
                        dumpDone = true;
                    }
                }
                renderBackend->present();
                if (dumpDone) {
                    break;  // 帧对已取到，提前收工（有界）
                }
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
