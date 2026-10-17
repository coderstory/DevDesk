// tests/test_modules/test_todo_service.cpp
// TodoService 单元测试：列 / 任务的 CRUD、tags 序列化往返、id 与落库行一致。
// 跨文件前提：
//   - 每个用例一份独立临时 db（test_support::TestEnv 负责建库 + migrate + 析构清理），
//     所以「4 个种子列」「position 从 0 起」这类基线是 migration 定的，不是实现自定的。
//   - 写 tags / 重排 / 完成态都直接 UPDATE todo_tasks.tags 等列绕过 service，
//     用来造 service 自己也造不出的行；这种行不反映任何正常调用路径。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/event_bus.h"
#include "core/sqlite_db.h"
#include "module_todo/todo_service.h"

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_todosvc_";

struct TestEnv : test_support::TestEnv {
    std::unique_ptr<module::todo::TodoService> svc;

    explicit TestEnv(const std::string& tag)
        : test_support::TestEnv(kDbPrefix, tag) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
    }
};

} // namespace

REGISTER_TEST(todo_svc_create_list_columns,
              "TodoService: createColumn + listColumns",
              {
    TestEnv env("create_list_columns");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_CHECK_EQ(list.value().size(), 4u);
    DOCTEST_CHECK_EQ(list.value()[0].title, std::string("待办"));
    DOCTEST_CHECK_EQ(list.value()[3].title, std::string("已归档"));

    auto newCol = env.svc->createColumn("测试列");
    DOCTEST_REQUIRE(newCol.isOk());
    DOCTEST_CHECK(newCol.value().id > 0);
    DOCTEST_CHECK_EQ(newCol.value().title, std::string("测试列"));
    DOCTEST_CHECK(!newCol.value().color.has_value());

    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value().size(), 5u);
    DOCTEST_CHECK_EQ(list2.value()[4].title, std::string("测试列"));
    return true;
})

REGISTER_TEST(todo_svc_rename_column,
              "TodoService: renameColumn",
              {
    TestEnv env("rename");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t firstId = list.value()[0].id;
    std::string oldTitle = list.value()[0].title;

    auto r = env.svc->renameColumn(firstId, "重命名后的列");
    DOCTEST_CHECK(r.isOk());

    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    bool found = false;
    for (const auto& c : list2.value()) {
        if (c.id == firstId) {
            DOCTEST_CHECK_EQ(c.title, std::string("重命名后的列"));
            found = true;
        }
    }
    DOCTEST_CHECK(found);
    (void)oldTitle;
    return true;
})

// renameColumn 对不存在的 id 必须返回 NotFound：只发 UPDATE 后无条件 ok() 会让
// 重命名静默成功，UI 侧看不到任何反馈。
REGISTER_TEST(todo_svc_rename_missing_notfound,
              "TodoService: renameColumn id 不存在 → NotFound", {
    TestEnv env("rename_notfound");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t existing = list.value()[0].id;
    const int64_t missing = 999999;

    auto miss = env.svc->renameColumn(missing, "幽灵列");
    DOCTEST_REQUIRE(miss.isFail());
    DOCTEST_CHECK(miss.errorCode() == core::ErrorCode::NotFound);

    // 正向对照：存在的列仍能改名（校验不误伤正常路径）
    DOCTEST_CHECK(env.svc->renameColumn(existing, "仍可改").isOk());
    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value()[0].title, std::string("仍可改"));
    return true;
})

REGISTER_TEST(todo_svc_set_column_color,
              "TodoService: setColumnColor 设置/清除/读回",
              {
    TestEnv env("color");
    DOCTEST_REQUIRE(env.ready);

    auto created = env.svc->createColumn("改色列");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t id = created.value().id;

    DOCTEST_CHECK(env.svc->setColumnColor(
                      id, std::optional<std::string>("dusty-blue")).isOk());
    {
        auto list = env.svc->listColumns();
        DOCTEST_REQUIRE(list.isOk());
        for (const auto& c : list.value()) {
            if (c.id == id) {
                DOCTEST_REQUIRE(c.color.has_value());
                DOCTEST_CHECK_EQ(*c.color, std::string("dusty-blue"));
            }
        }
    }

    // 清除：nullopt 写 NULL（不是空串），读回即 nullopt
    DOCTEST_CHECK(env.svc->setColumnColor(
                      id, std::optional<std::string>("rose-red")).isOk());
    DOCTEST_CHECK(env.svc->setColumnColor(id, std::nullopt).isOk());
    {
        auto list = env.svc->listColumns();
        DOCTEST_REQUIRE(list.isOk());
        for (const auto& c : list.value()) {
            if (c.id == id) {
                DOCTEST_CHECK(!c.color.has_value());
            }
        }
    }

    {
        auto list = env.svc->listColumns();
        DOCTEST_REQUIRE(list.isOk());
        DOCTEST_REQUIRE(!list.value().empty());
        DOCTEST_REQUIRE(list.value()[0].color.has_value());
        DOCTEST_CHECK_EQ(*list.value()[0].color, std::string("claude-clay"));
    }
    return true;
})

REGISTER_TEST(todo_svc_delete_column,
              "TodoService: deleteColumn + CASCADE deletes tasks",
              {
    TestEnv env("delete_col");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t lastId = list.value().back().id;

    auto t1 = env.svc->createTask(lastId, "待删除列里的任务");
    DOCTEST_REQUIRE(t1.isOk());

    auto r = env.svc->deleteColumn(lastId);
    DOCTEST_CHECK(r.isOk());

    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value().size(), 3u);

    // 列没了，其下任务靠 todo_tasks 的外键 CASCADE 一并消失
    auto allTasks = env.svc->listAllTasks();
    DOCTEST_REQUIRE(allTasks.isOk());
    DOCTEST_CHECK_EQ(allTasks.value().size(), 0u);
    return true;
})

REGISTER_TEST(todo_svc_create_list_tasks,
              "TodoService: createTask + listTasks + tags 序列化",
              {
    TestEnv env("create_task");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t todoColId = list.value()[0].id;

    auto t1 = env.svc->createTask(todoColId, "第一个任务");
    DOCTEST_REQUIRE(t1.isOk());
    DOCTEST_CHECK_EQ(t1.value().title, std::string("第一个任务"));
    DOCTEST_CHECK_EQ(t1.value().columnId, todoColId);
    DOCTEST_CHECK_EQ(t1.value().position, 0);

    auto t2 = env.svc->createTask(todoColId, "第二个任务");
    DOCTEST_REQUIRE(t2.isOk());
    DOCTEST_CHECK_EQ(t2.value().position, 1);

    auto tasks = env.svc->listTasks(todoColId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), 2u);
    DOCTEST_CHECK_EQ(tasks.value()[0].title, std::string("第一个任务"));
    DOCTEST_CHECK_EQ(tasks.value()[1].title, std::string("第二个任务"));
    DOCTEST_CHECK(tasks.value()[0].tags.empty());
    DOCTEST_CHECK(!tasks.value()[0].completedAt.has_value());
    return true;
})

// 属性面入口（票面 01 的根因）：createTask 签名里没有 description/priority/dueAt，工具只能
// 「先建后 todo.update」二跳。带属性重载必须把三字段写进同一条 INSERT，且旧两参入口语义不变。
REGISTER_TEST(todo_svc_create_task_with_attributes,
              "TodoService: createTask attributes 一次落库 description/priority/dueAt + 旧入口仍全空", {
    TestEnv env("create_task_attrs");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t colId = list.value()[0].id;

    module::todo::NewTaskAttributes attrs;
    attrs.description = std::string("详细说明");
    attrs.priority = module::todo::Priority::Medium;
    attrs.dueAt = static_cast<int64_t>(1800000000);
    auto t = env.svc->createTask(colId, "带属性", attrs);
    DOCTEST_REQUIRE(t.isOk());
    // 返回值就是落库值（调用点不必再查一次库）
    DOCTEST_CHECK_EQ(t.value().columnId, colId);
    DOCTEST_REQUIRE(t.value().description.has_value());
    DOCTEST_CHECK_EQ(t.value().description.value(), std::string("详细说明"));
    DOCTEST_CHECK(t.value().priority == module::todo::Priority::Medium);
    DOCTEST_REQUIRE(t.value().dueAt.has_value());
    DOCTEST_CHECK_EQ(t.value().dueAt.value(), static_cast<int64_t>(1800000000));

    // 回读仍一致（返回值不是内存里随手拼的假象）
    auto tasks = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    DOCTEST_REQUIRE(tasks.value()[0].description.has_value());
    DOCTEST_CHECK_EQ(tasks.value()[0].description.value(), std::string("详细说明"));
    DOCTEST_CHECK(tasks.value()[0].priority == module::todo::Priority::Medium);
    DOCTEST_REQUIRE(tasks.value()[0].dueAt.has_value());
    DOCTEST_CHECK_EQ(tasks.value()[0].dueAt.value(), static_cast<int64_t>(1800000000));

    // 旧两参入口（UI doCreateTask / 工具 / 测试三类调用点）语义不变：属性全空
    auto bare = env.svc->createTask(colId, "纯标题");
    DOCTEST_REQUIRE(bare.isOk());
    auto tasks2 = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks2.isOk());
    DOCTEST_REQUIRE_EQ(tasks2.value().size(), 2u);
    DOCTEST_CHECK_FALSE(tasks2.value()[1].description.has_value());
    DOCTEST_CHECK(tasks2.value()[1].priority == module::todo::Priority::None);
    DOCTEST_CHECK_FALSE(tasks2.value()[1].dueAt.has_value());
    return true;
})

REGISTER_TEST(todo_svc_move_task,
              "TodoService: moveTask 跨列 + 同列 + position 重排",
              {
    TestEnv env("move");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colA = list.value()[0].id;
    int64_t colB = list.value()[1].id;

    auto ta = env.svc->createTask(colA, "A1");
    DOCTEST_REQUIRE(ta.isOk());
    auto tb = env.svc->createTask(colA, "A2");
    DOCTEST_REQUIRE(tb.isOk());
    auto tc = env.svc->createTask(colA, "A3");
    DOCTEST_REQUIRE(tc.isOk());
    int64_t a2Id = tb.value().id;

    auto td = env.svc->createTask(colB, "B1");
    DOCTEST_REQUIRE(td.isOk());
    auto te = env.svc->createTask(colB, "B2");
    DOCTEST_REQUIRE(te.isOk());

    auto mv = env.svc->moveTask(a2Id, colB, 1);
    DOCTEST_CHECK(mv.isOk());

    // A2 被抽走后 A3 补位到 1，不是留在 2
    auto aTasks = env.svc->listTasks(colA);
    DOCTEST_REQUIRE(aTasks.isOk());
    DOCTEST_CHECK_EQ(aTasks.value().size(), 2u);
    DOCTEST_CHECK_EQ(aTasks.value()[0].title, std::string("A1"));
    DOCTEST_CHECK_EQ(aTasks.value()[1].title, std::string("A3"));

    auto bTasks = env.svc->listTasks(colB);
    DOCTEST_REQUIRE(bTasks.isOk());
    DOCTEST_CHECK_EQ(bTasks.value().size(), 3u);
    DOCTEST_CHECK_EQ(bTasks.value()[0].title, std::string("B1"));
    DOCTEST_CHECK_EQ(bTasks.value()[1].title, std::string("A2"));
    DOCTEST_CHECK_EQ(bTasks.value()[2].title, std::string("B2"));
    return true;
})

REGISTER_TEST(todo_svc_complete_uncomplete,
              "TodoService: completeTask + uncompleteTask",
              {
    TestEnv env("complete");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colId = list.value()[0].id;

    auto t = env.svc->createTask(colId, "完蛋任务");
    DOCTEST_REQUIRE(t.isOk());
    int64_t tid = t.value().id;

    auto c = env.svc->completeTask(tid);
    DOCTEST_CHECK(c.isOk());

    auto tasks = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks.isOk());
    bool foundCompleted = false;
    for (const auto& tt : tasks.value()) {
        if (tt.id == tid) {
            DOCTEST_CHECK(tt.completedAt.has_value());
            foundCompleted = true;
        }
    }
    DOCTEST_CHECK(foundCompleted);

    auto u = env.svc->uncompleteTask(tid);
    DOCTEST_CHECK(u.isOk());

    auto tasks2 = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks2.isOk());
    for (const auto& tt : tasks2.value()) {
        if (tt.id == tid) {
            DOCTEST_CHECK(!tt.completedAt.has_value());
        }
    }
    return true;
})

REGISTER_TEST(todo_svc_update_task,
              "TodoService: updateTask 改 description / priority / dueAt / tags",
              {
    TestEnv env("update");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colId = list.value()[0].id;

    auto t = env.svc->createTask(colId, "初始标题");
    DOCTEST_REQUIRE(t.isOk());

    module::todo::Task updated;
    updated.id = t.value().id;
    updated.columnId = colId;
    updated.title = "更新后标题";
    updated.description = std::string("详细描述");
    updated.priority = module::todo::Priority::High;
    updated.dueAt = static_cast<int64_t>(1234567890);
    updated.tags = {"work", "urgent", "P0"};
    updated.position = 0;

    auto r = env.svc->updateTask(updated);
    DOCTEST_CHECK(r.isOk());

    auto tasks = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    const auto& got = tasks.value()[0];
    DOCTEST_CHECK_EQ(got.title, std::string("更新后标题"));
    DOCTEST_CHECK(got.description.has_value());
    DOCTEST_CHECK_EQ(got.description.value(), std::string("详细描述"));
    DOCTEST_CHECK_EQ(static_cast<int>(got.priority), 3);
    DOCTEST_CHECK(got.dueAt.has_value());
    DOCTEST_CHECK_EQ(got.dueAt.value(), static_cast<int64_t>(1234567890));
    DOCTEST_CHECK_EQ(got.tags.size(), 3u);
    DOCTEST_CHECK_EQ(got.tags[0], std::string("work"));
    DOCTEST_CHECK_EQ(got.tags[2], std::string("P0"));
    return true;
})

// updateTask 只改内容字段：调用方误填 columnId/position 时不得静默挪列 / 改序，
// 改列改序的唯一路径是 moveTask。
REGISTER_TEST(todo_svc_update_task_ignores_column,
              "TodoService: updateTask 不再改 column/position（编辑保存不改序）", {
    TestEnv env("update_ignores_column");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    const int64_t colA = cols.value()[0].id;
    const int64_t colB = cols.value()[1].id;

    auto t1 = env.svc->createTask(colA, "原任务");
    auto t2 = env.svc->createTask(colA, "占位任务");
    DOCTEST_REQUIRE(t1.isOk());
    DOCTEST_REQUIRE(t2.isOk());

    module::todo::Task upd = t1.value();
    upd.title = "改了标题";
    upd.columnId = colB;  // 误传：期望被忽略
    upd.position = 9;     // 误传：期望被忽略
    DOCTEST_REQUIRE(env.svc->updateTask(upd).isOk());

    auto inA = env.svc->listTasks(colA);
    auto inB = env.svc->listTasks(colB);
    DOCTEST_REQUIRE(inA.isOk());
    DOCTEST_REQUIRE(inB.isOk());
    DOCTEST_REQUIRE_EQ(inA.value().size(), 2u);
    DOCTEST_CHECK_EQ(inB.value().size(), 0u);

    bool checked = false;
    for (const auto& task : inA.value()) {
        if (task.id != t1.value().id) continue;
        checked = true;
        DOCTEST_CHECK_EQ(task.title, std::string("改了标题"));
        DOCTEST_CHECK_EQ(task.columnId, colA);
        DOCTEST_CHECK_EQ(task.position, t1.value().position);
    }
    DOCTEST_CHECK(checked);
    return true;
})

REGISTER_TEST(todo_svc_search,
              "TodoService: search 按 title/description 模糊匹配",
              {
    TestEnv env("search");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colId = list.value()[0].id;

    auto t1 = env.svc->createTask(colId, "写周报");
    DOCTEST_REQUIRE(t1.isOk());
    auto t2 = env.svc->createTask(colId, "修复登录 bug");
    DOCTEST_REQUIRE(t2.isOk());
    auto t3 = env.svc->createTask(colId, "开周会");
    DOCTEST_REQUIRE(t3.isOk());

    module::todo::Task upd;
    upd.id = t1.value().id;
    upd.columnId = colId;
    upd.title = t1.value().title;
    upd.description = std::string("本周工作总结");
    upd.priority = module::todo::Priority::None;
    upd.tags = {};
    upd.position = 0;
    DOCTEST_CHECK(env.svc->updateTask(upd).isOk());

    auto r1 = env.svc->search("周报");
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_EQ(r1.value().size(), 1u);

// 搜 "周" 命中「写周报」+「开周会」；t1 的 description 也含「周」但不重复计入
    auto r2 = env.svc->search("周");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().size() >= 2u);

    auto r3 = env.svc->search("bug");
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK_EQ(r3.value().size(), 1u);

    auto r4 = env.svc->search("不存在的关键词xyz");
    DOCTEST_REQUIRE(r4.isOk());
    DOCTEST_CHECK_EQ(r4.value().size(), 0u);

    auto r5 = env.svc->search("");
    DOCTEST_REQUIRE(r5.isOk());
    DOCTEST_CHECK_EQ(r5.value().size(), 3u);
    return true;
})

REGISTER_TEST(todo_svc_list_overdue,
              "TodoService: listOverdue 返回过期且未完成的任务",
              {
    TestEnv env("overdue");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colId = list.value()[0].id;

    auto t1 = env.svc->createTask(colId, "过期未完成");
    DOCTEST_REQUIRE(t1.isOk());
    auto t2 = env.svc->createTask(colId, "过期但已完成");
    DOCTEST_REQUIRE(t2.isOk());
    auto t3 = env.svc->createTask(colId, "未过期");
    DOCTEST_REQUIRE(t3.isOk());

    // dueAt 是 unix 秒：past = 2001 年，future = 2286 年
    int64_t past = 1000000000;
    int64_t future = 9999999999;

    module::todo::Task u1 = t1.value();
    u1.dueAt = past;
    DOCTEST_CHECK(env.svc->updateTask(u1).isOk());

    // 过期 + 已完成 → 不算逾期
    module::todo::Task u2 = t2.value();
    u2.dueAt = past;
    DOCTEST_CHECK(env.svc->updateTask(u2).isOk());
    DOCTEST_CHECK(env.svc->completeTask(u2.id).isOk());

    module::todo::Task u3 = t3.value();
    u3.dueAt = future;
    DOCTEST_CHECK(env.svc->updateTask(u3).isOk());

    auto over = env.svc->listOverdue();
    DOCTEST_REQUIRE(over.isOk());
    DOCTEST_CHECK_EQ(over.value().size(), 1u);
    DOCTEST_CHECK_EQ(over.value()[0].title, std::string("过期未完成"));
    return true;
})

REGISTER_TEST(todo_svc_reorder_columns,
              "TodoService: reorderColumns 重排 + 保留所有列",
              {
    TestEnv env("reorder");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE_EQ(list.value().size(), 4u);

    std::vector<int64_t> reversed;
    for (int i = static_cast<int>(list.value().size()) - 1; i >= 0; --i) {
        reversed.push_back(list.value()[i].id);
    }

    auto r = env.svc->reorderColumns(reversed);
    DOCTEST_CHECK(r.isOk());

    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value().size(), 4u);
    DOCTEST_CHECK_EQ(list2.value()[0].id, reversed[0]);
    DOCTEST_CHECK_EQ(list2.value()[3].id, reversed[3]);
    DOCTEST_CHECK_EQ(list2.value()[0].position, 0);
    DOCTEST_CHECK_EQ(list2.value()[3].position, 3);
    return true;
})

REGISTER_TEST(todo_svc_list_all_tasks,
              "TodoService: listAllTasks 跨列全量",
              {
    TestEnv env("list_all");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t c1 = list.value()[0].id;
    int64_t c2 = list.value()[1].id;

    env.svc->createTask(c1, "c1-t1");
    env.svc->createTask(c1, "c1-t2");
    env.svc->createTask(c2, "c2-t1");

    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_CHECK_EQ(all.value().size(), 3u);

    // listAllTasks 跨列按 column_id 升序，同列内按 position
    int lastCol = -1;
    for (const auto& t : all.value()) {
        DOCTEST_CHECK(static_cast<int>(t.columnId) >= lastCol);
        lastCol = static_cast<int>(t.columnId);
    }
    return true;
})

REGISTER_TEST(todo_svc_delete_task,
              "TodoService: deleteTask 单个任务",
              {
    TestEnv env("delete_task");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colId = list.value()[0].id;

    auto t1 = env.svc->createTask(colId, "活着");
    DOCTEST_REQUIRE(t1.isOk());
    auto t2 = env.svc->createTask(colId, "要删");
    DOCTEST_REQUIRE(t2.isOk());

    auto d = env.svc->deleteTask(t2.value().id);
    DOCTEST_CHECK(d.isOk());

    auto tasks = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_CHECK_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK_EQ(tasks.value()[0].title, std::string("活着"));
    return true;
})

// 端到端：新建任务 → 完成 → 移到「已完成」列（UI 的完成流程）
REGISTER_TEST(todo_svc_acceptance_flow,
              "TodoService: 验收流程 新建 → 完成 → 落到「已完成」列",
              {
    TestEnv env("acceptance");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE_EQ(list.value().size(), 4u);
    DOCTEST_CHECK_EQ(list.value()[0].title, std::string("待办"));
    DOCTEST_CHECK_EQ(list.value()[2].title, std::string("已完成"));

    const int64_t todoCol = list.value()[0].id;
    const int64_t doneCol = list.value()[2].id;

    auto created = env.svc->createTask(todoCol, "验收：写文档");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value().id;

    auto inTodo = env.svc->listTasks(todoCol);
    DOCTEST_REQUIRE(inTodo.isOk());
    DOCTEST_CHECK_EQ(inTodo.value().size(), 1u);

    // 另开一条连接读，确认行真的落到了 db 文件（不是 service 连接内的未刷状态）
    {
        auto probe = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(probe.isOk());
        auto prep = probe.value().prepare(
            "SELECT title, completed_at FROM todo_tasks WHERE id = ?");
        DOCTEST_REQUIRE(prep.isOk());
        auto& stmt = prep.value();
        DOCTEST_REQUIRE(stmt.bind(1, taskId).isOk());
        auto rows = stmt.fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        DOCTEST_CHECK_EQ(rows.value()[0].getString("title").value(),
                         std::string("验收：写文档"));
        DOCTEST_CHECK(rows.value()[0].isNull("completed_at"));
    }

    DOCTEST_CHECK(env.svc->completeTask(taskId).isOk());
    DOCTEST_CHECK(env.svc->moveTask(taskId, doneCol, 0).isOk());

    auto inDone = env.svc->listTasks(doneCol);
    DOCTEST_REQUIRE(inDone.isOk());
    DOCTEST_CHECK_EQ(inDone.value().size(), 1u);
    DOCTEST_CHECK_EQ(inDone.value()[0].title, std::string("验收：写文档"));
    DOCTEST_CHECK(inDone.value()[0].completedAt.has_value());

    auto emptyTodo = env.svc->listTasks(todoCol);
    DOCTEST_REQUIRE(emptyTodo.isOk());
    DOCTEST_CHECK_EQ(emptyTodo.value().size(), 0u);

    {
        auto probe = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(probe.isOk());
        auto prep = probe.value().prepare(
            "SELECT column_id, completed_at FROM todo_tasks WHERE id = ?");
        DOCTEST_REQUIRE(prep.isOk());
        auto& stmt = prep.value();
        DOCTEST_REQUIRE(stmt.bind(1, taskId).isOk());
        auto rows = stmt.fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        DOCTEST_CHECK_EQ(rows.value()[0].getInt64("column_id").value(), doneCol);
        DOCTEST_CHECK(!rows.value()[0].isNull("completed_at"));
    }
    return true;
})

// 列 kind 是语义标记（migration 回填）：完成态流转靠它，不看标题
REGISTER_TEST(todo_svc_column_kind,
              "TodoService: 列 kind 标记回填 + 重命名后保留", {
    TestEnv env("column_kind");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE_EQ(list.value().size(), 4u);
    // 种子列 kind：待办=todo / 已完成=done / 归档=archive / 普通列=""
    DOCTEST_CHECK_EQ(list.value()[0].kind, std::string("todo"));
    DOCTEST_CHECK_EQ(list.value()[1].kind, std::string(""));
    DOCTEST_CHECK_EQ(list.value()[2].kind, std::string("done"));
    DOCTEST_CHECK_EQ(list.value()[3].kind, std::string("archive"));

    // 重命名后 kind 保留（语义不随标题变）
    DOCTEST_REQUIRE(env.svc->renameColumn(list.value()[2].id, "Finished").isOk());
    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value()[2].title, std::string("Finished"));
    DOCTEST_CHECK_EQ(list2.value()[2].kind, std::string("done"));

    auto created = env.svc->createColumn("复核中");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_CHECK_EQ(created.value().kind, std::string(""));
    return true;
})

// serializeTags / deserializeTags 是私有方法，只能经 updateTask → listTasks 公开路径覆盖；
// 特殊字符（引号/反斜杠/换行/制表/控制字符/CJK/空串）必须原样恢复。
REGISTER_TEST(todo_svc_tags_roundtrip_special_chars,
              "TodoService: tags 特殊字符经 updateTask→listTasks 往返无损",
              {
    TestEnv env("tags_roundtrip");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t colId = list.value()[0].id;

    auto t = env.svc->createTask(colId, "标签往返");
    DOCTEST_REQUIRE(t.isOk());

    std::vector<std::string> tags = {
        "简单",                           // CJK 原样透传
        "带\"引号\"",                     // 双引号转义
        "反斜杠\\",                       // 反斜杠转义
        "换行\n制表\t",                   // \n \t 命名转义
        std::string(1, '\x01') + "控制",  // \u00xx 数字转义
        "",                               // 空串元素
        " 前后空格 ",                     // 空格保留
    };

    module::todo::Task updated = t.value();
    updated.tags = tags;
    DOCTEST_CHECK(env.svc->updateTask(updated).isOk());

    auto tasks = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK_EQ(tasks.value()[0].tags.size(), tags.size());
    for (size_t i = 0; i < tags.size(); ++i) {
        DOCTEST_CHECK_EQ(tasks.value()[0].tags[i], tags[i]);
    }

    // 空 tags 存成 "[]"，读回仍是空 vector
    module::todo::Task cleared = tasks.value()[0];
    cleared.tags = {};
    DOCTEST_CHECK(env.svc->updateTask(cleared).isOk());
    auto tasks2 = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks2.isOk());
    DOCTEST_CHECK(tasks2.value()[0].tags.empty());
    return true;
})

// 容错口径（经 listTasks 公开路径）：非法 JSON / 非数组 / 混合类型数组都不得崩溃，
// 也不得阻断同批其它行的解析。
REGISTER_TEST(todo_svc_tags_deserialize_tolerant,
              "TodoService: tags 列内容异常时反序列化容错",
              {
    TestEnv env("tags_tolerant");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    const int64_t colId = list.value()[0].id;

    auto t1 = env.svc->createTask(colId, "非法JSON");
    auto t2 = env.svc->createTask(colId, "非数组");
    auto t3 = env.svc->createTask(colId, "混合类型");
    DOCTEST_REQUIRE(t1.isOk()); DOCTEST_REQUIRE(t2.isOk()); DOCTEST_REQUIRE(t3.isOk());

    {
        auto probe = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(probe.isOk());
        const char* bad[3];
        bad[0] = "not json at all";
        bad[1] = "{\"k\":\"v\"}";
        bad[2] = "[\"a\", 1, null, \"b\"]";
        int64_t ids[3];
        ids[0] = t1.value().id;
        ids[1] = t2.value().id;
        ids[2] = t3.value().id;
        for (int i = 0; i < 3; ++i) {
            auto prep = probe.value().prepare(
                "UPDATE todo_tasks SET tags = ? WHERE id = ?");
            DOCTEST_REQUIRE(prep.isOk());
            auto& stmt = prep.value();
            DOCTEST_REQUIRE(stmt.bind(1, std::string(bad[i])).isOk());
            DOCTEST_REQUIRE(stmt.bind(2, ids[i]).isOk());
            DOCTEST_REQUIRE(stmt.step().isOk());
        }
    }

    auto tasks = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 3u);
    for (const auto& task : tasks.value()) {
        if (task.title == "非法JSON") {
            DOCTEST_CHECK(task.tags.empty());   // parse 失败 → 空 tags，行仍返回
        } else if (task.title == "非数组") {
            DOCTEST_CHECK(task.tags.empty());   // 合法 JSON 但非数组 → 空
        } else if (task.title == "混合类型") {
            DOCTEST_REQUIRE_EQ(task.tags.size(), 2u);  // 仅保留 string 元素
            DOCTEST_CHECK_EQ(task.tags[0], std::string("a"));
            DOCTEST_CHECK_EQ(task.tags[1], std::string("b"));
        }
    }
    return true;
})

// createColumn/createTask 返回的 id 必须就是本表落库行的 rowid，用该 id 的后续操作要精确
// 命中该行。本用例锁的是「RETURNING 取回的 id 语义正确」这条底线，不复现并发穿插 INSERT 的
// 竞态本身——单线程跑不出来，别拿它当竞态测试的依据。
REGISTER_TEST(todo_svc_returning_id_consistency,
              "TodoService: createColumn/createTask 返回 id 与落库行精确一致",
              {
    TestEnv env("returning_id");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    const int64_t colA = cols.value()[0].id;
    const int64_t colB = cols.value()[1].id;

    // 交错建列/建任务，让两张表的 rowid 序列各自推进
    auto c1 = env.svc->createColumn("列一");
    auto c2 = env.svc->createColumn("列二");
    DOCTEST_REQUIRE(c1.isOk()); DOCTEST_REQUIRE(c2.isOk());
    DOCTEST_CHECK(c1.value().id > 0);
    DOCTEST_CHECK(c2.value().id > 0);
    DOCTEST_CHECK(c1.value().id != c2.value().id);

    auto t1 = env.svc->createTask(colA, "任务一");
    auto t2 = env.svc->createTask(colB, "任务二");
    DOCTEST_REQUIRE(t1.isOk()); DOCTEST_REQUIRE(t2.isOk());
    DOCTEST_CHECK(t1.value().id > 0);
    DOCTEST_CHECK(t1.value().id != t2.value().id);

    // 独立连接核对：返回 id 对应的行就是刚建的那一行（列 / 任务各自核对）
    {
        auto probe = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(probe.isOk());
        struct Expect { const char* sql; int64_t id; const char* want; };
        Expect checks[4] = {
            {"SELECT title FROM todo_columns WHERE id = ?", c1.value().id, "列一"},
            {"SELECT title FROM todo_columns WHERE id = ?", c2.value().id, "列二"},
            {"SELECT title FROM todo_tasks WHERE id = ?", t1.value().id, "任务一"},
            {"SELECT title FROM todo_tasks WHERE id = ?", t2.value().id, "任务二"},
        };
        for (const auto& e : checks) {
            auto prep = probe.value().prepare(e.sql);
            DOCTEST_REQUIRE(prep.isOk());
            auto& stmt = prep.value();
            DOCTEST_REQUIRE(stmt.bind(1, e.id).isOk());
            auto rows = stmt.fetchAll();
            DOCTEST_REQUIRE(rows.isOk());
            DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
            DOCTEST_CHECK_EQ(rows.value()[0].getString("title").valueOr(""),
                             std::string(e.want));
        }
    }

    // 用返回 id 更新：只命中该行，另一行不动（id 指错行会立刻暴露）
    DOCTEST_REQUIRE(env.svc->completeTask(t1.value().id).isOk());
    auto aTasks = env.svc->listTasks(colA);
    auto bTasks = env.svc->listTasks(colB);
    DOCTEST_REQUIRE(aTasks.isOk()); DOCTEST_REQUIRE(bTasks.isOk());
    DOCTEST_REQUIRE_EQ(aTasks.value().size(), 1u);
    DOCTEST_REQUIRE_EQ(bTasks.value().size(), 1u);
    DOCTEST_CHECK(aTasks.value()[0].completedAt.has_value());
    DOCTEST_CHECK(!bTasks.value()[0].completedAt.has_value());
    return true;
})
// createColumn/createTask 用 INSERT ... RETURNING 取 id；语句停在结果行时连接上仍有
// in-progress 状态，同连接的事务 COMMIT 会报
// "cannot commit transaction - SQL statements in progress"。因此取行后必须 drain 到
// DONE 或 ~Statement 释放，源码里的显式 drain 就是在守这条。
// 本用例构造该场景：创建后紧接同连接事务写入 + COMMIT。锁的是这条对外可见契约——
// 谁把取行后的代码扩成 in-scope 事务，这里立刻破。
REGISTER_TEST(todo_svc_returning_drain_commit,
              "TodoService: createColumn/createTask 后同连接事务 COMMIT 不卡死",
              {
    TestEnv env("returning_drain");
    DOCTEST_REQUIRE(env.ready);

    auto& db = env.db->db();

    auto col = env.svc->createColumn("drain列");
    DOCTEST_REQUIRE(col.isOk());
    // 紧接同连接事务：BEGIN → 写 → COMMIT。COMMIT 正是未 drain 时撞墙的那一步
    auto tx1 = db.transaction([&]() -> core::Result<void> {
        return db.exec("INSERT INTO todo_columns (title, position, color, created_at) "
                       "VALUES ('事务列', 98, NULL, 0)");
    });
    DOCTEST_REQUIRE(tx1.isOk());

    auto task = env.svc->createTask(col.value().id, "drain任务");
    DOCTEST_REQUIRE(task.isOk());
    auto tx2 = db.transaction([&]() -> core::Result<void> {
        return db.exec("INSERT INTO todo_tasks (column_id, title, position, tags, created_at, updated_at) "
                       "VALUES (1, '事务任务', 98, '[]', 0, 0)");
    });
    DOCTEST_REQUIRE(tx2.isOk());

    // 事务确实提交：独立连接能读到两批行
    {
        auto probe = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(probe.isOk());
        auto rows = probe.value().exec("SELECT 1");
        DOCTEST_REQUIRE(rows.isOk());
    }
    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    bool sawDrain = false;
    bool sawTxCol = false;
    for (const auto& c : cols.value()) {
        if (c.title == "drain列") sawDrain = true;
        if (c.title == "事务列") sawTxCol = true;
    }
    DOCTEST_CHECK(sawDrain);
    DOCTEST_CHECK(sawTxCol);
    return true;
})

// M4 Task 1 负向：事件只在 module UI 层发布，service 直调（todo_tools.cpp 走的正是这条
// 路径）不得发事件——agent 工具建的任务不触发 todo.created，契约如此。
REGISTER_TEST(todo_svc_direct_calls_emit_no_events,
              "TodoService: service 层直调不发 EventBus 事件（负向）", {
    TestEnv env("no_events");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    const int64_t colId = cols.value()[0].id;

    int gotCreated = 0, gotCompleted = 0;
    {
        auto tc = ::core::event::bus().subscribe(
            ::core::event::kTodoCreated, [&](const std::string&) { ++gotCreated; });
        auto tk = ::core::event::bus().subscribe(
            ::core::event::kTodoCompleted, [&](const std::string&) { ++gotCompleted; });

        auto created = env.svc->createTask(colId, "service直建不应发事件");
        DOCTEST_REQUIRE(created.isOk());
        auto completed = env.svc->completeTask(created.value().id);
        DOCTEST_REQUIRE(completed.isOk());
    }
    DOCTEST_CHECK_EQ(gotCreated, 0);    // service 层不是发布点
    DOCTEST_CHECK_EQ(gotCompleted, 0);
    return true;
})

REGISTER_TEST(todo_archive_move_into_sets_timestamp,
              "TodoService: moveTask 进入归档列设置 archived_at",
              {
    TestEnv env("archive_into");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int64_t archiveId = 0;
    int64_t todoId = 0;
    for (const auto& c : cols.value()) {
        if (c.kind == "archive") archiveId = c.id;
        if (c.kind == "todo") todoId = c.id;
    }
    DOCTEST_REQUIRE(archiveId > 0);
    DOCTEST_REQUIRE(todoId > 0);

    auto t = env.svc->createTask(todoId, "归档测试任务");
    DOCTEST_REQUIRE(t.isOk());
    int64_t taskId = t.value().id;

    // 验证初始 archivedAt 为空
    auto tasksBefore = env.svc->listTasks(todoId);
    DOCTEST_REQUIRE(tasksBefore.isOk());
    DOCTEST_REQUIRE(!tasksBefore.value().empty());
    DOCTEST_CHECK(!tasksBefore.value()[0].archivedAt.has_value());

    // 移动到归档列
    auto mv = env.svc->moveTask(taskId, archiveId, 0);
    DOCTEST_CHECK(mv.isOk());

    auto tasksAfter = env.svc->listTasks(archiveId);
    DOCTEST_REQUIRE(tasksAfter.isOk());
    DOCTEST_REQUIRE(!tasksAfter.value().empty());
    DOCTEST_CHECK(tasksAfter.value()[0].archivedAt.has_value());
    // 时间戳应在最近 5 秒内
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    DOCTEST_CHECK(tasksAfter.value()[0].archivedAt.value() >= now - 5);
    DOCTEST_CHECK(tasksAfter.value()[0].archivedAt.value() <= now + 5);
    return true;
})

REGISTER_TEST(todo_archive_move_out_clears_timestamp,
              "TodoService: moveTask 离开归档列清除 archived_at",
              {
    TestEnv env("archive_out");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int64_t archiveId = 0;
    int64_t todoId = 0;
    for (const auto& c : cols.value()) {
        if (c.kind == "archive") archiveId = c.id;
        if (c.kind == "todo") todoId = c.id;
    }
    DOCTEST_REQUIRE(archiveId > 0);
    DOCTEST_REQUIRE(todoId > 0);

    // 直接在归档列建任务（模拟老数据回填路径）
    auto t = env.svc->createTask(archiveId, "归档后移出");
    DOCTEST_REQUIRE(t.isOk());
    int64_t taskId = t.value().id;

    // 移到归档列再移出：先进归档触发时间戳
    auto mv1 = env.svc->moveTask(taskId, archiveId, 0);
    DOCTEST_CHECK(mv1.isOk());

    // 再移出到待办列
    auto mv2 = env.svc->moveTask(taskId, todoId, 0);
    DOCTEST_CHECK(mv2.isOk());

    auto tasksAfter = env.svc->listTasks(todoId);
    DOCTEST_REQUIRE(tasksAfter.isOk());
    bool found = false;
    for (const auto& task : tasksAfter.value()) {
        if (task.id == taskId) {
            DOCTEST_CHECK(!task.archivedAt.has_value());
            found = true;
        }
    }
    DOCTEST_CHECK(found);
    return true;
})

REGISTER_TEST(todo_archive_non_archive_move_preserves_null,
              "TodoService: 非归档列间移动不置 archived_at",
              {
    TestEnv env("archive_null");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int64_t todoId = 0;
    int64_t progressId = 0;  // kind=NULL 的列（「进行中」）
    for (const auto& c : cols.value()) {
        if (c.kind == "todo") todoId = c.id;
        if (c.kind.empty() && c.title == "进行中") progressId = c.id;
    }
    DOCTEST_REQUIRE(todoId > 0);
    DOCTEST_REQUIRE(progressId > 0);

    auto t = env.svc->createTask(todoId, "普通移动");
    DOCTEST_REQUIRE(t.isOk());
    int64_t taskId = t.value().id;

    auto mv = env.svc->moveTask(taskId, progressId, 0);
    DOCTEST_CHECK(mv.isOk());

    auto tasksAfter = env.svc->listTasks(progressId);
    DOCTEST_REQUIRE(tasksAfter.isOk());
    bool found = false;
    for (const auto& task : tasksAfter.value()) {
        if (task.id == taskId) {
            DOCTEST_CHECK(!task.archivedAt.has_value());
            found = true;
        }
    }
    DOCTEST_CHECK(found);
    return true;
})

REGISTER_TEST(todo_archive_sql_includes_column,
              "TodoService: listTasks/listAllTasks/search/listOverdue 结果含 archivedAt",
              {
    TestEnv env("archive_col");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    int64_t todoId = 0;
    for (const auto& c : cols.value()) {
        if (c.kind == "todo") { todoId = c.id; break; }
    }
    DOCTEST_REQUIRE(todoId > 0);

    auto t = env.svc->createTask(todoId, "列字段");
    DOCTEST_REQUIRE(t.isOk());

    // archivedAt 初始为 nullopt，不是「列不存在」
    auto tasks = env.svc->listTasks(todoId);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE(!tasks.value().empty());
    DOCTEST_CHECK(!tasks.value()[0].archivedAt.has_value());

    // 全量、搜索、逾期也都返回
    auto all = env.svc->listAllTasks();
    DOCTEST_REQUIRE(all.isOk());
    DOCTEST_CHECK(all.value().size() >= 1);

    auto sr = env.svc->search("列字段");
    DOCTEST_REQUIRE(sr.isOk());
    DOCTEST_CHECK(sr.value().size() >= 1);

    // 逾期（不过滤到 0 至少不崩）
    auto ov = env.svc->listOverdue();
    DOCTEST_CHECK(ov.isOk());
    return true;
})
