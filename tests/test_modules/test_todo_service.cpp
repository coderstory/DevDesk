// tests/test_modules/test_todo_service.cpp
// TodoService 单元测试（详见 spec §8.2）
#include "doctest.h"
#include "core/database.h"
#include "core/sqlite_db.h"
#include "module_todo/todo_service.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_todosvc_" + tag + "_" +
                std::to_string(counter) + ".db";
    return test_temp::tempPath(name);
}

// 一次性环境：开临时 db → migrate → 创建 TodoService
// RAII 自动清理
struct TestEnv {
    std::filesystem::path path;
    std::unique_ptr<core::Database> db;
    std::unique_ptr<module::todo::TodoService> svc;
    bool ready = false;

    explicit TestEnv(const std::string& tag) {
        path = tempDbPath(tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) return;
        if (r.unsafeValue().migrate().isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        svc = std::make_unique<module::todo::TodoService>(db->db());
        ready = true;
    }

    ~TestEnv() {
        db.reset();
        svc.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

REGISTER_TEST(todo_svc_create_list_columns,
              "TodoService: createColumn + listColumns",
              {
    TestEnv env("create_list_columns");
    DOCTEST_REQUIRE(env.ready);

    // migration 已插入 4 个默认列
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

REGISTER_TEST(todo_svc_set_column_color,
              "TodoService: setColumnColor 设置/清除/读回",
              {
    TestEnv env("color");
    DOCTEST_REQUIRE(env.ready);

    // 新建列（color NULL）→ 设为预设 token → 读回
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

    // 再改一次（覆盖旧值）+ 清除（nullopt → NULL）
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

    // 迁移种子列的 color 原样可读（'claude-clay' 等 token）
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

    // 先在该列下放一个任务
    auto t1 = env.svc->createTask(lastId, "待删除列里的任务");
    DOCTEST_REQUIRE(t1.isOk());

    // 删除列
    auto r = env.svc->deleteColumn(lastId);
    DOCTEST_CHECK(r.isOk());

    // 验证列消失
    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value().size(), 3u);

    // 验证任务也被 CASCADE 删了
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

REGISTER_TEST(todo_svc_move_task,
              "TodoService: moveTask 跨列 + 同列 + position 重排",
              {
    TestEnv env("move");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    int64_t colA = list.value()[0].id;  // 待办
    int64_t colB = list.value()[1].id;  // 进行中

    // 在 A 列放 3 个任务
    auto ta = env.svc->createTask(colA, "A1");
    DOCTEST_REQUIRE(ta.isOk());
    auto tb = env.svc->createTask(colA, "A2");
    DOCTEST_REQUIRE(tb.isOk());
    auto tc = env.svc->createTask(colA, "A3");
    DOCTEST_REQUIRE(tc.isOk());
    int64_t a2Id = tb.value().id;

    // 在 B 列放 2 个任务
    auto td = env.svc->createTask(colB, "B1");
    DOCTEST_REQUIRE(td.isOk());
    auto te = env.svc->createTask(colB, "B2");
    DOCTEST_REQUIRE(te.isOk());

    // 把 A2 (position=1) 移到 B 列的 position=1
    auto mv = env.svc->moveTask(a2Id, colB, 1);
    DOCTEST_CHECK(mv.isOk());

    // 验证 A 列剩 A1(0), A3(1)
    auto aTasks = env.svc->listTasks(colA);
    DOCTEST_REQUIRE(aTasks.isOk());
    DOCTEST_CHECK_EQ(aTasks.value().size(), 2u);
    DOCTEST_CHECK_EQ(aTasks.value()[0].title, std::string("A1"));
    DOCTEST_CHECK_EQ(aTasks.value()[1].title, std::string("A3"));

    // 验证 B 列有 B1(0), A2(1), B2(2)
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

    // 完成
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

    // 取消完成
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

    // 构造更新后的 Task
    module::todo::Task updated;
    updated.id = t.value().id;
    updated.columnId = colId;
    updated.title = "更新后标题";
    updated.description = std::string("详细描述");
    updated.priority = module::todo::Priority::High;
    updated.dueAt = static_cast<int64_t>(1234567890);
    updated.tags = (std::vector<std::string>{"work", "urgent", "P0"});
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
    DOCTEST_CHECK_EQ(static_cast<int>(got.priority), 3);  // High
    DOCTEST_CHECK(got.dueAt.has_value());
    DOCTEST_CHECK_EQ(got.dueAt.value(), static_cast<int64_t>(1234567890));
    DOCTEST_CHECK_EQ(got.tags.size(), 3u);
    DOCTEST_CHECK_EQ(got.tags[0], std::string("work"));
    DOCTEST_CHECK_EQ(got.tags[2], std::string("P0"));
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

    // 用 description 匹配
    module::todo::Task upd;
    upd.id = t1.value().id;
    upd.columnId = colId;
    upd.title = t1.value().title;
    upd.description = std::string("本周工作总结");
    upd.priority = module::todo::Priority::None;
    upd.tags = {};
    upd.position = 0;
    DOCTEST_CHECK(env.svc->updateTask(upd).isOk());

    // 搜 "周报" → 命中 title
    auto r1 = env.svc->search("周报");
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK_EQ(r1.value().size(), 1u);

    // 搜 "周" → 命中 "写周报" + "开周会"（description 里的 "本周工作总结" 属于 t1，不重复计）
    auto r2 = env.svc->search("周");
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().size() >= 2u);

    // 搜 "bug" → 命中 "修复登录 bug"
    auto r3 = env.svc->search("bug");
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK_EQ(r3.value().size(), 1u);

    // 搜 "不存在" → 空
    auto r4 = env.svc->search("不存在的关键词xyz");
    DOCTEST_REQUIRE(r4.isOk());
    DOCTEST_CHECK_EQ(r4.value().size(), 0u);

    // 空 query → 返回全部
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

    int64_t past = 1000000000;        // 2001 年
    int64_t future = 9999999999;      // 2286 年

    // t1: due_at 过期 + 未完成 → 应出现在 overdue
    module::todo::Task u1 = t1.value();
    u1.dueAt = past;
    DOCTEST_CHECK(env.svc->updateTask(u1).isOk());

    // t2: due_at 过期 + 已完成 → 不应出现
    module::todo::Task u2 = t2.value();
    u2.dueAt = past;
    DOCTEST_CHECK(env.svc->updateTask(u2).isOk());
    DOCTEST_CHECK(env.svc->completeTask(u2.id).isOk());

    // t3: due_at 未过期 → 不应出现
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

    // 反转顺序
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

    // 验证跨列按 column_id 排序
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
    // migration 里的 4 列标题
    DOCTEST_CHECK_EQ(list.value()[0].title, std::string("待办"));
    DOCTEST_CHECK_EQ(list.value()[2].title, std::string("已完成"));

    const int64_t todoCol = list.value()[0].id;
    const int64_t doneCol = list.value()[2].id;

    // 1. 新建任务（落到待办列）
    auto created = env.svc->createTask(todoCol, "验收：写文档");
    DOCTEST_REQUIRE(created.isOk());
    const int64_t taskId = created.value().id;

    auto inTodo = env.svc->listTasks(todoCol);
    DOCTEST_REQUIRE(inTodo.isOk());
    DOCTEST_CHECK_EQ(inTodo.value().size(), 1u);

    // 2. 验证真的写进了 db 文件（另开一条连接读）
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

    // 3. 完成 + 移到「已完成」列
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

    // 4. 再次确认 db 里 completed_at 已写入
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

// 列 kind 语义标记（migration v3）：完成态流转不依赖标题
REGISTER_TEST(todo_svc_column_kind,
              "TodoService: 列 kind 标记回填 + 重命名后保留", {
    TestEnv env("column_kind");
    DOCTEST_REQUIRE(env.ready);

    auto list = env.svc->listColumns();
    DOCTEST_REQUIRE(list.isOk());
    DOCTEST_REQUIRE_EQ(list.value().size(), 4u);
    // 种子列的 kind 回填：待办=todo 已完成=done 其余=空（普通列）
    DOCTEST_CHECK_EQ(list.value()[0].kind, std::string("todo"));
    DOCTEST_CHECK_EQ(list.value()[1].kind, std::string(""));
    DOCTEST_CHECK_EQ(list.value()[2].kind, std::string("done"));
    DOCTEST_CHECK_EQ(list.value()[3].kind, std::string(""));

    // 重命名后 kind 保留（语义不随标题变）
    DOCTEST_REQUIRE(env.svc->renameColumn(list.value()[2].id, "Finished").isOk());
    auto list2 = env.svc->listColumns();
    DOCTEST_REQUIRE(list2.isOk());
    DOCTEST_CHECK_EQ(list2.value()[2].title, std::string("Finished"));
    DOCTEST_CHECK_EQ(list2.value()[2].kind, std::string("done"));

    // 用户新建列 = 普通列（kind 空）
    auto created = env.svc->createColumn("复核中");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_CHECK_EQ(created.value().kind, std::string(""));
    return true;
})

// ---- 审计 2026-09-23 [26]：tags 序列化迁 platform::json 后的往返回归 ----
// 注意：REGISTER_TEST 的 body 是单个宏实参，花括号初始化列表里的顶层逗号
// 会被预处理器当参数分隔符（本文件上方 updateTask 用例的额外括号即为此）。
// 新增用例一律用 push_back / 括号保护规避。

// serializeTags/deserializeTags 是私有方法，经 updateTask → listTasks 公开路径
// 覆盖往返；特殊字符（引号/反斜杠/换行/制表/控制字符/CJK/空串）必须原样恢复。
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

    std::vector<std::string> tags;
    tags.push_back("简单");                          // CJK 原样透传
    tags.push_back("带\"引号\"");                    // 双引号转义
    tags.push_back("反斜杠\\");                      // 反斜杠转义
    tags.push_back("换行\n制表\t");                  // 命名转义
    tags.push_back(std::string(1, '\x01') + "控制"); // \u00xx 数字转义
    tags.push_back("");                              // 空串元素
    tags.push_back(" 前后空格 ");                    // 空格保留

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

    // 空 tags → "[]" → 空 vector（往返清零）
    module::todo::Task cleared = tasks.value()[0];
    cleared.tags = {};
    DOCTEST_CHECK(env.svc->updateTask(cleared).isOk());
    auto tasks2 = env.svc->listTasks(colId);
    DOCTEST_REQUIRE(tasks2.isOk());
    DOCTEST_CHECK(tasks2.value()[0].tags.empty());
    return true;
})

// deserializeTags 容错（经 listTasks 公开路径）：非法 JSON / 非数组 /
// 混合类型数组都不得崩溃或阻断行解析。
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

// ---- 审计 2026-09-23 [7] 残留：INSERT ... RETURNING id 取代 last_insert_rowid ----

// 回归守护：createColumn/createTask 返回的 id 必须就是本表落库行的 rowid，
// 且用该 id 的后续操作精确命中该行（旧实现竞态下 id 可能指错行）。
// 注：旧 bug 需要并发穿插 INSERT 才触发，单线程测试无法复现竞态本身，
// 此测试锁定的是「RETURNING 取回的 id 语义正确」这一底线。
REGISTER_TEST(todo_svc_returning_id_consistency,
              "TodoService: createColumn/createTask 返回 id 与落库行精确一致",
              {
    TestEnv env("returning_id");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    const int64_t colA = cols.value()[0].id;
    const int64_t colB = cols.value()[1].id;

    // 交错建列/任务，让两张表的 rowid 序列各自推进
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

    // 独立连接验证：返回 id 对应的行就是刚创建的那一行（列/任务各自核对）
    {
        auto probe = core::data::SqliteDb::open(env.path);
        DOCTEST_REQUIRE(probe.isOk());
        struct Expect { const char* sql; int64_t id; const char* want; };
        Expect checks[4];
        // 外层括号保护初始化列表逗号不被 REGISTER_TEST 宏拆成实参
        checks[0] = (Expect{"SELECT title FROM todo_columns WHERE id = ?", c1.value().id, "列一"});
        checks[1] = (Expect{"SELECT title FROM todo_columns WHERE id = ?", c2.value().id, "列二"});
        checks[2] = (Expect{"SELECT title FROM todo_tasks WHERE id = ?", t1.value().id, "任务一"});
        checks[3] = (Expect{"SELECT title FROM todo_tasks WHERE id = ?", t2.value().id, "任务二"});
        for (const auto& e : checks) {
            auto prep = probe.value().prepare(e.sql);
            DOCTEST_REQUIRE(prep.isOk());
            auto& stmt = prep.value();
            DOCTEST_REQUIRE(stmt.bind(1, e.id).isOk());
            auto rows = stmt.fetchAll();
            DOCTEST_REQUIRE(rows.isOk());
            DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);   // id 必须存在
            DOCTEST_CHECK_EQ(rows.value()[0].getString("title").valueOr(""),
                             std::string(e.want));         // 且标题吻合
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
// ---- RETURNING-drain 复查（B）：取行后同连接事务 COMMIT 不被卡 ----

// createColumn/createTask 用 INSERT ... RETURNING 取 id，语句停在结果行时
// 连接上有 in-progress 状态，同连接的事务 COMMIT 会报
// "cannot commit transaction - SQL statements in progress"（claude
// createProfile 曾实测触发，产品审计 2026-09-23 [6]）。本用例构造触发场景：
// 创建后紧接同连接事务写入 + COMMIT，必须成功。锁定「drain 到 DONE 或
// ~Statement finalize 释放」两条收口中的对外可见契约；若日后有人把取行后的
// 代码扩成 in-scope 事务（claude 同款事故），配合源码里的显式 drain 可保持
// 该契约不破。
REGISTER_TEST(todo_svc_returning_drain_commit,
              "TodoService: createColumn/createTask 后同连接事务 COMMIT 不卡死",
              {
    TestEnv env("returning_drain");
    DOCTEST_REQUIRE(env.ready);

    auto& db = env.db->db();

    auto col = env.svc->createColumn("drain列");
    DOCTEST_REQUIRE(col.isOk());
    // 紧接同连接事务：BEGIN → 写 → COMMIT（COMMIT 正是未 drain 时撞墙的一步）
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
        auto rows = probe.value().exec("SELECT 1");  // 可读性冒烟
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
    DOCTEST_CHECK(sawTxCol);   // 事务 COMMIT 真的落了库（而不是被静默拒绝）
    return true;
})
