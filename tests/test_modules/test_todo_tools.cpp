// tests/test_modules/test_todo_tools.cpp
// TodoTools 六个工具：参数 → Result/ToolResult 语义、真实落库副作用、注册表插拔。
// 跨文件前提：
//   - 错误回喂约定：参数非法 / 目标不存在 → execute 返回 Result ok +
//     ToolResult.isError=true + content 含中文说明，让 LLM 读到后自我纠正。
//     Result::fail 只留给编程不变量被破坏。
//   - isDestructive 的消费方在 AgentLoop：allowDestructive=0 时它拒绝执行。
//   - 工具注册在进程级 ToolRegistry（core::ToolRegistry::instance()），不是按模块实例存的。
#include "doctest.h"
#include "test_support.h"

#include "core/mcp_tool.h"
#include "core/event_bus.h"
#include "core/module_interface.h"
#include "core/tool.h"
#include "module_mcp/mcp_tool_mirror.h"
#include "module_todo/todo_service.h"
#include "module_todo/todo_tools.h"
#include "platform/json.h"

#include <memory>
#include <string>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_todotools_";

// 一次性环境：TestEnv(registerAppDb) + 真实 TodoService + TodoTools。
// 注册表清理由 ~ToolEnv 做（RAII）而不是用例末尾手动 unregister：断言 fail-fast
// 早退也会走析构，否则工具残留在进程级注册表里串到后续用例。
// 析构顺序不可换：~ToolEnv 体先 unregisterAll → tools 析构 → svc 析构 →
// 基类 TestEnv 析构（注销 appDatabase + 删库文件）。
struct ToolEnv : test_support::TestEnv {
    std::unique_ptr<module::todo::TodoService> svc;
    std::unique_ptr<module::todo::TodoTools> tools;

    explicit ToolEnv(const std::string& tag)
        : test_support::TestEnv(kDbPrefix, tag, /*registerAppDb=*/true) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
        tools = std::make_unique<module::todo::TodoTools>(*svc);
    }
    ~ToolEnv() { tools->unregisterAll(); }
};

// 从进程级注册表按名取工具并执行（与 AgentLoop 相同的 find → execute 路径）
core::Result<core::ToolResult> runTool(const std::string& name,
                                       const std::string& argsJson) {
    auto found = core::ToolRegistry::instance().find(name);
    if (found.isFail()) {
        return core::Result<core::ToolResult>::fail(found.errorCode(),
                                                    found.errorMessage());
    }
    return found.value()->execute(core::ToolArgs{argsJson});
}

// 种 n 个任务到第一列（经真实 TodoService 落库）；失败返回 false 供断言
bool seedTasks(module::todo::TodoService& svc, int n, const std::string& base) {
    auto cols = svc.listColumns();
    if (cols.isFail() || cols.value().empty()) return false;
    for (int i = 0; i < n; ++i) {
        if (svc.createTask(cols.value().front().id,
                           base + std::to_string(i + 1)).isFail()) {
            return false;
        }
    }
    return true;
}

} // namespace


extern "C" ::core::Module* create_todo_module();

REGISTER_TEST(todo_module_onstart_reentry_idempotent,
              "todo_module: onStart→onStop→onStart 重入 + 双 onStart/双 onStop no-op", {
    test_support::TestEnv env(kDbPrefix, "modreentry", /*registerAppDb=*/true);
    // 起点干净：本进程里可能已有别的用例注册过同名工具
    core::ToolRegistry::instance().unregisterTool("todo.list");
    std::unique_ptr<::core::Module> mod(create_todo_module());

    mod->onStart();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("todo.list").isOk());
    mod->onStart();   // 双 onStart：no-op，不重建 service_、不重复注册
    DOCTEST_CHECK(core::ToolRegistry::instance().find("todo.list").isOk());
    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("todo.list").isFail());
    mod->onStop();    // 双 onStop：no-op
    mod->onStart();   // off→on 循环后工具要恢复
    DOCTEST_CHECK(core::ToolRegistry::instance().find("todo.list").isOk());
    mod->onStop();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("todo.list").isFail());
    return true;
})


REGISTER_TEST(todo_tools_list_empty_then_seeded,
              "todo.list: 空库 {\"tasks\":[]} → 种 3 条返回标题 + completed 过滤", {
    ToolEnv env("list");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    // 空库返回 {"tasks":[]} 而不是报错
    auto empty = runTool("todo.list", "{}");
    DOCTEST_REQUIRE(empty.isOk());
    DOCTEST_CHECK_FALSE(empty.value().isError);
    DOCTEST_CHECK_EQ(empty.value().contentJson, std::string("{\"tasks\":[]}"));

    DOCTEST_REQUIRE(seedTasks(*env.svc, 3, "任务"));
    auto r = runTool("todo.list", "{}");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto tasks = parsed.value().asObject().get("tasks");
    DOCTEST_REQUIRE(tasks.isArray());
    DOCTEST_CHECK_EQ(tasks.size(), 3);
    DOCTEST_CHECK(r.value().contentJson.find("任务1") != std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("任务3") != std::string::npos);

    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE(env.svc->completeTask(all.value()[0].id).isOk());
    // completed=true 只返回已完成的那批
    auto doneOnly = runTool("todo.list", R"({"completed":true})");
    DOCTEST_REQUIRE(doneOnly.isOk());
    auto doneParsed = platform::json::parse(doneOnly.value().contentJson);
    DOCTEST_REQUIRE(doneParsed.isOk());
    DOCTEST_CHECK_EQ(doneParsed.value().asObject().get("tasks").size(), 1);
    return true;
})


// todo.list 的 overdue 过滤：口径与 Todo 页逾期弹窗必须一致（直接复用
// TodoService::listOverdue），因此用例同时锁工具层结果与服务层实际行为。
// 边界：用远期/过去 epoch 常数，不依赖真实时钟（服务层无时钟注入点）。
REGISTER_TEST(todo_tools_list_overdue_filter,
              "todo.list: overdue=true 只返回逾期未完成；未来/已完成/无截止不命中；completed 冲突报错", {
    ToolEnv env("list_overdue");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(!cols.value().empty());
    const int64_t colId = cols.value().front().id;
    const std::string colKind = cols.value().front().kind;
    DOCTEST_CHECK_MESSAGE(colKind == "todo", ("colKind=" + colKind).c_str());

    // dueAt 是 Unix 秒：past = 2001 年，future = 2286 年
    const int64_t past = 1000000000;
    const int64_t future = 9999999999;

    module::todo::NewTaskAttributes pastDue;
    pastDue.dueAt = past;

    auto t1 = env.svc->createTask(colId, "过期未完成", pastDue);
    DOCTEST_REQUIRE(t1.isOk());

    // 未来截止 → 不命中
    auto t2 = env.svc->createTask(colId, "未来截止");
    DOCTEST_REQUIRE(t2.isOk());
    module::todo::Task u2 = t2.value();
    u2.dueAt = future;
    DOCTEST_REQUIRE(env.svc->updateTask(u2).isOk());

    // 已完成但已过期 → 不命中
    auto t3 = env.svc->createTask(colId, "过期已完成", pastDue);
    DOCTEST_REQUIRE(t3.isOk());
    DOCTEST_REQUIRE(env.svc->completeTask(t3.value().id).isOk());

    // 无 dueAt → 不命中
    auto t4 = env.svc->createTask(colId, "无截止");
    DOCTEST_REQUIRE(t4.isOk());

    auto od = env.svc->listOverdue();
    DOCTEST_CHECK_MESSAGE(od.isOk() && od.value().size() == 1,
        ("overdueCount=" + std::to_string(od.isOk() ? od.value().size() : 999) +
         " colKind=" + colKind).c_str());

    auto r = runTool("todo.list", R"({"overdue":true})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    auto tasks = parsed.value().asObject().get("tasks");
    DOCTEST_REQUIRE(tasks.isArray());
    DOCTEST_CHECK_EQ(tasks.size(), 1);
    DOCTEST_CHECK(r.value().contentJson.find("过期未完成") != std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("未来截止") == std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("过期已完成") == std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("无截止") == std::string::npos);
    // 出参形状不变：dueAt 仍是原始 epoch，completedAt 仍是 null
    DOCTEST_CHECK(r.value().contentJson.find("\"dueAt\":1000000000") !=
                  std::string::npos);
    DOCTEST_CHECK(r.value().contentJson.find("\"completedAt\":null") !=
                  std::string::npos);

    // 缺省 / overdue=false → 与旧行为完全一致（4 条全返回，含逾期与非逾期）
    auto all = runTool("todo.list", "{}");
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_CHECK_FALSE(all.value().isError);
    auto allParsed = platform::json::parse(all.value().contentJson);
    DOCTEST_REQUIRE(allParsed.isOk());
    DOCTEST_CHECK_EQ(allParsed.value().asObject().get("tasks").size(), 4);

    auto explicitFalse = runTool("todo.list", R"({"overdue":false})");
    DOCTEST_REQUIRE(explicitFalse.isOk());
    DOCTEST_CHECK_FALSE(explicitFalse.value().isError);
    DOCTEST_CHECK_EQ(explicitFalse.value().contentJson, all.value().contentJson);

    // completed=false + overdue=true 允许：逾期集合本身即未完成
    auto incompleteOnly = runTool("todo.list", R"({"completed":false,"overdue":true})");
    DOCTEST_REQUIRE(incompleteOnly.isOk());
    DOCTEST_CHECK_FALSE(incompleteOnly.value().isError);
    DOCTEST_CHECK_EQ(incompleteOnly.value().contentJson, r.value().contentJson);

    // completed=true + overdue=true 是参数冲突 → 工具错误，不是空列表
    auto conflict = runTool("todo.list", R"({"completed":true,"overdue":true})");
    DOCTEST_REQUIRE(conflict.isOk());
    DOCTEST_CHECK(conflict.value().isError);
    DOCTEST_CHECK(conflict.value().contentJson.find("冲突") != std::string::npos);
    DOCTEST_CHECK(conflict.value().contentJson.find("逾期") != std::string::npos);

    // 类型错误同样回喂中文说明
    auto badType = runTool("todo.list", R"({"overdue":"yes"})");
    DOCTEST_REQUIRE(badType.isOk());
    DOCTEST_CHECK(badType.value().isError);
    DOCTEST_CHECK(badType.value().contentJson.find("overdue") != std::string::npos);

    // H21 收窄：逾期口径只认 kind=todo/doing 的列；归档列不再计入（与服务层唯一真源
    // core/overdue.h 一致）。这里锁实际行为：归档逾期任务不出现在 listOverdue / 工具结果。
    int64_t archiveColId = -1;
    for (const auto& c : cols.value()) {
        if (c.kind == "archive") archiveColId = c.id;
    }
    DOCTEST_REQUIRE(archiveColId >= 0);
    auto archived = env.svc->createTask(colId, "归档逾期", pastDue);
    DOCTEST_REQUIRE(archived.isOk());
    DOCTEST_REQUIRE(
        env.svc->moveTask(archived.value().id, archiveColId, 0).isOk());

    auto svcOverdue = env.svc->listOverdue();
    DOCTEST_REQUIRE(svcOverdue.isOk());
    DOCTEST_CHECK_EQ(svcOverdue.value().size(), 1u);  // 仅 todo/doing 列

    auto withArchived = runTool("todo.list", R"({"overdue":true})");
    DOCTEST_REQUIRE(withArchived.isOk());
    DOCTEST_CHECK_FALSE(withArchived.value().isError);
    auto archivedParsed = platform::json::parse(withArchived.value().contentJson);
    DOCTEST_REQUIRE(archivedParsed.isOk());
    DOCTEST_CHECK_EQ(archivedParsed.value().asObject().get("tasks").size(), 1);
    DOCTEST_CHECK(withArchived.value().contentJson.find("归档逾期") ==
                  std::string::npos);

    // schema 宣传新参数
    auto found = core::ToolRegistry::instance().find("todo.list");
    DOCTEST_REQUIRE(found.isOk());
    const std::string schema = found.value()->schemaJson();
    DOCTEST_CHECK(schema.find("\"overdue\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("互斥") != std::string::npos);
    return true;
})


// B-4：todo.list 的列 / 关键词 / limit 过滤 + 出参归档/文档/创建时间。
// 默认口径不含归档列：归档任务只在显式 columnId=归档列时返回。
REGISTER_TEST(todo_tools_list_filters_fields_and_default_archive_exclusion,
              "todo.list: 默认排除归档列 + columnId/keyword/limit + archivedAt/docPath/createdAt", {
    ToolEnv env("list_filters");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int64_t todoColId = 0;
    int64_t archiveColId = 0;
    for (const auto& c : cols.value()) {
        if (c.kind == "todo") todoColId = c.id;
        if (c.kind == "archive") archiveColId = c.id;
    }
    DOCTEST_REQUIRE(todoColId > 0);
    DOCTEST_REQUIRE(archiveColId > 0);

    module::todo::NewTaskAttributes attrs;
    attrs.description = "季度 summary";
    auto a1 = env.svc->createTask(todoColId, "Alpha 报告", attrs);
    DOCTEST_REQUIRE(a1.isOk());
    auto a1Full = a1.value();
    a1Full.docPath = "notes/alpha.md";
    DOCTEST_REQUIRE(env.svc->updateTask(a1Full).isOk());

    auto a2 = env.svc->createTask(todoColId, "beta 杂项");
    DOCTEST_REQUIRE(a2.isOk());
    auto arch = env.svc->createTask(todoColId, "旧归档");
    DOCTEST_REQUIRE(arch.isOk());
    DOCTEST_REQUIRE(
        env.svc->moveTask(arch.value().id, archiveColId, 0).isOk());

    // 默认：两条未归档；归档卡只在显式指定归档列时出现
    auto def = runTool("todo.list", "{}");
    DOCTEST_REQUIRE(def.isOk());
    DOCTEST_CHECK_FALSE(def.value().isError);
    auto defParsed = platform::json::parse(def.value().contentJson);
    DOCTEST_REQUIRE(defParsed.isOk());
    DOCTEST_CHECK_EQ(defParsed.value().asObject().get("tasks").size(), 2);
    DOCTEST_CHECK(def.value().contentJson.find("旧归档") == std::string::npos);

    auto archivedOnly = runTool(
        "todo.list", "{\"columnId\":" + std::to_string(archiveColId) + "}");
    DOCTEST_REQUIRE(archivedOnly.isOk());
    DOCTEST_CHECK_FALSE(archivedOnly.value().isError);
    auto archParsed = platform::json::parse(archivedOnly.value().contentJson);
    DOCTEST_REQUIRE(archParsed.isOk());
    DOCTEST_CHECK_EQ(archParsed.value().asObject().get("tasks").size(), 1);
    DOCTEST_CHECK(archivedOnly.value().contentJson.find("旧归档") !=
                  std::string::npos);
    // 归档时间有值（数字，不是 null）
    DOCTEST_CHECK(archivedOnly.value().contentJson.find("\"archivedAt\":null") ==
                  std::string::npos);
    DOCTEST_CHECK(archivedOnly.value().contentJson.find("\"archivedAt\":") !=
                  std::string::npos);

    // keyword 命中描述（大小写不敏感只对 ASCII 有意义，中文按字节匹配）
    auto byDesc = runTool("todo.list", R"({"keyword":"SUMMARY"})");
    DOCTEST_REQUIRE(byDesc.isOk());
    auto byDescParsed = platform::json::parse(byDesc.value().contentJson);
    DOCTEST_REQUIRE(byDescParsed.isOk());
    DOCTEST_CHECK_EQ(byDescParsed.value().asObject().get("tasks").size(), 1);
    DOCTEST_CHECK(byDesc.value().contentJson.find("Alpha 报告") !=
                  std::string::npos);

    auto byTitle = runTool("todo.list", R"({"keyword":"alpha"})");
    DOCTEST_REQUIRE(byTitle.isOk());
    auto byTitleParsed = platform::json::parse(byTitle.value().contentJson);
    DOCTEST_REQUIRE(byTitleParsed.isOk());
    DOCTEST_CHECK_EQ(byTitleParsed.value().asObject().get("tasks").size(), 1);
    // 出参补字段：关联文档 / 创建时间（有值）/ 归档时间（未归档 = null）
    DOCTEST_CHECK(byTitle.value().contentJson.find("\"docPath\":\"notes/alpha.md\"") !=
                  std::string::npos);
    DOCTEST_CHECK(byTitle.value().contentJson.find("\"createdAt\":") !=
                  std::string::npos);
    DOCTEST_CHECK(byTitle.value().contentJson.find("\"createdAt\":null") ==
                  std::string::npos);
    DOCTEST_CHECK(byTitle.value().contentJson.find("\"archivedAt\":null") !=
                  std::string::npos);

    // limit 钳 1..200：0 → 1 条；1 → 1 条
    auto limited = runTool("todo.list", R"({"limit":0})");
    DOCTEST_REQUIRE(limited.isOk());
    auto limitedParsed = platform::json::parse(limited.value().contentJson);
    DOCTEST_REQUIRE(limitedParsed.isOk());
    DOCTEST_CHECK_EQ(limitedParsed.value().asObject().get("tasks").size(), 1);

    // 参数类型 / 幽灵列错误回喂
    auto badLimit = runTool("todo.list", R"({"limit":"5"})");
    DOCTEST_REQUIRE(badLimit.isOk());
    DOCTEST_CHECK(badLimit.value().isError);
    auto ghostCol = runTool("todo.list", R"({"columnId":999999})");
    DOCTEST_REQUIRE(ghostCol.isOk());
    DOCTEST_CHECK(ghostCol.value().isError);
    DOCTEST_CHECK(ghostCol.value().contentJson.find("列不存在") !=
                  std::string::npos);

    // schema 宣传新参数
    auto found = core::ToolRegistry::instance().find("todo.list");
    DOCTEST_REQUIRE(found.isOk());
    const std::string schema = found.value()->schemaJson();
    DOCTEST_CHECK(schema.find("\"columnId\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("\"keyword\"") != std::string::npos);
    DOCTEST_CHECK(schema.find("\"limit\"") != std::string::npos);
    return true;
})

REGISTER_TEST(todo_tools_list_limit_clamped,
              "todo.list: limit 缺省 50、钳 1..200（超量数据）", {
    ToolEnv env("list_limit");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    DOCTEST_REQUIRE(seedTasks(*env.svc, 205, "批量"));

    auto def = runTool("todo.list", "{}");
    DOCTEST_REQUIRE(def.isOk());
    auto defParsed = platform::json::parse(def.value().contentJson);
    DOCTEST_REQUIRE(defParsed.isOk());
    DOCTEST_CHECK_EQ(defParsed.value().asObject().get("tasks").size(), 50);

    auto maxLimit = runTool("todo.list", R"({"limit":9999})");
    DOCTEST_REQUIRE(maxLimit.isOk());
    auto maxParsed = platform::json::parse(maxLimit.value().contentJson);
    DOCTEST_REQUIRE(maxParsed.isOk());
    DOCTEST_CHECK_EQ(maxParsed.value().asObject().get("tasks").size(), 200);

    auto explicit200 = runTool("todo.list", R"({"limit":200})");
    DOCTEST_REQUIRE(explicit200.isOk());
    auto e200Parsed = platform::json::parse(explicit200.value().contentJson);
    DOCTEST_REQUIRE(e200Parsed.isOk());
    DOCTEST_CHECK_EQ(e200Parsed.value().asObject().get("tasks").size(), 200);
    return true;
})

REGISTER_TEST(todo_tools_create_default_first_column,
              "todo.create: 缺省列=待办列（kind='todo'），service 真实落库 + 返回新 id", {
    ToolEnv env("create");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_CHECK_FALSE(cols.value().empty());

    auto r = runTool("todo.create", R"({"title":"x"})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), 1u);
    DOCTEST_CHECK_EQ(all.value()[0].title, std::string("x"));
    DOCTEST_CHECK_EQ(all.value()[0].columnId, cols.value()[0].id);

    // 返回体带新任务 id，LLM 后续操作要靠它
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK_EQ(parsed.value().asObject().get("id").asInt(),
                     all.value()[0].id);

    // 显式 columnId → 落到指定列
    const std::string args2 = "{\"title\":\"y\",\"columnId\":" +
                              std::to_string(cols.value()[1].id) + "}";
    auto r2 = runTool("todo.create", args2);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_FALSE(r2.value().isError);
    auto inCol2 = env.svc->listTasks(cols.value()[1].id);
    DOCTEST_REQUIRE(inCol2.isOk());
    DOCTEST_REQUIRE_EQ(inCol2.value().size(), 1u);
    DOCTEST_CHECK_EQ(inCol2.value()[0].title, std::string("y"));

    // 不存在的列 → isError，不静默造悬空任务
    auto bad = runTool("todo.create", R"({"title":"z","columnId":999999})");
    DOCTEST_REQUIRE(bad.isOk());
    DOCTEST_CHECK(bad.value().isError);
    return true;
})


// 属性面（票面 01）：description/priority/dueAt 必须能在**一次 todo.create** 里落库，
// 不再需要「先建后 todo.update」二跳 —— 二跳失败会留下「有标题、无属性」的半成品，
// 而用户看到的仍是「任务已创建」的成功语义。
// 断言落在**回读的真实任务**上而不只是工具返回的 JSON：返回体正确但库里没写进去，
// 正是二跳方案最典型的失败形态，只断 JSON 抓不到。
REGISTER_TEST(todo_tools_create_attributes_one_shot,
              "todo.create: attributes 一次调用带 description/priority/dueAt 真落库 + 只给标题对照", {
    ToolEnv env("create_attrs");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    // 三态齐全：一趟调用即可产出带属性的任务
    auto r = runTool("todo.create",
                     R"({"title":"带属性","description":"说明","priority":3,"dueAt":1800000000})");
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_REQUIRE_EQ(all.value().size(), 1u);
    const auto& t = all.value()[0];
    DOCTEST_CHECK_EQ(t.title, std::string("带属性"));
    DOCTEST_REQUIRE(t.description.has_value());
    DOCTEST_CHECK_EQ(t.description.value(), std::string("说明"));
    DOCTEST_CHECK(t.priority == module::todo::Priority::High);
    DOCTEST_REQUIRE(t.dueAt.has_value());
    DOCTEST_CHECK_EQ(t.dueAt.value(), static_cast<int64_t>(1800000000));
    // 工具返回体的 id 必须就是刚落库那条（LLM 靠它继续操作）
    auto parsed = platform::json::parse(r.value().contentJson);
    DOCTEST_REQUIRE(parsed.isOk());
    DOCTEST_CHECK_EQ(parsed.value().asObject().get("id").asInt(), t.id);

    // 对照组：只给 title 仍然建得成，且属性留默认空（新路径不许把最小用法弄坏）
    auto bare = runTool("todo.create", R"({"title":"纯标题"})");
    DOCTEST_REQUIRE(bare.isOk());
    DOCTEST_CHECK_FALSE(bare.value().isError);
    auto all2 = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all2.isOk());
    DOCTEST_REQUIRE_EQ(all2.value().size(), 2u);
    const auto& b = all2.value()[1];
    DOCTEST_CHECK_EQ(b.title, std::string("纯标题"));
    DOCTEST_CHECK_FALSE(b.description.has_value());
    DOCTEST_CHECK(b.priority == module::todo::Priority::None);
    DOCTEST_CHECK_FALSE(b.dueAt.has_value());

    // description 显式 null：create 没有旧值可「清空」，该分支语义 = 不给（不报错、也不写 "null"）
    auto nullDesc = runTool("todo.create", R"({"title":"空描述","description":null})");
    DOCTEST_REQUIRE(nullDesc.isOk());
    DOCTEST_CHECK_FALSE(nullDesc.value().isError);
    auto all3 = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all3.isOk());
    DOCTEST_REQUIRE_EQ(all3.value().size(), 3u);
    DOCTEST_CHECK_FALSE(all3.value()[2].description.has_value());

    // 三态里的中间态：只给 priority，description/dueAt 必须各自保持默认空 ——
    // 三个属性是**各自独立可选**，不能因为给了一个就连带把别的写成缺省值（那样叫数据污染）
    auto partial = runTool("todo.create", R"({"title":"只带优先级","priority":1})");
    DOCTEST_REQUIRE(partial.isOk());
    DOCTEST_CHECK_FALSE(partial.value().isError);
    auto all4 = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all4.isOk());
    DOCTEST_REQUIRE_EQ(all4.value().size(), 4u);
    const auto& p = all4.value()[3];
    DOCTEST_CHECK(p.priority == module::todo::Priority::Low);
    DOCTEST_CHECK_FALSE(p.description.has_value());
    DOCTEST_CHECK_FALSE(p.dueAt.has_value());
    return true;
})


// 属性面的校验失败必须「报错 + 不落库」：哪怕标题合法，也不能留下半成品
// （票面要消灭的正是「有标题、无属性」且 UI 不提示的形态）。
REGISTER_TEST(todo_tools_create_attributes_invalid_no_side_effect,
              "todo.create: attributes 非法 priority/dueAt、幽灵列 → isError 且一条都不建", {
    ToolEnv env("create_attrs_bad");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    // 优先级越界 → isError（与 todo.update 的值域 0-3 一致）
    auto badPri = runTool("todo.create", R"({"title":"x","priority":9})");
    DOCTEST_REQUIRE(badPri.isOk());
    DOCTEST_CHECK(badPri.value().isError);
    DOCTEST_CHECK(badPri.value().contentJson.find("优先级") != std::string::npos);

    // dueAt 类型不对 → isError（不静默当作未设置）
    auto badDue = runTool("todo.create", R"({"title":"x","dueAt":"明天"})");
    DOCTEST_REQUIRE(badDue.isOk());
    DOCTEST_CHECK(badDue.value().isError);

    // 既有语义不回归：幽灵列 → isError，且不静默造悬空任务（带属性时同样不落库）
    auto ghostCol = runTool("todo.create",
                            R"({"title":"x","columnId":999999,"priority":2})");
    DOCTEST_REQUIRE(ghostCol.isOk());
    DOCTEST_CHECK(ghostCol.value().isError);

    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_CHECK_EQ(all.value().size(), 0u);  // 三次失败一条都没落库
    return true;
})


REGISTER_TEST(todo_tools_update_overlays_fields,
              "todo.update: 提供的字段更新、未提供的保持 + 不存在 id 报错", {
    ToolEnv env("update");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    DOCTEST_REQUIRE(seedTasks(*env.svc, 1, "原题"));
    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE_EQ(all.value().size(), 1u);
    const int64_t id = all.value()[0].id;

    const std::string args = "{\"id\":" + std::to_string(id) +
                             ",\"title\":\"新题\",\"priority\":3}";
    auto r = runTool("todo.update", args);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto after = env.svc->listAllTasks();
    DOCTEST_REQUIRE_EQ(after.value().size(), 1u);
    DOCTEST_CHECK_EQ(after.value()[0].title, std::string("新题"));
    DOCTEST_CHECK(after.value()[0].priority == module::todo::Priority::High);
    // 未提供的字段保持原状：updateTask 是整字段更新，工具必须先取旧值叠加
    DOCTEST_CHECK_FALSE(after.value()[0].description.has_value());
    DOCTEST_CHECK_FALSE(after.value()[0].completedAt.has_value());

    // description 传 null = 清空；不存在的 id → isError
    DOCTEST_REQUIRE(env.svc->listAllTasks().isOk());
    const std::string descArgs =
        "{\"id\":" + std::to_string(id) + ",\"description\":\"说明文字\"}";
    auto withDesc = runTool("todo.update", descArgs);
    DOCTEST_REQUIRE(withDesc.isOk());
    auto afterDesc = env.svc->listAllTasks();
    DOCTEST_REQUIRE(afterDesc.value()[0].description.has_value());
    DOCTEST_CHECK_EQ(afterDesc.value()[0].description.value(),
                     std::string("说明文字"));

    auto miss = runTool("todo.update", R"({"id":999999,"title":"幽灵"})");
    DOCTEST_REQUIRE(miss.isOk());
    DOCTEST_CHECK(miss.value().isError);
    DOCTEST_CHECK(miss.value().contentJson.find("不存在") != std::string::npos);
    return true;
})


REGISTER_TEST(todo_tools_move_to_column,
              "todo.move: 跨列移动、缺省位置=列尾 + 目标列不存在报错", {
    ToolEnv env("move");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(cols.value().size() >= 2u);
    DOCTEST_REQUIRE(seedTasks(*env.svc, 2, "卡"));
    const int64_t colA = cols.value()[0].id;
    const int64_t colB = cols.value()[1].id;
    auto tasks = env.svc->listAllTasks();
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 2u);
    const int64_t id = tasks.value()[0].id;

    const std::string args = "{\"id\":" + std::to_string(id) +
                             ",\"columnId\":" + std::to_string(colB) + "}";
    auto r = runTool("todo.move", args);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto inB = env.svc->listTasks(colB);
    DOCTEST_REQUIRE(inB.isOk());
    DOCTEST_REQUIRE_EQ(inB.value().size(), 1u);
    DOCTEST_CHECK_EQ(inB.value()[0].id, id);
    DOCTEST_CHECK_EQ(inB.value()[0].position, 0);  // 不给 position 时追加到列尾
    auto leftInA = env.svc->listTasks(colA);
    DOCTEST_REQUIRE_EQ(leftInA.value().size(), 1u);

    // 目标列不存在 → isError
    const std::string badArgs = "{\"id\":" + std::to_string(id) +
                                ",\"columnId\":999999}";
    auto bad = runTool("todo.move", badArgs);
    DOCTEST_REQUIRE(bad.isOk());
    DOCTEST_CHECK(bad.value().isError);
    return true;
})


REGISTER_TEST(todo_tools_complete_flips_completed_at,
              "todo.complete: completed 标记翻转（Task 字段断言）+ 不存在 id 报错", {
    ToolEnv env("complete");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    DOCTEST_REQUIRE(seedTasks(*env.svc, 1, "任务"));
    auto before = env.svc->listAllTasks();
    DOCTEST_REQUIRE_EQ(before.value().size(), 1u);
    DOCTEST_CHECK_FALSE(before.value()[0].completedAt.has_value());
    const int64_t id = before.value()[0].id;

    const std::string args = "{\"id\":" + std::to_string(id) + "}";
    auto r = runTool("todo.complete", args);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);

    auto after = env.svc->listAllTasks();
    DOCTEST_REQUIRE(after.value()[0].completedAt.has_value());

    auto miss = runTool("todo.complete", R"({"id":424242})");
    DOCTEST_REQUIRE(miss.isOk());
    DOCTEST_CHECK(miss.value().isError);
    DOCTEST_CHECK(miss.value().contentJson.find("不存在") != std::string::npos);
    return true;
})


REGISTER_TEST(todo_tools_delete_destructive_and_removes,
              "todo.delete: isDestructive()==true + 删除真实生效", {
    ToolEnv env("del");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    DOCTEST_REQUIRE(seedTasks(*env.svc, 2, "任务"));
    auto tasks = env.svc->listAllTasks();
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 2u);
    const int64_t id = tasks.value()[0].id;

    // 删除不可逆，必须标破坏性：AgentLoop 在 allowDestructive=0 时会拒绝执行
    auto found = core::ToolRegistry::instance().find("todo.delete");
    DOCTEST_REQUIRE(found.isOk());
    DOCTEST_CHECK(found.value()->isDestructive());

    const std::string args = "{\"id\":" + std::to_string(id) + "}";
    auto r = runTool("todo.delete", args);
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_FALSE(r.value().isError);
    auto after = env.svc->listAllTasks();
    DOCTEST_REQUIRE_EQ(after.value().size(), 1u);
    DOCTEST_CHECK_NE(after.value()[0].id, id);

    // 删不存在的 id → isError
    auto miss = runTool("todo.delete", R"({"id":777777})");
    DOCTEST_REQUIRE(miss.isOk());
    DOCTEST_CHECK(miss.value().isError);
    return true;
})


REGISTER_TEST(todo_tools_invalid_params_feed_error,
              "工具级错误：Result ok + isError=true + 中文说明，且无副作用", {
    ToolEnv env("invalid");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    // 空 title → isError，且不静默建任务
    auto emptyTitle = runTool("todo.create", R"({"title":""})");
    DOCTEST_REQUIRE(emptyTitle.isOk());
    DOCTEST_CHECK(emptyTitle.value().isError);
    DOCTEST_CHECK(emptyTitle.value().contentJson.find("标题") !=
                  std::string::npos);
    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_CHECK_EQ(all.value().size(), 0u);

    // 非法 JSON 参数 → isError
    auto badJson = runTool("todo.create", "not-json");
    DOCTEST_REQUIRE(badJson.isOk());
    DOCTEST_CHECK(badJson.value().isError);

    // 缺必填 id → isError
    auto noId = runTool("todo.complete", "{}");
    DOCTEST_REQUIRE(noId.isOk());
    DOCTEST_CHECK(noId.value().isError);

    // 优先级越界 → isError
    DOCTEST_REQUIRE(seedTasks(*env.svc, 1, "任务"));
    auto seeded = env.svc->listAllTasks();
    const std::string badPri = "{\"id\":" +
                               std::to_string(seeded.value()[0].id) +
                               ",\"priority\":9}";
    auto pri = runTool("todo.update", badPri);
    DOCTEST_REQUIRE(pri.isOk());
    DOCTEST_CHECK(pri.value().isError);
    DOCTEST_CHECK(pri.value().contentJson.find("优先级") != std::string::npos);
    return true;
})


REGISTER_TEST(todo_tools_schema_spot_check_and_registry,
              "schema 抽查：6 工具注册齐全、input_schema 含 required，破坏性只有 delete", {
    ToolEnv env("schema");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    const char* names[] = {"todo.list", "todo.create", "todo.update",
                           "todo.move", "todo.complete", "todo.delete"};
    for (const char* n : names) {
        auto found = core::ToolRegistry::instance().find(n);
        DOCTEST_REQUIRE(found.isOk());
        const std::string schema = found.value()->schemaJson();
        DOCTEST_CHECK(schema.find("input_schema") != std::string::npos);
        DOCTEST_CHECK(schema.find(std::string("\"name\":\"") + n + "\"") !=
                      std::string::npos);
        // 每个工具宣传的入参键必须真实出现在 schema 里
        const std::string nStr(n);
        const char* expectedKey =
            nStr == "todo.list"     ? "completed"
            : (nStr == "todo.create" || nStr == "todo.update") ? "title"
            : nStr == "todo.move"   ? "columnId"
                                    : "id";  // complete / delete
        DOCTEST_CHECK(schema.find(std::string("\"") + expectedKey + "\"") !=
                      std::string::npos);
        // todo.list 的参数全可选，不声明 required；其余工具必须声明
        if (nStr == "todo.list") {
            DOCTEST_CHECK(schema.find("\"required\"") == std::string::npos);
        } else {
            DOCTEST_CHECK(schema.find("\"required\"") != std::string::npos);
        }
        // 破坏性标记只有 todo.delete
        DOCTEST_CHECK_EQ(found.value()->isDestructive(),
                         nStr == "todo.delete");
    }

    // 注销后不可再 find（模块开关插拔语义）；对不存在的名字重复注销是 no-op
    env.tools->unregisterAll();
    DOCTEST_CHECK(core::ToolRegistry::instance().find("todo.list").isFail());
    env.tools->unregisterAll();
    return true;
})

// debt#416（单一注册源 = ToolMirror 裁决）：原 need394 的 tool_kit::McpToolAdapter
// 直注注册块已删除，本用例改锁镜像命名规则——6 件 todo.* 经 ToolMirror 以 todo_* 名
// 进 MCP 对外面，后端与智能体面同一份实现（描述/schema 同源、执行零语义漂移）；
// 点号原名不得漏进对外面
REGISTER_TEST(todo_tools_mcp_registration,
              "debt416: 6 件 todo_* 经 ToolMirror 进 MCP 对外面，描述/schema 同源，空参与目标不存在走 isError", {
    ToolEnv env("mcp");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();

    core::mcp::McpToolRegistry mcp;
    module::mcp::ToolMirror mirror(mcp);
    mirror.sync();
    // 并行分片下同片可能有其它用例注册的工具：不钉全局总数，只钉 6 件 todo_* 全在。
    DOCTEST_CHECK(static_cast<int>(mcp.size()) >= 6);

    const char* names[] = {"todo_list",       "todo_create", "todo_update",
                           "todo_move",       "todo_complete", "todo_delete"};
    for (const char* n : names) {
        const std::string name(n);
        auto found = mcp.find(name);
        DOCTEST_REQUIRE_MESSAGE(found.isOk(), name);
        const auto& tool = found.value();
        DOCTEST_CHECK_EQ(tool->name(), name);
        // 描述/schema 从智能体面 schemaJson 拆出（非手写副本）
        DOCTEST_CHECK(!tool->description().empty());
        DOCTEST_CHECK(tool->inputSchemaJson().find("\"type\":\"object\"") !=
                      std::string::npos);
        // 点号原名不进对外面（对外全下划线）
        DOCTEST_CHECK(mcp.find("todo." + name.substr(5)).isFail());
    }
    DOCTEST_CHECK(mcp.find("todo_list").value()->description().find("待办任务") !=
                  std::string::npos);

    // 空参调用：todo_list → {"tasks":[...]}（与智能体面同形）
    {
        auto r = mcp.find("todo_list").value()->execute("{}");
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK_FALSE(r.value().isError);
        DOCTEST_CHECK(r.value().text.find("\"tasks\"") != std::string::npos);
    }
    // 业务失败仍走 Result ok + isError（MCP 面零语义漂移）
    {
        auto r = mcp.find("todo_delete").value()->execute(R"({"id":999999})");
        DOCTEST_REQUIRE(r.isOk());
        DOCTEST_CHECK(r.value().isError);
    }

    // 源无变更时二次 sync 零增删（幂等）
    mirror.sync();
    DOCTEST_CHECK(static_cast<int>(mcp.size()) >= 6);

    // 源侧注销 → 下一次 sync 镜像整批消失（模块 onStop 的自愈路径）
    env.tools->unregisterAll();
    mirror.sync();
    for (const char* n : names) {
        DOCTEST_CHECK(mcp.find(n).isFail());
    }
    return true;
})

REGISTER_TEST(todo_tools_changed_by_tool_event,
              "todo_tools: 写工具发 todo.changed_by_tool；todo.list 不发（A-7）", {
    ToolEnv env("changed_event");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    DOCTEST_REQUIRE(env.svc->listColumns().isOk());
    int count = 0;
    auto sub = ::core::event::bus().subscribe(
        ::core::event::kTodoChangedByTool,
        [&count](const std::string&) { ++count; });
    // B-9：payload 订阅面拿到 taskId + op
    std::string lastPayload;
    auto subPayload = ::core::event::bus().subscribeWithPayload(
        ::core::event::kTodoChangedByTool,
        [&lastPayload](const std::string&, const std::string& p) {
            lastPayload = p;
        });

    auto made = runTool("todo.create", R"({"title":"事件任务"})");
    DOCTEST_REQUIRE(made.isOk());
    DOCTEST_CHECK_EQ(count, 1);
    DOCTEST_CHECK(lastPayload.find("\"op\":\"create\"") != std::string::npos);

    auto list = runTool("todo.list", R"({})");
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(count, 1);  // 只读不发

    auto tasks = env.svc->listAllTasks();
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    const std::string args =
        "{\"id\":" + std::to_string(tasks.value()[0].id) + "}";
    auto done = runTool("todo.complete", args);
    DOCTEST_REQUIRE(done.isOk());
    DOCTEST_CHECK_EQ(count, 2);
    DOCTEST_CHECK(lastPayload.find("\"op\":\"complete\"") != std::string::npos);
    DOCTEST_CHECK(lastPayload.find(
                      "\"taskId\":" + std::to_string(tasks.value()[0].id)) !=
                  std::string::npos);
    DOCTEST_CHECK(lastPayload.find("\"source\":\"tool\"") != std::string::npos);
    return true;
})

REGISTER_TEST(todo_tools_move_position_clamped,
              "todo_tools: todo.move 的 position 钳到 [0, 列内任务数]（B-10）", {
    ToolEnv env("move_clamp");
    DOCTEST_REQUIRE(env.ready);
    env.tools->registerAll();
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    DOCTEST_REQUIRE(cols.value().size() >= 2);
    const int64_t c0 = cols.value()[0].id;
    const int64_t c1 = cols.value()[1].id;
    auto a = env.svc->createTask(c1, "A");
    auto b = env.svc->createTask(c1, "B");
    auto c = env.svc->createTask(c1, "C");
    DOCTEST_REQUIRE(a.isOk() && b.isOk() && c.isOk());

    auto moveTool = [&](int64_t id, int64_t col, int pos) {
        return runTool("todo.move",
                       "{\"id\":" + std::to_string(id) +
                           ",\"columnId\":" + std::to_string(col) +
                           ",\"position\":" + std::to_string(pos) + "}");
    };
    auto r1 = moveTool(c.value().id, c0, -9);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_FALSE(r1.value().isError);
    auto list0 = env.svc->listTasks(c0);
    DOCTEST_REQUIRE(list0.isOk());
    DOCTEST_REQUIRE_EQ(list0.value().size(), 1u);
    DOCTEST_CHECK_EQ(list0.value()[0].position, 0);

    auto r2 = moveTool(b.value().id, c0, 999);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_FALSE(r2.value().isError);
    auto list1 = env.svc->listTasks(c0);
    DOCTEST_REQUIRE(list1.isOk());
    DOCTEST_REQUIRE_EQ(list1.value().size(), 2u);
    DOCTEST_CHECK_EQ(list1.value().back().id, b.value().id);
    DOCTEST_CHECK_EQ(list1.value().back().position,
                     static_cast<int>(list1.value().size()) - 1);
    return true;
})
