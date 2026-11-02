// tests/probes/jelly_probe.cpp — B2 果冻效果验收探针（池 65，F-65-3 取证）
//
// 不接任何业务模块：本探针自建一个最小页面（页面底 + 一张 jellyCard + 一块透明命中面），
// 离屏驱动 core::dsl::Runtime，逐态稳帧 24 帧（盖过 0.18s 的 OutBack 过渡）后采样落图。
//
// 驱动方式照抄方案 §10.0.1 的 hover 范式：命中面 `.onHover()` 写进程内 bool，每帧 compose
// 把它作为 jellyCard 的 `hovered` 参数传进去（果冻是 hover 驱动，不是持续动画）。
//
// 断言（退出码裁决，不靠人眼看图）：
//   A1 果冻生效：hover 帧 vs normal 帧，卡左上角圆角区 patch 均值不同（圆角 12→16 + 放大
//      1.02，两者都让角部覆盖更多）
//   A2 中心不漂：卡中心区 patch 均值 hover vs normal 差 ≤2/通道（只是形变，不是变色）
//   A3 复位：指针移出稳帧后，角部与中心都回到 normal 值（≤1/通道）
// 另打印 A0（结构性判据）：每帧 compose 写进 element 的 radius / scale 目标值必须是
// 12/1.0（normal）与 16/1.02（hover）——这条一旦不成立，A1 必然同时失效。
//
// 用法：jelly_probe [--size 1000x700] [--out <目录>]
//   目录默认 ./.verify-screenshots/，落三张图：jelly-normal.png / jelly-hover.png /
//   jelly-restored.png。数据根指到临时目录（core::paths 随它走，不碰用户真实根）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 b1_material_probe）
#include "test_support.h"

#include "core/theme.h"
#include "ui_kit/jelly.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（探针不建应用级 UI，compose 直接驱动自建页面）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_jelly_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

constexpr float kCardX = 240.0f;
constexpr float kCardY = 200.0f;
constexpr float kCardW = 400.0f;
constexpr float kCardH = 200.0f;

constexpr int kSettleFrames = 24;  // 400ms@60fps，盖过 0.18s 过渡（OutBack 回弹也走完）

// 采样 patch（逻辑 px，相对卡 frame 的偏移）：角部取 10x10——圆角 12→16 的轮廓差
// 与 1.02 放大都落在这个带内；中心取 40x20，两态都完全在卡内，只该有形变不该有变色。
struct Patch {
    const char* tag;
    float ox, oy, pw, ph;
};
constexpr Patch kPatches[] = {
    {"corner", 0.0f, 0.0f, 10.0f, 10.0f},
    {"center", 180.0f, 90.0f, 40.0f, 20.0f},
};
constexpr int kPatchCount = 2;

// 单 patch 的采样结果：RGB 均值 + 逐像素原始 RGBA（供逐像素判据），全为 0..255
struct PatchSample {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    std::vector<unsigned char> px;  // top-down RGBA，宽高见 w/h
    int w = 0;
    int h = 0;
    bool valid = false;
};

std::string fmtRgb(const PatchSample& c) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f,%.1f,%.1f", c.r, c.g, c.b);
    return buf;
}

// 逐像素最大通道差 + 超阈像素数：比 patch 均值更能抓住「轮廓移动」这种局部变化
// （均值被大片相同区域稀释，卡外还有阴影压对比）。
struct PixelDiff {
    int maxDiff = 0;
    int over2 = 0;
    int over4 = 0;
    int total = 0;
};

PixelDiff pixelDiff(const PatchSample& a, const PatchSample& b) {
    PixelDiff d;
    if (!a.valid || !b.valid || a.w != b.w || a.h != b.h) return d;
    d.total = a.w * a.h;
    for (int i = 0; i < d.total; ++i) {
        int m = 0;
        for (int c = 0; c < 3; ++c) {
            m = std::max(m, std::abs(static_cast<int>(a.px[i * 4 + c]) -
                                     static_cast<int>(b.px[i * 4 + c])));
        }
        d.maxDiff = std::max(d.maxDiff, m);
        if (m > 2) ++d.over2;
        if (m > 4) ++d.over4;
    }
    return d;
}

}  // namespace

int main(int argc, char* argv[]) {
    // scratch 数据根最先设：core::paths 随它走，不碰用户真实根
    const std::filesystem::path scratchRoot =
        std::filesystem::temp_directory_path() /
        ("devdesk_jelly_root_" +
         std::to_string(static_cast<int>(getpid())));
    std::error_code ec;
    std::filesystem::remove_all(scratchRoot, ec);
    std::filesystem::create_directories(scratchRoot, ec);
    test_support::setEnvVar("DEVDESK_DATA_ROOT", scratchRoot.string().c_str());

    const auto args = probe::parseProbeArgs(argc, argv);
    std::filesystem::path outDir = ".verify-screenshots";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) outDir = argv[++i];
    }
    std::filesystem::create_directories(outDir, ec);

    if (!probe::initializeWindowBackend()) return 1;
    auto window =
        probe::makeWindow("jelly_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "jelly_probe";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    const eui::Color kTransparent{0.0f, 0.0f, 0.0f, 0.0f};

    // hover 状态源头：命中面的 onHover 回调写它，jellyCard 每帧读它。
    // 用 bool 而非集合：本探针只有一个可 hover 目标，不需要 §10.0.1 的集合语义。
    bool hovered = false;
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(
            runtime, window, pageId, w, h,
            [&](eui::Ui& u, const eui::Screen& screen) {
                ui = &u;
                const auto& t = core::theme::current();
                u.rect("jelly.page")
                    .position(0.0f, 0.0f)
                    .size(screen.width, screen.height)
                    .color(t.colors.background)
                    .build();
                ui_kit::jellyCard(u, "jelly.card", kCardX, kCardY, kCardW,
                                  kCardH, t, {}, hovered);
                // 透明命中面铺满卡：三态同色 + zIndex 抬到卡之上（命中优先级显式设计）
                u.rect("jelly.hit")
                    .position(kCardX, kCardY)
                    .size(kCardW, kCardH)
                    .zIndex(20)
                    .states(kTransparent, kTransparent, kTransparent)
                    .onHover([&hovered](bool h) { hovered = h; })
                    .build();
            });
    };

    auto settle = [&]() {
        for (int i = 0; i < kSettleFrames; ++i) composeFrame();
    };

    probe::FrameCapture capture(window);
    if (!capture.ok()) {
        std::cerr << "jelly_probe: FrameCapture 失败: " << capture.error()
                  << "\n";
        std::cerr << "FAIL\n";
        probe::terminateWindowBackend();
        return 4;
    }
    const float scale = capture.scale();

    // 单 patch 逐像素求均值 + 留原始像素；越界整块判 invalid（不静默裁剪）
    auto samplePatch = [&](const std::vector<unsigned char>& rgba, int imgW,
                           int imgH, float lx, float ly,
                           const Patch& pt) -> PatchSample {
        const int x0 = static_cast<int>(std::lround((lx + pt.ox) * scale));
        const int y0 = static_cast<int>(std::lround((ly + pt.oy) * scale));
        const int x1 = static_cast<int>(std::lround((lx + pt.ox + pt.pw) * scale));
        const int y1 = static_cast<int>(std::lround((ly + pt.oy + pt.ph) * scale));
        PatchSample out;
        if (x0 < 0 || y0 < 0 || x1 > imgW || y1 > imgH || x1 <= x0 ||
            y1 <= y0) {
            return out;
        }
        out.w = x1 - x0;
        out.h = y1 - y0;
        out.px.reserve(static_cast<std::size_t>(out.w) * out.h * 4u);
        double r = 0.0, g = 0.0, b = 0.0;
        long count = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t at =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(imgW) +
                     static_cast<std::size_t>(x)) *
                    4u;
                out.px.push_back(rgba[at]);
                out.px.push_back(rgba[at + 1]);
                out.px.push_back(rgba[at + 2]);
                out.px.push_back(rgba[at + 3]);
                r += rgba[at];
                g += rgba[at + 1];
                b += rgba[at + 2];
                ++count;
            }
        }
        out.r = static_cast<float>(r / count);
        out.g = static_cast<float>(g / count);
        out.b = static_cast<float>(b / count);
        out.valid = true;
        return out;
    };

    bool passed = true;
    std::string shotNames[3];
    bool shotOk = true;
    PatchSample samples[3][kPatchCount];  // [normal/hover/restored][patch]
    float cardRadius[3] = {0.0f, 0.0f, 0.0f};
    float cardScaleX[3] = {0.0f, 0.0f, 0.0f};

    auto grab = [&](int stateIdx, const char* fileName, float lx, float ly) {
        std::vector<unsigned char> rgba;
        int iw = 0, ih = 0;
        if (!capture.readFrame(runtime, rgba, iw, ih)) {
            std::cerr << "jelly_probe: readFrame 失败（state=" << stateIdx
                      << "）\n";
            shotOk = false;
            return;
        }
        for (int k = 0; k < kPatchCount; ++k) {
            samples[stateIdx][k] =
                samplePatch(rgba, iw, ih, lx, ly, kPatches[k]);
            if (!samples[stateIdx][k].valid) {
                std::cerr << "jelly_probe: 采样越界 state=" << stateIdx
                          << " patch=" << kPatches[k].tag << "\n";
                shotOk = false;
            }
        }
        const core::dsl::Element* card = ui != nullptr ? ui->find("jelly.card")
                                                       : nullptr;
        if (card != nullptr) {
            cardRadius[stateIdx] = card->radius;
            cardScaleX[stateIdx] = card->transform.scale.x;
        } else {
            std::cerr << "jelly_probe: jelly.card 不在树中（state=" << stateIdx
                      << "）\n";
            shotOk = false;
        }
        const std::string path = (outDir / fileName).string();
        shotNames[stateIdx] = path;
        if (!probe::writeRgbaToPng(path, rgba.data(), iw, ih)) {
            std::cerr << "jelly_probe: 落图失败: " << path << "\n";
            shotOk = false;
        } else {
            std::cout << "shot " << fileName << " " << path << "\n";
        }
    };

    // ① 常态：无指针事件
    settle();
    {
        const core::dsl::Element* card = ui != nullptr ? ui->find("jelly.card")
                                                       : nullptr;
        if (card == nullptr) {
            std::cerr << "jelly_probe: jelly.card 缺失，无法取样\nFAIL\n";
            runtime.shutdown(false);
            core::releaseInputQueue(window);
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        std::cout << "frame jelly.card " << card->frame.x << " " << card->frame.y
                  << " " << card->frame.width << " " << card->frame.height
                  << "\n";
    }
    grab(0, "jelly-normal.png", kCardX, kCardY);

    // ② hover：指针 motion 到卡中心（命中面拿 hover → 回调置 true → 下帧 compose 写入形变目标）
    const core::KeyModifiers noMods;
    core::queuePointerMotion(window, kCardX + kCardW * 0.5f,
                             kCardY + kCardH * 0.5f, core::PointerButtons{},
                             noMods);
    settle();
    grab(1, "jelly-hover.png", kCardX, kCardY);

    // ③ 复位：指针移出卡外（页面底无可交互元素），稳帧后应回到常态
    core::queuePointerMotion(window, w - 8.0f, h - 8.0f,
                             core::PointerButtons{}, noMods);
    settle();
    grab(2, "jelly-restored.png", kCardX, kCardY);

    if (!shotOk) passed = false;

    // ── 输出与断言 ──
    static const char* stateNames[3] = {"normal", "hover", "restored"};
    for (int s = 0; s < 3; ++s) {
        for (int k = 0; k < kPatchCount; ++k) {
            std::cout << "rgb " << stateNames[s] << " " << kPatches[k].tag << " "
                      << fmtRgb(samples[s][k]) << "\n";
        }
        std::cout << "state " << stateNames[s] << " radius=" << cardRadius[s]
                  << " scale=" << cardScaleX[s] << "\n";
    }

    // A0 结构性判据：每帧 compose 写入的 radius / scale 目标值
    const bool a0 = std::fabs(cardRadius[0] - 12.0f) < 0.01f &&
                    std::fabs(cardScaleX[0] - 1.0f) < 0.01f &&
                    std::fabs(cardRadius[1] - 16.0f) < 0.01f &&
                    std::fabs(cardScaleX[1] - 1.02f) < 0.01f;
    if (!a0) {
        std::cerr << "jelly_probe: A0 FAIL 形变目标值未按 hover 每帧写入 normal="
                  << cardRadius[0] << "/" << cardScaleX[0] << " hover="
                  << cardRadius[1] << "/" << cardScaleX[1] << "\n";
        passed = false;
    } else {
        std::cout << "A0 PASS 每帧目标值 normal=12/1.0 hover=16/1.02\n";
    }

    // A1 果冻生效：角部 patch **逐像素**有显著差异。取 patch 均值会被大片同色区域稀释
    // （实测均值差仅 ~0.95），所以判据是「至少 N 个像素通道差 >4」——圆角轮廓从 12 移到
    // 16、外加 1.02 放大，两者都在角部带内移动边缘，命中数远超噪声。
    const PixelDiff a1 = pixelDiff(samples[1][0], samples[0][0]);
    if (a1.over4 < 20) {
        std::cerr << "jelly_probe: A1 FAIL 角部形变不可辨 maxDiff=" << a1.maxDiff
                  << " px>4=" << a1.over4 << "/" << a1.total << "\n";
        passed = false;
    } else {
        std::cout << "A1 PASS 角部形变可辨 maxDiff=" << a1.maxDiff << " px>4="
                  << a1.over4 << "/" << a1.total << "\n";
    }
    // A2 中心不漂：形变非变色。逐像素同样判（边缘若真的发生位移，中心区会先亮）
    const PixelDiff a2 = pixelDiff(samples[1][1], samples[0][1]);
    if (a2.over4 > 0) {
        std::cerr << "jelly_probe: A2 FAIL 中心漂移 maxDiff=" << a2.maxDiff
                  << " px>4=" << a2.over4 << "/" << a2.total << "\n";
        passed = false;
    } else {
        std::cout << "A2 PASS 中心不漂 maxDiff=" << a2.maxDiff << "\n";
    }
    // A3 复位：指针移出后角部与中心都回到常态（逐像素全一致）
    const PixelDiff a3Corner = pixelDiff(samples[2][0], samples[0][0]);
    const PixelDiff a3Center = pixelDiff(samples[2][1], samples[0][1]);
    if (a3Corner.over2 > 0 || a3Center.over2 > 0) {
        std::cerr << "jelly_probe: A3 FAIL 未复位 corner maxDiff="
                  << a3Corner.maxDiff << " center maxDiff=" << a3Center.maxDiff
                  << "\n";
        passed = false;
    } else {
        std::cout << "A3 PASS 复位（corner/center 逐像素一致）\n";
    }

    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
        return 1;
    }
    std::cout << "jelly_probe: PASS\n";
    return 0;
}
