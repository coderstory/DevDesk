// home_probes.cpp — DevDesk probes：home 模块 6 组断言
//（spec docs/superpowers/specs/2026-09-28-devdesk-probes-design.md
//  §3-probe-full 的 home_probes 组，类 1b/2a/2b/4a/5a/6a）
//
// 组 A（类 1b sparkline z，P1）：sparkline 根 frame 与 label frame 不相交。
//   修复 046beaa：sparkline 固定进卡内顶带（y0..16 = label 16 之上，整盒高 16），
//   旧 32px 盒以 (0,0) 构建叠 label 文字带（home_module.cpp:156-164 注释）；
//   歧义记录：spec 写的「gap≥2px」系实现前估量——修复落地形态为顶带与 label
//   [16,32] **邻接**（gap=0），断言按修复后不变式 = 不相交 + 顶带严格在 label
//   之上（gap≥0，容差 0.5px）；回归 32 盒 → 相交 → 必红。
//   曲线段（linechart.h:147-167）空闲态 {0.0f} 零长度段被跳过（:150-152），
//   且平线贴 bottomY 时胶囊半宽会把段 frame 探进 label 带 → 不以段为断言。
//
// 组 B（类 2a 空转重绘，P1）：静止 UI 不请求重绘。
//   修复 046beaa：周期 onTimer 仅「有速率活动」时挂 1s（home_module.cpp:462-477
//   的 seriesLive/tokenLive 门控 + :147-154 挂载点）——旧 0.5s 常挂 onTimer →
//   整页 2Hz 重排重绘 + hover 0.1s 过渡反复打断（用户报「5 卡 hover 闪」）。
//   断言 = 稳定后连跑 60 帧 pollOnce，runtime.update() 返 true（= 帧末返回
//   paintRequested_，runtime_lifecycle.h:229-233）的帧数 ≤ 1；回归常驻 timer →
//   60 帧（1s）内 ≥2 → 红。
//
// 组 C（类 2b hover token，P1）：colors.hover != colors.surface。
//   修复 046beaa：浅色档 hover 由 #F5F2EC（== surface）降一步 #EDE8DF
//   （theme.cpp:59-64）——Home 卡 .press hover 态在浅色档曾完全不可见。
//   深色档（默认 current()）+ 浅色档各验一次，验完还原深色。
//
// 组 D（类 4a barChart 几何，P1）：bar 面板 plot 区总高 ≥ 36px。
//   审核 #3/#4/#5 修：卡高 208→260（wrap 128→196），plot 16px→84px
//   （home_module.cpp:567-570；barchart.h:84-88 plotY=70、plotHeight=H-112）。
//   度量 = grid.3.y - grid.0.y（barchart.h:114-122 四条横线 y = plotY + k·H/3
//   → grid.0 在 plotY、grid.3 在 bottomY）——不依赖 bar 高度（有 0.16s 帧
//   过渡动画）；数据种入 1 条今日调用 → 非空态 → 图表必建。
//
// 组 E（类 5a API 卡对称，P3）：home.api.chart 卡内 title/wrap 左右间隙相等。
//   审核 #5 修：title/wrap 去二次 position(16,..)（旧 content 已含 inset → 左
//   32 右 0；home_module.cpp:570 注释）→ 卡 content inset 16 + (0,·) → 左右
//   各 16。断言 |左间隙 - 右间隙| ≤ 2px。
//
// 组 F（类 6a stat 溢出，P1）：最小窗（--size 1200x720）下统计数字/badge 不溢。
//   数据注入走 proxy_stats 直连（spec 搭法）：测试时钟 + 单流累计 40M →
//   2s 窗口 delta 20M → rateTps=10M → formatRateTps → 「10000000」（8 位）。
//   判别字段（spec「落点未定则 measureTextSize」分支）：修复实际落点 = **字号
//   收缩**（statNumberFont，home_module.cpp:34-42，8 位 36px ≈ 173 ≤ cw 176；
//   旧 48px 固定 ≈ 230 > 176 必溢），**未加 clip/wrap**（home_module.cpp:118-146
//   无 .clip()/.wrap() 调用）→ 断言两段式：clip||wrap 任一 true → 过；否则
//   TextPrimitive::measureTextSize（dsl.h:1729 同款）内在宽 ≤ 元素 frame 宽。
//   注意：badge 判据相对当前元素 frame（cw）度量，旧长文案按当前元素宽已能
//   收纳，属弱守卫；数字（8 位）才是强判别。
//
// 运行：home_probes.exe [--frames N] [--size 1200x720]（尺寸矩阵最小窗跑
// 1200x720；statW 只随窗**宽**变 → 各组几何在默认 1200x1000 下同型）。

#include "probes/probe_harness.h"
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "module_home/home_service.h"  // startOfDayEpoch（逐时 token 种子的本地零点锚）
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

// 统计卡 id 表（THS-08 元数据表 statCardElementId 同值五个）
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

    // 数据底座（组 D/E/F 前提）：已 migrate 的临时库注册为进程级
    // appDatabase（home onStart 拿它建 HomeService；null 则 statsError、
    // API 分布全零 → 分布图走空态分支不建图，组 D 结构前提失效）。
    // 种子 = 1 条 profile（claude_call_logs FK）+ 今日 2 条**带用量**的调用。
    // 2026-09-29 ③ 改口径后纵轴 = 每小时 output_tokens 合计：没有 output_tokens
    // 的行合计为 0（= 没数据）→ 依然走空态，组 D 的结构前提就没了，故这里必须
    // 给用量；两条落在 9 时与 17 时 → 去头去尾后恰 9 根柱（9..17 闭区间）。
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
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 树 +
    // layout；spec §4.2 成对要求）
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

    // ==================== 组 A：类 1(b) sparkline z（P1） ====================
    // home.stat.tokens.spark（lineChart chromeless 根，sparkwrap 顶带 y0..16，
    // home_module.cpp:159-164）必须整带位于 label（.label y16..32，
    // home_module.cpp:109-117）之上：不相交 + gapY ≥ 0（邻接语义；≥2 系
    // spec 实现前估量，见文件头）。回归旧 32px 盒 (0,0) → 相交 → 红。
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

    // ==================== 组 B：类 2(a) 空转重绘（P1） ====================
    // 稳定 15 帧（初始动画/hover 收敛，当前树结构与上帧一致不再 paint）后
    // 连跑 60 帧 pollOnce（不注入、不 recompose），计数 update() 返回 true
    // （= paintRequested_，runtime_lifecycle.h:229-233）。空闲态 tokenLive
    // false → 无 .timer（home_module.cpp:462-477）→ 期望 ≤1（首帧遗留）。
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

    // ==================== 组 C：类 2(b) hover token（P1） ====================
    // hover != surface：深色档（默认 current()）天然不等；浅色档是审计根因
    //（旧 #F5F2EC == surface #F5F2EC，theme.cpp:59-64 修后 #EDE8DF）。双档
    // 各验一次，验完还原深色（后续段几何不受主题影响，保持确定性）。
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

    // ==================== 组 D/E 前：API 分布卡当前帧 ====================
    composeFrame();

    // ==================== 组 D：类 4(a) barChart 几何（P1） ====================
    // plot 区总高 = grid.3.y - grid.0.y（barchart.h:114-122；band 不依赖 bar
    // 的 0.16s 帧过渡）。种子的非零用量 → 非空态 → 去头去尾后 9 根柱（9..17）
    // 正常建图（2026-09-29 ③ 改口径：纵轴 = 每小时 output_tokens 合计）；
    // home_module.cpp:567-570 审计修：卡高 260 → wrap 196 → plot 84 ≥ 36。
    // 回归卡高 208 → wrap 128 → plot 16 < 36 → 红。
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

    // ==================== 组 E：类 5(a) API 卡对称（P3） ====================
    // title/wrap 相对卡 content（inset 16）以 (0,·) 定位 → 左右间隙各 16，
    // |左 - 右| ≤ 2px（任务口径；master 实测差 0）。回归二次
    // position(16,..) → 左 32 右 0。
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

    // ==================== 组 F：类 6(a) stat 溢出（P1） ====================
    // 注入：测试时钟（每次 nowMs() +1000ms）+ 单流累计 20M→40M → 2s 窗口
    // delta 20M → rateTps=10M → 「10000000」（8 位）。慢 48px 档 → 内在宽
    // ≈230 > cw 176 必溢；statNumberFont（home_module.cpp:34-42）收缩到
    // 36px → ≈173 ≤ 176 收纳。badge = 流式档「● 1 路 · 累计 40M」。
    // 判别两段式（见文件头）：clip/wrap true → 过；否则 measureTextSize。
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
        const int kMinWindowW = 1200;  // 最小窗宽：statW 208 → cw 176（判别几何）
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

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}