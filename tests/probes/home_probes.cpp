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

    // 数据底座（统计卡数据前提）：已 migrate 的临时库注册为进程级 appDatabase。
    // null 则 statsError → 统计卡走失败态，组 A/L 结构前提失效。
    // 种子 = 1 条 profile（claude_call_logs FK）+ 今日 2 条**带用量**的调用（9 时
    // 400 / 17 时 600）：喂 Token 速率卡「最近一次成功调用」与调用数统计。
    test_support::TestEnv env("hprobe_", "home", true);  // 注册为 appDatabase
    {
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const int64_t dayStart = module::home::startOfDayEpoch(
            static_cast<std::time_t>(now));
        auto& sq = env.db->db();
        // 造数结果有意忽略：失败会在探针断言面暴露（need295）
        (void)sq.exec(
            "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
            "api_key_check, model, created_at, updated_at) "
            "VALUES ('p1', 'http://x', 'sk-***', 'chk', 'm', " +
            std::to_string(now) + ", " + std::to_string(now) + ")");
        (void)sq.exec(
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
    // bug395：断言一律按 compose 实际逻辑高（windowLogicalSize 反推，bug183 口径）
    // ——系统钳窗后实际高 ≠ 请求值 h（见下方 envSkipped 注释），按 h 算绝对高度
    // 期望必红（组 D 曾因用 h 中招）。
    float composeH = h;
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
                                      composeH = screen.height;
                                      mod->compose(u, screen);
                                  });
    };
    auto settleFrames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            composeFrame();
        }
    };

    settleFrames(2);  // 首帧构建树（结构 diff 计入 paint 预算，组 B 前消化）

    // 「种子在场」的中途落图（--screenshot-seeded <路径>）：统计卡种子全画着。
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

    // 组 D（C-2 need380 wave5-c）：今日 token 图表卡已砍——barChart 结构反向断言
    //（查有即红）；统计行吃余高：图表缺席后弹性基准以上余高全部摊给统计行
    //（home_module statsGrow），home.stats 高 = compose 实际视口高（composeH，
    // 非 --size 请求值，bug395）− 非统计固定件（页头 40 +
    // 列 gap 2×20 + 操作行 44+16 底边距 + 0.5 防浮点余量）。旧判据（plot 区高
    // ≥36）随卡下线。
    {
        if (ui != nullptr && ui->find("home.api.chart.bars.grid.0") != nullptr) {
            std::cerr << "home_probes: 组 D FAIL——今日 token 图表卡已砍（C-2），"
                         "barChart 结构仍在场\n";
            passed = false;
        }
        const core::dsl::Element* stats =
            ui != nullptr ? ui->find("home.stats") : nullptr;
        if (stats == nullptr) {
            std::cerr << "home_probes: 组 D 结构缺失 home.stats\n";
            passed = false;
        } else {
            // 统计卡限高（用户 2026-11-24 需求）：单卡封顶 kHomeStatMaxH=120
            //（随 fontScale 缩放），余高留白页底——替换旧「吃满余高」判据。
            // 下限 96*fontScale 基准防压缩；上限逐卡断言（行数随窗宽变，
            // 块高 = maxH*行数 + gap*(行数-1)，逐卡判免重算行数）。
            const float maxCardH = core::theme::scaled(120.0f) + 1.0f;
            const float minCardH = core::theme::scaled(96.0f) - 1.0f;
            for (const char* cardId : kStatCardIds) {
                const core::dsl::Element* card = ui->find(cardId);
                if (card == nullptr) continue;  // 结构在场性由组 A 覆盖
                if (card->frame.height > maxCardH ||
                    card->frame.height < minCardH) {
                    std::cerr << "home_probes: 组 D FAIL——统计卡 " << cardId
                              << " 高 " << card->frame.height
                              << " 越出 [" << minCardH << ", " << maxCardH
                              << "]（限高钳制回退？）\n";
                    passed = false;
                }
            }
        }
    }

    // 组 E（C-2 need380 wave5-c）：图表卡缺席的另一半——home.api.chart 全家
    //（card/title/wrap）查有即红。旧判据（卡内 title/wrap 左右对称）随卡下线。
    {
        for (const char* elId :
             {"home.api.chart", "home.api.chart.title", "home.api.chart.wrap"}) {
            if (ui != nullptr && ui->find(elId) != nullptr) {
                std::cerr << "home_probes: 组 E FAIL——'" << elId
                          << "' 图表卡结构已砍（C-2）却仍在场\n";
                passed = false;
            }
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
    // 600。C-2（need380 wave5-c）图表卡已砍后，100% 字号档弹性基准 = 页头 40 +
    // 统计行 96 + 列 gap 2×20 + 操作行 44+16 底边距 = 236；基准以上余高**全部**
    // 摊给统计行（statsGrow）→ 各档视口 ≥236 时内容正好贴合视口、**零滚动**；
    // 内容超出只发生在低于弹性基准 236 的退化矮窗——那才是滚动吸收机制要守的
    // 行为，故守护档取 200。五档（screen 高 = 页区高口径）：
    //   · 845（合成档，旧设计最小窗值，专测历史刀口档位）；
    //   · 778 旧 Windows@125% 有效下限（现高于 640 设计下限，普通深矮窗保留覆盖）；
    //   · 640 设计最小窗 = macOS 真实页高（无自绘壳；Windows 真实页高 600）；
    //   · 600 Windows 自绘壳真实最小页高（640−40 标题条）= 最紧真实档，零滚动契约档；
    //   · 200 守护性深档（< 弹性基准 236）：内容 236 > 视口，滚动吸收不丢内容。
    // 判据（每档一致）：
    //   K(a) 整页滚动容器在场（矮窗可见性依赖它吸收溢出）
    //   K(b) 既有内容一条不少（stats / actions / root）且图表卡缺席（C-2 反向断言）
    //   K(c) 快捷按钮行仍在滚动内容盒内（滚到底够得着 = 没丢内容）
    //   K(e) 快捷按钮行距内容盒底 ≥ 16（防贴边底边距）
    //   K(d) 滚动条渲染契约：≥600 档不得出现（home.scroll.scroll 查无 = 零滚动），
    //        200 守护档必须在场（滚动吸收机制没被零滚动收口顺手拆掉）
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
            {200.0f, "1000×200 守护性深档（< 弹性基准 236；内容 236 > 视口，滚动吸收不丢内容）"},
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
            if (pageScroll == nullptr || stats == nullptr || chart != nullptr ||
                acts == nullptr || root == nullptr) {
                std::cerr << "home_probes: 组 K(a/b) FAIL（" << tier.label
                          << "）——结构不符 (scrollContent="
                          << (pageScroll != nullptr ? ok : no)
                          << ", stats=" << (stats != nullptr ? ok : no)
                          << ", chart=" << (chart != nullptr ? "在场(应缺席)" : no)
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
                                 "（弹性基准 236 ≤ 视口、余高全归统计行，应零滚动；"
                                 "统计行没吃满余高？）\n";
                    passed = false;
                }
            } else if (scrollbar == nullptr) {
                std::cerr << "home_probes: 组 K(d) FAIL（" << tier.label
                          << "）——低于弹性基准 236 却无滚动条（内容 236 > 视口，"
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

    // 组 M（C-2 need380 wave5-c）：图表卡随折叠能力整体下线——headhit 查有即红
    //（旧判据：点标题折叠 64×字号缩放/标题短文案居中/再点恢复，随卡下线；
    // --shot-collapsed 折叠态落图开关一并移除）。
    {
        if (ui != nullptr && ui->find("home.api.chart.headhit") != nullptr) {
            std::cerr << "home_probes: 组 M FAIL——图表卡 headhit 已砍（C-2），"
                         "折叠命中区仍在场\n";
            passed = false;
        }
    }

    // 组 N（池72/池73 防复发 → C-2 need380 wave5-c 改反向判据）
    //   N1 今日 token 图表卡（bars.bar.* / empty.*）查有即红——C-2 砍卡，小时
    //      分布入口收敛到 Claude 页（种子数据仍喂统计卡，前置种子保留）。
    //   N2 「切换 Claude Profile」快捷按钮（home.act.claude / .icon）查有即红——
    //      C-2 砍按钮（与 Claude 页右键「设为当前」重复）；旧像素字形判据
    //     （满行占比防缺字形方块）随按钮下线。
    {
        for (const char* elId :
             {"home.api.chart.bars.bar.0", "home.api.chart.empty.title",
             "home.act.claude", "home.act.claude.icon"}) {
            if (ui != nullptr && ui->find(elId) != nullptr) {
                std::cerr << "home_probes: 组 N FAIL——'" << elId
                          << "' 已砍结构（C-2）仍出现在元素树\n";
                passed = false;
            }
        }
        composeFrame();  // 反向断言后照常落一帧：后续收尾截图仍是首页常态
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