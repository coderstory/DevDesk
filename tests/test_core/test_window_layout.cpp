#include "doctest.h"
#include "core/window_layout.h"

#include <cmath>
#include <limits>
#include <map>
#include <string>

using core::window_layout::decideInitialWindow;
using core::window_layout::FrameInsets;
using core::window_layout::frameInsetsBaseline;
using core::window_layout::FrameInsetsBaseline;
using core::window_layout::frameInsetsForScale;
using core::window_layout::InitialWindowRequest;
using core::window_layout::minWindowSizeForWorkArea;
using core::window_layout::windowUnitName;
using core::window_layout::WorkArea;

namespace {

// 本机实测基线：1920×1080 @125%（工作区 1920×1020 物理），请求 1600×1000。
// 下面三个工厂只是命名场景的简写便利：REGISTER_TEST 早已支持用例体内的裸逗号
// （聚合初始化列表，见文件末护栏用例），不再需要工厂来规避宏切参
InitialWindowRequest request1600x1000() {
    return InitialWindowRequest{1600, 1000, 1200, 720};
}

WorkArea area(int width, int height) {
    return WorkArea{0, 0, width, height};
}

InitialWindowRequest request(int logicalWidth, int logicalHeight, int minWidth, int minHeight) {
    return InitialWindowRequest{logicalWidth, logicalHeight, minWidth, minHeight};
}

// Windows 基准（96 DPI 实测 14.4/37.6）显式注入。下面涉及收缩/钳制**具体数值**的
// 用例都是 Windows 实测档（125% → +18/+47）：必须走双参纯计算路径——单参重载查
// frameInsetsBaseline() 平台事实（macOS {0,28}），数值断言随平台翻车（CI macOS
// 首跑实证：3 例 FAIL 全是这条）。平台注入行为本身由 window_layout_frame_insets_
// platform 用例单独钉住。
FrameInsets winInsets(float scale) {
    return frameInsetsForScale(scale, FrameInsetsBaseline{14.4f, 37.6f});
}

} // namespace

REGISTER_TEST(window_layout_fits_at_scale1, "window_layout: 工作区足够 → 用请求尺寸（scale 1.0，与旧默认一致）", {
    const auto decision = decideInitialWindow(request1600x1000(), area(1920, 1080), 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);
    return true;
})

REGISTER_TEST(window_layout_scales_to_physical, "window_layout: 高 DPI 下请求尺寸换算成物理像素", {
    // 3840×2160 工作区够放 2000×1250 + 外框（2018×1297）
    const auto decision = decideInitialWindow(request1600x1000(), area(3840, 2160), 1.25f,
                                              frameInsetsForScale(1.25f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 2000);   // 1600 × 1.25
    DOCTEST_CHECK_EQ(decision.height, 1250);  // 1000 × 1.25
    return true;
})

REGISTER_TEST(window_layout_too_small_shrinks,
              "window_layout: 本机 1920×1020 @125% 放不下 → 收缩适配不最大化", {
    const auto decision = decideInitialWindow(request1600x1000(), area(1920, 1020), 1.25f,
                                              winInsets(1.25f));
    // 2000+18 = 2018 > 1920（宽超）；1250+47 = 1297 > 1020（高超）→ 两轴各自钳到
    // 「工作区 − 外框 − 呼吸边距32」：宽 1920−18−32 = 1870、高 1020−47−32 = 941。
    // UAT6 前此档走「超出即最大化」，用户反馈每次启动都全屏，不可接受
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1870);
    DOCTEST_CHECK_EQ(decision.height, 941);
    // 收缩后外框 + 呼吸边距整体放得进工作区（不再越界，也不贴满）
    DOCTEST_CHECK(decision.width + 18 + 32 <= 1920);
    DOCTEST_CHECK(decision.height + 47 + 32 <= 1020);
    // 不低于最小窗换算值（1200×720 逻辑 @1.25 = 1500×900 物理）
    DOCTEST_CHECK(decision.width >= 1500);
    DOCTEST_CHECK(decision.height >= 900);
    return true;
})

REGISTER_TEST(window_layout_single_axis_overflow_shrinks_that_axis,
              "window_layout: 单方向超出外框 → 只收缩该轴，不最大化", {
    const FrameInsets frame = frameInsetsForScale(1.0f);
    // 宽度差 1 物理 px、高度正好放下：只钳宽（钳到 工作区−外框−32），高不动
    const auto tooWide = decideInitialWindow(request1600x1000(),
                                             area(1600 + frame.width - 1, 1000 + frame.height), 1.0f, frame);
    DOCTEST_CHECK_FALSE(tooWide.maximize);
    DOCTEST_CHECK_EQ(tooWide.width, 1600 + frame.width - 1 - frame.width - 32);
    DOCTEST_CHECK_EQ(tooWide.height, 1000);
    // 高度差 1 物理 px、宽度正好放下（对称方向）：只钳高，宽不动
    const auto tooTall = decideInitialWindow(request1600x1000(),
                                             area(1600 + frame.width, 1000 + frame.height - 1), 1.0f, frame);
    DOCTEST_CHECK_FALSE(tooTall.maximize);
    DOCTEST_CHECK_EQ(tooTall.width, 1600);
    DOCTEST_CHECK_EQ(tooTall.height, 1000 + frame.height - 1 - frame.height - 32);
    return true;
})

// UAT6 用户真机档：1080p@125%（屏幕 1920×1080 物理、任务栏后工作区 1920×1032 物理）。
// 1600×1000 逻辑请求换算 2000×1250 超出 → 旧策略每次启动全屏；新策略收缩、永不默认最大化
REGISTER_TEST(window_layout_uat6_1080p_at_125_shrinks,
              "window_layout: UAT6 1080p@125%（工作区 1920×1032）→ 钳到 1870×953 不最大化", {
    const auto decision = decideInitialWindow(request1600x1000(), area(1920, 1032), 1.25f,
                                              winInsets(1.25f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    // 宽 1920−18−32 = 1870；高 1032−47−32 = 953
    DOCTEST_CHECK_EQ(decision.width, 1870);
    DOCTEST_CHECK_EQ(decision.height, 953);
    // 收缩后外框 + 呼吸边距整体放得进工作区
    DOCTEST_CHECK(decision.width + 18 + 32 <= 1920);
    DOCTEST_CHECK(decision.height + 47 + 32 <= 1032);
    return true;
})

// 100% 大屏档：请求原样落实、不最大化（防收缩策略误伤放得下的常规档）
REGISTER_TEST(window_layout_large_screen_scale1_keeps_request,
              "window_layout: 100% 大屏（2560×1440）→ 1600×1000 原样不最大化", {
    const auto decision = decideInitialWindow(request1600x1000(), area(2560, 1440), 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);
    return true;
})

// 极端小屏：钳到「工作区−外框−32」已低于最小窗换算值 → 以 minWindowSizeForWorkArea
// 同口径钳制值（= min(最小窗, 工作区−外框)，无呼吸边距）为下限，仍不最大化；
// min 无约束且工作区小于外框+呼吸边距时至少 1 单位（不给后端 0/负值）
REGISTER_TEST(window_layout_tiny_screen_min_floor_wins,
              "window_layout: 极端小屏收缩低于最小窗 → 以最小窗钳制值为准，不最大化", {
    // 900×600 工作区 @100%：fit 宽 = 900−15−32 = 853 < 最小窗 1200 → minFloor =
    // min(1200, 900−15) = 885 胜出；fit 高 = 600−38−32 = 530 < 720 → minFloor = 562
    const auto decision = decideInitialWindow(request1600x1000(), area(900, 600), 1.0f,
                                              winInsets(1.0f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 885);
    DOCTEST_CHECK_EQ(decision.height, 562);
    // min 无约束 + 工作区装不下外框+呼吸边距：钳完不为 0/负，至少 1（建窗保护）
    const auto degenerate = decideInitialWindow(request(1600, 1000, 0, 0), area(40, 80), 1.0f,
                                                winInsets(1.0f));
    DOCTEST_CHECK_FALSE(degenerate.maximize);
    DOCTEST_CHECK(degenerate.width >= 1);
    DOCTEST_CHECK(degenerate.height >= 1);
    return true;
})

REGISTER_TEST(window_layout_exact_fit_keeps_size, "window_layout: 外框正好等于工作区 → 不最大化", {
    const FrameInsets frame = frameInsetsForScale(1.0f);
    const auto decision = decideInitialWindow(request1600x1000(),
                                              area(1600 + frame.width, 1000 + frame.height), 1.0f, frame);
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);
    return true;
})

REGISTER_TEST(window_layout_unknown_work_area, "window_layout: 工作区未知 → 保留请求尺寸不最大化", {
    const auto decision = decideInitialWindow(request1600x1000(), area(0, 0), 1.25f,
                                              frameInsetsForScale(1.25f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 2000);
    DOCTEST_CHECK_EQ(decision.height, 1250);

    // 只有高度未知（宽有效）走同一降级口径
    const auto decision2 = decideInitialWindow(request1600x1000(), area(1920, 0), 1.0f,
                                               frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(decision2.maximize);
    DOCTEST_CHECK_EQ(decision2.width, 1600);
    return true;
})

// 请求尺寸低于最小值时，决策不小于最小值；min 传 0 表示该方向不约束
REGISTER_TEST(window_layout_min_size_floor, "window_layout: 决策不小于最小尺寸语义（1200×720）", {
    const auto decision = decideInitialWindow(request(800, 600, 1200, 720), area(1920, 1020), 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_EQ(decision.width, 1200);
    DOCTEST_CHECK_EQ(decision.height, 720);

    // min = 0 不约束，但仍不低于 1 像素：后端拿 0 建窗会出问题
    const auto decision2 = decideInitialWindow(request(0, 0, 0, 0), area(1920, 1020), 1.0f,
                                               frameInsetsForScale(1.0f));
    DOCTEST_CHECK_EQ(decision2.width, 1);
    DOCTEST_CHECK_EQ(decision2.height, 1);
    return true;
})

// 缩放系数非法 → 按 1.0 降级，与 windowScaleForPoint 查询失败返回 1.0f 同口径
REGISTER_TEST(window_layout_invalid_scale, "window_layout: 非法缩放系数降级为 1.0", {
    for (float bad : {0.0f, -1.25f, std::numeric_limits<float>::quiet_NaN()}) {
        const auto decision = decideInitialWindow(request1600x1000(), area(1920, 1080), bad,
                                                  frameInsetsForScale(bad));
        DOCTEST_CHECK_FALSE(decision.maximize);
        DOCTEST_CHECK_EQ(decision.width, 1600);
        DOCTEST_CHECK_EQ(decision.height, 1000);
    }
    return true;
})

// 外框经验值自检：125% 下必须还原 Windows 实测的 +18 / +47。本用例显式传 Windows 基准走
// 纯计算档，好让两个平台跑同一组期望值；平台注入档见下方
// window_layout_frame_insets_platform
REGISTER_TEST(window_layout_frame_insets_baseline, "window_layout: 外框经验值在 125% 还原实测 +18/+47", {
    constexpr FrameInsetsBaseline kWindowsBaseline{14.4f, 37.6f};
    const FrameInsets at125 = frameInsetsForScale(1.25f, kWindowsBaseline);
    DOCTEST_CHECK_EQ(at125.width, 18);
    DOCTEST_CHECK_EQ(at125.height, 47);
    // 96 DPI 基准向上取整（14.4→15 / 37.6→38），宁可高估外框
    const FrameInsets at100 = frameInsetsForScale(1.0f, kWindowsBaseline);
    DOCTEST_CHECK_EQ(at100.width, 15);
    DOCTEST_CHECK_EQ(at100.height, 38);
    // 非法缩放同降级口径
    const FrameInsets fallback = frameInsetsForScale(-3.0f, kWindowsBaseline);
    DOCTEST_CHECK_EQ(fallback.width, 15);
    DOCTEST_CHECK_EQ(fallback.height, 38);
    return true;
})

namespace {

// 本平台期望的外框基准 / 单位名。平台分支写在**函数体**里，不写进 REGISTER_TEST 的
// 宏实参——宏实参内嵌 `#if` 会被 clang 判 -Wembedded-directive，本仓库 -Werror 拦停
FrameInsetsBaseline expectedPlatformBaseline() {
#if defined(__APPLE__)
    return FrameInsetsBaseline{0.0f, 28.0f};   //只有标题栏（实测 833−805）
#elif defined(_WIN32)
    return FrameInsetsBaseline{14.4f, 37.6f};  //125% 实测 18/47 ÷ 1.25，96 DPI 基准
#else
    return FrameInsetsBaseline{0.0f, 28.0f};   //posix：与 macOS 同口径（见 _posix.cpp）
#endif
}

bool platformUnitNameOk() {
    const std::string unit = windowUnitName();
#if defined(__APPLE__)
    return unit == "pt";
#elif defined(_WIN32)
    return unit == "物理px";
#else
    return !unit.empty();
#endif
}

}  // namespace

REGISTER_TEST(window_layout_frame_insets_platform, "window_layout: 平台外框基准（macOS {0,28} / Windows {14.4,37.6}）", {
    const FrameInsetsBaseline expect = expectedPlatformBaseline();
    const FrameInsetsBaseline base = frameInsetsBaseline();
    DOCTEST_CHECK_EQ(base.width, expect.width);
    DOCTEST_CHECK_EQ(base.height, expect.height);
    // macOS 上 windowScaleForPoint 恒 1.0，所以 1.0 档换算结果就是基准向上取整
    const FrameInsets at1 = frameInsetsForScale(1.0f);
    DOCTEST_CHECK_EQ(at1.width, static_cast<int>(std::ceil(expect.width)));
    DOCTEST_CHECK_EQ(at1.height, static_cast<int>(std::ceil(expect.height)));
    // 注入点确实被生产路径消费（不是注入了却仍走老常量）：单参重载 == 显式传本平台基准
    const FrameInsets viaPlatform = frameInsetsForScale(1.25f);
    const FrameInsets viaExplicit = frameInsetsForScale(1.25f, frameInsetsBaseline());
    DOCTEST_CHECK_EQ(viaPlatform.width, viaExplicit.width);
    DOCTEST_CHECK_EQ(viaPlatform.height, viaExplicit.height);
    return true;
})

REGISTER_TEST(window_layout_unit_name, "window_layout: 平台窗口单位名（macOS pt / Windows 物理px）", {
    // 窗口单位名要如实反映平台，否则日志会把 macOS 的点标成物理px
    DOCTEST_CHECK(platformUnitNameOk());
    return true;
})

// macOS 档（本机实测：工作区 1470×833 点、windowScaleForPoint 恒 1.0、外框 {0,28}）：
// 设计最小窗 1000×640（UAT3 D-R3-2 从 1280×800 收窄）——640 ≤ 工作区可用 805，本机档
// **不钳**（旧 845 > 805 曾是钳制档；钳制行为本身由 below_work_area_paths ① 用例覆盖，
// 此处钉「设计值在真实工作区原样落实」）
REGISTER_TEST(window_layout_min_window_macos_work_area,
              "window_layout: macOS 档（工作区 1470×833 点）设计最小窗 640 ≤ 可用 805 不钳", {
    const auto min = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 1000, 640}, WorkArea{0, 0, 1470, 833}, 1.0f,
        FrameInsets{0, 28});
    DOCTEST_CHECK(min.height <= 805);  //≤ 工作区高 − 标题栏(28)
    DOCTEST_CHECK_EQ(min.height, 640);          //设计 640 ≤ 833 − 28 = 805，不钳
    DOCTEST_CHECK_EQ(min.width, 1000);          //宽不钳：1000 ≤ 1470 − 0
    return true;
})

// Windows 档（本机实测：工作区 1920×1020 物理 @125%、外框 {18,47}）：
// UAT3 D-R3-2 后设计最小窗 1000×640 → 换算 1250×800 物理，高 800 ≤ 可用 973
//（1020 − 47）**不再钳**（旧 1280×800 时代 800 逻辑 = 1000 物理 > 973 被钳 973 =
// 778 逻辑——钳制路径改由 below_work_area_paths ① 覆盖）。有效下限 = 设计下限
REGISTER_TEST(window_layout_min_window_windows_work_area,
              "window_layout: Windows 档（1920×1020 物理 @1.25）设计最小窗 1250×800 物理不钳", {
    constexpr FrameInsetsBaseline kWindowsBaseline{14.4f, 37.6f};
    const FrameInsets frame125 = frameInsetsForScale(1.25f, kWindowsBaseline);
    const auto min = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 1000, 640}, WorkArea{0, 0, 1920, 1020}, 1.25f,
        frame125);
    DOCTEST_CHECK_EQ(min.height, 800);   //640×1.25 = 800 ≤ 1020 − 47 = 973，不钳
    DOCTEST_CHECK_EQ(min.width, 1250);   //1000×1.25 = 1250；1250+18 < 1920 不钳
    // 不低于设计下限（物理口径），且外框仍装得进工作区
    DOCTEST_CHECK(min.height >= 800);
    DOCTEST_CHECK(min.height + frame125.height <= 1020);
    DOCTEST_CHECK(min.width + frame125.width <= 1920);
    return true;
})

REGISTER_TEST(window_layout_min_window_below_work_area_paths,
              "window_layout: 工作区小于最小值时的钳制/降级路径", {
    const auto req = InitialWindowRequest{1600, 1000, 1280, 800};

    // ① 工作区比设计下限小 → 按「工作区 − 外框」钳
    const auto small = minWindowSizeForWorkArea(req, WorkArea{0, 0, 1024, 600}, 1.0f,
                                                FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(small.width, 1024);  //1024 − 0
    DOCTEST_CHECK_EQ(small.height, 572);  //600 − 28

    // ② 工作区信息不可用（无头 / 查询失败）→ 不钳制，取换算值。与 decideInitialWindow
    //   同口径：信息不足时不擅自改窗口形态
    const auto unknown = minWindowSizeForWorkArea(req, WorkArea{0, 0, 0, 0}, 1.0f,
                                                 FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(unknown.width, 1280);
    DOCTEST_CHECK_EQ(unknown.height, 800);

    // ③ 工作区比外框还小 → 该方向算出 0，负值不传给后端。注意 0 在 GLFW 两个后端都是
    //   all-or-nothing：任一轴为 0 会让两轴的最小尺寸一起失效（见 window_layout.h
    //   规则 5），所以另一轴算好的值也一并放开。下面 width==800 钉住的就是这个事实，
    //   防止按「0 = 单轴不约束」改回去
    const auto tiny = minWindowSizeForWorkArea(req, WorkArea{0, 0, 800, 20}, 1.0f,
                                              FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(tiny.height, 0);
    DOCTEST_CHECK_EQ(tiny.width, 800);  //该轴本身仍算得出来（真实约束由后端放开）

    // ④ min 传 0 = 该方向不约束：保持 0 透传，不因钳制变成伪下限
    const auto unconstrained = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 0, 0}, WorkArea{0, 0, 1920, 1020}, 1.0f,
        FrameInsets{18, 47});
    DOCTEST_CHECK_EQ(unconstrained.width, 0);
    DOCTEST_CHECK_EQ(unconstrained.height, 0);

    // ⑤ 非法缩放 → 按 1.0 换算，与 decideInitialWindow 同降级口径
    const auto badScale = minWindowSizeForWorkArea(req, WorkArea{0, 0, 1470, 833}, 0.0f,
                                                  FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(badScale.width, 1280);
    DOCTEST_CHECK_EQ(badScale.height, 800);

    // ⑥ 换算顺序不可换：必须先乘 scale 再与工作区取 min。先钳后乘会得
    //    min(800, 973)×1.25 = 1000 物理，仍 > 可用 973 收不进屏，等于没钳
    constexpr FrameInsetsBaseline kWindowsBaseline{14.4f, 37.6f};
    const auto order = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 1280, 800}, WorkArea{0, 0, 1920, 1020}, 1.25f,
        frameInsetsForScale(1.25f, kWindowsBaseline));
    DOCTEST_CHECK_EQ(order.height, 973);
    DOCTEST_CHECK(order.height + 47 <= 1020);
    return true;
})

// 回归护栏：REGISTER_TEST 用可变参宏（`...` / `__VA_ARGS__`）承接 body，用例体内
// 的**顶层裸逗号**才不会被预处理器当成宏实参分隔符。下面四类过去必须规避，现在可直接
// 写在用例体内；本用例一旦因宏回退成 3 参数而编译不过，即护栏生效
REGISTER_TEST(window_layout_tin08_bare_comma_in_body,
              "window_layout: REGISTER_TEST 用例体内裸逗号（初始化列表/模板逗号）直接可用", {
    // ① 聚合初始化列表：顶层裸逗号
    const InitialWindowRequest req{1600, 1000, 1200, 720};
    DOCTEST_CHECK_EQ(req.minLogicalWidth, 1200);
    DOCTEST_CHECK_EQ(req.minLogicalHeight, 720);

    // ② 花括号直接写在函数实参位置
    const auto decision = decideInitialWindow(InitialWindowRequest{1600, 1000, 1200, 720},
                                              WorkArea{0, 0, 1920, 1080}, 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);

    // ③ 模板实参里的逗号：std::map<std::string, int> 式
    const std::map<std::string, int> sizes{{"w", decision.width}, {"h", decision.height}};
    DOCTEST_CHECK_EQ(sizes.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(sizes.at("w"), 1600);
    DOCTEST_CHECK_EQ(sizes.at("h"), 1000);

    // ④ 一条语句两个声明：声明列表里的逗号
    const int expectedW = 1600, expectedH = 1000;
    DOCTEST_CHECK_EQ(expectedW, decision.width);
    DOCTEST_CHECK_EQ(expectedH, decision.height);
    return true;
})

// ==================== 窗体记忆恢复（UAT6：restoreInitialWindow）====================
// 记忆尺寸必须走 decideInitialWindow 同一条收缩钳制链：换屏后巨屏记忆值自适应收缩、
// 过小记忆值提到最小窗下限、无记录回退默认；最大化标志独立透传（不参与尺寸链）。

namespace {

// main.cpp 生产口径的默认请求：默认 1600×1000 + 最小窗 1000×640（逻辑）
core::window_layout::InitialWindowDefaults mainDefaults() {
    return core::window_layout::InitialWindowDefaults{1600, 1000, 1000, 640};
}

}  // namespace

REGISTER_TEST(window_restore_no_record_uses_default,
              "window_restore: 无记录（0×0）→ 回退默认 1600×1000，不最大化", {
    const auto plan = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{0, 0, false}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(plan.fromSavedRecord);
    DOCTEST_CHECK_FALSE(plan.initial.maximize);
    DOCTEST_CHECK_EQ(plan.initial.width, 1600);
    DOCTEST_CHECK_EQ(plan.initial.height, 1000);
    // 实际采用的请求 = 默认口径（minWindowSizeForWorkArea 须拿它同口径换算）
    DOCTEST_CHECK_EQ(plan.requestUsed.logicalWidth, 1600);
    DOCTEST_CHECK_EQ(plan.requestUsed.logicalHeight, 1000);
    DOCTEST_CHECK_EQ(plan.requestUsed.minLogicalWidth, 1000);
    DOCTEST_CHECK_EQ(plan.requestUsed.minLogicalHeight, 640);
    return true;
})

REGISTER_TEST(window_restore_saved_size_kept,
              "window_restore: 有记忆（1280×800）→ 原样恢复，requestUsed 同口径", {
    const auto plan = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{1280, 800, false}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK(plan.fromSavedRecord);
    DOCTEST_CHECK_FALSE(plan.initial.maximize);
    DOCTEST_CHECK_EQ(plan.initial.width, 1280);
    DOCTEST_CHECK_EQ(plan.initial.height, 800);
    DOCTEST_CHECK_EQ(plan.requestUsed.logicalWidth, 1280);
    DOCTEST_CHECK_EQ(plan.requestUsed.logicalHeight, 800);
    // 最小窗换算与「直接拿记忆请求建 InitialWindowRequest」逐字段一致（main.cpp
    // 用 plan.requestUsed 喂 minWindowSizeForWorkArea，此断言防另建请求分叉）
    const auto minViaPlan = minWindowSizeForWorkArea(
        plan.requestUsed, area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    const auto minDirect = minWindowSizeForWorkArea(
        InitialWindowRequest{1280, 800, 1000, 640}, area(1920, 1080), 1.0f,
        frameInsetsForScale(1.0f));
    DOCTEST_CHECK_EQ(minViaPlan.width, minDirect.width);
    DOCTEST_CHECK_EQ(minViaPlan.height, minDirect.height);
    return true;
})

REGISTER_TEST(window_restore_oversized_clamps_via_same_chain,
              "window_restore: 巨屏记忆值（4000×3000 逻辑）→ 走同一条收缩链钳回工作区", {
    // UAT6 用户真机档：1080p@125%（工作区 1920×1032 物理、外框 +18/+47）
    const auto plan = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{4000, 3000, false}, mainDefaults(),
        area(1920, 1032), 1.25f, winInsets(1.25f));
    // 与直接拿同参数请求走 decideInitialWindow 逐字段相等（证明记忆值没有旁路钳制链）
    const auto direct = decideInitialWindow(
        InitialWindowRequest{4000, 3000, 1000, 640}, area(1920, 1032), 1.25f,
        winInsets(1.25f));
    DOCTEST_CHECK_EQ(plan.initial.width, direct.width);
    DOCTEST_CHECK_EQ(plan.initial.height, direct.height);
    DOCTEST_CHECK_EQ(plan.initial.width, 1870);   // 1920−18−32
    DOCTEST_CHECK_EQ(plan.initial.height, 953);   // 1032−47−32
    DOCTEST_CHECK_FALSE(plan.initial.maximize);
    DOCTEST_CHECK(plan.fromSavedRecord);
    return true;
})

REGISTER_TEST(window_restore_tiny_raises_to_min,
              "window_restore: 过小记忆值（300×200）→ 提到最小窗下限 1000×640", {
    const auto plan = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{300, 200, false}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK(plan.fromSavedRecord);
    DOCTEST_CHECK_EQ(plan.initial.width, 1000);
    DOCTEST_CHECK_EQ(plan.initial.height, 640);
    DOCTEST_CHECK_FALSE(plan.initial.maximize);
    return true;
})

REGISTER_TEST(window_restore_maximized_flag_passthrough,
              "window_restore: 最大化标志往返（有尺寸/无尺寸都透传，false 不误开）", {
    // 有尺寸记录 + 最大化 → 恢复最大化，windowSize 即还原态尺寸（退出最大化回到它）
    const auto maxed = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{1280, 800, true}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK(maxed.initial.maximize);
    DOCTEST_CHECK_EQ(maxed.initial.width, 1280);
    DOCTEST_CHECK_EQ(maxed.initial.height, 800);
    // 尺寸记录缺失但标志为 true：仍恢复最大化（还原态尺寸回退默认，不记全屏值）
    const auto maxedNoSize = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{0, 0, true}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK(maxedNoSize.initial.maximize);
    DOCTEST_CHECK_EQ(maxedNoSize.initial.width, 1600);
    // 正常关闭 → 不最大化（防标志误置位回归）
    const auto normal = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{1280, 800, false}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(normal.initial.maximize);
    return true;
})

REGISTER_TEST(window_restore_dirty_record_falls_back,
              "window_restore: 脏记录（负值/半记录）→ 当无记录处理，不产出负/零尺寸", {
    // 负值（SettingsStore 已收敛 0，这里防绕过 store 直传脏值的调用方）
    const auto negative = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{-500, -300, false}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(negative.fromSavedRecord);
    DOCTEST_CHECK_EQ(negative.initial.width, 1600);
    // 半记录（宽有效、高 0）：任一轴非正 = 无记录，整条回默认（不产 0 高窗口）
    const auto half = core::window_layout::restoreInitialWindow(
        core::window_layout::SavedWindowGeometry{1280, 0, false}, mainDefaults(),
        area(1920, 1080), 1.0f, frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(half.fromSavedRecord);
    DOCTEST_CHECK_EQ(half.initial.width, 1600);
    DOCTEST_CHECK_EQ(half.initial.height, 1000);
    return true;
})
