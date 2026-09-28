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
// 下面三个工厂是命名场景 / 简写便利：TIN-08 后 REGISTER_TEST 已支持用例体内的
// 裸逗号（聚合初始化列表，见文件末护栏用例），不再需要工厂规避宏切参。
InitialWindowRequest request1600x1000() {
    return InitialWindowRequest{1600, 1000, 1200, 720};
}

WorkArea area(int width, int height) {
    return WorkArea{0, 0, width, height};
}

InitialWindowRequest request(int logicalWidth, int logicalHeight, int minWidth, int minHeight) {
    return InitialWindowRequest{logicalWidth, logicalHeight, minWidth, minHeight};
}

} // namespace

// 放得下 → 用请求尺寸：缩放 1.0（= 旧默认口径，逻辑单位 == 物理像素）
REGISTER_TEST(window_layout_fits_at_scale1, "window_layout: 工作区足够 → 用请求尺寸（scale 1.0，与旧默认一致）", {
    const auto decision = decideInitialWindow(request1600x1000(), area(1920, 1080), 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);
    return true;
})

// 放得下 → 请求尺寸按缩放系数换算成物理像素（高 DPI 大屏）
REGISTER_TEST(window_layout_scales_to_physical, "window_layout: 高 DPI 下请求尺寸换算成物理像素", {
    // 3840×2160 工作区够放 2000×1250 + 外框（2018×1297）
    const auto decision = decideInitialWindow(request1600x1000(), area(3840, 2160), 1.25f,
                                              frameInsetsForScale(1.25f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 2000);   // 1600 × 1.25
    DOCTEST_CHECK_EQ(decision.height, 1250);  // 1000 × 1.25
    return true;
})

// 本机实测场景（本次缺陷）→ 启动即最大化
REGISTER_TEST(window_layout_too_small_maximizes, "window_layout: 本机 1920×1020 @125% 放不下 → 最大化", {
    const auto decision = decideInitialWindow(request1600x1000(), area(1920, 1020), 1.25f,
                                              frameInsetsForScale(1.25f));
    // 2000+18 = 2018 > 1920（宽超）；1250+47 = 1297 > 1020（高超）
    DOCTEST_CHECK(decision.maximize);
    // 尺寸字段仍留换算结果（供日志诊断；最大化时后端忽略）
    DOCTEST_CHECK_EQ(decision.width, 2000);
    DOCTEST_CHECK_EQ(decision.height, 1250);
    return true;
})

// 只差一个方向也判放不下
REGISTER_TEST(window_layout_width_only_overflow, "window_layout: 单方向超出外框 → 最大化", {
    const FrameInsets frame = frameInsetsForScale(1.0f);
    // 宽度差 1 物理 px、高度正好放下
    const auto tooWide = decideInitialWindow(request1600x1000(),
                                             area(1600 + frame.width - 1, 1000 + frame.height), 1.0f, frame);
    DOCTEST_CHECK(tooWide.maximize);
    // 高度差 1 物理 px、宽度正好放下（对称方向）
    const auto tooTall = decideInitialWindow(request1600x1000(),
                                             area(1600 + frame.width, 1000 + frame.height - 1), 1.0f, frame);
    DOCTEST_CHECK(tooTall.maximize);
    return true;
})

// 边界：外框正好等于工作区 → 放得下，不最大化
REGISTER_TEST(window_layout_exact_fit_keeps_size, "window_layout: 外框正好等于工作区 → 不最大化", {
    const FrameInsets frame = frameInsetsForScale(1.0f);
    const auto decision = decideInitialWindow(request1600x1000(),
                                              area(1600 + frame.width, 1000 + frame.height), 1.0f, frame);
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);
    return true;
})

// 工作区信息不可用（无头 / 查询失败）→ 原样用请求尺寸，不擅自最大化
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

// 最小尺寸语义：请求低于 1200×720 时决策不小于它（min 传 0 = 该方向不约束）
REGISTER_TEST(window_layout_min_size_floor, "window_layout: 决策不小于最小尺寸语义（1200×720）", {
    const auto decision = decideInitialWindow(request(800, 600, 1200, 720), area(1920, 1020), 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_EQ(decision.width, 1200);
    DOCTEST_CHECK_EQ(decision.height, 720);

    // min = 0 → 不约束；仍不低于 1 像素（防后端拿 0 建窗）
    const auto decision2 = decideInitialWindow(request(0, 0, 0, 0), area(1920, 1020), 1.0f,
                                               frameInsetsForScale(1.0f));
    DOCTEST_CHECK_EQ(decision2.width, 1);
    DOCTEST_CHECK_EQ(decision2.height, 1);
    return true;
})

// 缩放系数非法 → 按 1.0 降级（与 windowScaleForPoint 查询失败返回 1.0f 同口径）
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

// 外框经验值自检：125% 下必须还原本机 Windows 实测的 +18 / +47（缺陷数据基线）。
// 2026-09-28 组 A：基准值改为平台注入（frameInsetsForScale(scale) 用本平台基准），
// 本用例改成**显式传 Windows 基准**的纯计算档——断言与数值逐字不变，两个平台
// 上都跑同一组期望值（平台注入档见下方 window_layout_frame_insets_platform）
REGISTER_TEST(window_layout_frame_insets_baseline, "window_layout: 外框经验值在 125% 还原实测 +18/+47", {
    constexpr FrameInsetsBaseline kWindowsBaseline{14.4f, 37.6f};
    const FrameInsets at125 = frameInsetsForScale(1.25f, kWindowsBaseline);
    DOCTEST_CHECK_EQ(at125.width, 18);
    DOCTEST_CHECK_EQ(at125.height, 47);
    // 96 DPI 基准向上取整（14.4→15 / 37.6→38，宁可高估外框）
    const FrameInsets at100 = frameInsetsForScale(1.0f, kWindowsBaseline);
    DOCTEST_CHECK_EQ(at100.width, 15);
    DOCTEST_CHECK_EQ(at100.height, 38);
    // 非法缩放同降级口径
    const FrameInsets fallback = frameInsetsForScale(-3.0f, kWindowsBaseline);
    DOCTEST_CHECK_EQ(fallback.width, 15);
    DOCTEST_CHECK_EQ(fallback.height, 38);
    return true;
})

// ============ 2026-09-28 组 A：平台外框注入 + 最小窗换算/钳制 ============

namespace {

// 本平台期望的外框基准 / 单位名（平台分支写在**函数体**里，不写进 REGISTER_TEST
// 的宏实参——宏实参内嵌 `#if` 会被 clang 判 -Wembedded-directive + -Werror 编译失败）
FrameInsetsBaseline expectedPlatformBaseline() {
#if defined(__APPLE__)
    return FrameInsetsBaseline{0.0f, 28.0f};   // 只有标题栏（实测 833−805）
#elif defined(_WIN32)
    return FrameInsetsBaseline{14.4f, 37.6f};  // 125% 实测 18/47 ÷ 1.25，96 DPI 基准
#else
    return FrameInsetsBaseline{0.0f, 28.0f};   // posix：与 macOS 同口径（见 _posix.cpp）
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

// 平台基准注入：macOS 只有标题栏（0/28），Windows 保持 96 DPI 基准（14.4/37.6）
REGISTER_TEST(window_layout_frame_insets_platform, "window_layout: 平台外框基准（macOS {0,28} / Windows {14.4,37.6}）", {
    const FrameInsetsBaseline expect = expectedPlatformBaseline();
    const FrameInsetsBaseline base = frameInsetsBaseline();
    DOCTEST_CHECK_EQ(base.width, expect.width);
    DOCTEST_CHECK_EQ(base.height, expect.height);
    // macOS 上 windowScaleForPoint 恒 1.0 → 1.0 档换算即基准向上取整
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
    // 别再让日志把 macOS 的点标成物理px（2026-09-28 审计的病因之一）
    DOCTEST_CHECK(platformUnitNameOk());
    return true;
})

// macOS 档（本机实测：工作区 1470×833 点、windowScaleForPoint 恒 1.0、外框 {0,28}）：
// 设计下限 845 点 > 工作区可用 805 → 必须被钳到 805，否则窗口结构性收不进屏
REGISTER_TEST(window_layout_min_window_macos_work_area,
              "window_layout: macOS 档（工作区 1470×833 点）最小窗高被钳到 ≤805", {
    const auto min = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 1330, 845}, WorkArea{0, 0, 1470, 833}, 1.0f,
        FrameInsets{0, 28});
    DOCTEST_CHECK(min.height <= 805);  // verify 判据：≤ 工作区高 − 标题栏(28)
    DOCTEST_CHECK_EQ(min.height, 805);          // 833 − 28
    DOCTEST_CHECK_EQ(min.width, 1330);          // 宽不钳：1330 ≤ 1470 − 0
    return true;
})

// Windows 档（本机实测：工作区 1920×1020 物理 @125%、外框 {18,47}）：
// 换算后 845 逻辑 = 1056 物理 > 工作区 1020 → 按工作区钳到 973。
// 这不是"比改动前更差"：改动前是 **845 物理（= 676 逻辑）**，低于 845 的设计意图；
// 钳后 973 物理 = 778 逻辑，且 973 + 47 = 1020 == 工作区（最小窗外框仍装得进屏）
REGISTER_TEST(window_layout_min_window_windows_work_area,
              "window_layout: Windows 档（1920×1020 物理 @1.25）换算+按工作区钳，与改动前对照不回归", {
    constexpr FrameInsetsBaseline kWindowsBaseline{14.4f, 37.6f};
    const FrameInsets frame125 = frameInsetsForScale(1.25f, kWindowsBaseline);
    const auto min = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 1330, 845}, WorkArea{0, 0, 1920, 1020}, 1.25f,
        frame125);
    DOCTEST_CHECK_EQ(min.height, 973);   // min(845×1.25 = 1056, 1020 − 47 = 973)
    DOCTEST_CHECK_EQ(min.width, 1663);   // 1330×1.25 = 1662.5 → 1663；1663+18 < 1920 不钳
    // 与改动前的物理值（原值直传 = 845 物理）对照：钳后更大且外框仍装得进工作区
    DOCTEST_CHECK(min.height > 845);
    DOCTEST_CHECK(min.height + frame125.height <= 1020);
    DOCTEST_CHECK(min.width + frame125.width <= 1920);
    return true;
})

// 「工作区 < 最小值」的各条降级路径（新增用例覆盖的就是这条分支）
REGISTER_TEST(window_layout_min_window_below_work_area_paths,
              "window_layout: 工作区小于最小值时的钳制/降级路径", {
    const auto req = InitialWindowRequest{1600, 1000, 1330, 845};

    // ① 工作区比设计下限小 → 按「工作区 − 外框」钳
    const auto small = minWindowSizeForWorkArea(req, WorkArea{0, 0, 1024, 600}, 1.0f,
                                                FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(small.width, 1024);  // 1024 − 0
    DOCTEST_CHECK_EQ(small.height, 572);  // 600 − 28

    // ② 工作区信息不可用（无头 / 查询失败）→ 不钳制，取换算值
    //   （与 decideInitialWindow 第 3 条同口径：信息不足时不擅自改窗口形态）
    const auto unknown = minWindowSizeForWorkArea(req, WorkArea{0, 0, 0, 0}, 1.0f,
                                                 FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(unknown.width, 1330);
    DOCTEST_CHECK_EQ(unknown.height, 845);

    // ③ 工作区比外框还小 → 该方向 0 = 不约束（负值不传给后端）。
    //   注意 0 在 GLFW 两后端都是 all-or-nothing（任一轴 0 → 两轴最小尺寸一起
    //   失效，见 window_layout.h 规则 5），所以另一轴算好的值也一并放开——
    //   下面这条 width==800 把这个事实钉住，防止后来者按"0 = 单轴不约束"改回去
    const auto tiny = minWindowSizeForWorkArea(req, WorkArea{0, 0, 800, 20}, 1.0f,
                                              FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(tiny.height, 0);
    DOCTEST_CHECK_EQ(tiny.width, 800);  // 该轴本身仍算得出来（真实约束由后端放开）

    // ④ min 传 0 = 该方向不约束：保持 0 透传，不因钳制变成伪下限
    const auto unconstrained = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 0, 0}, WorkArea{0, 0, 1920, 1020}, 1.0f,
        FrameInsets{18, 47});
    DOCTEST_CHECK_EQ(unconstrained.width, 0);
    DOCTEST_CHECK_EQ(unconstrained.height, 0);

    // ⑤ 非法缩放 → 按 1.0 换算（与 decideInitialWindow 同降级口径）
    const auto badScale = minWindowSizeForWorkArea(req, WorkArea{0, 0, 1470, 833}, 0.0f,
                                                  FrameInsets{0, 28});
    DOCTEST_CHECK_EQ(badScale.width, 1330);
    DOCTEST_CHECK_EQ(badScale.height, 805);

    // ⑥ 换算顺序：先乘 scale 再与工作区取 min（Windows 档下若先钳后乘会得
    //    min(845, 973)×1.25 = 1056 物理 → 仍收不进屏，等于没钳）
    constexpr FrameInsetsBaseline kWindowsBaseline{14.4f, 37.6f};
    const auto order = minWindowSizeForWorkArea(
        InitialWindowRequest{1600, 1000, 1330, 845}, WorkArea{0, 0, 1920, 1020}, 1.25f,
        frameInsetsForScale(1.25f, kWindowsBaseline));
    DOCTEST_CHECK_EQ(order.height, 973);
    DOCTEST_CHECK(order.height + 47 <= 1020);
    return true;
})

// ==================== TIN-08 回归护栏 ====================
// REGISTER_TEST 改用可变参宏（`...` / `__VA_ARGS__`）承接 body 后，用例体内的
// **顶层裸逗号**不再被预处理器当成宏实参分隔符。下面四类过去必须规避
//（工厂函数 / 括号包裹 / 逐条 push_back / 类型别名），现在可直接写在用例体内；
// 本用例一旦因宏回退成 3 参数而编译不过，即护栏生效。
REGISTER_TEST(window_layout_tin08_bare_comma_in_body,
              "window_layout: [TIN-08] REGISTER_TEST 用例体内裸逗号（初始化列表/模板逗号）直接可用", {
    // ① 聚合初始化列表：顶层裸逗号（旧写法需经 request1600x1000() 工厂函数绕开）
    const InitialWindowRequest req{1600, 1000, 1200, 720};
    DOCTEST_CHECK_EQ(req.minLogicalWidth, 1200);
    DOCTEST_CHECK_EQ(req.minLogicalHeight, 720);

    // ② 同一处再套一层：花括号直接写在函数实参位置（旧写法需先建变量）
    const auto decision = decideInitialWindow(InitialWindowRequest{1600, 1000, 1200, 720},
                                              WorkArea{0, 0, 1920, 1080}, 1.0f,
                                              frameInsetsForScale(1.0f));
    DOCTEST_CHECK_FALSE(decision.maximize);
    DOCTEST_CHECK_EQ(decision.width, 1600);
    DOCTEST_CHECK_EQ(decision.height, 1000);

    // ③ 模板实参里的逗号：std::map<std::string, int> 式（旧写法需类型别名）
    const std::map<std::string, int> sizes{{"w", decision.width}, {"h", decision.height}};
    DOCTEST_CHECK_EQ(sizes.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(sizes.at("w"), 1600);
    DOCTEST_CHECK_EQ(sizes.at("h"), 1000);

    // ④ 一条语句两个声明：声明列表里的逗号（旧写法需拆成两句）
    const int expectedW = 1600, expectedH = 1000;
    DOCTEST_CHECK_EQ(expectedW, decision.width);
    DOCTEST_CHECK_EQ(expectedH, decision.height);
    return true;
})
