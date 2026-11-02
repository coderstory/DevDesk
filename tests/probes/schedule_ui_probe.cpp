// schedule_ui_probe.cpp — 定时页编辑/删除入口的真窗口探针 + 构图留痕（R3）
//
// R3（编辑弹窗接线）的 A1/A4 真窗口证据：无头单测断言了节点存在与落库语义，
// 但「新建按钮 / 行级编辑与删除 / 删除二次确认弹窗 / 编辑弹窗」从未有过真实
// 渲染帧。本探针用真窗口 + 真命中分发驱动三个状态：
//   A. 列表态：种子 2 条用户规则，断言 header「新建」、用户行 .edit/.delete/.switch
//      齐全、系统行零操作控件（.edit/.delete 不存在）、系统段锁图标标题存在；
//      --shot-list <png> 落列表帧。
//   B. 编辑弹窗态：点「新建」→ 断言 schedule.edit.name/save/cancel 存在（关闭态
//      常驻构建， existence 不够，需确认 dialog 开启后输入框仍可命中且按钮帧
//      在窗口内）；--shot-edit <png> 落弹窗帧。
//   C. 删除确认态：点行「删除」→ 断言 deleteConfirm 弹窗节点齐全（标题/正文/
//      purge 开关/取消/删除）；点「取消」关弹窗且行还在；再点「删除」→ 点
//      「删除」确认 → store.getRule 返回 NotFound（真落库验证，默认软删路径）；
//      --shot-delete <png> 落确认弹窗帧。
//   D. 运行历史抽屉态（2026-11-01）：种子 3 条 run（成功/失败/错过）+ 渠道投递，
//      点页头「运行历史」→ 断言抽屉卡/状态点/渠道 chip/重试钮齐全，并断言
//      **三个按钮右边界同基准**（右上 × / 行内重试 / 底部关闭 = 内容列右基准）；
//      --shot-history <png> 落抽屉帧。
//
// 种子走真 ScheduleStore::createRule 落库口（不绕产品代码写表）。模块经
// core::appDatabase() 取连接 → TestEnv 以 registerAppDb=true 注册。
//
// 运行：schedule_ui_probe [--shot-list <png>] [--shot-edit <png>] [--shot-delete <png>]
//                            [--shot-history <png>]
// 不带构图开关 = 纯断言门禁（ctest -L probe 跑的就是这条）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 agent_dialog_probe）
#include "test_support.h"

#include "core/time.h"
#include "module_schedule/schedule_module.h"
#include "module_schedule/schedule_store.h"
#include "ui_kit/materials.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_schedule_ui_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

bool shotRequested(const std::string& path) { return !path.empty(); }

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    std::string shotList, shotEdit, shotDelete, shotHistory;
    // 2026-11-01：浅色主题档（--light）与空历史档（--empty）——运行历史抽屉
    // 两套主题都要看过，空态是独立形态（不能拿有数据态推断）。
    // 窗口效果档不在本探针提供：半透/磨砂实现在 AppKit 层，离屏帧缓冲读不到，
    // 观感一律走真机系统截图（见 docs/design/2026-11-01-窗口效果档验收与覆盖核对.md）；
    // 档位切换后的布局回归用 uat62_page_shots_probe --window-effect。
    // 2026-11-02 B4：`--empty-rules` = 用户定时空态档（不播种规则），
    // `--shot-empty` = 该档截图出口（S1 卡化与按钮降级的空态证据）。
    bool light = false;
    bool emptyHistory = false;
    bool emptyRules = false;
    std::string shotEmpty;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-list" && i + 1 < argc) {
            shotList = argv[++i];
        } else if (arg == "--shot-empty" && i + 1 < argc) {
            shotEmpty = argv[++i];
        } else if (arg == "--shot-edit" && i + 1 < argc) {
            shotEdit = argv[++i];
        } else if (arg == "--shot-delete" && i + 1 < argc) {
            shotDelete = argv[++i];
        } else if (arg == "--shot-history" && i + 1 < argc) {
            shotHistory = argv[++i];
        } else if (arg == "--light") {
            light = true;
        } else if (arg == "--empty") {
            emptyHistory = true;
        } else if (arg == "--empty-rules") {
            emptyRules = true;
        }
    }

    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 注册为进程级 appDatabase：ScheduleModule::onStart 经 core::appDatabase() 取连接
    test_support::TestEnv tdb("sprobe_", "schedui", /*registerAppDb=*/true);

    // 种子（真 ScheduleStore 落库口）：2 条用户一次性规则（未来 1 小时 / 2 小时，
    // 不派发——probe 只渲染不跑调度断言）
    {
        module::schedule::ScheduleStore store(tdb.handle());
        const int64_t now = static_cast<int64_t>(core::time::nowUnix());
        // --empty-rules：不播规则（用户定时空态档）；ruleId 保 0 供历史种子跳过
        int64_t ruleId = 0;
        if (!emptyRules) {
        module::schedule::RuleRecord r1;
        r1.name = "清理临时文件";
        r1.kind = "once";
        r1.specJson = "{\"at\":" + std::to_string(now + 3600) + "}";
        r1.actionKind = "remind";
        r1.actionJson = "{}";
        r1.missedPolicy = "once";
        auto r1res = store.createRule(r1);
        if (r1res.isFail()) {
            std::cerr << "schedule_ui_probe: 种子 createRule(1) 失败\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        ruleId = r1res.value().id;
        module::schedule::RuleRecord r2;
        r2.name = "日报草稿提醒";
        r2.kind = "once";
        r2.specJson = "{\"at\":" + std::to_string(now + 7200) + "}";
        r2.actionKind = "remind";
        r2.actionJson = "{}";
        r2.missedPolicy = "once";
        auto r2res = store.createRule(r2);
        if (r2res.isFail()) {
            std::cerr << "schedule_ui_probe: 种子 createRule(2) 失败\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        }  // if (!emptyRules)

        // 渠道配置（运行历史抽屉的渠道 chip 用它显示可读名）
        module::schedule::ChannelRecord ch;
        ch.kind = "wecom";
        ch.name = "";
        ch.enabled = true;
        if (store.createChannel(ch).isFail()) {
            std::cerr << "schedule_ui_probe: 种子 createChannel 失败\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }

        // 运行历史种子：成功 / 失败 / 错过 三条（planned_at 在过去，避免被引擎重复触发）
        const auto channels = store.listChannels();
        const int64_t channelId =
            (channels.isOk() && !channels.value().empty())
                ? channels.value().front().id
                : 0;
        const int64_t base = now - 3600;
        auto seedRun = [&](int64_t offset, const std::string& status,
                           const std::string& error) {
            module::schedule::RunRecord run;
            run.origin = "user";
            run.originKey = "rule:" + std::to_string(ruleId);
            run.ruleId = ruleId;
            run.plannedAt = base + offset;
            run.startedAt = base + offset + 3;
            run.endedAt = base + offset + 5;
            run.status = status;
            run.attempt = 1;
            run.durationMs = 2140;
            run.resultSummary = "已提醒";
            run.error = error;
            auto res = store.insertRun(run);
            if (res.isFail()) {
                std::cerr << "schedule_ui_probe: 种子 insertRun 失败\n";
                return static_cast<int64_t>(0);
            }
            return res.value().id;
        };
        int64_t okRun = 0;
        int64_t failRun = 0;
        if (!emptyHistory && !emptyRules) {
            okRun = seedRun(0, "success", "");
            failRun = seedRun(600, "failed", "企业微信接口返回 93000");
            seedRun(1200, "missed", "");
        }
        if (channelId != 0 && !emptyHistory) {
            module::schedule::DeliveryRecord d1;
            d1.runId = okRun;
            d1.channelId = channelId;
            d1.attempt = 0;
            d1.status = "success";
            store.insertDelivery(d1);
            module::schedule::DeliveryRecord d2;
            d2.runId = failRun;
            d2.channelId = channelId;
            d2.attempt = 0;
            d2.status = "failed";
            store.insertDelivery(d2);
            module::schedule::DeliveryRecord d3;
            d3.runId = failRun;
            d3.channelId = channelId;
            d3.attempt = 1;
            d3.status = "failed";
            store.insertDelivery(d3);
        }
    }

    module::schedule::ScheduleModule mod;
    mod.onStart();
    if (light) {
        core::theme::setLight();
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "schedule";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod.compose(u, screen);
                                  });
    };
    auto clickPoint = [&](float cx, float cy) {
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    auto clickElement = [&](const core::dsl::Element& element) {
        clickPoint(element.frame.x + element.frame.width * 0.5f,
                   element.frame.y + element.frame.height * 0.5f);
    };
    // recompose 重建元素树 → 开前捕获的指针全部作废，逐帧重查
    auto findElement = [&](const std::string& id) -> const core::dsl::Element* {
        return ui != nullptr ? ui->find(id) : nullptr;
    };
    auto expectNode = [&](const std::string& id, bool* ok) -> const core::dsl::Element* {
        const auto* el = findElement(id);
        if (el == nullptr) {
            std::cerr << "schedule_ui_probe: 期望节点缺失 '" << id << "'\n";
            *ok = false;
        }
        return el;
    };
    auto expectNoNode = [&](const std::string& id, bool* ok) {
        if (findElement(id) != nullptr) {
            std::cerr << "schedule_ui_probe: 期望节点不存在却命中 '" << id << "'\n";
            *ok = false;
        }
    };
    auto shot = [&](const std::string& path) {
        if (!shotRequested(path)) return;
        if (!probe::captureFramePng(window, runtime, path)) {
            std::cerr << "schedule_ui_probe: 落图失败 " << path << "\n";
            passed = false;
        }
        // 落图内部 pollOnce 会重建元素树，之后所有 find 重新 composeFrame
    };

    // 颜色等同（写入 element 的目标值比对，不是像素采样）
    auto sameColor = [](const eui::Color& a, const eui::Color& b) {
        return std::fabs(a.r - b.r) < 0.02f && std::fabs(a.g - b.g) < 0.02f &&
               std::fabs(a.b - b.b) < 0.02f && std::fabs(a.a - b.a) < 0.02f;
    };
    // 按钮级判定：ui_kit::button 把 level 配色写进 <id>.bg 的三态 normal 色
    // → Primary = t.colors.primary，Secondary = t.colors.hover（button.cpp 配色表）
    auto buttonLevelIs = [&](const std::string& id, bool primary) {
        const core::dsl::Element* bg = findElement(id + ".bg");
        if (bg == nullptr) return false;
        const auto& t = core::theme::current();
        return sameColor(bg->color, primary ? t.colors.primary : t.colors.hover);
    };

    // ---------- A. 列表态：四入口 + 系统段零操作控件 ----------
    composeFrame();

    if (emptyRules) {
        // ---------- S1（B4）：空态——页头钮降级 + 空态卡 L2 材质 ----------
        const auto& t = core::theme::current();
        if (!buttonLevelIs("schedule.list.new", /*primary=*/false)) {
            std::cerr << "schedule_ui_probe: S1 FAIL 空态页头新建钮不是 Secondary\n";
            passed = false;
        }
        if (!buttonLevelIs("schedule.list.user.empty.action", /*primary=*/false)) {
            std::cerr << "schedule_ui_probe: S1 FAIL 空态卡中央钮不是 Secondary\n";
            passed = false;
        }
        // 全页唯一绿钮不存在：只扫列表页子树（弹窗关闭态也常驻构建、自带 Primary
        // 保存钮，扫全树会把看不见的弹窗按钮算进来）
        int primaryFills = 0;
        std::function<void(const core::dsl::Element&)> walk =
            [&](const core::dsl::Element& el) {
                if (el.kind == core::dsl::ElementKind::Rect &&
                    sameColor(el.color, t.colors.primary)) {
                    std::cerr << "schedule_ui_probe: S1 发现绿底矩形 " << el.id
                              << "\n";
                    ++primaryFills;
                }
                for (const auto& child : el.children) {
                    if (child != nullptr) walk(*child);
                }
            };
        if (const auto* listRoot = findElement("schedule.list.scroll")) {
            walk(*listRoot);
        } else {
            std::cerr << "schedule_ui_probe: S1 FAIL 列表根节点缺失\n";
            passed = false;
        }
        if (primaryFills != 0) {
            std::cerr << "schedule_ui_probe: S1 FAIL 空态仍存在 " << primaryFills
                      << " 个主色填充（应 0）\n";
            passed = false;
        }
        // 空态卡 L2 材质：卡面写入 itemSurface（半透玻璃底），且与页面底像素有差
        const auto* card = expectNode("schedule.list.user.empty.card", &passed);
        if (card != nullptr && !sameColor(card->color, ui_kit::itemSurface(t))) {
            std::cerr << "schedule_ui_probe: S1 FAIL 空态卡未用 L2 itemSurface\n";
            passed = false;
        }
        // 采样先于落图：captureFramePng 会 pollOnce 重建元素树，之后上面拿到的
        // 元素指针即作废（读到的是旧帧几何）。同理采样前也先跑稳帧，否则读到的是
        // 首帧（布局刚落、渲染尚未跟上）的画面。
        for (int i = 0; i < 8; ++i) composeFrame();
        const auto* sampleCard = findElement("schedule.list.user.empty.card");
        if (sampleCard != nullptr) {
            probe::FrameCapture cap(window);
            std::vector<unsigned char> rgba;
            int iw = 0, ih = 0;
            if (!cap.ok() || !cap.readFrame(runtime, rgba, iw, ih)) {
                std::cerr << "schedule_ui_probe: S1 FAIL 读帧失败（材质采样）\n";
                passed = false;
            } else {
                const float scale = cap.scale();
                auto sample = [&](float lx, float ly) {
                    const int x = static_cast<int>(std::lround(lx * scale));
                    const int y = static_cast<int>(std::lround(ly * scale));
                    if (x < 0 || y < 0 || x >= iw || y >= ih) return -1.0f;
                    const std::size_t at =
                        (static_cast<std::size_t>(y) * iw + x) * 4u;
                    return static_cast<float>(rgba[at]) +
                           static_cast<float>(rgba[at + 1]) +
                           static_cast<float>(rgba[at + 2]);
                };
                // 卡内（左内 6px、下 6px——避文字/居中按钮/圆角）vs 卡外（左内缩
                // gutter 同高度）。逐点比而非区块均值：卡面与页面底近色。
                const float inX = sampleCard->frame.x + 6.0f;
                const float outX = std::max(1.0f, sampleCard->frame.x - 5.0f);
                const float lowY = sampleCard->frame.y + sampleCard->frame.height - 6.0f;
                const float inV = sample(inX, lowY);
                const float outV = sample(outX, lowY);
                std::cout << "S1 材质采样 卡内=" << inV << " 卡外=" << outV
                          << "（卡 @" << sampleCard->frame.x << "," << sampleCard->frame.y
                          << " " << sampleCard->frame.width << "x"
                          << sampleCard->frame.height << "）\n";
                if (inV < 0.0f || outV < 0.0f) {
                    std::cerr << "schedule_ui_probe: S1 FAIL 材质采样越界\n";
                    passed = false;
                } else if (std::fabs(inV - outV) < 3.0f) {
                    std::cerr << "schedule_ui_probe: S1 FAIL 卡内外像素无差（卡未透出层次）\n";
                    passed = false;
                }
            }
        }
        if (passed) std::cout << "S1 PASS 空态页头降级 + 空态卡 L2 材质\n";
        shot(shotEmpty);
        if (!passed) {
            std::cerr << "schedule_ui_probe: FAILED\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 4;
        }
        std::cout << "schedule_ui_probe: PASS\n";
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 0;
    }

    expectNode("schedule.list.new", &passed);
    expectNode("schedule.list.sys.title", &passed);
    expectNode("schedule.list.user.title", &passed);
    expectNode("schedule.list.user.0", &passed);
    expectNode("schedule.list.user.0.name", &passed);
    expectNode("schedule.list.user.0.edit", &passed);
    expectNode("schedule.list.user.0.delete", &passed);
    expectNode("schedule.list.user.0.switch", &passed);
    // 系统行只有锁图标与只读文案，零操作控件（A1 的「系统段无操作」判据）
    expectNoNode("schedule.list.sys.0.edit", &passed);
    expectNoNode("schedule.list.sys.0.delete", &passed);
    expectNoNode("schedule.list.sys.0.switch", &passed);
    // 操作按钮帧必须落在窗口客户区内（Windows 125% / 最小窗同判据的几何前提）
    if (const auto* del = findElement("schedule.list.user.0.delete")) {
        if (del->frame.x < 0 || del->frame.x + del->frame.width > w ||
            del->frame.y < 0 || del->frame.y + del->frame.height > h) {
            std::cerr << "schedule_ui_probe: 行删除操作帧越窗 ("
                      << del->frame.x << "," << del->frame.y << ")\n";
            passed = false;
        }
    }
    shot(shotList);

    // ---------- S2/S3（B4）：有数据态——页头 Primary + 每行一张 L2 卡 + 卡几何 ----------
    {
        const auto& t = core::theme::current();
        if (!buttonLevelIs("schedule.list.new", /*primary=*/true)) {
            std::cerr << "schedule_ui_probe: S2 FAIL 有数据时页头新建钮不是 Primary\n";
            passed = false;
        }
        const auto* c0 = expectNode("schedule.list.user.0.bg", &passed);
        const auto* c1 = expectNode("schedule.list.user.1.bg", &passed);
        if (c0 != nullptr) {
            if (!sameColor(c0->color, ui_kit::itemSurface(t))) {
                std::cerr << "schedule_ui_probe: S2 FAIL 行卡未用 L2 itemSurface\n";
                passed = false;
            }
            if (c0->radius <= 0.0f) {
                std::cerr << "schedule_ui_probe: S2 FAIL 行卡无圆角（非 jellyCard 形态）\n";
                passed = false;
            }
        }
        // S3-1 各行卡 x/宽一致（±1.5）+ 卡宽 = 内容宽 − 2×inset（左右对称）
        if (c0 != nullptr && c1 != nullptr) {
            if (std::fabs(c0->frame.x - c1->frame.x) > 1.5f ||
                std::fabs(c0->frame.width - c1->frame.width) > 1.5f) {
                std::cerr << "schedule_ui_probe: S3 FAIL 行卡几何不一致 ("
                          << c0->frame.x << "," << c0->frame.width << ") vs ("
                          << c1->frame.x << "," << c1->frame.width << ")\n";
                passed = false;
            }
            // S3-2 相邻卡间距 = kRowCardGap(8)
            const float gap = c1->frame.y - (c0->frame.y + c0->frame.height);
            std::cout << "S3 相邻卡间距=" << gap << " 卡宽=" << c0->frame.width << "\n";
            if (std::fabs(gap - 8.0f) > 1.5f) {
                std::cerr << "schedule_ui_probe: S3 FAIL 相邻卡间距 " << gap
                          << "（期望 8）\n";
                passed = false;
            }
        }
        // S3-3 卡与段标题间距一致：系统段（空态卡）与用户段同口径
        const auto* sysTitle = findElement("schedule.list.sys.title");
        const auto* sysCard = findElement("schedule.list.sys.empty.card");
        const auto* userTitle = findElement("schedule.list.user.title");
        if (sysTitle != nullptr && sysCard != nullptr && userTitle != nullptr &&
            c0 != nullptr) {
            const float sysGap =
                sysCard->frame.y - (sysTitle->frame.y + sysTitle->frame.height);
            const float userGap =
                c0->frame.y - (userTitle->frame.y + userTitle->frame.height);
            std::cout << "S3 段标题→卡间距 系统=" << sysGap << " 用户=" << userGap
                      << "\n";
            if (std::fabs(sysGap - userGap) > 1.5f) {
                std::cerr << "schedule_ui_probe: S3 FAIL 段标题→卡间距不一致 ("
                          << sysGap << " vs " << userGap << ")\n";
                passed = false;
            }
        }
        // 卡宽与内容列对齐：卡右缘 + inset == 列右缘，卡左缘 == 列左缘 + inset
        const auto* rowsCol = findElement("schedule.list.user.rows");
        if (rowsCol != nullptr && c0 != nullptr) {
            const float colLeft = rowsCol->frame.x;
            const float colRight = rowsCol->frame.x + rowsCol->frame.width;
            const float insetL = c0->frame.x - colLeft;
            const float insetR = colRight - (c0->frame.x + c0->frame.width);
            if (std::fabs(insetL - insetR) > 1.5f) {
                std::cerr << "schedule_ui_probe: S3 FAIL 行卡左右内缩不对称 ("
                          << insetL << " vs " << insetR << ")\n";
                passed = false;
            }
        }
        if (passed) std::cout << "S2 PASS 有数据页头 Primary + 每行 L2 卡；S3 PASS 卡几何\n";

        // S4（F-67-1 hover 驱动）：指针进卡 → jellyCard 目标圆角变 hoverRadius，
        // 移出 → 回常态。读 compose 写入的 element.radius（结构性判据，非像素）。
        if (c0 != nullptr) {
            const float cx = c0->frame.x + c0->frame.width * 0.5f;
            const float cy = c0->frame.y + c0->frame.height * 0.5f;
            const core::KeyModifiers noMods;
            core::queuePointerMotion(window, cx, cy, core::PointerButtons{}, noMods);
            for (int i = 0; i < 6; ++i) composeFrame();
            const auto* hovBg = findElement("schedule.list.user.0.bg");
            const float hoverRadius = hovBg != nullptr ? hovBg->radius : -1.0f;
            const float hoverScale = hovBg != nullptr ? hovBg->transform.scale.x : -1.0f;
            core::queuePointerMotion(window, w - 8.0f, 8.0f, core::PointerButtons{},
                                     noMods);
            for (int i = 0; i < 6; ++i) composeFrame();
            const auto* outBg = findElement("schedule.list.user.0.bg");
            const float outRadius = outBg != nullptr ? outBg->radius : -1.0f;
            std::cout << "S4 hover 圆角 " << hoverRadius << " / 常态 " << outRadius
                      << " 缩放 " << hoverScale << "\n";
            if (hoverRadius <= outRadius || std::fabs(hoverScale - 1.02f) > 0.01f) {
                std::cerr << "schedule_ui_probe: S4 FAIL 行卡 hover 未驱动果冻形变\n";
                passed = false;
            } else {
                std::cout << "S4 PASS 行卡 hover 驱动 jellyCard\n";
            }

            // S4b：指针停在卡内「编辑」文字上 → 卡仍保持 hover（双集合并集）。
            // 单一集合会在这帧被命中面的 erase 抹掉（兄弟不继承 hover）。
            if (const auto* edit = findElement("schedule.list.user.0.edit")) {
                const float ex = edit->frame.x + edit->frame.width * 0.5f;
                const float ey = edit->frame.y + edit->frame.height * 0.5f;
                core::queuePointerMotion(window, ex, ey, core::PointerButtons{},
                                         noMods);
                for (int i = 0; i < 6; ++i) composeFrame();
                const auto* ctlBg = findElement("schedule.list.user.0.bg");
                const float ctlRadius = ctlBg != nullptr ? ctlBg->radius : -1.0f;
                std::cout << "S4b 指针在编辑文字上 卡圆角=" << ctlRadius << "\n";
                if (ctlRadius <= outRadius) {
                    std::cerr << "schedule_ui_probe: S4 FAIL 指针移到行内控件上卡丢失 hover\n";
                    passed = false;
                } else {
                    std::cout << "S4b PASS 卡内控件共担 hover（不移出卡）\n";
                }
                core::queuePointerMotion(window, w - 8.0f, 8.0f,
                                         core::PointerButtons{}, noMods);
                for (int i = 0; i < 6; ++i) composeFrame();
            }
        }
    }

    // ---------- D. 运行历史抽屉：卡片/状态点/渠道 chip/重试 + 按钮右边界同基准 ----------
    if (const auto* entry = findElement("schedule.list.history")) {
        clickElement(*entry);
        composeFrame();
    }
    expectNode("schedule.history.drawer.panel", &passed);
    expectNode("schedule.history.title", &passed);
    expectNode("schedule.history.summary", &passed);
    expectNode("schedule.history.close", &passed);
    expectNode("schedule.history.titleClose", &passed);
    if (emptyHistory) {
        // 空历史档：空态组件在位、无任何行卡
        expectNode("schedule.history.empty", &passed);
        expectNoNode("schedule.history.row.0", &passed);
        shot(shotHistory);
        if (const auto* close = findElement("schedule.history.close")) {
            clickElement(*close);
            composeFrame();
        }
    } else {
    expectNode("schedule.history.row.0", &passed);
    expectNode("schedule.history.row.0.dot", &passed);
    expectNode("schedule.history.row.0.status", &passed);
    expectNode("schedule.history.row.0.planned", &passed);
    expectNode("schedule.history.row.0.actual", &passed);
    // 渠道 chip 挂在有投递记录的行上（planned_at 倒序：错过 → 失败 → 成功）
    expectNode("schedule.history.row.1.chip.0", &passed);
    expectNode("schedule.history.row.0.retry", &passed);  // 失败/错过行才给重试
    // 对齐门禁：右上 × 与底部关闭钮右边界 = 弹窗内容列右基准（弹窗右 −24）；
    // 行内重试钮右边界 = 卡片右 −14（卡内边距）。三者各写各的 x 就是「没对齐」。
    if (const auto* panel = findElement("schedule.history.drawer.panel")) {
        const float contentRight = panel->frame.x + panel->frame.width - 24.0f;
        for (const char* id : {"schedule.history.titleClose",
                               "schedule.history.close"}) {
            const auto* btn = findElement(id);
            if (btn == nullptr) continue;
            const float right = btn->frame.x + btn->frame.width;
            if (std::fabs(right - contentRight) > 1.5f) {
                std::cerr << "schedule_ui_probe: " << id << " 右边界 " << right
                          << " ≠ 内容列右基准 " << contentRight << "\n";
                passed = false;
            }
        }
    }
    if (const auto* card = findElement("schedule.history.row.0")) {
        if (const auto* retry = findElement("schedule.history.row.0.retry")) {
            const float cardRight = card->frame.x + card->frame.width;
            const float retryRight = retry->frame.x + retry->frame.width;
            if (std::fabs((cardRight - retryRight) - 14.0f) > 1.5f) {
                std::cerr << "schedule_ui_probe: 重试钮右边界距卡右 "
                          << (cardRight - retryRight) << "（期望 14）\n";
                passed = false;
            }
        }
    }
    shot(shotHistory);
    // 关抽屉（backdrop 盖全窗，不关会挡住后续 B/C 段的点击）
    if (const auto* close = findElement("schedule.history.close")) {
        clickElement(*close);
        composeFrame();
    }
    }  // else: 非空历史档

    // ---------- B. 编辑弹窗态：点「新建」开弹窗 ----------
    if (const auto* btn = findElement("schedule.list.new")) {
        clickElement(*btn);
        composeFrame();
    } else {
        passed = false;
    }
    expectNode("schedule.edit.name", &passed);
    expectNode("schedule.edit.save", &passed);
    expectNode("schedule.edit.cancel", &passed);
    shot(shotEdit);  // 弹窗开启态构图（关掉再拍就成了列表帧）
    if (const auto* cancel = findElement("schedule.edit.cancel")) {
        clickElement(*cancel);
        composeFrame();
    }

    // ---------- C. 删除确认：取消不动真格，确认后行消失 ----------
    if (const auto* del = findElement("schedule.list.user.0.delete")) {
        clickElement(*del);
        composeFrame();
    }
    expectNode("schedule.list.deleteConfirm.title", &passed);
    expectNode("schedule.list.deleteConfirm.body", &passed);
    expectNode("schedule.list.deleteConfirm.purge", &passed);
    expectNode("schedule.list.deleteConfirm.cancel", &passed);
    expectNode("schedule.list.deleteConfirm.ok", &passed);
    shot(shotDelete);
    // 取消 → 弹窗关、行还在
    if (const auto* cancel = findElement("schedule.list.deleteConfirm.cancel")) {
        clickElement(*cancel);
        composeFrame();
    }
    expectNode("schedule.list.user.0.name", &passed);
    // 再开确认 → 点「删除」→ 默认软删路径 → 行从列表消失 + getRule=NotFound
    if (const auto* del = findElement("schedule.list.user.0.delete")) {
        clickElement(*del);
        composeFrame();
    }
    if (const auto* ok = findElement("schedule.list.deleteConfirm.ok")) {
        clickElement(*ok);
        composeFrame();
    }
    // 两行种子删掉一条后剩条顶到 user.0，改断言第二行消失（行数由下方 listRules 兜底）
    expectNoNode("schedule.list.user.1.name", &passed);
    {
        module::schedule::ScheduleStore store(tdb.handle());
        const int64_t now = static_cast<int64_t>(core::time::nowUnix());
        module::schedule::RuleRecord probe;
        probe.kind = "once";
        probe.specJson = "{\"at\":" + std::to_string(now + 60) + "}";
        // 上面只是占位对象；真正要查的是被删那条：经 listRules 数行数（软删后 1 条）
        auto rows = store.listRules();
        if (rows.isFail()) {
            std::cerr << "schedule_ui_probe: listRules 失败\n";
            passed = false;
        } else if (rows.value().size() != 1) {
            std::cerr << "schedule_ui_probe: 软删后 listRules 行数="
                      << rows.value().size() << "（期望 1）\n";
            passed = false;
        }
    }

    if (!probe::captureIfRequested(args, window, runtime, &passed)) passed = false;
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "schedule_ui_probe: FAILED\n";
        return 4;
    }
    std::cout << "schedule_ui_probe: PASS\n";
    return 0;
}
