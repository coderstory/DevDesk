// tests/test_modules/test_agent_ui.cpp
// 智能体页 UI 无头用例：页面骨架 / 任务弹窗（新建·编辑·校验·关闭四路径）/ 详情弹窗
// （配置摘要、run 列表、消息块级展开）/ 侧边栏接线 / 常驻入队 / 弹窗几何预算。
//
// 跨文件前提（节点 id 契约，agent_module.cpp 侧为准）：
//   页面   agent.page / agent.page.header / agent.title / agent.page.newTask /
//          agent.tools.*（可用工具区块）/ agent.tasklist（scrollView）+.content /
//          agent.tasklist.panel（UAT6 列表容器 = background 灰面板，item 白卡浮起）/
//          agent.task.<id>（UAT6 卡片化：= 卡 stack，.bg 是白卡本体（surface+
//          radiusWeChat+细边，hover 加深，点击开详情），.row 内缩 12px 内边距；
//          .title / .status / .status.dot / .detail / .edit /
//          .toggle / .delete / [.limit 仅限次任务] / [.warn 配置异常] /
//          [.off 仅停用] / [.enqueue 仅常驻] / .run（行内操作钮统一 56×32）/
//          run 结束通知（Task 1；fix round 起经 takeRunEndNotice 走 app 级通道）/
//          失败迁移同帧发 OS 系统通知（id 前缀 agent-run-，成功/取消零发送）
//   删除确认弹窗 agent.dialog.delete.*：.title / .msg / .cancel / .confirm（zIndex 1100，
//          取消/遮罩/Esc 均不删；确认才走 store 级联删）
//   任务弹窗 agent.dialog.task.*（新建 / 编辑共用，UAT5 重设计）：
//          .name（任务名称大输入，空 = deriveTaskName(goal)）/ .goal（多行）/
//          .trigger（segment 根，选项 .trigger.item.N——可点面 .bg，callDropdownChange 有回落）/
//          .cron（仅定时）/ .once（仅闹钟，本地时间输入）/
//          .limit（仅定时·事件，行为开关行的"只跑一次"）/
//          .slot / .iter / .destructive / .cancel / .save / .close / .step.<i>[.conn]（步骤胶囊+短连接线；当前步校验失败标 "! " 红字，无全局错误行）/
//          .aichip（指令标签行右缘「✎ AI 起草」chip，T7b；UAT5 从标题行挪入）/
//          .trigger.hint（触发语义提示，定时/闹钟预览非空时让位）/
//          .meta.hint（迭代上限后果 + 破坏性操作说明，恒渲染）
//   AI 草稿子弹窗 agent.dialog.task.draft.*（叠在任务弹窗上，zIndex 1100，T7b）：
//          .input（多行）/ .generate / .dismiss / .close / .caption /
//          .notice / .warnings（生成完成才渲染）/ .error（失败 / 校验错才渲染）
//   详情弹窗 agent.dialog.detail.*（UAT6 方案 A 重设计：弹性宽 max(720, 窗口/2)、
//          头部任务名 + 顶部 segment 三档，旧宽屏双栏/窄屏 2-tab/配置浮层三套 IA 退役）：
//          .title（真实任务名，18px 700）/.header.kicker/.header.status（● 着色）/
//          .header.meta（触发摘要；读取失败时 error 色）/
//          .edit[.wrap]（头部「编辑任务」chip：关详情→开既有编辑弹窗）/
//          .close / .stop[.wrap]（运行中才渲染；点击后切「停止中」禁用，C1 协作式取消）/
//          .summary.last/.next/.stats（各 .label/.value/.sub，常驻三卡横排）/
//          .nav（segment 三档根）+ .nav.item.<i>[.bg 可点面/.text]——选中=wechat 绿
//          底白字（反向锁死旧暖橙）；tab 互斥：0=runlist / 1=detail / 2=config.scroll /
//          .runlist（.title/.count/.scroll | .empty）+ .runlist.<i>[.err[.hint]]
//          （点 .runlist.<i>.bg 选中并自动切 tab 1）/
//          .detail（tab1 根）：.detail.status[.pill/.dot/.text] / .detail.id /
//          .detail.meta.<field>[.label/.value]（2 列网格，field 同旧竖排）/
//          .detail.schedule[.plan/.actual/.verdict] / .detail.error[.label/.scroll/.text] /
//          .msgs.title / .msgs.toggle（默认折叠）/ .msgs.copyfinal / .msgs.hint /
//          .msgscroll（展开态固定区）或 .detail.scroll（余高不足整栏滚）/ .empty /
//          .config.scroll（tab2 根）+ .config.body.<field>[.label/.value]（field ∈
//          name/trigger/schedule/model/iterations/limit/goal/destructive/
//          problem/context；problem 仅异常时渲染，error 色）——字段 id 沿用旧浮层契约 /
//          .msg.<i>[.badge / .time / .toggle / .preview / .copy（仅展开态）/
//          .blk.<j>[.out]] / .footer.hint / .footer.close / .footer.edit
//   页级复制 toast agent.detail.toast（.message/.title/.bg 等，compose 尾部最后画）
//   旧页内表单（agent.form.* / agent.edit.*）与页内详情展开区已删除——本文件用
//   「节点不存在」把它们钉死，防回潮。
//
// 无头口径：
//   - 裸 eui::Ui + ui.begin/mod.compose/ui.end + ui.layout（frame 靠 layout 填充）；
//   - 按钮/行/弹窗关闭面点击 = 直调元素上真实挂的回调：button 的 onClick 挂在
//     <id>.bg，弹窗遮罩挂 <dialog>.backdrop；
//   - 文本输入 = 对 <id>.hit 直调 onTextInput（真实 input 事件路径）；
//   - 下拉切换 = 直调 <id>.item.<i> 的 onClick；开关切换 = 直调 <id>.hit 的 onClick。
//   真窗口几何 + 真命中分发的探针侧在 tests/probes/agent_probes.cpp。
#include "doctest.h"
#include "test_dialog_frame_helper.h"  // dialog_frame：弹窗「元素在框内」共享断言
#include "test_support.h"

#include "app_sidebar.h"
#include "core/event_bus.h"
#include "core/module_interface.h"
#include "core/render/text.h"   // 开关标签盒容纳断言（真实字体测量）
#include "core/router.h"
#include "core/theme.h"
#include "core/time.h"
#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "module_agent/agent_time_text.h"
#include "module_agent/trigger_config.h"
#include "platform/http_client.h"
#include "platform/notification.h"

#include <components/input.h>   // InputState：读回弹窗内输入框真实文本（预填判据）
#include <eui/dsl.h>
#include <ui_kit/card.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

// RAII：用例退出（含 fail-fast 提前 return 路径）时把本模块注册的工具从进程级
// 注册表摘除（ToolRegistry 不在 resetCaseGlobals 清单里）
struct ToolGuard {
    std::vector<std::string> names;
    ToolGuard(std::initializer_list<std::string> toolNames) : names(toolNames) {}
    ~ToolGuard() {
        for (const auto& name : names) {
            core::ToolRegistry::instance().unregisterTool(name);
        }
    }
};

using InputState = components::input_detail::InputModel::InputState;

const eui::Screen kScreen{1600.0f, 1000.0f};

// 系统通知 spy 后端：记录 send 调用（id/body/level + 发送线程），授权恒放行。供 OS 通道
// 断言（失败迁移必达恰 1 次 / 成功零调用 / 发送不在主线程——M2 后台化）注入；
// 用例结束必须 setBackendForTest(nullptr) + resetForTest() 复位，否则污染后续用例。
// 发送可能在后台线程落地（M2）：lastId/lastBody/lastLevel/senderId 的写在 fetch_add
// 之前，测试侧以 acquire 读 sendCalls 拿到 happens-before 后再读，无数据竞争。
struct OsNotifySpy : platform::notify::Backend {
    std::atomic<int> sendCalls{0};
    std::string lastId;
    std::string lastBody;
    int lastLevel = -1;
    std::thread::id senderId{};
    core::Result<platform::notify::Status> send(
        const platform::notify::Notification& n) override {
        lastId = n.id;
        lastBody = n.body;
        lastLevel = static_cast<int>(n.level);
        senderId = std::this_thread::get_id();
        sendCalls.fetch_add(1, std::memory_order_release);
        return core::Result<platform::notify::Status>::ok(
            platform::notify::Status::Accepted);
    }
    platform::notify::Authorization authorizationStatus() override {
        return platform::notify::Authorization::Authorized;
    }
    void requestAuthorization() override {}
};

// 裸 Ui compose 一帧 + 显式 layout（元素 frame 需显式布局）
void composeTo(eui::Ui& ui, module::agent::AgentModule& mod,
               const eui::Screen& screen, const char* pageId) {
    ui.begin(pageId);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

// 直调元素自身 onClick（契约：可点元素 id 元素本身带 onClick；
// components::button 的 onClick 落在 <id>.bg，调用方传 <id>.bg）
bool clickId(eui::Ui& ui, const std::string& id) {
    const auto* e = ui.find(id);
    if (e == nullptr || e->onClick == nullptr) return false;
    e->onClick();
    return true;
}

// 按 UTF-8 字符边界逐字符送进 input 的 onTextInput（真实 input 事件路径）
void typeInto(core::dsl::Element* hit, const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        core::TextInputEvent ev;
        ev.text = text.substr(i, len);
        hit->onTextInput(ev);
        i += len;
    }
}

// 触发类型 / 槽位下拉切换：直调 item 上真实挂的 onChange 回调
bool callDropdownChange(eui::Ui& ui, const std::string& ddId, int idx) {
    const auto* item = ui.find(ddId + ".item." + std::to_string(idx));
    if (item != nullptr && item->onClick != nullptr) {
        item->onClick();
        return true;
    }
    // UAT5：触发方式改 segment 选项钮（可点面 = .item.N.bg）；下拉型字段（slot /
    // cron.template）仍是 .item.N 直点。两形态同 helper，六处旧 trigger 调用零改动
    const auto* opBg = ui.find(ddId + ".item." + std::to_string(idx) + ".bg");
    if (opBg != nullptr && opBg->onClick != nullptr) {
        opBg->onClick();
        return true;
    }
    return false;
}

std::string inputText(eui::Ui& ui, const std::string& id) {
    return ui.state<InputState>(id).text;
}

bool nearPx(float a, float b) { return std::fabs(a - b) <= 0.5f; }

// inner 完全落在 outer 内（0.5 容差，与 nearPx 同口径）
bool frameContained(const core::dsl::Element& outer,
                    const core::dsl::Element& inner) {
    return inner.frame.x >= outer.frame.x - 0.5f &&
           inner.frame.y >= outer.frame.y - 0.5f &&
           inner.frame.x + inner.frame.width <=
               outer.frame.x + outer.frame.width + 0.5f &&
           inner.frame.y + inner.frame.height <=
               outer.frame.y + outer.frame.height + 0.5f;
}

// 打开新建态任务弹窗（点页顶「+ 新建任务」）；成功即已 recompose 一帧
bool openNewTaskDialog(eui::Ui& ui, module::agent::AgentModule& mod,
                       const char* pageId) {
    composeTo(ui, mod, kScreen, pageId);
    // 空库 = 空态 CTA「创建第一个任务」(.empty.btn.bg)；
    // 有任务列表 = 页头「+ 新建任务」(.page.newTask.bg)
    if (clickId(ui, "agent.page.newTask.bg")) return true;
    if (clickId(ui, "agent.empty.btn.bg")) return true;
    return false;
}

// 打开某任务的详情弹窗（点行内「详情」chip）
bool openDetailDialog(eui::Ui& ui, module::agent::AgentModule& mod,
                      const std::string& tid) {
    return clickId(ui, "agent.task." + tid + ".detail");
}

// 本地「今天 + dayOffset 的 12:00」：选中午避开夏令时凌晨跳变，任何时区都必然存在。
// 返回归一化后的 tm（mktime 会改写字段，先算再取一次 localtime，避免 DST 日期错位）
std::tm localNoonTm(int dayOffset) {
    std::tm nowLocal{};
    core::time::localTime(core::time::nowUnix(), &nowLocal);
    nowLocal.tm_mday += dayOffset;
    nowLocal.tm_hour = 12;
    nowLocal.tm_min = 0;
    nowLocal.tm_sec = 0;
    nowLocal.tm_isdst = -1;
    const std::time_t t = std::mktime(&nowLocal);
    std::tm normalized{};
    core::time::localTime(t, &normalized);
    return normalized;
}

std::string formatLocalMinute(const std::tm& local) {
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
    return buf;
}

int64_t epochOfLocal(const std::tm& local) {
    std::tm copy = local;
    copy.tm_isdst = -1;
    return static_cast<int64_t>(std::mktime(&copy));
}

std::string onceConfig(int64_t epoch) {
    return R"({"once_at":)" + std::to_string(epoch) + "}";
}

// 任务弹窗公共前奏：开新建 → 输入目标（真实 input 事件路径）。bool 返回供
// DOCTEST_REQUIRE 在调用点判停（doctest 宏会 return，不能在 void 辅助函数里用）
// UAT6 步骤条适配：goal 在 step 2，需先导航
bool openNewTaskWithGoal(eui::Ui& ui, module::agent::AgentModule& mod,
                         const std::string& pagePrefix,
                         const std::string& goal) {
    if (!openNewTaskDialog(ui, mod, (pagePrefix + "a").c_str())) return false;
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, (pagePrefix + "b").c_str());
    // 导航到步骤 2（指令）
    if (!clickId(ui2, "agent.dialog.task.step.2.bg")) return false;
    eui::Ui ui2s2;
    composeTo(ui2s2, mod, kScreen, (pagePrefix + "b2").c_str());
    auto* goalHit = ui2s2.find("agent.dialog.task.goal.hit");
    if (goalHit == nullptr) return false;
    typeInto(goalHit, goal);
    return true;
}

} // namespace

REGISTER_TEST(agent_ui_compose_skeleton,
              "agent_ui: 空库页面骨架（列表是 scrollView / 旧页内表单节点已删 / 弹窗未开不渲染）", {
    test_support::TestDb tdb("agentui", "skeleton");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_skeleton");

    const auto* page = ui.find("agent.page");
    DOCTEST_REQUIRE(page != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(page->frame.width, 1600.0f),
                          "agent.page.frame.width 实测 " +
                              std::to_string(page->frame.width));
    const auto* title = ui.find("agent.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("智能体"));

    // 「+ 新建任务」：空库时隐藏（只留空态引导 CTA「创建第一个任务」）
    DOCTEST_CHECK(ui.find("agent.page.newTask") == nullptr);
    DOCTEST_CHECK(ui.find("agent.page.newTask.bg") == nullptr);

    // 旧页内新建表单 / 编辑表单节点全部不存在（骨架契约）
    DOCTEST_CHECK(ui.find("agent.form") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.goal.label") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.create") == nullptr);
    DOCTEST_CHECK(ui.find("agent.form.error") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.form") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.save") == nullptr);
    DOCTEST_CHECK(ui.find("agent.edit.error") == nullptr);

    // 任务列表容器 = scrollView（.content 是 scrollView 独有的内容列，
    // 普通 ui.column 不会产生该节点）
    DOCTEST_REQUIRE(ui.find("agent.tasklist") != nullptr);
    DOCTEST_CHECK_MESSAGE(ui.find("agent.tasklist.content") != nullptr,
                          "agent.tasklist 不是 scrollView（缺 .content 内容列）");

    DOCTEST_CHECK(ui.find("agent.tools.header") != nullptr);

    // 弹窗未打开：弹窗内容不渲染（打开/关闭状态由节点存在性直接可判）
    DOCTEST_CHECK(ui.find("agent.dialog.task.goal") == nullptr);
    DOCTEST_CHECK(ui.find("agent.dialog.detail.msgscroll") == nullptr);

    // 空库无任务行（fresh 库首个任务 id = 1）
    DOCTEST_CHECK(ui.find("agent.task.1") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_db_import_reloads_tasks,
              "agent_ui: db.imported 置脏 → compose 重载任务列表", {
    test_support::TestDb tdb("agentui", "dbimport");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_dbimport1");
    DOCTEST_REQUIRE(ui.find("agent.empty.title") != nullptr);   // 空库空态 S2.8
    DOCTEST_CHECK(!mod.dbImportedDirtyForTest());

    // 模拟远端库导入：库里多出一条任务，但**不发任何事件**（缓存应保持旧值）
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("导入任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dbimport2");
    DOCTEST_CHECK(ui2.find("agent.task." + tid) == nullptr);   // 对照：未广播 → 仍旧

    ::core::event::bus().emit(::core::event::kDbImported);
    DOCTEST_CHECK(mod.dbImportedDirtyForTest());   // handler 已置原子脏标记

    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dbimport3");
    DOCTEST_CHECK(ui3.find("agent.task." + tid) != nullptr);   // 广播后重载可见
    DOCTEST_CHECK(ui3.find("agent.task." + tid + ".title") != nullptr);
    DOCTEST_CHECK(!mod.dbImportedDirtyForTest());   // compose 已收割清标记

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_new_task_dialog_fields,
              "agent_ui: 点「+ 新建任务」→ 步骤条弹窗各步字段验证；cron 仅触发方式=定时时渲染", {
    test_support::TestDb tdb("agentui", "dialogfields");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_dlg1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dlg2");

    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.task") != nullptr,
                            "弹窗外框 agent.dialog.task 缺失");
    // 常驻元素
    DOCTEST_CHECK(ui2.find("agent.dialog.task.cancel") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.save") != nullptr);
    // W4 C1：上一步/测试按钮条件渲染——step 0 不渲染 prev，测试仅末步渲染
    DOCTEST_CHECK(ui2.find("agent.dialog.task.prev") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.test") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.close") != nullptr);

    // 步骤条：4 个胶囊节点（bg 可点面）+ 短连接线
    for (int i = 0; i < 4; ++i) {
        DOCTEST_CHECK_MESSAGE(
            ui2.find("agent.dialog.task.step." + std::to_string(i) + ".bg") != nullptr,
            "步骤 " + std::to_string(i) + " 节点缺失");
    }
    DOCTEST_CHECK(ui2.find("agent.dialog.task.step.1.conn") != nullptr);

    // Step 0（基础信息）：名称字段可见；其它步内容不可见
    DOCTEST_CHECK(ui2.find("agent.dialog.task.name") != nullptr);   // 名称输入在 step 0
    DOCTEST_CHECK(ui2.find("agent.dialog.task.goal.caption") == nullptr);  // goal 在 step 2
    DOCTEST_CHECK(ui2.find("agent.dialog.task.trigger") == nullptr);       // 触发在 step 1

    // ---- 导航到步骤 1（触发） ----
    eui::Ui ui_s1;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    composeTo(ui_s1, mod, kScreen, "agent_ui_dlg_s1");
    DOCTEST_CHECK(ui_s1.find("agent.dialog.task.name") == nullptr);         // step 0 已隐藏
    DOCTEST_CHECK(ui_s1.find("agent.dialog.task.trigger") != nullptr);      // 触发 segment 出现
    // 六类触发按钮
    for (int i = 0; i < module::agent::kTriggerCount; ++i) {
        DOCTEST_CHECK_MESSAGE(
            ui_s1.find("agent.dialog.task.trigger.item." + std::to_string(i) + ".bg") != nullptr,
            "触发 segment 第 " + std::to_string(i) + " 项缺失");
    }
    // 默认手动 → cron/once/event 不渲染
    DOCTEST_CHECK(ui_s1.find("agent.dialog.task.cron") == nullptr);
    DOCTEST_CHECK(ui_s1.find("agent.dialog.task.event.0") == nullptr);

    // 切到定时 → cron 出现
    DOCTEST_REQUIRE_MESSAGE(callDropdownChange(ui_s1, "agent.dialog.task.trigger", 1),
                            "trigger.item.1 不存在");
    eui::Ui ui_s1b;
    composeTo(ui_s1b, mod, kScreen, "agent_ui_dlg_s1b");
    DOCTEST_CHECK_MESSAGE(ui_s1b.find("agent.dialog.task.cron") != nullptr,
                          "触发方式=定时 时未渲染 cron");
    DOCTEST_CHECK(ui_s1b.find("agent.dialog.task.cron.caption") != nullptr);
    DOCTEST_CHECK(ui_s1b.find("agent.dialog.task.cron.template.field") != nullptr);

    // 切到事件 → event chips 出现
    DOCTEST_REQUIRE(callDropdownChange(ui_s1b, "agent.dialog.task.trigger", 3));
    eui::Ui ui_s1c;
    composeTo(ui_s1c, mod, kScreen, "agent_ui_dlg_s1c");
    DOCTEST_CHECK(ui_s1c.find("agent.dialog.task.event.0") != nullptr);
    DOCTEST_CHECK(ui_s1c.find("agent.dialog.task.event.1") != nullptr);
    DOCTEST_CHECK(ui_s1c.find("agent.dialog.task.event.2") != nullptr);
    DOCTEST_CHECK(ui_s1c.find("agent.dialog.task.cron") == nullptr);

    // ---- 导航到步骤 2（指令） ----
    eui::Ui ui_s2;
    DOCTEST_REQUIRE(clickId(ui_s1c, "agent.dialog.task.step.2.bg"));
    composeTo(ui_s2, mod, kScreen, "agent_ui_dlg_s2");
    DOCTEST_CHECK(ui_s2.find("agent.dialog.task.goal") != nullptr);
    DOCTEST_CHECK(ui_s2.find("agent.dialog.task.goal.caption") != nullptr);
    DOCTEST_CHECK(ui_s2.find("agent.dialog.task.aichip") != nullptr);
    // W4 C3：执行记忆字段已移除（并入系统提示词单字段）
    DOCTEST_CHECK(ui_s2.find("agent.dialog.task.note") == nullptr);
    DOCTEST_CHECK(ui_s2.find("agent.dialog.task.sysprompt") != nullptr);
    DOCTEST_CHECK(ui_s2.find("agent.dialog.task.trigger") == nullptr);

    // ---- 导航到步骤 3（执行与安全） ----
    eui::Ui ui_s3;
    DOCTEST_REQUIRE(clickId(ui_s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui_s3, mod, kScreen, "agent_ui_dlg_s3");
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.slot") != nullptr);
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.iter") != nullptr);
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.destructive") != nullptr);
    // W4 C3：自动记录开关已移除（行为固定开启）
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.autonote") == nullptr);
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.meta.hint") != nullptr);
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.group.danger") != nullptr);
    DOCTEST_CHECK(ui_s3.find("agent.dialog.task.goal") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_create_task_persists_fields,
              "agent_ui: 步骤条弹窗新建保存 → 落库行字段与表单取值一致 + 弹窗关闭", {
    test_support::TestDb tdb("agentui", "dlgsave");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_save1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_save2");

    // Step 0（基础信息）：填任务名称
    auto* nameHit = ui2.find("agent.dialog.task.name.hit");
    DOCTEST_REQUIRE(nameHit != nullptr);
    DOCTEST_REQUIRE(nameHit->onTextInput != nullptr);
    typeInto(nameHit, "每日汇总待办");

    // 导航到 step 2（指令）：填 goal
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_save2s2");
    auto* goalHit = ui2s2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    DOCTEST_REQUIRE(goalHit->onTextInput != nullptr);
    typeInto(goalHit, "汇总今日待办");

    // 导航到 step 1（触发）：选定时 + 填 cron
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_save2s1");
    DOCTEST_REQUIRE(callDropdownChange(ui2s1, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_save3");
    DOCTEST_REQUIRE_MESSAGE(ui3.find("agent.dialog.task.cron") != nullptr,
                            "定时态缺 cron 输入");
    mod.setEditCronDraft("0 9 * * 1-5");

    // 导航到 step 3（执行与安全）：改槽位/迭代/破坏性
    eui::Ui ui3s3;
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.step.3.bg"));
    composeTo(ui3s3, mod, kScreen, "agent_ui_save3s3");
    // 槽位 = opus（X 宏表第 3 项，index 2）
    DOCTEST_REQUIRE(callDropdownChange(ui3s3, "agent.dialog.task.slot", 2));
    // 迭代上限 = 7（非默认 10）
    mod.setDialogIterDraft(7);
    // 破坏性 = true
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3s3, "agent.dialog.task.destructive.track"),
                            "破坏性开关 track 缺失");

    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_save4");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui4, "agent.dialog.task.save.bg"),
                            "保存按钮 agent.dialog.task.save.bg 缺失");

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    const auto& row = tasks.value()[0];
    DOCTEST_CHECK_EQ(row.name, std::string("每日汇总待办"));   // 独立名称输入直落
    DOCTEST_CHECK_EQ(row.goal, std::string("汇总今日待办"));
    DOCTEST_CHECK_EQ(row.triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(row.triggerConfig, std::string(R"({"cron":"0 9 * * 1-5"})"));
    DOCTEST_CHECK_EQ(row.modelSlot, std::string("opus"));
    DOCTEST_CHECK_EQ(row.maxIterations, 7);
    DOCTEST_CHECK(row.allowDestructive);

    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_save5");
    DOCTEST_CHECK(ui5.find("agent.dialog.task.goal") == nullptr);
    DOCTEST_CHECK(ui5.find("agent.task." + std::to_string(row.id) + ".title") != nullptr);

    mod.onStop();
    return true;
})

// 「每天」= 不指定时刻：表单不渲染 cron；保存落 trigger_type=daily 且 trigger_config 为空
REGISTER_TEST(agent_ui_daily_trigger_saves_without_time,
              "agent_ui: 触发方式=每天 → 不渲染 cron、落库 trigger_type=daily 且无配置", {
    test_support::TestDb tdb("agentui", "dailysave");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_daily1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_daily2");

    // 导航到 step 2（指令）填 goal
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_daily2s2");
    auto* goalHit = ui2s2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    DOCTEST_REQUIRE(goalHit->onTextInput != nullptr);
    typeInto(goalHit, "每天生成晨会待办");

    // 导航到 step 1（触发）：第 5 项 = 每天（索引 4）
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_daily2s1");
    DOCTEST_REQUIRE_MESSAGE(callDropdownChange(ui2s1, "agent.dialog.task.trigger", 4),
                            "agent.dialog.task.trigger.item.4 不存在");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_daily3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.task.cron") == nullptr,
                          "触发方式=每天 时不应渲染 cron 输入");
    // W4 步骤条：导航 step 3 再保存
    eui::Ui ui3s3;
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.step.3.bg"));
    composeTo(ui3s3, mod, kScreen, "agent_ui_daily3s3");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3s3, "agent.dialog.task.save.bg"),
                            "保存按钮 agent.dialog.task.save.bg 缺失");

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    const auto& row = tasks.value()[0];
    DOCTEST_CHECK_EQ(row.goal, std::string("每天生成晨会待办"));
    DOCTEST_CHECK_EQ(row.triggerType, std::string("daily"));
    DOCTEST_CHECK(row.triggerConfig.empty());

    mod.onStop();
    return true;
})

// ---- 闹钟（once / once_at）表单：新建 / 回填 / 校验 / 切换 / 详情摘要 ----

REGISTER_TEST(agent_ui_once_trigger_form_and_save,
              "agent_ui: 触发方式=闹钟 → 只渲染时间输入；合法未来时间落库 once_at", {
    test_support::TestDb tdb("agentui", "oncesave");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskWithGoal(ui, mod, "agent_ui_oncenew", "明天中午提醒"));
    // 导航到步骤 1（触发）
    eui::Ui ui1;
    composeTo(ui1, mod, kScreen, "agent_ui_oncenew.s1");
    DOCTEST_REQUIRE(clickId(ui1, "agent.dialog.task.step.1.bg"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_oncenew.c");
    // 类型集合 = 6（once 追加在末尾，既有下标 0..4 不动）
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.task.trigger.item.5") != nullptr,
                            "触发方式下拉缺「闹钟」（item.5）");
    DOCTEST_CHECK(ui2.find("agent.dialog.task.trigger.item.6") == nullptr);

    DOCTEST_REQUIRE(callDropdownChange(ui2, "agent.dialog.task.trigger", 5));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_oncenew.d");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.task.once") != nullptr,
                          "闹钟分支缺时间输入 agent.dialog.task.once");
    DOCTEST_CHECK(ui3.find("agent.dialog.task.once.caption") != nullptr);
    DOCTEST_CHECK(ui3.find("agent.dialog.task.once.preview") != nullptr);
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.task.cron") == nullptr,
                          "闹钟分支不应渲染 cron 输入");
    DOCTEST_CHECK(ui3.find("agent.dialog.task.event.0") == nullptr);
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.task.limit") == nullptr,
                          "闹钟天然只跑一次，不应渲染 max_runs 开关");
    DOCTEST_CHECK_MESSAGE(!inputText(ui3, "agent.dialog.task.once").empty(),
                          "闹钟新建缺省应为「现在 + 1 小时」");
    const auto* defaultPreview = ui3.find("agent.dialog.task.once.preview");
    DOCTEST_REQUIRE_MESSAGE(defaultPreview != nullptr, "闹钟缺省值缺预览行");
    DOCTEST_CHECK_MESSAGE(defaultPreview->text.rfind("将在 ", 0) == 0,
                          "闹钟缺省值不是合法未来时间：" + defaultPreview->text);

    const std::tm tomorrowNoon = localNoonTm(1);
    const std::string timeText = formatLocalMinute(tomorrowNoon);
    const int64_t expectedEpoch = epochOfLocal(tomorrowNoon);
    mod.setEditOnceDraft(timeText);
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_oncenew.e");
    const auto* preview = ui4.find("agent.dialog.task.once.preview");
    DOCTEST_REQUIRE_MESSAGE(preview != nullptr, "合法闹钟时间缺预览行");
    DOCTEST_CHECK_MESSAGE(preview->text.find("将在 " + timeText) != std::string::npos,
                          "预览文案未回显规范化时间：" + preview->text);
    DOCTEST_CHECK(preview->text.find("错过不补") != std::string::npos);
    // W4 步骤条：导航 step 3 再保存
    eui::Ui ui4s3;
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.task.step.3.bg"));
    composeTo(ui4s3, mod, kScreen, "agent_ui_oncenew.e3");
    DOCTEST_REQUIRE(clickId(ui4s3, "agent.dialog.task.save.bg"));

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    const auto& row = tasks.value()[0];
    DOCTEST_CHECK_EQ(row.goal, std::string("明天中午提醒"));
    DOCTEST_CHECK_EQ(row.triggerType, std::string("once"));
    DOCTEST_CHECK_MESSAGE(row.triggerConfig.find("cron") == std::string::npos,
                          "闹钟配置残留 cron：" + row.triggerConfig);
    DOCTEST_CHECK_MESSAGE(row.triggerConfig.find("events") == std::string::npos,
                          "闹钟配置残留 events：" + row.triggerConfig);
    DOCTEST_CHECK_MESSAGE(row.triggerConfig.find("max_runs") == std::string::npos,
                          "闹钟不应写入 max_runs：" + row.triggerConfig);
    auto cfg = module::agent::decodeTriggerConfig(row.triggerConfig);
    DOCTEST_REQUIRE_MESSAGE(cfg.isOk(), "闹钟 trigger_config 无法解析：" +
                                            cfg.errorMessage());
    DOCTEST_REQUIRE(cfg.value().onceAt.has_value());
    DOCTEST_CHECK_EQ(*cfg.value().onceAt, expectedEpoch);
    DOCTEST_CHECK(!cfg.value().maxRuns.has_value());
    DOCTEST_CHECK(cfg.value().cron.empty());
    DOCTEST_CHECK(cfg.value().events.empty());

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_once_edit_prefills_and_roundtrips,
              "agent_ui: 闹钟任务编辑 → 时间输入预填；不改时间保存 once_at 不变", {
    test_support::TestDb tdb("agentui", "onceedit");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());

    const std::tm tomorrowNoon = localNoonTm(1);
    const std::string timeText = formatLocalMinute(tomorrowNoon);
    const int64_t expectedEpoch = epochOfLocal(tomorrowNoon);
    auto created = store.createTask("明天中午提醒", "明天中午提醒", "once", "sonnet",
                                    20, true, onceConfig(expectedEpoch));
    DOCTEST_REQUIRE(created.isOk());
    // 预埋执行记忆旧值（store 已停写两列）：UI 保存后必须原样保留
    DOCTEST_REQUIRE(tdb.handle().exec(
        "UPDATE agent_tasks SET context_note='闹钟上下文', auto_note=0 WHERE id=" +
        std::to_string(created.value())).isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_onceedit1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".edit"),
                            "闹钟任务行缺编辑入口");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_onceedit2");
    // W4：编辑打开停在 step 0（基础信息）；导航 step 1 看 once 预填
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_onceedit2s1");
    DOCTEST_REQUIRE_MESSAGE(ui2s1.find("agent.dialog.task.once") != nullptr,
                            "编辑闹钟任务未渲染时间输入");
    DOCTEST_CHECK_EQ(inputText(ui2s1, "agent.dialog.task.once"), timeText);
    DOCTEST_CHECK(ui2s1.find("agent.dialog.task.cron") == nullptr);
    // W4 C1：错误行已移除；step.1 不标错（有效 once_at → 下一步不禁用）
    DOCTEST_CHECK_MESSAGE(clickId(ui2s1, "agent.dialog.task.save.bg"),
                          "下一步按钮不应禁用（有效 once 配置）");
    // 导航 step 2 看 goal 预填
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2s1, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_onceedit2s2");
    DOCTEST_CHECK_EQ(inputText(ui2s2, "agent.dialog.task.goal"),
                     std::string("明天中午提醒"));

    // 导航 step 3 → 保存（不改时间 once_at 不变）
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_onceedit2s3");
    DOCTEST_REQUIRE(clickId(ui2s3, "agent.dialog.task.save.bg"));
    auto updated = store.getTask(created.value());
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().id, created.value());
    DOCTEST_CHECK_EQ(updated.value().name, std::string("明天中午提醒"));
    DOCTEST_CHECK_EQ(updated.value().goal, std::string("明天中午提醒"));
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("once"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig, onceConfig(expectedEpoch));
    DOCTEST_CHECK_EQ(updated.value().modelSlot, std::string("sonnet"));
    DOCTEST_CHECK_EQ(updated.value().maxIterations, 20);
    DOCTEST_CHECK(updated.value().allowDestructive);
    // 保存不写 context_note/auto_note：预埋旧值（'闹钟上下文'/0）原样保留
    auto memCols = tdb.handle().prepare(
        "SELECT context_note, auto_note FROM agent_tasks WHERE id=" +
        std::to_string(created.value()));
    DOCTEST_REQUIRE(memCols.isOk());
    auto memRows = memCols.value().fetchAll();
    DOCTEST_REQUIRE(memRows.isOk());
    DOCTEST_REQUIRE_EQ(memRows.value().size(), 1u);
    DOCTEST_CHECK_EQ(memRows.value()[0].getString("context_note").valueOr("?"),
                     std::string("闹钟上下文"));
    DOCTEST_CHECK_EQ(memRows.value()[0].getInt64("auto_note").valueOr(-1), 0);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_once_rejects_bad_and_past_time,
              "agent_ui: 闹钟非法格式 / 超范围 / 过去时间 → 弹窗报错且不落库", {
    test_support::TestDb tdb("agentui", "oncebad");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskWithGoal(ui, mod, "agent_ui_oncebad", "非法闹钟任务"));
    // W4 步骤条：导航 step 1 设触发=闹钟
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_oncebad.c0");
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    eui::Ui ui2v2;
    composeTo(ui2v2, mod, kScreen, "agent_ui_oncebad.c");
    DOCTEST_REQUIRE(callDropdownChange(ui2v2, "agent.dialog.task.trigger", 5));

    int frameNo = 0;
    // W4 C1：格式/日期非法 → 逐步门控禁下一步 + step.1 标错（"! " 前缀）+ 预览行报具体错误
    // （agent.dialog.task.error 全局错误行已移除）
    auto gatingError = [&](const std::string& text) -> std::string {
        mod.setEditOnceDraft(text);
        const std::string pid = "agent_ui_oncebad.s" + std::to_string(++frameNo);
        eui::Ui frame;
        composeTo(frame, mod, kScreen, pid.c_str());
        if (clickId(frame, "agent.dialog.task.save.bg")) return "<下一步按钮未被禁用>";
        const auto* preview = frame.find("agent.dialog.task.once.preview");
        if (preview == nullptr) return "<错误预览行缺失>";
        if (frame.find("agent.dialog.task.once") == nullptr) {
            return "<弹窗被关闭>";
        }
        return preview->text;
    };

    const std::string formatErr = gatingError("abc");
    DOCTEST_CHECK_MESSAGE(formatErr.find("格式") != std::string::npos,
                          "abc 错误文案：" + formatErr);
    const std::string rangeErr = gatingError("2026-13-40 99:99");
    DOCTEST_CHECK_MESSAGE(rangeErr.find("超出范围") != std::string::npos,
                          "超范围错误文案：" + rangeErr);
    // mktime 会把 2 月 30 日归一化成 3 月 2 日——归一化逐字段比对必须拦住
    const std::string invalidDateErr = gatingError("2026-02-30 12:00");
    DOCTEST_CHECK_MESSAGE(invalidDateErr.find("不存在") != std::string::npos,
                          "2026-02-30 错误文案：" + invalidDateErr);

    // F1 门控行为补锚：禁用态「下一步」被点（空 onClick 或内部守卫）不得改步——
    // 仍停在触发步（step 1 独有的 once 输入面在，即未跳 step 2）
    {
        eui::Ui d0;
        composeTo(d0, mod, kScreen, "agent_ui_oncebad.dc0");
        const auto* saveBg = d0.find("agent.dialog.task.save.bg");
        DOCTEST_REQUIRE(saveBg != nullptr);
        if (saveBg->onClick) saveBg->onClick();
        eui::Ui d1;
        composeTo(d1, mod, kScreen, "agent_ui_oncebad.dc1");
        DOCTEST_CHECK_MESSAGE(d1.find("agent.dialog.task.once") != nullptr,
                              "禁用「下一步」点击后仍应停在触发步（currentStep 未变）");
    }

    // 过去时间：解析合法（门控放行），预览行报红 + 保存被拒（弹窗不关）
    mod.setEditOnceDraft(formatLocalMinute(localNoonTm(-1)));
    eui::Ui pastF;
    composeTo(pastF, mod, kScreen, "agent_ui_oncebad.past");
    const auto* pastPrev = pastF.find("agent.dialog.task.once.preview");
    DOCTEST_REQUIRE_MESSAGE(pastPrev != nullptr, "过去时间未渲染预览行");
    DOCTEST_CHECK_MESSAGE(pastPrev->text.find("晚于当前时间") != std::string::npos,
                          "过去时间错误文案：" + pastPrev->text);
    DOCTEST_REQUIRE(clickId(pastF, "agent.dialog.task.step.3.bg"));
    eui::Ui pastS3;
    composeTo(pastS3, mod, kScreen, "agent_ui_oncebad.past3");
    DOCTEST_REQUIRE(clickId(pastS3, "agent.dialog.task.save.bg"));
    eui::Ui pastAfter;
    composeTo(pastAfter, mod, kScreen, "agent_ui_oncebad.pastAfter");
    DOCTEST_CHECK_MESSAGE(pastAfter.find("agent.dialog.task.panel") != nullptr,
                          "过去时间保存后弹窗被关闭（应拒绝）");

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_once_switch_type_clears_payload,
              "agent_ui: 定时↔闹钟切换保存 → 不残留对方的 trigger_config 载荷", {
    test_support::TestDb tdb("agentui", "onceswitch");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    const std::tm tomorrowNoon = localNoonTm(1);
    const int64_t expectedEpoch = epochOfLocal(tomorrowNoon);

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskWithGoal(ui, mod, "agent_ui_onceswitch", "切换类型任务"));
    // W4 步骤条：导航 step 1 切触发类型
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_onceswitch.c0");
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    eui::Ui ui2v2;
    composeTo(ui2v2, mod, kScreen, "agent_ui_onceswitch.c");
    DOCTEST_REQUIRE(callDropdownChange(ui2v2, "agent.dialog.task.trigger", 1));
    mod.setEditCronDraft("0 9 * * *");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_onceswitch.d");
    DOCTEST_REQUIRE(callDropdownChange(ui3, "agent.dialog.task.trigger", 5));
    mod.setEditOnceDraft(formatLocalMinute(tomorrowNoon));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_onceswitch.e");
    // 导航 step 3 保存
    eui::Ui ui4s3;
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.task.step.3.bg"));
    composeTo(ui4s3, mod, kScreen, "agent_ui_onceswitch.e3");
    DOCTEST_REQUIRE(clickId(ui4s3, "agent.dialog.task.save.bg"));

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    const auto& row = tasks.value()[0];
    DOCTEST_CHECK_EQ(row.triggerConfig, onceConfig(expectedEpoch));
    DOCTEST_CHECK(row.triggerConfig.find("cron") == std::string::npos);

    // 反向：编辑闹钟 → 切回定时 → 保存只写 cron，once_at 不残留
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_onceswitch.f");
    DOCTEST_REQUIRE(clickId(ui5, "agent.task." + std::to_string(row.id) + ".edit"));
    eui::Ui ui6;
    composeTo(ui6, mod, kScreen, "agent_ui_onceswitch.g");
    // W4 步骤条：导航 step 1 切回定时
    eui::Ui ui6s1;
    DOCTEST_REQUIRE(clickId(ui6, "agent.dialog.task.step.1.bg"));
    composeTo(ui6s1, mod, kScreen, "agent_ui_onceswitch.g1");
    DOCTEST_REQUIRE(callDropdownChange(ui6s1, "agent.dialog.task.trigger", 1));
    eui::Ui ui7;
    composeTo(ui7, mod, kScreen, "agent_ui_onceswitch.h");
    mod.setEditCronDraft("30 8 * * 1");
    // 导航 step 3 保存
    eui::Ui ui7s3;
    DOCTEST_REQUIRE(clickId(ui7, "agent.dialog.task.step.3.bg"));
    composeTo(ui7s3, mod, kScreen, "agent_ui_onceswitch.h3");
    DOCTEST_REQUIRE(clickId(ui7s3, "agent.dialog.task.save.bg"));

    auto updated = store.getTask(row.id);
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig, std::string(R"({"cron":"30 8 * * 1"})"));
    DOCTEST_CHECK(updated.value().triggerConfig.find("once_at") == std::string::npos);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_once_detail_shows_alarm_label_and_time,
              "agent_ui: 闹钟任务详情摘要显示「闹钟」标签与本地时间", {
    test_support::TestDb tdb("agentui", "oncedetail");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());

    const std::tm tomorrowNoon = localNoonTm(1);
    const std::string timeText = formatLocalMinute(tomorrowNoon);
    auto created = store.createTask("闹钟任务", "闹钟摘要", "once", "default", 10,
                                    false, onceConfig(epochOfLocal(tomorrowNoon)));
    DOCTEST_REQUIRE(created.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_oncedetail1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_oncedetail2");
    // 头部状态行的触发摘要（方案 A）：once 型 =「闹钟 · <本地时间>」，不出现 cron
    const auto* meta = ui2.find("agent.dialog.detail.header.meta");
    DOCTEST_REQUIRE_MESSAGE(meta != nullptr, "详情弹窗缺头部触发摘要 .header.meta");
    DOCTEST_CHECK_MESSAGE(meta->text.find("闹钟") != std::string::npos,
                          "头部摘要未显示「闹钟」：" + meta->text);
    DOCTEST_CHECK_MESSAGE(meta->text.find(timeText) != std::string::npos,
                          "头部摘要未显示闹钟本地时间：" + meta->text);
    DOCTEST_CHECK_MESSAGE(meta->text.find("cron") == std::string::npos,
                          "闹钟详情不应出现 cron：" + meta->text);
    // 任务配置 tab（方案 A：配置收进 tab 2）：闹钟时间落 .config.body.schedule.value
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.2.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_oncedetail3");
    const auto* scheduleVal =
        ui3.find("agent.dialog.detail.config.body.schedule.value");
    DOCTEST_REQUIRE_MESSAGE(scheduleVal != nullptr,
                            "任务配置 tab 缺 .config.body.schedule.value");
    DOCTEST_CHECK_MESSAGE(scheduleVal->text.find(timeText) != std::string::npos,
                          "配置定时字段未显示闹钟时间：" + scheduleVal->text);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_once_edit_bad_config_shows_problem_without_fallback,
              "agent_ui: 闹钟坏配置编辑 → 输入留空 + 配置异常，不回落成手动", {
    test_support::TestDb tdb("agentui", "oncebadcfg");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());

    // decode 能过但缺 once_at：一次性类型 + cron 载荷（语义分裂，列表也会挂配置异常）
    auto created = store.createTask("坏闹钟", "坏闹钟目标", "once", "default", 10,
                                    false, R"({"cron":"0 9 * * *"})");
    DOCTEST_REQUIRE(created.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_oncebadcfg1");
    DOCTEST_REQUIRE(clickId(ui,
                            "agent.task." + std::to_string(created.value()) + ".edit"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_oncebadcfg2");
    // W4 步骤条：导航 step 1 看触发态
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_oncebadcfg2s1");
    // 触发方式仍是闹钟（有 once 时间输入 = 未回落手动；回落手动不会有该节点）
    DOCTEST_REQUIRE_MESSAGE(ui2s1.find("agent.dialog.task.once") != nullptr,
                            "坏配置编辑回落成了其它触发类型");
    DOCTEST_CHECK(inputText(ui2s1, "agent.dialog.task.once").empty());
    // W4 C1：全局错误行已移除；空输入经预览行报红提示
    const auto* prev = ui2s1.find("agent.dialog.task.once.preview");
    DOCTEST_REQUIRE_MESSAGE(prev != nullptr, "坏闹钟配置编辑未渲染预览行");
    DOCTEST_CHECK_MESSAGE(prev->text.find("请填写") != std::string::npos,
                          "空输入预览提示缺失：" + prev->text);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_save_rejects_bad_cron,
              "agent_ui: 非法 cron 保存被拒（弹窗内报错 / 不落库 / 弹窗不关）", {
    test_support::TestDb tdb("agentui", "badcron");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_bad1"));
    // W4 步骤条：导航 step 2 填 goal
    eui::Ui ui2s2;
    composeTo(ui2s2, mod, kScreen, "agent_ui_bad2s2");
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.2.bg"));
    eui::Ui ui2s2v2;
    composeTo(ui2s2v2, mod, kScreen, "agent_ui_bad2s2v2");
    auto* goalHit = ui2s2v2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    typeInto(goalHit, "非法 cron 任务");
    // 导航 step 1：设定时 + 非法 cron
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2s2v2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_bad2s1");
    DOCTEST_REQUIRE(callDropdownChange(ui2s1, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_bad3");
    mod.setEditCronDraft("0 9 * * 1-5x");
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_bad4");
    // W4 C1：逐步门控——非法 cron 禁下一步 + step.1 标错 + 预览行报错（错误行已移除）
    DOCTEST_CHECK_MESSAGE(!clickId(ui4, "agent.dialog.task.save.bg"),
                          "非法 cron 未禁用下一步按钮");
    const auto* cprev = ui4.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(cprev != nullptr, "非法 cron 未渲染预览行");
    DOCTEST_CHECK(cprev->text.find("cron") != std::string::npos);
    // 直跳 step 3 点创建 → saveTaskDialog 二次校验拒绝，弹窗不关
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.task.step.3.bg"));
    eui::Ui ui4s3;
    composeTo(ui4s3, mod, kScreen, "agent_ui_bad4s3");
    DOCTEST_REQUIRE(clickId(ui4s3, "agent.dialog.task.save.bg"));
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_bad5");
    DOCTEST_CHECK_MESSAGE(ui5.find("agent.dialog.task.panel") != nullptr,
                          "非法 cron 保存后弹窗被关闭（应拒绝）");
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_cancel_close_backdrop_esc,
              "agent_ui: 取消 / ✕ / 点遮罩 / Esc 四条关闭路径都不落库", {
    test_support::TestDb tdb("agentui", "dlgclose");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    // 打开新建弹窗并敲入 goal；返回 false = 夹具没建成，调用方 REQUIRE 转红
    auto typedGoalAndCompose = [&](const char* pageId, eui::Ui& ui) -> bool {
        eui::Ui page;
        if (!openNewTaskDialog(page, mod, pageId)) return false;
        // openNewTaskDialog 是先 compose 后点击——弹窗节点要再 compose 一帧才可见
        composeTo(page, mod, kScreen, pageId);
        // W4 步骤条：goal 在 step 2，先导航
        if (!clickId(page, "agent.dialog.task.step.2.bg")) return false;
        composeTo(ui, mod, kScreen, pageId);
        auto* hit = ui.find("agent.dialog.task.goal.hit");
        if (hit == nullptr) return false;
        typeInto(hit, "不该落库的目标");
        return true;
    };

    // ① 取消
    eui::Ui ui1;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close1", ui1));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui1, "agent.dialog.task.cancel.bg"),
                            "取消按钮 agent.dialog.task.cancel.bg 缺失");
    eui::Ui c1;
    composeTo(c1, mod, kScreen, "agent_ui_close1b");
    DOCTEST_CHECK(c1.find("agent.dialog.task.goal") == nullptr);

    // ② ✕
    eui::Ui ui2;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close2", ui2));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.dialog.task.close"),
                            "✕ 关闭面 agent.dialog.task.close 缺失");
    eui::Ui c2;
    composeTo(c2, mod, kScreen, "agent_ui_close2b");
    DOCTEST_CHECK(c2.find("agent.dialog.task.goal") == nullptr);

    // ③ 点遮罩（dialog 组件 backdrop onClick → onOpenChange(false)）
    eui::Ui ui3;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close3", ui3));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3, "agent.dialog.task.backdrop"),
                            "遮罩 agent.dialog.task.backdrop 缺失");
    eui::Ui c3;
    composeTo(c3, mod, kScreen, "agent_ui_close3b");
    DOCTEST_CHECK(c3.find("agent.dialog.task.goal") == nullptr);

    // ④ Esc（焦点不在输入框时经模块级 onKeyEvent）
    eui::Ui ui4;
    DOCTEST_REQUIRE(typedGoalAndCompose("agent_ui_close4", ui4));
    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK(mod.onKeyEvent(esc));
    eui::Ui c4;
    composeTo(c4, mod, kScreen, "agent_ui_close4b");
    DOCTEST_CHECK(c4.find("agent.dialog.task.goal") == nullptr);

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_edit_dialog_prefills_and_updates,
              "agent_ui: 行内「编辑」→ 弹窗预填（goal/cron/slot/iter 与库一致）→ 保存走 updateTask", {
    test_support::TestDb tdb("agentui", "dlgedit");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("旧任务", "旧目标", "schedule", "sonnet", 20, true,
                                    R"({"cron":"0 9 * * 1-5"})");
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_edit1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".edit"),
                            "任务行缺 agent.task.<id>.edit");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_edit2");
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.task.name") != nullptr,
                            "编辑态未打开任务弹窗");
    // W4 步骤条：step 2 看 goal 预填
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_edit2s2");
    DOCTEST_CHECK_EQ(inputText(ui2s2, "agent.dialog.task.goal"), std::string("旧目标"));
    // step 1 看 cron 预填
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_edit2s1");
    DOCTEST_REQUIRE_MESSAGE(ui2s1.find("agent.dialog.task.cron") != nullptr,
                            "schedule 任务未渲染 cron 输入");
    DOCTEST_CHECK_EQ(inputText(ui2s1, "agent.dialog.task.cron"),
                     std::string("0 9 * * 1-5"));
    // step 3 看 iter 预填 + 改槽位
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s1, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_edit2s3");
    DOCTEST_CHECK_EQ(inputText(ui2s3, "agent.dialog.task.iter"), std::string("20"));

    DOCTEST_REQUIRE(callDropdownChange(ui2s3, "agent.dialog.task.slot", 3));
    mod.setDialogIterDraft(3);
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_edit3");
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.save.bg"));

    auto updated = store.getTask(created.value());
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value().goal, std::string("旧目标"));
    DOCTEST_CHECK_EQ(updated.value().triggerType, std::string("schedule"));
    DOCTEST_CHECK_EQ(updated.value().triggerConfig,
                     std::string(R"({"cron":"0 9 * * 1-5"})"));            // 预填后原样回写
    DOCTEST_CHECK_EQ(updated.value().modelSlot, std::string("haiku"));
    DOCTEST_CHECK_EQ(updated.value().maxIterations, 3);
    DOCTEST_CHECK(updated.value().allowDestructive);                       // 预填 true 未翻
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_edit4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.goal") == nullptr);

    // 启停是行内独立动作：不进编辑弹窗，文案随 enabled 翻转且落库取反
    const auto* toggle = ui4.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_CHECK_EQ(toggle->text, std::string("禁用"));
    DOCTEST_REQUIRE(toggle->onClick != nullptr);
    toggle->onClick();
    auto disabled = store.getTask(created.value());
    DOCTEST_REQUIRE(disabled.isOk());
    DOCTEST_CHECK_FALSE(disabled.value().enabled);
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_edit5");
    const auto* toggle2 = ui5.find("agent.task." + tid + ".toggle");
    DOCTEST_REQUIRE(toggle2 != nullptr);
    DOCTEST_CHECK_EQ(toggle2->text, std::string("启用"));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_run_button_opens_detail_dialog,
              "agent_ui: 点「启动」→ 详情弹窗立即打开；跑完展开消息区后最终 assistant 回复可见", {
    test_support::TestDb tdb("agentui", "runopens");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("端到端任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"最终答复"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_run1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 启动后第一帧详情弹窗就该开，不等 run 跑完。此刻 run 行可能还没落库、消息区显示
    // 「还没有消息」，所以这里只能钉弹窗本体 + 配置摘要——钉消息会偶发红
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_run2");
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.detail") != nullptr,
                            "点启动后未立即打开详情弹窗");
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.detail.info") == nullptr,
                          ".info 已退休（触发摘要在 .header.meta），不得回潮");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.header.meta") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav") != nullptr);

    // 有界轮询（10ms 步进 ≤5s）+ 每轮 recompose 跟进刷新（弹窗随刷新周期更新）
    bool completed = false;
    eui::Ui ui3;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui3, mod, kScreen, "agent_ui_run_poll");
        auto runs = store.listRuns(created.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "run 线程 5s 内未落库 completed");

    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_run4");
    const auto* status = ui4.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("成功") != std::string::npos);
    const auto* run0 = ui4.find("agent.dialog.detail.runlist.0.status");
    DOCTEST_REQUIRE_MESSAGE(run0 != nullptr, "详情弹窗缺 run 列表行状态");
    DOCTEST_CHECK(run0->text.find("成功") != std::string::npos);
    // 点 run 行自动切「运行详情」tab（方案 A 要点 ③），再展开消息区
    //（D6 默认折叠：先开区，「最终 assistant 默认展开」规则生效）
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.detail.runlist.0.bg"));
    eui::Ui ui4b;
    composeTo(ui4b, mod, kScreen, "agent_ui_run4b");
    DOCTEST_REQUIRE(clickId(ui4b, "agent.dialog.detail.msgs.toggle"));
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_run5");
    const auto* finalBlk = ui5.find("agent.dialog.detail.msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "展开消息区后最终 assistant 文本块缺失（msg.1.blk.0）");
    DOCTEST_CHECK_MESSAGE(finalBlk->text.find("最终答复") != std::string::npos,
                          "最终回复内容未渲染，实测: " + finalBlk->text);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_dialog_shows_config_runs_messages,
              "agent_ui: 详情弹窗头部摘要+任务配置 tab 字段 + run 列表倒序/最近高亮 + 消息默认折叠（展开后最终回复展开）", {
    test_support::TestDb tdb("agentui", "detaildlg");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("详情任务", "把逾期的待办全部完成", "schedule",
                                    "sonnet", 20, true,
                                    R"({"cron":"0 9 * * 1-5"})");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    // 两次 run：先 limited（带 error 与 token），后 completed（最近一次）——run 列表的
    // 倒序与「最近一次高亮」判据依赖这个顺序
    auto r1 = store.startRun(taskId);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r1.value(), 0, "user",
                                        R"([{"type":"text","text":"第一个 run"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "limited", 7, int64_t(190000),
                                    int64_t(15000),
                                    std::string("token 累计超上限 200K")).isOk());
    const std::string longOutput(600, 'z');
    auto r2 = store.startRun(taskId);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r2.value(), 0, "user",
                                        R"([{"type":"text","text":"查一下"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r2.value(), 1, "assistant",
                        R"([{"type":"text","text":"最终答复内容"},)"
                        R"({"type":"tool_use","id":"tu1","name":"todo.list",)"
                        R"("input":{"limit":5}}])",
                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r2.value(), 2, "user",
                        R"([{"type":"tool_result","tool_use_id":"tu1",)"
                        R"("content":[{"type":"text","text":")" + longOutput +
                        R"("}],"is_error":false}])",
                        std::string("tu1")).isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_dd1");
    DOCTEST_REQUIRE_MESSAGE(openDetailDialog(ui, mod, tid),
                            "任务行缺 agent.task.<id>.detail");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dd2");

    // 头部触发摘要（方案 A）：定时型 =「定时 · cron <表达式>」；目标/破坏性在配置 tab
    const auto* info = ui2.find("agent.dialog.detail.header.meta");
    DOCTEST_REQUIRE_MESSAGE(info != nullptr,
                            "详情弹窗缺头部触发摘要 agent.dialog.detail.header.meta");
    DOCTEST_CHECK(info->text.find("定时") != std::string::npos);
    DOCTEST_CHECK(info->text.find("0 9 * * 1-5") != std::string::npos);
    DOCTEST_CHECK(info->text.find("把逾期的待办全部完成") == std::string::npos);
    DOCTEST_CHECK(info->text.find("破坏性") == std::string::npos);

    // 切任务配置 tab（方案 A：配置收进 tab 2，浮层退役）：字段值逐项核对
    const auto* runlistBefore = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(runlistBefore != nullptr);
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.2.bg"));
    eui::Ui uiP;
    composeTo(uiP, mod, kScreen, "agent_ui_ddP");
    const auto* goalV = uiP.find("agent.dialog.detail.config.body.goal.value");
    DOCTEST_REQUIRE_MESSAGE(goalV != nullptr, "配置 tab 缺 .config.body.goal.value");
    DOCTEST_CHECK(goalV->text.find("把逾期的待办全部完成") != std::string::npos);
    DOCTEST_CHECK(uiP.find("agent.dialog.detail.config.body.trigger.value") !=
                  nullptr);
    DOCTEST_CHECK(
        uiP.find("agent.dialog.detail.config.body.schedule.value") != nullptr);
    const auto* modelV = uiP.find("agent.dialog.detail.config.body.model.value");
    DOCTEST_REQUIRE(modelV != nullptr);
    DOCTEST_CHECK_EQ(modelV->text, std::string("sonnet"));
    const auto* iterV2 =
        uiP.find("agent.dialog.detail.config.body.iterations.value");
    DOCTEST_REQUIRE(iterV2 != nullptr);
    DOCTEST_CHECK_EQ(iterV2->text, std::string("20"));
    const auto* destrV =
        uiP.find("agent.dialog.detail.config.body.destructive.value");
    DOCTEST_REQUIRE(destrV != nullptr);
    DOCTEST_CHECK_EQ(destrV->text, std::string("允许"));
    const auto* limitV =
        uiP.find("agent.dialog.detail.config.body.limit.value");
    DOCTEST_REQUIRE(limitV != nullptr);
    DOCTEST_CHECK_EQ(limitV->text, std::string("不限次数"));
    // tab 互斥（方案 A）：配置 tab 在场时 runlist / 运行详情块退场
    DOCTEST_CHECK(uiP.find("agent.dialog.detail.runlist") == nullptr);
    DOCTEST_CHECK(uiP.find("agent.dialog.detail.detail") == nullptr);
    // 无配置异常 → problem 字段不渲染（零噪音）
    DOCTEST_CHECK(uiP.find("agent.dialog.detail.config.body.problem") == nullptr);

    // run 列表：倒序（最近一次在前）+ 行容器三列（.time/.status/.duration）+
    // 最近一次 .bg 选中底高亮（P2 容器化：文本在子节点）
    const auto* run0 = ui2.find("agent.dialog.detail.runlist.0");
    const auto* run1 = ui2.find("agent.dialog.detail.runlist.1");
    DOCTEST_REQUIRE_MESSAGE(run0 != nullptr && run1 != nullptr,
                            "详情弹窗缺 run 列表行容器");
    const auto* run0Status = ui2.find("agent.dialog.detail.runlist.0.status");
    const auto* run1Status = ui2.find("agent.dialog.detail.runlist.1.status");
    DOCTEST_REQUIRE_MESSAGE(run0Status != nullptr && run1Status != nullptr,
                            "runlist 行缺 .status 子节点");
    DOCTEST_CHECK_EQ(run0Status->text, std::string("成功"));
    DOCTEST_CHECK_EQ(run1Status->text, std::string("受限"));
    // 时间列钉 §11.1 格式：MM-DD HH:MM:SS（14 字符）
    const auto* run0Time = ui2.find("agent.dialog.detail.runlist.0.time");
    DOCTEST_REQUIRE_MESSAGE(run0Time != nullptr, "runlist 行缺 .time 子节点");
    DOCTEST_CHECK_EQ(run0Time->text.size(), static_cast<size_t>(14));
    DOCTEST_CHECK(run0Time->text[2] == '-' && run0Time->text[5] == ' ');
    const auto* run0Bg = ui2.find("agent.dialog.detail.runlist.0.bg");
    const auto* run1Bg = ui2.find("agent.dialog.detail.runlist.1.bg");
    DOCTEST_REQUIRE(run0Bg != nullptr && run1Bg != nullptr);
    DOCTEST_CHECK_MESSAGE(run0Bg->color.a > run1Bg->color.a,
                          "最近一次 run 未高亮（选中 .bg 应为 primarySoftBg）");
    const auto* errRow = ui2.find("agent.dialog.detail.runlist.1.err");
    DOCTEST_REQUIRE(errRow != nullptr);
    DOCTEST_CHECK(errRow->text.find("200K") != std::string::npos);

    // 运行详情 tab（点 nav 或点 run 行自动切档）：消息区默认折叠——无 .msgscroll、
    // 有 .msgs.hint、copyfinal 仍可见；点 .msgs.toggle 展开后：最终 assistant 文本块
    // 默认展开、其余（用户/工具）折叠
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui uiT;
    composeTo(uiT, mod, kScreen, "agent_ui_ddT");
    DOCTEST_REQUIRE_MESSAGE(uiT.find("agent.dialog.detail.msgscroll") == nullptr,
                            "消息区默认应折叠（无 .msgscroll）");
    DOCTEST_CHECK(uiT.find("agent.dialog.detail.msgs.hint") != nullptr);
    DOCTEST_CHECK(uiT.find("agent.dialog.detail.msgs.copyfinal") != nullptr);
    DOCTEST_REQUIRE(clickId(uiT, "agent.dialog.detail.msgs.toggle"));
    eui::Ui uiM;
    composeTo(uiM, mod, kScreen, "agent_ui_ddM");
    DOCTEST_REQUIRE(uiM.find("agent.dialog.detail.msgscroll") != nullptr);
    const auto* finalBlk = uiM.find("agent.dialog.detail.msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "消息区展开后最终 assistant 文本块未默认展开");
    DOCTEST_CHECK(finalBlk->text.find("最终答复内容") != std::string::npos);
    // 用户消息：只有 .preview，无块
    const auto* userPreview = uiM.find("agent.dialog.detail.msg.0.preview");
    DOCTEST_REQUIRE_MESSAGE(userPreview != nullptr, "用户消息未走折叠态");
    DOCTEST_CHECK(uiM.find("agent.dialog.detail.msg.0.blk.0") == nullptr);
    // tool_result 消息默认折叠
    DOCTEST_CHECK(uiM.find("agent.dialog.detail.msg.2.blk.0.out") == nullptr);

    // token 断言（tab 1 元信息网格）：点选旧 run（runlist.1 = limited，
    // 190000+15000=205000）→ .detail.meta 读千分位 token；无 token run 显示「—」。
    // runlist.1.bg 在 ui2（tab 0 帧）上点：选中旧 run 并自动切回 tab 1
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.1.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dd3");
    const auto* inputV = ui3.find("agent.dialog.detail.detail.meta.input.value");
    const auto* outputV = ui3.find("agent.dialog.detail.detail.meta.output.value");
    const auto* iterV = ui3.find("agent.dialog.detail.detail.meta.iterations.value");
    DOCTEST_REQUIRE_MESSAGE(inputV != nullptr && outputV != nullptr &&
                                iterV != nullptr,
                            "运行详情缺 .detail.meta token/迭代行（2 列网格）");
    DOCTEST_CHECK_EQ(inputV->text, std::string("190,000"));
    DOCTEST_CHECK_EQ(outputV->text, std::string("15,000"));
    DOCTEST_CHECK_EQ(iterV->text, std::string("7"));
    const auto* errFull = ui3.find("agent.dialog.detail.detail.error.text");
    DOCTEST_REQUIRE_MESSAGE(errFull != nullptr, "失败 run 缺 .detail.error.text");
    DOCTEST_CHECK(errFull->text.find("token 累计超上限 200K") != std::string::npos);
    // 切回最近一次（无 token run）：回执行记录 tab 点行（自动回运行详情），输入/输出转「—」
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.detail.nav.item.0.bg"));
    eui::Ui ui3b;
    composeTo(ui3b, mod, kScreen, "agent_ui_dd3b");
    DOCTEST_REQUIRE(clickId(ui3b, "agent.dialog.detail.runlist.0.bg"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_dd4");
    const auto* inputBack = ui4.find("agent.dialog.detail.detail.meta.input.value");
    DOCTEST_REQUIRE(inputBack != nullptr);
    DOCTEST_CHECK_EQ(inputBack->text, std::string("—"));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_message_toggle_and_copy,
              "agent_ui: 详情弹窗内消息展开→tool_use chip + tool_result 全文 + 高度增大 + 复制 chip", {
    test_support::TestDb tdb("agentui", "msgtoggle");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("日志任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    auto r = store.startRun(taskId);
    DOCTEST_REQUIRE(r.isOk());
    const std::string longOutput(600, 'z');
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                        R"([{"type":"text","text":"查一下"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r.value(), 1, "assistant",
                        R"([{"type":"text","text":"我来查"},)"
                        R"({"type":"tool_use","id":"tu1","name":"todo.list",)"
                        R"("input":{"limit":5}}])",
                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        r.value(), 2, "user",
                        R"([{"type":"tool_result","tool_use_id":"tu1",)"
                        R"("content":[{"type":"text","text":")" + longOutput +
                        R"("}],"is_error":false}])",
                        std::string("tu1")).isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const std::string base = "agent.dialog.detail";

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_mt1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_mt2");
    // 消息区在「运行详情」tab（方案 A 三档）：切档后默认折叠（D6），
    // 先展开消息区，块级比较才有对象
    DOCTEST_REQUIRE(clickId(ui2, base + ".nav.item.1.bg"));
    eui::Ui ui2a;
    composeTo(ui2a, mod, kScreen, "agent_ui_mt2a");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2a, base + ".msgs.toggle"),
                            "消息区缺 .msgs.toggle");
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_mt2b");
    const auto* m2c = ui2b.find(base + ".msg.2");
    DOCTEST_REQUIRE(m2c != nullptr);
    const float collapsedH = m2c->frame.height;
    DOCTEST_CHECK(collapsedH > 0.0f);
    // assistant 混合块（seq1）默认展开：tool_use chip 可见——文案「工具 · name」（无 emoji）
    const auto* chip = ui2b.find(base + ".msg.1.blk.1");
    DOCTEST_REQUIRE_MESSAGE(chip != nullptr, "tool_use 块缺失 msg.1.blk.1");
    DOCTEST_CHECK_MESSAGE(chip->text.find("todo.list") != std::string::npos &&
                              chip->text.find("工具 · ") == 0,
                          "tool chip 应为「工具 · name」形态，实测: " + chip->text);
    DOCTEST_CHECK(chip->text.find("🔧") == std::string::npos);

    DOCTEST_REQUIRE_MESSAGE(clickId(ui2b, base + ".msg.2.toggle"),
                            "msg.2 展开 chip 缺失");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_mt3");
    const auto* m2e = ui3.find(base + ".msg.2");
    DOCTEST_REQUIRE(m2e != nullptr);
    DOCTEST_CHECK_MESSAGE(m2e->frame.height > collapsedH,
                          "展开高度应大于收起高 " + std::to_string(collapsedH) +
                              "，实测 " + std::to_string(m2e->frame.height));
    const auto* out = ui3.find(base + ".msg.2.blk.0.out");
    DOCTEST_REQUIRE(out != nullptr);
    DOCTEST_CHECK_MESSAGE(out->text.find("zzz") != std::string::npos &&
                              out->text.size() >= 600,
                          "tool_result 输出全文未渲染，实测 " +
                              std::to_string(out->text.size()) + " 字节");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3, base + ".msg.2.copy"),
                            "复制 chip 缺失");
    const auto* copyBtn = ui3.find(base + ".msg.2.copy");
    DOCTEST_CHECK_EQ(copyBtn->text, std::string("复制"));

    // 再点收起 → 回到折叠态
    DOCTEST_REQUIRE(clickId(ui3, base + ".msg.2.toggle"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_mt4");
    DOCTEST_CHECK(ui4.find(base + ".msg.2.blk.0.out") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_copy_final_reply_chip,
               "agent_ui: 详情弹窗「消息（N）」标题行复制回复 chip（tool 收尾取最后 assistant / 无回复不渲染）", {
    test_support::TestDb tdb("agentui", "copyfinal");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    const std::string base = "agent.dialog.detail";

    // 任务 A：run 以 tool_result 收尾——最终回复在中间的 assistant（钉「最后一条
    // assistant ≠ 最后一条消息」风险）
    auto createdA = store.createTask("任务A", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(createdA.isOk());
    const std::string tidA = std::to_string(createdA.value());
    auto rA = store.startRun(createdA.value());
    DOCTEST_REQUIRE(rA.isOk());
    DOCTEST_REQUIRE(store.appendMessage(rA.value(), 0, "user",
                                        R"([{"type":"text","text":"查一下"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        rA.value(), 1, "assistant",
                        R"([{"type":"text","text":"我先查。"},)"
                        R"({"type":"tool_use","id":"tu1","name":"todo.list","input":{"limit":5}},)"
                        R"({"type":"text","text":"查完了，共 2 条。"}])",
                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(
                        rA.value(), 2, "user",
                        R"([{"type":"tool_result","tool_use_id":"tu1",)"
                        R"("content":[{"type":"text","text":"ok"}],"is_error":false}])",
                        std::string("tu1")).isOk());
    DOCTEST_REQUIRE(store.finishRun(rA.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    // 任务 B：run 只有 user 消息（刚起 / 全 tool_use）→ 无最终回复
    auto createdB = store.createTask("任务B", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(createdB.isOk());
    const std::string tidB = std::to_string(createdB.value());
    auto rB = store.startRun(createdB.value());
    DOCTEST_REQUIRE(rB.isOk());
    DOCTEST_REQUIRE(store.appendMessage(rB.value(), 0, "user",
                                        R"([{"type":"text","text":"刚起跑"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(rB.value(), "limited", 0, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 任务 A：chip 存在、可点、几何落在弹窗内且在标题行右侧（「运行详情」tab 内）
    eui::Ui uiA1;
    composeTo(uiA1, mod, kScreen, "agent_ui_cf1");
    DOCTEST_REQUIRE(openDetailDialog(uiA1, mod, tidA));
    eui::Ui uiA2;
    composeTo(uiA2, mod, kScreen, "agent_ui_cf2");
    DOCTEST_REQUIRE(clickId(uiA2, base + ".nav.item.1.bg"));
    eui::Ui uiA2n;
    composeTo(uiA2n, mod, kScreen, "agent_ui_cf2n");
    const std::string chipId = base + ".msgs.copyfinal";
    const auto* chip = uiA2n.find(chipId);
    DOCTEST_REQUIRE_MESSAGE(chip != nullptr, "有最终回复时缺「复制回复」chip");
    DOCTEST_CHECK_EQ(chip->text, std::string("复制回复"));
    const auto* title = uiA2n.find(base + ".msgs.title");
    const auto* panel = uiA2n.find(base);
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_REQUIRE(panel != nullptr);
    // 无头几何断言：chip 与标题带纵向重叠（chip 高 32 比标题行 18 高，居中对齐上移 7）、
    // chip 在标题文本右侧、整体在弹窗内
    DOCTEST_CHECK_MESSAGE(chip->frame.y < title->frame.y + 18.0f &&
                              chip->frame.y + chip->frame.height >
                                  title->frame.y,
                          "chip 未与「消息（N）」标题同带: chip.y=" +
                              std::to_string(chip->frame.y) + " title.y=" +
                              std::to_string(title->frame.y));
    DOCTEST_CHECK_MESSAGE(chip->frame.x > title->frame.x,
                          "chip 应在标题右侧");
    DOCTEST_CHECK(frameContained(*panel, *chip));
    DOCTEST_REQUIRE_MESSAGE(clickId(uiA2n, chipId + ".bg"),
                            "复制回复 chip 无命中面 <id>.bg");
    // 既有逐条复制 chip 零回归：展开消息区后 msg.2 展开仍有 .copy
    DOCTEST_REQUIRE(clickId(uiA2n, base + ".msgs.toggle"));
    eui::Ui uiA2b;
    composeTo(uiA2b, mod, kScreen, "agent_ui_cf2b");
    DOCTEST_REQUIRE(clickId(uiA2b, base + ".msg.2.toggle"));
    eui::Ui uiA3;
    composeTo(uiA3, mod, kScreen, "agent_ui_cf3");
    DOCTEST_CHECK(uiA3.find(base + ".msg.2.copy") != nullptr);
    DOCTEST_CHECK(uiA3.find(chipId) != nullptr);

    // 任务 B：无 assistant 回复 → chip 不渲染（在「运行详情」tab 里断言缺席）
    eui::Ui uiB1;
    composeTo(uiB1, mod, kScreen, "agent_ui_cf4");
    DOCTEST_REQUIRE(openDetailDialog(uiB1, mod, tidB));
    eui::Ui uiB2;
    composeTo(uiB2, mod, kScreen, "agent_ui_cf5");
    DOCTEST_REQUIRE(clickId(uiB2, base + ".nav.item.1.bg"));
    eui::Ui uiB2n;
    composeTo(uiB2n, mod, kScreen, "agent_ui_cf5n");
    DOCTEST_CHECK_MESSAGE(uiB2n.find(chipId) == nullptr,
                          "无最终回复时「复制回复」chip 应不渲染");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_dialog_close_and_reopen,
              "agent_ui: 关详情弹窗不改任务列表；重开回到最近一次 run", {
    test_support::TestDb tdb("agentui", "detailreopen");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("重开任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value();
    const std::string tid = std::to_string(taskId);

    auto r1 = store.startRun(taskId);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r1.value(), 0, "user",
                                        R"([{"type":"text","text":"旧 run 消息"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "limited", 3, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    auto r2 = store.startRun(taskId);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r2.value(), 0, "user",
                                        R"([{"type":"text","text":"新 run 消息"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(r2.value(), 1, "assistant",
                                        R"([{"type":"text","text":"新 run 回复"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const std::string base = "agent.dialog.detail";

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_ro1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_ro2");
    // 运行详情 tab：D6 默认折叠——无消息滚动区，只有标题行 + 占位提示
    DOCTEST_REQUIRE(clickId(ui2, base + ".nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, kScreen, "agent_ui_ro2n");
    DOCTEST_CHECK(ui2n.find(base + ".msgscroll") == nullptr);
    DOCTEST_CHECK(ui2n.find(base + ".msgs.hint") != nullptr);
    // 展开消息区：默认选中最近一次 run（新 run 回复自动展开）
    DOCTEST_REQUIRE(clickId(ui2n, base + ".msgs.toggle"));
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_ro2b");
    DOCTEST_REQUIRE(ui2b.find(base + ".msg.1.blk.0") != nullptr);
    // 显式点选旧 run（runlist.1 = limited；点击落 .bg）：先回执行记录 tab（三档互斥），
    // 点行自动回「运行详情」并收回消息区
    DOCTEST_REQUIRE(clickId(ui2b, base + ".nav.item.0.bg"));
    eui::Ui ui2c;
    composeTo(ui2c, mod, kScreen, "agent_ui_ro2c");
    DOCTEST_REQUIRE(clickId(ui2c, base + ".runlist.1.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_ro3");
    DOCTEST_CHECK(ui3.find(base + ".msgscroll") == nullptr);
    DOCTEST_REQUIRE(clickId(ui3, base + ".msgs.toggle"));
    eui::Ui ui3b;
    composeTo(ui3b, mod, kScreen, "agent_ui_ro3b");
    const auto* oldPreview = ui3b.find(base + ".msg.0.preview");
    DOCTEST_REQUIRE_MESSAGE(oldPreview != nullptr, "选中旧 run 后消息区未切换");
    DOCTEST_CHECK(oldPreview->text.find("旧 run 消息") != std::string::npos);

    DOCTEST_REQUIRE(clickId(ui3b, base + ".close"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_ro4");
    DOCTEST_CHECK(ui4.find(base + ".msgscroll") == nullptr);
    const auto* rowTitle = ui4.find("agent.task." + tid + ".title");
    DOCTEST_REQUIRE_MESSAGE(rowTitle != nullptr, "关弹窗后任务行消失（回写了列表？）");
    DOCTEST_CHECK_EQ(rowTitle->text, std::string("重开任务"));

    DOCTEST_REQUIRE(openDetailDialog(ui4, mod, tid));
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_ro5");
    // 重开回到 tab 0「执行记录」+ 折叠态（openTaskDetail 清零 detailTab_/messagesOpen_）
    DOCTEST_CHECK_MESSAGE(ui5.find(base + ".msgscroll") == nullptr,
                          "重开后消息区应回到默认折叠");
    DOCTEST_CHECK(ui5.find(base + ".runlist.title") != nullptr);
    DOCTEST_REQUIRE(clickId(ui5, base + ".nav.item.1.bg"));
    eui::Ui ui5n;
    composeTo(ui5n, mod, kScreen, "agent_ui_ro5n");
    DOCTEST_REQUIRE(clickId(ui5n, base + ".msgs.toggle"));
    eui::Ui ui5b;
    composeTo(ui5b, mod, kScreen, "agent_ui_ro5b");
    const auto* reopened = ui5b.find(base + ".msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(reopened != nullptr,
                            "重开后未回到最近一次 run（新 run 回复未默认展开）");
    DOCTEST_CHECK(reopened->text.find("新 run 回复") != std::string::npos);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_messages_collapsed_by_default,
              "agent_ui: 消息区默认折叠（无 .msgscroll/.msgs.body、hint 占位、copyfinal 可见）；展开后最终 assistant 默认展开", {
    test_support::TestDb tdb("agentui", "msgcollapse");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("折叠任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                        R"([{"type":"text","text":"问题"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 1, "assistant",
                                        R"([{"type":"text","text":"最终答案"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    const std::string base = "agent.dialog.detail";

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_mc1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_mc2");
    // 「运行详情」tab（消息区在 tab 1）：折叠态无 .msgscroll（= 无 .msgs.body 消息行）、
    // hint 占位在场、copyfinal 不随折叠消失
    DOCTEST_REQUIRE(clickId(ui2, base + ".nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, kScreen, "agent_ui_mc2n");
    DOCTEST_CHECK_MESSAGE(ui2n.find(base + ".msgscroll") == nullptr,
                          "默认不得渲染 .msgscroll");
    DOCTEST_CHECK(ui2n.find(base + ".msg.0") == nullptr);
    const auto* hint = ui2n.find(base + ".msgs.hint");
    DOCTEST_REQUIRE_MESSAGE(hint != nullptr, "折叠态缺 .msgs.hint 占位");
    DOCTEST_CHECK_EQ(hint->text, std::string("展开查看完整链路"));
    DOCTEST_CHECK(ui2n.find(base + ".msgs.copyfinal") != nullptr);
    // 展开：.msgscroll 上桌，最终 assistant 默认展开、用户消息仍块级折叠
    DOCTEST_REQUIRE(clickId(ui2n, base + ".msgs.toggle"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_mc3");
    DOCTEST_REQUIRE(ui3.find(base + ".msgscroll") != nullptr);
    const auto* finalBlk = ui3.find(base + ".msg.1.blk.0");
    DOCTEST_REQUIRE_MESSAGE(finalBlk != nullptr,
                            "展开后最终 assistant 文本块未默认展开");
    DOCTEST_CHECK_EQ(finalBlk->text, std::string("最终答案"));
    DOCTEST_CHECK(ui3.find(base + ".msg.0.blk.0") == nullptr);
    // 再收起：消息行退场、hint 回归
    DOCTEST_REQUIRE(clickId(ui3, base + ".msgs.toggle"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_mc4");
    DOCTEST_CHECK(ui4.find(base + ".msgscroll") == nullptr);
    DOCTEST_CHECK(ui4.find(base + ".msgs.hint") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_config_popover,
              "agent_ui: 任务配置 tab（方案 A）——长字段 wrap 全文 / 滚动区不越面板底 / 浮层机制退役零回潮 / tab 切换主区 y 不变", {
    test_support::TestDb tdb("agentui", "cfgpop");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    // 长目标 + 长系统提示词：wrap 字段走估高，配置 tab 内容可内滚（ASCII 长串，换行判据同效）
    const std::string longGoal(400, 'z');
    const std::string longNote(400, 'n');
    auto created = store.createTask("配置任务", longGoal, "schedule", "default",
                                    10, false, R"({"cron":"0 9 * * 1-5"})", longNote);
    DOCTEST_REQUIRE(created.isOk());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    const std::string tid = std::to_string(created.value());
    const std::string base = "agent.dialog.detail";

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_cp1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_cp2");
    const auto* runlistBefore = ui2.find(base + ".runlist");
    DOCTEST_REQUIRE(runlistBefore != nullptr);
    // 浮层机制退役（方案 A 收进 tab）：scrim / popover 在任何 tab 都不得回潮
    DOCTEST_CHECK(ui2.find(base + ".config.scrim") == nullptr);
    DOCTEST_CHECK(ui2.find(base + ".config.popover") == nullptr);

    // 切任务配置 tab：wrap 字段全文在场（长目标/长记忆不截断）
    DOCTEST_REQUIRE(clickId(ui2, base + ".nav.item.2.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_cp3");
    const auto* cfgScroll = ui3.find(base + ".config.scroll");
    DOCTEST_REQUIRE_MESSAGE(cfgScroll != nullptr, "任务配置 tab 缺 .config.scroll");
    const auto* goalV = ui3.find(base + ".config.body.goal.value");
    DOCTEST_REQUIRE_MESSAGE(goalV != nullptr, "配置 tab 缺 goal 字段");
    DOCTEST_CHECK(goalV->text.size() >= longGoal.size());
    const auto* noteV = ui3.find(base + ".config.body.context.value");
    DOCTEST_CHECK_MESSAGE(noteV == nullptr, "配置 tab context 字段应已移除（W4 C3）");
    const auto* destrV = ui3.find(base + ".config.body.destructive.value");
    DOCTEST_REQUIRE_MESSAGE(destrV != nullptr, "配置 tab 缺 destructive 字段（W4 C3 补位）");
    DOCTEST_CHECK_MESSAGE(destrV->text == std::string("关闭") ||
                          destrV->text == std::string("开启") ||
                          destrV->text == std::string("禁止") ||
                          destrV->text == std::string("—"),
                          std::string("destructive 单元格文本异常：") + destrV->text);
    // 配置 tab 互斥：runlist / 运行详情块退场；scrim / popover 仍不回潮
    DOCTEST_CHECK(ui3.find(base + ".runlist") == nullptr);
    DOCTEST_CHECK(ui3.find(base + ".detail") == nullptr);
    DOCTEST_CHECK(ui3.find(base + ".config.scrim") == nullptr);
    DOCTEST_CHECK(ui3.find(base + ".config.popover") == nullptr);
    // 滚动区收在面板内（底 ≤ 面板底 − 23.5，+0.5 容差同 nearPx）
    const auto* panel = ui3.find(base + ".panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(
        cfgScroll->frame.y + cfgScroll->frame.height <=
            panel->frame.y + panel->frame.height - 23.5f,
        "配置滚动区越出面板底：scroll 底 " +
            std::to_string(cfgScroll->frame.y + cfgScroll->frame.height) +
            " 面板底 " + std::to_string(panel->frame.y + panel->frame.height));

    // 切回执行记录 tab：runlist 原位（tab 切换不移动任何档的几何）
    DOCTEST_REQUIRE(clickId(ui3, base + ".nav.item.0.bg"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_cp4");
    const auto* runlistAfter = ui4.find(base + ".runlist");
    DOCTEST_REQUIRE(runlistAfter != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(runlistAfter->frame.y, runlistBefore->frame.y),
                          "切 tab 不得移动执行记录几何");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_copy_toast,
              "agent_ui: 复制反馈走页级 toast（copyfinal / 逐条 copy 点击后 agent.detail.toast.message 有「已复制」且 root opacity=1）", {
    test_support::TestDb tdb("agentui", "copytoast");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("toast任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                        R"([{"type":"text","text":"查"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 1, "assistant",
                                        R"([{"type":"text","text":"答"}])",
                                        std::nullopt).isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    const std::string base = "agent.dialog.detail";

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_ct1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_ct2");
    // 初始无 toast 文案（toast_.open=false；页级 toast 不随 tab 进退）
    const auto* toastRoot0 = ui2.find("agent.detail.toast");
    DOCTEST_REQUIRE(toastRoot0 != nullptr);   // 每帧无条件构建
    DOCTEST_CHECK(toastRoot0->opacity == 0.0f);
    // copyfinal 在「运行详情」tab（折叠态即可点，chip 常驻标题行）
    DOCTEST_REQUIRE(clickId(ui2, base + ".nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, kScreen, "agent_ui_ct2n");
    DOCTEST_REQUIRE(clickId(ui2n, base + ".msgs.copyfinal.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_ct3");
    const auto* msg = ui3.find("agent.detail.toast.message");
    DOCTEST_REQUIRE_MESSAGE(msg != nullptr, "复制后缺 agent.detail.toast.message");
    DOCTEST_CHECK(msg->text.find("已复制") != std::string::npos);
    const auto* toastRoot = ui3.find("agent.detail.toast");
    DOCTEST_REQUIRE(toastRoot != nullptr);
    DOCTEST_CHECK_MESSAGE(toastRoot->opacity == 1.0f,
                          "toast root opacity 应为 1，实测 " +
                              std::to_string(toastRoot->opacity));
    // 逐条 copy：展开消息区 + 展开 msg.0 → 点 .copy → toast 文案切换
    DOCTEST_REQUIRE(clickId(ui3, base + ".msgs.toggle"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_ct4");
    DOCTEST_REQUIRE(clickId(ui4, base + ".msg.0.toggle"));
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_ct5");
    DOCTEST_REQUIRE(clickId(ui5, base + ".msg.0.copy"));
    eui::Ui ui6;
    composeTo(ui6, mod, kScreen, "agent_ui_ct6");
    const auto* msg2 = ui6.find("agent.detail.toast.message");
    DOCTEST_REQUIRE(msg2 != nullptr);
    DOCTEST_CHECK(msg2->text.find("已复制") != std::string::npos);
    DOCTEST_CHECK(msg2->text.find("消息全文") != std::string::npos);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_runlist_error_rows_no_overlap,
              "agent_ui: 详情弹窗 3 条 error 的 run 列表不压消息标题（runsH 必须计入 error 行的 gap；回退即红）", {
    test_support::TestDb tdb("agentui", "errrows");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("错误行任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    // 3 次 run 全部带 error 文案（每次 = 状态行 18 + 错误行 16，列内 gap 4）
    for (int i = 0; i < 3; ++i) {
        auto r = store.startRun(created.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), "limited", i + 1,
                                        int64_t(100), int64_t(10),
                                        std::string("错误文案 ") +
                                            std::to_string(i)).isOk());
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_err1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_err2");

    const auto* panel = ui2.find("agent.dialog.detail.panel");
    const auto* runlistBox = ui2.find("agent.dialog.detail.runlist");
    const auto* lastErr = ui2.find("agent.dialog.detail.runlist.2.err");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr && runlistBox != nullptr,
                            "面板/时间线容器缺失");
    DOCTEST_REQUIRE_MESSAGE(lastErr != nullptr, "第 3 条 error 行缺失");
    // 时间线在独立滚动容器里后，行间互不重叠是同一契约（error 行是 scroll 子元素，
    // gap 漏算依旧会让行互相压）；行不再与右栏消息标题竞争，只对本容器负责
    DOCTEST_CHECK_MESSAGE(
        lastErr->frame.y + lastErr->frame.height <=
            runlistBox->frame.y + runlistBox->frame.height + 0.5f,
        "error 行越出时间线容器：error 底 " +
            std::to_string(lastErr->frame.y + lastErr->frame.height) +
            " > 容器底 " +
            std::to_string(runlistBox->frame.y + runlistBox->frame.height));
    const auto* err0 = ui2.find("agent.dialog.detail.runlist.0.err");
    const auto* err1 = ui2.find("agent.dialog.detail.runlist.1.err");
    DOCTEST_REQUIRE(err0 != nullptr && err1 != nullptr);
    DOCTEST_CHECK(err0->frame.y + err0->frame.height <= err1->frame.y + 0.5f);
    DOCTEST_CHECK(err1->frame.y + err1->frame.height <= lastErr->frame.y + 0.5f);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_edit_read_failure_shows_page_error,
              "agent_ui: 点「编辑」读库失败（陈旧缓存）→ 页内错误行可见、弹窗不开、下次成功动作清除", {
    test_support::TestDb tdb("agentui", "editerr");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("将消失的任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_eerr1");
    DOCTEST_REQUIRE(ui.find("agent.task." + tid + ".edit") != nullptr);
    // 制造陈旧缓存：UI 帧里仍有该行，库里已被删（外部删除 / 另一实例）
    DOCTEST_REQUIRE(tdb.handle().exec("DELETE FROM agent_tasks WHERE id=" + tid).isOk());

    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".edit"),
                            "编辑 chip 缺失");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_eerr2");
    const auto* pageErr = ui2.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(pageErr != nullptr,
                            "编辑读库失败没有任何可见反馈（静默路径）");
    DOCTEST_CHECK(pageErr->text.find("读取任务失败") != std::string::npos);
    // 弹窗不进编辑态（没数据可编）
    DOCTEST_CHECK(ui2.find("agent.dialog.task.goal") == nullptr);
    // 同类静默路径：启停切换失败也走同一条页内错误行
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.task." + tid + ".toggle"),
                            "启停 chip 缺失");
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_eerr2b");
    const auto* toggleErr = ui2b.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(toggleErr != nullptr, "启停失败没有任何可见反馈");
    DOCTEST_CHECK(toggleErr->text.find("启停切换失败") != std::string::npos);
    DOCTEST_REQUIRE(clickId(ui2b, "agent.page.newTask.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_eerr3");
    DOCTEST_CHECK(ui3.find("agent.page.error") == nullptr);
    // W4 步骤条：新建弹窗打开停在 step 0（名称输入可见，goal 在 step 2）
    DOCTEST_CHECK(ui3.find("agent.dialog.task.name") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_tools_section_toggle,
              "agent_ui: 可用工具区块收起默认→展开列出注册表全部工具（名称+描述+参数）", {
    test_support::TestDb tdb("agentui", "tools");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list", "app.now",
                         "net.fetch"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    core::ToolRegistry::instance().unregisterTool("hot.list");
    core::ToolRegistry::instance().unregisterTool("app.now");
    core::ToolRegistry::instance().unregisterTool("net.fetch");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();   // 注册自带五工具（测试环境干净起点）

    const size_t registered =
        core::ToolRegistry::instance().listTools().size();
    DOCTEST_REQUIRE(registered >= 1);

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_tools1");
    const auto* header = ui.find("agent.tools.header");
    DOCTEST_REQUIRE(header != nullptr);
    DOCTEST_REQUIRE(header->onClick != nullptr);
    const auto* label = ui.find("agent.tools.header.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_MESSAGE(
        label->text.find("可用工具") != std::string::npos &&
            label->text.find(std::to_string(registered)) != std::string::npos,
        "头部缺「可用工具（N）」计数，实测: " + label->text);


    DOCTEST_CHECK(ui.find("agent.tools.card.0") == nullptr);
    header->onClick();
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_tools2");
    size_t cards = 0;
    for (size_t k = 0; k < registered + 4; ++k) {
        if (ui2.find("agent.tools.card." + std::to_string(k)) != nullptr) {
            ++cards;
        }
    }
    DOCTEST_CHECK_MESSAGE(cards == registered,
                          "工具卡数 " + std::to_string(cards) +
                              " != 注册表数 " + std::to_string(registered));
    // listTools 是 map 序（name 字典序）：app.notify < app.now < app.stats <
    // bash.run < fs.edit < fs.glob < fs.grep < fs.read < fs.write <
    // git.branch < git.checkout < git.clone < git.commit < git.diff < git.log <
    // git.pull < git.status < hot.list < net.fetch（五默认工具 + 14 内置工具）
    const auto* name0 = ui2.find("agent.tools.card.0.name");
    DOCTEST_REQUIRE(name0 != nullptr);
    DOCTEST_CHECK_EQ(name0->text, std::string("app.notify"));
    // B3 工具卡白卡面：cardSurface（H15 alpha 0.92）底 + 1px borderDefault 描边
    const auto* toolBg = ui2.find("agent.tools.card.0.bg");
    DOCTEST_REQUIRE(toolBg != nullptr);
    const auto& themeTools = core::theme::current();
    const auto sameColorL = [](const eui::Color& a, const eui::Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };
    DOCTEST_CHECK_MESSAGE(sameColorL(toolBg->color, ui_kit::cardSurface(themeTools)),
                          "工具卡底色应为 cardSurface（alpha 0.92）");
    DOCTEST_CHECK_MESSAGE(nearPx(toolBg->border.width, 1.0f) &&
                              sameColorL(toolBg->border.color,
                                         themeTools.colors.borderDefault),
                          "工具卡应有 1px borderDefault 描边");
    const auto* name1 = ui2.find("agent.tools.card.1.name");
    DOCTEST_REQUIRE(name1 != nullptr);
    DOCTEST_CHECK_EQ(name1->text, std::string("app.now"));
    const auto* name2 = ui2.find("agent.tools.card.2.name");
    DOCTEST_REQUIRE(name2 != nullptr);
    DOCTEST_CHECK_EQ(name2->text, std::string("app.stats"));
    const auto* name3 = ui2.find("agent.tools.card.3.name");
    DOCTEST_REQUIRE(name3 != nullptr);
    DOCTEST_CHECK_EQ(name3->text, std::string("bash.run"));
    const auto* name4 = ui2.find("agent.tools.card.4.name");
    DOCTEST_REQUIRE(name4 != nullptr);
    DOCTEST_CHECK_EQ(name4->text, std::string("fs.edit"));
    // 参数文字已并入 desc（双行布局：行 1=名称，行 2=描述+参数）
    const auto* desc0 = ui2.find("agent.tools.card.0.desc");
    DOCTEST_REQUIRE(desc0 != nullptr);
    DOCTEST_CHECK(desc0->text.find("参数：") != std::string::npos);

    const auto* header2 = ui2.find("agent.tools.header");
    DOCTEST_REQUIRE(header2 != nullptr && header2->onClick != nullptr);
    header2->onClick();
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_tools3");
    DOCTEST_CHECK(ui3.find("agent.tools.card.0") == nullptr);

    mod.onStop();
    return true;
})

// H18-批3 工具区纵向弹性双档（S4.1 锚点 1000×640 / 大窗 1600×1000）。
// 防的事故：工具列表高回退固定封顶 240——窗口拉高后工具区不增高、可见行数不增。
// 断言钉列表容器 agent.tools.list.wrap 的布局高：最小窗真实页高 600 = 240 锚点
// （不退），大窗 > 240（弹性生效）。自然高须先大于弹性值，否则 min(…, 自然高)
// 会先封顶、断言失去意义——测试环境 onStart 注册 19 个工具（5 默认 + 14 内置）。
REGISTER_TEST(agent_tools_height_dual_tier,
              "agent_ui: 工具列表纵向弹性双档——最小窗页高 600 高=240 锚点 / 大窗 >240", {
    test_support::TestDb tdb("agentui", "toolsh");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify", "hot.list", "app.now",
                         "net.fetch"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    core::ToolRegistry::instance().unregisterTool("hot.list");
    core::ToolRegistry::instance().unregisterTool("app.now");
    core::ToolRegistry::instance().unregisterTool("net.fetch");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    const size_t registered = core::ToolRegistry::instance().listTools().size();
    DOCTEST_REQUIRE_MESSAGE(
        registered >= 8,
        "工具注册数不足 8（自然高 ≤ 320 会掩盖 1000 档弹性判定），实测 " +
            std::to_string(registered));

    // 展开工具区（toolsOpen_ 是模块状态，后续两档 compose 复用）
    eui::Ui ui0;
    composeTo(ui0, mod, kScreen, "agent_ui_toolsh0");
    DOCTEST_REQUIRE(clickId(ui0, "agent.tools.header"));

    // 1000 档：1600×1000（与 H3 双档同口径）
    eui::Ui uiBig;
    composeTo(uiBig, mod, kScreen, "agent_ui_toolsh_big");
    const auto* bigWrap = uiBig.find("agent.tools.list.wrap");
    DOCTEST_REQUIRE(bigWrap != nullptr);
    DOCTEST_CHECK_MESSAGE(
        bigWrap->frame.height > 240.5f,
        "1000 档工具列表应随窗增高 >240，实测 " +
            std::to_string(bigWrap->frame.height));

    // 最小窗真实页高：1000×640 窗口减 40 标题条 = 600——弹性公式不得抬高锚点
    const eui::Screen kMinScreen{1000.0f, 600.0f};
    eui::Ui uiMin;
    composeTo(uiMin, mod, kMinScreen, "agent_ui_toolsh_min");
    const auto* minWrap = uiMin.find("agent.tools.list.wrap");
    DOCTEST_REQUIRE(minWrap != nullptr);
    DOCTEST_CHECK_MESSAGE(
        nearPx(minWrap->frame.height, 240.0f),
        "最小窗页高 600 工具列表应保持 240 锚点，实测 " +
            std::to_string(minWrap->frame.height));

    mod.onStop();
    return true;
})

// 判据只能钉我们自己算的布局预算（面板设计尺寸、每行底 ≤ 按钮行顶、超长文本不挤位、
// 下拉展开态全在屏内）：「弹窗不超出视口」被 dialog 组件自身的 clamp 兜住，恒真。

// UAT5 任务弹窗重设计几何（弹性宽 = max(720, 窗口/2)，kScreen 1600 → 800×680）。
// 防的事故：① 弹性宽回退固定 760 → 面板宽断言红；② segment 选项不平分整行 →
// 选项几何断言红；③ 槽位下拉（仍是下拉）展开掉出屏底 → 屏内断言红；④ 超长目标与
// 超长任务名不挤位（输入盒宽恒定 + 行内动作不被标题挤出）。几何断言走 layout 后
// frame 绝对坐标再减面板原点（与详情弹窗几何用例同口径）。
REGISTER_TEST(agent_ui_dialog_geometry_design_size_and_budget,
              "agent_ui: 任务弹窗几何——弹性宽 800x496（步骤条）/ segment 平分整行 / 槽位下拉屏内 / 超长文本不挤位", {
    test_support::TestDb tdb("agentui", "dlggeom");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    // 超长目标（300 字符 → 多行输入内部滚动压力）；超长任务名绕过
    // deriveTaskName 直接种（模拟库里已有的长名）
    const std::string longGoal(300, 'g');
    const std::string longName(120, 'N');
    auto created = store.createTask("几何任务", longGoal, "schedule", "default", 10,
                                    false, R"({"cron":"0 9 * * 1-5"})");
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto longNamed = store.createTask(longName, "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(longNamed.isOk());
    const std::string nid = std::to_string(longNamed.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_geo1");
    DOCTEST_REQUIRE(clickId(ui, "agent.task." + tid + ".edit"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_geo2");

    // ① 面板设计尺寸：弹性宽 = max(720, 1600/2) = 800；高 496
    const auto* panel = ui2.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 800.0f),
                          "任务弹窗宽应为弹性值 800，实测 " +
                              std::to_string(panel->frame.width));
    // W4 C1：步骤条重做后高 496（内容区 280 + 步骤条 64 + 标题/按钮/间距）
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.height, 496.0f),
                          "任务弹窗高应为设计值 496，实测 " +
                              std::to_string(panel->frame.height));
    DOCTEST_CHECK(panel->frame.x >= -0.5f && panel->frame.y >= -0.5f &&
                  panel->frame.x + panel->frame.width <= kScreen.width + 0.5f &&
                  panel->frame.y + panel->frame.height <= kScreen.height + 0.5f);
    const float px = panel->frame.x;
    const float py = panel->frame.y;
    const float pw = 800.0f;
    // 裁定 2：内容区双侧内边距 24px（v1.0 §三.4），正文行宽 = 弹窗宽 − 2×24 = −48。
    // 旧锚 56（28×2）为收紧前口径；kDialogBodyInsetX = 48 是产品真值。
    const float innerW = pw - 48.0f;   // 752

    // ② 步骤胶囊几何：4 块面节点 y=64 h=32 r=16，首节点 step.0 在 (24, 64)
    constexpr float kPillH = 32.0f;
    constexpr float kStepperY = 64.0f;
    const auto* step0 = ui2.find("agent.dialog.task.step.0");
    DOCTEST_REQUIRE(step0 != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(step0->frame.x - px, 24.0f), "step.0 x 应=24（实测锚定），实测 " + std::to_string(step0->frame.x - px));
    DOCTEST_CHECK_MESSAGE(nearPx(step0->frame.y - py, kStepperY), "step.0 y 应=64");
    DOCTEST_CHECK_MESSAGE(nearPx(step0->frame.height, kPillH), "step.0 h 应=32");

    // ②b step 0：名称输入 y=144（kContentTop+28）h=40 全宽（x0=24）
    const auto* nameInput = ui2.find("agent.dialog.task.name");
    DOCTEST_REQUIRE(nameInput != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(nameInput->frame.x - px, 24.0f), "名称输入应 x=24");
    DOCTEST_CHECK_MESSAGE(nearPx(nameInput->frame.y - py, 144.0f), "名称输入应 y=144");
    DOCTEST_CHECK_MESSAGE(nearPx(nameInput->frame.height, 40.0f), "名称输入应高 40");
    DOCTEST_CHECK_MESSAGE(nearPx(nameInput->frame.width, innerW),
                          "名称输入应全宽 752，实测 " +
                              std::to_string(nameInput->frame.width));

    // ③ 导航 step 1：触发 segment 平分整行（opW=(752−5×6)/6≈120.33 x=24+i×(opW+6) y=136 h=32）
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_geo2s1");
    const float opW = (innerW - 6.0f * 5.0f) / 6.0f;  // 与产品码同公式
    for (int i = 0; i < module::agent::kTriggerCount; ++i) {
        const auto* op = ui2s1.find("agent.dialog.task.trigger.item." +
                                    std::to_string(i) + ".bg");
        DOCTEST_REQUIRE(op != nullptr);
        DOCTEST_CHECK_MESSAGE(
            nearPx(op->frame.x - px, 24.0f + (opW + 6.0f) * i),
            "segment 第 " + std::to_string(i) + " 项 x 错位");
        DOCTEST_CHECK_MESSAGE(nearPx(op->frame.width, opW),
                              "segment 第 " + std::to_string(i) + " 项宽应 " +
                                  std::to_string(opW));
        DOCTEST_CHECK(nearPx(op->frame.height, 32.0f));
        DOCTEST_CHECK(nearPx(op->frame.y - py, 136.0f));
    }
    const auto* segLast = ui2s1.find("agent.dialog.task.trigger.item.5.bg");
    DOCTEST_REQUIRE(segLast != nullptr);
    DOCTEST_CHECK(segLast->frame.x - px + segLast->frame.width <=
                  24.0f + innerW + 0.5f);

    // ④ 导航 step 2：goal textarea h≥60 全宽；行底 ≤ 按钮行顶
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2s1, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_geo2s2");
    const auto* goal = ui2s2.find("agent.dialog.task.goal");
    DOCTEST_REQUIRE(goal != nullptr);
    DOCTEST_CHECK_MESSAGE(goal->frame.height >= 60.0f,
                          "编辑弹窗 goal 输入应 ≥60px，实测 " +
                              std::to_string(goal->frame.height));
    DOCTEST_CHECK_MESSAGE(nearPx(goal->frame.width, innerW),
                          "goal 输入应全宽 752，实测 " +
                              std::to_string(goal->frame.width));
    const auto* saveBtn = ui2s2.find("agent.dialog.task.save");
    DOCTEST_REQUIRE_MESSAGE(saveBtn != nullptr, "保存按钮缺失");
    // W4 C3：自动记录已移除，替换字段是系统提示词
    const auto* sysprompt = ui2s2.find("agent.dialog.task.sysprompt");
    DOCTEST_REQUIRE_MESSAGE(sysprompt != nullptr, "系统提示词缺失");
    DOCTEST_CHECK_MESSAGE(
        sysprompt->frame.y + sysprompt->frame.height <= saveBtn->frame.y + 0.5f,
        "系统提示词底 " +
            std::to_string(sysprompt->frame.y + sysprompt->frame.height) +
            " 压到按钮行顶 " + std::to_string(saveBtn->frame.y));
    DOCTEST_CHECK(ui2s2.find("agent.dialog.task.autonote") == nullptr);
    DOCTEST_CHECK_MESSAGE(
        saveBtn->frame.y + saveBtn->frame.height <=
            panel->frame.y + panel->frame.height - 7.5f,
        "按钮行溢出面板底（btnY=h-56）");

    // ⑤ 导航 step 3：槽位下拉展开不越屏 + 破坏性行位置稳定
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_geo2s3");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2s3, "agent.dialog.task.slot.field"),
                            "模型槽位下拉字段命中面缺失");
    eui::Ui ui2c;
    composeTo(ui2c, mod, kScreen, "agent_ui_geo2c");
    const auto* slotPopup = ui2c.find("agent.dialog.task.slot.popup");
    DOCTEST_REQUIRE(slotPopup != nullptr);
    DOCTEST_CHECK_MESSAGE(slotPopup->opacity == 1.0f && slotPopup->zIndex > 0,
                          "槽位下拉未展开（opacity/z 未升档）");
    const auto* slotLast = ui2c.find("agent.dialog.task.slot.item.5");
    DOCTEST_REQUIRE_MESSAGE(slotLast != nullptr, "槽位下拉最后一项（子代理）缺失");
    DOCTEST_CHECK(slotLast->frame.width > 0.0f && slotLast->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        slotLast->frame.y >= -0.5f &&
            slotLast->frame.y + slotLast->frame.height <= kScreen.height + 0.5f,
        "槽位下拉最后一项掉出屏底: 底=" +
            std::to_string(slotLast->frame.y + slotLast->frame.height));
    // 弹层不挤动下方行
    const auto* metaBefore = ui2s3.find("agent.dialog.task.destructive");
    const float metaYBefore = metaBefore != nullptr ? metaBefore->frame.y : -1.0f;
    const auto* metaAfter = ui2c.find("agent.dialog.task.destructive");
    DOCTEST_REQUIRE(metaAfter != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(metaAfter->frame.y, metaYBefore),
                          "槽位下拉展开挤动了破坏性开关行");

    // ⑥ 超长任务名不挤掉行内动作（标题盒仍在动作 chip 左侧）
    DOCTEST_REQUIRE(clickId(ui2c, "agent.dialog.task.cancel.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_geo3");
    const auto* longTitle = ui3.find("agent.task." + nid + ".title");
    const auto* detailChip = ui3.find("agent.task." + nid + ".detail");
    const auto* longRow = ui3.find("agent.task." + nid);
    DOCTEST_REQUIRE_MESSAGE(longTitle != nullptr && detailChip != nullptr,
                            "超长任务名的行/标题/详情 chip 缺失");
    DOCTEST_CHECK(longTitle->frame.width > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        longTitle->frame.x + longTitle->frame.width <= detailChip->frame.x + 0.5f,
        "超长任务名挤出行内动作：标题右缘 " +
            std::to_string(longTitle->frame.x + longTitle->frame.width) +
            " > 详情 chip 左缘 " + std::to_string(detailChip->frame.x));
    DOCTEST_CHECK_MESSAGE(frameContained(*longRow, *detailChip),
                          "详情 chip 跑到任务行外");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_dialog_geometry_and_empty_state,
              "agent_ui: 详情弹窗弹性几何 800x660（max(720,窗口/2)）/ 内容底 ≤ 面板底 / 空态占位垂直居中", {
    test_support::TestDb tdb("agentui", "dgeom");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto withMsgs = store.createTask("有消息", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(withMsgs.isOk());
    auto noMsgs = store.createTask("无消息", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(noMsgs.isOk());
    for (int64_t id : {withMsgs.value(), noMsgs.value()}) {
        auto r = store.startRun(id);
        DOCTEST_REQUIRE(r.isOk());
        if (id == withMsgs.value()) {
            DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                                R"([{"type":"text","text":"你好"}])",
                                                std::nullopt).isOk());
        }
        DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                        std::nullopt, std::nullopt).isOk());
    }
    const std::string tid = std::to_string(withMsgs.value());
    const std::string nid = std::to_string(noMsgs.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 有消息：弹性几何 + 消息区底 ≤ 面板底
    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_dg1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dg2");
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(
        nearPx(panel->frame.width, 800.0f),
        "详情弹窗宽应为弹性值 max(720, 1600/2)=800，实测 " +
            std::to_string(panel->frame.width));
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.height, 552.0f),
                          "详情弹窗高应为设计值 552，实测 " +
                              std::to_string(panel->frame.height));
    // 头部真实任务名（方案 A：不再恒「任务详情」）
    const auto* title = ui2.find("agent.dialog.detail.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("有消息"));
    // 执行记录 tab 全宽单列（三档 tab 互斥；旧双栏退役）
    // 裁定 2：innerW = 弹窗宽 − 2×24（v1.0 §三.4 内容区双侧 24px 内边距）
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(runlist != nullptr);
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.width, panel->frame.width - 48.0f),
        "执行记录应为单列全宽（innerW = 弹性宽-48），实测 " +
            std::to_string(runlist->frame.width));
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.scroll") == nullptr);
    // 消息区默认折叠（D6）：点 run 行自动切「运行详情」，展开后再量 .msgscroll 几何
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.0.bg"));
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_dg2b");
    DOCTEST_REQUIRE(clickId(ui2b, "agent.dialog.detail.msgs.toggle"));
    eui::Ui ui2c;
    composeTo(ui2c, mod, kScreen, "agent_ui_dg2c");
    const auto* panel2c = ui2c.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel2c != nullptr);
    const auto* scroll = ui2c.find("agent.dialog.detail.msgscroll");
    DOCTEST_REQUIRE(scroll != nullptr);
    DOCTEST_CHECK(scroll->frame.height > 0.0f);
    // 消息区底距面板底 ≥24（底部按钮带上缘；+0.5 容差与 nearPx 同口径）
    DOCTEST_CHECK_MESSAGE(
        scroll->frame.y + scroll->frame.height <=
            panel2c->frame.y + panel2c->frame.height - 23.5f,
        "消息滚动区溢出面板底：底 " +
            std::to_string(scroll->frame.y + scroll->frame.height) + " 面板底 " +
            std::to_string(panel2c->frame.y + panel2c->frame.height));
    // 底部按钮带收在面板内（footer.close | footer.edit 右对齐）
    const auto* footerClose = ui2c.find("agent.dialog.detail.footer.close.bg");
    const auto* footerEdit = ui2c.find("agent.dialog.detail.footer.edit.bg");
    DOCTEST_REQUIRE(footerClose != nullptr && footerEdit != nullptr);
    DOCTEST_CHECK(frameContained(*panel2c, *footerClose));
    DOCTEST_CHECK(frameContained(*panel2c, *footerEdit));
    DOCTEST_CHECK_MESSAGE(
        footerClose->frame.x + footerClose->frame.width <=
            footerEdit->frame.x + 0.5f,
        "底部按钮应右对齐成对（关闭在编辑任务左侧）");

    // 无消息：切「运行详情」走 .empty 分支，占位文本垂直居中
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.close"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dg3");
    DOCTEST_REQUIRE(openDetailDialog(ui3, mod, nid));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_dg4");
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui4b;
    composeTo(ui4b, mod, kScreen, "agent_ui_dg4b");
    const auto* empty = ui4b.find("agent.dialog.detail.empty");
    DOCTEST_REQUIRE_MESSAGE(empty != nullptr, "无消息 run 未渲染空态占位");
    DOCTEST_CHECK_MESSAGE(empty->verticalAlign == eui::VerticalAlign::Center,
                          "空态占位应垂直居中（verticalAlign(Center)）");
    DOCTEST_CHECK(empty->frame.height > 0.0f);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_sidebar_registers_agent,
              "agent_ui: 真 app::compose 侧边栏出现「智能体」项（Router 注册 AgentModule）", {
    test_support::TestDb tdb("agentui", "sidebar");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    ::core::Router router;
    router.registerModule(&mod);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::Agent);

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;
    eui::Ui ui;
    ui.begin("agent_ui_sidebar");
    app::compose(ui, screen);
    ui.end();

    const auto* label = ui.find("app.sidebar.item.Agent.label");
    DOCTEST_REQUIRE(label != nullptr);
    DOCTEST_CHECK_EQ(label->text, std::string("智能体"));
    DOCTEST_CHECK(ui.find("app.sidebar.item.Agent.hit") != nullptr);
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::Agent);
    DOCTEST_CHECK(ui.find("agent.title") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_resident_enqueue_starts_run,
              "agent_ui: 常驻任务行的「入队」按钮经 Scheduler 真派发起 run（落 completed）", {
    test_support::TestDb tdb("agentui", "enqueue");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("常驻任务", "直接回复完成，不调用任何工具",
                                   "resident", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"done"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_enqueue");
    const auto* enqueue = ui.find("agent.task." + tid + ".enqueue");
    DOCTEST_REQUIRE_MESSAGE(enqueue != nullptr,
                            "常驻任务行缺 agent.task.<id>.enqueue 按钮");
    DOCTEST_REQUIRE(enqueue->onClick != nullptr);
    enqueue->onClick();

    bool completed = false;
    for (int i = 0; i < 500; ++i) {
        auto runs = store.listRuns(taskId.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "入队后 5s 内未落库 completed（未接到 scheduler？）");
    mod.onStop();
    return true;
})

REGISTER_TEST(agent_has_live_run_excludes_orphaned_finish_failure,
              "agent: 判停忽略孤儿 run（收尾写库失败 → 行永远 running）", {
    // AgentLoop 收尾 finishRun 写库失败时，DB 那行永远停在 'running'，而 run 线程早已结束。
    // 若「有 running 行」就当作「还有 run 在跑」，uiPolling_ 永不复位 → 此后每帧一次
    // refreshData()（N+1 次查询）直到重启。触发条件窄（磁盘满 / DB 写错），后果重。
    //
    // 本用例钉死 hasLiveRun 的排除语义：孤儿不算活 run（否则每帧轮询），真在跑的仍然算
    // （否则 UI 不刷新，真 run 跑完了都不更新状态）。
    // 夹具口径照真实结构：latestRuns_ 是「taskId → 最近一次 run」，AgentRunRow 自带 id
    // （runId）；孤儿集合按 runId 存（FinishFailureFn 回报的就是 runId）——把 taskId 与
    // runId 混用正是判停失效的成因，⑦ 专门钉这条。
    auto running = [](int64_t runId, int64_t taskId) {
        module::agent::AgentRunRow r;
        r.id = runId;
        r.taskId = taskId;
        r.status = "running";
        return r;
    };
    auto done = [](int64_t runId, int64_t taskId) {
        module::agent::AgentRunRow r;
        r.id = runId;
        r.taskId = taskId;
        r.status = "completed";
        return r;
    };

    DOCTEST_CHECK(module::agent::hasLiveRun({{1, running(10, 1)}}, {}));
    DOCTEST_CHECK_FALSE(module::agent::hasLiveRun({{1, running(10, 1)}}, {10}));
    // 孤儿 + 另一个真在跑的 → 仍然有活 run：不能因为有一个孤儿就整体停轮询
    DOCTEST_CHECK(module::agent::hasLiveRun(
        {{1, running(10, 1)}, {2, running(11, 2)}}, {10}));
    DOCTEST_CHECK_FALSE(module::agent::hasLiveRun({{1, done(10, 1)}}, {}));
    // 空表 → 没有活 run（不能崩，也不能因空表恒真）
    DOCTEST_CHECK_FALSE(module::agent::hasLiveRun({}, {}));
    DOCTEST_CHECK(module::agent::hasLiveRun({{1, running(10, 1)}}, {99}));
    // ⑦ 按 taskId（map 的键）而不是 runId 去匹配孤儿，必须匹配不上：runId 10 / taskId 1，
    // 传 {1} 不该把它排除掉、仍算活 run。传 {1} 若被排除 = 拿错了键，排除逻辑形同虚设
    DOCTEST_CHECK(module::agent::hasLiveRun({{1, running(10, 1)}}, {1}));
    return true;
})

// ===================== Task 1：run 完成桌面通知（迁移检测纯逻辑 + toast 接线） =====================

REGISTER_TEST(agent_run_end_notify_transition_rules,
              "agent: run 完成迁移检测纯逻辑（completed 静默 / failed·limited 必响 / 同 runId 不重复）", {
    using StatusMap = std::map<int64_t, std::string>;

    // ① 上轮 running 本轮 completed → 不通知（completed 静默）
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{10, "completed"}}, {})
                      .empty());
    // ② 上轮 running 本轮 failed → 通知一次（恰好一个且是它）
    {
        const auto r = module::agent::detectRunEndNotifications(
            {{10, "running"}}, {{10, "failed"}}, {});
        DOCTEST_REQUIRE(r.size() == 1);
        DOCTEST_CHECK(r[0] == 10);
    }
    // ③ 上轮 running 本轮 limited → 同样必响
    {
        const auto r = module::agent::detectRunEndNotifications(
            {{10, "running"}}, {{10, "limited"}}, {});
        DOCTEST_REQUIRE(r.size() == 1);
        DOCTEST_CHECK(r[0] == 10);
    }
    // ④ 同 runId 二次迁移（已进已通知集合）→ 不再通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{10, "failed"}}, {10})
                      .empty());
    // ⑤ 稳态不算迁移：failed→failed 不通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "failed"}}, {{10, "failed"}}, {})
                      .empty());
    // ⑥ prev 缺席（未观测过 running，如 run 结束后才进页）→ 不通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      StatusMap{}, {{10, "failed"}}, {})
                      .empty());
    // ⑦ cancelled 静默（不在必响名单：completed/cancelled 都不打扰）
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{10, "cancelled"}}, {})
                      .empty());
    // ⑧ 混合迁移：failed + completed 并存 → 只通知 failed 那个 runId
    {
        const auto r = module::agent::detectRunEndNotifications(
            {{10, "running"}, {11, "running"}},
            {{10, "completed"}, {11, "failed"}}, {});
        DOCTEST_REQUIRE(r.size() == 1);
        DOCTEST_CHECK(r[0] == 11);
    }
    // ⑨ run 掉出本轮快照（被新 run 顶出最近列表）→ 不通知，不误报
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      {{10, "running"}}, {{11, "running"}}, {})
                      .empty());
    // ⑩ 空快照不崩、不通知
    DOCTEST_CHECK(module::agent::detectRunEndNotifications(
                      StatusMap{}, StatusMap{}, {})
                      .empty());
    return true;
})

// Task 1 续（T5 审计小项）：已通知集合（notifiedRunIds_）只增不减 —— 每次 run 结束 +1、
// 会话内永不释放，是本模块唯一的无界容器。窗口外 id 不会再被观测到（快照只取最近 N 条
// run，更老的 run 不可能回到窗口），故每轮把不在窗口内的 id 剔掉即可，通知语义不变。
// 红证据（实现前）：本例引用尚不存在的 pruneNotifiedRunIds → 编译期红（缺 API）。
REGISTER_TEST(agent_run_end_notify_prune_keeps_window_only,
              "agent: 已通知集合随窗口收敛（窗口内保留 / 窗口外剔除 / 空窗口清空）", {
    // 窗口 {7,9}、集合 {1,7,9,42} → 只留 7、9
    {
        std::set<int64_t> notified = {1, 7, 9, 42};
        module::agent::pruneNotifiedRunIds({{7, "completed"}, {9, "failed"}},
                                           notified);
        DOCTEST_CHECK_EQ(notified.size(), 2u);
        DOCTEST_CHECK(notified.count(7) == 1u);
        DOCTEST_CHECK(notified.count(9) == 1u);
        DOCTEST_CHECK(notified.count(1) == 0u);
        DOCTEST_CHECK(notified.count(42) == 0u);
    }
    // 空窗口 → 全清（不是「无窗口信息就保留全部」）
    {
        std::set<int64_t> notified = {1, 2, 3};
        module::agent::pruneNotifiedRunIds({}, notified);
        DOCTEST_CHECK(notified.empty());
    }
    // 空集合 → 不崩
    {
        std::set<int64_t> notified;
        module::agent::pruneNotifiedRunIds({{5, "failed"}}, notified);
        DOCTEST_CHECK(notified.empty());
    }
    return true;
})

REGISTER_TEST(agent_ui_run_failed_shows_toast,
              "agent_ui: run 失败 → 通知入队（failed 必响，标题含失败、正文含任务名）", {
    test_support::TestDb tdb("agentui", "failtoast");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("会失败的任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        // 拖慢失败返回：保证轮询帧先观测到 running（否则首帧即终态 → 无迁移、无通知）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return core::Result<platform::http::Response>::fail(
            core::ErrorCode::Internal, "connection refused");
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_failtoast1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 有界轮询（10ms 步进 ≤5s）+ 每轮 recompose：迁移发生的帧通知必须入队
    //（fix round 起页面内不再画 agent.toast，通知统一由装配层取走发 app 级 toast）
    bool toasted = false;
    eui::Ui ui2;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_failtoast_poll");
        const auto notice = mod.takeRunEndNotice();
        if (notice.has_value()) {
            DOCTEST_CHECK_MESSAGE(notice->first.find("失败") != std::string::npos,
                                   "通知标题不含「失败」，实测: " + notice->first);
            DOCTEST_CHECK_MESSAGE(notice->second.find("会失败的任务") != std::string::npos,
                                   "通知正文不含任务名，实测: " + notice->second);
            const auto again = mod.takeRunEndNotice();
            DOCTEST_CHECK_MESSAGE(!again.has_value(),
                                  "同 runId 不应通知两次（取走即清）");
            toasted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(toasted, "run 失败后 5s 内未产生 run 结束通知");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_run_completed_silent_no_toast,
              "agent_ui: run 成功 → 不产生 run 结束通知（completed 静默）", {
    test_support::TestDb tdb("agentui", "silentok");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("静默任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                    R"({"type":"text","text":"done"}],)"
                    R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    // 成功静默的 OS 通道面：completed 迁移对系统通知也必须零打扰（spy 恒零调用）
    platform::notify::resetForTest();
    OsNotifySpy spy;
    platform::notify::setBackendForTest(&spy);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_silent1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 轮询到落库 completed，再补 3 帧确认迁移帧过后 toast 仍空（completed 静默）
    bool completed = false;
    eui::Ui ui2;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_silent_poll");
        auto runs = store.listRuns(taskId.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "completed") {
            completed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(completed, "run 5s 内未落库 completed");
    for (int i = 0; i < 3; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_silent_tail");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto notice = mod.takeRunEndNotice();
    DOCTEST_CHECK_MESSAGE(!notice.has_value(),
                          "completed 迁移不该产生通知，实测: " +
                              (notice.has_value() ? notice->first
                                                  : std::string("<无>")));
    DOCTEST_CHECK_EQ(spy.sendCalls.load(), 0);
    platform::notify::setBackendForTest(nullptr);
    platform::notify::resetForTest();

    mod.onStop();
    return true;
})

// 系统通知通道（失败/受限迁移面）：入队应用内 toast 的同时必须向 OS 发通知——
// 稳定 id 前缀 agent-run-、body 含任务名、level=Error；同一 runId 恰发一次。
// 成功侧零调用由 agent_ui_run_completed_silent_no_toast 的 spy 断言钉住。
REGISTER_TEST(agent_ui_run_failed_sends_system_notification,
              "agent_ui: run 失败 → OS 系统通知恰一次（id 前缀 agent-run-、正文含任务名、level=Error）", {
    test_support::TestDb tdb("agentui", "sysnotify");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("会失败的任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        // 拖慢失败返回：保证轮询帧先观测到 running（否则首帧即终态 → 无迁移）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return core::Result<platform::http::Response>::fail(
            core::ErrorCode::Internal, "connection refused");
    };
    module::agent::AgentModule mod(deps);
    platform::notify::resetForTest();
    OsNotifySpy spy;
    platform::notify::setBackendForTest(&spy);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_sysnotify1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 有界轮询至迁移帧（通知入队 = 系统通知同帧已发），再补帧确认不重发
    bool noticed = false;
    eui::Ui ui2;
    for (int i = 0; i < 500; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_sysnotify_poll");
        if (mod.takeRunEndNotice().has_value()) {
            noticed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(noticed, "run 失败后 5s 内未产生 run 结束通知");
    // M2：OS 发送已挪后台线程，落地异步——有界等待后取 acquire 配对再读 spy 字段
    bool osSent = false;
    for (int i = 0; i < 500 && !osSent; ++i) {
        osSent = spy.sendCalls.load(std::memory_order_acquire) >= 1;
        if (!osSent) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(osSent, "OS 系统通知后台发送 5s 内未落地");
    DOCTEST_CHECK_MESSAGE(spy.senderId != std::this_thread::get_id(),
                          "OS 系统通知在 UI 主线程发送（M2 后台化未生效）");
    DOCTEST_CHECK_EQ(spy.sendCalls.load(), 1);
    DOCTEST_CHECK_MESSAGE(spy.lastId.rfind("agent-run-", 0) == 0,
                          "系统通知 id 无 agent-run- 前缀，实测: " + spy.lastId);
    DOCTEST_CHECK_MESSAGE(spy.lastBody.find("会失败的任务") != std::string::npos,
                          "系统通知正文不含任务名，实测: " + spy.lastBody);
    DOCTEST_CHECK_EQ(spy.lastLevel,
                     static_cast<int>(platform::notify::Level::Error));
    for (int i = 0; i < 3; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_sysnotify_tail");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_CHECK_EQ(spy.sendCalls.load(), 1);
    platform::notify::setBackendForTest(nullptr);
    platform::notify::resetForTest();

    mod.onStop();
    return true;
})

// E1 二道闸（L5）：删除确认是「渲染时快照 + 确认时执行」，两帧之间 run 可能已起
//（scheduler/事件派发不挑时机，起跑以 DB running 行为证——scheduler 起的 run 不进
// 模块 runs_，但同样落 running 行）。确认删除必须再查一次：running 行在册 → 拒删，
// 任务与 run 记录原样保留。红证据：闸缺席时确认直通 store 级联删，任务消失必红。
REGISTER_TEST(agent_ui_delete_confirm_second_gate_blocks_running,
              "agent_ui: 确认弹窗开着期间起了 run（DB running 行）→ 确认删除被二道闸拒绝", {
    test_support::TestDb tdb("agentui", "delgate");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("门闸任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                    R"({"type":"text","text":"done"}],)"
                    R"({"type":"text","text":"done"}])"
                    R"(,"usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 帧 1：任务列表渲染（无 running 行，删除 chip 可点）→ 点删除，确认弹窗开
    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_delgate1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".delete.bg"),
                            "删除 chip 未渲染/不可点");
    // 弹窗开着 → 起跑（scheduler 同款落库路径：store 直插 running run 行）
    auto runId = store.startRun(taskId.value());
    DOCTEST_REQUIRE_MESSAGE(runId.isOk(), "running run 行插入失败");
    // 下一帧装配层拉取（app::compose 每帧调 pollRunEnd）：out-of-band 起跑的观测
    // 路径 = 兜底轻查询命中 → refreshData 把 running 行带进 latestRuns_。不跑它，
    // 帧 2 的页内 compose 因 tasksDirty_/uiPolling_ 全假不刷新，闸读到的还是空缓存
    mod.pollRunEnd();
    // 帧 2：确认弹窗渲染（latestRuns_ 已含 running 行）
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_delgate2");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.dialog.delete.confirm.bg"),
                            "确认删除按钮未渲染");
    // 二道闸生效：任务与 run 记录原样保留
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    bool found = false;
    for (const auto& t : tasks.value()) {
        if (t.id == taskId.value()) found = true;
    }
    DOCTEST_CHECK_MESSAGE(found,
                          "running 行在册时确认删除仍删掉了任务（二道闸未生效）");
    auto runs = store.listRuns(taskId.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK_EQ(runs.value().size(), static_cast<size_t>(1));
    // 确认弹窗已关（performDeleteTask 无论准驳都先收确认态）
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_delgate3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.delete") == nullptr,
                          "删除确认弹窗未关闭");

    mod.onStop();
    return true;
})

// ===================== Task 1 fix round：通知走 app 级通道（页面无关） =====================

// 「非智能体页也能发通知」：run 失败迁移发生在 Home 页（非 Agent 页）时，装配层
// 每帧拉取路径（app::compose 内 poll + take + showAppNotice）必须把通知送达
// app.notice toast。守的是接线方向：模块只暴露状态，装配层拉取后发应用级通知。
REGISTER_TEST(agent_run_end_notice_app_channel_off_page,
              "agent_ui: run 失败迁移在非智能体页时经装配层拉取路径弹 app.notice（跨页可见）", {
    test_support::TestDb tdb("agentui", "appchan");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("跨页通知任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());
    const std::string tid = std::to_string(taskId.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        // 拖慢失败返回：保证轮询帧先观测到 running（否则首帧即终态 → 无迁移、无通知）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return core::Result<platform::http::Response>::fail(
            core::ErrorCode::Internal, "connection refused");
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    // 阶段一：在 Agent 页启动 run（唯一合法启动入口是页面按钮）
    eui::Ui uiStart;
    composeTo(uiStart, mod, kScreen, "agent_appchan_start");
    DOCTEST_REQUIRE_MESSAGE(clickId(uiStart, "agent.task." + tid + ".run.bg"),
                            "启动按钮未渲染/不可点");

    // 阶段二：切到 Home 页（非 Agent 页）——迁移检测与通知都必须与 Agent 页可见性解耦
    ::core::Router router;
    router.registerModule(&mod);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::Home);
    DOCTEST_CHECK(router.currentPage() != ::core::PageId::Agent);

    // 阶段三：循环 app::compose（等价装配层每帧拉取）。迁移发生的那帧，
    // app.notice（应用级 toast，跨页可见）必须带上「失败」标题与任务名
    bool toasted = false;
    eui::Ui uiApp;
    for (int i = 0; i < 500; ++i) {
        uiApp.begin("agent_appchan_frame");
        app::compose(uiApp, screen);
        uiApp.end();
        const auto* title = uiApp.find("app.notice.title");
        if (title != nullptr && !title->text.empty()) {
            DOCTEST_CHECK_MESSAGE(title->text.find("失败") != std::string::npos,
                                   "app 通知标题不含「失败」，实测: " + title->text);
            const auto* msg = uiApp.find("app.notice.message");
            DOCTEST_REQUIRE_MESSAGE(msg != nullptr, "app 通知缺正文节点");
            DOCTEST_CHECK_MESSAGE(msg->text.find("跨页通知任务") != std::string::npos,
                                  "app 通知正文不含任务名，实测: " + msg->text);
            toasted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(toasted, "非 Agent 页期间 run 失败，5s 内未弹出 app.notice");

    mod.onStop();
    return true;
})

// 取走语义：takeRunEndNotice 一次取走后清空，同 runId 不重复（守 notifiedRunIds_
// 经 app 通道的同一约束——装配层拉两帧不得收到两条）
REGISTER_TEST(agent_run_end_notice_take_semantics,
              "agent_ui: takeRunEndNotice 取走即清（同 runId 两帧只取到一次）", {
    test_support::TestDb tdb("agentui", "takenotice");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("取走语义任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 无迁移时取走 = 空
    {
        const auto n = mod.takeRunEndNotice();
        DOCTEST_CHECK(!n.has_value());
    }

    // 直写一条终态 run（running → failed），再经 poll 触发迁移：take 必须恰好取到一次
    {
        auto runId = store.startRun(taskId.value());
        DOCTEST_REQUIRE(runId.isOk());
        // 先 poll 一次让 prevRunStatuses_ 捕获 running（迁移判据 = prev running → now 终态）
        mod.pollRunEnd();
        DOCTEST_REQUIRE(store.finishRun(runId.value(), "failed", 1,
                                        std::optional<int64_t>{},
                                        std::optional<int64_t>{},
                                        "api broken").isOk());

        mod.pollRunEnd();   // 装配层等价路径：轮询检测（不依赖页面 compose）
        const auto first = mod.takeRunEndNotice();
        DOCTEST_REQUIRE_MESSAGE(first.has_value(), "迁移发生后未取到通知");
        DOCTEST_CHECK_MESSAGE(first->first.find("失败") != std::string::npos,
                               "通知标题不含「失败」，实测: " + first->first);
        DOCTEST_CHECK_MESSAGE(first->second.find("取走语义任务") != std::string::npos,
                               "通知正文不含任务名，实测: " + first->second);
        const auto second = mod.takeRunEndNotice();
        DOCTEST_CHECK_MESSAGE(!second.has_value(),
                              "同 runId 第二帧不应再取到通知（取走即清）");
    }

    mod.onStop();
    return true;
})

// 冷启动盲区兜底：三守卫条件（uiPolling_/tasksDirty_/prevAlive）全假时，外部直写库插入的
// running run 进不了 prevRunStatuses_（prev 恒空 = 冷启动盲区），其 running→failed 迁移
// 零通知。兜底 = pollRunEnd 在三条件全假时做低频轻查询（hasRunningRun），发现 running 即
// refreshData 一次进入正常观测路径。
REGISTER_TEST(agent_run_end_fallback_cold_start,
              "agent_ui: 三守卫全假 + 库内新出现 running run → 兜底轻查询触发 refreshData，终态迁移产生通知", {
    test_support::TestDb tdb("agentui", "fallback");
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("兜底盲区任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 排干 onStart 的 tasksDirty_：此后 uiPolling_/tasksDirty_/prevRunStatuses_ 全空，
    // 三守卫条件全假 —— 外部直写库 run 的冷启动盲区成立
    mod.pollRunEnd();
    DOCTEST_CHECK(!mod.takeRunEndNotice().has_value());

    // 外部直写库插入 running run（不经页面、不经模块，模块侧三条件仍全假）
    auto runId = store.startRun(taskId.value());
    DOCTEST_REQUIRE(runId.isOk());

    // 兜底帧：轻查询发现 running → refreshData 一次，prevRunStatuses_ 捕获 running
    mod.pollRunEnd();

    // 外部把 run 直写成终态；下一帧走正常观测路径（prevAlive）必须给出迁移通知
    DOCTEST_REQUIRE(store.finishRun(runId.value(), "failed", 1,
                                    std::optional<int64_t>{},
                                    std::optional<int64_t>{},
                                    "fallback probe").isOk());
    mod.pollRunEnd();
    const auto notice = mod.takeRunEndNotice();
    DOCTEST_REQUIRE_MESSAGE(notice.has_value(),
                            "冷启动盲区未兜底：外部直写库 run 结束后无通知");
    DOCTEST_CHECK_MESSAGE(notice->first.find("失败") != std::string::npos,
                          "通知标题不含「失败」，实测: " + notice->first);
    DOCTEST_CHECK_MESSAGE(notice->second.find("兜底盲区任务") != std::string::npos,
                          "通知正文不含任务名，实测: " + notice->second);

    mod.onStop();
    return true;
})

// 兜底轻查询节流：兜底路径每帧都会走到（三守卫全假期间），轻查询若不节流就退化成
// 每帧一条 SQL（锁竞争回潮）。时钟走 schedulerNow 缝冻结，断言同 15s 窗口内多帧只
// 执行一次轻查询、跨窗口才允许第二次（runFallbackChecksForTest 计数）。
REGISTER_TEST(agent_run_end_fallback_throttle,
              "agent_ui: 兜底轻查询 15s 节流——同窗口多帧只查一次，跨窗口再查", {
    test_support::TestDb tdb("agentui", "fbthrottle");
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto taskId = store.createTask("节流任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(taskId.isOk());

    int64_t clockSec = 5000;
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.schedulerNow = [&clockSec] { return clockSec; };
    module::agent::AgentModule mod(deps);
    mod.onStart();
    mod.pollRunEnd();   // 排干 onStart 的 tasksDirty_（走常规路径，不计兜底查询）

    // 库内无 running run：兜底路径每帧都走到，但节流窗口内只允许一次轻查询
    mod.pollRunEnd();
    DOCTEST_CHECK_EQ(mod.runFallbackChecksForTest(), 1);
    mod.pollRunEnd();
    mod.pollRunEnd();
    DOCTEST_CHECK_EQ(mod.runFallbackChecksForTest(), 1);   // 同窗口不再查
    clockSec = 5000 + 16;                                   // 跨过 15s 窗口
    mod.pollRunEnd();
    DOCTEST_CHECK_EQ(mod.runFallbackChecksForTest(), 2);
    // 全程无 running run、无迁移 → 不得产生任何通知
    DOCTEST_CHECK(!mod.takeRunEndNotice().has_value());

    mod.onStop();
    return true;
})

// Task 2：定时分支模板下拉 + 下次触发预览。布局契约：模板 dropdown 与预览行
// 不得改变弹窗总高（UAT5 重设计后面板 680、安全组破坏性行 y=548），预览行只能落在
// cron 行与 meta 行之间的原 gap 内。
REGISTER_TEST(agent_ui_cron_template_dropdown_and_preview,
              "agent_ui: 定时分支模板下拉（≥4 条）选中回填 cron / 预览行随 editCron 即时更新（合法显下次触发、非法显校验、无匹配显一年内）/ 弹窗高不变", {
    test_support::TestDb tdb("agentui", "crontpl");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 配色通道断言用（eui::Color 无 operator==）：逐分量比。
    const auto sameColor = [](const eui::Color& a, const eui::Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };
    // 非恒真自证：错误色与次要文本色必须不同 —— 两值相等时下面所有配色断言都是空转。
    DOCTEST_CHECK_MESSAGE(
        !sameColor(::core::theme::current().colors.error,
                   ::core::theme::current().colors.textTertiary),
        "错误色与次要文本色不得相同，否则配色断言恒真");

    eui::Ui ui1;
    DOCTEST_REQUIRE(openNewTaskDialog(ui1, mod, "agent_ui_crontpl1"));
    // W4：先导航 step 1 才可见 trigger segment
    eui::Ui ui1s1;
    composeTo(ui1s1, mod, kScreen, "agent_ui_crontpl1s1");
    DOCTEST_REQUIRE(clickId(ui1s1, "agent.dialog.task.step.1.bg"));
    eui::Ui ui1s1v2;
    composeTo(ui1s1v2, mod, kScreen, "agent_ui_crontpl1s1v2");
    DOCTEST_REQUIRE(callDropdownChange(ui1s1v2, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_crontpl3");

    // ① 模板下拉存在；item.0 是「选择模板…」占位，item.1..4 对应 kCronTemplates
    DOCTEST_REQUIRE_MESSAGE(ui3.find("agent.dialog.task.cron.template") != nullptr,
                            "模板下拉 agent.dialog.task.cron.template 缺失");
    const auto* tplLabel3 = ui3.find("agent.dialog.task.cron.template.label");
    DOCTEST_REQUIRE_MESSAGE(tplLabel3 != nullptr, "模板下拉 label 缺失");
    DOCTEST_CHECK_EQ(tplLabel3->text, std::string("选择模板…"));
    for (int i = 0; i < module::agent::kCronTemplateCount; ++i) {
        const auto* item = ui3.find("agent.dialog.task.cron.template.item." +
                                    std::to_string(i + 1));
        DOCTEST_REQUIRE_MESSAGE(item != nullptr,
                                "模板项 item." + std::to_string(i + 1) + " 缺失");
        const auto* itemLabel = ui3.find(
            "agent.dialog.task.cron.template.item.label." + std::to_string(i + 1));
        DOCTEST_REQUIRE_MESSAGE(itemLabel != nullptr,
                                "模板项 label." + std::to_string(i + 1) + " 缺失");
        DOCTEST_CHECK_EQ(itemLabel->text,
                         std::string(module::agent::kCronTemplates[i].label));
    }

    // ② 布局契约：面板高 496；破坏性行在 step 3 才渲染（切过去取 y）
    const auto* panel3 = ui3.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE(panel3 != nullptr);
    // W4 C1：步骤条重做后高 496
    DOCTEST_CHECK_MESSAGE(nearPx(panel3->frame.height, 496.0f),
                          "任务弹窗高应恒为 496，实测 " +
                              std::to_string(panel3->frame.height));
    eui::Ui ui3s3;
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.step.3.bg"));
    composeTo(ui3s3, mod, kScreen, "agent_ui_crontpl3s3");
    const auto* metaRow3 = ui3s3.find("agent.dialog.task.destructive");
    DOCTEST_REQUIRE(metaRow3 != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(metaRow3->frame.y, panel3->frame.y + 214.0f),
                          "破坏性行 y 应恒为面板顶 +214（W4 步骤条后），实测 " +
                              std::to_string(metaRow3->frame.y));
    // cron 为空 → 预览行不渲染（skipIfEmpty）
    DOCTEST_CHECK(ui3.find("agent.dialog.task.cron.preview") == nullptr);

    // ③ 选中模板 2（每天 9 点）→ cron 回填 + 预览立即出现
    // 切回 step 1（② 切到 step 3 取 destructive y 了）
    eui::Ui ui3s1;
    DOCTEST_REQUIRE(clickId(ui3s3, "agent.dialog.task.step.1.bg"));
    composeTo(ui3s1, mod, kScreen, "agent_ui_crontpl3s1");
    DOCTEST_REQUIRE(
        callDropdownChange(ui3s1, "agent.dialog.task.cron.template", 2));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_crontpl4");
    DOCTEST_CHECK_EQ(inputText(ui4, "agent.dialog.task.cron"),
                     std::string("0 9 * * *"));
    const auto* preview4 = ui4.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(preview4 != nullptr, "合法 cron 未渲染预览行");
    DOCTEST_CHECK_MESSAGE(preview4->text.rfind("下次触发：", 0) == 0,
                          "预览行应以「下次触发：」开头，实测: " + preview4->text);
    // 配色通道（T4 审计缺口）：合法 cron 的预览行是**次要文本色**、不是错误色 ——
    // 只断文案时「合法表达式被误染错误色」会静默漏过。
    DOCTEST_CHECK_MESSAGE(
        sameColor(preview4->textColor,
                  ::core::theme::current().colors.textTertiary),
        "合法 cron 预览行应为次要文本色");
    // 预览行落在 cron 行与底部按钮行之间（W4：meta 行在 step 3，step 1 以按钮行顶为界）
    const auto* cronRow4 = ui4.find("agent.dialog.task.cron");
    DOCTEST_REQUIRE(cronRow4 != nullptr);
    const auto* saveRow4 = ui4.find("agent.dialog.task.save");
    DOCTEST_REQUIRE(saveRow4 != nullptr);
    DOCTEST_CHECK_MESSAGE(
        preview4->frame.y >= cronRow4->frame.y + cronRow4->frame.height - 0.5f,
        "预览行应在 cron 行之下");
    DOCTEST_CHECK_MESSAGE(
        preview4->frame.y + preview4->frame.height <= saveRow4->frame.y + 0.5f,
        "预览行压到按钮行：底 " +
            std::to_string(preview4->frame.y + preview4->frame.height) +
            " 按钮行顶 " + std::to_string(saveRow4->frame.y));

    // ④ 手改非法表达式 → 预览位显示校验文案（不等到保存才报）
    mod.setEditCronDraft("0 9 * * 1-5x");
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_crontpl5");
    const auto* preview5 = ui5.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(preview5 != nullptr, "非法 cron 未渲染预览行");
    DOCTEST_CHECK_MESSAGE(preview5->text.find("cron 表达式非法") !=
                              std::string::npos,
                          "非法 cron 预览应显校验文案，实测: " + preview5->text);
    DOCTEST_CHECK_MESSAGE(
        sameColor(preview5->textColor, ::core::theme::current().colors.error),
        "非法 cron 预览行应为错误色");

    // ⑤ 无匹配（2 月 31 日）→ 预览显「一年内无触发时间」
    mod.setEditCronDraft("0 9 31 2 *");
    eui::Ui ui6;
    composeTo(ui6, mod, kScreen, "agent_ui_crontpl6");
    const auto* preview6 = ui6.find("agent.dialog.task.cron.preview");
    DOCTEST_REQUIRE_MESSAGE(preview6 != nullptr, "无匹配 cron 未渲染预览行");
    DOCTEST_CHECK_MESSAGE(preview6->text == "一年内无触发时间",
                          "无匹配预览文案错误，实测: " + preview6->text);
    // 「无匹配」与「非法」走同一错误色通道（两条 error 分支各断一次，防只修一半）。
    DOCTEST_CHECK_MESSAGE(
        sameColor(preview6->textColor, ::core::theme::current().colors.error),
        "无匹配 cron 预览行应为错误色");

    mod.onStop();
    return true;
})

// ---------------------------------------------------------------------------
// Task 4：错误行「去 Claude 页」直达引导
// ---------------------------------------------------------------------------

// 跳转目标占位模块：navigateTo 对未注册页回退 Home，判据 currentPage()==ClaudeProxy
// 需要 ClaudeProxy 已注册
class StubPageModule : public core::Module {
public:
    explicit StubPageModule(core::PageId page) : page_(page) {}
    core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return "stub"; }
    std::string iconGlyph() const override { return "x"; }
    void compose(eui::Ui&, const eui::Screen&) override {}

private:
    core::PageId page_;
};

// 配置不可用类错误原文常量：与 direct_llm_client.cpp 的 kConfigErrorPrefix 逐字一致
// （文案一变本组用例即红——前缀匹配刻意脆弱，防文案漂移后引导静默失效）
const char* const kNoProfileErr =
    "LLM 配置不可用：没有激活的 Claude profile，请在 DevDesk 的 Claude 页选中 "
    "Profile 并点击「启用」";
const char* const kSlotResolveErr =
    "LLM 配置不可用：无法解析模型槽位「default」（该槽位未配置模型，请在 Claude 页补全）";

REGISTER_TEST(agent_needs_claude_page_hint_predicate,
              "agent: needsClaudePageHint 前缀匹配「LLM 配置不可用」类错误原文，其余一律 false", {
    using module::agent::needsClaudePageHint;
    // 正例：direct_llm_client.cpp 的四类配置错误（无激活 profile / 未配 Base URL /
    // 未配 API Key / 槽位未配置）共用同一前缀
    DOCTEST_CHECK(needsClaudePageHint(kNoProfileErr));
    DOCTEST_CHECK(needsClaudePageHint(kSlotResolveErr));
    DOCTEST_CHECK(needsClaudePageHint(
        "LLM 配置不可用：激活 profile 未配置 Base URL，请在 Claude 页补全"));
    DOCTEST_CHECK(needsClaudePageHint(
        "LLM 配置不可用：激活 profile 未配置 API Key，请在 Claude 页补全"));
    // 同前缀 + 追加后缀仍匹配（错误行拼接 / 重试提示等）
    DOCTEST_CHECK(needsClaudePageHint(
        std::string(kNoProfileErr) + "（已重试 3 次）"));
    DOCTEST_CHECK(needsClaudePageHint(
        std::string(kSlotResolveErr) + "，请检查配置"));
    // 负例：不含该前缀的普通错误一律 false（代理运行态错误已不在此列——
    // 智能体直连，代理开不开与它无关）
    DOCTEST_CHECK(!needsClaudePageHint(""));
    DOCTEST_CHECK(!needsClaudePageHint("错误文案 1"));
    DOCTEST_CHECK(!needsClaudePageHint("读取任务失败：库锁"));
    DOCTEST_CHECK(!needsClaudePageHint("启停切换失败：库锁"));
    DOCTEST_CHECK(!needsClaudePageHint("落库 agent_messages 失败：disk full"));
    DOCTEST_CHECK(!needsClaudePageHint("请求失败：connection refused"));
    return true;
})

REGISTER_TEST(agent_ui_detail_error_hint_navigates_to_claude,
              "agent_ui: 详情弹窗「LLM 配置不可用」错误行下出现「去 Claude 页」可点引导，点击跳转 Claude 页", {
    test_support::TestDb tdb("agentui", "errhint");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("错误引导任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    // 两条 run 分别落两类配置不可用错误原文（runlist 按时间倒序：runlist.1 在前 = 最新）
    auto r1 = store.startRun(created.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kNoProfileErr).isOk());
    auto r2 = store.startRun(created.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kSlotResolveErr).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_hintnav1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_hintnav2");

    // 两处错误行下都有引导（id 契约：可点元素带 text 与 onClick）
    for (const char* row : {"agent.dialog.detail.runlist.0",
                            "agent.dialog.detail.runlist.1"}) {
        const std::string hintId = std::string(row) + ".err.hint";
        const auto* hint = ui2.find(hintId);
        DOCTEST_REQUIRE_MESSAGE(hint != nullptr,
                                std::string(row) + " 下缺「去 Claude 页」引导");
        DOCTEST_CHECK_MESSAGE(hint->text.find("去 Claude 页") != std::string::npos,
                              "引导文案缺「去 Claude 页」，实测: " + hint->text);
        DOCTEST_REQUIRE_MESSAGE(hint->onClick != nullptr,
                                 "引导元素不可点（缺 onClick）");
    }
    // 引导行与错误行不重叠（hint 也是 scroll 内容的子元素）；左栏内不越容器
    const auto* err0 = ui2.find("agent.dialog.detail.runlist.0.err");
    const auto* hint0 = ui2.find("agent.dialog.detail.runlist.0.err.hint");
    const auto* runlistBox = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(err0 != nullptr && hint0 != nullptr && runlistBox != nullptr);
    DOCTEST_CHECK(err0->frame.y + err0->frame.height <= hint0->frame.y + 0.5f);
    DOCTEST_CHECK_MESSAGE(
        hint0->frame.y + hint0->frame.height <=
            runlistBox->frame.y + runlistBox->frame.height,
        "引导行越出时间线容器：hint 底 " +
            std::to_string(hint0->frame.y + hint0->frame.height) + " 容器底 " +
            std::to_string(runlistBox->frame.y + runlistBox->frame.height));

    // 点击引导 → Router 切 Claude 页（须 ClaudeProxy 已注册，否则回退 Home）
    ::core::Router router;
    router.registerModule(&mod);
    StubPageModule claudePage(core::PageId::ClaudeProxy);
    router.registerModule(&claudePage);
    test_support::ScopedAppRouter scoped(&router);
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.0.err.hint"));
    DOCTEST_CHECK_MESSAGE(router.currentPage() == ::core::PageId::ClaudeProxy,
                          "点击引导未跳转 Claude 页，实测: " +
                              std::string(::core::pageIdToString(
                                  router.currentPage())));
    // 第二条错误行的引导同样可跳（从 Claude 页回 Agent 后再点）
    router.navigateTo(::core::PageId::Agent);
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.1.err.hint"));
    DOCTEST_CHECK(router.currentPage() == ::core::PageId::ClaudeProxy);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_error_hint_only_two_classes,
              "agent_ui: 其它错误（网络/上游 4xx/落库/普通文案）不渲染「去 Claude 页」引导", {
    test_support::TestDb tdb("agentui", "errhintneg");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("普通失败任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    const std::string otherErrs[] = {
        "上游 HTTP 401: {\"error\":\"invalid_api_key\"}",
        "请求失败：connection refused",
        "落库 agent_messages 失败：disk full",
        "错误文案 0",
    };
    for (const std::string& err : otherErrs) {
        auto r = store.startRun(created.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), "failed", 1, int64_t(10),
                                        int64_t(1), err).isOk());
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_hintneg1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_hintneg2");
    for (int i = 0; i < 4; ++i) {
        const std::string hintId =
            "agent.dialog.detail.runlist." + std::to_string(i) + ".err.hint";
        DOCTEST_CHECK_MESSAGE(ui2.find(hintId) == nullptr,
                              "非配置不可用类错误渲染了引导: " + hintId);
        const std::string errId =
            "agent.dialog.detail.runlist." + std::to_string(i) + ".err";
        DOCTEST_CHECK(ui2.find(errId) != nullptr);   // 错误行本身仍在
    }

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_page_error_hint_navigates_to_claude,
              "agent_ui: 页内错误行同属「LLM 配置不可用」→ 同样「去 Claude 页」引导（弹窗外路径）", {
    test_support::TestDb tdb("agentui", "pagehint");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("页内引导任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    // 配置不可用类错误两条 run（同一任务，最新在前）
    auto r1 = store.startRun(created.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kSlotResolveErr).isOk());
    auto r2 = store.startRun(created.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "limited", 1, int64_t(10),
                                    int64_t(1), kNoProfileErr).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_pagehint1");
    const auto* pageErr = ui.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(pageErr != nullptr,
                            "run 的配置不可用错误未同步到页内错误行（弹窗外零可见出口）");
    DOCTEST_CHECK(pageErr->text.rfind("LLM 配置不可用", 0) == 0);
    const auto* hint = ui.find("agent.page.error.hint");
    DOCTEST_REQUIRE_MESSAGE(hint != nullptr, "页内错误行下缺「去 Claude 页」引导");
    DOCTEST_CHECK_MESSAGE(hint->text.find("去 Claude 页") != std::string::npos,
                          "引导文案缺「去 Claude 页」，实测: " + hint->text);
    DOCTEST_REQUIRE_MESSAGE(hint->onClick != nullptr, "页内引导不可点（缺 onClick）");

    // 几何：引导行在错误行之下、不压工具区头部；错误+引导挤占的 listH 已扣减
    const auto* toolsHeader = ui.find("agent.tools.header");
    const auto* tasklist = ui.find("agent.tasklist");
    DOCTEST_REQUIRE(toolsHeader != nullptr && tasklist != nullptr);
    DOCTEST_CHECK(pageErr->frame.y + pageErr->frame.height <=
                  hint->frame.y + 0.5f);
    DOCTEST_CHECK(hint->frame.y + hint->frame.height <=
                  toolsHeader->frame.y + 0.5f);
    DOCTEST_CHECK(tasklist->frame.y + tasklist->frame.height <=
                  kScreen.height + 0.5f);

    // 点击引导 → Router 切 Claude 页
    ::core::Router router;
    router.registerModule(&mod);
    StubPageModule claudePage(core::PageId::ClaudeProxy);
    router.registerModule(&claudePage);
    test_support::ScopedAppRouter scoped(&router);
    DOCTEST_REQUIRE(clickId(ui, "agent.page.error.hint"));
    DOCTEST_CHECK_MESSAGE(router.currentPage() == ::core::PageId::ClaudeProxy,
                          "点击页内引导未跳转 Claude 页，实测: " +
                              std::string(::core::pageIdToString(
                                  router.currentPage())));

    mod.onStop();
    return true;
})

// Task 4 补：页内错误行「已展示过的 run 不再回弹」——用户用成功动作清掉错误行后，
// 只要那条 run 还是任务的最近一次 run，后续 refreshData（启停/事件/轮询任一触发）
// 不得把同一份旧错误重新顶回来（旧行为：轮询见 pageError_ 为空即重新同步，粘到有新
// run 为止）。判据三段：①首帧出现；②清除 + **触发一次 refreshData** 后仍为空；
// ③真来了新 run 的失败 → 必须重新出现（防「一律静默」把诊断出口也修死）。
// ②必须带触发条件：只清不刷新时 refreshData 根本不跑（既有用例的绿灯是巧合），
// 回弹只在「清除 + 任一刷新触发」同时成立时发生。
REGISTER_TEST(agent_ui_page_error_no_rebound_until_new_run,
              "agent_ui: 页内错误行已展示的 run 不回弹（清除后刷新仍空；新 run 失败要重现）", {
    test_support::TestDb tdb("agentui", "pagenorebound");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("回弹检查任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto r1 = store.startRun(created.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kNoProfileErr).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // ① 首帧：run 的配置不可用错误同步到页内错误行（既有行为，别修坏）
    eui::Ui ui1;
    composeTo(ui1, mod, kScreen, "agent_ui_pagenorebound1");
    DOCTEST_REQUIRE_MESSAGE(ui1.find("agent.page.error") != nullptr,
                            "首帧页内错误行缺失（同步路径被修坏）");

    // ② 用户清除 + 触发刷新：启停成功会清错误行并置 tasksDirty_，下一帧必进
    //    refreshData —— 旧行为下同一份旧错误就在这里回弹。
    DOCTEST_REQUIRE_MESSAGE(clickId(ui1, "agent.task." + tid + ".toggle"),
                            "启停 chip 缺失");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_pagenorebound2");
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.page.error") == nullptr,
                          "清除后旧 run 的错误回弹（同一条 run 被重复同步）");

    // ③ 任务真跑了新一次且又失败 → 新错误必须重新出现（否则是「一律静默」）
    auto r2 = store.startRun(created.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kNoProfileErr).isOk());
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.task." + tid + ".toggle"),
                            "启停 chip 缺失（第二段）");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_pagenorebound3");
    const auto* again = ui3.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(again != nullptr,
                            "新 run 的失败没有重新出现在页内错误行（诊断出口被误静默）");
    DOCTEST_CHECK(again->text.find("LLM 配置不可用") != std::string::npos);

    mod.onStop();
    return true;
})

// Task 4 剩余项（「多任务同错时取 map 迭序第一条、不可复现」）—— 把语义钉死，
// 使它可复现、可回归：① 多个任务各有「配置不可用」类错误时，页内错误行取
// **taskId 最小者**（`latestRuns_` 是 `std::map<int64_t, ...>`，迭序确定 = 升序，
// 不是随机）；② 用户清掉后同步循环**接着找别的任务**的错（不是「遇错即停」），
// 另一个任务的错误顶上来；③ 两条都展示过 → 不再循环回第一条（`pageErrorRunId_`
// 逐个记住已展示的 run）。
REGISTER_TEST(agent_ui_page_error_multi_task_order,
              "agent_ui: 页内错误行多任务语义（取 taskId 最小者 / 清除后下一个顶上 / 不循环）", {
    test_support::TestDb tdb("agentui", "pagemulti");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    // 两个任务 + 两条**文案不同**的配置不可用错误：可据文案分辨当前展示的是哪一条。
    auto ta = store.createTask("多任务 A", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(ta.isOk());
    auto tb = store.createTask("多任务 B", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(tb.isOk());
    auto ra = store.startRun(ta.value());
    DOCTEST_REQUIRE(ra.isOk());
    DOCTEST_REQUIRE(store.finishRun(ra.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kNoProfileErr).isOk());
    auto rb = store.startRun(tb.value());
    DOCTEST_REQUIRE(rb.isOk());
    DOCTEST_REQUIRE(store.finishRun(rb.value(), "failed", 1, int64_t(10),
                                    int64_t(1), kSlotResolveErr).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // ① 首帧：取 taskId 最小（A）的错
    eui::Ui ui1;
    composeTo(ui1, mod, kScreen, "agent_ui_pagemulti1");
    const auto* err1 = ui1.find("agent.page.error");
    DOCTEST_REQUIRE(err1 != nullptr);
    DOCTEST_CHECK_MESSAGE(err1->text.find(kNoProfileErr) != std::string::npos,
                          "首帧未取 taskId 最小者的错误，实测: " + err1->text);

    // ② 清除 + 触发一次 refreshData（启停成功）→ 同步循环接着找 → B 的错顶上
    DOCTEST_REQUIRE(clickId(ui1, "agent.task." + std::to_string(ta.value()) + ".toggle"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_pagemulti2");
    const auto* err2 = ui2.find("agent.page.error");
    DOCTEST_REQUIRE_MESSAGE(err2 != nullptr,
                            "清除后未接着报另一个任务的错（遇错即停？）");
    DOCTEST_CHECK_MESSAGE(err2->text.find(kSlotResolveErr) != std::string::npos,
                          "顶上来的不是 B 任务的错误，实测: " + err2->text);

    // ③ 再清一次 + 再刷新：两条都已展示过 → 错误行保持空（不循环回 A）
    DOCTEST_REQUIRE(clickId(ui2, "agent.task." + std::to_string(ta.value()) + ".toggle"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_pagemulti3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.page.error") == nullptr,
                          "两条已展示过的错误循环回了（回弹 / 兜圈）");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_page_error_no_hint_other_errors,
              "agent_ui: 页内错误行非「LLM 配置不可用」类（读库失败/普通 run 失败）不渲染引导", {
    test_support::TestDb tdb("agentui", "pagehintneg");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("页内无引导任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "failed", 1, int64_t(10),
                                    int64_t(1), "请求失败：connection refused").isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 普通 run 失败不同步页内错误行（非配置不可用类错误）
    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_pagehintneg1");
    DOCTEST_CHECK(ui.find("agent.page.error") == nullptr);
    DOCTEST_CHECK(ui.find("agent.page.error.hint") == nullptr);

    // 读库失败走页内错误行（既有路径）但不渲染引导
    DOCTEST_REQUIRE(tdb.handle().exec("DELETE FROM agent_tasks WHERE id=" + tid).isOk());
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_pagehintneg2");
    DOCTEST_REQUIRE(clickId(ui2, "agent.task." + tid + ".edit"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_pagehintneg3");
    DOCTEST_REQUIRE(ui3.find("agent.page.error") != nullptr);
    DOCTEST_CHECK(ui3.find("agent.page.error.hint") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_new_task_memory_columns_not_written,
              "agent_ui: 新建弹窗无执行记忆/自动记录 UI；保存不写 context_note/auto_note（行取 DB 默认值）", {
    test_support::TestDb tdb("agentui", "notecreate");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_note1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_note2");

    // W4 C3：执行记忆/自动记录 UI 已移除（打开态 step 0 即不渲染）
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.task.note") == nullptr,
                          "agent.dialog.task.note 应已移除");
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.task.autonote") == nullptr,
                          "agent.dialog.task.autonote 应已移除");

    // 导航 step 2 填 goal → step 3 保存
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_note2s2");
    auto* goalHit = ui2s2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    typeInto(goalHit, "测试执行记忆");
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_note2s3");
    DOCTEST_REQUIRE(clickId(ui2s3, "agent.dialog.task.save.bg"));

    // 读回：两列未被写入 → 取建表 DEFAULT（''/1）
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    auto memCols = tdb.handle().prepare(
        "SELECT context_note, auto_note FROM agent_tasks WHERE id=" +
        std::to_string(tasks.value()[0].id));
    DOCTEST_REQUIRE(memCols.isOk());
    auto memRows = memCols.value().fetchAll();
    DOCTEST_REQUIRE(memRows.isOk());
    DOCTEST_REQUIRE_EQ(memRows.value().size(), 1u);
    DOCTEST_CHECK_EQ(memRows.value()[0].getString("context_note").valueOr("?"),
                     std::string(""));
    DOCTEST_CHECK_EQ(memRows.value()[0].getInt64("auto_note").valueOr(-1), 1);

    // 弹窗关闭
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_note4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.note") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_edit_dialog_memory_columns_untouched,
              "agent_ui: 编辑弹窗无 note/autonote UI；编辑保存不改写 context_note/auto_note", {
    test_support::TestDb tdb("agentui", "noteedit");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("记忆任务", "旧目标", "manual", "default", 10,
                                    false);
    DOCTEST_REQUIRE(created.isOk());
    // 预埋执行记忆旧值（store 已停写两列）：编辑保存后必须原样保留
    DOCTEST_REQUIRE(tdb.handle().exec(
        "UPDATE agent_tasks SET context_note='旧记忆内容', auto_note=0 WHERE id=" +
        std::to_string(created.value())).isOk());
    const std::string tid = std::to_string(created.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_noteedit1");
    DOCTEST_REQUIRE(clickId(ui, "agent.task." + tid + ".edit"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_noteedit2");

    // W4 C3：note/autonote UI 已移除
    DOCTEST_CHECK(ui2.find("agent.dialog.task.note") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.autonote") == nullptr);

    // 导航 step 2 填 goal → step 3 保存
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_noteedit2s2");
    // 不填 goal（仅验证编辑保存不动 context_note/auto_note；goal 不变）
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_noteedit2s3");
    DOCTEST_REQUIRE(clickId(ui2s3, "agent.dialog.task.save.bg"));

    auto updated = store.getTask(created.value());
    DOCTEST_REQUIRE(updated.isOk());
    // 编辑保存不写两列：预埋旧值（'旧记忆内容'/0）原样保留
    auto memCols = tdb.handle().prepare(
        "SELECT context_note, auto_note FROM agent_tasks WHERE id=" +
        std::to_string(created.value()));
    DOCTEST_REQUIRE(memCols.isOk());
    auto memRows = memCols.value().fetchAll();
    DOCTEST_REQUIRE(memRows.isOk());
    DOCTEST_REQUIRE_EQ(memRows.value().size(), 1u);
    DOCTEST_CHECK_EQ(memRows.value()[0].getString("context_note").valueOr("?"),
                     std::string("旧记忆内容"));
    DOCTEST_CHECK_EQ(memRows.value()[0].getInt64("auto_note").valueOr(-1), 0);
    DOCTEST_CHECK_EQ(updated.value().goal, std::string("旧目标"));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_dialog_geometry_sysprompt_within_bounds,
              "agent_ui: 步骤条弹窗内容（系统提示词等）不越弹窗边界 + 行底 ≤ 按钮行顶", {
    test_support::TestDb tdb("agentui", "notegeom");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_notegeo1"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_notegeo2");

    const auto* panel = ui2.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    // W4 C1：弹窗高 496（步骤条重做后）
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.height, 496.0f),
                          "任务弹窗高应为设计值 496，实测 " +
                              std::to_string(panel->frame.height));

    // W4 C3：执行记忆/自动记录已移除（打开态 step 0 即不渲染）
    DOCTEST_CHECK(ui2.find("agent.dialog.task.note") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.task.autonote") == nullptr);

    // 导航到 step 2：系统提示词 textarea 在面板内、底 ≤ 按钮行顶
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_notegeo2s2");
    const auto* sysprompt = ui2s2.find("agent.dialog.task.sysprompt");
    DOCTEST_REQUIRE(sysprompt != nullptr);
    DOCTEST_CHECK_MESSAGE(frameContained(*panel, *sysprompt),
                          "系统提示词 textarea 跑出弹窗外");
    const auto* saveBtn = ui2s2.find("agent.dialog.task.save");
    DOCTEST_REQUIRE(saveBtn != nullptr);
    DOCTEST_CHECK_MESSAGE(
        sysprompt->frame.y + sysprompt->frame.height <= saveBtn->frame.y + 0.5f,
        "系统提示词底 " +
            std::to_string(sysprompt->frame.y + sysprompt->frame.height) +
            " 压到按钮行顶 " + std::to_string(saveBtn->frame.y));

    // 导航到 step 3：安全组破坏性行 y 恒等面板顶 +216（W4 步骤条后）
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_notegeo2s3");
    const auto* metaRow = ui2s3.find("agent.dialog.task.destructive");
    DOCTEST_REQUIRE(metaRow != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(metaRow->frame.y, panel->frame.y + 214.0f),
                          "破坏性行 y 应恒为面板顶 +214（W4 步骤条后），实测 " +
                              std::to_string(metaRow->frame.y));

    mod.onStop();
    return true;
})

// ---- 任务级 run 汇总（详情弹窗汇总行）：纯函数三件 + 弹窗接线 ----

REGISTER_TEST(agent_format_run_duration_text,
              "agent_summary: 时长秒 → 紧凑中文（负值钳 0 / 无零秒尾 / 天时进位）", {
    using module::agent::formatRunDurationText;
    DOCTEST_CHECK_EQ(formatRunDurationText(0), std::string("0秒"));
    DOCTEST_CHECK_EQ(formatRunDurationText(45), std::string("45秒"));
    DOCTEST_CHECK_EQ(formatRunDurationText(60), std::string("1分"));
    DOCTEST_CHECK_EQ(formatRunDurationText(133), std::string("2分13秒"));
    DOCTEST_CHECK_EQ(formatRunDurationText(3600), std::string("1时"));
    DOCTEST_CHECK_EQ(formatRunDurationText(4980), std::string("1时23分"));
    DOCTEST_CHECK_EQ(formatRunDurationText(90000), std::string("1天1时"));
    DOCTEST_CHECK_EQ(formatRunDurationText(-5), std::string("0秒"));   // 时钟回拨
    return true;
})

// formatRunDurationText 的直测保留在 agent_time_text_formats（test_agent_time_text.cpp，
// 文案契约含「0秒」回拨钳不变）；formatRunResultTimeText / formatTaskRunSummaryText
// 已随「汇总行文本」退休，旧直测一并删除（spec §11/§12.2）。

// 弹窗接线（UAT6 方案 A 布局壳）：摘要三卡常驻（无 run 也渲染），三卡/segment/
// tab 内容按布局公式排列（panel 内坐标：summaryY=64、summaryH=81、navY=159、
// contentY=199；宽 = max(720, 窗口/2)，kScreen 1600 → 800×660、innerW=744）。
// 旧「配置头 + 左右分栏」几何随方案 A 单列三档作废；卡片真实数值断言保留
REGISTER_TEST(agent_ui_detail_dialog_run_summary,
              "agent_ui: 详情弹窗摘要三卡常驻 + 三卡/segment/tab 内容按布局公式排列（无 run 几何不变）", {
    test_support::TestDb tdb("agentui", "summary");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto with = store.createTask("有汇总", "g", "schedule", "default", 10, false,
                                 R"({"cron":"0 9 * * *"})");
    auto without = store.createTask("无汇总", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(with.isOk() && without.isOk());
    const std::string tid = std::to_string(with.value());
    const std::string nid = std::to_string(without.value());
    for (int i = 0; i < 3; ++i) {
        auto r = store.startRun(with.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), i == 1 ? "failed" : "completed",
                                        1, std::nullopt, std::nullopt,
                                        std::nullopt)
                            .isOk());
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_sum1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".detail"),
                            "任务行缺 agent.task.<id>.detail");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_sum2");
    // 三卡常驻：标签在位（值是占位，真实数据 P2 接管）
    const auto* lastCard = ui2.find("agent.dialog.detail.summary.last");
    const auto* nextCard = ui2.find("agent.dialog.detail.summary.next");
    const auto* statsCard = ui2.find("agent.dialog.detail.summary.stats");
    DOCTEST_REQUIRE_MESSAGE(lastCard != nullptr && nextCard != nullptr &&
                                statsCard != nullptr,
                            "详情弹窗缺摘要三卡");
    const auto* lastLabel = ui2.find("agent.dialog.detail.summary.last.label");
    DOCTEST_REQUIRE(lastLabel != nullptr);
    DOCTEST_CHECK(lastLabel->text == "最近一次");
    const auto* statsLabel = ui2.find("agent.dialog.detail.summary.stats.label");
    DOCTEST_REQUIRE(statsLabel != nullptr);
    DOCTEST_CHECK(statsLabel->text == "近 7 天");
    // P2：三卡真数据——7d 窗口内 3 run（2 成功 1 失败）；最近一次 = 第 3 条
    // completed；schedule 任务下一次给具体时间（今天/明天 09:00，不钉死哪天）
    const auto* lastValue = ui2.find("agent.dialog.detail.summary.last.value");
    const auto* nextValue = ui2.find("agent.dialog.detail.summary.next.value");
    const auto* statsValue = ui2.find("agent.dialog.detail.summary.stats.value");
    DOCTEST_REQUIRE_MESSAGE(lastValue != nullptr && nextValue != nullptr &&
                                statsValue != nullptr,
                            "摘要三卡缺 .value 节点");
    DOCTEST_CHECK(lastValue->text.find("成功") != std::string::npos);
    DOCTEST_CHECK_MESSAGE(statsValue->text == "成功 2 · 失败 1",
                          "7d 统计应显「成功 2 · 失败 1」，实测: " +
                              statsValue->text);
    const auto* statsSub = ui2.find("agent.dialog.detail.summary.stats.sub");
    DOCTEST_REQUIRE(statsSub != nullptr);
    DOCTEST_CHECK(statsSub->text.find("其中受限 0") != std::string::npos);
    DOCTEST_CHECK(statsSub->text.find("66%") != std::string::npos);   // 2/3 整数截断
    DOCTEST_CHECK_MESSAGE(!nextValue->text.empty() && nextValue->text != "—",
                          "schedule 任务「下一次」应给具体时间，实测: " +
                              nextValue->text);
    DOCTEST_CHECK(nextValue->text.find("09:00") != std::string::npos);
    const auto* nextSub = ui2.find("agent.dialog.detail.summary.next.sub");
    DOCTEST_REQUIRE(nextSub != nullptr);
    DOCTEST_CHECK(nextSub->text.find("定时") != std::string::npos);
    // 布局公式（方案 A 单列三档，弹性 1600 屏 → 面板 800×660）：
    // summaryY=64 / summaryH=81 / navY=159（摘要底 +14）/ contentY=199（nav 底 +8）
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    const auto* nav = ui2.find("agent.dialog.detail.nav");
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(panel != nullptr && nav != nullptr && runlist != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(lastCard->frame.y - panel->frame.y, 48.0f),
                          "摘要卡应钉面板顶 +64（头部带之下），实测 " +
                              std::to_string(lastCard->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(nearPx(lastCard->frame.height, 81.0f),
                          "摘要卡高应 81，实测 " +
                              std::to_string(lastCard->frame.height));
    // 三卡横排：等宽、水平间隙 8、同 y
    DOCTEST_CHECK_MESSAGE(nearPx(nextCard->frame.y, lastCard->frame.y),
                          "摘要三卡应同排（y 相等）");
    DOCTEST_CHECK_MESSAGE(
        nearPx(nextCard->frame.x - (lastCard->frame.x + lastCard->frame.width),
               8.0f),
        "摘要卡水平间隙应 8，实测 " +
            std::to_string(nextCard->frame.x - lastCard->frame.x -
                           lastCard->frame.width));
    DOCTEST_CHECK_MESSAGE(nearPx(lastCard->frame.width, nextCard->frame.width),
                          "摘要卡应等宽三等分");
    DOCTEST_CHECK_MESSAGE(
        nearPx(nav->frame.y - panel->frame.y, 143.0f),
        "segment 应在面板顶 +143（实测锚定），实测 " +
            std::to_string(nav->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(nearPx(nav->frame.width, 752.0f),
                          "segment 应全宽 752（弹性宽 800-48，裁定 2：内容区双侧 24px），实测 " +
                              std::to_string(nav->frame.width));
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.y - panel->frame.y, 183.0f),
        "tab 内容区应在面板顶 +183（navY 143 + kNavH 32 + 8，UAT6.1 顶部收紧后），实测 " +
            std::to_string(runlist->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(
        runlist->frame.y + runlist->frame.height <=
            panel->frame.y + panel->frame.height - 23.5f,
        "主区溢出面板底（底部按钮带需让位）");
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.width, 752.0f),
        "执行记录应单列全宽 752（旧左右分栏已退役），实测 " +
            std::to_string(runlist->frame.width));

    // 无 run 的任务：三卡照常渲染（空态文案），几何与有 run 完全一致（不再下移）
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.close"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_sum3");
    DOCTEST_REQUIRE(clickId(ui3, "agent.task." + nid + ".detail"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_sum4");
    const auto* last4 = ui4.find("agent.dialog.detail.summary.last");
    const auto* lastValue4 = ui4.find("agent.dialog.detail.summary.last.value");
    const auto* statsValue4 =
        ui4.find("agent.dialog.detail.summary.stats.value");
    DOCTEST_REQUIRE_MESSAGE(last4 != nullptr && lastValue4 != nullptr &&
                                statsValue4 != nullptr,
                            "无 run 任务的三卡缺席（摘要卡必须常驻）");
    DOCTEST_CHECK(lastValue4->text == "从未执行");
    DOCTEST_CHECK(statsValue4->text == "近 7 天暂无执行");
    const auto* panel4 = ui4.find("agent.dialog.detail.panel");
    const auto* runlist4 = ui4.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE_MESSAGE(panel4 != nullptr && runlist4 != nullptr,
                            "无 run 任务缺面板/主区");
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist4->frame.y - panel4->frame.y, 183.0f),
        "无 run 任务主区几何应与有 run 一致（+183），实测 " +
            std::to_string(runlist4->frame.y - panel4->frame.y));

    mod.onStop();
    return true;
})

// 汇总查询失败必须显形（静默失败面）：摘要卡 last/stats 值转「读取失败」
REGISTER_TEST(agent_ui_detail_summary_error_visible,
              "agent_ui: run 汇总查询失败 → 弹窗内显形错误行（不静默）", {
    test_support::TestDb tdb("agentui", "sumerr");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("汇总失败", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_serr1");
    // 摘掉聚合源表：writeVersion 被这次写 bump，下一次 refreshData 的汇总查询必失败
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TABLE agent_runs").isOk());
    DOCTEST_REQUIRE(clickId(ui, "agent.task." + tid + ".detail"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_serr2");
    // 汇总失败显形（spec 边界表）：last/stats 卡值转「读取失败」，不静默
    const auto* lastValue = ui2.find("agent.dialog.detail.summary.last.value");
    const auto* statsValue = ui2.find("agent.dialog.detail.summary.stats.value");
    DOCTEST_REQUIRE_MESSAGE(lastValue != nullptr && statsValue != nullptr,
                            "汇总查询失败没有任何可见反馈（静默路径）");
    DOCTEST_CHECK_MESSAGE(lastValue->text == "读取失败",
                          "last 卡值应显「读取失败」，实测：" + lastValue->text);
    DOCTEST_CHECK_MESSAGE(statsValue->text == "读取失败",
                          "stats 卡值应显「读取失败」，实测：" +
                              statsValue->text);

    mod.onStop();
    return true;
})

// ---- 布局壳三尺寸（UAT6 方案 A：单列三档 tab 一套几何走全尺寸；钳制口径 = 上游
// overlay token 复算）。三个无头用例各钉一档：弹性档（1600 屏 → 800×660）/
// 窄窗钳制（700×560 → 652×512）/ 矮窗钳制（900×600 → 720×552）

REGISTER_TEST(agent_ui_detail_shell_wide_default,
              "agent_ui: 布局壳弹性档 800×660——segment 三档薄荷绿 primary（D-12 令牌统一）/ tab 互斥 / 消息折叠展开档位 / summaryH=81", {
    test_support::TestDb tdb("agentui", "shellwide");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("宽屏壳", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    // 种一条消息：msgscroll 只在有消息时渲染，档位断言要有真实内容才有判别力
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                        R"([{"type":"text","text":"你好"}])",
                                        std::nullopt)
                        .isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_shellw1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_shellw2");

    // 1600×1000 视口：panel = 弹性宽 max(720, 1600/2)=800 × 660
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 800.0f) &&
                              nearPx(panel->frame.height, 552.0f),
                          "弹性档应为 800×660（max(720, 窗口/2)），实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));
    // segment 三档（方案 A：全尺寸同一 nav，旧「宽屏无 nav」档位退役）
    const auto& tok = ::core::theme::current();
    const auto* navOn = ui2.find("agent.dialog.detail.nav.item.0.bg");
    const auto* navOff = ui2.find("agent.dialog.detail.nav.item.1.bg");
    DOCTEST_REQUIRE_MESSAGE(navOn != nullptr && navOff != nullptr,
                            "segment 缺 .nav.item.<i>.bg（三档手搓 segment）");
    // 正向：选中块 = 薄荷绿 primary（D-12 迁移后 primary 槽位即 #36D399，
    // 与产品码 t.colors.primary 同源；任务弹窗 segment 同语言）
    DOCTEST_CHECK_MESSAGE(
        nearPx(navOn->color.r, tok.colors.primary.r) &&
            nearPx(navOn->color.g, tok.colors.primary.g) &&
            nearPx(navOn->color.b, tok.colors.primary.b),
        "segment 选中块应为薄荷绿 primary 底，实测 rgb(" +
            std::to_string(navOn->color.r) + "," + std::to_string(navOn->color.g) +
            "," + std::to_string(navOn->color.b) + ")");
    // 旧「反向锁死暖橙」断言已删：W2.3（b6e3161d）扫荡后 primary 槽位就是
    // 薄荷绿（暖橙旧槽在 D-12 迁移中退役），该断言与正向断言共用同一令牌、
    // 自相矛盾恒红。暖橙令牌已不存在，无可锁对象。
    // 未选中：灰底深字（background 底 + textPrimary 文字）
    DOCTEST_CHECK_MESSAGE(
        nearPx(navOff->color.r, tok.colors.background.r) &&
            nearPx(navOff->color.g, tok.colors.background.g) &&
            nearPx(navOff->color.b, tok.colors.background.b),
        "segment 未选中块应为 background 灰底");
    // 按钮文字在 EUI-NEO 逐态实现里恒为 .text.normal（core::Color 默认 a=1，
// textHover/textPressed 判空分支永不成立），不存在单节点 .text。
    const auto* navLabel2 = ui2.find("agent.dialog.detail.nav.item.2.text.normal");
    DOCTEST_REQUIRE(navLabel2 != nullptr);
    DOCTEST_CHECK_EQ(navLabel2->text, std::string("任务配置"));
    // tab 互斥（tab 0 默认）：runlist 在场，运行详情/配置退场
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.scroll") == nullptr);
    const auto* lastCard = ui2.find("agent.dialog.detail.summary.last");
    DOCTEST_REQUIRE(lastCard != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(lastCard->frame.height, 81.0f),
                          "摘要卡高应 81");
    // 头部触发摘要非空（读取失败时才允许为失败文案）
    const auto* hint = ui2.find("agent.dialog.detail.header.meta");
    DOCTEST_REQUIRE(hint != nullptr);
    DOCTEST_CHECK(!hint->text.empty());
    // 运行详情 tab：消息默认折叠（无 msgscroll / detail.scroll），展开后 msgscroll 在场
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, kScreen, "agent_ui_shellw2n");
    DOCTEST_CHECK_MESSAGE(ui2n.find("agent.dialog.detail.detail.scroll") == nullptr,
                          "弹性档正常高不应整栏滚动（应 msgscroll 模式）");
    DOCTEST_CHECK_MESSAGE(ui2n.find("agent.dialog.detail.msgscroll") == nullptr,
                          "消息区默认应折叠（无 .msgscroll）");
    DOCTEST_REQUIRE(clickId(ui2n, "agent.dialog.detail.msgs.toggle"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_shellw3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.detail.detail.scroll") == nullptr,
                          "弹性档展开消息也不应整栏滚动");
    DOCTEST_CHECK(ui3.find("agent.dialog.detail.msgscroll") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_shell_narrow_tabs,
              "agent_ui: 布局壳 700×560 钳制档——panel 652×512、同一三档 nav（旧窄屏 2-tab 分叉退役）、tab 0/1/2 互斥切换", {
    test_support::TestDb tdb("agentui", "shellnarrow");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("窄屏壳", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 700−48=652 / 560−48=512（钳制走上游 overlay token 同一公式）
    const eui::Screen screen{700.0f, 560.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_shelln1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_shelln2");

    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 652.0f) &&
                              nearPx(panel->frame.height, 512.0f),
                          "窄窗面板应钳到 652×512，实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));
    // 方案 A：同一套三档 nav（与弹性档同 id 同结构——旧窄屏 2-tab 分叉退役）
    const auto* nav = ui2.find("agent.dialog.detail.nav");
    DOCTEST_REQUIRE_MESSAGE(nav != nullptr, "窄窗缺 segment nav");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.0.bg") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.1.bg") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.2.bg") != nullptr);
    const auto* navItem1Bg = ui2.find("agent.dialog.detail.nav.item.1.bg");
    DOCTEST_REQUIRE(navItem1Bg != nullptr);
    // 单列几何与弹性档同一公式：navY=159 / contentY=199，主内容全宽 = innerW 596
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(runlist != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(nav->frame.y - panel->frame.y, 143.0f),
                          "钳制档 nav 应 +159（与弹性档同公式），实测 " +
                              std::to_string(nav->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.x - panel->frame.x, 24.0f) &&
            nearPx(runlist->frame.width, 604.0f),
        "钳制档主内容应单列全宽（x0=24、宽 604=652-48），实测 x=" +
            std::to_string(runlist->frame.x - panel->frame.x) + " 宽 " +
            std::to_string(runlist->frame.width));
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.y - panel->frame.y, 183.0f),
        "钳制档内容应 +183（navY143+kNavH32+8，与弹性档同公式），实测 " +
            std::to_string(runlist->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(runlist->frame.y + runlist->frame.height <=
                              panel->frame.y + panel->frame.height + 0.5f,
                          "窄窗内容溢出面板底");
    // tab 0 互斥：无运行详情块 / 无 msgscroll / 无配置滚动区
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.msgscroll") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.scroll") == nullptr);
    // 底部按钮带在钳制档同样在场（几何随面板收缩）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.footer.close.bg") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.footer.edit.bg") != nullptr);

    // 切 tab 1：详情列上桌，时间线退场；再切 tab 2：配置滚动区上桌，前两档退场
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_shelln3");
    DOCTEST_REQUIRE_MESSAGE(ui3.find("agent.dialog.detail.detail") != nullptr,
                            "切「运行详情」tab 后详情列未上桌");
    DOCTEST_CHECK(ui3.find("agent.dialog.detail.runlist") == nullptr);
    DOCTEST_CHECK(ui3.find("agent.dialog.detail.config.scroll") == nullptr);
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.detail.nav.item.2.bg"));
    eui::Ui ui4;
    composeTo(ui4, mod, screen, "agent_ui_shelln4");
    DOCTEST_REQUIRE_MESSAGE(ui4.find("agent.dialog.detail.config.scroll") != nullptr,
                            "切「任务配置」tab 后配置滚动区未上桌");
    DOCTEST_CHECK(ui4.find("agent.dialog.detail.runlist") == nullptr);
    DOCTEST_CHECK(ui4.find("agent.dialog.detail.detail") == nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_shell_short_wide_scroll,
              "agent_ui: 布局壳 900×600 钳制档——panel 720×552；长错误吃满详情预算 → 运行详情走 .detail.scroll 整栏滚、不建 .msgscroll", {
    test_support::TestDb tdb("agentui", "shellshort");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("矮屏壳", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    // 种消息 + 长错误：msgscroll 缺席的断言只有在「有消息可滚」时才有判别力；
    // 长错误块吃掉详情预算 → 消息余高 < 96 才触发整栏滚降级
    DOCTEST_REQUIRE(store.appendMessage(r.value(), 0, "user",
                                        R"([{"type":"text","text":"你好"}])",
                                        std::nullopt)
                        .isOk());
    const std::string longErr(2000, 'E');
    DOCTEST_REQUIRE(store.finishRun(r.value(), "failed", 1, std::nullopt,
                                    std::nullopt, longErr)
                        .isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 900 屏 → max(720, 450)=720 不钳；600−48=552 → 高钳 552
    const eui::Screen screen{900.0f, 600.0f};
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_shells1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_shells2");

    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 720.0f) &&
                              nearPx(panel->frame.height, 552.0f),
                          "矮窗面板应 720×552，实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));
    // tab 0：runlist 全宽在场（同一套三档，无宽窄分叉）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav") != nullptr);
    // 切「运行详情」：长错误吃满预算 → 余高不足 96 → 整栏滚降级（无 .msgscroll）
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, screen, "agent_ui_shells2n");
    const auto* detailScroll = ui2n.find("agent.dialog.detail.detail.scroll");
    DOCTEST_REQUIRE_MESSAGE(detailScroll != nullptr,
                            "矮窗长错误未走 .detail.scroll 整栏滚动");
    DOCTEST_CHECK_MESSAGE(ui2n.find("agent.dialog.detail.msgscroll") == nullptr,
                          "降级态不应再建 .msgscroll（嵌套滚动）");
    DOCTEST_CHECK(ui2n.find("agent.dialog.detail.detail.error.text") != nullptr);
    // 展开消息区（排除「折叠掩盖降级断言」的假绿）：仍走整栏滚
    DOCTEST_REQUIRE(clickId(ui2n, "agent.dialog.detail.msgs.toggle"));
    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_shells3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.detail.msgscroll") == nullptr,
                          "矮窗展开消息也不得建 .msgscroll（嵌套滚动）");
    DOCTEST_CHECK(ui3.find("agent.dialog.detail.detail.scroll") != nullptr);
    DOCTEST_CHECK_MESSAGE(
        detailScroll->frame.y + detailScroll->frame.height <=
            panel->frame.y + panel->frame.height - 23.5f,
        "整栏滚动区溢出面板底");

    mod.onStop();
    return true;
})

// 任务列表「配置异常」徽章：坏配置任务在列表里必须显形——Scheduler 每跑必跳过
//（scheduler.cpp 的 decode / parseCron 失败分支），而用户看到的却是「任务配好了，
// 什么都没发生」。三类坏配置各落一行；对照组（合法 cron / 合法事件 / manual）
// 一个标记都不许有（正常路径零噪音）；原因落详情弹窗配置摘要（徽章只有 4 字）。
REGISTER_TEST(agent_ui_task_config_problem_badge,
              "agent_ui: 任务列表配置异常徽章——坏配置显形 / 正常配置零噪音 / 详情给原因", {
    test_support::TestDb tdb("agentui", "cfgbadge");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    // 坏配置三条（store 直种不校验，模拟旧数据/手工改库）：cron 表达式非法 /
    // cron 型坏 JSON / 事件型配置解出来但永不可能命中
    auto badCron = store.createTask("坏 cron", "g", "schedule", "default", 10, false,
                                    R"({"cron":"0 9 * * 8"})");
    auto badJson = store.createTask("坏 JSON", "g", "schedule", "default", 10, false,
                                    "{不是 JSON");
    auto badEvent = store.createTask("坏事件", "g", "event", "default", 10, false,
                                     R"({"events":[]})");
    // 对照组三条：合法 cron / 合法事件 / manual（manual 不消费 trigger_config）
    auto okCron = store.createTask("正常 cron", "g", "schedule", "default", 10, false,
                                   R"({"cron":"0 9 * * 1-5"})");
    auto okEvent = store.createTask("正常事件", "g", "event", "default", 10, false,
                                    R"({"events":["todo.created"]})");
    auto okManual = store.createTask("手动任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(badCron.isOk() && badJson.isOk() && badEvent.isOk() &&
                    okCron.isOk() && okEvent.isOk() && okManual.isOk());
    const std::string tidBadCron = std::to_string(badCron.value());
    const std::string tidBadJson = std::to_string(badJson.value());
    const std::string tidBadEvent = std::to_string(badEvent.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_cfgbadge1");

    // ① 三类坏配置都必须在列表里有可见标记
    for (const std::string& tid : {tidBadCron, tidBadJson, tidBadEvent}) {
        const auto* warn = ui.find("agent.task." + tid + ".warn");
        DOCTEST_REQUIRE_MESSAGE(warn != nullptr,
                                "配置异常任务 " + tid + " 在列表上无标记（静默失败）");
        DOCTEST_CHECK_MESSAGE(warn->text == "配置异常",
                              "徽章文案应为「配置异常」，实测：" + warn->text);
        // 徽章要落在行内、不与标题/状态重叠（宽度预算算错即红）
        const auto* row = ui.find("agent.task." + tid);
        const auto* title = ui.find("agent.task." + tid + ".title");
        const auto* status = ui.find("agent.task." + tid + ".status");
        DOCTEST_REQUIRE(row != nullptr && title != nullptr && status != nullptr);
        DOCTEST_CHECK_MESSAGE(frameContained(*row, *warn), "徽章跑到任务行外");
        DOCTEST_CHECK_MESSAGE(
            title->frame.x + title->frame.width <= warn->frame.x + 0.5f,
            "标题与徽章重叠：标题右缘 " +
                std::to_string(title->frame.x + title->frame.width) +
                " > 徽章左缘 " + std::to_string(warn->frame.x));
        DOCTEST_CHECK_MESSAGE(warn->frame.x + warn->frame.width <= status->frame.x + 0.5f,
                              "徽章与状态列重叠");
        // 徽章配色须与正文不同（否则等于没标记）
        DOCTEST_CHECK(warn->textColor.r != title->textColor.r ||
                      warn->textColor.g != title->textColor.g ||
                      warn->textColor.b != title->textColor.b);
    }
    // ② 对照组：正常配置/不消费 trigger_config 的任务一个标记都不许有
    for (int64_t id : {okCron.value(), okEvent.value(), okManual.value()}) {
        DOCTEST_CHECK_MESSAGE(
            ui.find("agent.task." + std::to_string(id) + ".warn") == nullptr,
            "正常任务 " + std::to_string(id) + " 被挂了配置异常标记（噪音）");
    }

    // ③ 原因进任务配置 tab problem 字段（方案 A 收进 tab；徽章只有 4 字）
    DOCTEST_REQUIRE_MESSAGE(openDetailDialog(ui, mod, tidBadCron),
                            "任务行缺详情 chip");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_cfgbadge2");
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.2.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_cfgbadge3");
    const auto* problemVal =
        ui3.find("agent.dialog.detail.config.body.problem.value");
    DOCTEST_REQUIRE_MESSAGE(problemVal != nullptr,
                            "任务配置 tab 缺 .config.body.problem.value");
    DOCTEST_CHECK_MESSAGE(problemVal->text.find("cron") != std::string::npos,
                          "配置 tab 未给出配置异常原因，实测：" + problemVal->text);
    // 异常字段用 error 色显形（spec §9.1）——与普通值同色即白显示
    const auto* normalVal =
        ui3.find("agent.dialog.detail.config.body.model.value");
    DOCTEST_REQUIRE(normalVal != nullptr);
    DOCTEST_CHECK(problemVal->textColor.r != normalVal->textColor.r ||
                  problemVal->textColor.g != normalVal->textColor.g ||
                  problemVal->textColor.b != normalVal->textColor.b);

    mod.onStop();
    return true;
})

// ---- 限次 / 一次性任务（max_runs）的 UI 面：行为开关行开关 + 列表徽章 + 详情摘要（02）----
// 语义：开关勾上 = trigger_config 里写 max_runs=1（「只跑一次」），不勾 = 不写该键
//（旧行为：恒循环）。开关只对 schedule / event 渲染——manual / resident 的
// trigger_config 不被 Scheduler 消费，给它们写上限 = 写完就骗人的死配置

REGISTER_TEST(agent_ui_dialog_once_only_toggle_persists_and_roundtrips,
              "agent_ui: 「只跑一次」开关 → 落库 max_runs=1；编辑回填勾选且原样回写", {
    test_support::TestDb tdb("agentui", "onceonly");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_once1"));
    // openNewTaskDialog 是先 compose 后点击——弹窗节点要再 compose 一帧才可见
    eui::Ui ui2s0;
    composeTo(ui2s0, mod, kScreen, "agent_ui_once1b");
    // W4 步骤条：step 2 填 goal
    eui::Ui ui2s2;
    DOCTEST_REQUIRE(clickId(ui2s0, "agent.dialog.task.step.2.bg"));
    composeTo(ui2s2, mod, kScreen, "agent_ui_once2s2");
    auto* goalHit = ui2s2.find("agent.dialog.task.goal.hit");
    DOCTEST_REQUIRE(goalHit != nullptr);
    typeInto(goalHit, "18 点提醒一次");
    // 默认触发方式 = 手动 → step 3 不许渲染「只跑一次」开关（写了也没人消费）
    eui::Ui ui2s3;
    DOCTEST_REQUIRE(clickId(ui2s2, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3, mod, kScreen, "agent_ui_once2s3");
    DOCTEST_CHECK_MESSAGE(ui2s3.find("agent.dialog.task.limit") == nullptr,
                          "手动触发不该渲染「只跑一次」开关（写了也没人消费）");
    // step 1 切事件型 → step 3 开关出现（事件型同样能配一次性：下次保存笔记时跑一次）
    eui::Ui ui2s1;
    DOCTEST_REQUIRE(clickId(ui2s3, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1, mod, kScreen, "agent_ui_once2s1");
    DOCTEST_REQUIRE(callDropdownChange(ui2s1, "agent.dialog.task.trigger", 3));
    eui::Ui ui2s3b;
    DOCTEST_REQUIRE(clickId(ui2s1, "agent.dialog.task.step.3.bg"));
    composeTo(ui2s3b, mod, kScreen, "agent_ui_once2s3b");
    DOCTEST_CHECK_MESSAGE(ui2s3b.find("agent.dialog.task.limit") != nullptr,
                          "事件触发缺「只跑一次」开关");
    // step 1 切定时 → step 3 开关在 + 文案是大白话（不是 max_runs 术语）
    eui::Ui ui2s1c;
    DOCTEST_REQUIRE(clickId(ui2s3b, "agent.dialog.task.step.1.bg"));
    composeTo(ui2s1c, mod, kScreen, "agent_ui_once2s1c");
    DOCTEST_REQUIRE(callDropdownChange(ui2s1c, "agent.dialog.task.trigger", 1));
    mod.setEditCronDraft("0 18 * * *");
    eui::Ui ui3;
    DOCTEST_REQUIRE(clickId(ui2s1c, "agent.dialog.task.step.3.bg"));
    composeTo(ui3, mod, kScreen, "agent_ui_once3");
    const auto* limitLabel = ui3.find("agent.dialog.task.limit.label");
    DOCTEST_REQUIRE_MESSAGE(limitLabel != nullptr, "「只跑一次」开关缺文案");
    DOCTEST_CHECK_EQ(limitLabel->text, std::string("只跑一次"));
    DOCTEST_REQUIRE_MESSAGE(clickId(ui3, "agent.dialog.task.limit.hit"),
                            "「只跑一次」开关命中面缺失");

    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_once4");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui4, "agent.dialog.task.save.bg"), "保存按钮缺失");

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), static_cast<size_t>(1));
    const auto& row = tasks.value()[0];
    DOCTEST_CHECK_EQ(row.triggerConfig,
                     std::string(R"({"cron":"0 18 * * *","max_runs":1})"));

    // 再开编辑：开关回填勾选（knob 在右端）+ 保存不改写配置（预填→原样回写的往返）
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_once5");
    DOCTEST_REQUIRE(clickId(ui5, "agent.task." + std::to_string(row.id) + ".edit"));
    eui::Ui ui6s0;
    composeTo(ui6s0, mod, kScreen, "agent_ui_once6s0");
    eui::Ui ui6;
    DOCTEST_REQUIRE(clickId(ui6s0, "agent.dialog.task.step.3.bg"));
    composeTo(ui6, mod, kScreen, "agent_ui_once6");
    const auto* knob = ui6.find("agent.dialog.task.limit.knob");
    const auto* track = ui6.find("agent.dialog.task.limit.track");
    DOCTEST_REQUIRE_MESSAGE(knob != nullptr && track != nullptr,
                            "编辑态「只跑一次」开关缺失");
    DOCTEST_CHECK_MESSAGE(knob->frame.x > track->frame.x + 1.0f,
                          "max_runs=1 的任务编辑态开关未勾选：knob.x=" +
                              std::to_string(knob->frame.x) +
                              " track.x=" + std::to_string(track->frame.x));
    DOCTEST_REQUIRE(clickId(ui6, "agent.dialog.task.save.bg"));
    auto again = store.getTask(row.id);
    DOCTEST_REQUIRE(again.isOk());
    DOCTEST_CHECK_EQ(again.value().triggerConfig, row.triggerConfig);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_task_run_limit_badge_and_detail_text,
              "agent_ui: 限次任务（max_runs）列表徽章 + 配置浮层字段写明跑完自动停用；不限次任务零噪音", {
    test_support::TestDb tdb("agentui", "limitbadge");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto once = store.createTask("只跑一次", "g", "schedule", "default", 10, false,
                                 R"({"cron":"0 18 * * *","max_runs":1})");
    auto twice = store.createTask("跑两次", "g", "event", "default", 10, false,
                                  R"({"events":["md.saved"],"max_runs":2})");
    auto plain = store.createTask("普通任务", "g", "schedule", "default", 10, false,
                                  R"({"cron":"0 9 * * *"})");
    // 对照组：manual 不消费 trigger_config（Scheduler 两条过滤都不含它）→ 不挂徽章
    auto manualLimited = store.createTask("手动限量", "g", "manual", "default", 10,
                                          false, R"({"cron":"0 9 * * *","max_runs":1})");
    DOCTEST_REQUIRE(once.isOk() && twice.isOk() && plain.isOk() && manualLimited.isOk());
    const std::string tidOnce = std::to_string(once.value());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_limitbadge1");

    const auto* badgeOnce = ui.find("agent.task." + tidOnce + ".limit");
    DOCTEST_REQUIRE_MESSAGE(badgeOnce != nullptr, "限次任务在列表上无徽章（看不出是一次性）");
    DOCTEST_CHECK_EQ(badgeOnce->text, std::string("一次性"));
    const auto* badgeTwice =
        ui.find("agent.task." + std::to_string(twice.value()) + ".limit");
    DOCTEST_REQUIRE_MESSAGE(badgeTwice != nullptr, "max_runs=2 的任务无徽章");
    DOCTEST_CHECK_EQ(badgeTwice->text, std::string("限2次"));
    // 徽章落在行内、不压标题 / 状态列（宽度预算算错即红）
    const auto* row = ui.find("agent.task." + tidOnce);
    const auto* title = ui.find("agent.task." + tidOnce + ".title");
    const auto* status = ui.find("agent.task." + tidOnce + ".status");
    DOCTEST_REQUIRE(row != nullptr && title != nullptr && status != nullptr);
    DOCTEST_CHECK_MESSAGE(frameContained(*row, *badgeOnce), "徽章跑到任务行外");
    DOCTEST_CHECK_MESSAGE(title->frame.x + title->frame.width <= badgeOnce->frame.x + 0.5f,
                          "标题与徽章重叠");
    DOCTEST_CHECK_MESSAGE(badgeOnce->frame.x + badgeOnce->frame.width <=
                              status->frame.x + 0.5f,
                          "徽章与状态列重叠");
    // 对照组零噪音
    for (int64_t id : {plain.value(), manualLimited.value()}) {
        DOCTEST_CHECK_MESSAGE(
            ui.find("agent.task." + std::to_string(id) + ".limit") == nullptr,
            "不限次任务 " + std::to_string(id) + " 被挂了限次徽章（噪音）");
    }

    // 限次语义迁任务配置 tab 字段（方案 A 收进 tab；徽章只有 3 字）
    DOCTEST_REQUIRE_MESSAGE(openDetailDialog(ui, mod, tidOnce), "任务行缺详情 chip");
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_limitbadge2");
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.2.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_limitbadge3");
    const auto* limitVal =
        ui3.find("agent.dialog.detail.config.body.limit.value");
    DOCTEST_REQUIRE_MESSAGE(limitVal != nullptr,
                            "任务配置 tab 缺 .config.body.limit.value");
    DOCTEST_CHECK_MESSAGE(limitVal->text.find("只跑一次") != std::string::npos,
                          "配置 tab 未写明限次语义，实测：" + limitVal->text);
    DOCTEST_CHECK(limitVal->text.find("跑完自动停用") != std::string::npos);

    mod.onStop();
    return true;
})

// 布局预算（行为开关行）：开关标签盒 = width − 66（EUI-NEO switch.h：labelX = track 46
// + gap 10，左右 inset 各 10），开关对标签没有省略号、只有 Clip —— 宽度给少就是静默裁剪。
// 本用例不看手写常量：直接量「渲染出来的 label 盒宽 ≥ 该节点自己文案的实测字宽」
//（真实字体测量，与 test_font_em_layout.cpp 同口径），源码改盒宽忘了文案预算即红。
// 覆盖面：行为开关行（「允许破坏性操作」+「只跑一次」）与事件分支的三个事件开关。
REGISTER_TEST(agent_ui_dialog_switch_labels_fit_boxes,
              "agent_ui: max_runs 开关「只跑一次」/「允许破坏性操作」/三个事件开关的标签盒容得下各自文案（静默裁剪回归守卫）", {
    test_support::TestDb tdb("agentui", "labelfit");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 标签盒容纳检查：盒宽取自 composeTaskDialog 渲染出的 label 节点 frame，
    // 字宽用真实字体测量。UAT5 事件开关改胶囊 chip（components::button）——
    // 文案在 .text；toggleSwitch 系（limit/destructive）仍在 .label，两形态都查
    auto labelFitProblem = [](eui::Ui& u, const std::string& id) -> std::string {
        const auto* label = u.find(id + ".label");
        if (label == nullptr) {
            // 按钮文字恒为逐态实现 .text.normal（core::Color 默认 alpha=1）
            label = u.find(id + ".text.normal");
        }
        if (label == nullptr) return "缺节点 " + id + ".label/.text.normal";
        const float need =
            core::TextPrimitive::measureTextWidth(label->text, "", 12.0f, 400);
        if (label->frame.width + 0.5f < need) {
            return id + " 盒宽 " + std::to_string(label->frame.width) +
                   " < 字宽 " + std::to_string(need) + "（'" + label->text +
                   "' 会被静默裁剪）";
        }
        return {};
    };

    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_labelfit1"));
    // W4 步骤条：导航 step 1 选定时、step 3 看标签盒
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_labelfit2");
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.task.step.1.bg"));
    eui::Ui ui2s1;
    composeTo(ui2s1, mod, kScreen, "agent_ui_labelfit2s1");
    DOCTEST_REQUIRE(callDropdownChange(ui2s1, "agent.dialog.task.trigger", 1));
    eui::Ui ui3;
    DOCTEST_REQUIRE(clickId(ui2s1, "agent.dialog.task.step.3.bg"));
    composeTo(ui3, mod, kScreen, "agent_ui_labelfit3");
    const std::string destructiveProblem =
        labelFitProblem(ui3, "agent.dialog.task.destructive");
    DOCTEST_CHECK_MESSAGE(destructiveProblem.empty(), destructiveProblem);
    const std::string limitProblem = labelFitProblem(ui3, "agent.dialog.task.limit");
    DOCTEST_CHECK_MESSAGE(limitProblem.empty(), limitProblem);

    // ② W4 步骤条：执行与安全只在 step 3——limit 与模型槽位/迭代上限同排
    //（面板局部 y=116），破坏性开关在「安全」组（116+52+18+20+4=210，开关本体 +4=214）；
    // goal 在 step 2，右缘同属内容列右边界（x0+innerW），跨步几何仍须对齐。
    const auto* destructive = ui3.find("agent.dialog.task.destructive");
    const auto* limit = ui3.find("agent.dialog.task.limit");
    DOCTEST_REQUIRE_MESSAGE(destructive != nullptr && limit != nullptr,
                            "破坏性开关 / 只跑一次节点缺失");
    const auto* panel3 = ui3.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE_MESSAGE(panel3 != nullptr, "任务弹窗面板缺失");
    eui::Ui ui3s2;
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.step.2.bg"));
    composeTo(ui3s2, mod, kScreen, "agent_ui_labelfit3s2");
    const auto* goal = ui3s2.find("agent.dialog.task.goal");
    DOCTEST_REQUIRE_MESSAGE(goal != nullptr, "step 2 缺 goal 输入节点");
    // y 断言用面板相对坐标（绝对 y 含 panel 居中偏移）
    DOCTEST_CHECK_MESSAGE(nearPx(limit->frame.y - panel3->frame.y, 116.0f),
                          "只跑一次应在 step 3 首行（面板局部 y=116），实测 y=" +
                              std::to_string(limit->frame.y - panel3->frame.y));
    DOCTEST_CHECK_MESSAGE(nearPx(destructive->frame.y - panel3->frame.y, 214.0f),
                          "破坏性开关应在安全组（面板局部 y=214），实测 y=" +
                              std::to_string(destructive->frame.y - panel3->frame.y));
    DOCTEST_CHECK_MESSAGE(
        nearPx(limit->frame.x + limit->frame.width,
               goal->frame.x + goal->frame.width),
        "「只跑一次」未与 goal 输入右缘对齐：限次右缘 " +
            std::to_string(limit->frame.x + limit->frame.width) + " goal 右缘 " +
            std::to_string(goal->frame.x + goal->frame.width));

    // ③ 事件分支：先回 step 1 切触发方式（step 3 不渲染 trigger segment），
    // 再进 step 3 查三个事件开关同排标签盒
    eui::Ui ui3s1;
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.step.1.bg"));
    composeTo(ui3s1, mod, kScreen, "agent_ui_labelfit3s1");
    DOCTEST_REQUIRE(callDropdownChange(ui3s1, "agent.dialog.task.trigger", 3));
    // 事件 chip 属 step 1，先在 step 1 帧量标签盒
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_labelfit4");
    for (int i = 0; i < 3; ++i) {
        const std::string eventId = "agent.dialog.task.event." + std::to_string(i);
        const std::string problem = labelFitProblem(ui4, eventId);
        DOCTEST_CHECK_MESSAGE(problem.empty(), problem);
    }
    // 「只跑一次」属 step 3，另起一帧
    eui::Ui ui4s3;
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.task.step.3.bg"));
    composeTo(ui4s3, mod, kScreen, "agent_ui_labelfit4s3");
    const std::string limitProblemEvent =
        labelFitProblem(ui4s3, "agent.dialog.task.limit");
    DOCTEST_CHECK_MESSAGE(limitProblemEvent.empty(), limitProblemEvent);

    // ④ 手动不渲染：给不消费 trigger_config 的任务显示开关等于假承诺
    eui::Ui ui4s1;
    DOCTEST_REQUIRE(clickId(ui4s3, "agent.dialog.task.step.1.bg"));
    composeTo(ui4s1, mod, kScreen, "agent_ui_labelfit4s1");
    DOCTEST_REQUIRE(callDropdownChange(ui4s1, "agent.dialog.task.trigger", 0));
    eui::Ui ui5;
    DOCTEST_REQUIRE(clickId(ui4s1, "agent.dialog.task.step.3.bg"));
    composeTo(ui5, mod, kScreen, "agent_ui_labelfit5");
    DOCTEST_CHECK_MESSAGE(ui5.find("agent.dialog.task.limit") == nullptr,
                          "手动触发渲染了「只跑一次」开关（写了也没人消费）");

    mod.onStop();
    return true;
})

// ---- T7b：AI 草稿 chip + 子弹窗（输入 / 异步生成 / 预填 / 降级警告）----
//
// 异步用例口径：worker 完成只置原子并 requestUiUpdate；无头测试的帧必须自己驱动，
// 轮询循环里每帧新建命名 Ui（eui::Ui 不做赋值复用），settle 后再补一帧取断言面。
// 所有 mock 响应体 = Anthropic content[0].text 包草稿 JSON（转义交给 stringify）。

namespace {

std::string draftResponseBody(const std::string& draftJson) {
    auto escaped = platform::json::stringify(platform::json::makeString(draftJson));
    if (escaped.isFail()) return "{}";
    return "{\"id\":\"m1\",\"stop_reason\":\"end_turn\",\"content\":[{"
           "\"type\":\"text\",\"text\":" +
           escaped.value() +
           "}],\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}";
}

} // namespace

REGISTER_TEST(agent_ui_draft_chip_and_subdialog_lifecycle,
              "agent_ui_draft: 指令行 chip 几何（不压 goal 行 / 不压 ✕）/ 子弹窗开合"
              "（chip·✕·遮罩·Esc 先子后父）/ 外层 496 高（W4 步骤条）", {
    test_support::TestDb tdb("agentui", "draftlife");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui uiA;
    composeTo(uiA, mod, kScreen, "agent_ui_dfl0");
    DOCTEST_REQUIRE(clickId(uiA, "agent.empty.btn.bg"));
    eui::Ui uiB;
    composeTo(uiB, mod, kScreen, "agent_ui_dfl1");

    // 外层几何契约：W4 C1 步骤条重做后 496 高（弹性宽 + 紧凑行预算）
    const auto* panel = uiB.find("agent.dialog.task.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.height, 496.0f),
                          "任务弹窗高应恒为 496，实测 " +
                              std::to_string(panel->frame.height));

    // chip 在 step 2 才渲染；几何：面板内、指令标签行右缘（y=112 h=32）、右缘 ≤ ✕ 左缘
    eui::Ui uiBs2;
    DOCTEST_REQUIRE(clickId(uiB, "agent.dialog.task.step.2.bg"));
    composeTo(uiBs2, mod, kScreen, "agent_ui_dfl1s2");
    const auto* chipBg = uiBs2.find("agent.dialog.task.aichip.bg");
    DOCTEST_REQUIRE(chipBg != nullptr);
    DOCTEST_CHECK_MESSAGE(frameContained(*panel, *chipBg),
                          "AI chip 跑出弹窗面板");
    DOCTEST_CHECK_MESSAGE(nearPx(chipBg->frame.y - panel->frame.y, 112.0f),
                          "AI chip 应 y=112（指令标签行右缘），实测 " +
                              std::to_string(chipBg->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(nearPx(chipBg->frame.height, 32.0f), "AI chip 应高 32");
    const auto* closeBtn = uiBs2.find("agent.dialog.task.close");
    DOCTEST_REQUIRE(closeBtn != nullptr);
    // chip 在面板内，宽 ≤ 面板右缘
    DOCTEST_CHECK_MESSAGE(chipBg->frame.x + chipBg->frame.width <=
                              panel->frame.x + panel->frame.width + 0.5f,
                          "AI chip 右缘越出面板");

    // ① 点 chip 开子弹窗：380 高、输入框/底部按钮带都在面板内、外层仍在
    DOCTEST_REQUIRE(clickId(uiBs2, "agent.dialog.task.aichip.bg"));
    eui::Ui uiC;
    composeTo(uiC, mod, kScreen, "agent_ui_dfl2");
    const auto* dpanel = uiC.find("agent.dialog.task.draft.panel");
    DOCTEST_REQUIRE(dpanel != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(dpanel->frame.height, 380.0f),
                          "AI 草稿子弹窗高应恒为 380，实测 " +
                              std::to_string(dpanel->frame.height));
    const auto* dinput = uiC.find("agent.dialog.task.draft.input");
    const auto* genBtn = uiC.find("agent.dialog.task.draft.generate");
    const auto* dismissBtn = uiC.find("agent.dialog.task.draft.dismiss");
    const auto* genBtnBg = uiC.find("agent.dialog.task.draft.generate.bg");
    const auto* dismissBg = uiC.find("agent.dialog.task.draft.dismiss.bg");
    DOCTEST_REQUIRE(dinput != nullptr);
    DOCTEST_REQUIRE(genBtn != nullptr);
    DOCTEST_REQUIRE(dismissBtn != nullptr);
    DOCTEST_REQUIRE(genBtnBg != nullptr);
    DOCTEST_REQUIRE(dismissBg != nullptr);
    DOCTEST_CHECK_MESSAGE(frameContained(*dpanel, *dinput),
                          "草稿输入框跑出子弹窗");
    DOCTEST_CHECK_MESSAGE(frameContained(*dpanel, *genBtn) &&
                              frameContained(*dpanel, *dismissBtn),
                          "按钮行跑出子弹窗");
    // UAT6 重排：输入区加高填满内容宽（裁定 2：内容区双侧 24px，460−48=412、高 140）
    DOCTEST_CHECK_MESSAGE(
        nearPx(dinput->frame.width, dpanel->frame.width - 48.0f),
        "草稿输入框应填满弹窗内容区，实测宽 " +
            std::to_string(dinput->frame.width));
    DOCTEST_CHECK_MESSAGE(nearPx(dinput->frame.height, 140.0f),
                          "草稿输入框应加高到 140，实测 " +
                              std::to_string(dinput->frame.height));
    // 按钮带：两钮同排整体居中（组中心 == 面板中心）、行底距弹窗底 20（y=h-60）
    DOCTEST_CHECK_MESSAGE(nearPx(genBtn->frame.y, dismissBtn->frame.y),
                          "生成/关闭两钮应同排");
    DOCTEST_CHECK_MESSAGE(
        nearPx(genBtn->frame.y + genBtn->frame.height,
               dpanel->frame.y + dpanel->frame.height - 20.0f),
        "按钮带行底应距弹窗底 20（y=h-60），实测按钮底 " +
            std::to_string(genBtn->frame.y + genBtn->frame.height));
    const float groupCenter =
        (genBtn->frame.x + dismissBtn->frame.x + dismissBtn->frame.width) * 0.5f;
    DOCTEST_CHECK_MESSAGE(
        nearPx(groupCenter, dpanel->frame.x + dpanel->frame.width * 0.5f),
        "按钮组应整体居中，实测组中心 " + std::to_string(groupCenter));
    // 按钮带压不到结果行：结果槽底（面板内 306）在按钮顶上方
    DOCTEST_CHECK_MESSAGE(
        genBtn->frame.y >= dpanel->frame.y + 306.0f - 0.5f,
        "按钮带顶压到结果槽底（面板内 306），实测按钮顶 " +
            std::to_string(genBtn->frame.y - dpanel->frame.y));
    // 微信化色锁：生成=Primary 微信绿、关闭=Secondary 白底（surface）
    const auto& wc = ::core::theme::current().colors;
    auto tone = [](const eui::Color& x, const eui::Color& y) {
        return std::fabs(x.r - y.r) < 0.01f && std::fabs(x.g - y.g) < 0.01f &&
               std::fabs(x.b - y.b) < 0.01f;
    };
    DOCTEST_CHECK_MESSAGE(tone(genBtnBg->color, wc.primary),
                          "生成草稿钮应为微信 Primary 绿");
    DOCTEST_CHECK_MESSAGE(tone(dismissBg->color, wc.surface),
                          "关闭钮应为微信 Secondary 白底");
    DOCTEST_CHECK(uiC.find("agent.dialog.task.panel") != nullptr);

    // ② ✕ 关子弹窗（外层保留）
    DOCTEST_REQUIRE(clickId(uiC, "agent.dialog.task.draft.close"));
    eui::Ui uiD;
    composeTo(uiD, mod, kScreen, "agent_ui_dfl3");
    DOCTEST_CHECK(uiD.find("agent.dialog.task.draft.panel") == nullptr);
    DOCTEST_CHECK(uiD.find("agent.dialog.task.panel") != nullptr);

    // ③ 重开 + Esc：先关子弹窗，再一次 Esc 才关外层
    DOCTEST_REQUIRE(clickId(uiD, "agent.dialog.task.aichip.bg"));
    eui::Ui uiE;
    composeTo(uiE, mod, kScreen, "agent_ui_dfl4");
    DOCTEST_REQUIRE(uiE.find("agent.dialog.task.draft.panel") != nullptr);
    eui::KeyEvent esc;
    esc.key = eui::InputKey::Escape;
    esc.action = eui::KeyAction::Press;
    DOCTEST_CHECK(mod.onKeyEvent(esc));
    eui::Ui uiF;
    composeTo(uiF, mod, kScreen, "agent_ui_dfl5");
    DOCTEST_CHECK(uiF.find("agent.dialog.task.draft.panel") == nullptr);
    DOCTEST_CHECK(uiF.find("agent.dialog.task.panel") != nullptr);
    DOCTEST_CHECK(mod.onKeyEvent(esc));
    eui::Ui uiG;
    composeTo(uiG, mod, kScreen, "agent_ui_dfl6");
    DOCTEST_CHECK(uiG.find("agent.dialog.task.panel") == nullptr);

    // ④ 遮罩点击关子弹窗（dialog 组件 backdrop onClick → onOpenChange(false)）
    // 本用例库内无任务 → 页头无「+ 新建任务」，走空态 CTA
    DOCTEST_REQUIRE(clickId(uiG, "agent.empty.btn.bg"));
    eui::Ui uiH;
    composeTo(uiH, mod, kScreen, "agent_ui_dfl7");
    // 裁定 3：aichip 只在 step 2 渲染（设计稿 08-新建任务wizard 只在「系统指令」
    // 步给 AI 起草按钮，点击无跨步导航语义），点前先导航到 step 2。
    DOCTEST_REQUIRE(clickId(uiH, "agent.dialog.task.step.2.bg"));
    eui::Ui uiH2;
    composeTo(uiH2, mod, kScreen, "agent_ui_dfl7s2");
    DOCTEST_REQUIRE(clickId(uiH2, "agent.dialog.task.aichip.bg"));
    eui::Ui uiI;
    composeTo(uiI, mod, kScreen, "agent_ui_dfl8");
    DOCTEST_REQUIRE_MESSAGE(
        clickId(uiI, "agent.dialog.task.draft.backdrop"),
        "子弹窗遮罩 agent.dialog.task.draft.backdrop 缺失");
    eui::Ui uiJ;
    composeTo(uiJ, mod, kScreen, "agent_ui_dfl9");
    DOCTEST_CHECK(uiJ.find("agent.dialog.task.draft.panel") == nullptr);
    DOCTEST_CHECK(uiJ.find("agent.dialog.task.panel") != nullptr);

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_draft_async_prefill_no_autosave,
              "agent_ui_draft: 异步生成回主线程预填（goal/cron/触发/记忆）/ 完成"
              "提示行 / 永不自动落库", {
    test_support::TestDb tdb("agentui", "draftprefill");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    const std::string draftJson =
        R"({"goal":"每天 8 点整理热榜并发通知","contextNote":"只处理 top3",)"
        R"("triggerType":"schedule","cron":"0 8 * * *","events":[],)"
        R"("onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":[]})";
    const std::string body = draftResponseBody(draftJson);
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [body](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = body;
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui uiA;
    composeTo(uiA, mod, kScreen, "agent_ui_dpf0");
    DOCTEST_REQUIRE(clickId(uiA, "agent.empty.btn.bg"));
    eui::Ui uiB;
    composeTo(uiB, mod, kScreen, "agent_ui_dpf1");
    // 裁定 3：aichip 只在 step 2 渲染，先导航（设计稿 08 无跨步导航语义）
    DOCTEST_REQUIRE(clickId(uiB, "agent.dialog.task.step.2.bg"));
    eui::Ui uiB2;
    composeTo(uiB2, mod, kScreen, "agent_ui_dpf1s2");
    DOCTEST_REQUIRE(clickId(uiB2, "agent.dialog.task.aichip.bg"));
    eui::Ui uiC;
    composeTo(uiC, mod, kScreen, "agent_ui_dpf2");
    auto* hit = uiC.find("agent.dialog.task.draft.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    typeInto(hit, "每天早上八点看一下热榜，有大新闻就通知我");
    DOCTEST_REQUIRE(clickId(uiC, "agent.dialog.task.draft.generate.bg"));

    // 轮询驱动主线程收割（worker 完成即置位，上限 ~2s）
    bool settled = false;
    for (int i = 0; i < 400 && !settled; ++i) {
        eui::Ui frame;
        composeTo(frame, mod, kScreen, "agent_ui_dpf3");
        settled = frame.find("agent.dialog.task.draft.notice") != nullptr ||
                  frame.find("agent.dialog.task.draft.error") != nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_REQUIRE_MESSAGE(settled, "生成在 2s 内未回到主线程（reap 未收割）");
    eui::Ui u;
    composeTo(u, mod, kScreen, "agent_ui_dpf4");

    // 预填：只填模型给出的可信字段，回 UI 线程后的表单可读回
    DOCTEST_CHECK_EQ(inputText(u, "agent.dialog.task.goal"),
                     std::string("每天 8 点整理热榜并发通知"));
    // W4 C3：执行记忆（note）字段已移除并入 systemPrompt，回填断言删除；
    // 载荷里的 contextNote 不再解析（task_draft.cpp 注释同口径）。
    // cron 只在 step 1 渲染——另起一帧导航到 step 1 读回表单状态
    eui::Ui uc;
    DOCTEST_REQUIRE(clickId(u, "agent.dialog.task.step.1.bg"));
    composeTo(uc, mod, kScreen, "agent_ui_dpf4s1");
    DOCTEST_CHECK_EQ(inputText(uc, "agent.dialog.task.cron"),
                     std::string("0 8 * * *"));
    const auto* cronAfterFill = uc.find("agent.dialog.task.cron");
    DOCTEST_REQUIRE(cronAfterFill != nullptr);
    // 完成提示行 + 无警告/错误行；子弹窗与外层都保持打开（生成不关窗）
    const auto* notice = u.find("agent.dialog.task.draft.notice");
    DOCTEST_REQUIRE(notice != nullptr);
    DOCTEST_CHECK(notice->text.find("已预填表单") != std::string::npos);
    DOCTEST_CHECK(notice->text.find("定时") != std::string::npos);
    // 微信化文字档锁：完成提示行 = textSecondary
    DOCTEST_CHECK(std::fabs(notice->textColor.r -
                            core::theme::current().colors.textSecondary.r) <
                  0.01f);
    DOCTEST_CHECK(u.find("agent.dialog.task.draft.warnings") == nullptr);
    DOCTEST_CHECK(u.find("agent.dialog.task.draft.error") == nullptr);
    DOCTEST_CHECK(u.find("agent.dialog.task.draft.panel") != nullptr);
    DOCTEST_CHECK(u.find("agent.dialog.task.panel") != nullptr);

    // 生成结果永不自动保存：库里一个任务都没有，保存仍是用户动作
    module::agent::AgentStore store(tdb.handle());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_draft_degrade_warning_no_trigger_takeover,
              "agent_ui_draft: 触发载荷校验失败降级为警告（cron 非法）——表单触发"
              "不被接管、goal 照常预填、警告行可见、不落库", {
    test_support::TestDb tdb("agentui", "draftwarn");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    const std::string draftJson =
        R"({"goal":"整理逾期待办","contextNote":"",)"
        R"("triggerType":"schedule","cron":"完全不是 cron","events":[],)"
        R"("onceLocal":"","maxIterations":10,"allowDestructive":false,)"
        R"("needsUserInput":[]})";
    const std::string body = draftResponseBody(draftJson);
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [body](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = body;
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui uiA;
    composeTo(uiA, mod, kScreen, "agent_ui_dwr0");
    DOCTEST_REQUIRE(clickId(uiA, "agent.empty.btn.bg"));
    eui::Ui uiB;
    composeTo(uiB, mod, kScreen, "agent_ui_dwr1");
    // W4 步骤条：触发方式 segment 只在 step 1 渲染
    DOCTEST_REQUIRE(clickId(uiB, "agent.dialog.task.step.1.bg"));
    eui::Ui uiB1;
    composeTo(uiB1, mod, kScreen, "agent_ui_dwr1s1");
    // 先手动选「定时」：模型载荷不可信时不得把它改掉 / 也不得回填坏 cron
    DOCTEST_REQUIRE(callDropdownChange(uiB1, "agent.dialog.task.trigger", 1));
    // 裁定 3：aichip 只在 step 2 渲染，先导航（设计稿 08 无跨步导航语义）
    DOCTEST_REQUIRE(clickId(uiB1, "agent.dialog.task.step.2.bg"));
    eui::Ui uiB2;
    composeTo(uiB2, mod, kScreen, "agent_ui_dwr1s2");
    DOCTEST_REQUIRE(clickId(uiB2, "agent.dialog.task.aichip.bg"));
    eui::Ui uiC;
    composeTo(uiC, mod, kScreen, "agent_ui_dwr2");
    auto* hit = uiC.find("agent.dialog.task.draft.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    typeInto(hit, "每天整理一次逾期待办");
    DOCTEST_REQUIRE(clickId(uiC, "agent.dialog.task.draft.generate.bg"));

    bool settled = false;
    for (int i = 0; i < 400 && !settled; ++i) {
        eui::Ui frame;
        composeTo(frame, mod, kScreen, "agent_ui_dwr3");
        settled = frame.find("agent.dialog.task.draft.notice") != nullptr ||
                  frame.find("agent.dialog.task.draft.error") != nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_REQUIRE_MESSAGE(settled, "生成在 2s 内未回到主线程（reap 未收割）");
    eui::Ui u;
    composeTo(u, mod, kScreen, "agent_ui_dwr4");

    // goal 预填成功；触发不被接管：仍是用户选的「定时」，坏 cron 一字不回填
    DOCTEST_CHECK_EQ(inputText(u, "agent.dialog.task.goal"),
                     std::string("整理逾期待办"));
    // cron 只在 step 1 渲染——另起一帧导航到 step 1 验证「坏 cron 一字不回填」
    eui::Ui uc;
    DOCTEST_REQUIRE(clickId(u, "agent.dialog.task.step.1.bg"));
    composeTo(uc, mod, kScreen, "agent_ui_dwr4s1");
    const auto* cronStillExists = uc.find("agent.dialog.task.cron");
    DOCTEST_REQUIRE(cronStillExists != nullptr);
    DOCTEST_CHECK(inputText(uc, "agent.dialog.task.cron").empty());
    // 降级警告可见（不 block 整体）；完成提示行也在
    const auto* warn = u.find("agent.dialog.task.draft.warnings");
    DOCTEST_REQUIRE(warn != nullptr);
    DOCTEST_CHECK(warn->text.find("cron") != std::string::npos);
    // 警告行保留 amber t.colors.warning（微信档无 amber 槽位，与 claude 详情警示行同款）
    DOCTEST_CHECK(std::fabs(warn->textColor.r -
                            core::theme::current().colors.warning.r) < 0.01f);
    const auto* notice = u.find("agent.dialog.task.draft.notice");
    DOCTEST_REQUIRE(notice != nullptr);
    DOCTEST_CHECK(notice->text.find("已预填表单") != std::string::npos);

    module::agent::AgentStore store(tdb.handle());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_draft_error_paths,
              "agent_ui_draft: 空输入不发包 / 上游 500 → 错误行呈现、表单不动、不落库", {
    test_support::TestDb tdb("agentui", "drafterr");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    int calls = 0;
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [&calls](const platform::http::Request&) {
        ++calls;
        platform::http::Response res;
        res.statusCode = 500;
        res.body = "boom";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui uiA;
    composeTo(uiA, mod, kScreen, "agent_ui_der0");
    DOCTEST_REQUIRE(clickId(uiA, "agent.empty.btn.bg"));
    eui::Ui uiB;
    composeTo(uiB, mod, kScreen, "agent_ui_der1");
    // 裁定 3：aichip 只在 step 2 渲染，先导航（设计稿 08 无跨步导航语义）
    DOCTEST_REQUIRE(clickId(uiB, "agent.dialog.task.step.2.bg"));
    eui::Ui uiB2;
    composeTo(uiB2, mod, kScreen, "agent_ui_der1s2");
    DOCTEST_REQUIRE(clickId(uiB2, "agent.dialog.task.aichip.bg"));
    eui::Ui uiC;
    composeTo(uiC, mod, kScreen, "agent_ui_der2");

    // ① 空输入：校验错误行，一个包都不发（不起线程）
    DOCTEST_REQUIRE(clickId(uiC, "agent.dialog.task.draft.generate.bg"));
    eui::Ui uiD;
    composeTo(uiD, mod, kScreen, "agent_ui_der3");
    const auto* err = uiD.find("agent.dialog.task.draft.error");
    DOCTEST_REQUIRE(err != nullptr);
    DOCTEST_CHECK(err->text.find("一句话") != std::string::npos);
    // 微信化错误行锁：错误文字 = error（#FA5151）
    DOCTEST_CHECK(std::fabs(err->textColor.r -
                            core::theme::current().colors.error.r) <
                  0.01f);
    DOCTEST_CHECK_EQ(calls, 0);

    // ② 上游 500：请求失败 → 错误行、无预填、不落库
    auto* hit = uiD.find("agent.dialog.task.draft.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    typeInto(hit, "每天整理逾期待办");
    DOCTEST_REQUIRE(clickId(uiD, "agent.dialog.task.draft.generate.bg"));
    bool settled = false;
    for (int i = 0; i < 400 && !settled; ++i) {
        eui::Ui frame;
        composeTo(frame, mod, kScreen, "agent_ui_der4");
        settled = frame.find("agent.dialog.task.draft.notice") != nullptr ||
                  frame.find("agent.dialog.task.draft.error") != nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DOCTEST_REQUIRE_MESSAGE(settled, "失败结果在 2s 内未回到主线程");
    eui::Ui u;
    composeTo(u, mod, kScreen, "agent_ui_der5");
    DOCTEST_CHECK(u.find("agent.dialog.task.draft.error") != nullptr);
    DOCTEST_CHECK(u.find("agent.dialog.task.draft.notice") == nullptr);
    DOCTEST_CHECK(inputText(u, "agent.dialog.task.goal").empty());
    DOCTEST_CHECK(calls >= 1);

    module::agent::AgentStore store(tdb.handle());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_draft_close_discards_inflight_result,
              "agent_ui_draft: 生成中关子弹窗 → 请求不杀、结果按 epoch 作废，"
              "表单一字不动、重开可见「已作废」提示 / 生成中按钮文案", {
    test_support::TestDb tdb("agentui", "draftdiscard");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");

    // 闸门 mock：压住响应直到测试放行（5s 自愈上限防任何路径挂死）
    std::atomic<bool> release{false};
    const std::string draftJson =
        R"({"goal":"不该被预填的目标","contextNote":"",)"
        R"("triggerType":"manual","cron":"","events":[],"onceLocal":"",)"
        R"("maxIterations":10,"allowDestructive":false,"needsUserInput":[]})";
    const std::string body = draftResponseBody(draftJson);
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [&release, body](const platform::http::Request&) {
        auto t0 = std::chrono::steady_clock::now();
        while (!release.load() &&
               std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        platform::http::Response res;
        res.statusCode = 200;
        res.body = body;
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui uiA;
    composeTo(uiA, mod, kScreen, "agent_ui_ddc0");
    DOCTEST_REQUIRE(clickId(uiA, "agent.empty.btn.bg"));
    eui::Ui uiB;
    composeTo(uiB, mod, kScreen, "agent_ui_ddc1");
    // 裁定 3：aichip 只在 step 2 渲染，先导航（设计稿 08 无跨步导航语义）；
    // 导航后 currentStep_ 保持 2，后续循环内与收尾重开点击无需再导航。
    DOCTEST_REQUIRE(clickId(uiB, "agent.dialog.task.step.2.bg"));
    eui::Ui uiB2;
    composeTo(uiB2, mod, kScreen, "agent_ui_ddc1s2");
    DOCTEST_REQUIRE(clickId(uiB2, "agent.dialog.task.aichip.bg"));
    eui::Ui uiC;
    composeTo(uiC, mod, kScreen, "agent_ui_ddc2");
    auto* hit = uiC.find("agent.dialog.task.draft.input.hit");
    DOCTEST_REQUIRE(hit != nullptr);
    typeInto(hit, "随便描述一句");
    DOCTEST_REQUIRE(clickId(uiC, "agent.dialog.task.draft.generate.bg"));

    // 生成在途（被闸压住，此处无竞态）：按钮呈生成中态
    eui::Ui uiGen;
    composeTo(uiGen, mod, kScreen, "agent_ui_ddc3");
    // 按钮文字恒为逐态实现 .text.normal（core::Color 默认 alpha=1）
    const auto* genText = uiGen.find("agent.dialog.task.draft.generate.text.normal");
    DOCTEST_REQUIRE(genText != nullptr);
    DOCTEST_CHECK_EQ(genText->text, std::string("生成中…"));

    // 在途时关掉子弹窗 → epoch 作废；放行闸门让请求自然跑完（请求不被杀）
    DOCTEST_REQUIRE(clickId(uiGen, "agent.dialog.task.draft.close"));
    release.store(true);

    // 持续 compose 驱动主线程收割；重开子弹窗验证「已作废」而不是预填
    bool discarded = false;
    for (int i = 0; i < 400 && !discarded; ++i) {
        eui::Ui frame;
        composeTo(frame, mod, kScreen, "agent_ui_ddc4");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (frame.find("agent.dialog.task.draft.panel") != nullptr) {
            continue;   // 理论不走（已关）；防御
        }
        DOCTEST_REQUIRE(clickId(frame, "agent.dialog.task.aichip.bg"));
        eui::Ui opened;
        composeTo(opened, mod, kScreen, "agent_ui_ddc5");
        const auto* err = opened.find("agent.dialog.task.draft.error");
        if (err != nullptr && err->text.find("作废") != std::string::npos) {
            discarded = true;
        } else if (err == nullptr) {
            // 还没收割完，关掉继续等下一轮
            clickId(opened, "agent.dialog.task.draft.close");
        }
    }
    DOCTEST_REQUIRE_MESSAGE(discarded, "作废提示在 2s 内未出现");

    // 最终断言面：重开一次取稳定帧——表单一字未动、无完成提示、不落库
    eui::Ui uPre;
    composeTo(uPre, mod, kScreen, "agent_ui_ddc6");
    DOCTEST_REQUIRE(clickId(uPre, "agent.dialog.task.aichip.bg"));
    eui::Ui u;
    composeTo(u, mod, kScreen, "agent_ui_ddc7");
    DOCTEST_CHECK(inputText(u, "agent.dialog.task.goal").empty());
    DOCTEST_CHECK(u.find("agent.dialog.task.draft.notice") == nullptr);
    module::agent::AgentStore store(tdb.handle());
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), static_cast<size_t>(0));

    mod.onStop();
    return true;
})

// E1 删除流：chip → 确认弹窗（文案含任务名与不可恢复）→ 取消零变化 → 再开确认 →
// 确认后列表与库都不含该任务，他任务零波及
REGISTER_TEST(agent_ui_delete_task_confirm_flow,
              "agent_ui: 删除 chip → 确认弹窗；取消不删，确认删净且不伤及他任务", {
    test_support::TestDb tdb("agentui", "deleteflow");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto t1 = store.createTask("建错的任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(t1.isOk());
    const std::string tid = std::to_string(t1.value());
    auto t2 = store.createTask("要保留的任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(t2.isOk());
    const std::string keepId = std::to_string(t2.value());
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_del1");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui, "agent.task." + tid + ".delete.bg"),
                            "删除 chip 未渲染/不可点");

    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_del2");
    const auto* msg = ui2.find("agent.dialog.delete.msg");
    DOCTEST_REQUIRE_MESSAGE(msg != nullptr, "点删除后确认弹窗未出现");
    DOCTEST_CHECK_MESSAGE(msg->text.find("建错的任务") != std::string::npos,
                          "确认文案不含任务名: " + msg->text);
    DOCTEST_CHECK(msg->text.find("不可恢复") != std::string::npos);

    // 取消路径：弹窗关、列表与库零变化
    DOCTEST_REQUIRE_MESSAGE(clickId(ui2, "agent.dialog.delete.cancel.bg"),
                            "取消按钮未渲染");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_del3");
    DOCTEST_CHECK(ui3.find("agent.dialog.delete.msg") == nullptr);
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.task." + tid + ".title") != nullptr,
                          "取消后任务不应被删");
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), 2u);

    // 确认路径：任务从列表与库消失，保留任务原样
    DOCTEST_REQUIRE(clickId(ui3, "agent.task." + tid + ".delete.bg"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_del4");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui4, "agent.dialog.delete.confirm.bg"),
                            "确认删除按钮未渲染");
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_del5");
    DOCTEST_CHECK_MESSAGE(ui5.find("agent.task." + tid + ".title") == nullptr,
                          "确认后任务仍在列表");
    DOCTEST_CHECK(ui5.find("agent.task." + keepId + ".title") != nullptr);
    tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK_EQ(tasks.value()[0].id, t2.value());

    mod.onStop();
    return true;
})

// E1 运行中禁删：DB 有 running run 时删除 chip 点击不开确认弹窗（守卫在 lambda 内，
// chip 本身仍挂 onClick）；run 终态后入口恢复
REGISTER_TEST(agent_ui_delete_task_disabled_while_running,
              "agent_ui: running 任务的删除点击不弹确认，终态后恢复可删", {
    test_support::TestDb tdb("agentui", "delerun");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("在跑任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    // 种子必须在 onStart 之后：onStart 的 sweepStaleRuns 会把遗留 running 行清成 failed，
    // 先种后启等于白种
    auto runId = store.startRun(created.value());   // 库内 running，模拟进行中
    DOCTEST_REQUIRE(runId.isOk());

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_delerun1");
    const auto* status = ui.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("运行中") != std::string::npos);
    DOCTEST_REQUIRE(clickId(ui, "agent.task." + tid + ".delete.bg"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_delerun2");
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.delete.msg") == nullptr,
                          "running 任务点删除不应弹出确认");

    DOCTEST_REQUIRE(store.finishRun(runId.value(), "completed", 1, 1, 1,
                                    std::nullopt).isOk());
    // 装配层等价路径：tasksDirty_/uiPolling_ 均假后，DB 直写终态靠 pollRunEnd 的
    // prevAlive 守卫驱动 refreshData（既有用例同款，2325 行起多处先例）
    mod.pollRunEnd();
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_delerun3");
    DOCTEST_REQUIRE(clickId(ui3, "agent.task." + tid + ".delete.bg"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_delerun4");
    DOCTEST_REQUIRE_MESSAGE(ui4.find("agent.dialog.delete.msg") != nullptr,
                            "run 终态后删除入口应恢复");
    // 收尾：确认删除，不留悬挂确认态
    DOCTEST_REQUIRE(clickId(ui4, "agent.dialog.delete.confirm.bg"));
    mod.onStop();
    return true;
})

// 列表状态视觉批：状态圆点（几何 + 色 token 双断言）、停用行标题置灰 + 已停用
// 徽章、运行中启动钮禁用（防双起）、行 hover 命中层。全部数值断言；配色断言带
// 非恒真自证（三色两两不同），防止 token 改成同值时配色检查空转。
REGISTER_TEST(agent_ui_task_list_status_visuals,
              "agent_ui: 列表状态视觉——failed 圆点 8px error 色 / 成功 success / 停用行标题 0.45 置灰加已停用徽章 / running 启动钮禁用文案切换 / UAT6 卡片化（灰面板+白卡三态+四边内边距+卡间距+行内钮统一 56×32）", {
    test_support::TestDb tdb("agentui", "listvis");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto okId = store.createTask("成功任务", "g", "manual", "default", 10, false);
    auto failId = store.createTask("失败任务", "g", "manual", "default", 10, false);
    auto idleId =
        store.createTask("没跑过任务", "g", "manual", "default", 10, false);
    auto offId = store.createTask("停用任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(okId.isOk());
    DOCTEST_REQUIRE(failId.isOk());
    DOCTEST_REQUIRE(idleId.isOk());
    DOCTEST_REQUIRE(offId.isOk());
    const std::string okTid = std::to_string(okId.value());
    const std::string failTid = std::to_string(failId.value());
    const std::string idleTid = std::to_string(idleId.value());
    const std::string offTid = std::to_string(offId.value());
    DOCTEST_REQUIRE(store.finishRun(store.startRun(okId.value()).value(),
                                    "completed", 1, 1, 1, std::nullopt)
                        .isOk());
    DOCTEST_REQUIRE(store.finishRun(store.startRun(failId.value()).value(),
                                    "failed", 1, 1, 1, std::nullopt)
                        .isOk());
    DOCTEST_REQUIRE(store.setTaskEnabled(offId.value(), false).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [](const platform::http::Request&) {
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"end_turn","content":[)"
                   R"({"type":"text","text":"done"}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 配色通道断言用（eui::Color 无 operator==）：逐分量比
    const auto sameColor = [](const eui::Color& a, const eui::Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };
    // 非恒真自证：error/success/textTertiary 两两不同，否则下面配色断言空转
    DOCTEST_CHECK(!sameColor(core::theme::current().colors.error,
                             core::theme::current().colors.success));
    DOCTEST_CHECK(!sameColor(core::theme::current().colors.error,
                             core::theme::current().colors.textTertiary));

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_listvis1");

    // 圆点：8x8 + 全圆角，色随终态映射；中文文字仍在（可访问性双通道）。
    // 断言宏是 fail-fast（return false），不能收进辅助 lambda——用表驱动循环摊开
    struct DotCase {
        std::string tid;
        eui::Color want;
        const char* statusText;
    };
    const DotCase dotCases[] = {
        {failTid, core::theme::current().colors.error, "失败"},
        {okTid, core::theme::current().colors.success, "成功"},
        {idleTid, core::theme::current().colors.textTertiary, "—"},
    };
    for (const auto& dc : dotCases) {
        const auto* dot = ui.find("agent.task." + dc.tid + ".status.dot");
        DOCTEST_CHECK_MESSAGE(dot != nullptr, "状态圆点未渲染: " + dc.tid);
        DOCTEST_CHECK(nearPx(dot->frame.width, 8.0f));
        DOCTEST_CHECK(nearPx(dot->frame.height, 8.0f));
        DOCTEST_CHECK(nearPx(dot->radius, 8.0f));
        DOCTEST_CHECK_MESSAGE(sameColor(dot->color, dc.want),
                              "圆点色不符: " + dc.tid);
        const auto* st = ui.find("agent.task." + dc.tid + ".status");
        DOCTEST_CHECK(st != nullptr);
        DOCTEST_CHECK(st->text.find(dc.statusText) != std::string::npos);
    }

    // 停用行：标题 alpha 0.45 + 「已停用」徽章；启用行两者皆无且标题不置灰
    const auto* offTitle = ui.find("agent.task." + offTid + ".title");
    DOCTEST_REQUIRE(offTitle != nullptr);
    DOCTEST_CHECK_MESSAGE(std::fabs(offTitle->textColor.a - 0.45f) < 0.02f,
                          "停用行标题未按 0.45 置灰");
    const auto* offBadge = ui.find("agent.task." + offTid + ".off");
    DOCTEST_REQUIRE(offBadge != nullptr);
    DOCTEST_CHECK_EQ(offBadge->text, std::string("已停用"));
    DOCTEST_CHECK(ui.find("agent.task." + idleTid + ".off") == nullptr);
    const auto* idleTitle = ui.find("agent.task." + idleTid + ".title");
    DOCTEST_REQUIRE(idleTitle != nullptr);
    DOCTEST_CHECK(idleTitle->textColor.a > 0.9f);

    // ---- S6.1 实底白卡几何/背景（玻璃退役）----
    // 列表容器收敛为 surface 白底面板
    const auto& themeNow = core::theme::current();
    const auto* panel = ui.find("agent.tasklist.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(sameColor(panel->color, ui_kit::cardSurface(themeNow)),
                          "列表容器应为 surface 白底面板");
    // 卡列表在面板内四边留边 10（kTaskCardInset；卡宽 = 列表宽 − 边距）
    const auto* tasklist = ui.find("agent.tasklist");
    DOCTEST_REQUIRE(tasklist != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(tasklist->frame.x - panel->frame.x, 10.0f),
                          "卡列表左留边应 10");
    DOCTEST_CHECK_MESSAGE(nearPx(tasklist->frame.y - panel->frame.y, 10.0f),
                          "卡列表上留边应 10");
    DOCTEST_CHECK_MESSAGE(
        nearPx(panel->frame.width - tasklist->frame.width, 20.0f),
        "卡列表应比面板窄 2×10（弹性契约：卡宽=列表宽−边距）");

    // 卡本体三态：normal surface 白卡 / hover 加深 hover / pressed
    // primarySoftBg（列表无选中语义，第三档只是按压反馈，无绿圈白勾）+ radius 圆角
    // + 细边 + 点击开详情
    const auto* rowBg = ui.find("agent.task." + idleTid + ".bg");
    DOCTEST_REQUIRE(rowBg != nullptr);
    DOCTEST_CHECK(rowBg->hasStateColors);
    DOCTEST_CHECK_MESSAGE(sameColor(rowBg->color, themeNow.colors.surface),
                          "卡底应为 surface 白卡");
    DOCTEST_CHECK_MESSAGE(
        sameColor(rowBg->hoverColor, themeNow.colors.hover),
        "卡 hover 应加深一档 hover（原 transparent→hover 行为已随卡片化换代）");
    DOCTEST_CHECK_MESSAGE(
        sameColor(rowBg->pressedColor, themeNow.colors.primarySoftBg),
        "卡按压档应为 primarySoftBg");
    DOCTEST_CHECK_MESSAGE(nearPx(rowBg->radius, themeNow.sizes.radius),
                          "卡圆角应为 radius (8px)");
    DOCTEST_CHECK_MESSAGE(nearPx(rowBg->border.width, 1.0f) &&
                              sameColor(rowBg->border.color,
                                        themeNow.colors.borderDefault),
                          "卡应为 1px borderDefault 细边");
    DOCTEST_CHECK(rowBg->onClick != nullptr);
    // 卡几何：卡高 = 行 44 + 上下内边距 12 = 68；.row 四边内缩 12
    const auto* card = ui.find("agent.task." + idleTid);
    const auto* cardRow = ui.find("agent.task." + idleTid + ".row");
    DOCTEST_REQUIRE(card != nullptr && cardRow != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(card->frame.height, 68.0f),
                          "卡高应 44+2×12=68，实测 " +
                              std::to_string(card->frame.height));
    DOCTEST_CHECK_MESSAGE(nearPx(cardRow->frame.x - card->frame.x, 12.0f),
                          "卡内容左内边距应 12");
    DOCTEST_CHECK_MESSAGE(nearPx(cardRow->frame.y - card->frame.y, 12.0f),
                          "卡内容上内边距应 12");
    DOCTEST_CHECK_MESSAGE(
        nearPx(card->frame.width - cardRow->frame.width, 24.0f),
        "卡内容左右内边距合计应 24");
    DOCTEST_CHECK_MESSAGE(nearPx(cardRow->frame.height, 44.0f), "卡内行高应 44");
    // 卡间距 8：相邻两卡 y 差 = 68 + 8 = 76（按 y 排序逐对验，不依赖 store 顺序）
    const std::string cardIds[] = {okTid, failTid, idleTid, offTid};
    std::vector<const core::dsl::Element*> cards;
    for (const auto& cid : cardIds) {
        const auto* c = ui.find("agent.task." + cid);
        DOCTEST_REQUIRE(c != nullptr);
        cards.push_back(c);
    }
    std::sort(cards.begin(), cards.end(),
              [](const core::dsl::Element* a, const core::dsl::Element* b) {
                  return a->frame.y < b->frame.y;
              });
    for (size_t i = 1; i < cards.size(); ++i) {
        DOCTEST_CHECK_MESSAGE(
            nearPx(cards[i]->frame.y - cards[i - 1]->frame.y, 76.0f),
            "相邻卡间距应 68+8=76，实测 " +
                std::to_string(cards[i]->frame.y - cards[i - 1]->frame.y));
    }
    // 行内操作钮统一规格（真机反馈「启动比其他按钮大」的回归钉）：启动与
    // 详情 chip 同宽 56、同高 32、字号 12——主操作只用 Primary 色表达
    const auto* runBgS = ui.find("agent.task." + idleTid + ".run.bg");
    // 按钮文字恒为逐态实现 .text.normal（core::Color 默认 alpha=1）
    const auto* runTextS = ui.find("agent.task." + idleTid + ".run.text.normal");
    const auto* detailChipS = ui.find("agent.task." + idleTid + ".detail.chip");
    DOCTEST_REQUIRE(runBgS != nullptr && runTextS != nullptr &&
                    detailChipS != nullptr);
    DOCTEST_CHECK_MESSAGE(nearPx(runBgS->frame.width, 56.0f) &&
                              nearPx(runBgS->frame.height, 32.0f),
                          "启动钮应统一 56×32");
    DOCTEST_CHECK_MESSAGE(
        nearPx(runBgS->frame.width, detailChipS->frame.width),
        "启动钮与行内 chip 不同宽——同排操作钮须同尺寸（主操作不许大一号）");
    DOCTEST_CHECK_MESSAGE(nearPx(runTextS->fontSize, 12.0f),
                          "启动钮字号应 12（原派生 14.7 是「大一号」根因）");

    // 运行中：启动钮禁用 + 文案「运行中…」；同帧对照（没在跑的任务可用）证明
    // disabled 位来自运行判定而非 loop 未装配。终态后恢复「启动」可点。
    auto runId = store.startRun(idleId.value());
    DOCTEST_REQUIRE(runId.isOk());
    mod.pollRunEnd();   // DB 直写 running 行进 latestRuns_（装配层等价路径）
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_listvis2");
    const auto* runBg = ui2.find("agent.task." + idleTid + ".run.bg");
    DOCTEST_REQUIRE(runBg != nullptr);
    DOCTEST_CHECK_MESSAGE(runBg->disabled, "运行中任务的启动钮未禁用");
    const auto* runText = ui2.find("agent.task." + idleTid + ".run.text.normal");
    DOCTEST_REQUIRE(runText != nullptr);
    DOCTEST_CHECK(runText->text.find("运行中") != std::string::npos);
    const auto* okRunBg = ui2.find("agent.task." + okTid + ".run.bg");
    DOCTEST_REQUIRE(okRunBg != nullptr);
    DOCTEST_CHECK_FALSE(okRunBg->disabled);
    DOCTEST_REQUIRE(store.finishRun(runId.value(), "completed", 1, 1, 1,
                                    std::nullopt)
                        .isOk());
    mod.pollRunEnd();
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_listvis3");
    const auto* runBg2 = ui3.find("agent.task." + idleTid + ".run.bg");
    DOCTEST_REQUIRE(runBg2 != nullptr);
    DOCTEST_CHECK_FALSE(runBg2->disabled);
    const auto* runText2 = ui3.find("agent.task." + idleTid + ".run.text.normal");
    DOCTEST_REQUIRE(runText2 != nullptr);
    DOCTEST_CHECK_EQ(runText2->text, std::string("启动"));

    mod.onStop();
    return true;
})

// 表单语义提示批：六类触发各一行「怎么填」提示（定时/闹钟的预览行优先占槽，
// 预览为空才轮到提示）；meta 行一行讲迭代上限后果（受限终态）与破坏性开关
// 后果（高危工具）。文案断言 = id 契约存在性 + 关键词语义点。
REGISTER_TEST(agent_ui_task_dialog_semantic_hints,
              "agent_ui: 表单语义提示——手动提示非空 / 每天提示含「应用」/ 定时空 cron 提示含 cron、填了预览接管 / meta 行提示含受限与高危", {
    test_support::TestDb tdb("agentui", "hints");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    // 默认「手动」：提示行存在且讲清「点启动才跑」。
    // openNewTaskDialog 是先 compose 后点击——弹窗节点要再 compose 一帧才可见
    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskDialog(ui, mod, "agent_ui_hints1"));
    eui::Ui ui1b;
    composeTo(ui1b, mod, kScreen, "agent_ui_hints1b");
    // W4 步骤条：触发提示只在 step 1 渲染，meta 提示只在 step 3 渲染
    DOCTEST_REQUIRE(clickId(ui1b, "agent.dialog.task.step.1.bg"));
    eui::Ui ui1s1;
    composeTo(ui1s1, mod, kScreen, "agent_ui_hints1s1");
    const auto* manual = ui1s1.find("agent.dialog.task.trigger.hint");
    DOCTEST_REQUIRE(manual != nullptr);
    DOCTEST_CHECK(manual->text.find("启动") != std::string::npos);
    // meta 行提示：迭代后果（受限终态）+ 破坏性开关后果（高危工具），两个
    // 关键词缺一不可
    DOCTEST_REQUIRE(clickId(ui1s1, "agent.dialog.task.step.3.bg"));
    eui::Ui ui1s3;
    composeTo(ui1s3, mod, kScreen, "agent_ui_hints1s3");
    const auto* meta = ui1s3.find("agent.dialog.task.meta.hint");
    DOCTEST_REQUIRE(meta != nullptr);
    DOCTEST_CHECK(meta->text.find("受限") != std::string::npos);
    DOCTEST_CHECK(meta->text.find("高危") != std::string::npos);
    DOCTEST_CHECK(meta->text.size() > 20);   // 非恒真自证：两关键词确有正文承载

    // 「每天」：隐藏语义必须点名「应用」——防按「固定时刻闹钟」直觉误读
    eui::Ui ui2;
    DOCTEST_REQUIRE(openNewTaskDialog(ui2, mod, "agent_ui_hints2"));
    eui::Ui ui2x;
    composeTo(ui2x, mod, kScreen, "agent_ui_hints2x");
    DOCTEST_REQUIRE(clickId(ui2x, "agent.dialog.task.step.1.bg"));
    eui::Ui ui2s1;
    composeTo(ui2s1, mod, kScreen, "agent_ui_hints2s1");
    DOCTEST_REQUIRE(callDropdownChange(ui2s1, "agent.dialog.task.trigger", 4));
    eui::Ui ui2b;
    composeTo(ui2b, mod, kScreen, "agent_ui_hints2b");
    const auto* daily = ui2b.find("agent.dialog.task.trigger.hint");
    DOCTEST_REQUIRE(daily != nullptr);
    DOCTEST_CHECK(daily->text.find("应用") != std::string::npos);

    // 「定时」cron 空：预览未占用槽 → 提示讲 cron 怎么填
    eui::Ui ui3;
    DOCTEST_REQUIRE(openNewTaskDialog(ui3, mod, "agent_ui_hints3"));
    eui::Ui ui3x;
    composeTo(ui3x, mod, kScreen, "agent_ui_hints3x");
    DOCTEST_REQUIRE(clickId(ui3x, "agent.dialog.task.step.1.bg"));
    eui::Ui ui3s1;
    composeTo(ui3s1, mod, kScreen, "agent_ui_hints3s1");
    DOCTEST_REQUIRE(callDropdownChange(ui3s1, "agent.dialog.task.trigger", 1));
    eui::Ui ui3b;
    composeTo(ui3b, mod, kScreen, "agent_ui_hints3b");
    const auto* sched = ui3b.find("agent.dialog.task.trigger.hint");
    DOCTEST_REQUIRE(sched != nullptr);
    DOCTEST_CHECK(sched->text.find("cron") != std::string::npos);

    // 「定时」填了合法 cron：预览行接管槽位，提示行退场（同槽互斥不叠加）
    mod.setEditCronDraft("0 9 * * 1-5");
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_hints4");
    DOCTEST_CHECK(ui4.find("agent.dialog.task.trigger.hint") == nullptr);
    DOCTEST_CHECK(ui4.find("agent.dialog.task.cron.preview") != nullptr);

    // 「常驻」「事件」：提示非空且点出关键动作 / 关键词
    eui::Ui ui5;
    DOCTEST_REQUIRE(openNewTaskDialog(ui5, mod, "agent_ui_hints5"));
    eui::Ui ui5x;
    composeTo(ui5x, mod, kScreen, "agent_ui_hints5x");
    DOCTEST_REQUIRE(clickId(ui5x, "agent.dialog.task.step.1.bg"));
    eui::Ui ui5s1;
    composeTo(ui5s1, mod, kScreen, "agent_ui_hints5s1");
    DOCTEST_REQUIRE(callDropdownChange(ui5s1, "agent.dialog.task.trigger", 2));
    eui::Ui ui5b;
    composeTo(ui5b, mod, kScreen, "agent_ui_hints5b");
    const auto* resident = ui5b.find("agent.dialog.task.trigger.hint");
    DOCTEST_REQUIRE(resident != nullptr);
    DOCTEST_CHECK(resident->text.find("入队") != std::string::npos);
    eui::Ui ui6;
    DOCTEST_REQUIRE(openNewTaskDialog(ui6, mod, "agent_ui_hints6"));
    eui::Ui ui6x;
    composeTo(ui6x, mod, kScreen, "agent_ui_hints6x");
    DOCTEST_REQUIRE(clickId(ui6x, "agent.dialog.task.step.1.bg"));
    eui::Ui ui6s1;
    composeTo(ui6s1, mod, kScreen, "agent_ui_hints6s1");
    DOCTEST_REQUIRE(callDropdownChange(ui6s1, "agent.dialog.task.trigger", 3));
    eui::Ui ui6b;
    composeTo(ui6b, mod, kScreen, "agent_ui_hints6b");
    const auto* event = ui6b.find("agent.dialog.task.trigger.hint");
    DOCTEST_REQUIRE(event != nullptr);
    DOCTEST_CHECK(!event->text.empty());

    // 「闹钟」：默认缺省时刻让预览行非空 → 提示让位（不与预览叠加）
    eui::Ui ui7;
    DOCTEST_REQUIRE(openNewTaskDialog(ui7, mod, "agent_ui_hints7"));
    eui::Ui ui7x;
    composeTo(ui7x, mod, kScreen, "agent_ui_hints7x");
    DOCTEST_REQUIRE(clickId(ui7x, "agent.dialog.task.step.1.bg"));
    eui::Ui ui7s1;
    composeTo(ui7s1, mod, kScreen, "agent_ui_hints7s1");
    DOCTEST_REQUIRE(callDropdownChange(ui7s1, "agent.dialog.task.trigger", 5));
    eui::Ui ui7b;
    composeTo(ui7b, mod, kScreen, "agent_ui_hints7b");
    DOCTEST_CHECK(ui7b.find("agent.dialog.task.trigger.hint") == nullptr);
    DOCTEST_CHECK(ui7b.find("agent.dialog.task.once.preview") != nullptr);

    mod.onStop();
    return true;
})

// MinGW std::thread 构造函数间歇抛 std::system_error（resource_unavailable_try_again）：
// Win32 CreateThread 在系统线程资源紧张时返回 NULL，std::thread 包装此失败为异常。
// AgentModule::startTask-476 的 catch(std::system_error&) 块摘除 ActiveRun，
// hasActiveRun→false→停止钮永不渲染。macOS pthread_create 几乎永不失败（LPD）。
// 属工具链线程资源差异，非代码同步缺陷——阻塞 HTTP 桩保证 run 线程不可能早退。
static bool agentThreadCreationReliableUnderMinGW() {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

// C1 停止流：运行中渲染停止钮 → 点击置 cancel 位（isStopRequested 可断言）→
// 钮切「停止中」禁用 → 放行后 run 在迭代边界收尾 cancelled → 列表「已取消」
REGISTER_TEST(agent_ui_stop_button_cancel_flow,
              "agent_ui: 运行中停止钮可见可点，点击后 cancel 位与反馈态可断言，终态 cancelled", {
    if (!agentThreadCreationReliableUnderMinGW()) {
        DOCTEST_SKIP("MinGW std::thread 间歇创建失败（std::system_error）→ ActiveRun 被摘除→hasActiveRun=false→停止钮永不渲染；"
                     "属工具链线程资源差异，阻塞 HTTP 桩排除「早退」假说");
    }
    test_support::TestDb tdb("agentui", "stopflow");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("慢任务", "直接回复完成，不调用任何工具",
                                    "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());

    // 阻塞桩：首个 LLM 请求卡住直到放行（模拟 in-flight 迭代），上限 5s 防挂死。
    // 响应必须是 tool_use（app.stats）而不是 end_turn——cancel 只在迭代边界（下轮 send
    // 前）被查，end_turn 单轮 run 会在 send 返回后直接 completed break，永远吃不到 cancel
    std::atomic<bool> releaseGate{false};
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    deps.http = [&releaseGate](const platform::http::Request&) {
        for (int i = 0; i < 500 && !releaseGate.load(std::memory_order_acquire);
             ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        platform::http::Response res;
        res.statusCode = 200;
        res.body = R"({"id":"m1","stop_reason":"tool_use","content":[)"
                   R"({"type":"tool_use","id":"t1","name":"app.stats","input":{}}],)"
                   R"("usage":{"input_tokens":1,"output_tokens":1}})";
        return core::Result<platform::http::Response>::ok(res);
    };
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_stop1");
    DOCTEST_REQUIRE(clickId(ui, "agent.task." + tid + ".run.bg"));

    // 运行中（在跑 ActiveRun 已同步注册）：详情弹窗标题行应出现停止钮
    bool stopVisible = false;
    eui::Ui ui2;
    for (int i = 0; i < 100 && !stopVisible; ++i) {
        composeTo(ui2, mod, kScreen, "agent_ui_stop_poll");
        stopVisible = ui2.find("agent.dialog.detail.stop.bg") != nullptr;
        if (!stopVisible) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(stopVisible, "运行中详情弹窗未渲染停止钮");

    // 点击停止：cancel 位经模块观测面可断言；钮切「停止中」并禁用
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.stop.bg"));
    DOCTEST_CHECK_MESSAGE(mod.isStopRequested(created.value()),
                          "点击停止后 cancel 位未置");
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_stop_after");
    const auto* stopBtn = ui3.find("agent.dialog.detail.stop");
    DOCTEST_REQUIRE(stopBtn != nullptr);
    DOCTEST_CHECK_MESSAGE(stopBtn->text.find("停止中") != std::string::npos,
                          "点击后未切「停止中」反馈态，实测: " + stopBtn->text);
    DOCTEST_CHECK_MESSAGE(stopBtn->text.find("当前轮结束后生效") != std::string::npos,
                          "停止钮文案缺生效时机说明");

    // 放行 → 当前轮跑完 → 迭代边界取消优先于超限判定 → cancelled 落库
    releaseGate.store(true, std::memory_order_release);
    bool cancelled = false;
    eui::Ui ui4;
    for (int i = 0; i < 500 && !cancelled; ++i) {
        composeTo(ui4, mod, kScreen, "agent_ui_stop_term");
        auto runs = store.listRuns(created.value(), 1);
        if (runs.isOk() && !runs.value().empty() &&
            runs.value().front().status == "cancelled") {
            cancelled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DOCTEST_REQUIRE_MESSAGE(cancelled, "放行后 5s 内 run 未落库 cancelled");

    // 列表状态列「已取消」
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_ui_stop_list");
    const auto* status = ui5.find("agent.task." + tid + ".status");
    DOCTEST_REQUIRE(status != nullptr);
    DOCTEST_CHECK(status->text.find("已取消") != std::string::npos);

    mod.onStop();
    return true;
})

// C1 非运行态：详情弹窗无停止钮（无 ActiveRun 可置位，渲染入口等于骗点击）；
// 无在跑 run 时 requestStopRun 落空不崩、不产生新 run
REGISTER_TEST(agent_ui_stop_button_absent_when_idle,
              "agent_ui: 非运行态详情无停止钮；无 run 时 requestStopRun 落空不崩", {
    test_support::TestDb tdb("agentui", "stopidle");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("空闲任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    const std::string tid = std::to_string(created.value());
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_stopidle1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, tid));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_stopidle2");
    DOCTEST_CHECK_MESSAGE(ui2.find("agent.dialog.detail.stop.bg") == nullptr,
                          "非运行态不应渲染停止钮");

    mod.requestStopRun(created.value());   // 无 run：LOG 落空，不崩
    auto runs = store.listRuns(created.value(), 10);
    DOCTEST_REQUIRE(runs.isOk());
    DOCTEST_CHECK(runs.value().empty());   // 未产生新 run

    mod.onStop();
    return true;
})

// ---- P2 时间线 / run 详情 / 摘要真数据（spec §12.3；几何断言沿用三尺寸壳）----

// 本地「相对 now 的第 dayDelta 天的 HH:MM:SS」epoch（DST 日不按 86400 硬减，
// 与 agent_time_text 的边界口径一致）
namespace {
std::time_t uiDayAt(std::time_t now, int dayDelta, int hour, int min, int sec) {
    std::tm t{};
    core::time::localTime(now, &t);
    t.tm_mday += dayDelta;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return std::mktime(&t);
}
} // namespace

REGISTER_TEST(agent_ui_detail_timeline_day_groups,
              "agent_ui: 时间线按天分组（今天/昨天/更早）+ 20 条上限 .runlist.note", {
    test_support::TestDb tdb("agentui", "daygroups");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto a = store.createTask("分组任务", "g", "manual", "default", 10, false);
    auto b = store.createTask("封顶任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(a.isOk() && b.isOk());
    const std::time_t now = core::time::nowUnix();
    auto pin = [&](int64_t runId, std::time_t started) -> bool {
        return tdb.handle()
            .exec("UPDATE agent_runs SET started_at=" +
                  std::to_string(static_cast<int64_t>(started)) +
                  ", ended_at=" +
                  std::to_string(static_cast<int64_t>(started) + 30) +
                  " WHERE id=" + std::to_string(runId))
            .isOk();
    };
    // A：今天 / 昨天 / 8 天前各一条 → 3 个分组
    for (std::time_t at : {uiDayAt(now, 0, 9, 0, 0), uiDayAt(now, -1, 9, 0, 0),
                           uiDayAt(now, -8, 9, 0, 0)}) {
        auto r = store.startRun(a.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                        std::nullopt, std::nullopt)
                            .isOk());
        DOCTEST_REQUIRE(pin(r.value(), at));
    }
    // B：20 条全今天 → 触顶 note
    for (int i = 0; i < 20; ++i) {
        auto r = store.startRun(b.value());
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 1, std::nullopt,
                                        std::nullopt, std::nullopt)
                            .isOk());
        DOCTEST_REQUIRE(pin(r.value(), now - i));
    }

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_dg1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(a.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_dg2");
    const auto* day0 = ui2.find("agent.dialog.detail.runlist.day.0");
    const auto* day1 = ui2.find("agent.dialog.detail.runlist.day.1");
    const auto* day2 = ui2.find("agent.dialog.detail.runlist.day.2");
    DOCTEST_REQUIRE_MESSAGE(day0 != nullptr && day1 != nullptr && day2 != nullptr,
                            "3 个日期分组头缺失");
    DOCTEST_CHECK(day0->text == "今天");
    DOCTEST_CHECK(day1->text == "昨天");
    // 更早的分组 = YYYY-MM-DD（10 字符、含两个 '-'；不断言具体日期防跨零点/时区红）
    DOCTEST_CHECK_EQ(day2->text.size(), static_cast<size_t>(10));
    DOCTEST_CHECK(day2->text != "今天" && day2->text != "昨天");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist.day.3") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist.note") == nullptr);

    // B：20 条 → .runlist.19 在场、.runlist.20 无、note 显形
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.close"));
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_ui_dg3");
    DOCTEST_REQUIRE(openDetailDialog(ui3, mod, std::to_string(b.value())));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_ui_dg4");
    DOCTEST_CHECK(ui4.find("agent.dialog.detail.runlist.19") != nullptr);
    DOCTEST_CHECK(ui4.find("agent.dialog.detail.runlist.20") == nullptr);
    const auto* note = ui4.find("agent.dialog.detail.runlist.note");
    DOCTEST_REQUIRE_MESSAGE(note != nullptr, "20 条上限缺 .runlist.note");
    DOCTEST_CHECK(note->text == "仅显示最近 20 次");

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_timeline_select_narrow,
              "agent_ui: 窄屏点 .runlist.<i>.bg 选中旧 run 并自动切「运行详情」tab", {
    test_support::TestDb tdb("agentui", "tlsel");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("选择任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    auto r1 = store.startRun(created.value());
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_REQUIRE(store.finishRun(r1.value(), "failed", 3, std::nullopt,
                                    std::nullopt, std::string("旧 run 错误"))
                        .isOk());
    auto r2 = store.startRun(created.value());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_REQUIRE(store.finishRun(r2.value(), "completed", 1, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    const eui::Screen screen{700.0f, 560.0f};   // 窄屏：tab 0 默认
    eui::Ui ui;
    composeTo(ui, mod, screen, "agent_ui_tls1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, screen, "agent_ui_tls2");
    DOCTEST_REQUIRE_MESSAGE(ui2.find("agent.dialog.detail.runlist.title") != nullptr,
                            "窄屏默认 tab 0 应显示时间线");
    // 点旧 run（runlist.1 = failed）→ 选中 + 自动切 tab 1
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.runlist.1.bg"));
    eui::Ui ui3;
    composeTo(ui3, mod, screen, "agent_ui_tls3");
    DOCTEST_CHECK_MESSAGE(ui3.find("agent.dialog.detail.runlist.title") == nullptr,
                          "点行后应自动切「运行详情」（tab 0 内容退场）");
    const auto* statusText = ui3.find("agent.dialog.detail.detail.status.text");
    DOCTEST_REQUIRE_MESSAGE(statusText != nullptr, "tab 1 缺 .detail.status.text");
    DOCTEST_CHECK_EQ(statusText->text, std::string("失败"));
    const auto* runIdText = ui3.find("agent.dialog.detail.detail.id");
    DOCTEST_REQUIRE(runIdText != nullptr);
    DOCTEST_CHECK_EQ(runIdText->text,
                     "#" + std::to_string(r1.value()));
    const auto* verdict = ui3.find("agent.dialog.detail.detail.schedule.verdict");
    DOCTEST_REQUIRE(verdict != nullptr);
    DOCTEST_CHECK_EQ(verdict->text, std::string("无计划对照"));   // manual 无计划

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_schedule_vs_actual,
              "agent_ui: 定时任务 run 详情「计划 vs 实际」= 按时（前一日 09:00 计划 +30s）", {
    test_support::TestDb tdb("agentui", "schedact");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("定时对照", "g", "schedule", "default", 10,
                                    false, R"({"cron":"0 9 * * *"})");
    DOCTEST_REQUIRE(created.isOk());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_REQUIRE(store.finishRun(r.value(), "completed", 2, std::nullopt,
                                    std::nullopt, std::nullopt).isOk());
    // 钉 started_at = 昨天 09:00:30 本地（含当前分钟口径 → 计划 09:00，差 30s = 按时）
    const std::time_t plan =
        uiDayAt(core::time::nowUnix(), -1, 9, 0, 0) + 30;
    DOCTEST_REQUIRE(tdb.handle()
                        .exec("UPDATE agent_runs SET started_at=" +
                              std::to_string(static_cast<int64_t>(plan)) +
                              ", ended_at=" +
                              std::to_string(static_cast<int64_t>(plan) + 60) +
                              " WHERE id=" + std::to_string(r.value()))
                        .isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_sa1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_sa2");
    // 「计划 vs 实际」在运行详情 tab（三档之一）：切档后断言
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, kScreen, "agent_ui_sa2n");
    const auto* planText = ui2n.find("agent.dialog.detail.detail.schedule.plan");
    const auto* actualText = ui2n.find("agent.dialog.detail.detail.schedule.actual");
    const auto* verdict = ui2n.find("agent.dialog.detail.detail.schedule.verdict");
    DOCTEST_REQUIRE_MESSAGE(planText != nullptr && actualText != nullptr &&
                                verdict != nullptr,
                            "右栏缺 .detail.schedule 三节点");
    DOCTEST_CHECK(planText->text.find("计划") == 0);
    DOCTEST_CHECK(planText->text.find("09:00") != std::string::npos);
    DOCTEST_CHECK(actualText->text.find("实际") == 0);
    DOCTEST_CHECK_EQ(verdict->text, std::string("按时"));

    mod.onStop();
    return true;
})

REGISTER_TEST(agent_ui_detail_error_scroll,
              "agent_ui: 运行详情 tab 长错误全文进 .detail.error.scroll（wrap 不截断）；弹性档余高足 → 固定区不降级", {
    test_support::TestDb tdb("agentui", "errscroll");
    test_support::FakeProfileSource profile;
    ToolGuard statsGuard{"app.stats", "app.notify"};
    core::ToolRegistry::instance().unregisterTool("app.stats");
    core::ToolRegistry::instance().unregisterTool("app.notify");
    module::agent::AgentStore store(tdb.handle());
    auto created = store.createTask("长错误任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(created.isOk());
    auto r = store.startRun(created.value());
    DOCTEST_REQUIRE(r.isOk());
    const std::string longErr(2000, 'E');
    DOCTEST_REQUIRE(store.finishRun(r.value(), "failed", 1, std::nullopt,
                                    std::nullopt, longErr)
                        .isOk());

    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();

    eui::Ui ui;
    composeTo(ui, mod, kScreen, "agent_ui_es1");
    DOCTEST_REQUIRE(openDetailDialog(ui, mod, std::to_string(created.value())));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_ui_es2");
    // 长错误块在「运行详情」tab：切档后断言
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, mod, kScreen, "agent_ui_es2n");
    const auto* errText = ui2n.find("agent.dialog.detail.detail.error.text");
    const auto* errScroll = ui2n.find("agent.dialog.detail.detail.error.scroll");
    DOCTEST_REQUIRE_MESSAGE(errText != nullptr && errScroll != nullptr,
                            "长错误缺 .detail.error.scroll/.text");
    // 全文不截断（2000 字节原文逐字在场）
    DOCTEST_CHECK_EQ(errText->text.size(), longErr.size());
    const auto* panel = ui2n.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_MESSAGE(frameContained(*panel, *errScroll),
                          "错误滚动区越出面板：scroll(x=" +
                              std::to_string(errScroll->frame.x) + ",y=" +
                              std::to_string(errScroll->frame.y) + ",w=" +
                              std::to_string(errScroll->frame.width) + ",h=" +
                              std::to_string(errScroll->frame.height) +
                              ") panel(x=" + std::to_string(panel->frame.x) +
                              ",y=" + std::to_string(panel->frame.y) +
                              ",w=" + std::to_string(panel->frame.width) +
                              ",h=" + std::to_string(panel->frame.height) + ")");
    // UAT6 弹窗高收口 660→552：弹性档内容高 309（552−183−60），长错误块吃满
    // 96 上限后 detailTopH=250，309−250−32−12=15 < 96 → 余高不足整栏滚降级。
    DOCTEST_CHECK_MESSAGE(ui2n.find("agent.dialog.detail.detail.scroll") != nullptr,
                          "弹性档 552 高 + 长错误应降级为 .detail.scroll 整栏滚");
    // 消息区默认折叠（本任务无消息也无妨——折叠态本就不建 .msgscroll）
    DOCTEST_CHECK(ui2n.find("agent.dialog.detail.msgscroll") == nullptr);

    mod.onStop();
    return true;
})


// F1 弹窗防回潮：P7 任务向导 + 删除确认逐弹窗「全元素在框内」+ 标题锚
// （y=16 / fontSize=20，规格 S2.4）。回退即红：任一可见子孙 frame 越出 panel
// （>0.5px）、标题 y 偏移≠16、标题字号≠20。
REGISTER_TEST(agent_ui_dialog_frame_containment,
              "agent_ui: P7 任务向导/删除确认弹窗全元素在框内 + 标题 y=16/字号 20（防回潮）", {
    test_support::TestDb tdb("agentui", "dialogframe");
    test_support::FakeProfileSource profile;
    module::agent::AgentModule::Deps deps;
    deps.db = &tdb.handle();
    deps.profileSource = &profile;
    module::agent::AgentModule mod(deps);
    mod.onStart();
    module::agent::AgentStore store(tdb.handle());
    auto t1 = store.createTask("框内任务", "g", "manual", "default", 10, false);
    DOCTEST_REQUIRE(t1.isOk());
    const std::string tid = std::to_string(t1.value());

    auto check = [&](eui::Ui& ui, const char* panelId, const char* titleId,
                     const char* tag) -> bool {
        std::string v = dialog_frame::allInside(ui, panelId);
        DOCTEST_CHECK_MESSAGE(v.empty(), std::string(tag) + " 全元素在框内: " + v);
        v = dialog_frame::titleAnchor(ui, panelId, titleId);
        DOCTEST_CHECK_MESSAGE(v.empty(), std::string(tag) + " 标题锚: " + v);
        return true;
    };

    // P7 任务向导（openNewTaskWithGoal 停在 step 2 指令页，goal 已预填）
    eui::Ui ui;
    DOCTEST_REQUIRE(openNewTaskWithGoal(ui, mod, "agent_df", "框内向导"));
    eui::Ui ui2;
    composeTo(ui2, mod, kScreen, "agent_df_chk");
    DOCTEST_CHECK(check(ui2, "agent.dialog.task.panel", "agent.dialog.task.title", "P7 向导"));

    // 关向导 → 任务行删除 chip → P10 删除确认
    eui::Ui ui3;
    composeTo(ui3, mod, kScreen, "agent_df_close");
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.task.backdrop"));
    eui::Ui ui4;
    composeTo(ui4, mod, kScreen, "agent_df_del");
    DOCTEST_REQUIRE_MESSAGE(clickId(ui4, "agent.task." + tid + ".delete.bg"),
                            "删除 chip 不可点");
    eui::Ui ui5;
    composeTo(ui5, mod, kScreen, "agent_df_del2");
    DOCTEST_CHECK(check(ui5, "agent.dialog.delete.panel", "agent.dialog.delete.title",
          "P10 删除确认"));

    mod.onStop();
    return true;
})
