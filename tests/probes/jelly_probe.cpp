// tests/probes/jelly_probe.cpp — B2 果冻效果验收探针（池 65 取证；need201 v2 升级）
//
// 不接任何业务模块：本探针自建一个最小页面（页面底 + 一张 jellyCard + 一块透明命中面），
// 离屏驱动 core::dsl::Runtime，逐态稳帧后采样落图。
//
// 驱动方式照抄方案 §10.0.1 的 hover 范式：命中面 `.onHover()` 写进程内 bool，每帧 compose
// 把它作为 jellyCard 的 `hovered` 参数传进去（v2 时间线由 hover 进入沿触发，jellyCard
// 内部用测试缝 jellyAnimStartForTests 暴露 t0 供本探针换算逐帧 elapsed）。
//
// 断言（退出码裁决，不靠人眼看图）：
//   A0v2 结构性判据：动画期（squash/hover 帧）每帧 compose 写进 element 的 radius /
//      scale 目标 = jellyPose(elapsed) 时间线值（坑①②：目标每帧换值）且 zIndex=20；
//      非动画帧（normal/restored）目标 = 常态 12/1.0 且 zIndex=0。
//   A1 果冻生效：拉伸段 hover 帧 vs normal 帧，角部 patch 逐像素显著差异（轮廓移动）。
//   A2 中心不漂：hover 帧 vs normal 帧，中心区逐像素一致（只是形变，不是变色）。
//   A3 复位：指针移出稳帧后，角部与中心都回到 normal 值（≤2/通道）。
//   A4 明显性（need201）：hover 后 ~45ms 压扁中段 vs normal，角部 patch 均值差显著
//      大于旧版水平（旧版 hover 1.02 实测均值差仅 ~0.95，判据 >2.0）。
//
// 用法：jelly_probe [--size 1000x700] [--out <目录>]
//   目录默认 ./.verify-screenshots/，落四张图：jelly-normal.png / jelly-squash.png /
//   jelly-hover.png / jelly-restored.png。数据根指到临时目录（core::paths 随它走，
//   不碰用户真实根）。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 b1_material_probe）
#include "test_support.h"

#include "core/theme.h"
#include "ui_kit/jelly.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <ratio>
#include <string>
#include <unistd.h>
#include <thread>
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

constexpr int kSettleFrames = 24;  // 400ms@60fps，盖过 0.18s 的回收过渡

// 采样态次序：常态 → 压扁中段（P1）→ 拉伸段（P2）→ 复位
enum StateIdx { kNormal = 0, kSquash = 1, kHover = 2, kRestored = 3, kStateCount = 4 };

// 采样 patch（逻辑 px，相对卡 frame 的偏移）：角部取 10x10——压扁/拉伸/圆角变化的
// 轮廓差都落在这个带内；中心取 40x20，各态都完全在卡内，只该有形变不该有变色。
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

// 三通道均值差（A4 明显性判据用；旧版 hover 1.02 实测该值仅 ~0.95）
float meanDiff(const PatchSample& a, const PatchSample& b) {
    if (!a.valid || !b.valid) return 0.0f;
    return (std::fabs(a.r - b.r) + std::fabs(a.g - b.g) +
            std::fabs(a.b - b.b)) /
           3.0f;
}

// compose 期遥测：jellyCard 写进 element 的目标值 + 同帧时间线 elapsed。
// 遥测在 compose 回调里就地采集（与 jellyCard 同帧同线程，elapsed 差仅微秒级），
// grab 时取到的即被采样帧的快照。
struct ComposeTelemetry {
    bool valid = false;
    float elapsedMs = -1.0f;
    float radius = 0.0f;
    float sx = 0.0f;
    float sy = 0.0f;
    int zIndex = -999;
};

ComposeTelemetry g_telemetry;

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
                // 遥测：同帧取 jellyCard 写入的目标值与时间线 elapsed（测试缝）
                const core::dsl::Element* cardEl = u.find("jelly.card");
                if (cardEl != nullptr) {
                    g_telemetry.valid = true;
                    g_telemetry.radius = cardEl->radius;
                    g_telemetry.sx = cardEl->transform.scale.x;
                    g_telemetry.sy = cardEl->transform.scale.y;
                    g_telemetry.zIndex = cardEl->zIndex;
                    std::chrono::steady_clock::time_point t0;
                    g_telemetry.elapsedMs =
                        ui_kit::jellyAnimStartForTests("jelly.card", t0)
                            ? std::chrono::duration<float, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count()
                            : -1.0f;
                }
            });
    };

    auto settle = [&]() {
        for (int i = 0; i < kSettleFrames; ++i) composeFrame();
    };

    // compose 至时间线（墙钟）elapsed ≥ targetMs，断帧条件用 compose 期遥测（=
    // 被采样帧的真实时间线时刻；帧后墙钟会被落图停顿污染，实测 ~400ms 级偏移）。
    // ⚠ composeModuleFrame 自身不做帧 pacing（实测 ~13µs/帧），而 jellyCard v2
    // 时间线吃真实 steady_clock——不 pacing 的话 600 帧才走 ~8ms 墙钟，压扁段根本
    // 到不了。按 60fps 节拍睡足每帧，墙钟与运行时动画钟同步推进。
    // cap 600 帧防 hover 未生效时死循环——没生效的话后续 A0v2/A4 会红，不静默。
    auto advanceTo = [&](float targetMs) {
        for (int i = 0; i < 600; ++i) {
            const auto frameStart = std::chrono::steady_clock::now();
            composeFrame();
            if (g_telemetry.valid && g_telemetry.elapsedMs >= targetMs) break;
            while (std::chrono::duration<float, std::milli>(
                       std::chrono::steady_clock::now() - frameStart)
                       .count() < 16.6f) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
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
    std::string shotNames[kStateCount];
    bool shotOk = true;
    PatchSample samples[kStateCount][kPatchCount];
    ComposeTelemetry tele[kStateCount];
    float cardRadius[kStateCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    float cardScaleX[kStateCount] = {0.0f, 0.0f, 0.0f, 0.0f};
    float cardScaleY[kStateCount] = {0.0f, 0.0f, 0.0f, 0.0f};

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
            cardScaleY[stateIdx] = card->transform.scale.y;
        } else {
            std::cerr << "jelly_probe: jelly.card 不在树中（state=" << stateIdx
                      << "）\n";
            shotOk = false;
        }
        tele[stateIdx] = g_telemetry;
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
    grab(kNormal, "jelly-normal.png", kCardX, kCardY);

    // ② hover：指针 motion 到卡中心（命中面拿 hover → 回调置 true → 进入沿起表），
    //    推进到压扁中段（~45ms，P1）采帧 —— A4 明显性判据样本
    const core::KeyModifiers noMods;
    core::queuePointerMotion(window, kCardX + kCardW * 0.5f,
                             kCardY + kCardH * 0.5f, core::PointerButtons{},
                             noMods);
    advanceTo(40.0f);
    grab(kSquash, "jelly-squash.png", kCardX, kCardY);

    // ③ 移出中断第一次动画 → 再进触发全新时间线 → 推进到拉伸段（~150ms，P2 中前部）
    //    采帧 —— A1/A2 主样本。（squash 落图有数百毫秒停顿，原时间线已烧过 P2；
    //    重触发保证 hover 帧落在拉伸段峰值邻域。）
    core::queuePointerMotion(window, w - 8.0f, h - 8.0f,
                             core::PointerButtons{}, noMods);
    composeFrame();  // 泵事件：hovered=false（中断 → 目标回常态，wasHovered 复位）
    core::queuePointerMotion(window, kCardX + kCardW * 0.5f,
                             kCardY + kCardH * 0.5f, core::PointerButtons{},
                             noMods);
    advanceTo(150.0f);
    grab(kHover, "jelly-hover.png", kCardX, kCardY);

    // ④ 复位：指针移出卡外（页面底无可交互元素），稳帧后应回到常态
    core::queuePointerMotion(window, w - 8.0f, h - 8.0f,
                             core::PointerButtons{}, noMods);
    settle();
    grab(kRestored, "jelly-restored.png", kCardX, kCardY);

    if (!shotOk) passed = false;

    // ── 输出与断言 ──
    static const char* stateNames[kStateCount] = {"normal", "squash", "hover",
                                                  "restored"};
    for (int s = 0; s < kStateCount; ++s) {
        for (int k = 0; k < kPatchCount; ++k) {
            std::cout << "rgb " << stateNames[s] << " " << kPatches[k].tag
                      << " " << fmtRgb(samples[s][k]) << "\n";
        }
        std::cout << "state " << stateNames[s] << " radius=" << cardRadius[s]
                  << " scale=" << cardScaleX[s] << "," << cardScaleY[s]
                  << " zIndex=" << tele[s].zIndex
                  << " elapsed=" << tele[s].elapsedMs << "\n";
    }

    // A0v2 结构性判据：动画期每帧 compose 写入目标 = jellyPose(elapsed) 时间线值
    // + zIndex(20)；非动画帧 = 常态目标（12/1.0）+ zIndex 0。
    bool a0 = true;
    for (int s : {kSquash, kHover}) {
        const ComposeTelemetry& tm = tele[s];
        if (!tm.valid || tm.elapsedMs < 0.0f) {
            std::cerr << "jelly_probe: A0v2 FAIL state=" << stateNames[s]
                      << " 无动画期遥测（hover 未生效？）\n";
            a0 = false;
            continue;
        }
        const ui_kit::JellyPose pose = ui_kit::jellyPose(tm.elapsedMs);
        const bool hit = std::fabs(tm.radius - pose.radius) < 0.05f &&
                         std::fabs(tm.sx - pose.sx) < 0.005f &&
                         std::fabs(tm.sy - pose.sy) < 0.005f &&
                         tm.zIndex == 20;
        if (!hit) {
            std::cerr << "jelly_probe: A0v2 FAIL state=" << stateNames[s]
                      << " elapsed=" << tm.elapsedMs
                      << " 目标(" << tm.radius << "," << tm.sx << ","
                      << tm.sy << ",z" << tm.zIndex
                      << ") ≠ 时间线(" << pose.radius << "," << pose.sx
                      << "," << pose.sy << ",z20)\n";
            a0 = false;
        }
    }
    for (int s : {kNormal, kRestored}) {
        const ComposeTelemetry& tm = tele[s];
        const bool rest = tm.valid && tm.elapsedMs < 0.0f &&
                          std::fabs(tm.radius - 12.0f) < 0.01f &&
                          std::fabs(tm.sx - 1.0f) < 0.01f &&
                          std::fabs(tm.sy - 1.0f) < 0.01f &&
                          tm.zIndex == 0;
        if (!rest) {
            std::cerr << "jelly_probe: A0v2 FAIL state=" << stateNames[s]
                      << " 非动画帧应为常态目标 12/1.0/1.0/z0，实测("
                      << tm.radius << "," << tm.sx << "," << tm.sy
                      << ",z" << tm.zIndex << ")\n";
            a0 = false;
        }
    }
    if (a0) {
        std::cout << "A0v2 PASS 每帧目标=时间线值（动画期 z20，常态 z0）\n";
    } else {
        passed = false;
    }

    // A1 果冻生效：拉伸段 vs 常态，角部 patch **逐像素**有显著差异。取 patch 均值
    // 会被大片同色区域稀释，所以判据是「至少 20 个像素通道差 >4」——压扁/拉伸/
    // 圆角过冲都在角部带内移动边缘，命中数远超噪声。
    const PixelDiff a1 = pixelDiff(samples[kHover][0], samples[kNormal][0]);
    if (a1.over4 < 20) {
        std::cerr << "jelly_probe: A1 FAIL 角部形变不可辨 maxDiff=" << a1.maxDiff
                  << " px>4=" << a1.over4 << "/" << a1.total << "\n";
        passed = false;
    } else {
        std::cout << "A1 PASS 角部形变可辨 maxDiff=" << a1.maxDiff << " px>4="
                  << a1.over4 << "/" << a1.total << "\n";
    }

    // A2 中心不漂：形变非变色。拉伸段 vs 常态中心区逐像素一致（边缘位移先动角部，
    // 中心区 40x20 离边远，各缩放相位下都完整在卡内）
    const PixelDiff a2 = pixelDiff(samples[kHover][1], samples[kNormal][1]);
    if (a2.over4 > 0) {
        std::cerr << "jelly_probe: A2 FAIL 中心漂移 maxDiff=" << a2.maxDiff
                  << " px>4=" << a2.over4 << "/" << a2.total << "\n";
        passed = false;
    } else {
        std::cout << "A2 PASS 中心不漂 maxDiff=" << a2.maxDiff << "\n";
    }

    // A3 复位：指针移出后角部与中心都回到常态（逐像素全一致）
    const PixelDiff a3Corner = pixelDiff(samples[kRestored][0], samples[kNormal][0]);
    const PixelDiff a3Center = pixelDiff(samples[kRestored][1], samples[kNormal][1]);
    if (a3Corner.over2 > 0 || a3Center.over2 > 0) {
        std::cerr << "jelly_probe: A3 FAIL 未复位 corner maxDiff="
                  << a3Corner.maxDiff << " center maxDiff=" << a3Center.maxDiff
                  << "\n";
        passed = false;
    } else {
        std::cout << "A3 PASS 复位（corner/center 逐像素一致）\n";
    }

    // A4 明显性（need201）：压扁中段 vs 常态，角部 patch 均值差显著大于旧版。
    // 旧版 hover 1.02 实测均值差仅 ~0.95（被大片同色稀释）；v2 压扁 sy≈0.92 让
    // 角部轮廓位移 ~8px，均值差应 >2.0（约旧版两倍）且逐像素下限不低于旧版 A1。
    const float a4Mean = meanDiff(samples[kSquash][0], samples[kNormal][0]);
    const PixelDiff a4Px = pixelDiff(samples[kSquash][0], samples[kNormal][0]);
    if (!(a4Mean > 2.0f) || a4Px.over4 < 20) {
        std::cerr << "jelly_probe: A4 FAIL 压扁不明显 meanDiff=" << a4Mean
                  << " maxDiff=" << a4Px.maxDiff
                  << " px>4=" << a4Px.over4 << "/" << a4Px.total
                  << "（elapsed=" << tele[kSquash].elapsedMs << "）\n";
        passed = false;
    } else {
        std::cout << "A4 PASS 压扁显著 meanDiff=" << a4Mean
                  << "（旧版 ~0.95） maxDiff=" << a4Px.maxDiff << " px>4="
                  << a4Px.over4 << "/" << a4Px.total << "\n";
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
