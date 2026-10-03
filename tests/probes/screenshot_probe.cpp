// tests/probes/screenshot_probe.cpp — probe 帧捕获能力（--screenshot）自测
//
// 被测对象：tests/probes/probe_harness.h 的「帧捕获」段（process 内
// glReadPixels → PNG）。为什么要有这个 probe：screencapture 在 agent 会话里拿不到
// 「屏幕录制」TCC 授权，视觉证据链曾长期只有几何断言；这条能力一旦接错（PNG 写坏 /
// 翻轴翻反 / 全黑 / 开关没生效），只有看图的人才会发现，而看图的人恰好不在场。
//
// 五组断言，逐条对应一个真实失效模式：
//   ① writeRgbaToPng 往返（不起窗口）：合成 6x4 逐像素不同的 RGBA → 写 PNG →
//      **用 libpng 独立解码**读回 → 尺寸 + 逐像素精确相等。证「写出来的是合法
//      PNG、像素没串」；同时证明编码路径不依赖 GL 上下文。
//   ② flipRowsToTopDown：2x3 逐行不同色 → 翻轴后行序反转。证「GL 左下原点 →
//      PNG 顶行」的翻轴是对的（不翻 = 上下颠倒图）。
//   ③ 失败必须显式：往不存在的目录写 PNG → 返回 false（不静默假成功）。
//   ④ 零开销：未传 --screenshot 时 captureIfRequested 返 true 且**一个文件都没写**
//      （哨兵路径必须不存在）——既有 probe 行为不变的保证。
//   ⑤ 真窗口抓图（live）：铺满底色 A + 上半屏色块 B → capturePng →
//      魔数 \x89PNG 逐字节核 + 手解 IHDR 宽高 + libpng 解码回像素，
//      断言 **上半屏是 B、下半屏是 A**（翻轴错立刻红）+ 两种颜色都真实出现
//      （证不是全黑/全空）+ --screenshot 开关真能落盘。
//
// 产物写 tests 的临时目录（test_support::TempDir，析构整棵删），不污染仓库；
// ctest 的 --screenshot 变体把图留在构建目录（build/ 已 gitignore）。
#include "probes/probe_harness.h"  // GL / GLFW 头由 harness 统一给（glad 路线）
#include "test_support.h"

#include <png.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool gPassed = true;

bool check(bool ok, const std::string& what) {
    if (!ok) {
        gPassed = false;
        std::cerr << "screenshot_probe: FAIL——" << what << "\n";
    }
    return ok;
}

/// 合成 top-down RGBA 图：每像素值 = (x*7+y*13, y*29, x*3+11, 255)。
void fillSynthetic(std::vector<unsigned char>& rgba, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)) *
                4u;
            rgba[at + 0] = static_cast<unsigned char>(x * 7 + y * 13);
            rgba[at + 1] = static_cast<unsigned char>(y * 29);
            rgba[at + 2] = static_cast<unsigned char>(x * 3 + 11);
            rgba[at + 3] = 255;
        }
    }
}

/// 用 libpng（与写图同族但独立调用路径）把 PNG 读回 top-down RGBA。
/// 解不开返回 false 并把 libpng 的 message 打进 report。
bool decodePng(const std::string& path,
               std::vector<unsigned char>& rgba,
               int& width,
               int& height) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        std::cerr << "screenshot_probe: libpng 读不开 '" << path
                  << "': " << image.message << "\n";
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    width = static_cast<int>(image.width);
    height = static_cast<int>(image.height);
    rgba.assign(PNG_IMAGE_SIZE(image), 0u);
    if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) {
        std::cerr << "screenshot_probe: libpng 解码失败 '" << path
                  << "': " << image.message << "\n";
        png_image_free(&image);
        return false;
    }
    png_image_free(&image);
    return true;
}

/// 手解 PNG 头：魔数 + IHDR 宽高（不依赖任何库，证「字节级就是 PNG」）。
/// 不认识的文件返回 false。
bool inspectPngHeader(const std::string& path, int& width, int& height) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    unsigned char head[24] = {0};
    in.read(reinterpret_cast<char*>(head), sizeof(head));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(head))) {
        return false;
    }
    static const unsigned char kMagic[8] = {0x89, 'P', 'N', 'G',
                                           0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i) {
        if (head[i] != kMagic[i]) {
            return false;
        }
    }
    // 8..11 = "IHDR" chunk 长度；12..15 = "IHDR"；宽高都是大端 32 位
    if (head[12] != 'I' || head[13] != 'H' || head[14] != 'D' ||
        head[15] != 'R') {
        return false;
    }
    width = (static_cast<int>(head[16]) << 24) |
            (static_cast<int>(head[17]) << 16) |
            (static_cast<int>(head[18]) << 8) |
            static_cast<int>(head[19]);
    height = (static_cast<int>(head[20]) << 24) |
             (static_cast<int>(head[21]) << 16) |
             (static_cast<int>(head[22]) << 8) |
             static_cast<int>(head[23]);
    return true;
}

unsigned char* pixelAt(std::vector<unsigned char>& rgba, int width, int x, int y) {
    return rgba.data() +
           (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x)) *
               4u;
}

/// 颜色相等（逐通道 ±tol）：浮点 eui::Color → GL 8bit 有舍入，容 2/255 足够
/// 判别（断言用的两种颜色通道差 >100），又不至于被舍入打红。
bool nearColor(const unsigned char* px, int r, int g, int b, int tol = 4) {
    const int dr = static_cast<int>(px[0]) - r;
    const int dg = static_cast<int>(px[1]) - g;
    const int db = static_cast<int>(px[2]) - b;
    return dr <= tol && dr >= -tol && dg <= tol && dg >= -tol && db <= tol &&
           db >= -tol;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    test_support::TempDir tmp("shot");

    // ==================== ① writeRgbaToPng 往返（纯 CPU，不起窗口）============
    {
        constexpr int kW = 6;
        constexpr int kH = 4;
        std::vector<unsigned char> src(
            static_cast<std::size_t>(kW) * kH * 4u, 0u);
        fillSynthetic(src, kW, kH);
        const std::string path = tmp.file("roundtrip.png").string();
        if (!check(probe::writeRgbaToPng(path, src.data(), kW, kH),
                   "① writeRgbaToPng 对合法参数返回 false")) {
            // 写不出图，后续读回无从谈起
        } else {
            std::vector<unsigned char> back;
            int w = 0;
            int h = 0;
            if (check(decodePng(path, back, w, h), "① libpng 读不回写出的 PNG")) {
                check(w == kW && h == kH,
                      "① 尺寸往返错：写出 6x4，读回 " + std::to_string(w) + "x" +
                          std::to_string(h));
                std::size_t mismatch = 0;
                const std::size_t count =
                    std::min(src.size(), back.size());
                for (std::size_t i = 0; i < count; ++i) {
                    if (src[i] != back[i]) {
                        ++mismatch;
                    }
                }
                check(mismatch == 0 && src.size() == back.size(),
                      "① 像素往返错：" + std::to_string(mismatch) +
                          " 字节不一致（写入 " +
                          std::to_string(src.size()) + " / 读回 " +
                          std::to_string(back.size()) + "）");
            }
        }
    }

    // ==================== ② flipRowsToTopDown（翻轴）============
    {
        constexpr int kW = 2;
        constexpr int kH = 3;
        std::vector<unsigned char> rgba(
            static_cast<std::size_t>(kW) * kH * 4u, 0u);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                unsigned char* px =
                    rgba.data() +
                    (static_cast<std::size_t>(y) * kW + x) * 4u;
                px[0] = static_cast<unsigned char>(y);  // 行号即色
                px[1] = px[2] = px[0];
                px[3] = 255;
            }
        }
        probe::flipRowsToTopDown(rgba, kW, kH);
        const bool reversed =
            pixelAt(rgba, kW, 0, 0)[0] == 2 &&
            pixelAt(rgba, kW, 0, 1)[0] == 1 &&
            pixelAt(rgba, kW, 0, 2)[0] == 0;
        check(reversed,
              "② flipRowsToTopDown 没把行序反转（GL 左下原点 → PNG 顶行）；"
              "抓出来的图会上下颠倒");
    }

    // ==================== ③ 失败必须显式 ====================
    {
        const std::string bad =
            tmp.file("no_such_dir/deeper/x.png").string();
        std::vector<unsigned char> one(4u, 200u);
        const bool ok =
            probe::writeRgbaToPng(bad, one.data(), 1, 1);
        check(!ok, "③ 往不存在的目录写 PNG 却返回 true（静默假成功）");
        std::vector<unsigned char> nullish;
        check(!probe::writeRgbaToPng(tmp.file("bad.png").string(), nullptr, 4, 4),
              "③ rgba=nullptr 却返回 true");
        check(!probe::writeRgbaToPng(tmp.file("bad2.png").string(),
                                     one.data(), 0, 4),
              "③ width=0 却返回 true");
    }

    // ==================== ④ 零开销：未传 --screenshot 不写任何文件 ============
    // 放在建窗之后跑：传真实的 window/runtime，证明零开销路径「连窗口都不碰」
    // ——只提前 return，不建渲染后端、不渲染、不落盘（既有 probe 行为不变）。

    // ==================== ⑤ 真窗口抓图（live）====================
    if (!probe::initializeWindowBackend()) {
        std::cerr << "screenshot_probe: 窗口后端初始化失败\n";
        return 1;
    }
    auto window = probe::makeWindow("screenshot_probe", args.windowW, args.windowH);
    if (window == nullptr) {
        probe::terminateWindowBackend();
        std::cerr << "screenshot_probe: 窗口创建失败\n";
        return 2;
    }
    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // 已知纯色构图：下半屏 = 底色 A，上半屏 = 色块 B（铺满宽，不依赖缩放系数）
    constexpr int kBgR = 16, kBgG = 32, kBgB = 48;
    constexpr int kTopR = 224, kTopG = 80, kTopB = 32;
    const float logicalW = static_cast<float>(args.windowW);
    const float logicalH = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, "shot", logicalW, logicalH,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      auto bg = u.rect("shot.bg");
                                      bg.position(0.0f, 0.0f)
                                          .size(screen.width, screen.height)
                                          .color(eui::Color{
                                              kBgR / 255.0f, kBgG / 255.0f,
                                              kBgB / 255.0f, 1.0f});
                                      auto top = u.rect("shot.top");
                                      top.position(0.0f, 0.0f)
                                          .size(screen.width, screen.height * 0.5f)
                                          .color(eui::Color{
                                              kTopR / 255.0f, kTopG / 255.0f,
                                              kTopB / 255.0f, 1.0f});
                                  });
    };
    composeFrame();
    composeFrame();  // 第二帧让 transition/首帧布局收敛

    // ---- ④ 零开销：screenshotPath 为空 → 立刻返 true、零文件 ----
    {
        probe::ProbeArgs bare = args;
        bare.screenshotPath.clear();
        const std::string sentinel = tmp.file("must_not_exist.png").string();
        bool passedFlag = false;  // 成功路径不该被碰（仍为 false 即零改动）
        const bool ok =
            probe::captureIfRequested(bare, window, runtime, &passedFlag);
        check(ok, "④ 零开销路径返回 false（不该触碰窗口/渲染后端）");
        check(!passedFlag, "④ 零开销路径竟然置了 passed=false");
        std::error_code ec;
        check(!std::filesystem::exists(sentinel, ec) && !ec,
              "④ 零开销路径竟写了文件 " + sentinel);
    }

    check(ui != nullptr && ui->find("shot.top") != nullptr,
          "⑤ 构图失败：shot.top 未进 DOM");
    // 注：不套 assertDrawnAbove —— 两个 rect 各自是顶层根元素（无公共祖先），
    // 该 helper 只对有 LCA 的兄弟对有意义。真正的 z 序判据在下面：**像素**上
    // 色块 B 必须真盖住底色 A（assertDrawnAbove 那套 DOM 序在这里证不了画出来
    // 的结果，而我们要验的恰恰是画出来的结果）。

    const std::string livePath = tmp.file("live.png").string();
    {
        probe::FrameCapture capture(window);
        if (!check(capture.ok(), "⑤ FrameCapture 建失败：" + capture.error())) {
            runtime.shutdown(false);
            core::releaseInputQueue(window);
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return gPassed ? 0 : 1;
        }
        std::cout << "screenshot_probe: framebuffer " << capture.pixelWidth()
                  << "x" << capture.pixelHeight() << " scale " << capture.scale()
                  << "\n";
        if (!check(capture.capturePng(runtime, livePath),
                   "⑤ capturePng 返回 false（渲染或写盘失败）")) {
            runtime.shutdown(false);
            core::releaseInputQueue(window);
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return gPassed ? 0 : 1;
        }

        // ---- 魔数 + IHDR（字节级，与库无关）----
        int hdrW = 0;
        int hdrH = 0;
        if (check(inspectPngHeader(livePath, hdrW, hdrH),
                  "⑤ 落图缺 \\x89PNG 魔数 / 不是合法 PNG 头")) {
            check(hdrW == capture.pixelWidth() && hdrH == capture.pixelHeight(),
                  "⑤ IHDR 尺寸 " + std::to_string(hdrW) + "x" +
                      std::to_string(hdrH) + " != framebuffer " +
                      std::to_string(capture.pixelWidth()) + "x" +
                      std::to_string(capture.pixelHeight()));
        }

        // ---- libpng 独立解码 + 颜色断言 ----
        std::vector<unsigned char> rgba;
        int w = 0;
        int h = 0;
        if (check(decodePng(livePath, rgba, w, h), "⑤ libpng 解不开落图")) {
            check(w == capture.pixelWidth() && h == capture.pixelHeight(),
                  "⑤ libpng 读回尺寸与 framebuffer 不符");
            if (w == capture.pixelWidth() && h == capture.pixelHeight()) {
                const unsigned char* upper =
                    pixelAt(rgba, w, w / 2, h / 4);
                const unsigned char* lower =
                    pixelAt(rgba, w, w / 2, (h * 3) / 4);
                check(nearColor(upper, kTopR, kTopG, kTopB),
                      "⑤ 上半屏不是色块 B（实测 " +
                          std::to_string(upper[0]) + "," +
                          std::to_string(upper[1]) + "," +
                          std::to_string(upper[2]) +
                          "）——翻轴反了 / 没真渲染");
                check(nearColor(lower, kBgR, kBgG, kBgB),
                      "⑤ 下半屏不是底色 A（实测 " +
                          std::to_string(lower[0]) + "," +
                          std::to_string(lower[1]) + "," +
                          std::to_string(lower[2]) +
                          "）——抓到了空窗 / 全黑");
                // 全图统计：两种颜色都真实出现，且非纯色（非全黑/全空）
                std::size_t topPixels = 0;
                std::size_t bgPixels = 0;
                std::size_t blackPixels = 0;
                for (int y = 0; y < h; y += 4) {
                    for (int x = 0; x < w; x += 4) {
                        const unsigned char* px = pixelAt(rgba, w, x, y);
                        if (nearColor(px, kTopR, kTopG, kTopB)) ++topPixels;
                        if (nearColor(px, kBgR, kBgG, kBgB)) ++bgPixels;
                        if (px[0] < 2 && px[1] < 2 && px[2] < 2) ++blackPixels;
                    }
                }
                check(topPixels > 0 && bgPixels > 0,
                      "⑤ 全图缺色：B " + std::to_string(topPixels) + " 像素 / A " +
                          std::to_string(bgPixels) + " 像素");
                check(blackPixels == 0,
                      "⑤ 全图含 " + std::to_string(blackPixels) +
                          " 个纯黑采样点（抓到的是空窗）");
            }
        }
    }

    // ---- --screenshot 开关本身：传了就必须真落一张并可解 ----
    if (!args.screenshotPath.empty()) {
        std::cout << "screenshot_probe: --screenshot " << args.screenshotPath
                  << "\n";
        bool passedFlag = true;
        const bool ok =
            probe::captureIfRequested(args, window, runtime, &passedFlag);
        if (!check(ok && passedFlag,
                   "⑤ --screenshot " + args.screenshotPath + " 未成功落图")) {
            // captureIfRequested 已置 passedFlag=false 并在 stderr 说明原因
        }
        int w = 0;
        int h = 0;
        check(inspectPngHeader(args.screenshotPath, w, h) && w > 0 && h > 0,
              "⑤ --screenshot 落下的文件不是合法 PNG / 尺寸为 0");
        std::vector<unsigned char> rgba;
        check(decodePng(args.screenshotPath, rgba, w, h),
              "⑤ --screenshot 落下的 PNG 解不开");
    }

    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();

    if (gPassed) {
        std::cout << "screenshot_probe: PASS\n";
    } else {
        std::cerr << "FAIL\n";
    }
    return gPassed ? 0 : 1;
}
