// tests/test_modules/test_release_logic.cpp
// 上线模块纯逻辑场景单测：发布日判定 / 时间基准 / 到期聚合与分级 / 完整性校验 /
// 环境与任务进度和完成。零 IO、零 DB、零系统时钟——nowMinute 全部由用例传入。
#include "doctest.h"

#include "core/time.h"
#include "module_release/release_logic.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace {

namespace rel = module::release;

rel::TaskEnv makeEnv(std::int64_t id, const std::string& name, int startMinute,
                     int endMinute, bool removed = false) {
    rel::TaskEnv env;
    env.id = id;
    env.taskId = 1;
    env.envName = name;
    env.startMinute = startMinute;
    env.endMinute = endMinute;
    env.removed = removed;
    return env;
}

rel::TaskService makeService(std::int64_t id, std::int64_t taskEnvId, const std::string& name,
                             rel::ServiceKind kind = rel::ServiceKind::Backend,
                             const std::string& branch = "main", bool removed = false) {
    rel::TaskService svc;
    svc.id = id;
    svc.taskEnvId = taskEnvId;
    svc.serviceName = name;
    svc.kind = kind;
    svc.defaultBranch = branch;
    svc.branch = branch;
    svc.removed = removed;
    return svc;
}

rel::TaskConfig makeConfig(std::int64_t id, std::int64_t taskServiceId, rel::ConfigKind kind,
                           const std::string& content, bool skipped = false,
                           std::int64_t doneAt = 0) {
    rel::TaskConfig cfg;
    cfg.id = id;
    cfg.taskServiceId = taskServiceId;
    cfg.kind = kind;
    cfg.content = content;
    cfg.skipped = skipped;
    cfg.doneAt = doneAt;
    return cfg;
}

rel::TaskStep makeStep(std::int64_t taskServiceId, rel::StepKind kind, std::int64_t doneAt) {
    rel::TaskStep step;
    step.taskServiceId = taskServiceId;
    step.kind = kind;
    step.doneAt = doneAt;
    return step;
}

rel::Task makeTask(rel::TaskStatus status) {
    rel::Task task;
    task.id = 1;
    task.planDate = "2026-10-27";
    task.status = status;
    return task;
}

constexpr std::int64_t kDoneAt = 1700000000;

// makeTask 的 planDate 固定为 2026-10-27；这里给出同一计划日零点的 epoch 秒 +
// 分钟偏移（分钟可为负或 > 1440，与 release_logic 内部 nowMinute 同口径）。
// 口径与 release_logic::planDateElapsedMinutes 完全一致：本地 mktime 零点起算，
// std::mktime 按本机时区把 2026-10-27 00:00:00 折算为 epoch。
// 反重构警告：不得改回 daysFromCivil（UTC 口径）——非 UTC 时区下与实现之间的
// 时区偏移会让本文件的跨午夜 / 基线边界用例必红。
std::int64_t planDayNowUnix(int minute) {
    std::tm planMidnight{};
    planMidnight.tm_year = 2026 - 1900;
    planMidnight.tm_mon = 9;    // 10 月
    planMidnight.tm_mday = 27;
    planMidnight.tm_isdst = -1;  // 与 release_logic 内部同款：让 mktime 按当日规则定 DST
    const std::time_t midnight = std::mktime(&planMidnight);
    return static_cast<std::int64_t>(midnight) + static_cast<std::int64_t>(minute) * 60;
}

} // namespace

REGISTER_TEST(release_regular_day_tuesday_thursday,
              "release 发布日: 仅周二/周四，非法日期不误判（防：周三当发布日、无效日期被接受）", {
    DOCTEST_CHECK(rel::isRegularReleaseDay("2026-10-27"));   // 周二
    DOCTEST_CHECK(rel::isRegularReleaseDay("2026-10-29"));   // 周四
    DOCTEST_CHECK(rel::isRegularReleaseDay("2028-02-29"));   // 闰日周二
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026-10-28"));  // 周三
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026-10-24"));  // 周六
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026-10-25"));  // 周日
    // 非法输入一律 false：不存在的日期不能被「归一化」成邻近发布日
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026-02-30"));
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026-13-01"));
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026-1-01"));
    DOCTEST_CHECK(!rel::isRegularReleaseDay("2026/10/27"));
    DOCTEST_CHECK(!rel::isRegularReleaseDay("not-a-date"));
    DOCTEST_CHECK(!rel::isRegularReleaseDay(""));
    return true;
})

REGISTER_TEST(release_baseline_earliest_live_env,
              "release 时间基准: 未移除环境最早开始；无环境 -1（防：把已移除环境算进基准）", {
    const std::vector<rel::TaskEnv> envs{
        makeEnv(1, "已移除", 60, 120, true),
        makeEnv(2, "EU", 540, 600),
        makeEnv(3, "EM", 600, 660)};
    DOCTEST_CHECK_EQ(rel::taskBaseline(envs), static_cast<std::int64_t>(540));

    const std::vector<rel::TaskEnv> allRemoved{makeEnv(1, "EM", 600, 660, true)};
    DOCTEST_CHECK_EQ(rel::taskBaseline(allRemoved), static_cast<std::int64_t>(-1));
    DOCTEST_CHECK_EQ(rel::taskBaseline({}), static_cast<std::int64_t>(-1));
    return true;
})

REGISTER_TEST(release_due_aggregation_window,
              "release 到期聚合: 基准-60 橙、基准 黄、按项收集（防：漏提醒/提前打扰）", {
    const rel::Task task = makeTask(rel::TaskStatus::Pending);
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    const std::vector<rel::TaskService> services;  // 无服务 → 无部署提醒
    const std::vector<rel::TaskConfig> configs;

    DOCTEST_CHECK_EQ(rel::taskBaseline(envs), static_cast<std::int64_t>(600));
    // 基准−60 之前：什么都不欠
    DOCTEST_CHECK(rel::dueItems(task, envs, services, configs, 539).empty());

    // 基准−60 整点：提测 + CAB 各一条橙
    const auto atPrereq = rel::dueItems(task, envs, services, configs, 540);
    DOCTEST_CHECK_EQ(atPrereq.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(atPrereq[0].title, std::string("提测单未通过"));
    DOCTEST_CHECK_EQ(atPrereq[1].title, std::string("CAB 单未通过"));
    DOCTEST_CHECK(atPrereq[0].level == rel::RemindLevel::Orange);
    DOCTEST_CHECK(rel::highestLevel(atPrereq) == rel::RemindLevel::Orange);

    // 基准到：公告转黄，前置仍橙 → 同一窗内取最高级（橙）
    const auto atBaseline = rel::dueItems(task, envs, services, configs, 600);
    DOCTEST_CHECK_EQ(atBaseline.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(atBaseline[2].title, std::string("上线公告未发布"));
    DOCTEST_CHECK(atBaseline[2].level == rel::RemindLevel::Yellow);
    DOCTEST_CHECK(rel::highestLevel(atBaseline) == rel::RemindLevel::Orange);

    // 前置全完成、公告未发 → 只剩一条黄
    rel::Task prereqDone = task;
    prereqDone.pretestOk = true;
    prereqDone.cabOk = true;
    const auto onlyAnnounce = rel::dueItems(prereqDone, envs, services, configs, 600);
    DOCTEST_CHECK_EQ(onlyAnnounce.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(onlyAnnounce[0].level == rel::RemindLevel::Yellow);
    DOCTEST_CHECK(rel::highestLevel(onlyAnnounce) == rel::RemindLevel::Yellow);

    // 公告已发 + 前置完成 → 全部做完就不弹
    rel::Task allDone = prereqDone;
    allDone.announceSentAt = kDoneAt;
    DOCTEST_CHECK(rel::dueItems(allDone, envs, services, configs, 600).empty());
    return true;
})

REGISTER_TEST(release_due_tickets_follow_config_content,
              "release 工单提醒: 只有填了对应内容才欠工单（防：无内容也催工单/跳过项误催）", {
    rel::Task task = makeTask(rel::TaskStatus::Pending);
    task.pretestOk = true;
    task.cabOk = true;
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    const std::vector<rel::TaskService> services{makeService(10, 1, "svc-a")};
    // 空 content / skipped / 只有空内容 → 不欠工单
    std::vector<rel::TaskConfig> configs{
        makeConfig(1, 10, rel::ConfigKind::Nacos, "", false),
        makeConfig(2, 10, rel::ConfigKind::Cron, "", true),
        makeConfig(3, 10, rel::ConfigKind::Db, "update x set y=1", false)};
    auto items = rel::dueItems(task, envs, services, configs, 540);
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(items[0].title, std::string("数据库工单未审批"));

    // 填了 nacos 内容 → 两张工单都欠
    configs[0].content = "nacos.yaml";
    items = rel::dueItems(task, envs, services, configs, 540);
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(items[0].title, std::string("Nacos 工单未审批"));
    DOCTEST_CHECK_EQ(items[1].title, std::string("数据库工单未审批"));

    // 审批完一张 → 只剩另一张
    task.ticketNacosOk = true;
    items = rel::dueItems(task, envs, services, configs, 540);
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(items[0].title, std::string("数据库工单未审批"));

    // 已移除服务的内容不算欠账
    const std::vector<rel::TaskService> removedSvc{
        makeService(10, 1, "svc-a", rel::ServiceKind::Backend, "main", true)};
    DOCTEST_CHECK(rel::dueItems(task, envs, removedSvc, configs, 540).empty());

    // 环境整体被移除 → 无基准，也不提醒
    const std::vector<rel::TaskEnv> removedEnv{makeEnv(1, "EM", 600, 720, true)};
    DOCTEST_CHECK(rel::dueItems(task, removedEnv, services, configs, 540).empty());
    return true;
})

REGISTER_TEST(release_due_deploy_cross_midnight,
              "release 部署提醒: 跨零点环境开始后仍要红（防：00:15 因分钟回绕漏发部署提醒）", {
    rel::Task task = makeTask(rel::TaskStatus::Pending);
    task.pretestOk = true;
    task.cabOk = true;
    task.announceSentAt = kDoneAt;
    // 23:00 开始、次日 00:30 结束（end <= start = 跨零点）
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 1380, 30)};
    const std::vector<rel::TaskService> services{makeService(10, 1, "svc-a")};
    DOCTEST_CHECK_EQ(rel::taskBaseline(envs), static_cast<std::int64_t>(1380));

    // 开始前一分钟：不提醒
    DOCTEST_CHECK(rel::dueItems(task, envs, services, {}, 1379).empty());

    // 次日 00:15（nowMinute = 1440 + 15）：部署仍欠 → 红
    const auto items = rel::dueItems(task, envs, services, {}, 1455);
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(items[0].title, std::string("部署未完成"));
    DOCTEST_CHECK_EQ(items[0].scope, std::string("EM"));
    DOCTEST_CHECK(items[0].level == rel::RemindLevel::Red);
    DOCTEST_CHECK(rel::highestLevel(items) == rel::RemindLevel::Red);
    return true;
})

REGISTER_TEST(release_due_draft_and_final_status,
              "release 到期聚合: 配置中不打扰、过基准红「任务未提交」、Done/Voided 静默", {
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    const rel::Task draft = makeTask(rel::TaskStatus::Draft);

    // 未到基准 → 不弹任何提醒
    DOCTEST_CHECK(rel::dueItems(draft, envs, {}, {}, 500).empty());
    // 基准−60 已到但仍在配置中 → 仍不打扰（Draft 未提交不看前置）
    DOCTEST_CHECK(rel::dueItems(draft, envs, {}, {}, 550).empty());

    // 已过基准：红「任务未提交」追加在末尾，窗色取红
    const auto items = rel::dueItems(draft, envs, {}, {}, 600);
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(4));  // 提测+ CAB + 公告 + 未提交
    DOCTEST_CHECK_EQ(items.back().title, std::string("任务未提交"));
    DOCTEST_CHECK(items.back().level == rel::RemindLevel::Red);
    DOCTEST_CHECK(rel::highestLevel(items) == rel::RemindLevel::Red);

    // Done / Voided：停止提醒（作废不占名额、也不再弹）
    DOCTEST_CHECK(rel::dueItems(makeTask(rel::TaskStatus::Done), envs, {}, {}, 9999).empty());
    DOCTEST_CHECK(rel::dueItems(makeTask(rel::TaskStatus::Voided), envs, {}, {}, 9999).empty());
    return true;
})

REGISTER_TEST(release_highest_level_priority,
              "release 窗色分级: 红 > 橙 > 黄，空集为黄（防：混档窗被低级别覆盖）", {
    DOCTEST_CHECK(rel::highestLevel({}) == rel::RemindLevel::Yellow);
    const std::vector<rel::DueItem> yellowOnly{
        {rel::RemindLevel::Yellow, 1, "task", "a", ""}};
    DOCTEST_CHECK(rel::highestLevel(yellowOnly) == rel::RemindLevel::Yellow);
    const std::vector<rel::DueItem> withOrange{
        {rel::RemindLevel::Yellow, 1, "task", "a", ""},
        {rel::RemindLevel::Orange, 1, "task", "b", ""}};
    DOCTEST_CHECK(rel::highestLevel(withOrange) == rel::RemindLevel::Orange);
    const std::vector<rel::DueItem> withRed{
        {rel::RemindLevel::Orange, 1, "task", "b", ""},
        {rel::RemindLevel::Yellow, 1, "task", "a", ""},
        {rel::RemindLevel::Red, 1, "task", "c", ""}};
    DOCTEST_CHECK(rel::highestLevel(withRed) == rel::RemindLevel::Red);
    return true;
})

REGISTER_TEST(release_validate_missing_items,
              "release 提交校验: 分支空/配置空各报一条，跳过与已移除不参与（防：漏拦或误拦）", {
    const std::vector<rel::TaskService> services{
        makeService(1, 1, "A"),                                      // 完整
        makeService(2, 1, "B", rel::ServiceKind::Backend, ""),       // 分支空
        makeService(3, 1, "C", rel::ServiceKind::Backend, "", true)}; // 已移除 → 不校验
    const std::vector<rel::TaskConfig> configs{
        makeConfig(1, 1, rel::ConfigKind::Nacos, "ok"),
        makeConfig(2, 1, rel::ConfigKind::Db, ""),                   // 未跳过且空 → 缺
        makeConfig(3, 1, rel::ConfigKind::Cron, "", true),           // 跳过 → 不校验
        makeConfig(4, 2, rel::ConfigKind::Menu, ""),                 // 缺
        makeConfig(5, 3, rel::ConfigKind::Gateway, "")};             // 已移除服务 → 不校验
    const auto missing = rel::validateTask(services, configs);
    DOCTEST_CHECK_EQ(missing.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK(missing[0].find("B") != std::string::npos);
    DOCTEST_CHECK(missing[0].find("分支") != std::string::npos);
    DOCTEST_CHECK(missing[1].find("数据库") != std::string::npos);
    DOCTEST_CHECK(missing[2].find("菜单") != std::string::npos);

    // 全部填好 → 通过；跳过项不影响
    const std::vector<rel::TaskService> okServices{makeService(1, 1, "A")};
    const std::vector<rel::TaskConfig> okConfigs{
        makeConfig(1, 1, rel::ConfigKind::Nacos, "x"),
        makeConfig(2, 1, rel::ConfigKind::Db, "", true)};
    DOCTEST_CHECK(rel::validateTask(okServices, okConfigs).empty());
    return true;
})

REGISTER_TEST(release_env_progress_counts,
              "release 环境进度: 3 固定步骤 + 5 期望配置槽位，跳过视为关闭/他环境/已移除不计（防：进度虚高）", {
    const rel::TaskEnv env = makeEnv(1, "EM", 600, 720);
    const std::vector<rel::TaskService> services{
        makeService(10, 1, "svc-a"),
        makeService(11, 2, "svc-b")};  // 属于别的环境
    const std::vector<rel::TaskConfig> configs{
        makeConfig(1, 10, rel::ConfigKind::Nacos, "a", false, kDoneAt),
        makeConfig(2, 10, rel::ConfigKind::Db, "b", false, 0),
        makeConfig(3, 10, rel::ConfigKind::Cron, "", true, 0),   // 跳过 → 槽位关闭计 done
        makeConfig(4, 10, rel::ConfigKind::Menu, "c", false, kDoneAt),
        makeConfig(5, 10, rel::ConfigKind::Gateway, "d", false, kDoneAt),
        makeConfig(6, 11, rel::ConfigKind::Nacos, "e", false, 0)};  // 他环境服务
    const std::vector<rel::TaskStep> steps{
        makeStep(10, rel::StepKind::MergeBranch, kDoneAt),
        makeStep(10, rel::StepKind::Deploy, kDoneAt),
        makeStep(10, rel::StepKind::Config, 0),  // 聚合标记不参与
        makeStep(11, rel::StepKind::Verify, 0)};  // 他环境服务

    rel::Progress progress = rel::envProgress(env, services, configs, steps);
    DOCTEST_CHECK_EQ(progress.total, 8);  // 3 步骤 + 5 期望配置（缺行按未完成占 total）
    DOCTEST_CHECK_EQ(progress.done, 6);   // 合并/部署 + nacos/menu/gateway 完成 + cron 跳过
    DOCTEST_CHECK(!rel::envCompleted(env, services, configs, steps));

    // 补齐验证与数据库配置 → 满进度
    std::vector<rel::TaskStep> fullSteps = steps;
    fullSteps.push_back(makeStep(10, rel::StepKind::Verify, kDoneAt));
    std::vector<rel::TaskConfig> fullConfigs = configs;
    fullConfigs[1].doneAt = kDoneAt;
    progress = rel::envProgress(env, services, fullConfigs, fullSteps);
    DOCTEST_CHECK_EQ(progress.done, progress.total);
    DOCTEST_CHECK(rel::envCompleted(env, services, fullConfigs, fullSteps));
    return true;
})

REGISTER_TEST(release_env_completion_edge_cases,
              "release 环境完成: 服务全移除/环境移除算完成，前端无配置而步骤未做完不算（防：空环境卡死）", {
    // 服务全被移除 → 没服务可做 = 不欠账
    const rel::TaskEnv env = makeEnv(1, "EM", 600, 720);
    const std::vector<rel::TaskService> allRemoved{
        makeService(10, 1, "svc-a", rel::ServiceKind::Backend, "main", true)};
    DOCTEST_CHECK(rel::envCompleted(env, allRemoved, {}, {}));

    // 环境整体被移除 → 不欠账（即使其下服务未做完）
    const rel::TaskEnv removedEnv = makeEnv(2, "EU", 600, 720, true);
    const std::vector<rel::TaskService> liveSvc{makeService(20, 2, "svc-b")};
    DOCTEST_CHECK(rel::envCompleted(removedEnv, liveSvc, {}, {}));

    // 前端服务没有配置项，但三步必须都完成
    const std::vector<rel::TaskService> frontend{
        makeService(30, 1, "web", rel::ServiceKind::PcWeb)};
    const std::vector<rel::TaskStep> partial{
        makeStep(30, rel::StepKind::MergeBranch, kDoneAt),
        makeStep(30, rel::StepKind::Deploy, kDoneAt)};
    DOCTEST_CHECK(!rel::envCompleted(env, frontend, {}, partial));
    const std::vector<rel::TaskStep> full{
        makeStep(30, rel::StepKind::MergeBranch, kDoneAt),
        makeStep(30, rel::StepKind::Deploy, kDoneAt),
        makeStep(30, rel::StepKind::Verify, kDoneAt)};
    DOCTEST_CHECK(rel::envCompleted(env, frontend, {}, full));
    return true;
})

REGISTER_TEST(release_task_progress_and_completion,
              "release 任务完成: 全环境完成 + 提测/CAB/适用工单/公告（防：漏前置也判完成）", {
    rel::Task task = makeTask(rel::TaskStatus::Pending);
    task.pretestOk = true;
    task.cabOk = true;
    task.announceSentAt = kDoneAt;

    const std::vector<rel::TaskEnv> envs{
        makeEnv(1, "EM", 600, 720),
        makeEnv(2, "EU", 660, 780, true)};  // 已移除环境不计
    const std::vector<rel::TaskService> services{
        makeService(10, 1, "web", rel::ServiceKind::PcWeb),
        makeService(20, 2, "svc-removed-env")};  // 属于已移除环境
    const std::vector<rel::TaskStep> steps{
        makeStep(10, rel::StepKind::MergeBranch, kDoneAt),
        makeStep(10, rel::StepKind::Deploy, kDoneAt),
        makeStep(10, rel::StepKind::Verify, kDoneAt)};

    rel::Progress progress = rel::taskProgress(task, envs, services, {}, steps);
    DOCTEST_CHECK_EQ(progress.total, 6);  // 3 步骤 + 提测 + CAB + 公告
    DOCTEST_CHECK_EQ(progress.done, 6);
    DOCTEST_CHECK(rel::taskCompleted(task, envs, services, {}, steps));

    // 公告未发 → 差一项，不算完成
    rel::Task noAnnounce = task;
    noAnnounce.announceSentAt = 0;
    progress = rel::taskProgress(noAnnounce, envs, services, {}, steps);
    DOCTEST_CHECK_EQ(progress.done, 5);
    DOCTEST_CHECK_EQ(progress.total, 6);
    DOCTEST_CHECK(!rel::taskCompleted(noAnnounce, envs, services, {}, steps));

    // 新增后端服务：分母固定 3 步骤 + 5 配置，与库中配置行数无关。
    // 只有 1 行 nacos 内容 → 其余 4 槽位缺行按未完成计，工单同步欠账
    const std::vector<rel::TaskService> withBackend{
        makeService(10, 1, "web", rel::ServiceKind::PcWeb),
        makeService(30, 1, "svc-backend", rel::ServiceKind::Backend)};
    const std::vector<rel::TaskConfig> configs{
        makeConfig(1, 30, rel::ConfigKind::Nacos, "nacos.yaml")};
    progress = rel::taskProgress(noAnnounce, envs, withBackend, configs, steps);
    DOCTEST_CHECK_EQ(progress.total, 15);  // web 3 + backend 8 + 提测/CAB/工单/公告
    DOCTEST_CHECK_EQ(progress.done, 5);    // web 3 步骤 + 提测 + CAB
    DOCTEST_CHECK(!rel::taskCompleted(noAnnounce, envs, withBackend, configs, steps));

    // backend 三步补齐 + 5 配置槽位全部关闭（1 完成 + 4 跳过）+ 工单审批 + 公告 → 满进度
    std::vector<rel::TaskStep> fullSteps = steps;
    fullSteps.push_back(makeStep(30, rel::StepKind::MergeBranch, kDoneAt));
    fullSteps.push_back(makeStep(30, rel::StepKind::Deploy, kDoneAt));
    fullSteps.push_back(makeStep(30, rel::StepKind::Verify, kDoneAt));
    std::vector<rel::TaskConfig> doneConfigs{
        makeConfig(1, 30, rel::ConfigKind::Nacos, "nacos.yaml", false, kDoneAt),
        makeConfig(2, 30, rel::ConfigKind::Db, "", true),
        makeConfig(3, 30, rel::ConfigKind::Cron, "", true),
        makeConfig(4, 30, rel::ConfigKind::Menu, "", true),
        makeConfig(5, 30, rel::ConfigKind::Gateway, "", true)};
    rel::Task ticketOk = task;  // 公告已发
    ticketOk.ticketNacosOk = true;
    progress = rel::taskProgress(ticketOk, envs, withBackend, doneConfigs, fullSteps);
    DOCTEST_CHECK_EQ(progress.done, 15);
    DOCTEST_CHECK_EQ(progress.total, 15);
    DOCTEST_CHECK(rel::taskCompleted(ticketOk, envs, withBackend, doneConfigs, fullSteps));
    return true;
})

REGISTER_TEST(release_due_deploy_steps_done_no_false_red,
              "release 部署提醒: 步骤全部完成后不再报红，撤回一步恢复红（防：环境做完仍假红）", {
    rel::Task task = makeTask(rel::TaskStatus::Pending);
    task.pretestOk = true;
    task.cabOk = true;
    task.announceSentAt = kDoneAt;
    task.ticketNacosOk = true;  // nacos 配置有内容 → 工单已审批，不产生橙条
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    const std::vector<rel::TaskService> services{makeService(10, 1, "svc-a")};
    const std::vector<rel::TaskConfig> configs{
        makeConfig(1, 10, rel::ConfigKind::Nacos, "a", false, kDoneAt),
        makeConfig(2, 10, rel::ConfigKind::Db, "", true),
        makeConfig(3, 10, rel::ConfigKind::Cron, "", true),
        makeConfig(4, 10, rel::ConfigKind::Menu, "", true),
        makeConfig(5, 10, rel::ConfigKind::Gateway, "", true)};
    const std::vector<rel::TaskStep> steps{
        makeStep(10, rel::StepKind::MergeBranch, kDoneAt),
        makeStep(10, rel::StepKind::Deploy, kDoneAt),
        makeStep(10, rel::StepKind::Verify, kDoneAt)};

    // 已过开始时刻、步骤与配置全部关闭 → 不再产出部署条目（防假红）
    DOCTEST_CHECK(rel::dueItems(task, envs, services, configs, steps, 700).empty());

    // 撤回一步（验证 doneAt 归 0）→ 恢复一条红「部署未完成」
    std::vector<rel::TaskStep> undoVerify = steps;
    undoVerify[2].doneAt = 0;
    const auto items = rel::dueItems(task, envs, services, configs, undoVerify, 700);
    DOCTEST_CHECK_EQ(items.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(items[0].title, std::string("部署未完成"));
    DOCTEST_CHECK(items[0].level == rel::RemindLevel::Red);
    DOCTEST_CHECK(items[0].scope == std::string("EM"));
    return true;
})

REGISTER_TEST(release_due_deploy_config_slots,
              "release 部署提醒: 跳过配置不算未完成、缺行算未完成、服务全移除不报（防：跳过项催红/缺项漏红）", {
    rel::Task task = makeTask(rel::TaskStatus::Pending);
    task.pretestOk = true;
    task.cabOk = true;
    task.announceSentAt = kDoneAt;
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    const std::vector<rel::TaskService> services{makeService(10, 1, "svc-a")};
    const std::vector<rel::TaskStep> steps{
        makeStep(10, rel::StepKind::MergeBranch, kDoneAt),
        makeStep(10, rel::StepKind::Deploy, kDoneAt),
        makeStep(10, rel::StepKind::Verify, kDoneAt)};

    // 5 项配置全部跳过（本次没有）→ 配置槽位全部关闭，不欠账 → 无红条
    const std::vector<rel::TaskConfig> allSkipped{
        makeConfig(1, 10, rel::ConfigKind::Nacos, "", true),
        makeConfig(2, 10, rel::ConfigKind::Db, "", true),
        makeConfig(3, 10, rel::ConfigKind::Cron, "", true),
        makeConfig(4, 10, rel::ConfigKind::Menu, "", true),
        makeConfig(5, 10, rel::ConfigKind::Gateway, "", true)};
    DOCTEST_CHECK(rel::dueItems(task, envs, services, allSkipped, steps, 700).empty());

    // 库里没有任何配置行 → 5 槽位全部按未完成计 → 红条
    DOCTEST_CHECK_EQ(rel::dueItems(task, envs, services, {}, steps, 700).size(),
                     static_cast<std::size_t>(1));

    // 服务全部移除 → 环境 0/0 算完成，不产出部署条目
    const std::vector<rel::TaskService> allRemoved{
        makeService(10, 1, "svc-a", rel::ServiceKind::Backend, "main", true)};
    DOCTEST_CHECK(rel::dueItems(task, envs, allRemoved, {}, {}, 700).empty());
    return true;
})

REGISTER_TEST(release_env_progress_fixed_config_denominator,
              "release 进度分母: 后端固定 3+5、前端固定 3，与库中配置行数无关（防：列表/详情分母漂移）", {
    const rel::TaskEnv env = makeEnv(1, "EM", 600, 720);

    // 后端：库里 0 行配置 → 分母仍是 3+5=8，缺行按未完成计
    const std::vector<rel::TaskService> backend{makeService(10, 1, "svc-a")};
    rel::Progress progress = rel::envProgress(env, backend, {}, {});
    DOCTEST_CHECK_EQ(progress.total, 8);
    DOCTEST_CHECK_EQ(progress.done, 0);

    // 库里 2 行配置 → 分母不变；done 只计关闭槽位（nacos 完成 + db 跳过）
    const std::vector<rel::TaskConfig> twoRows{
        makeConfig(1, 10, rel::ConfigKind::Nacos, "a", false, kDoneAt),
        makeConfig(2, 10, rel::ConfigKind::Db, "", true)};
    progress = rel::envProgress(env, backend, twoRows, {});
    DOCTEST_CHECK_EQ(progress.total, 8);
    DOCTEST_CHECK_EQ(progress.done, 2);

    // 前端：无配置分母；即使库里有配置行也不计入
    const std::vector<rel::TaskService> frontend{
        makeService(20, 1, "web", rel::ServiceKind::PcWeb)};
    progress = rel::envProgress(env, frontend, {}, {});
    DOCTEST_CHECK_EQ(progress.total, 3);
    DOCTEST_CHECK_EQ(progress.done, 0);
    const std::vector<rel::TaskConfig> frontendRows{
        makeConfig(3, 20, rel::ConfigKind::Nacos, "x", false, kDoneAt)};
    progress = rel::envProgress(env, frontend, frontendRows, {});
    DOCTEST_CHECK_EQ(progress.total, 3);
    DOCTEST_CHECK_EQ(progress.done, 0);
    return true;
})

REGISTER_TEST(release_status_terminal_passthrough,
              "release 状态推导: Done/Voided 终态透传，不随时间与基准改写"
              "（防：已完成/已作废任务被推导回流成执行中或待执行）", {
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    for (const int minute : {0, 599, 600, 1455}) {
        DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Done), envs,
                                        planDayNowUnix(minute)) ==
                      rel::TaskStatus::Done);
        DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Voided), envs,
                                        planDayNowUnix(minute)) ==
                      rel::TaskStatus::Voided);
    }
    // 无有效环境也不回流
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Done), {},
                                    planDayNowUnix(600)) == rel::TaskStatus::Done);
    return true;
})

REGISTER_TEST(release_status_draft_stays_draft,
              "release 状态推导: Draft 基准前后恒为 Draft，状态栏不显示执行中"
              "（防：过基准的未提交任务被展示成执行中，与库值/提醒口径冲突）", {
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), envs,
                                    planDayNowUnix(0)) == rel::TaskStatus::Draft);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), envs,
                                    planDayNowUnix(599)) == rel::TaskStatus::Draft);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), envs,
                                    planDayNowUnix(600)) == rel::TaskStatus::Draft);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), envs,
                                    planDayNowUnix(601)) == rel::TaskStatus::Draft);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), envs,
                                    planDayNowUnix(1455)) == rel::TaskStatus::Draft);
    // 无有效环境 / 计划日期非法同样恒 Draft（库值本来就是 Draft，推导不得改写）
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), {},
                                    planDayNowUnix(600)) == rel::TaskStatus::Draft);
    rel::Task badDate = makeTask(rel::TaskStatus::Draft);
    badDate.planDate = "2026-02-30";
    DOCTEST_CHECK(rel::deriveStatus(badDate, envs, planDayNowUnix(600)) ==
                  rel::TaskStatus::Draft);
    return true;
})

REGISTER_TEST(release_status_pending_transitions_at_baseline,
              "release 状态推导: Pending 未到基准待执行、到基准执行中（含恰好相等那一刻）"
              "（防：边界时刻被误判或提前/滞后跳变）", {
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), envs,
                                    planDayNowUnix(0)) == rel::TaskStatus::Pending);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), envs,
                                    planDayNowUnix(599)) == rel::TaskStatus::Pending);
    // 恰好等于时间基准的那一刻 → Running（边界语义与 dueItems 的 now >= baseline 一致）
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), envs,
                                    planDayNowUnix(600)) == rel::TaskStatus::Running);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), envs,
                                    planDayNowUnix(601)) == rel::TaskStatus::Running);
    return true;
})

REGISTER_TEST(release_status_running_stays_running,
              "release 状态推导: Running 不回退"
              "（防：执行中任务因时间/跨零点换算被回卷成待执行）", {
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    for (const int minute : {0, 600, 1455}) {
        DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Running), envs,
                                        planDayNowUnix(minute)) ==
                      rel::TaskStatus::Running);
    }
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Running), {},
                                    planDayNowUnix(600)) == rel::TaskStatus::Running);
    return true;
})

REGISTER_TEST(release_status_cross_midnight_after_baseline,
              "release 状态推导: 跨零点环境次日已过基准 → Running，Draft 仍恒 Draft"
              "（防：次日分钟按当日口径换算为负，被误判成基准未过）", {
    // 23:00 开始、次日 00:30 结束（end <= start = 跨零点）。次日 00:15 的累计
    // 分钟 = 1455；若把次日时刻按「当日分钟」口径换算（15 − 1380 为负）会误判
    // 未过基准，deriveStatus 必须用跨零点累计口径。
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 1380, 30)};
    DOCTEST_CHECK_EQ(rel::taskBaseline(envs), static_cast<std::int64_t>(1380));
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), envs,
                                    planDayNowUnix(1379)) == rel::TaskStatus::Pending);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), envs,
                                    planDayNowUnix(1455)) == rel::TaskStatus::Running);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Draft), envs,
                                    planDayNowUnix(1455)) == rel::TaskStatus::Draft);
    return true;
})

REGISTER_TEST(release_status_no_baseline_keeps_stored,
              "release 状态推导: 无有效环境（基准 -1）保持库值"
              "（防：全移除/无环境下被凭空推成执行中）", {
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), {},
                                    planDayNowUnix(600)) == rel::TaskStatus::Pending);
    const std::vector<rel::TaskEnv> allRemoved{makeEnv(1, "EM", 600, 720, true)};
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Pending), allRemoved,
                                    planDayNowUnix(600)) == rel::TaskStatus::Pending);
    DOCTEST_CHECK(rel::deriveStatus(makeTask(rel::TaskStatus::Running), allRemoved,
                                    planDayNowUnix(600)) == rel::TaskStatus::Running);
    return true;
})

REGISTER_TEST(release_status_invalid_plan_date_keeps_stored,
              "release 状态推导: 计划日期非法保持库值"
              "（防：坏日期被当成已过基准推成执行中）", {
    const std::vector<rel::TaskEnv> envs{makeEnv(1, "EM", 600, 720)};
    rel::Task badDate = makeTask(rel::TaskStatus::Pending);
    badDate.planDate = "not-a-date";
    DOCTEST_CHECK(rel::deriveStatus(badDate, envs, planDayNowUnix(600)) ==
                  rel::TaskStatus::Pending);
    badDate.planDate = "2026-02-30";
    DOCTEST_CHECK(rel::deriveStatus(badDate, envs, planDayNowUnix(600)) ==
                  rel::TaskStatus::Pending);
    return true;
})