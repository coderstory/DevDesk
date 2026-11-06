// tests/test_modules/test_wallpaper_macos_platform.cpp
// macOS 壁纸平台层的**纯逻辑**部分：图片铺法几何、全屏覆盖率聚合、图片尺寸头解码。
//
// 三块都是跨平台可测的（几何/聚合零平台依赖；图片探针在 macOS 上用 ImageIO，
// 其它平台走 stub 恒 unknown，用例按平台分支）。真桌面 / 真播放归 probes。
// REGISTER_TEST 描述串必须含 ASCII "wallpaper"（--filter 只认 ASCII）。
#include "doctest.h"

#include "module_wallpaper/platform/fullscreen_geometry.h"
#include "module_wallpaper/platform/image_fit_geometry.h"
#include "module_wallpaper/platform/image_probe.h"
#include "module_wallpaper/platform/importer_macos.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fit = module::wallpaper::image_fit_geometry;
namespace fs = module::wallpaper::fullscreen_geometry;
using module::wallpaper::ImageFit;

bool near(double a, double b, double tolerance = 1e-6) {
    return std::fabs(a - b) <= tolerance;
}

} // namespace

// ── 图片铺法几何 ──

REGISTER_TEST(wallpaper_macos_fit_fill_covers_canvas,
              "wallpaper macOS: 填充铺法铺满画布、居中、允许溢出（比例不变）", {
    // 填充：铺满画布、比例不变、居中、允许超出。
    // 图 1000×1000 → 画布 1600×900，scale = max(1.6, 0.9) = 1.6 → 1600×1600。
    const fit::Placement placement = fit::placement(1000, 1000, 1600, 900, ImageFit::Fill);
    DOCTEST_CHECK_FALSE(placement.tiles);
    DOCTEST_CHECK(near(placement.rect.w, 1600.0));
    DOCTEST_CHECK(near(placement.rect.h, 1600.0));
    DOCTEST_CHECK(near(placement.rect.x, 0.0));
    // 垂直居中：(900 - 1600) / 2 = -350（上下各溢出 350，这是「填充」的定义）
    DOCTEST_CHECK(near(placement.rect.y, -350.0));
    return true;
})

REGISTER_TEST(wallpaper_macos_fit_fit_contained,
              "wallpaper macOS: 适应铺法完整可见、留边、居中（不裁切）", {
    // 适应：完整可见、留边、居中。同一组数 → scale = min(1.6, 0.9) = 0.9 → 900×900。
    const fit::Placement placement = fit::placement(1000, 1000, 1600, 900, ImageFit::Fit);
    DOCTEST_CHECK_FALSE(placement.tiles);
    DOCTEST_CHECK(near(placement.rect.w, 900.0));
    DOCTEST_CHECK(near(placement.rect.h, 900.0));
    DOCTEST_CHECK(near(placement.rect.x, 350.0));  // (1600-900)/2
    DOCTEST_CHECK(near(placement.rect.y, 0.0));
    return true;
})

REGISTER_TEST(wallpaper_macos_fit_center_original_size,
              "wallpaper macOS: 居中铺法按原始像素尺寸、不缩放", {
    // 居中：原始像素尺寸、不缩放。
    const fit::Placement placement = fit::placement(1000, 400, 1600, 900, ImageFit::Center);
    DOCTEST_CHECK(near(placement.rect.w, 1000.0));
    DOCTEST_CHECK(near(placement.rect.h, 400.0));
    DOCTEST_CHECK(near(placement.rect.x, 300.0));
    DOCTEST_CHECK(near(placement.rect.y, 250.0));
    return true;
})

REGISTER_TEST(wallpaper_macos_fit_tile_anchored_top_left,
              "wallpaper macOS: 平铺砖块为原始尺寸且锚左上角（不居中）", {
    // 平铺：砖块 = 原始像素尺寸、锚左上角（不居中——居中会让平铺错位半块砖）。
    const fit::Placement placement = fit::placement(320, 180, 1600, 900, ImageFit::Tile);
    DOCTEST_CHECK(placement.tiles);
    DOCTEST_CHECK(near(placement.rect.x, 0.0));
    DOCTEST_CHECK(near(placement.rect.y, 0.0));
    DOCTEST_CHECK(near(placement.rect.w, 320.0));
    DOCTEST_CHECK(near(placement.rect.h, 180.0));
    return true;
})

REGISTER_TEST(wallpaper_macos_fit_degenerate_inputs,
              "wallpaper macOS: 铺法几何对零/负输入降级为零矩形（不崩不除零）", {
    // 任一边非正 → 零矩形（不另起「什么都不画」分支，避免两条路径里漏一条）。
    for (const auto& sizes : {std::pair<double, double>{0, 100}, {100, 0},
                              {-5, 100}, {100, -5}}) {
        const fit::Placement placement =
            fit::placement(sizes.first, sizes.second, 1600, 900, ImageFit::Fill);
        DOCTEST_CHECK(placement.rect.w == 0 && placement.rect.h == 0);
    }
    const fit::Placement zeroCanvas = fit::placement(100, 100, 0, 900, ImageFit::Fill);
    DOCTEST_CHECK(zeroCanvas.rect.w == 0 && zeroCanvas.rect.h == 0);
    return true;
})

REGISTER_TEST(wallpaper_macos_fit_four_modes_differ,
              "wallpaper macOS: 四种铺法产生四种不同落位（防枚举变了几何没跟）", {
    // 四种铺法必须真的产生四种不同的落位（防「枚举变了但几何没跟上」）。
    const double w = 1000, h = 1000, cw = 1600, ch = 900;
    const auto fill = fit::placement(w, h, cw, ch, ImageFit::Fill);
    const auto contain = fit::placement(w, h, cw, ch, ImageFit::Fit);
    const auto center = fit::placement(w, h, cw, ch, ImageFit::Center);
    const auto tile = fit::placement(w, h, cw, ch, ImageFit::Tile);
    DOCTEST_CHECK(!near(fill.rect.w, contain.rect.w));
    DOCTEST_CHECK(!near(contain.rect.w, center.rect.w));
    DOCTEST_CHECK(tile.tiles && !fill.tiles && !contain.tiles && !center.tiles);
    return true;
})

// ── 全屏覆盖率聚合 ──

REGISTER_TEST(wallpaper_macos_fullscreen_single_window_covers,
              "wallpaper macOS: 单窗口铺满 visibleFrame 判全覆盖", {
    // 单窗口铺满 visibleFrame → 覆盖率 1.0。
    fs::Rect visible{0, 0, 1600, 900};
    std::vector<fs::WindowSample> samples{
        {1234, fs::Rect{0, 0, 1600, 900}}};
    const fs::CoverageResult coverage =
        fs::aggregate(samples, visible, 900, fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(coverage.global, 1.0));
    DOCTEST_CHECK(fs::covers(coverage));
    DOCTEST_CHECK_EQ(coverage.globalPid, 1234LL);
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_half_window_not_covering,
              "wallpaper macOS: 半屏窗口不算全屏让路", {
    fs::Rect visible{0, 0, 1600, 900};
    std::vector<fs::WindowSample> samples{
        {1234, fs::Rect{0, 0, 1600, 450}}};
    const fs::CoverageResult coverage =
        fs::aggregate(samples, visible, 900, fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(coverage.global, 0.5));
    DOCTEST_CHECK_FALSE(fs::covers(coverage));
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_same_pid_two_windows_aggregate,
              "wallpaper macOS: 同 pid 双窗口上下拼接按 pid 聚合判全覆盖", {
    // 同一 app 上下拼两块各半屏 → 按 pid 求和后覆盖率 1.0。
    // 这是「按 pid 聚合」承重的证据：逐窗口口径会永远 ≤0.5，必漏判。
    fs::Rect visible{0, 0, 1600, 900};
    std::vector<fs::WindowSample> samples{
        {777, fs::Rect{0, 0, 1600, 450}},
        {777, fs::Rect{0, 450, 1600, 450}}};
    const fs::CoverageResult coverage =
        fs::aggregate(samples, visible, 900, fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(coverage.global, 1.0));
    DOCTEST_CHECK(fs::covers(coverage));
    DOCTEST_CHECK_EQ(coverage.rectCount, 2);
    // 逐窗口最佳值只有 0.5 —— 两者不等即证明聚合没退化成逐窗口。
    DOCTEST_CHECK(near(coverage.perWindowBest, 0.5));
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_different_pids_take_max,
              "wallpaper macOS: 不同 app 各占一半取最大值不算全覆盖", {
    // 两个不同 app 各占一半 → 取最大值 0.5，不算满覆盖。
    fs::Rect visible{0, 0, 1600, 900};
    std::vector<fs::WindowSample> samples{
        {1, fs::Rect{0, 0, 1600, 450}},
        {2, fs::Rect{0, 450, 1600, 450}}};
    const fs::CoverageResult coverage =
        fs::aggregate(samples, visible, 900, fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(coverage.global, 0.5));
    DOCTEST_CHECK_FALSE(fs::covers(coverage));
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_coverage_capped_at_one,
              "wallpaper macOS: 溢出屏幕的窗口覆盖率封顶 1.0", {
    // 溢出窗口（比屏幕还大的全屏游戏）覆盖率封顶 1.0，不出现 1.2 这种数。
    fs::Rect visible{0, 0, 1600, 900};
    std::vector<fs::WindowSample> samples{
        {9, fs::Rect{-200, -200, 2000, 1300}}};
    const fs::CoverageResult coverage =
        fs::aggregate(samples, visible, 900, fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(coverage.global, 1.0));
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_empty_and_degenerate,
              "wallpaper macOS: 空样本与零尺寸屏幕不除零、判不覆盖", {
    const fs::CoverageResult empty =
        fs::aggregate({}, fs::Rect{0, 0, 1600, 900}, 900, fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(empty.global, 0.0));
    DOCTEST_CHECK_FALSE(fs::covers(empty));
    // 零尺寸 visibleFrame：不除零，覆盖率 0。
    const fs::CoverageResult degenerate = fs::aggregate(
        {{1, fs::Rect{0, 0, 100, 100}}}, fs::Rect{0, 0, 0, 0}, 900,
        fs::DesktopWindowInset{});
    DOCTEST_CHECK(near(degenerate.global, 0.0));
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_inset_compensation,
              "wallpaper macOS: 桌面层窗口内缩补偿后重新等于满覆盖", {
    // 内缩补偿：桌面层窗口在 CGWindowList 里比屏幕小一圈（实测 14/9）。
    // 先外扩再与 visibleFrame 求交，结果必须重新等于满覆盖。
    fs::Rect visible{0, 0, 1470, 956};
    const fs::DesktopWindowInset inset = fs::measuredInset();
    // 桌面层窗口原样报的值 = 屏幕内缩后：x=14, y=9, w=1470-28, h=956-18
    std::vector<fs::WindowSample> samples{
        {42, fs::Rect{14, 9, 1470.0 - 28.0, 956.0 - 18.0}}};
    const fs::CoverageResult coverage =
        fs::aggregate(samples, visible, 956, inset);
    DOCTEST_CHECK(near(coverage.global, 1.0, 1e-9));
    DOCTEST_CHECK(fs::covers(coverage));
    return true;
})

REGISTER_TEST(wallpaper_macos_fullscreen_flip_top_left_to_bottom_left,
              "wallpaper macOS: 坐标系翻转只换 y 不换尺寸（写反则覆盖率恒 0）", {
    // 左上原点 → 左下原点：只换 y，不换尺寸。写反会让覆盖率恒 0。
    const fs::Rect flipped =
        fs::flipTopLeftToBottomLeft(fs::Rect{10, 20, 100, 50}, 900);
    DOCTEST_CHECK(near(flipped.x, 10.0));
    DOCTEST_CHECK(near(flipped.y, 900.0 - (20.0 + 50.0)));
    DOCTEST_CHECK(near(flipped.w, 100.0));
    DOCTEST_CHECK(near(flipped.h, 50.0));
    return true;
})

// ── 图片尺寸头解码 ──

REGISTER_TEST(wallpaper_macos_image_probe_unknown_for_missing_file,
              "wallpaper macOS: 图片探针对不存在的文件返回 known=false（不过滤）", {
    // 契约：读不出来一律 known=false（调用方语义 = 不过滤，宁滥勿缺）。
    const auto result = module::wallpaper::probeImageSize(
        "/definitely/not/a/real/image/path-xyz.png");
    DOCTEST_CHECK_FALSE(result.known);
    return true;
})

REGISTER_TEST(wallpaper_macos_image_probe_rejects_garbage,
              "wallpaper macOS: 图片探针对假 PNG 返回 known=false 且不崩", {
    // 假 PNG（只有扩展名对）→ 建不出解码器 → known=false，不崩。
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "devdesk_fake_probe_image.png";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 64; ++i) out << "not a png";
    }
    const auto result = module::wallpaper::probeImageSize(path.string());
    DOCTEST_CHECK_FALSE(result.known);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
})

REGISTER_TEST(wallpaper_macos_image_probe_empty_path,
              "wallpaper macOS: 图片探针对空路径返回 known=false", {
    const auto result = module::wallpaper::probeImageSize("");
    DOCTEST_CHECK_FALSE(result.known);
    return true;
})

// ── 花見源目录可达性（bug200 症状①）：「存在但无权限（TCC）」与「不存在」可区分 ──
// 防：旧实现两者同一降级 → 用户装了花見但没给完全磁盘访问权限时，看到的是
// 「找不到花見下载目录（花見未安装或从未下载）」的误导文案。
REGISTER_TEST(wallpaper_macos_importer_source_no_access_distinguished,
              "wallpaper macOS: 花見源目录「存在但无权限」与「不存在」可区分（TCC 场景）", {
    namespace fs = std::filesystem;
    using module::wallpaper::importer::hanamiImportAvailable;
    using module::wallpaper::importer::probeHanamiSourceDirectory;
    if (!hanamiImportAvailable()) return true;  // stub 平台无此缝（恒 missing）
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "devdesk_hanami_tcc_probe";
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    // 可读态。
    const auto readable = probeHanamiSourceDirectory(dir.string());
    DOCTEST_CHECK(readable.readable);
    DOCTEST_CHECK_FALSE(readable.noAccess);
    // 无权限态：目录存在、枚举被拒（chmod 000 模拟 TCC；root 环境拦不住枚举
    // → 该半断言条件化，不因环境假红）。
    fs::permissions(dir, fs::perms::none, ec);
    const auto denied = probeHanamiSourceDirectory(dir.string());
    if (!denied.readable) {
        DOCTEST_CHECK_MESSAGE(denied.noAccess,
                              "目录存在且枚举失败应判 noAccess（TCC）");
    }
    fs::permissions(dir, fs::perms::owner_all, ec);  // 恢复以便清理
    // 不存在态：missing，不是 noAccess。
    const auto missing = probeHanamiSourceDirectory(dir.string() + ".missing");
    DOCTEST_CHECK_FALSE(missing.readable);
    DOCTEST_CHECK_FALSE(missing.noAccess);
    fs::remove_all(dir, ec);
    return true;
})
