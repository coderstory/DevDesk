// tests/test_modules/test_splash_contract.cpp
// splash 源码契约测试（W2a，D-11 反转后新增）。
//
// 防的事故：splash 回归极光波 shadertoy 渐变（§8 禁项 #4：大面积渐变背景）；
// 以及 splash 重新引入硬编码色字面量（W0.5 清单 W-W1~W-W3 + #15-#17）。
//
// 为什么读源文件而不是跑渲染：splash 需 GL 上下文 + 2.3s 时序，无头单测活不过；
// test_main.cpp 的 fixture 恒 setSplashDisabledForTests(true) 让 drawSplashIfNeeded
// 短路返 false，跑不出 splash 路径。时序/真渲染由 probe（screenshot_probe）覆盖；
// 本文件只钉源码契约——这是唯一能在无头套件里钉住"不回归极光波 / 不回归硬编码色"的层。
#include "doctest.h"

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
    DOCTEST_CHECK(contains(src, "void setSplashDisabledForTests(bool disabled);"));
    return true;
})

REGISTER_TEST(splash_wechat_tokens, "splash.cpp: 底色 + 进度条 + 文字走 wechat 令牌（D-11 WeChat-minimal）", {
    // 防：splash 脱离 wechat 令牌、改回硬编码色字面量（W0.5 W-W1~W-W3 + #15-#17）。
    // 底色 pageBg、进度条填充 primary、logo 图标 primary、文字 textPrimary/textSecondary。
    const std::string src = readSourceFile("src/ui_kit/splash.cpp");
    DOCTEST_CHECK(!src.empty());
    DOCTEST_CHECK(contains(src, "wechat.pageBg"));          // 全屏底色
    DOCTEST_CHECK(contains(src, "wechat.primary"));        // 进度条填充 + logo 图标
    DOCTEST_CHECK(contains(src, "wechat.textPrimary"));    // logo 文字
    DOCTEST_CHECK(contains(src, "wechat.textSecondary"));   // 副标题文字
    DOCTEST_CHECK(contains(src, "wechat.cardBg"));          // 进度条轨道
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
