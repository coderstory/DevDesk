// schedule_ui_probe.cpp — 定时页瀑布流重设计（池140）的真窗口探针 + 构图留痕
//
// 池140（TAB + 瀑布卡片重设计）与 bug 150（命中面级果冻断言）的真窗口证据：
// 无头单测断言节点存在与落库语义，但「TAB 切换 / 列数断点 / 整卡命中 / 果冻
// 实机命中链路」需要真实渲染帧与真实指针命中分发。本探针用真窗口驱动：
//   A. 系统定时 TAB（默认）：页头三件套（标题/运行历史/新建）+ 块面 TAB
//      （选中 .bg 有、未选中无）+ 计数 + 系统卡（锁图标、来源块、零操作控件、
//      命中面无 onClick、命中面 = 整卡几何且建在卡 stack 最后）；
//      --shot-list <png> 落系统 TAB 帧。
//   B. TAB 切换 + 列数断点：点「用户定时」TAB → 用户卡出现、选中态翻转；
//      列数按当前窗口宽走 columnCountForWidth 断点，列宽 = (W−gap×(列数−1))/列数，
//      两卡分列（x 不同）。QA 在 1000×640 / 1600×1000 两档窗口各跑一遍本探针
//      即覆盖两档断点（--size WxH）。
//   C. 命中面级果冻断言（bug 150）：**真实指针 motion 进卡**（不直调 onHover
//      回调）→ tint 层出现（opacity 目标 1）、卡面圆角/缩放不变（hover 不放大）；
//      bg.visualStateSourceId 指向 .hit 且 pressedScale≈0.965（按下果冻接线）；
//      系统卡同法验 hover 浏览反馈（且无按下果冻接线）；指针移出 → tint 回 0。
//   D. 点卡开编辑弹窗：弹窗「启用」开关在表单首行、编辑态左下「删除」文字钮
//      存在；新建弹窗（页头唯一入口）无删除钮。
//   E. 归档折叠段：点折叠头展开 → 归档卡整卡可点 + hover 反馈。
//   G. 运行历史抽屉（原 D 段保留）：卡片/状态点/渠道 chip/重试 + 右边界同基准。
//   F. 删除链路：编辑弹窗左下「删除」→ 既有删除确认小弹窗 → 软删落库。
//
// 种子走真 ScheduleStore::createRule 落库口；系统卡经 provider 注册缝挂静态
// 来源；归档卡种一条 archivedAt 非空规则。模块经 core::appDatabase() 取连接。
//
// 运行：schedule_ui_probe [--shot-list <png>] [--shot-edit <png>] [--shot-delete <png>]
//                            [--shot-history <png>] [--empty-rules] [--shot-empty <png>]
// 不带构图开关 = 纯断言门禁（ctest -L probe 跑的就是这条）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 agent_dialog_probe）
#include "test_support.h"

#include "core/time.h"
#include "module_schedule/schedule_module.h"
#include "module_schedule/schedule_store.h"
#include "module_schedule/schedule_ui_list.h"  // columnCountForWidth（列数断点断言）

#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
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
    // --light 浅色主题档 / --empty 空历史档 / --empty-rules 用户定时空态档
    //（空态 = 纯文字提示，无卡无任何按钮，池137）。
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

    // 种子（真 ScheduleStore 落库口）：2 条用户一次性规则（未来 1/2 小时）+
    // 1 条已归档规则；系统卡经 provider 注册缝挂静态来源。
    {
        module::schedule::ScheduleStore store(tdb.handle());
        const int64_t now = static_cast<int64_t>(core::time::nowUnix());
        // --empty-rules：不播规则（用户定时空态档）
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
        if (store.createRule(r2).isFail()) {
            std::cerr << "schedule_ui_probe: 种子 createRule(2) 失败\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        // 归档卡种子：归档走 archiveRule 专用口——updateRule 按契约不写
        // archived_at（归档/软删只有 archiveRule / deleteRule 入口，防误写解归档）；
        // 直接 updateRule 塞 archivedAt 会被静默丢弃 → 归档段永远空（bug 150 收口）。
        module::schedule::RuleRecord r3;
        r3.name = "已归档提醒";
        r3.kind = "once";
        r3.specJson = "{\"at\":" + std::to_string(now + 10800) + "}";
        r3.actionKind = "remind";
        r3.actionJson = "{}";
        r3.missedPolicy = "once";
        auto r3res = store.createRule(r3);
        if (r3res.isOk()) {
            if (store.archiveRule(r3res.value().id).isFail()) {
                std::cerr << "schedule_ui_probe: 种子 archiveRule 失败\n";
            }
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
    if (light) {
        core::theme::setLight();
    }
    // 系统卡类别种子（池140：系统 TAB 需要一张真实系统卡供只读/命中面断言）。
    // 必须先于 onStart 注册：reload 只在 onStart/存档操作触发，onStart 后注册的
    // 来源要等下一次 reload 才装载（首轮探针实证：sys.0 恒缺位）。
    {
        struct ProbeSystemSource : module::schedule::ScheduleProvider {
            std::vector<module::schedule::ScheduleItemView> items() const override {
                module::schedule::ScheduleItemView v;
                v.source = "probe";
                v.key = "probe:task:1";
                v.title = "探针系统任务";
                v.summary = "系统卡只读形态（探针种子）";
                v.nextFireAt =
                    static_cast<int64_t>(core::time::nowUnix()) + 3600;
                v.sourceRoute = "Probe";
                return {v};
            }
        };
        const auto reg = mod.providerRegistry().registerProvider(
            std::make_unique<ProbeSystemSource>());
        if (reg.isFail()) {
            std::cerr << "schedule_ui_probe: 系统来源注册失败\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
    }
    mod.onStart();

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
        // 真实鼠标语义：点击前必先 motion 到点（真实指针没有瞬移）。纯瞬移按下
        // 会让 EUI 的 hover 目标/交互捕获链路踩到陈旧捕获态——曾致删除确认弹窗
        // 的 ok 点击被误派发到遮罩/取消路径（关窗不删，bug 150 收口实测）。
        core::queuePointerMotion(window, cx, cy, core::PointerButtons{},
                                 core::KeyModifiers{});
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
    // 真实指针 hover：motion 进点 → 驱帧 → 读 tint 目标值（命中面级，不直调回调）
    const core::KeyModifiers noMods;
    auto hoverAt = [&](float x, float y, int frames) {
        core::queuePointerMotion(window, x, y, core::PointerButtons{}, noMods);
        for (int i = 0; i < frames; ++i) composeFrame();
    };
    auto moveAway = [&]() { hoverAt(w - 8.0f, 8.0f, 6); };
    // 点击后隔帧成对（Press 与 Release 隔帧送达——单帧只送 Press、onClick 永不
    // 触发；home_probes 组 M 同款双帧口径）+ 弹窗过渡（~150ms）多帧收敛
    auto settleFrames = [&](int n) {
        for (int i = 0; i < n; ++i) composeFrame();
    };

    // 颜色等同（写入 element 的目标值比对，不是像素采样）
    auto sameColor = [](const eui::Color& a, const eui::Color& b) {
        return std::fabs(a.r - b.r) < 0.02f && std::fabs(a.g - b.g) < 0.02f &&
               std::fabs(a.b - b.b) < 0.02f && std::fabs(a.a - b.a) < 0.02f;
    };
    // 按钮级判定：ui_kit::button 把 level 配色写进 <id>.bg 的三态 normal 色
    auto buttonLevelIs = [&](const std::string& id, bool primary) {
        const core::dsl::Element* bg = findElement(id + ".bg");
        if (bg == nullptr) return false;
        const auto& t = core::theme::current();
        return sameColor(bg->color, primary ? t.colors.primary : t.colors.hover);
    };
    // 命中面级断言三件套（bug 150）：命中面 = 整卡几何 + 卡 stack 最后子级 +
    // 指定 onClick 有无
    auto expectHitSurface = [&](const std::string& cardId, bool clickable,
                                bool* ok) {
        const auto* card = findElement(cardId);
        const auto* hit = findElement(cardId + ".hit");
        if (card == nullptr || hit == nullptr) {
            std::cerr << "schedule_ui_probe: " << cardId << " 命中面缺失\n";
            *ok = false;
            return;
        }
        if (std::fabs(hit->frame.x - card->frame.x) > 1.5f ||
            std::fabs(hit->frame.y - card->frame.y) > 1.5f ||
            std::fabs(hit->frame.width - card->frame.width) > 1.5f ||
            std::fabs(hit->frame.height - card->frame.height) > 1.5f) {
            std::cerr << "schedule_ui_probe: " << cardId
                      << ".hit 命中面 ≠ 整卡几何\n";
            *ok = false;
        }
        if (card->children.empty() ||
            card->children.back()->id != cardId + ".hit") {
            std::cerr << "schedule_ui_probe: " << cardId
                      << ".hit 不是卡 stack 最后子级（EUI 兄弟命中逆序，命中面"
                         "被截走 = 果冻/点卡实机失效，bug 150 口径）\n";
            *ok = false;
        }
        if (clickable != (hit->onClick != nullptr)) {
            std::cerr << "schedule_ui_probe: " << cardId
                      << ".hit onClick 期望 " << (clickable ? "有" : "无")
                      << " 实际 " << (hit->onClick != nullptr ? "有" : "无") << "\n";
            *ok = false;
        }
    };
    // hover tint 实机判定：真实 motion 进点后 tint opacity 目标 = 期望
    auto expectTintOpacity = [&](const std::string& cardId, float want,
                                 bool* ok) {
        const auto* tint = findElement(cardId + ".tint");
        if (tint == nullptr) {
            std::cerr << "schedule_ui_probe: " << cardId << ".tint 缺失\n";
            *ok = false;
            return;
        }
        if (std::fabs(tint->opacity - want) > 0.02f) {
            std::cerr << "schedule_ui_probe: " << cardId << ".tint opacity="
                      << tint->opacity << " 期望 " << want
                      << "（真实指针命中未达卡面——bug 150 形态）\n";
            *ok = false;
        }
    };

    // ---------- A. 系统定时 TAB（默认）：页头 + TAB + 系统卡 ----------
    composeFrame();

    if (emptyRules) {
        // ---------- S1（池137/139）：空态 = 纯文字提示，无卡无任何按钮 ----------
        const auto& t = core::theme::current();
        if (!buttonLevelIs("schedule.list.new", /*primary=*/true)) {
            std::cerr << "schedule_ui_probe: S1 FAIL 页头新建钮不是 Primary"
                         "（唯一新建入口恒主钮）\n";
            passed = false;
        }
        expectNode("schedule.list.sys.empty", &passed);
        expectNoNode("schedule.list.sys.0", &passed);
        // 用户 TAB 同口径：纯文字、无任何按钮（池137）
        if (const auto* tab1 = findElement("schedule.list.tab.1")) {
            clickElement(*tab1);
            settleFrames(15);
        }
        expectNode("schedule.list.user.empty", &passed);
        expectNoNode("schedule.list.user.empty.action", &passed);
        expectNoNode("schedule.list.user.0", &passed);
        // 全页（滚动区内）唯一绿钮不存在：只扫列表滚动区子树（页头新建在区外）
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
        if (const auto* scrollRoot = findElement("schedule.list.scroll")) {
            walk(*scrollRoot);
        } else {
            std::cerr << "schedule_ui_probe: S1 FAIL 滚动区根节点缺失\n";
            passed = false;
        }
        if (primaryFills != 0) {
            std::cerr << "schedule_ui_probe: S1 FAIL 滚动区仍存在 "
                      << primaryFills << " 个主色填充（应 0）\n";
            passed = false;
        }
        if (passed) std::cout << "S1 PASS 空态纯文字无按钮 + 页头唯一新建 Primary\n";
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

    expectNode("schedule.list.title", &passed);
    expectNode("schedule.list.history", &passed);
    expectNode("schedule.list.new", &passed);
    expectNode("schedule.list.tab.0.text", &passed);
    expectNode("schedule.list.tab.1.text", &passed);
    expectNode("schedule.list.tabcount", &passed);
    // 块面 TAB（wtab 契约：选中建 .bg，未选中不建）
    expectNode("schedule.list.tab.0.bg", &passed);
    expectNoNode("schedule.list.tab.1.bg", &passed);
    // 系统卡：锁图标 + 来源块 + 零操作控件
    expectNode("schedule.list.sys.0", &passed);
    expectNode("schedule.list.sys.0.lock", &passed);
    expectNode("schedule.list.sys.0.source", &passed);
    expectNoNode("schedule.list.sys.0.edit", &passed);
    expectNoNode("schedule.list.sys.0.delete", &passed);
    expectNoNode("schedule.list.sys.0.switch", &passed);
    // 命中面级断言：系统卡 = 整卡 hover 面、无 onClick（bug 150 口径）
    expectHitSurface("schedule.list.sys.0", /*clickable=*/false, &passed);
    moveAway();
    shot(shotList);

    // ---------- C0. 系统卡 hover 实机（真实 motion 进卡）----------
    if (const auto* sysCard = findElement("schedule.list.sys.0")) {
        hoverAt(sysCard->frame.x + sysCard->frame.width * 0.5f,
                sysCard->frame.y + sysCard->frame.height * 0.5f, 6);
        expectTintOpacity("schedule.list.sys.0", 1.0f, &passed);
        // 系统卡无按下果冻接线（不可点无果冻，§4.4）
        if (const auto* bg = findElement("schedule.list.sys.0.bg")) {
            if (!bg->visualStateSourceId.empty()) {
                std::cerr << "schedule_ui_probe: 系统卡 bg 不应挂按下果冻（只读）\n";
                passed = false;
            }
        }
        if (passed) std::cout << "C0 PASS 系统卡 hover 浏览反馈（实机命中链路）\n";
        moveAway();
    }

    // ---------- B. TAB 切换 + 用户卡结构 + 列数断点 ----------
    if (const auto* tab1 = findElement("schedule.list.tab.1")) {
        clickElement(*tab1);
        settleFrames(15);
    } else {
        passed = false;
    }
    expectNode("schedule.list.tab.1.bg", &passed);
    expectNoNode("schedule.list.tab.0.bg", &passed);
    expectNode("schedule.list.user.0.bg", &passed);
    expectNode("schedule.list.user.1.bg", &passed);
    expectNoNode("schedule.list.user.0.edit", &passed);
    expectNoNode("schedule.list.user.0.delete", &passed);
    expectNoNode("schedule.list.user.0.switch", &passed);
    expectHitSurface("schedule.list.user.0", /*clickable=*/true, &passed);
    // 列数断点（池140 §3.1）：列数由当前窗口内容宽推导，列宽走瀑布公式
    {
        const float contentW = w - 40.0f;  // 页 pad 20 × 2
        const int cols = module::schedule::columnCountForWidth(contentW);
        const float gap = 8.0f;  // need219 §4.3 卡间距统一档 8px（与 schedule_ui_list kCardGap 同步）
        const float colW =
            (contentW - gap * static_cast<float>(cols - 1)) /
            static_cast<float>(cols);
        const auto* c0 = findElement("schedule.list.user.0.bg");
        const auto* c1 = findElement("schedule.list.user.1.bg");
        if (c0 != nullptr) {
            std::cout << "B 列数=" << cols << " 列宽期望=" << colW
                      << " 实际=" << c0->frame.width << "\n";
            if (std::fabs(c0->frame.width - colW) > 1.5f) {
                std::cerr << "schedule_ui_probe: B FAIL 列宽 " << c0->frame.width
                          << " ≠ 瀑布公式值 " << colW << "（列数 " << cols
                          << "）\n";
                passed = false;
            }
            if (c1 != nullptr && cols >= 2 &&
                std::fabs(c0->frame.x - c1->frame.x) <= 1.0f) {
                std::cerr << "schedule_ui_probe: B FAIL 两卡未分列（x 相同）\n";
                passed = false;
            }
        }
        if (passed) std::cout << "B PASS TAB 切换 + 列数断点（列数 " << cols
                              << "）+ 卡面零操作钮\n";
    }

    // ---------- C. 用户卡果冻命中面级断言（bug 150 核心）----------
    {
        const auto* c0 = findElement("schedule.list.user.0.bg");
        if (c0 != nullptr) {
            const float baseRadius = c0->radius;
            const float baseScale = c0->transform.scale.x;
            // 按下果冻接线：bg.visualStateSourceId 指向 .hit、0.965
            if (c0->visualStateSourceId.empty() ||
                c0->visualStateSourceId.find(".hit") ==
                    std::string::npos) {
                std::cerr << "schedule_ui_probe: C FAIL 用户卡 bg 未挂按下果冻"
                             "（visualStateSource → .hit）\n";
                passed = false;
            } else if (std::fabs(c0->pressedScale - 0.965f) > 0.001f) {
                std::cerr << "schedule_ui_probe: C FAIL pressedScale="
                          << c0->pressedScale << "（期望 0.965）\n";
                passed = false;
            }
            // 真实指针进卡（不直调 onHover）：tint 出现 + hover 不放大不变形
            hoverAt(c0->frame.x + c0->frame.width * 0.5f,
                    c0->frame.y + c0->frame.height * 0.5f, 6);
            expectTintOpacity("schedule.list.user.0", 1.0f, &passed);
            if (const auto* hov = findElement("schedule.list.user.0.bg")) {
                std::cout << "C hover 圆角 " << hov->radius << "（常态 "
                          << baseRadius << "）缩放 " << hov->transform.scale.x
                          << "\n";
                if (std::fabs(hov->radius - baseRadius) > 0.5f ||
                    std::fabs(hov->transform.scale.x - baseScale) > 0.01f) {
                    std::cerr << "schedule_ui_probe: C FAIL hover 改变了卡面"
                                 "形变（§6 hover 不放大）\n";
                    passed = false;
                }
            }
            // 指针移出 → tint 回 0
            moveAway();
            expectTintOpacity("schedule.list.user.0", 0.0f, &passed);
            if (passed) std::cout << "C PASS 用户卡果冻命中面实机断言"
                                     "（motion 进/出卡 + 按下接线）\n";
        }
    }

    // ---------- D. 点卡开编辑弹窗（启用开关 + 左下删除）----------
    if (const auto* card = findElement("schedule.list.user.0")) {
        clickElement(*card);
        settleFrames(15);
    }
    {
        // 弹窗开合的 opacity 目标值挂在 <id>.panel 上（components::dialog 契约）
        const auto* panel = findElement("schedule.edit.dialog.panel");
        if (panel == nullptr || panel->opacity < 0.5f) {
            std::cerr << "schedule_ui_probe: D FAIL 点卡未打开编辑弹窗\n";
            passed = false;
        }
        expectNode("schedule.edit.enabled", &passed);  // 表单首行「启用」开关
        expectNode("schedule.edit.delete", &passed);   // 编辑态左下删除文字钮
        expectNode("schedule.edit.save", &passed);
        expectNode("schedule.edit.cancel", &passed);
        shot(shotEdit);
        if (const auto* cancel = findElement("schedule.edit.cancel")) {
            clickElement(*cancel);
            settleFrames(15);
        }
    }

    // D2. 新建弹窗（页头唯一入口）：无删除钮（新规则无可删）
    if (const auto* btn = findElement("schedule.list.new")) {
        clickElement(*btn);
        settleFrames(15);
    }
    {
        const auto* panel = findElement("schedule.edit.dialog.panel");
        if (panel == nullptr || panel->opacity < 0.5f) {
            std::cerr << "schedule_ui_probe: D2 FAIL 页头新建未打开编辑弹窗\n";
            passed = false;
        }
        expectNode("schedule.edit.enabled", &passed);
        expectNoNode("schedule.edit.delete", &passed);
        if (const auto* cancel = findElement("schedule.edit.cancel")) {
            clickElement(*cancel);
            settleFrames(15);
        }
    }

    // ---------- E. 归档折叠段（用户 TAB 滚动区尾部）----------
    if (const auto* header = findElement("schedule.list.archive.header")) {
        clickElement(*header);
        settleFrames(15);
        expectNode("schedule.list.archive.0", &passed);
        expectNode("schedule.list.archive.0.hit", &passed);
        expectHitSurface("schedule.list.archive.0", /*clickable=*/true, &passed);
        if (const auto* card = findElement("schedule.list.archive.0")) {
            hoverAt(card->frame.x + card->frame.width * 0.5f,
                    card->frame.y + card->frame.height * 0.5f, 6);
            expectTintOpacity("schedule.list.archive.0", 1.0f, &passed);
            moveAway();
        }
        if (passed) std::cout << "E PASS 归档折叠段整卡可点 + hover 反馈\n";
    } else {
        std::cerr << "schedule_ui_probe: E FAIL 归档折叠头缺失\n";
        passed = false;
    }

    // ---------- G. 运行历史抽屉（先于删除：软删会摘走该规则的 runs）----------
    if (const auto* entry = findElement("schedule.list.history")) {
        clickElement(*entry);
        settleFrames(15);
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
            settleFrames(15);
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
    // 关抽屉（backdrop 盖全窗，不关会挡住后续 F 段的点击）
    if (const auto* close = findElement("schedule.history.close")) {
        clickElement(*close);
        settleFrames(15);
    }
    }  // else: 非空历史档

    // ---------- F. 删除链路：编辑弹窗左下删除 → 确认弹窗 → 软删落库 ----------
    if (const auto* card = findElement("schedule.list.user.0")) {
        clickElement(*card);
        settleFrames(15);
        const auto* del = findElement("schedule.edit.delete");
        if (del == nullptr) {
            std::cerr << "schedule_ui_probe: F FAIL 编辑弹窗无删除钮\n";
            passed = false;
        } else {
            clickElement(*del);
            settleFrames(15);
        }
    }
    // 编辑弹窗已收、确认弹窗开
    {
        const auto* panel = findElement("schedule.edit.dialog.panel");
        if (panel != nullptr && panel->opacity >= 0.5f) {
            std::cerr << "schedule_ui_probe: F FAIL 点删除后编辑弹窗未收\n";
            passed = false;
        }
    }
    expectNode("schedule.list.deleteConfirm.title", &passed);
    expectNode("schedule.list.deleteConfirm.body", &passed);
    expectNode("schedule.list.deleteConfirm.purge", &passed);
    expectNode("schedule.list.deleteConfirm.cancel", &passed);
    expectNode("schedule.list.deleteConfirm.ok", &passed);
    shot(shotDelete);
    // 取消 → 弹窗关、卡还在
    if (const auto* cancel = findElement("schedule.list.deleteConfirm.cancel")) {
        clickElement(*cancel);
        settleFrames(15);
    }
    expectNode("schedule.list.user.0.name", &passed);
    // 再走一遍 → 确认删除 → 默认软删 → 卡消失 + listRules 少一行
    if (const auto* card = findElement("schedule.list.user.0")) {
        clickElement(*card);
        settleFrames(15);
    }
    if (const auto* del = findElement("schedule.edit.delete")) {
        clickElement(*del);
        settleFrames(15);
    }
    if (const auto* ok = findElement("schedule.list.deleteConfirm.ok")) {
        clickElement(*ok);
        settleFrames(15);
    }
    // 两条活跃规则删掉一条后剩 1 条顶到 user.0（归档卡在折叠段，编号独立）
    expectNoNode("schedule.list.user.1.bg", &passed);
    {
        module::schedule::ScheduleStore store(tdb.handle());
        auto rows = store.listRules();
        if (rows.isFail()) {
            std::cerr << "schedule_ui_probe: listRules 失败\n";
            passed = false;
        } else if (rows.value().size() != 2) {
            // listRules WHERE deleted_at IS NULL：软删行**被过滤**（此前注释把
            // 软删/归档语义写反）。删前 3 行 = r1 + r2 + r3(归档在库)；软删 r1
            // 后 = r2(活跃) + r3(归档在库) = 2 行
            std::cerr << "schedule_ui_probe: 软删后 listRules 行数="
                      << rows.value().size() << "（期望 2 = 1 活跃 + 1 归档在库）\n";
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
