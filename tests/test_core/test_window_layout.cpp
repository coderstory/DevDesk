#include "doctest.h"
#include "core/window_layout.h"

#include <limits>
#include <map>
#include <string>

using core::window_layout::decideInitialWindow;
using core::window_layout::FrameInsets;
using core::window_layout::frameInsetsForScale;
using core::window_layout::InitialWindowRequest;
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

// 外框经验值自检：125% 下必须还原本机实测的 +18 / +47（缺陷数据基线）
REGISTER_TEST(window_layout_frame_insets_baseline, "window_layout: 外框经验值在 125% 还原实测 +18/+47", {
    const FrameInsets at125 = frameInsetsForScale(1.25f);
    DOCTEST_CHECK_EQ(at125.width, 18);
    DOCTEST_CHECK_EQ(at125.height, 47);
    // 96 DPI 基准向上取整（14.4→15 / 37.6→38，宁可高估外框）
    const FrameInsets at100 = frameInsetsForScale(1.0f);
    DOCTEST_CHECK_EQ(at100.width, 15);
    DOCTEST_CHECK_EQ(at100.height, 38);
    // 非法缩放同降级口径
    const FrameInsets fallback = frameInsetsForScale(-3.0f);
    DOCTEST_CHECK_EQ(fallback.width, 15);
    DOCTEST_CHECK_EQ(fallback.height, 38);
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
