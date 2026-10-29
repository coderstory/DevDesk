// tests/test_modules/test_schedule_ui_list.cpp
// 定时列表页 + 模块装配（波 S7a）。每条用例对应一个业务场景并注明防的具体事故。
// 覆盖：列表装配排序 / 开关切换落库 / 归档折叠过滤 / 系统段空注册表不出错 /
// 模块注册表项完整（含 onStart/onStop 幂等与无头 compose 不崩）。
#include "doctest.h"
#include "test_support.h"

#include "app_registry.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "module_schedule/schedule_module.h"
#include "module_schedule/schedule_provider.h"
#include "module_schedule/schedule_store.h"
#include "module_schedule/schedule_ui_history.h"
#include "module_schedule/schedule_ui_list.h"
#include "settings_config.h"

#include <eui/dsl.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using module::schedule::ListAssembly;
using module::schedule::RuleRecord;
using module::schedule::ScheduleItemView;
using module::schedule::ScheduleListUi;
using module::schedule::ScheduleProviderRegistry;
using module::schedule::ScheduleStore;

constexpr const char* kDbPrefix = "devdesk_test_schedlist_";

eui::Screen makeScreen() {
    eui::Screen s;
    s.width = 1200.0f;
    s.height = 800.0f;
    return s;
}

// 一帧 compose + layout（frame 断言必须真 layout）
void composeFrame(ScheduleListUi& ui, eui::Ui& euiUi, const eui::Screen& screen,
                  const char* frame) {
    euiUi.begin(frame);
    ui.compose(euiUi, screen);
    euiUi.end();
    euiUi.layout(screen.width, screen.height);
}

// 已 migrate 的临时库并注册为进程级 appDatabase（模块 onStart 依赖）
core::Result<core::Database> openMigrated(const std::string& tag) {
    auto path = test_support::tempDbPath(kDbPrefix, tag);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto opened = core::Database::open(path);
    if (opened.isFail())
        return core::Result<core::Database>::fail(opened.errorCode(), opened.errorMessage());
    auto migrated = opened.value().migrate();
    if (migrated.isFail())
        return core::Result<core::Database>::fail(migrated.errorCode(), migrated.errorMessage());
    return opened;
}

RuleRecord makeRule(const std::string& name, int64_t id,
                    std::optional<int64_t> nextFireAt) {
    RuleRecord r;
    r.id = id;  // createRule 忽略入参 id；此 id 仅用于预期排序推导
    r.name = name;
    r.kind = "calendar";
    r.specJson = R"({"unit":"week","interval":2,"weekdays":[2],"time":"09:00","anchor_date":"2026-10-27"})";
    r.actionKind = "remind";
    r.actionJson = R"({"title":"站立会"})";
    r.nextFireAt = nextFireAt;
    return r;
}

ScheduleItemView makeSystemItem(const std::string& key, int64_t nextFireAt) {
    ScheduleItemView item;
    item.source = "release";
    item.key = key;
    item.title = "上线提醒";
    item.summary = "每工作日 09:30";
    item.nextFireAt = nextFireAt;
    item.sourceRoute = "Release";
    return item;
}

} // namespace

// 防：next_fire_at 混排时列表顺序错乱——到期早的排前面、无下次时刻的排后面；
// 同时锁「归档进折叠组、系统项进系统组」的归属，归属错 = 开关/编辑入口出现在
// 只读行上（§4.2/§4.4）。
REGISTER_TEST(schedule_list_assemble_sort_and_group,
              "schedule 列表装配：各段按 next_fire_at 升序、无值排后，归档/系统项分组正确", {
    std::vector<RuleRecord> rules;
    rules.push_back(makeRule("晚到", 1, 1762000000));
    rules.push_back(makeRule("无下次", 2, std::nullopt));
    rules.push_back(makeRule("早到", 3, 1761000000));
    rules.push_back(makeRule("归档项", 4, 1761500000));
    rules.back().archivedAt = 1760000000;

    std::vector<ScheduleItemView> items;
    items.push_back(makeSystemItem("release:task:1:precheck", 1763000000));
    items.push_back(makeSystemItem("release:task:2:precheck", 1762500000));

    const ListAssembly asm_ = module::schedule::assembleList(rules, items);

    DOCTEST_REQUIRE_EQ(asm_.userRows.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(asm_.userRows[0].name, std::string("早到"));
    DOCTEST_CHECK_EQ(asm_.userRows[1].name, std::string("晚到"));
    DOCTEST_CHECK_EQ(asm_.userRows[2].name, std::string("无下次"));  // 无值排后
    DOCTEST_REQUIRE_EQ(asm_.archivedRows.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(asm_.archivedRows[0].name, std::string("归档项"));
    DOCTEST_CHECK(asm_.archivedRows[0].archived);
    DOCTEST_REQUIRE_EQ(asm_.systemRows.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(asm_.systemRows[0].isSystem);
    DOCTEST_CHECK_EQ(asm_.systemRows[0].key,
                     std::string("release:task:2:precheck"));  // 更早的在前
    DOCTEST_CHECK_EQ(asm_.systemRows[1].key,
                     std::string("release:task:1:precheck"));
    return true;
})

// 防：开关切换只改内存不改库——重启后规则回到旧状态（用户以为关了其实没关）；
// 同时锁不存在 id 的失败路径必须显式 fail，不得静默吞掉。
REGISTER_TEST(schedule_list_toggle_persists_to_db,
              "schedule 列表开关：setRuleEnabled 落库往返 / 不存在 id 显式失败", {
    auto opened = openMigrated("toggle");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    ScheduleStore store(db);
    ScheduleProviderRegistry providers;
    ScheduleListUi ui(store, providers);

    const auto created = store.createRule(makeRule("开关", 0, 1761000000));
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    // 关 → 落库 enabled=0
    DOCTEST_CHECK(ui.setRuleEnabled(id, false).isOk());
    const auto off = store.getRule(id);
    DOCTEST_REQUIRE(off.isOk());
    DOCTEST_CHECK_FALSE(off.value().enabled);

    // 开 → 落库 enabled=1（回切路径同样走 updateRule）
    DOCTEST_CHECK(ui.setRuleEnabled(id, true).isOk());
    const auto on = store.getRule(id);
    DOCTEST_REQUIRE(on.isOk());
    DOCTEST_CHECK(on.value().enabled);

    // 不存在的 id → fail，不崩不吞
    DOCTEST_CHECK(ui.setRuleEnabled(999999, false).isFail());
    return true;
})

// 防：一次性规则执行后仍混在「用户定时」主列表——归档行必须归入折叠区且
// 默认折叠不渲染（§4.4/验收 4：列表默认折叠）；展开后归档行出现且带开关。
REGISTER_TEST(schedule_list_archive_collapsed_by_default,
              "schedule 列表归档：归档行入折叠区默认不渲染，展开后渲染", {
    auto opened = openMigrated("archive");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    ScheduleStore store(db);
    ScheduleProviderRegistry providers;
    ScheduleListUi listUi(store, providers);

    RuleRecord once = makeRule("一次性", 0, 1761000000);
    once.kind = "once";
    once.specJson = R"({"at":1761000000})";
    once.archivedAt = 1761000060;
    DOCTEST_CHECK(store.createRule(once).isOk());

    listUi.reload(1760000000);
    DOCTEST_REQUIRE(listUi.assembly().userRows.empty());
    DOCTEST_REQUIRE_EQ(listUi.assembly().archivedRows.size(), static_cast<size_t>(1));

    // 默认折叠：compose 后归档行不渲染，折叠头渲染
    const eui::Screen screen = makeScreen();
    eui::Ui frame;
    DOCTEST_CHECK_FALSE(listUi.archiveExpanded());
    composeFrame(listUi, frame, screen, "archive_collapsed");
    DOCTEST_CHECK(frame.find("schedule.list.archive.0") == nullptr);
    DOCTEST_CHECK(frame.find("schedule.list.archive.header") != nullptr);

    // 展开：归档行渲染且带开关
    listUi.setArchiveExpanded(true);
    composeFrame(listUi, frame, screen, "archive_expanded");
    DOCTEST_CHECK(frame.find("schedule.list.archive.0") != nullptr);
    DOCTEST_CHECK(frame.find("schedule.list.archive.0.switch") != nullptr);
    return true;
})

// 防：ScheduleProviderRegistry 空注册表（本波 S8 未接任何来源）时列表页崩溃
// 或系统段渲染异常——空态必须渲染「暂无系统定时」占位。
REGISTER_TEST(schedule_list_empty_provider_registry_ok,
              "schedule 系统段：空注册表不出错，渲染「暂无系统定时」空态", {
    auto opened = openMigrated("emptyprov");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    ScheduleStore store(db);
    ScheduleProviderRegistry providers;  // 本波空注册表（release 来源 S8 接）
    ScheduleListUi listUi(store, providers);

    DOCTEST_CHECK(providers.allItems().empty());
    listUi.reload(1760000000);
    DOCTEST_CHECK(listUi.assembly().systemRows.empty());
    DOCTEST_CHECK(listUi.errorText().empty());

    const eui::Screen screen = makeScreen();
    eui::Ui frame;
    composeFrame(listUi, frame, screen, "empty_sys");
    DOCTEST_CHECK(frame.find("schedule.list.sys.empty.message") != nullptr);
    DOCTEST_CHECK(frame.find("schedule.list.sys.0") == nullptr);
    return true;
})

// 防：注册表加行漏改某处——PageId 枚举、kModuleRegistry 行、工厂、模块
// getter 四者漂移（侧边栏入口不出现 / 托盘短名与页面名不一致）。同时锁
// Schedule 位于 Todo 之后 = 侧边栏 work 组内的位置契约。
REGISTER_TEST(schedule_module_registry_entry_complete,
              "schedule 模块注册表项：PageId/kModuleRegistry/工厂/getter 四者一致且位置在 Todo 之后", {
    // PageId 序：Schedule 在 Todo 之后、ClaudeProxy 之前（侧边栏顺序契约）
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Schedule) >
                  static_cast<int>(::core::PageId::Todo));
    DOCTEST_CHECK(static_cast<int>(::core::PageId::Schedule) <
                  static_cast<int>(::core::PageId::ClaudeProxy));

    // 真实注册表里恰有一行 Schedule 且元数据完整
    const ::app::registry::ModuleEntry* entry = nullptr;
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page == ::core::PageId::Schedule) {
            entry = &e;
            break;
        }
    }
    DOCTEST_REQUIRE(entry != nullptr);
    // 开关卡已补齐：门控键 = kModuleIdSchedule（默认启用，走通用模块门控）
    DOCTEST_CHECK_EQ(std::string(entry->gatingKey),
                     std::string(module::settings::kModuleIdSchedule));
    DOCTEST_CHECK_EQ(std::string(entry->shortName), std::string("定时"));
    DOCTEST_CHECK_EQ(std::string(entry->iconGlyph), eui::utf8(0xF017));  // fa-clock

    // 工厂非空且 getter 与表双副本一致
    std::unique_ptr<::core::Module> mod(entry->factory());
    DOCTEST_REQUIRE(mod != nullptr);
    DOCTEST_CHECK(mod->pageId() == ::core::PageId::Schedule);
    DOCTEST_CHECK_EQ(mod->shortName(), std::string(entry->shortName));
    DOCTEST_CHECK_EQ(mod->iconGlyph(), std::string(entry->iconGlyph));
    return true;
})

// 防：模块 onStart/onStop 生命周期缺陷——双 onStart 重复装配（重复注册渠道/
// 重建 store）、onStop 后残留装配、未启动时 compose 崩溃。
// TestEnv RAII 注册/注销 appDatabase：用例中途 fail-fast 也不会把注册泄漏给
// 同进程后续用例。
REGISTER_TEST(schedule_module_start_stop_idempotent,
              "schedule 模块生命周期：onStart 幂等 / onStop 幂等 / 启停后无头 compose 不崩", {
    test_support::TestEnv env(kDbPrefix, "lifecycle", /*registerAppDb=*/true);
    (void)env;
    module::schedule::ScheduleModule mod;
    mod.onStart();
    mod.onStart();  // 双 onStart = no-op（不重复装配）
    mod.onStop();
    mod.onStop();   // 双 onStop = no-op

    const eui::Screen screen = makeScreen();
    eui::Ui frame;
    frame.begin("lifecycle");
    mod.compose(frame, screen);  // 停止后 compose 走兜底分支，不崩
    frame.end();
    frame.layout(screen.width, screen.height);
    return true;
})

// 防：规则摘要解析失败把整页带崩——spec_json 非法/未知 kind 必须回退
// kind 名展示（列表可用性优先于摘要精确性）。
REGISTER_TEST(schedule_list_summary_fallback_no_crash,
              "schedule 规则摘要：非法 spec_json / 未知 kind 回退 kind 名不崩", {
    RuleRecord broken;
    broken.kind = "calendar";
    broken.specJson = "{not-json";
    DOCTEST_CHECK_EQ(module::schedule::ruleSummary(broken),
                     std::string("calendar"));

    RuleRecord unknown;
    unknown.kind = "mystery";
    unknown.specJson = "{}";
    DOCTEST_CHECK_EQ(module::schedule::ruleSummary(unknown),
                     std::string("mystery"));

    RuleRecord week;
    week.kind = "calendar";
    week.specJson =
        R"({"unit":"week","interval":2,"weekdays":[2],"time":"09:00"})";
    DOCTEST_CHECK_EQ(module::schedule::ruleSummary(week),
                     std::string("每 2 周 周二 09:00"));
    return true;
})

// 防：入口按钮退化回 LOG 占位不接抽屉——点击「运行历史」必须打开抽屉并装载
// runs+投递数据，关闭钮必须收起。删掉 openHistory 接线（onClick 不再置 open/
// 不再装数据）后：抽屉 opacity 恒 0、historyItems 恒空、row.* 不出现，本用例必红。
REGISTER_TEST(schedule_list_history_drawer_open_close_wiring,
              "schedule 历史抽屉接线：点入口开抽屉装载两表数据，点关闭收起", {
    auto opened = openMigrated("histwiring");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    ScheduleStore store(db);
    ScheduleProviderRegistry providers;
    ScheduleListUi listUi(store, providers);

    const auto created = store.createRule(makeRule("历史规则", 0, 1761000000));
    DOCTEST_REQUIRE(created.isOk());
    const int64_t ruleId = created.value().id;
    auto makeRunRow = [&](int64_t plannedAt, const std::string& status) {
        module::schedule::RunRecord run;
        run.origin = "user";
        run.originKey = "rule:" + std::to_string(ruleId);
        run.ruleId = ruleId;
        run.plannedAt = plannedAt;
        run.startedAt = plannedAt;
        run.status = status;
        return run;
    };
    DOCTEST_REQUIRE(store.insertRun(makeRunRow(1760001000, "success")).isOk());
    const auto failedRun = store.insertRun(makeRunRow(1760002000, "failed"));
    DOCTEST_REQUIRE(failedRun.isOk());
    module::schedule::DeliveryRecord d;
    d.runId = failedRun.value().id;
    d.channelId = 1;
    d.attempt = 0;
    d.status = "failed";
    DOCTEST_REQUIRE(store.insertDelivery(d).isOk());

    listUi.reload(1760010000);
    const eui::Screen screen = makeScreen();
    eui::Ui frame;

    // 初始：抽屉关闭态也常驻构建（panel 节点在、opacity=0）
    composeFrame(listUi, frame, screen, "hist_wiring_closed");
    const auto* panel = frame.find("schedule.history.drawer.panel");
    DOCTEST_REQUIRE(panel != nullptr);
    DOCTEST_CHECK_EQ(panel->opacity, 0.0f);

    // 点页头入口 → 置 open + 装载（planned_at 倒序：failed 在前且带渠道汇总）
    const auto* entry = frame.find("schedule.list.history");
    DOCTEST_REQUIRE(entry != nullptr);
    DOCTEST_REQUIRE(entry->onClick != nullptr);
    entry->onClick();
    DOCTEST_CHECK(listUi.historyOpen());
    DOCTEST_REQUIRE_EQ(listUi.historyItems().size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(listUi.historyItems()[0].run.plannedAt, 1760002000);
    DOCTEST_CHECK_EQ(listUi.historyItems()[0].run.status, std::string("failed"));
    DOCTEST_REQUIRE_EQ(listUi.historyItems()[0].channelSummaries.size(),
                       static_cast<size_t>(1));

    composeFrame(listUi, frame, screen, "hist_wiring_open");
    DOCTEST_CHECK_EQ(frame.find("schedule.history.drawer.panel")->opacity, 1.0f);
    DOCTEST_CHECK(frame.find("schedule.history.row.0") != nullptr);
    DOCTEST_CHECK(frame.find("schedule.history.row.1") != nullptr);

    // 点抽屉内关闭钮 → 收起（下一帧 opacity 回 0；button 的 onClick 挂在 .bg 节点）
    const auto* closeBtn = frame.find("schedule.history.close.bg");
    DOCTEST_REQUIRE(closeBtn != nullptr);
    DOCTEST_REQUIRE(closeBtn->onClick != nullptr);
    closeBtn->onClick();
    DOCTEST_CHECK_FALSE(listUi.historyOpen());
    composeFrame(listUi, frame, screen, "hist_wiring_closed_again");
    DOCTEST_CHECK_EQ(frame.find("schedule.history.drawer.panel")->opacity, 0.0f);
    return true;
})

// 防：零历史（新库/空 run 表）点入口崩溃或渲染残行——空态抽屉只出面板与关闭钮。
REGISTER_TEST(schedule_list_history_drawer_empty_ok,
              "schedule 历史抽屉接线：无运行记录时打开为空态不崩", {
    auto opened = openMigrated("histempty");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    ScheduleStore store(db);
    ScheduleProviderRegistry providers;
    ScheduleListUi listUi(store, providers);

    DOCTEST_CHECK(store.createRule(makeRule("空历史规则", 0, 1761000000)).isOk());
    listUi.reload(1760010000);

    const eui::Screen screen = makeScreen();
    eui::Ui frame;
    composeFrame(listUi, frame, screen, "hist_empty_closed");
    const auto* entry = frame.find("schedule.list.history");
    DOCTEST_REQUIRE(entry != nullptr);
    DOCTEST_REQUIRE(entry->onClick != nullptr);
    entry->onClick();
    DOCTEST_CHECK(listUi.historyOpen());
    DOCTEST_CHECK(listUi.historyItems().empty());

    composeFrame(listUi, frame, screen, "hist_empty_open");
    DOCTEST_CHECK_EQ(frame.find("schedule.history.drawer.panel")->opacity, 1.0f);
    DOCTEST_CHECK(frame.find("schedule.history.row.0") == nullptr);
    return true;
})

// 防：摘要键名与 buildCalendarJson 落库键脱节（P2-6）——month 的「第 n 个周 x」
// 实际存对象 nth_weekday{n,weekday}、year 只有 month_day，读错键 = 摘要退化成
// 「每月」「每年」；last_workday 形态此前无文案。
REGISTER_TEST(schedule_list_summary_month_year_keys, "schedule 列表：month/year 摘要读 buildCalendarJson 实际落库键", {
    module::schedule::RuleRecord r;
    r.kind = "calendar";
    r.name = "月度";
    r.actionKind = "remind";
    r.specJson = R"({"unit":"month","interval":1,"nth_weekday":{"n":2,"weekday":2},"time":"09:00"})";
    const auto monthNth = module::schedule::ruleSummary(r);
    DOCTEST_CHECK(monthNth.find("第 2 个") != std::string::npos);
    DOCTEST_CHECK(monthNth.find("周二") != std::string::npos);

    r.specJson = R"({"unit":"month","interval":1,"last_workday":true,"time":"09:00"})";
    const auto monthLast = module::schedule::ruleSummary(r);
    DOCTEST_CHECK(monthLast.find("最后一个工作日") != std::string::npos);

    r.name = "年度";
    r.specJson = R"({"unit":"year","interval":1,"month_day":15,"time":"09:00"})";
    const auto yearDay = module::schedule::ruleSummary(r);
    DOCTEST_CHECK(yearDay.find("15 日") != std::string::npos);
    return true;
});

// 防：编辑/删除入口接错 store 调用（软删默认、勾选彻底删才物理删；失败不静默关弹窗）。
REGISTER_TEST(schedule_list_edit_delete_wiring, "schedule 列表：编辑回填 + 删除软删/物理删接线", {
    auto opened = openMigrated("edit_delete_wiring");
    DOCTEST_REQUIRE(opened.isOk());
    auto& db = opened.value().db();
    ScheduleStore store(db);

    module::schedule::RuleRecord r = makeRule("待编辑", 0, std::nullopt);
    const auto created = store.createRule(r);
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    ScheduleProviderRegistry providers;
    ScheduleListUi ui(store, providers, nullptr);
    ui.reload(1760000000);

    // 编辑入口：回填开弹窗；成功保存（updateRule 路径）改名后落库生效
    ui.openEditRule(id);
    DOCTEST_CHECK(ui.editOpen());
    DOCTEST_CHECK(ui.errorText().empty());
    module::schedule::RuleRecord edited;
    edited.id = id;
    edited.name = "改名后";
    edited.kind = created.value().kind;
    edited.specJson = created.value().specJson;
    edited.actionKind = created.value().actionKind;
    edited.actionJson = created.value().actionJson;
    edited.channelIds = created.value().channelIds;
    edited.missedPolicy = created.value().missedPolicy;
    edited.nextFireAt = created.value().nextFireAt;
    DOCTEST_CHECK(ui.saveRuleForTest(edited).empty());
    const auto renamed = store.getRule(id);
    DOCTEST_REQUIRE(renamed.isOk());
    DOCTEST_CHECK(renamed.value().name == "改名后");
    DOCTEST_CHECK_FALSE(ui.editOpen());

    // 删除入口：默认软删（getRule NotFound = 对外不可见），确认后弹窗收起
    ui.requestDeleteRule(id);
    DOCTEST_CHECK(ui.deleteConfirmOpen());
    ui.confirmPendingDelete();
    DOCTEST_CHECK_FALSE(ui.deleteConfirmOpen());
    DOCTEST_CHECK(ui.errorText().empty());
    DOCTEST_CHECK(store.getRule(id).isFail());  // 软删后 getRule=NotFound

    // 彻底删除路径：purge 后行消失（软删行还在库，仅过滤）
    const auto again = store.createRule(makeRule("待物理删", 0, std::nullopt));
    DOCTEST_REQUIRE(again.isOk());
    ui.requestDeleteRule(again.value().id);
    ui.setPendingDeletePurgeForTest(true);
    ui.confirmPendingDelete();
    DOCTEST_CHECK(ui.errorText().empty());
    DOCTEST_CHECK(store.getRule(again.value().id).isFail());

    return true;
});

REGISTER_TEST(schedule_module_provider_seam_pre_start,
              "schedule 模块：装配段注册缝（onStart 前可注册、onStop 不清空注册表）", {
    module::schedule::ScheduleModule mod;
    struct StaticSource : module::schedule::ScheduleProvider {
        std::vector<module::schedule::ScheduleItemView> items() const override {
            module::schedule::ScheduleItemView v;
            v.source = "release";
            v.key = "release:task:1:precheck";
            return {v};
        }
    };
    const auto reg = mod.providerRegistry().registerProvider(
        std::make_unique<StaticSource>());
    DOCTEST_CHECK(reg.isOk());
    // 重复注册同 source 判重拒绝
    const auto dup = mod.providerRegistry().registerProvider(
        std::make_unique<StaticSource>());
    DOCTEST_CHECK(dup.isFail());
    DOCTEST_CHECK(mod.providerRegistry().allItems().size() == 1);
    // 未启动时 onStop = no-op，注册表保留（装配期注册不因启停循环丢失）
    mod.onStop();
    DOCTEST_CHECK(mod.providerRegistry().allItems().size() == 1);
    return true;
})
