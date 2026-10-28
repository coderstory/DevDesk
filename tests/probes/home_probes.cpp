// home_probes.cpp — home 模块几何探针。组 A..F + K，每组一段，各自带判据注释。
//
// 运行：home_probes.exe [--frames N] [--size 1200x600]
// 全程离线假种子，可复现、不打任何外部接口。
// statW 只随窗**宽**变，所以各组几何在默认 1200x1000 与守护短窗 1200x600 下同型
//（600 < 设计最小窗高 640，UAT3 D-R3-2 起刻意低于一切真实下限作守护性深档）。

#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "module_home/home_service.h"  // startOfDayEpoch（逐时 token 种子的本地零点锚）
#include <vector>
#include "core/theme.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>

namespace {

// RGB 通道差异（>1/255 视为不同色；alpha 不参与——hover/surface 语义比色）
bool colorsDiffer(const eui::Color& a, const eui::Color& b) {
    constexpr float kEps = 0.004f;
    return std::fabs(a.r - b.r) > kEps || std::fabs(a.g - b.g) > kEps ||
           std::fabs(a.b - b.b) > kEps || std::fabs(a.a - b.a) > kEps;
}

// 两 frame 在 y 上的间隙（b.y - (a.y + a.height)）：正 = a 在 b 上方分离，
// 0 = 邻接，负 = a 探进 b
float frameGapY(const core::dsl::Element& a, const core::dsl::Element& b) {
    return b.frame.y - (a.frame.y + a.frame.height);
}

bool framesIntersect(const core::dsl::Element& a, const core::dsl::Element& b) {
    return a.frame.x < b.frame.x + b.frame.width &&
           b.frame.x < a.frame.x + a.frame.width &&
           a.frame.y < b.frame.y + b.frame.height &&
           b.frame.y < a.frame.y + a.frame.height;
}

// 统计卡 id 表（与 THS-08 元数据表 statCardElementId 同值的五个）
constexpr const char* kStatCardIds[] = {
    "home.stat.tokens", "home.stat.notes", "home.stat.todo",
    "home.stat.overdue", "home.stat.api",
};

// 文本防溢判别（组 F）：clip/wrap 任一 true = 已显式保护；否则用
// TextPrimitive::measureTextSize（layout 同款，dsl.h:1729）量内在宽，
// 内在宽 ≤ 元素 frame 宽 + 1px（rounding）视为放得下。
bool textFitsOrGuarded(const core::dsl::Element& el) {
    if (el.clip || el.wrap) {
        return true;
    }
    core::TextStyle style;
    style.text = el.text;
    style.fontFamily = el.fontFamily;
    style.fontSize = el.fontSize;
    style.fontWeight = el.fontWeight;
    style.maxWidth = el.maxWidth;
    style.wrap = el.wrap;
    style.lineHeight = el.lineHeight;
    const core::Vec2 intrinsic = core::TextPrimitive::measureTextSize(style);
    return intrinsic.x <= el.frame.width + 1.0f;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    auto cfg = test_support::tempConfigFile("pfx_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    // 数据底座（组 D/E/F 前提）：已 migrate 的临时库注册为进程级 appDatabase。
    // null 则 statsError、API 分布全零 → 分布图走空态分支不建图，组 D 结构前提失效。
    // 种子 = 1 条 profile（claude_call_logs FK）+ 今日 2 条**带用量**的调用：
    // 纵轴 = 每小时 output_tokens 合计，output_tokens 为 0 的行合计 0（= 没数据）
    // → 依然走空态、组 D 结构前提失效。两条落在 9 时与 17 时 → 去头去尾后恰
    // 9 根柱（9..17 闭区间）。
    test_support::TestEnv env("hprobe_", "home", true);  // 注册为 appDatabase
    {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const int64_t dayStart = module::home::startOfDayEpoch(
            static_cast<std::time_t>(now));
        auto& sq = env.db->db();
        sq.exec(
            "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
            "api_key_check, model, created_at, updated_at) "
            "VALUES ('p1', 'http://x', 'sk-***', 'chk', 'm', " +
            std::to_string(now) + ", " + std::to_string(now) + ")");
        sq.exec(
            "INSERT INTO claude_call_logs (profile_id, method, path, "
            "status_code, latency_ms, output_tokens, started_at) VALUES "
            "(1, 'POST', '/v1/messages', 200, 2000, 400, " +
            std::to_string(dayStart + 3600 * 9) + "), "
            "(1, 'POST', '/v1/messages', 200, 2400, 600, " +
            std::to_string(dayStart + 3600 * 17) + ")");
    }

    core::proxy_stats::resetForTest();  // 进程内确定性：空闲无采样/无流

    std::unique_ptr<::core::Module> mod(create_home_module());
    mod->onStart();  // service_ + refreshStats（读种子库）

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "home";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 树 + layout），
    // 两者成对，缺一个会让帧序与真机不一致
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };
    auto settleFrames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            composeFrame();
        }
    };

    settleFrames(2);  // 首帧构建树（结构 diff 计入 paint 预算，组 B 前消化）

    // 「种子在场」的中途落图（--screenshot-seeded <路径>）：统计卡 / 图表种子全画着。
    // 未给该开关 = 立刻返回（零开销）。
    probe::captureSeedIfRequested(args, window, runtime, &passed);

    // 组 A：类 1(b) sparkline z（P1）
    // home.stat.tokens.spark（sparkwrap 顶带 y0..16）必须整带位于 label
    // （.label y16..32）之上：不相交 + gapY ≥ 0（邻接语义）。回归成 32px 盒
    // (0,0) → 与 label 相交 → 红。
    // 不断言曲线段：linechart.h 空闲态 {0.0f} 零长度段被跳过，且平线贴 bottomY
    // 时胶囊半宽会把段 frame 探进 label 带。
    {
        const core::dsl::Element* spark =
            ui != nullptr ? ui->find("home.stat.tokens.spark") : nullptr;
        const core::dsl::Element* label =
            ui != nullptr ? ui->find("home.stat.tokens.label") : nullptr;
        if (spark == nullptr || label == nullptr) {
            std::cerr << "home_probes: 组 A 结构缺失 (spark="
                      << (spark != nullptr ? "ok" : "null")
                      << ", label=" << (label != nullptr ? "ok" : "null")
                      << ")——Token 卡/图表未构建\n";
            passed = false;
        } else {
            const float gapY = frameGapY(*spark, *label);
            if (framesIntersect(*spark, *label) || gapY < -0.5f) {
                std::cerr << "home_probes: 类1(b) FAIL——spark frame ("
                          << spark->frame.x << "," << spark->frame.y << " "
                          << spark->frame.width << "x" << spark->frame.height
                          << ") 与 label frame (" << label->frame.x << ","
                          << label->frame.y << " " << label->frame.width << "x"
                          << label->frame.height << ") 相交，gapY=" << gapY
                          << "（修复后 spark 应整带在 label 之上）\n";
                passed = false;
            }
        }
    }

    // 组 B：类 2(a) 空转重绘（P1）
    // 稳定 15 帧（初始动画/hover 收敛）后连跑 60 帧 pollOnce（不注入、不
    // recompose），计数 update() 返回 true（= paintRequested_）的帧数。
    // 空闲态 tokenLive false → 无 .timer，期望 ≤1（首帧遗留）。回归成常驻
    // onTimer → 60 帧内 ≥2 → 红。
    {
        settleFrames(15);
        int paintFrames = 0;
        for (int i = 0; i < 60; ++i) {
            if (probe::pollOnce(window, runtime)) {
                ++paintFrames;
            }
        }
        if (paintFrames > 1) {
            std::cerr << "home_probes: 类2(a) FAIL——空转 60 帧 update() 返 "
                         "true " << paintFrames
                      << " 帧（期望 ≤1；onTimer 常驻整页重绘回归？）\n";
            passed = false;
        }
    }

    // 组 C：类 2(b) hover token（P1）
    // hover != surface：深色档（默认 current()）天然不等，浅色档需显式验。
    // 双档各验一次，验完还原深色（后续段几何不受主题影响，保持确定性）。
    {
        bool cOk = true;
        const auto checkTheme = [&](const char* themeName) {
            const auto& t = core::theme::current();
            if (!colorsDiffer(t.colors.hover, t.colors.surface)) {
                std::cerr << "home_probes: 类2(b) FAIL——" << themeName
                          << " 档 hover (" << t.colors.hover.r << ","
                          << t.colors.hover.g << "," << t.colors.hover.b
                          << ") == surface (" << t.colors.surface.r << ","
                          << t.colors.surface.g << "," << t.colors.surface.b
                          << ")（hover 反馈不可见）\n";
                cOk = false;
            }
        };
        checkTheme("深色默认");
        core::theme::setLight();
        checkTheme("浅色");
        core::theme::setDark();
        if (!cOk) {
            passed = false;
        }
    }

    // 组 D/E 前：API 分布卡当前帧
    composeFrame();

    // 组 D：类 4(a) barChart 几何（P1）
    // plot 区总高 = grid.3.y - grid.0.y（barchart.h 四条横线 y = plotY + k·H/3
    // → grid.0 在 plotY、grid.3 在 bottomY）；不量 bar 高度，因为 bar 有 0.16s
    // 帧过渡。种子的非零用量 → 非空态 → 去头去尾后 9 根柱（9..17）正常建图。
    // UAT6 零滚动收口后卡高 220（kHomeChartH）→ wrap 156 → plot 44 ≥ 36 仍过；
    // 回归成卡高 <212 → plot <36 → 红。
    {
        const core::dsl::Element* grid0 =
            ui != nullptr ? ui->find("home.api.chart.bars.grid.0") : nullptr;
        const core::dsl::Element* grid3 =
            ui != nullptr ? ui->find("home.api.chart.bars.grid.3") : nullptr;
        if (grid0 == nullptr || grid3 == nullptr) {
            std::cerr << "home_probes: 类4(a) 结构缺失 (grid.0="
                      << (grid0 != nullptr ? "ok" : "null")
                      << ", grid.3=" << (grid3 != nullptr ? "ok" : "null")
                      << ")——barChart 未构建（种子落空或走空态分支）\n";
            passed = false;
        } else {
            const float plotH = grid3->frame.y - grid0->frame.y;
            if (plotH < 35.5f) {
                std::cerr << "home_probes: 类4(a) FAIL——bar 面板 plot 区总高 "
                          << plotH << "px < 36（审计修 260 卡高回退？）\n";
                passed = false;
            }
        }
    }

    // 组 E：类 5(a) API 卡对称（P3）
    // title/wrap 相对卡 content（inset 16）以 (0,·) 定位 → 左右间隙各 16，
    // |左 - 右| ≤ 2px。content 已含 inset，再叠一层 position(16,..) → 左 32 右 0。
    {
        const core::dsl::Element* card =
            ui != nullptr ? ui->find("home.api.chart") : nullptr;
        const core::dsl::Element* title =
            ui != nullptr ? ui->find("home.api.chart.title") : nullptr;
        const core::dsl::Element* wrap =
            ui != nullptr ? ui->find("home.api.chart.wrap") : nullptr;
        if (card == nullptr || title == nullptr || wrap == nullptr) {
            std::cerr << "home_probes: 类5(a) 结构缺失 (card="
                      << (card != nullptr ? "ok" : "null")
                      << ", title=" << (title != nullptr ? "ok" : "null")
                      << ", wrap=" << (wrap != nullptr ? "ok" : "null")
                      << ")\n";
            passed = false;
        } else {
            const float cardRight = card->frame.x + card->frame.width;
            const auto checkSymmetric = [&](const char* name,
                                            const core::dsl::Element& el) {
                const float leftGap = el.frame.x - card->frame.x;
                const float rightGap =
                    cardRight - (el.frame.x + el.frame.width);
                if (std::fabs(leftGap - rightGap) > 2.0f) {
                    std::cerr << "home_probes: 类5(a) FAIL——" << name
                              << " 左右间隙差 " << (leftGap - rightGap)
                              << "px（左 " << leftGap << " 右 " << rightGap
                              << "；content 已含 inset，不应二次偏移）\n";
                    passed = false;
                }
            };
            checkSymmetric("title", *title);
            checkSymmetric("wrap", *wrap);
        }
    }

    // 组 F：类 6(a) stat 溢出（P1）
    // 注入：测试时钟（每次 nowMs() +1000ms）+ 单流累计 20M→40M → 2s 窗口
    // delta 20M → rateTps=10M → 「10000000」（8 位）。数字自 UAT2 起为 22px
    // 固定字号（fontSizeLarge+4，见 drawGlassStatCard；旧的 48px+动态收缩已删）：
    // 8 位内在宽 ≈106 < cw 176 天然收纳，无溢出。本组防的是回归——字号再涨
    // 或盒再窄 → 下面两段式判别兜底变红。
    // badge = 流式档「● 1 路 · 累计 40M」，相对当前元素 frame 度量，是弱判别；
    // 数字（8 位）才是强判别。
    // 判别两段式：clip/wrap true → 过；否则 measureTextSize 量内在宽比盒宽。
    {
        int64_t fakeT = -1;
        core::proxy_stats::setTestClock([&fakeT]() {
            fakeT += 1000;
            return fakeT;
        });
        const uint64_t streamId = core::proxy_stats::beginStream();
        core::proxy_stats::updateStream(streamId, 20000000);
        core::proxy_stats::updateStream(streamId, 40000000);
        composeFrame();
        const int kMinWindowW = 1000;  // 设计最小窗宽（UAT3 D-R3-2，main.cpp kMinWindowLogical）：低于此宽的文本溢出归因窗口过小
        for (const char* cardId : kStatCardIds) {
            for (const char* suffix : {"number", "badge"}) {
                const std::string elId =
                    std::string(cardId) + "." + suffix;
                const core::dsl::Element* el =
                    ui != nullptr ? ui->find(elId) : nullptr;
                if (el == nullptr) {
                    std::cerr << "home_probes: 类6(a) 结构缺失 '" << elId
                              << "'\n";
                    passed = false;
                    continue;
                }
                if (textFitsOrGuarded(*el)) {
                    continue;
                }
                core::TextStyle style;
                style.text = el->text;
                style.fontSize = el->fontSize;
                style.fontWeight = el->fontWeight;
                const core::Vec2 intrinsic =
                    core::TextPrimitive::measureTextSize(style);
                std::cerr << "home_probes: 类6(a) FAIL——'" << elId
                          << "' 文本「" << el->text << "」内在宽 "
                          << intrinsic.x << "px > 可用宽 "
                          << el->frame.width << "px（clip="
                          << (el->clip ? 1 : 0) << " wrap="
                          << (el->wrap ? 1 : 0)
                          << "；窗宽 " << args.windowW
                          << (args.windowW >= kMinWindowW
                                  ? ""
                                  : " ——最小窗 1200 判定！")
                          << "，字号收缩或文案长度回退？）\n";
                passed = false;
            }
        }
        core::proxy_stats::endStream(streamId);
        core::proxy_stats::resetForTest();  // 清时钟与状态
    }

    // 组 K：矮窗多档——首页在真实最小窗各档仍完整构建；≥ 真实最小页高零滚动、
    // 低于它才由 home.scroll 吸收溢出
    // home_probes_min_window 只跑 1200×600，而真实设计最小窗是 1000×640 逻辑
    // （main.cpp kMinWindowLogical*，UAT3 D-R3-2 从 1280×800 收窄）。收窄后两平台
    // **均不再钳**：macOS 640 ≤ 805 可用、Windows@125% 640×1.25=800 物理 ≤ 973
    // 可用——有效下限 = 设计下限（640）。
    //
    // UAT6 最小窗零滚动收口（用户：「首页出现滚动条完全没必要」）：本组 compose
    // 的 screen 是**页区高**口径——Windows 自绘壳 1000×640 窗 = 页高 600（标题条
    // 40，app_sidebar.cpp bodyTop），macOS 系统标题栏 = 页高 640；真实最紧档是
    // 600。首页内容 = 页头 40 + 统计行 144×档 + gap 20×3 + 图表卡 220
    // （kHomeChartH 260→220）+ 快捷按钮行 44+16 底边距 = 100% 档 524：600/640
    // 档**零滚动**（滚动条元素不渲染）；内容超出只发生在 115%/130% 字号档或低于
    // 真实最小页高的退化矮窗——那才是滚动吸收机制要守的行为，故新加 480 守护档
    // 让机制仍被真实验证。五档（screen 高 = 页区高口径）：
    //   · 845（合成档，旧设计最小窗值，专测历史刀口档位）；
    //   · 778 旧 Windows@125% 有效下限（现高于 640 设计下限，普通深矮窗保留覆盖）；
    //   · 640 设计最小窗 = macOS 真实页高（无自绘壳；Windows 真实页高 600）；
    //   · 600 Windows 自绘壳真实最小页高（640−40 标题条）= 最紧真实档，零滚动契约档；
    //   · 480 守护性深档（< 真实最小页高 600）：内容高 524 > 视口，滚动吸收不丢内容。
    // 判据（每档一致）：
    //   K(a) 整页滚动容器在场（矮窗可见性依赖它吸收溢出）
    //   K(b) 既有内容一条不少（stats / chart / actions / root）
    //   K(c) 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
    //   K(e) 快捷按钮行距内容盒底 ≥ 16（防贴边底边距）
    //   K(d) 滚动条渲染契约：≥600 档不得出现（home.scroll.scroll 查无 = 零滚动），
    //        480 守护档必须在场（滚动吸收机制没被零滚动收口顺手拆掉）
    {
        struct Tier {
            float height;
            const char* label;
        };
        const Tier tiers[] = {
            {845.0f, "845 合成档（旧设计最小窗值；现行设计最小窗 1000×640）"},
            {778.0f, "1000×778 旧 Windows@125% 有效下限档（现高于 640 设计下限，普通深矮窗）"},
            {640.0f, "1000×640 设计最小窗 = macOS 真实页高（Windows 实页高 600）；零滚动档"},
            {600.0f, "1000×600 Windows 自绘壳真实最小页高（640−40 标题条）；零滚动契约档"},
            {480.0f, "1000×480 守护性深档（< 真实最小页高 600；内容高 524 > 视口，滚动吸收不丢内容）"},
        };
        for (const Tier& tier : tiers) {
            eui::Screen tierScreen;
            tierScreen.width = 1000.0f;
            tierScreen.height = tier.height;
            eui::Ui tierUi;
            tierUi.begin("home_probe_k_" +
                         std::to_string(static_cast<int>(tier.height)));
            mod->compose(tierUi, tierScreen);
            tierUi.end();
            // 必须 layout：compose 只建树不排版，不 layout 所有 frame 都是 0，
            // 后面那些「>= 0 / 不溢出」判据会全落进「0 <= 0.5」的恒真假绿。
            tierUi.layout(tierScreen.width, tierScreen.height);

            const core::dsl::Element* pageScroll =
                tierUi.find("home.scroll.content");
            const core::dsl::Element* stats = tierUi.find("home.stats");
            const core::dsl::Element* chart = tierUi.find("home.api.chart");
            const core::dsl::Element* acts = tierUi.find("home.actions");
            const core::dsl::Element* root = tierUi.find("home.root");
            const core::dsl::Element* scrollbar =
                tierUi.find("home.scroll.scroll");
            const char* ok = "ok";
            const char* no = "null";
            if (pageScroll == nullptr || stats == nullptr || chart == nullptr ||
                acts == nullptr || root == nullptr) {
                std::cerr << "home_probes: 组 K(a/b) FAIL（" << tier.label
                          << "）——结构缺失 (scrollContent="
                          << (pageScroll != nullptr ? ok : no)
                          << ", stats=" << (stats != nullptr ? ok : no)
                          << ", chart=" << (chart != nullptr ? ok : no)
                          << ", acts=" << (acts != nullptr ? ok : no)
                          << ", root=" << (root != nullptr ? ok : no) << ")\n";
                passed = false;
                continue;
            }
            // K(c) 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
            if (acts->frame.y + acts->frame.height >
                root->frame.y + root->frame.height + 0.5f) {
                std::cerr << "home_probes: 组 K(c) FAIL（" << tier.label
                          << "）——快捷按钮行溢出滚动内容盒：底="
                          << (acts->frame.y + acts->frame.height) << " > 内容底="
                          << (root->frame.y + root->frame.height) << "\n";
                passed = false;
            }
            // K(e) 快捷按钮行底部留白 = 16（2026-10-27 真机反馈贴边；home.root
            // 底 padding 0，行距内容盒底的距离全部来自行自身 margin.bottom）。
            if (const float bottomGap =
                    (root->frame.y + root->frame.height) -
                    (acts->frame.y + acts->frame.height);
                bottomGap < 16.0f - 0.5f) {
                std::cerr << "home_probes: 组 K(e) FAIL（" << tier.label
                          << "）——快捷按钮行距内容盒底 " << bottomGap
                          << " < 16（防贴边底边距被拆？）\n";
                passed = false;
            }
            // K(d) 滚动条渲染契约：scrollView 只在「内容高 > 视口」时 build
            // <id>.scroll 滚动条（scrollview.h scrollable 分支）——≥600 档查无
            //（零滚动契约），480 守护档必须在场（滚动吸收机制被真实验证）。
            if (tier.height >= 600.0f) {
                if (scrollbar != nullptr) {
                    std::cerr << "home_probes: 组 K(d) FAIL（" << tier.label
                              << "）——页高 ≥ 真实最小页高 600 仍出滚动条"
                                 "（内容高 524 ≤ 视口应零滚动；图表卡高度回涨？）\n";
                    passed = false;
                }
            } else if (scrollbar == nullptr) {
                std::cerr << "home_probes: 组 K(d) FAIL（" << tier.label
                          << "）——低于真实最小页高却无滚动条（内容高 524 > 视口，"
                             "home.scroll 吸收机制被拆掉？）\n";
                passed = false;
            }
        }
    }

    // 退出前落一张「当前状态图」（--screenshot <路径>）
    // 通用看图口：跑一次 home_probes 就能拿到此刻首页长什么样。不传该开关时
    // captureIfRequested 立刻返回（零开销 / 不写文件）。
    probe::captureIfRequested(args, window, runtime, &passed);

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}