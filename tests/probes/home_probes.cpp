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
    // 环境不可用 ≠ 代码 bug（skill 铁律）：显示器装不下请求的逻辑窗高时，系统会钳窗口
    //（实测 1200x1000 请求 → 1625x1065px≈1200x787 逻辑），按请求尺寸布局的页底内容
    // 落在帧外、像素判据无从采样 → 显式返回 77（ctest SKIP_RETURN_CODE），失败仍优先红。
    bool envSkipped = false;

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

    // 组 A：统计卡文字列对齐 + spark 同带（2026-10-31 改判据）
    // 原判据「spark 整带在 label 之上」正是文字下移的成因：五张卡里只有 Token 卡带
    // spark，于是它在纵向让出 16px，数字比其余四张低一截，小窗（statH=96）时数字底边
    // 还会越出卡底。现判据（谁再让单张卡的文字位移就红）：
    //   A1 五卡 .label 顶 / .number 顶逐一对齐（容差 0.5）
    //   A2 label→number 间隙 = 4px（numGap，2026-11-01 重构 8→4：96 高卡内容区只有 56，
    //      文本块 16+4+34=54 才装得下）
    //   A3 数字底边不越出卡底（statH=96、内边距 40 → 内容区 56；这条只守「不溢出」）
    //   A4 spark 与数字同带（顶对齐、同宽、高 = 数字高 − 4）且宽 = 内容宽（垫在数字下面）
    //   A5 spark 不得盖到 label 带（防回归成 32px 盒压住标题）
    {
        const core::dsl::Element* tokenSpark =
            ui != nullptr ? ui->find("home.stat.tokens.spark") : nullptr;
        const core::dsl::Element* tokenNumber =
            ui != nullptr ? ui->find("home.stat.tokens.number") : nullptr;
        const core::dsl::Element* tokenLabel =
            ui != nullptr ? ui->find("home.stat.tokens.label") : nullptr;
        if (tokenSpark == nullptr || tokenNumber == nullptr || tokenLabel == nullptr) {
            std::cerr << "home_probes: 组 A 结构缺失 (spark="
                      << (tokenSpark != nullptr ? "ok" : "null")
                      << ", number=" << (tokenNumber != nullptr ? "ok" : "null")
                      << ", label=" << (tokenLabel != nullptr ? "ok" : "null")
                      << ")——Token 卡/图表未构建\n";
            passed = false;
        } else {
            float refLabelY = -1.0f;
            float refNumberY = -1.0f;
            for (const char* cardId : kStatCardIds) {
                const std::string base(cardId);
                const core::dsl::Element* label =
                    ui != nullptr ? ui->find(base + ".label") : nullptr;
                const core::dsl::Element* number =
                    ui != nullptr ? ui->find(base + ".number") : nullptr;
                const core::dsl::Element* bg =
                    ui != nullptr ? ui->find(base) : nullptr;
                if (label == nullptr || number == nullptr) {
                    std::cerr << "home_probes: 组 A 结构缺失——" << base
                              << " 的 label/number 未构建\n";
                    passed = false;
                    continue;
                }
                if (refLabelY < 0.0f) {
                    refLabelY = label->frame.y;
                    refNumberY = number->frame.y;
                }
                if (std::fabs(label->frame.y - refLabelY) > 0.5f ||
                    std::fabs(number->frame.y - refNumberY) > 0.5f) {
                    std::cerr << "home_probes: 组 A1 FAIL——" << base
                              << " 文字行与首卡不对齐（label y=" << label->frame.y
                              << " vs " << refLabelY << "，number y="
                              << number->frame.y << " vs " << refNumberY << "）\n";
                    passed = false;
                }
                const float gap = frameGapY(*label, *number);
                if (std::fabs(gap - 4.0f) > 0.5f) {
                    std::cerr << "home_probes: 组 A2 FAIL——" << base
                              << " label→number 间隙 " << gap << "px（期望 4）\n";
                    passed = false;
                }
                if (bg != nullptr) {
                    const float bottomMargin =
                        (bg->frame.y + bg->frame.height) -
                        (number->frame.y + number->frame.height);
                    if (bottomMargin < 0.5f) {
                        std::cerr << "home_probes: 组 A3 FAIL——" << base
                                  << " 数字越出卡底 " << -bottomMargin
                                  << "px（应整块在卡内）\n";
                        passed = false;
                    }
                }
            }
            // spark 带 = 数字带顶部对齐、同宽、高 = 数字带 − 4（底部留 4px 让平线可见）
            if (std::fabs(tokenSpark->frame.y - tokenNumber->frame.y) > 0.5f ||
                std::fabs(tokenSpark->frame.height - (tokenNumber->frame.height - 4.0f)) > 0.5f ||
                std::fabs(tokenSpark->frame.width - tokenNumber->frame.width) > 0.5f) {
                std::cerr << "home_probes: 组 A4 FAIL——spark frame ("
                          << tokenSpark->frame.x << "," << tokenSpark->frame.y << " "
                          << tokenSpark->frame.width << "x" << tokenSpark->frame.height
                          << ") 与数字带 (" << tokenNumber->frame.x << ","
                          << tokenNumber->frame.y << " " << tokenNumber->frame.width
                          << "x" << tokenNumber->frame.height
                          << ") 不一致（spark 应垫在数字同一带、占满内容宽）\n";
                passed = false;
            }
            if (framesIntersect(*tokenSpark, *tokenLabel)) {
                std::cerr << "home_probes: 组 A5 FAIL——spark 盖进了 label 带\n";
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
    // label（badge 并入 label 文本「label · badge」后为两行结构），相对当前元素 frame 度量，是弱判别；
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
            for (const char* suffix : {"number", "label"}) {
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
    // （kHomeChartH 260→220）+ 快捷按钮行 44+16 底边距 = 100% 档 476：600/640
    // 档**零滚动**（滚动条元素不渲染）；内容超出只发生在 115%/130% 字号档或低于
    // 真实最小页高的退化矮窗——那才是滚动吸收机制要守的行为，故新加 460 守护档
    // 让机制仍被真实验证。五档（screen 高 = 页区高口径）：
    //   · 845（合成档，旧设计最小窗值，专测历史刀口档位）；
    //   · 778 旧 Windows@125% 有效下限（现高于 640 设计下限，普通深矮窗保留覆盖）；
    //   · 640 设计最小窗 = macOS 真实页高（无自绘壳；Windows 真实页高 600）；
    //   · 600 Windows 自绘壳真实最小页高（640−40 标题条）= 最紧真实档，零滚动契约档；
    //   · 460 守护性深档（< 真实最小页高 600）：内容高 476 > 视口，滚动吸收不丢内容。
    // 判据（每档一致）：
    //   K(a) 整页滚动容器在场（矮窗可见性依赖它吸收溢出）
    //   K(b) 既有内容一条不少（stats / chart / actions / root）
    //   K(c) 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
    //   K(e) 快捷按钮行距内容盒底 ≥ 16（防贴边底边距）
    //   K(d) 滚动条渲染契约：≥600 档不得出现（home.scroll.scroll 查无 = 零滚动），
    //        460 守护档必须在场（滚动吸收机制没被零滚动收口顺手拆掉）
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
            {460.0f, "1000×460 守护性深档（< 真实最小页高 600；内容高 476 > 视口，滚动吸收不丢内容）"},
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
            //（零滚动契约），460 守护档必须在场（滚动吸收机制被真实验证）。
            if (tier.height >= 600.0f) {
                if (scrollbar != nullptr) {
                    std::cerr << "home_probes: 组 K(d) FAIL（" << tier.label
                              << "）——页高 ≥ 真实最小页高 600 仍出滚动条"
                                 "（内容高 476 ≤ 视口应零滚动；图表卡高度回涨？）\n";
                    passed = false;
                }
            } else if (scrollbar == nullptr) {
                std::cerr << "home_probes: 组 K(d) FAIL（" << tier.label
                          << "）——低于真实最小页高却无滚动条（内容高 476 > 视口，"
                             "home.scroll 吸收机制被拆掉？）\n";
                passed = false;
            }
        }
    }

    // 组 L（B7 D1）：5 张统计卡正常态只留短标签（无「· 说明」后缀）；
    // 失败态专门造一次（不 onStart 的模块 = statsLoaded_ false）验证「加载失败」仍显形。
    {
        for (const char* cardId : kStatCardIds) {
            const core::dsl::Element* el =
                ui != nullptr ? ui->find(std::string(cardId) + ".label")
                              : nullptr;
            if (el == nullptr) {
                std::cerr << "home_probes: 组 L 结构缺失 '" << cardId
                          << ".label'\n";
                passed = false;
                continue;
            }
            if (el->text.find(" · ") != std::string::npos) {
                std::cerr << "home_probes: 组 L FAIL——'" << cardId
                          << ".label' 正常态仍是「" << el->text
                          << "」（应只留指标名，B7 D1）\n";
                passed = false;
            }
        }

        // 失败态：新模块实例不调 onStart → service_ 空、statsLoaded_ false
        std::unique_ptr<::core::Module> failedMod(create_home_module());
        const eui::Screen failScreen{1000.0f, 800.0f};
        eui::Ui failUi;
        failUi.begin("home_probe_l_failed");
        failedMod->compose(failUi, failScreen);
        failUi.end();
        failUi.layout(failScreen.width, failScreen.height);
        // 失败态：每张卡都必须显形（不是「至少一张」——短标签卡能放下会掩盖
        // 长标签卡把 badge 丢掉：池117 的速率卡就是这样漏过来的）。
        bool allFailedShown = true;
        for (const char* cardId : kStatCardIds) {
            const core::dsl::Element* el =
                failUi.find(std::string(cardId) + ".label");
            if (el == nullptr) {
                std::cerr << "home_probes: 组 L 失败态结构缺失 '" << cardId
                          << ".label'\n";
                passed = false;
                allFailedShown = false;
                continue;
            }
            if (el->text.find("加载失败") == std::string::npos) {
                std::cerr << "home_probes: 组 L FAIL——失败态 '" << cardId
                          << ".label' 文本为「" << el->text
                          << "」，丢了「加载失败」（失败必须显形，池117）\n";
                passed = false;
                allFailedShown = false;
            }
        }
        (void)allFailedShown;
    }

    // 组 M（B7 D2）：点标题行 → 折叠态卡高 64×字号缩放、标题改短且垂直居中；
    // 再点一次恢复展开态（不留副作用给收尾截图）。
    {
        auto clickCenter = [&](const core::dsl::Element& el) {
            const float cx = el.frame.x + el.frame.width * 0.5f;
            const float cy = el.frame.y + el.frame.height * 0.5f;
            core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                     core::PointerAction::Press, {});
            core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                     core::PointerAction::Release, {});
        };
        const auto* headHit =
            ui != nullptr ? ui->find("home.api.chart.headhit") : nullptr;
        const auto* chartExpanded =
            ui != nullptr ? ui->find("home.api.chart") : nullptr;
        if (headHit == nullptr || chartExpanded == nullptr) {
            std::cerr << "home_probes: 组 M 结构缺失（headhit/chart）\n";
            passed = false;
        } else {
            const float expandedH = chartExpanded->frame.height;
            clickCenter(*headHit);
            composeFrame();
            composeFrame();
            const auto* chart =
                ui != nullptr ? ui->find("home.api.chart") : nullptr;
            const auto* title =
                ui != nullptr ? ui->find("home.api.chart.title") : nullptr;
            const float expectH = 64.0f * core::theme::fontScale();
            if (chart == nullptr || title == nullptr) {
                std::cerr << "home_probes: 组 M 折叠后结构缺失\n";
                passed = false;
            } else {
                if (std::fabs(chart->frame.height - expectH) > 0.5f) {
                    std::cerr << "home_probes: 组 M FAIL——折叠态卡高 "
                              << chart->frame.height << " 期望 " << expectH
                              << "（kHomeChartCollapsedH 64×字号缩放）\n";
                    passed = false;
                }
                if (title->text != "今日 Token 分布") {
                    std::cerr << "home_probes: 组 M FAIL——折叠态标题「"
                              << title->text << "」，应短文案「今日 Token 分布」\n";
                    passed = false;
                }
                // 垂直居中：标题盒中心 = 卡内容区中心（卡上下各 16 内边距）
                const float titleCenter = title->frame.y + title->frame.height * 0.5f;
                const float contentCenter =
                    chart->frame.y + chart->frame.height * 0.5f;
                if (std::fabs(titleCenter - contentCenter) > 1.0f) {
                    std::cerr << "home_probes: 组 M FAIL——折叠态标题中心 "
                              << titleCenter << " 与卡中心 " << contentCenter
                              << " 偏差 > 1px（应垂直居中）\n";
                    passed = false;
                }
                if (chart->frame.height >= expandedH) {
                    std::cerr << "home_probes: 组 M FAIL——折叠态卡高未低于展开态（"
                              << chart->frame.height << " >= " << expandedH
                              << "）\n";
                    passed = false;
                }
            }
            // 折叠态落图（B7 D2 目视判据：上下留白相等 / 文案不截断）。
            // 收 argv 里的 --shot-collapsed <路径>；不给 = 零开销。
            for (int ai = 1; ai < argc; ++ai) {
                if (std::string(argv[ai]) == "--shot-collapsed" &&
                    ai + 1 < argc) {
                    probe::captureFramePng(window, runtime, argv[ai + 1]);
                }
            }
            // 恢复展开态：再点一次标题行（折叠态 headhit 仍在同位置）
            const auto* headHitAgain =
                ui != nullptr ? ui->find("home.api.chart.headhit") : nullptr;
            if (headHitAgain != nullptr) {
                clickCenter(*headHitAgain);
                composeFrame();
                composeFrame();
                const auto* restored =
                    ui != nullptr ? ui->find("home.api.chart") : nullptr;
                if (restored != nullptr &&
                    std::fabs(restored->frame.height - expandedH) > 0.5f) {
                    std::cerr << "home_probes: 组 M FAIL——再点后卡高未恢复（"
                              << restored->frame.height << " vs " << expandedH
                              << "）\n";
                    passed = false;
                }
            } else {
                std::cerr << "home_probes: 组 M 折叠态 headhit 丢失（无法恢复）\n";
                passed = false;
            }
        }
    }

    // 组 N（池72/池73 防复发）
    //   N1 有数据时图表必须真有柱体：种子（9 时 400 / 17 时 600）→ 去头去尾 9 根柱，
    //      bar.0 与 bar.8 高度 > 1px（1px 是零值底线，等于没画）。旧诊断口径是
    //      「结构缺失/plot 区高」，只要不撞空态分支就绿——挡不住「有数据走空态」。
    //   N2 「切换 Claude Profile」按钮图标不是缺字形方块：像素通道采样图标盒，
    //      判据 = 「满行占比」（字形包围盒内宽度 > 85% 的行 / 总行数）。
    //      ⚠ 不用 G3 的 ink/green 判据——实测缺字形的实心方块 ink=96.9%、
    //      green=61.3%，真实字形（0xF007 fa-user）ink=99.3%、green=43.9%，
    //      两者都满足「有墨迹且非绿主导」，判据不区分（本组实测过，会假绿）。
    //      换成形状分布：实心方块是矩形，65% 的行顶满包围盒宽；真实字形笔画
    //      收放，满行只占 32%。阈值 55%。
    {
        composeFrame();
        // ---- N1 有数据必有柱 ----
        const core::dsl::Element* bar0 =
            ui != nullptr ? ui->find("home.api.chart.bars.bar.0") : nullptr;
        const core::dsl::Element* bar8 =
            ui != nullptr ? ui->find("home.api.chart.bars.bar.8") : nullptr;
        const core::dsl::Element* emptyTitle =
            ui != nullptr ? ui->find("home.api.chart.empty.title") : nullptr;
        if (emptyTitle != nullptr) {
            std::cerr << "home_probes: 组 N1 FAIL——种子有 token 却走了空态分支"
                         "（图表不画图）\n";
            passed = false;
        } else if (bar0 == nullptr || bar8 == nullptr) {
            std::cerr << "home_probes: 组 N1 FAIL——柱体元素缺失 (bar.0="
                      << (bar0 != nullptr ? "ok" : "null")
                      << ", bar.8=" << (bar8 != nullptr ? "ok" : "null")
                      << ")——有数据仍不画图\n";
            passed = false;
        } else if (bar0->frame.height <= 1.5f || bar8->frame.height <= 1.5f) {
            std::cerr << "home_probes: 组 N1 FAIL——有数据但柱高塌成零底线 (bar.0="
                      << bar0->frame.height << ", bar.8=" << bar8->frame.height
                      << ")\n";
            passed = false;
        } else {
            std::cout << "home_probes: 组 N1 PASS 柱高 bar.0="
                      << bar0->frame.height << " bar.8=" << bar8->frame.height
                      << "\n";
        }

        // ---- N2 图标字形存在性（形状分布判据）----
        const core::dsl::Element* iconEl =
            ui != nullptr ? ui->find("home.act.claude.icon") : nullptr;
        if (iconEl == nullptr) {
            std::cerr << "home_probes: 组 N2 图标元素缺失 home.act.claude.icon"
                         "（结构判据跳过像素）\n";
            passed = false;
        } else if (iconEl->fontFamily != "Icon" || iconEl->text.empty()) {
            std::cerr << "home_probes: 组 N2 FAIL——图标未绑定 Icon 字体 (family="
                      << iconEl->fontFamily << ", text.len="
                      << iconEl->text.size() << ")\n";
            passed = false;
        } else {
            probe::FrameCapture glyphCapture(window);
            std::vector<unsigned char> rgba;
            int imgW = 0, imgH = 0;
            if (!glyphCapture.ok()) {
                std::cerr << "home_probes: 组 N2 FrameCapture 不可用: "
                          << glyphCapture.error() << "（像素判据跳过）\n";
            } else if (!glyphCapture.readFrame(runtime, rgba, imgW, imgH)) {
                std::cerr << "home_probes: 组 N2 读帧失败（像素判据跳过）\n";
            } else {
                const float pxScale = glyphCapture.scale();
                // frame 是内容绝对坐标，滚动位移只作用在绘制变换
                //（scrollTransformForElement），不在 frame 上——采样坐标 = 内容 y
                // − 当前滚动偏移。页面默认在顶部（偏移 0）；小屏/高 DPI 下窗口被系统
                // 钳制、图标落在视口外时，先滚到底再采样（setScrollOffset 帧末消费并
                // clamp 到 maxOffset），采样完滚回顶部还原页面状态。
                float scrollY = 0.0f;
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                auto measureIconBox = [&]() -> bool {
                    const core::dsl::Element* el =
                        ui != nullptr ? ui->find("home.act.claude.icon") : nullptr;
                    if (el == nullptr) return false;
                    const float viewY = el->frame.y - scrollY;
                    x0 = static_cast<int>(std::lround(el->frame.x * pxScale));
                    y0 = static_cast<int>(std::lround(viewY * pxScale));
                    x1 = static_cast<int>(std::lround(
                        (el->frame.x + el->frame.width) * pxScale));
                    y1 = static_cast<int>(std::lround(
                        (viewY + el->frame.height) * pxScale));
                    return true;
                };
                bool boxOk = measureIconBox();
                if (boxOk && (y0 < 0 || y1 > imgH)) {
                    const core::dsl::Element* scrollEl =
                        ui != nullptr ? ui->find("home.scroll") : nullptr;
                    const float maxOffset =
                        scrollEl != nullptr ? scrollEl->scrollMaxOffset : 0.0f;
                    if (maxOffset > 0.0f) {
                        ui->setScrollOffset("home.scroll", maxOffset);
                        settleFrames(3);
                        scrollY = maxOffset;
                        boxOk = measureIconBox();
                        if (boxOk) {
                            glyphCapture.readFrame(runtime, rgba, imgW, imgH);
                        }
                    }
                }
                if (!boxOk) {
                    std::cerr << "home_probes: 组 N2 重排后图标元素丢失"
                                 "（像素判据无法进行）\n";
                    passed = false;
                } else if (x0 < 0 || y0 < 0 || x1 > imgW || y1 > imgH ||
                           x1 <= x0 || y1 <= y0) {
                    // 区分「窗口被系统钳小」与「真布局 bug」：前者是显示器装不下请求的
                    // 逻辑窗高（帧高/scale < 请求高），属环境不可用 → 77，不是红。
                    const float actualLogicalH =
                        pxScale > 0.0f ? static_cast<float>(imgH) / pxScale : 0.0f;
                    if (static_cast<float>(h) > actualLogicalH + 1.0f) {
                        std::cerr << "home_probes: 组 N2 环境不支持——请求 "
                                  << args.windowW << "x" << h
                                  << "，系统把窗口钳到 " << imgW << "x" << imgH
                                  << "px（逻辑高≈" << actualLogicalH
                                  << "，显示器装不下）；像素判据跳过，返回 77\n";
                        envSkipped = true;
                    } else {
                        std::cerr << "home_probes: 组 N2 采样越界 (" << x0 << ","
                                  << y0 << ")-(" << x1 << "," << y1 << ")"
                                  << " img=" << imgW << "x" << imgH
                                  << " scrollY=" << scrollY << "\n";
                        passed = false;
                    }
                } else {
                    // 字形笔画色 = 主题 primary 绿：g 明显高于 r/b（按钮 ghost 态图标色）
                    std::vector<std::pair<int, int>> glyphPx;
                    for (int y = y0; y < y1; ++y) {
                        for (int x = x0; x < x1; ++x) {
                            const std::size_t at =
                                (static_cast<std::size_t>(y) * imgW + x) * 4u;
                            const int r = rgba[at], g = rgba[at + 1],
                                      b = rgba[at + 2];
                            if (g > 90 && g > r + 30 && g > b + 20) {
                                glyphPx.emplace_back(x, y);
                            }
                        }
                    }
                    if (glyphPx.empty()) {
                        std::cerr << "home_probes: 组 N2 FAIL——图标区无笔画像素"
                                     "（缺字形/未渲染）\n";
                        passed = false;
                    } else {
                        int minX = glyphPx[0].first, maxX = glyphPx[0].first;
                        int minY = glyphPx[0].second, maxY = glyphPx[0].second;
                        for (const auto& p : glyphPx) {
                            minX = std::min(minX, p.first);
                            maxX = std::max(maxX, p.first);
                            minY = std::min(minY, p.second);
                            maxY = std::max(maxY, p.second);
                        }
                        const int bboxW = maxX - minX + 1;
                        int rows = 0, fullRows = 0;
                        for (int y = minY; y <= maxY; ++y) {
                            int cnt = 0;
                            for (const auto& p : glyphPx) {
                                if (p.second == y) ++cnt;
                            }
                            if (cnt == 0) continue;
                            ++rows;
                            if (static_cast<float>(cnt) >
                                static_cast<float>(bboxW) * 0.85f) {
                                ++fullRows;
                            }
                        }
                        const float fullRatio =
                            rows > 0 ? static_cast<float>(fullRows) /
                                           static_cast<float>(rows)
                                     : 1.0f;
                        std::cout << "home_probes: 组 N2 字形 包围盒=" << bboxW
                                  << "x" << (maxY - minY + 1) << " 满行占比="
                                  << fullRatio * 100.0f << "%\n";
                        if (fullRatio > 0.55f) {
                            std::cerr << "home_probes: 组 N2 FAIL——图标是实心方块"
                                         "（满行占比 "
                                      << fullRatio * 100.0f
                                      << "% > 55%，缺字形 fallback）\n";
                            passed = false;
                        }
                    }
                }
                // 采样期间为看图标滚过页尾：滚回顶部，收尾截图仍是首页常态
                if (scrollY > 0.0f) {
                    ui->setScrollOffset("home.scroll", 0.0f);
                    settleFrames(2);
                }
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
    return passed ? (envSkipped ? 77 : 0) : 1;
}