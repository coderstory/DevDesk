// ReleaseStore 落库层用例。每个用例用独立临时库（TestDb 已 migrate 到含 release_* 九表），
// 不碰用户真实库。场景与所防事故：
//   1) 服务增改查删 —— 防「upsert 静默插重复行 / 更新不落库 / 删不存在返回成功」
//   2) 环境关联服务 —— 防「upsert 后旧关联残留 / 删服务不从环境摘掉 / 删服务误级联任务快照」
//   3) 任务按日期降序与按日查找 —— 防「列表顺序漂移 / find 未命中不返回 nullopt / 更新不落库」
//   4) addTaskEnv 连带写服务快照 —— 防「环境与任务服务脱钩、快照字段丢失、移除变成物理删」
//   5) 配置项顺序与回写 —— 防「配置项顺序错乱、doneAt/skipped 丢失」
//   6) 步骤置位与撤回 —— 防「撤回删除整行（历史丢失）/ 重复置位插出重复行」
//   7) 日志追加与顺序 —— 防「日志按 id 而非时间排序 / 跨任务串日志」
//   8) 任务分页与总数 —— 防「一次读全表再内存切片 / 分页越界返脏数据 / 总数口径漂移」
//   9) createTaskWithEnvs 一次落库任务+N环境+每环境M服务 —— 防「原子接口漏写环境/服务、快照字段丢失」
//  10) createTaskWithEnvs 中途失败整体回滚 —— 防「残留 Draft 死单、残留死单挡住同日重试」
#include "doctest.h"
#include "test_support.h"

#include "module_release/release_store.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace {

namespace rel = module::release;

struct SeedIds {
    std::int64_t taskId = 0;
    std::int64_t envId = 0;
    std::int64_t serviceId = 0;
};

// 建「任务 → 环境快照 → 一个服务快照」最小链；任一步失败返回 0，由调用方断言。
SeedIds seedTaskService(rel::ReleaseStore& store, const std::string& planDate) {
    SeedIds ids;
    rel::Task task;
    task.planDate = planDate;
    auto taskId = store.createTask(task);
    if (taskId.isFail()) return ids;
    ids.taskId = taskId.value();

    rel::TaskEnv env;
    env.taskId = ids.taskId;
    env.envName = "EM";
    env.startMinute = 540;
    env.endMinute = 600;

    rel::TaskService svc;
    svc.serviceName = "API";
    svc.kind = rel::ServiceKind::Backend;
    svc.defaultBranch = "main";
    svc.branch = "release/1";

    auto envId = store.addTaskEnv(env, {svc});
    if (envId.isFail()) return ids;
    ids.envId = envId.value();

    auto services = store.listTaskServices(ids.envId);
    if (services.isFail() || services.value().empty()) return ids;
    ids.serviceId = services.value()[0].id;
    return ids;
}

} // namespace

REGISTER_TEST(release_store_service_crud,
              "release_store: 服务新增/更新/排序/删除（重名 AlreadyExists、重复删 NotFound）", {
    test_support::TestDb tdb("releasestore", "svc");
    rel::ReleaseStore store(tdb.handle());

    rel::ServiceDef web;
    web.name = "Web";
    web.kind = rel::ServiceKind::PcWeb;
    web.defaultBranch = "release/web";
    web.sort = 2;
    auto webId = store.upsertService(web);
    DOCTEST_REQUIRE(webId.isOk());
    DOCTEST_CHECK(webId.value() > 0);

    rel::ServiceDef api;
    api.name = "API";
    api.kind = rel::ServiceKind::Backend;
    api.defaultBranch = "main";
    api.sort = 1;
    DOCTEST_REQUIRE(store.upsertService(api).isOk());

    auto listed = store.listServices();
    DOCTEST_REQUIRE(listed.isOk());
    DOCTEST_REQUIRE_EQ(listed.value().size(), 2u);
    DOCTEST_CHECK_EQ(listed.value()[0].name, std::string("API")); // sort=1 在前
    DOCTEST_CHECK(listed.value()[0].kind == rel::ServiceKind::Backend);
    DOCTEST_CHECK_EQ(listed.value()[1].defaultBranch, std::string("release/web"));

    rel::ServiceDef renamed = listed.value()[1];
    renamed.name = "Web-2";
    renamed.kind = rel::ServiceKind::MobileWeb;
    auto updated = store.upsertService(renamed);
    DOCTEST_REQUIRE(updated.isOk());
    DOCTEST_CHECK_EQ(updated.value(), webId.value());

    auto after = store.listServices();
    DOCTEST_REQUIRE(after.isOk());
    DOCTEST_REQUIRE_EQ(after.value().size(), 2u);
    DOCTEST_CHECK_EQ(after.value()[1].name, std::string("Web-2"));
    DOCTEST_CHECK(after.value()[1].kind == rel::ServiceKind::MobileWeb);

    rel::ServiceDef dup; // 重名不得插第二行
    dup.name = "Web-2";
    DOCTEST_CHECK(store.upsertService(dup).isFail());

    DOCTEST_REQUIRE(store.deleteService(webId.value()).isOk());
    DOCTEST_CHECK(store.deleteService(webId.value()).isFail());
    return true;
})

REGISTER_TEST(release_store_env_service_links,
              "release_store: 环境关联服务（upsert 整体替换、删服务自动摘关联、删服务不动任务快照、重复删 NotFound）", {
    test_support::TestDb tdb("releasestore", "env");
    rel::ReleaseStore store(tdb.handle());

    rel::ServiceDef api;
    api.name = "API";
    auto apiId = store.upsertService(api);
    DOCTEST_REQUIRE(apiId.isOk());
    rel::ServiceDef web;
    web.name = "Web";
    web.kind = rel::ServiceKind::PcWeb;
    web.sort = 1;
    auto webId = store.upsertService(web);
    DOCTEST_REQUIRE(webId.isOk());

    rel::EnvDef env;
    env.name = "EM";
    env.serviceIds = {apiId.value(), webId.value()};
    env.startMinute = 540;
    env.endMinute = 60; // 跨零点
    auto envId = store.upsertEnv(env);
    DOCTEST_REQUIRE(envId.isOk());
    DOCTEST_CHECK(envId.value() > 0);

    auto envs = store.listEnvs();
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_REQUIRE_EQ(envs.value().size(), 1u);
    DOCTEST_CHECK_EQ(envs.value()[0].name, std::string("EM"));
    DOCTEST_CHECK_EQ(envs.value()[0].startMinute, 540);
    DOCTEST_CHECK_EQ(envs.value()[0].endMinute, 60);
    DOCTEST_REQUIRE_EQ(envs.value()[0].serviceIds.size(), 2u);
    DOCTEST_CHECK_EQ(envs.value()[0].serviceIds[0], apiId.value());
    DOCTEST_CHECK_EQ(envs.value()[0].serviceIds[1], webId.value());

    // upsert 语义 = 以入参为准：旧关联必须被整体替换而不是追加
    env.id = envId.value();
    env.name = "EU";
    env.serviceIds = {webId.value()};
    DOCTEST_REQUIRE(store.upsertEnv(env).isOk());
    envs = store.listEnvs();
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_REQUIRE_EQ(envs.value().size(), 1u);
    DOCTEST_CHECK_EQ(envs.value()[0].name, std::string("EU"));
    DOCTEST_REQUIRE_EQ(envs.value()[0].serviceIds.size(), 1u);
    DOCTEST_CHECK_EQ(envs.value()[0].serviceIds[0], webId.value());

    // 先为任务建快照（引用基础服务 Web），再删基础服务：快照按值落库，
    // deleteService 若误级联/误改 release_task_service，下面的逐字段断言会红。
    rel::Task task;
    task.planDate = "2026-11-04";
    auto taskId = store.createTask(task);
    DOCTEST_REQUIRE(taskId.isOk());

    rel::TaskEnv snapEnv;
    snapEnv.taskId = taskId.value();
    snapEnv.envName = "EM";
    snapEnv.startMinute = 540;
    snapEnv.endMinute = 60; // 跨零点，与基础环境同窗
    rel::TaskService snapWeb;
    snapWeb.serviceName = "Web";
    snapWeb.kind = rel::ServiceKind::PcWeb;
    snapWeb.defaultBranch = "release/web";
    snapWeb.branch = "";
    auto snapEnvId = store.addTaskEnv(snapEnv, {snapWeb});
    DOCTEST_REQUIRE(snapEnvId.isOk());

    // 删服务（基础配置）→ 自动从环境摘掉；任务快照不受影响
    DOCTEST_REQUIRE(store.deleteService(webId.value()).isOk());
    envs = store.listEnvs();
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_REQUIRE_EQ(envs.value().size(), 1u);
    DOCTEST_CHECK(envs.value()[0].serviceIds.empty());

    // 任务快照与基础配置脱钩：删基础服务后逐字段核对原样（服务名/类型/默认分支/环境时间窗）
    auto snapEnvs = store.listTaskEnvs(taskId.value());
    DOCTEST_REQUIRE(snapEnvs.isOk());
    DOCTEST_REQUIRE_EQ(snapEnvs.value().size(), 1u);
    DOCTEST_CHECK_EQ(snapEnvs.value()[0].envName, std::string("EM"));
    DOCTEST_CHECK_EQ(snapEnvs.value()[0].startMinute, 540);
    DOCTEST_CHECK_EQ(snapEnvs.value()[0].endMinute, 60);
    DOCTEST_CHECK(!snapEnvs.value()[0].removed);

    auto snapSvcs = store.listTaskServices(snapEnvId.value());
    DOCTEST_REQUIRE(snapSvcs.isOk());
    DOCTEST_REQUIRE_EQ(snapSvcs.value().size(), 1u);
    DOCTEST_CHECK_EQ(snapSvcs.value()[0].serviceName, std::string("Web"));
    DOCTEST_CHECK(snapSvcs.value()[0].kind == rel::ServiceKind::PcWeb);
    DOCTEST_CHECK_EQ(snapSvcs.value()[0].defaultBranch, std::string("release/web"));
    DOCTEST_CHECK(!snapSvcs.value()[0].removed);

    DOCTEST_REQUIRE(store.deleteEnv(envId.value()).isOk());
    envs = store.listEnvs();
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_CHECK(envs.value().empty());
    DOCTEST_CHECK(store.deleteEnv(envId.value()).isFail());
    return true;
})

REGISTER_TEST(release_store_tasks_order_and_lookup,
              "release_store: 任务按 planDate 降序、findTaskByDate 命中/未命中、作废不占当天", {
    test_support::TestDb tdb("releasestore", "task");
    rel::ReleaseStore store(tdb.handle());

    rel::Task later;
    later.planDate = "2026-11-03";
    later.status = rel::TaskStatus::Pending;
    later.announceText = "公告";
    later.pretestNo = "PT-1";
    later.cabOk = true;
    auto laterId = store.createTask(later);
    DOCTEST_REQUIRE(laterId.isOk());

    rel::Task earlier;
    earlier.planDate = "2026-10-30";
    auto earlierId = store.createTask(earlier);
    DOCTEST_REQUIRE(earlierId.isOk());

    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 2u);
    DOCTEST_CHECK_EQ(tasks.value()[0].planDate, std::string("2026-11-03"));
    DOCTEST_CHECK_EQ(tasks.value()[1].planDate, std::string("2026-10-30"));

    auto hit = store.findTaskByDate("2026-10-30");
    DOCTEST_REQUIRE(hit.isOk());
    DOCTEST_REQUIRE(hit.value().has_value());
    DOCTEST_CHECK_EQ(hit.value()->id, earlierId.value());
    DOCTEST_CHECK(hit.value()->status == rel::TaskStatus::Draft);

    auto miss = store.findTaskByDate("2026-12-01");
    DOCTEST_REQUIRE(miss.isOk());
    DOCTEST_CHECK(!miss.value().has_value());

    rel::Task upd = *hit.value();
    upd.status = rel::TaskStatus::Running;
    upd.announceSentAt = 1700000000;
    upd.cabOk = true;
    DOCTEST_REQUIRE(store.updateTask(upd).isOk());
    auto reload = store.findTaskByDate("2026-10-30");
    DOCTEST_REQUIRE(reload.isOk());
    DOCTEST_REQUIRE(reload.value().has_value());
    DOCTEST_CHECK(reload.value()->status == rel::TaskStatus::Running);
    DOCTEST_CHECK_EQ(reload.value()->announceSentAt, 1700000000);
    DOCTEST_CHECK(reload.value()->cabOk);

    rel::Task ghost; // 更新不存在的 id 必须失败，不能静默成功
    ghost.id = 999999;
    ghost.planDate = "2027-01-01";
    DOCTEST_CHECK(store.updateTask(ghost).isFail());

    // 作废任务不占当天名额：同一天再建一个作废任务后，查找仍命中有效的那个
    rel::Task voided;
    voided.planDate = "2026-10-30";
    voided.status = rel::TaskStatus::Voided;
    DOCTEST_REQUIRE(store.createTask(voided).isOk());
    auto still = store.findTaskByDate("2026-10-30");
    DOCTEST_REQUIRE(still.isOk());
    DOCTEST_REQUIRE(still.value().has_value());
    DOCTEST_CHECK_EQ(still.value()->id, upd.id);
    auto allTasks = store.listTasks();
    DOCTEST_REQUIRE(allTasks.isOk());
    DOCTEST_CHECK_EQ(allTasks.value().size(), 3u);
    DOCTEST_REQUIRE(laterId.isOk());
    return true;
})

REGISTER_TEST(release_store_task_env_snapshot,
              "release_store: addTaskEnv 连带写服务快照；改分支落库；移除只置 removed 标志", {
    test_support::TestDb tdb("releasestore", "tenv");
    rel::ReleaseStore store(tdb.handle());

    rel::Task task;
    task.planDate = "2026-11-05";
    auto taskId = store.createTask(task);
    DOCTEST_REQUIRE(taskId.isOk());

    rel::TaskEnv env;
    env.taskId = taskId.value();
    env.envName = "EM";
    env.startMinute = 540;
    env.endMinute = 600;

    rel::TaskService api;
    api.serviceName = "API";
    api.kind = rel::ServiceKind::Backend;
    api.defaultBranch = "main";
    api.branch = "release/1";
    rel::TaskService web;
    web.serviceName = "Web";
    web.kind = rel::ServiceKind::PcWeb;
    web.defaultBranch = "main";
    web.branch = "";

    auto envId = store.addTaskEnv(env, {api, web});
    DOCTEST_REQUIRE(envId.isOk());
    DOCTEST_CHECK(envId.value() > 0);

    auto envs = store.listTaskEnvs(taskId.value());
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_REQUIRE_EQ(envs.value().size(), 1u);
    DOCTEST_CHECK_EQ(envs.value()[0].id, envId.value());
    DOCTEST_CHECK_EQ(envs.value()[0].taskId, taskId.value());
    DOCTEST_CHECK_EQ(envs.value()[0].envName, std::string("EM"));
    DOCTEST_CHECK_EQ(envs.value()[0].endMinute, 600);
    DOCTEST_CHECK(!envs.value()[0].removed);

    auto services = store.listTaskServices(envId.value());
    DOCTEST_REQUIRE(services.isOk());
    DOCTEST_REQUIRE_EQ(services.value().size(), 2u);
    DOCTEST_CHECK(services.value()[0].id > 0);
    DOCTEST_CHECK_EQ(services.value()[0].taskEnvId, envId.value());
    DOCTEST_CHECK_EQ(services.value()[0].serviceName, std::string("API"));
    DOCTEST_CHECK(services.value()[0].kind == rel::ServiceKind::Backend);
    DOCTEST_CHECK_EQ(services.value()[0].defaultBranch, std::string("main"));
    DOCTEST_CHECK_EQ(services.value()[0].branch, std::string("release/1"));
    DOCTEST_CHECK_EQ(services.value()[1].serviceName, std::string("Web"));
    DOCTEST_CHECK(services.value()[1].kind == rel::ServiceKind::PcWeb);
    DOCTEST_CHECK(!services.value()[1].removed);

    rel::TaskService edited = services.value()[0];
    edited.branch = "hotfix/9";
    DOCTEST_REQUIRE(store.updateTaskService(edited).isOk());
    services = store.listTaskServices(envId.value());
    DOCTEST_REQUIRE(services.isOk());
    DOCTEST_CHECK_EQ(services.value()[0].branch, std::string("hotfix/9"));

    // 移除服务 = removed 标志：行保留（历史/进度留痕），不是物理删
    DOCTEST_REQUIRE(store.removeTaskService(services.value()[0].id).isOk());
    services = store.listTaskServices(envId.value());
    DOCTEST_REQUIRE(services.isOk());
    DOCTEST_REQUIRE_EQ(services.value().size(), 2u);
    DOCTEST_CHECK(services.value()[0].removed);

    DOCTEST_REQUIRE(store.removeTaskEnv(envId.value()).isOk());
    envs = store.listTaskEnvs(taskId.value());
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_REQUIRE_EQ(envs.value().size(), 1u);
    DOCTEST_CHECK(envs.value()[0].removed);
    DOCTEST_CHECK(store.removeTaskEnv(999999).isFail());
    return true;
})

REGISTER_TEST(release_store_task_configs,
              "release_store: 配置项按 order 排序、upsert 回写 content/skipped/doneAt、不存在 id 失败", {
    test_support::TestDb tdb("releasestore", "cfg");
    rel::ReleaseStore store(tdb.handle());

    const auto ids = seedTaskService(store, "2026-11-06");
    DOCTEST_REQUIRE(ids.serviceId > 0);

    rel::TaskConfig nacos;
    nacos.taskServiceId = ids.serviceId;
    nacos.kind = rel::ConfigKind::Nacos;
    nacos.content = "nacos-content";
    nacos.order = 1;
    DOCTEST_REQUIRE(store.upsertTaskConfig(nacos).isOk());

    rel::TaskConfig db;
    db.taskServiceId = ids.serviceId;
    db.kind = rel::ConfigKind::Db;
    db.skipped = true;
    db.order = 0;
    DOCTEST_REQUIRE(store.upsertTaskConfig(db).isOk());

    auto configs = store.listTaskConfigs(ids.serviceId);
    DOCTEST_REQUIRE(configs.isOk());
    DOCTEST_REQUIRE_EQ(configs.value().size(), 2u);
    DOCTEST_CHECK(configs.value()[0].kind == rel::ConfigKind::Db);
    DOCTEST_CHECK_EQ(configs.value()[0].order, 0);
    DOCTEST_CHECK(configs.value()[0].skipped);
    DOCTEST_CHECK(configs.value()[1].kind == rel::ConfigKind::Nacos);
    DOCTEST_CHECK_EQ(configs.value()[1].content, std::string("nacos-content"));

    rel::TaskConfig edited = configs.value()[1];
    edited.content = "nacos-content-2";
    edited.skipped = true;
    edited.doneAt = 1700000123;
    DOCTEST_REQUIRE(store.upsertTaskConfig(edited).isOk());
    configs = store.listTaskConfigs(ids.serviceId);
    DOCTEST_REQUIRE(configs.isOk());
    DOCTEST_REQUIRE_EQ(configs.value().size(), 2u);
    DOCTEST_CHECK_EQ(configs.value()[1].content, std::string("nacos-content-2"));
    DOCTEST_CHECK(configs.value()[1].skipped);
    DOCTEST_CHECK_EQ(configs.value()[1].doneAt, 1700000123);

    rel::TaskConfig ghost;
    ghost.id = 999999;
    ghost.taskServiceId = ids.serviceId;
    DOCTEST_CHECK(store.upsertTaskConfig(ghost).isFail());
    return true;
})

REGISTER_TEST(release_store_steps_set_and_revoke,
              "release_store: 步骤置位/重复置位不插重复行、doneAt=0 撤回但保留行", {
    test_support::TestDb tdb("releasestore", "step");
    rel::ReleaseStore store(tdb.handle());

    const auto ids = seedTaskService(store, "2026-11-09");
    DOCTEST_REQUIRE(ids.serviceId > 0);

    DOCTEST_REQUIRE(store.setStepDone(ids.serviceId, rel::StepKind::Deploy, 1700000000).isOk());
    auto steps = store.listTaskSteps(ids.serviceId);
    DOCTEST_REQUIRE(steps.isOk());
    DOCTEST_REQUIRE_EQ(steps.value().size(), 1u);
    DOCTEST_CHECK(steps.value()[0].kind == rel::StepKind::Deploy);
    DOCTEST_CHECK_EQ(steps.value()[0].doneAt, 1700000000);
    DOCTEST_CHECK_EQ(steps.value()[0].taskServiceId, ids.serviceId);

    // 重复置位同一 kind：更新已有行，不新增
    DOCTEST_REQUIRE(store.setStepDone(ids.serviceId, rel::StepKind::Deploy, 1700000005).isOk());
    steps = store.listTaskSteps(ids.serviceId);
    DOCTEST_REQUIRE(steps.isOk());
    DOCTEST_REQUIRE_EQ(steps.value().size(), 1u);
    DOCTEST_CHECK_EQ(steps.value()[0].doneAt, 1700000005);

    DOCTEST_REQUIRE(store.setStepDone(ids.serviceId, rel::StepKind::MergeBranch, 1700000100).isOk());
    steps = store.listTaskSteps(ids.serviceId);
    DOCTEST_REQUIRE(steps.isOk());
    DOCTEST_REQUIRE_EQ(steps.value().size(), 2u);

    // 撤回 = doneAt 置 0，行保留（历史与进度统计都依赖「有过这一步」）
    DOCTEST_REQUIRE(store.setStepDone(ids.serviceId, rel::StepKind::Deploy, 0).isOk());
    steps = store.listTaskSteps(ids.serviceId);
    DOCTEST_REQUIRE(steps.isOk());
    DOCTEST_REQUIRE_EQ(steps.value().size(), 2u);
    int deployIndex = -1;
    for (std::size_t i = 0; i < steps.value().size(); ++i) {
        if (steps.value()[i].kind == rel::StepKind::Deploy) {
            deployIndex = static_cast<int>(i);
            break;
        }
    }
    DOCTEST_REQUIRE(deployIndex >= 0);
    DOCTEST_CHECK_EQ(steps.value()[static_cast<std::size_t>(deployIndex)].doneAt, 0);
    return true;
})

REGISTER_TEST(release_store_logs_ordered_and_scoped,
              "release_store: 日志按 at 升序、字段原样回读、按任务隔离", {
    test_support::TestDb tdb("releasestore", "log");
    rel::ReleaseStore store(tdb.handle());

    const auto ids = seedTaskService(store, "2026-11-12");
    DOCTEST_REQUIRE(ids.taskId > 0);

    rel::TaskLog second;
    second.taskId = ids.taskId;
    second.scope = "task";
    second.step = "announce";
    second.action = "confirm";
    second.at = 200;
    second.note = "second";
    DOCTEST_REQUIRE(store.appendLog(second).isOk());

    rel::TaskLog first;
    first.taskId = ids.taskId;
    first.scope = "EM/API";
    first.step = "deploy";
    first.action = "revoke";
    first.at = 100;
    first.note = "first";
    DOCTEST_REQUIRE(store.appendLog(first).isOk());

    auto logs = store.listLogs(ids.taskId);
    DOCTEST_REQUIRE(logs.isOk());
    DOCTEST_REQUIRE_EQ(logs.value().size(), 2u);
    DOCTEST_CHECK_EQ(logs.value()[0].at, 100);
    DOCTEST_CHECK_EQ(logs.value()[0].scope, std::string("EM/API"));
    DOCTEST_CHECK_EQ(logs.value()[0].step, std::string("deploy"));
    DOCTEST_CHECK_EQ(logs.value()[0].action, std::string("revoke"));
    DOCTEST_CHECK_EQ(logs.value()[1].at, 200);
    DOCTEST_CHECK_EQ(logs.value()[1].note, std::string("second"));

    rel::Task other;
    other.planDate = "2026-11-13";
    auto otherId = store.createTask(other);
    DOCTEST_REQUIRE(otherId.isOk());
    rel::TaskLog otherLog;
    otherLog.taskId = otherId.value();
    otherLog.action = "confirm";
    otherLog.at = 50;
    DOCTEST_REQUIRE(store.appendLog(otherLog).isOk());

    auto firstLogs = store.listLogs(ids.taskId);
    DOCTEST_REQUIRE(firstLogs.isOk());
    DOCTEST_CHECK_EQ(firstLogs.value().size(), 2u);
    auto otherLogs = store.listLogs(otherId.value());
    DOCTEST_REQUIRE(otherLogs.isOk());
    DOCTEST_REQUIRE_EQ(otherLogs.value().size(), 1u);
    DOCTEST_CHECK_EQ(otherLogs.value()[0].at, 50);
    return true;
})

REGISTER_TEST(release_store_tasks_paging,
              "release_store: listTasksPage 按 planDate 降序分页（首 10 / offset20 尾 5 / 越界空），"
              "countTasks 与插入数一致"
              "（防一次读全表内存切片、分页边界错位、总数与分页口径漂移）", {
    test_support::TestDb tdb("releasestore", "page");
    rel::ReleaseStore store(tdb.handle());

    // 造 25 条：日期 2026-12-01 ~ 2026-12-25，按日期升序插入。
    // 首屏若不是 12-25 起，说明排序退化成了插入序 / id 序。
    for (int day = 1; day <= 25; ++day) {
        rel::Task task;
        task.planDate = "2026-12-" + (day < 10 ? std::string("0") : std::string()) +
                        std::to_string(day);
        DOCTEST_REQUIRE(store.createTask(task).isOk());
    }

    auto total = store.countTasks();
    DOCTEST_REQUIRE(total.isOk());
    DOCTEST_CHECK_EQ(total.value(), 25);

    auto first = store.listTasksPage(0, 10);
    DOCTEST_REQUIRE(first.isOk());
    DOCTEST_REQUIRE_EQ(first.value().size(), 10u);
    DOCTEST_CHECK_EQ(first.value().front().planDate, std::string("2026-12-25"));
    DOCTEST_CHECK_EQ(first.value().back().planDate, std::string("2026-12-16"));

    // 尾页：offset=20 只剩 5 条
    auto tail = store.listTasksPage(20, 10);
    DOCTEST_REQUIRE(tail.isOk());
    DOCTEST_REQUIRE_EQ(tail.value().size(), 5u);
    DOCTEST_CHECK_EQ(tail.value().front().planDate, std::string("2026-12-05"));
    DOCTEST_CHECK_EQ(tail.value().back().planDate, std::string("2026-12-01"));

    // 跨页拼起来不重不漏（防排序/边界错位造成翻页重复或丢条）
    std::set<std::int64_t> ids;
    for (int offset = 0; offset < 30; offset += 10) {
        auto page = store.listTasksPage(offset, 10);
        DOCTEST_REQUIRE(page.isOk());
        for (const auto& task : page.value()) ids.insert(task.id);
    }
    DOCTEST_CHECK_EQ(ids.size(), 25u);

    // 越界 offset 返回空（不是最后一条 / 报错）
    auto beyond = store.listTasksPage(30, 10);
    DOCTEST_REQUIRE(beyond.isOk());
    DOCTEST_CHECK(beyond.value().empty());
    auto exactEnd = store.listTasksPage(25, 10);
    DOCTEST_REQUIRE(exactEnd.isOk());
    DOCTEST_CHECK(exactEnd.value().empty());

    // 负参数必须拒绝：SQLite 的 LIMIT -1 是「不设上限」，接受会退化成一次读全表
    DOCTEST_CHECK(store.listTasksPage(-1, 10).isFail());
    DOCTEST_CHECK(store.listTasksPage(0, -1).isFail());
    // limit=0 是合法空页（调用方算出的可见数为 0 时不应报错）
    auto emptyPage = store.listTasksPage(0, 0);
    DOCTEST_REQUIRE(emptyPage.isOk());
    DOCTEST_CHECK(emptyPage.value().empty());
    return true;
})

REGISTER_TEST(release_store_tasks_paging_includes_voided,
              "release_store: countTasks/listTasksPage 计入已作废任务（列表页展示作废卡），"
              "与 findTaskByDate 排除 voided 的口径分开"
              "（防作废任务被列表吞掉导致总数少算、尾页缺条）", {
    test_support::TestDb tdb("releasestore", "voidpage");
    rel::ReleaseStore store(tdb.handle());

    rel::Task valid;
    valid.planDate = "2026-12-10";
    valid.status = rel::TaskStatus::Pending;
    auto validId = store.createTask(valid);
    DOCTEST_REQUIRE(validId.isOk());

    // 同一天再建一个作废任务（findTaskByDate 允许，作废不占当天名额）
    rel::Task voided;
    voided.planDate = "2026-12-10";
    voided.status = rel::TaskStatus::Voided;
    auto voidedId = store.createTask(voided);
    DOCTEST_REQUIRE(voidedId.isOk());

    rel::Task older;
    older.planDate = "2026-12-09";
    DOCTEST_REQUIRE(store.createTask(older).isOk());

    // 列表口径：作废任务仍展示（「已作废」chip），故总数与分页都算它
    auto total = store.countTasks();
    DOCTEST_REQUIRE(total.isOk());
    DOCTEST_CHECK_EQ(total.value(), 3);

    auto all = store.listTasksPage(0, 10);
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), 3u);
    // 同一天按 id DESC：后建的作废任务排在前面，且确实出现在列表里
    DOCTEST_CHECK_EQ(all.value()[0].id, voidedId.value());
    DOCTEST_CHECK(all.value()[0].status == rel::TaskStatus::Voided);

    // 查找口径不变：同一天命中的仍是有效任务（作废不占当天名额）
    auto hit = store.findTaskByDate("2026-12-10");
    DOCTEST_REQUIRE(hit.isOk());
    DOCTEST_REQUIRE(hit.value().has_value());
    DOCTEST_CHECK_EQ(hit.value()->id, validId.value());
    DOCTEST_CHECK(hit.value()->status != rel::TaskStatus::Voided);

    // 尾页边界按含作废的总数切：offset=2 取最后 1 条
    auto tail = store.listTasksPage(2, 1);
    DOCTEST_REQUIRE(tail.isOk());
    DOCTEST_REQUIRE_EQ(tail.value().size(), 1u);
    DOCTEST_CHECK_EQ(tail.value()[0].planDate, std::string("2026-12-09"));
    return true;
})

REGISTER_TEST(release_store_create_task_with_envs_atomic,
              "release_store: createTaskWithEnvs 一次落库任务 + N 环境 + 每环境 M 服务，"
              "字段与入参快照一致、taskId 一律绑定新任务"
              "（防原子接口漏写某个环境/服务、快照字段与基础配置脱钩）", {
    test_support::TestDb tdb("releasestore", "atomic");
    rel::ReleaseStore store(tdb.handle());

    rel::Task task;
    task.planDate = "2026-11-20";
    task.status = rel::TaskStatus::Draft;

    rel::TaskEnv em;
    em.envName = "EM";
    em.startMinute = 540;
    em.endMinute = 600;
    rel::TaskService api;
    api.serviceName = "API";
    api.kind = rel::ServiceKind::Backend;
    api.defaultBranch = "main";
    api.branch = "release/1";
    rel::TaskService web;
    web.serviceName = "Web";
    web.kind = rel::ServiceKind::PcWeb;
    web.defaultBranch = "main";

    rel::TaskEnv eu;
    eu.envName = "EU";
    eu.startMinute = 0;
    eu.endMinute = 120;
    rel::TaskService gw;
    gw.serviceName = "GW";
    gw.kind = rel::ServiceKind::Backend;
    gw.defaultBranch = "main";
    gw.branch = "main";

    auto created = store.createTaskWithEnvs(task, {em, eu}, {{api, web}, {gw}});
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_CHECK(created.value() > 0);

    // 任务行落库：同日可查、状态 Draft、总数 +1
    auto hit = store.findTaskByDate("2026-11-20");
    DOCTEST_REQUIRE(hit.isOk());
    DOCTEST_REQUIRE(hit.value().has_value());
    DOCTEST_CHECK_EQ(hit.value()->id, created.value());
    DOCTEST_CHECK(hit.value()->status == rel::TaskStatus::Draft);
    auto total = store.countTasks();
    DOCTEST_REQUIRE(total.isOk());
    DOCTEST_CHECK_EQ(total.value(), 1);

    // 两个环境快照按写入序落库，名称/时间窗与入参一致
    auto envs = store.listTaskEnvs(created.value());
    DOCTEST_REQUIRE(envs.isOk());
    DOCTEST_REQUIRE_EQ(envs.value().size(), 2u);
    DOCTEST_CHECK_EQ(envs.value()[0].envName, std::string("EM"));
    DOCTEST_CHECK_EQ(envs.value()[0].taskId, created.value());
    DOCTEST_CHECK_EQ(envs.value()[0].startMinute, 540);
    DOCTEST_CHECK_EQ(envs.value()[0].endMinute, 600);
    DOCTEST_CHECK(!envs.value()[0].removed);
    DOCTEST_CHECK_EQ(envs.value()[1].envName, std::string("EU"));
    DOCTEST_CHECK_EQ(envs.value()[1].startMinute, 0);
    DOCTEST_CHECK_EQ(envs.value()[1].endMinute, 120);

    // 每环境服务数量与字段：第一个环境 2 个、第二个环境 1 个，无串环境
    auto emSvcs = store.listTaskServices(envs.value()[0].id);
    DOCTEST_REQUIRE(emSvcs.isOk());
    DOCTEST_REQUIRE_EQ(emSvcs.value().size(), 2u);
    DOCTEST_CHECK_EQ(emSvcs.value()[0].serviceName, std::string("API"));
    DOCTEST_CHECK_EQ(emSvcs.value()[0].branch, std::string("release/1"));
    DOCTEST_CHECK_EQ(emSvcs.value()[0].defaultBranch, std::string("main"));
    DOCTEST_CHECK_EQ(emSvcs.value()[1].serviceName, std::string("Web"));
    DOCTEST_CHECK(emSvcs.value()[1].kind == rel::ServiceKind::PcWeb);
    auto euSvcs = store.listTaskServices(envs.value()[1].id);
    DOCTEST_REQUIRE(euSvcs.isOk());
    DOCTEST_REQUIRE_EQ(euSvcs.value().size(), 1u);
    DOCTEST_CHECK_EQ(euSvcs.value()[0].serviceName, std::string("GW"));

    // 等长校验：环境数与服务清单数不一致必须事务前拒绝，不产生任何写入
    DOCTEST_CHECK(store.createTaskWithEnvs(task, {em}, {}).isFail());
    total = store.countTasks();
    DOCTEST_REQUIRE(total.isOk());
    DOCTEST_CHECK_EQ(total.value(), 1);
    return true;
})

REGISTER_TEST(release_store_create_task_with_envs_rollback,
              "release_store: createTaskWithEnvs 任一环境写失败整体回滚，任务/已写环境全不留，"
              "回滚后同计划日可直接重试"
              "（防残留 Draft 死单污染列表、挡住同日重试）", {
    test_support::TestDb tdb("releasestore", "atomicrb");
    rel::ReleaseStore store(tdb.handle());

    // 稳定失败注入：临时测试库上建触发器，第二个环境快照写入时 RAISE(ABORT)。
    // 只建在测试库、不碰生产 schema；触发器名在独立临时库内无碰撞问题。
    auto trig = tdb.handle().exec(
        "CREATE TRIGGER inject_env_fail "
        "BEFORE INSERT ON release_task_env "
        "WHEN NEW.env_name = 'BOOM' "
        "BEGIN SELECT RAISE(ABORT, 'injected env failure'); END");
    DOCTEST_REQUIRE(trig.isOk());

    rel::Task task;
    task.planDate = "2026-11-21";

    rel::TaskEnv em;
    em.envName = "EM";
    rel::TaskService api;
    api.serviceName = "API";
    api.kind = rel::ServiceKind::Backend;

    rel::TaskEnv boom;
    boom.envName = "BOOM";  // 第二个环境：触发注入失败
    rel::TaskService gw;
    gw.serviceName = "GW";
    gw.kind = rel::ServiceKind::Backend;

    auto before = store.countTasks();
    DOCTEST_REQUIRE(before.isOk());
    DOCTEST_CHECK_EQ(before.value(), 0);

    // 第一个环境成功、第二个失败：整体必须回滚
    auto created = store.createTaskWithEnvs(task, {em, boom}, {{api}, {gw}});
    DOCTEST_CHECK(created.isFail());

    // 回滚验证：任务、环境、服务三层全部无残留
    auto total = store.countTasks();
    DOCTEST_REQUIRE(total.isOk());
    DOCTEST_CHECK_EQ(total.value(), 0);  // 防「残留 Draft 死单让任务总数多 1」
    auto hit = store.findTaskByDate("2026-11-21");
    DOCTEST_REQUIRE(hit.isOk());
    DOCTEST_CHECK(!hit.value().has_value());  // 防「残留死单挡住同日重试/被误开成旧任务」
    auto tasks = store.listTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK(tasks.value().empty());

    // 撤掉注入后重试：失败后可原样重来，不被任何残留数据挡住
    DOCTEST_REQUIRE(tdb.handle().exec("DROP TRIGGER inject_env_fail").isOk());
    auto retry = store.createTaskWithEnvs(task, {em}, {{api}});
    DOCTEST_REQUIRE(retry.isOk());
    DOCTEST_CHECK(retry.value() > 0);
    auto retryEnvs = store.listTaskEnvs(retry.value());
    DOCTEST_REQUIRE(retryEnvs.isOk());
    DOCTEST_REQUIRE_EQ(retryEnvs.value().size(), 1u);
    auto retrySvcs = store.listTaskServices(retryEnvs.value()[0].id);
    DOCTEST_REQUIRE(retrySvcs.isOk());
    DOCTEST_REQUIRE_EQ(retrySvcs.value().size(), 1u);
    return true;
})
