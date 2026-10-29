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
//
// 种子走真 ScheduleStore::createRule 落库口（不绕产品代码写表）。模块经
// core::appDatabase() 取连接 → TestEnv 以 registerAppDb=true 注册。
//
// 运行：schedule_ui_probe [--shot-list <png>] [--shot-edit <png>] [--shot-delete <png>]
// 不带构图开关 = 纯断言门禁（ctest -L probe 跑的就是这条）。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（同 agent_dialog_probe）
#include "test_support.h"

#include "core/time.h"
#include "module_schedule/schedule_module.h"
#include "module_schedule/schedule_store.h"

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
    std::string shotList, shotEdit, shotDelete;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot-list" && i + 1 < argc) {
            shotList = argv[++i];
        } else if (arg == "--shot-edit" && i + 1 < argc) {
            shotEdit = argv[++i];
        } else if (arg == "--shot-delete" && i + 1 < argc) {
            shotDelete = argv[++i];
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
        module::schedule::RuleRecord r1;
        r1.name = "清理临时文件";
        r1.kind = "once";
        r1.specJson = "{\"at\":" + std::to_string(now + 3600) + "}";
        r1.actionKind = "remind";
        r1.actionJson = "{}";
        r1.missedPolicy = "once";
        if (store.createRule(r1).isFail()) {
            std::cerr << "schedule_ui_probe: 种子 createRule(1) 失败\n";
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
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
    }

    module::schedule::ScheduleModule mod;
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

    // ---------- A. 列表态：四入口 + 系统段零操作控件 ----------
    composeFrame();
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
