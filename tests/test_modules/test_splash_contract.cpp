// tests/test_modules/test_splash_contract.cpp
// splash 源码契约测试（W2a，D-11 反转后新增）。
//
// 防的事故：splash 回归极光波 shadertoy 渐变（§8 禁项 #4：大面积渐变背景）；
// 以及 splash 重新引入硬编码色字面量（W0.5 清单 W-W1~W-W3 + #15-#17）。
//
// UAT6 动态印染新增两层：①曲线行为断言——墨染时长/前沿半径/浓度包络是 splash.h
// 内联纯函数，无头直调钉曲线（时长档、单调性、终态收敛）；②接线源码断言——
// splash.cpp 只许经 inkProgress 驱动墨染、挂 md.tree.anim.driver 同款 onFrame
// 帧泵、洗染色走 wechat 令牌（禁 color(wc.primary) 回退由既有负向契约钉）。
//
// 为什么读源文件而不是跑渲染：splash 需 GL 上下文 + 2.3s 时序，无头单测活不过；
// test_main.cpp 的 fixture 恒 setSplashDisabledForTests(true) 让 drawSplashIfNeeded
// 短路返 false，跑不出 splash 路径。时序/真渲染由 probe（screenshot_probe）覆盖；
// 本文件只钉源码契约——这是唯一能在无头套件里钉住"不回归极光波 / 不回归硬编码色"的层。
#include "doctest.h"

#include <ui_kit/splash.h>  // 墨染曲线纯函数（kInkDurationSec / inkFrontFraction / inkWashAlpha）

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// 读源文件全文为字符串；失败返回空串（调用方用 DOCTEST_CHECK(!src.empty()) 接住）。
//
// 路径推导：__FILE__ = <source_root>/tests/test_modules/test_splash_contract.cpp
// （CMake 以 ${CMAKE_SOURCE_DIR}/... 绝对路径登记源文件，故 __FILE__ 是绝对路径）。
// source_root = parent_path() ×3（test_modules → tests → root）。
// weakly_canonical 规整 .. / . / 平台分隔符差异，不要求目标存在。
std::string readSourceFile(const std::string& relPath) {
    namespace fs = std::filesystem;
    const fs::path here = fs::weakly_canonical(fs::path(__FILE__));
    const fs::path sourceRoot =
        here.parent_path().parent_path().parent_path();
    const fs::path target = sourceRoot / relPath;
    std::ifstream in(target, std::ios::binary);
    if (!in.is_open()) return std::string{};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

REGISTER_TEST(splash_api_signature, "splash.h: API 签名保 drawSplashIfNeeded + setSplashDisabledForTests", {
    // 防：splash.h 接口签名被改（main.cpp 等调用方不动；API 变会断调用方编译）。
    // D-11 反转只重写 splash.cpp 实现，header 声明恒定。
    const std::string src = readSourceFile("src/ui_kit/splash.h");
    DOCTEST_CHECK(!src.empty());
    DOCTEST_CHECK(contains(src,
                   "bool drawSplashIfNeeded(eui::Ui& ui, const eui::Screen& screen);"));
    DOCTEST_CHECK(contains(src, "void setSplashDisabledForTests(bool disabled)"));
    return true;
})

REGISTER_TEST(splash_wechat_tokens, "splash.cpp: 底色 + 进度条 + 文字走 wechat 令牌（D-11 WeChat-minimal + H 批绿色印染）", {
    // 防：splash 脱离 wechat 令牌、改回硬编码色字面量（W0.5 W-W1~W-W3 + #15-#17）。
    // H 批（UAT2 2026-10-20）：底色 pageBg → sidebarBgTinted（用户"绿色印染"诉求）；
    // 图标/进度条填充 primary → primaryPressed（#07C160 在微绿底 #E8F0EC 上仅 2.06:1，
    // 纯白底上限 2.39:1，浅底方案到不了图形 AA 3:1；#059A4C 在该底上 3.16:1 ≥ 3:1）；
    // 轨道保持 cardBg（白轨对微绿底 1.16:1 > 原灰底 1.07:1；dividerPrimary 仅 1.09:1 更弱）。
    const std::string src = readSourceFile("src/ui_kit/splash.cpp");
    DOCTEST_CHECK(!src.empty());
    DOCTEST_CHECK(contains(src, "wechat.sidebarBgTinted"));  // 全屏底色（H 批绿色印染）
    DOCTEST_CHECK(contains(src, "wechat.primaryPressed"));   // 进度条填充 + logo 图标（AA 对比档）
    DOCTEST_CHECK(contains(src, "wechat.textPrimary"));    // logo 文字
    DOCTEST_CHECK(contains(src, "wechat.textSecondary"));   // 副标题文字
    DOCTEST_CHECK(contains(src, "wechat.cardBg"));          // 进度条轨道
    // 负向契约（H 批）：防回退到低对比组合——底色回 pageBg 灰（用户诉求回退）、
    // 图标/填充回 primary #07C160（微绿底上 2.06:1 破图形 AA）。匹配代码级调用形态，
    // 不匹配注释里的令牌名叙述。
    DOCTEST_CHECK(!contains(src, "color(wc.pageBg)"));
    DOCTEST_CHECK(!contains(src, "color(wc.primary)"));
    return true;
})

REGISTER_TEST(splash_no_shadertoy_regression, "splash.cpp: 不回归极光波 shadertoy（§8 禁项 #4 + D-11 反转）", {
    // 防：splash 重新引入 shadertoy / fragment shader / 极光波渐变（D-11 已反转去除）。
    // 任意一条命中即说明极光波代码路径复活，splash 视觉与 WeChat-minimal 基调冲突。
    // 注：用 "shadertoy("（带左括号）而非裸 "shadertoy"——后者会误命中本测试允许的
    // 描述性注释（如"D-11 反转：去除极光波 shadertoy 渐变"）；代码级调用才该被拦。
    const std::string src = readSourceFile("src/ui_kit/splash.cpp");
    DOCTEST_CHECK(!src.empty());
    DOCTEST_CHECK(!contains(src, "shadertoy("));        // EUI-NEO DSL 调用点
    DOCTEST_CHECK(!contains(src, "ShaderToyGraph"));   // 图类型字段
    DOCTEST_CHECK(!contains(src, "ShaderToyError"));   // 错误回调参数类型
    DOCTEST_CHECK(!contains(src, "splash.frag"));      // shader 资产路径串
    DOCTEST_CHECK(!contains(src, ".frag"));           // 任意 frag 资产引用
    DOCTEST_CHECK(!contains(src, "onCompileError"));   // 编译错误回调
    DOCTEST_CHECK(!contains(src, ".gradient("));       // 进度条改实色填充，禁渐变 API
    return true;
})

REGISTER_TEST(splash_no_hardcoded_literals, "splash.cpp: 不含硬编码色字面量（W0.5 全量 6 处）", {
    // 防：splash 重新引入 W0.5 清单里的 6 处字面量——
    // 3 处白色（W-W1 logo 文字 / W-W2 副标题 / W-W3 进度条轨道）+
    // 3 处非白（#15 深蓝兜底 / #16 渐变蓝端点 / #17 渐变绿端点）。
    // 任意一条命中即说明清字面量工作被回滚。
    const std::string src = readSourceFile("src/ui_kit/splash.cpp");
    DOCTEST_CHECK(!src.empty());
    // W-W1/W-W2/W-W3：白色花括号字面量（含 0.10 透明轨道变体）
    DOCTEST_CHECK(!contains(src, "Color{1.0f, 1.0f, 1.0f"));
    DOCTEST_CHECK(!contains(src, "Color{1.0f, 1.0f, 1.0f, 0.10f}"));
    // #15：深蓝兜底底色（splash.cpp:82）
    DOCTEST_CHECK(!contains(src, "Color{0.02f, 0.10f, 0.20f"));
    // #16：渐变蓝端点（splash.cpp:159）
    DOCTEST_CHECK(!contains(src, "Color{0.04f, 0.42f, 1.0f"));
    // #17：渐变绿端点（splash.cpp:160）
    DOCTEST_CHECK(!contains(src, "Color{0.13f, 0.69f, 0.44f"));
    return true;
})

// ==================== UAT6 动态印染（墨染径向扩散）====================

// 墨染曲线行为断言：直调 splash.h 内联纯函数（测试缝同 header inline 先例，
// 不拉 splash.cpp.o）。防：时长档漂出用户 1~1.5s、前沿半径非单调/不达满圆、
// 浓度包络不归零（终态残留墨染 ≠ 静态底色，W 批底色断言会被动态层遮蔽）
REGISTER_TEST(splash_ink_curve, "splash.h: 墨染曲线（时长 1.2s / 半径 easeOut 单调达满 / 浓度稀释归零）", {
    DOCTEST_CHECK_EQ(ui_kit::kInkDurationSec, 1.2f);  // 用户档 1~1.5s
    // 前沿半径：0 → 0、单调升、1 → 1（铺满全屏，最远角也吃到墨）
    DOCTEST_CHECK_EQ(ui_kit::inkFrontFraction(0.0f), 0.0f);
    DOCTEST_CHECK(ui_kit::inkFrontFraction(0.0f) < ui_kit::inkFrontFraction(0.25f));
    DOCTEST_CHECK(ui_kit::inkFrontFraction(0.25f) < ui_kit::inkFrontFraction(0.5f));
    DOCTEST_CHECK(ui_kit::inkFrontFraction(0.5f) < ui_kit::inkFrontFraction(0.75f));
    DOCTEST_CHECK_EQ(ui_kit::inkFrontFraction(1.0f), 1.0f);
    // easeOut 自检：前半程过 87.5%（1 − 0.5³ = 0.875）——先急速铺开再缓收
    DOCTEST_CHECK_GT(ui_kit::inkFrontFraction(0.5f), 0.874f);
    // 非法入参钳制（负值/超界与端点同值，不产生倒退或外抛）
    DOCTEST_CHECK_EQ(ui_kit::inkFrontFraction(-0.5f), 0.0f);
    DOCTEST_CHECK_EQ(ui_kit::inkFrontFraction(1.5f), 1.0f);
    // 浓度包络：随扩散单调摊薄、终点归零（终态=纯静态底色）、入参钳制
    DOCTEST_CHECK_GT(ui_kit::inkWashAlpha(0.0f), ui_kit::inkWashAlpha(0.25f));
    DOCTEST_CHECK_GT(ui_kit::inkWashAlpha(0.25f), ui_kit::inkWashAlpha(0.5f));
    DOCTEST_CHECK_EQ(ui_kit::inkWashAlpha(1.0f), 0.0f);
    DOCTEST_CHECK_EQ(ui_kit::inkWashAlpha(1.5f), 0.0f);
    DOCTEST_CHECK_EQ(ui_kit::inkWashAlpha(-1.0f), 1.0f);
    return true;
})

// 墨染接线源码断言：防「曲线在但没接上」——inkProgress 必须真的驱动半径/浓度，
// 帧泵（md.tree.anim.driver 同款 onFrame 元素）必须在途挂载、收敛剪除，洗染色
// 走 wechat.primaryPressed 令牌（有机晕染的噪声卫星也在令牌体系内，禁字面量）
REGISTER_TEST(splash_ink_dynamic_wiring, "splash.cpp: 墨染接线（inkProgress 驱动 + onFrame 帧泵 + 终态收敛 + 令牌色）", {
    const std::string src = readSourceFile("src/ui_kit/splash.cpp");
    DOCTEST_CHECK(!src.empty());
    // 进度源（等效 uniform）：elapsed 归一化驱动，曲线经 splash.h 内联函数接线
    DOCTEST_CHECK(contains(src, "kInkDurationSec"));
    DOCTEST_CHECK(contains(src, "inkFrontFraction(inkProgress)"));
    DOCTEST_CHECK(contains(src, "inkWashAlpha(inkProgress)"));
    // 帧泵在途挂载 + 收敛剪除（条件块包住整套墨染元素与泵）
    DOCTEST_CHECK(contains(src, "if (inkProgress < 1.0f)"));
    DOCTEST_CHECK(contains(src, "splash.ink.anim.driver"));
    DOCTEST_CHECK(contains(src, ".onFrame("));
    // 墨染元素与洗染色令牌（同 logo/进度条的 AA 对比档令牌）
    DOCTEST_CHECK(contains(src, "splash.ink.ring."));
    DOCTEST_CHECK(contains(src, "splash.ink.blob."));
    return true;
})
