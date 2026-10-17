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

#include "core/module_interface.h"
#include "core/tool.h"
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


REGISTER_TEST(todo_tools_create_default_first_column,
              "todo.create: 缺省列=第一列，service 真实落库 + 返回新 id", {
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
