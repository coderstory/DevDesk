// tests/test_modules/test_todo_doc_link.cpp
// UAT6.2 H31：todo 卡片关联 1 篇 md 文档作详细说明。
// 覆盖（每条例注明防什么事故）：
//   - service 层：docPath 置值/解除落库往返、NULL 行读回空串；
//   - 迁移层：老库补 doc_path 列幂等 + 行数据保留；
//   - 渲染层：卡片正面文档图标有关联才出现、只提示不可点；
//   - 全链层：详情选文档 → 保存携带 docPath；点击关联文档 → 跳转负载
//     open:<相对路径>；文档失联 → 灰态禁点且不自动解除关联。
// 注意：本文件用例随 UAT6.2 波 4 交付时按指令「已写未执行」，由主会话统一收口跑。
#include "doctest.h"
#include "test_support.h"

#include "core/database.h"
#include "core/markdown_notes_source.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "core/service_locator.h"
#include "module_todo/todo_dialogs.h"
#include "module_todo/todo_service.h"

#include <eui/dsl.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" ::core::Module* create_todo_module();

namespace {

constexpr const char* kDbPrefix = "devdesk_test_tododoc_";

struct TestEnv : test_support::TestEnv {
    std::unique_ptr<module::todo::TodoService> svc;

    explicit TestEnv(const std::string& tag)
        : test_support::TestEnv(kDbPrefix, tag, /*registerAppDb=*/true) {
        svc = std::make_unique<module::todo::TodoService>(db->db());
    }
};

// 无头 compose 一帧（test_todo_ui.cpp 同款）
struct Frame {
    eui::Ui ui;
    eui::Screen screen;
    ::core::Module* mod = nullptr;

    Frame(::core::Module* m, float w = 1600.0f, float h = 1000.0f) : mod(m) {
        screen.width = w;
        screen.height = h;
        refresh();
    }
    void refresh() {
        ui.begin("todo_doc_frame");
        mod->compose(ui, screen);
        ui.end();
    }
};

// 笔记提供方桩（不建真实 MarkdownService/文件系统）：missingPath 非空时该路径
// 判不存在（模拟文档被删/改名）
struct StubNotesSource : public ::core::IMarkdownNotesSource {
    std::vector<::core::NoteRef> notes;
    std::string missingPath;

    ::core::Result<std::vector<::core::NoteRef>> listNotes() override {
        return ::core::Result<std::vector<::core::NoteRef>>::ok(notes);
    }
    bool noteExists(const std::string& relPath) override {
        return relPath != missingPath;
    }
};

// Markdown 页占位模块：只用于让 Router 接受 navigateTo(Markdown) 挂负载
struct StubMdModule : ::core::Module {
    ::core::PageId pageId() const override { return ::core::PageId::Markdown; }
    std::string displayName() const override { return "md"; }
    std::string iconGlyph() const override { return ""; }
    void compose(eui::Ui&, const eui::Screen&) override {}
};

// 详情弹窗「点击关联文档」触发跳转后，Router 上挂给 Markdown 页的负载应为
// "open:<相对路径>"——与 Markdown 侧 consumeNavPayload 的 open: 前缀契约同源。
// 返回 bool（断言宏展开含 return false，只能住在返回 bool 的函数里）
bool clickCardAndOpenDetail(Frame& frame, const char* cardId) {
    const auto* card = frame.ui.find(cardId);
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(card->onClick != nullptr);
    card->onClick();
    frame.refresh();
    return true;
}

} // namespace

// —— service 层 ——

// 防的事故：关联只进表单不进库（跳转打开错文档）；解除后残留旧路径（详情仍
// 显示已解除的文档并跳转打开它）。
REGISTER_TEST(todo_doclink_svc_set_and_clear,
              "TodoService: docPath 置值/解除落库往返", {
    TestEnv env("svc_set_clear");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto created = env.svc->createTask(cols.value()[0].id, "关联文档任务");
    DOCTEST_REQUIRE(created.isOk());
    DOCTEST_CHECK(created.value().docPath.empty());  // 新建默认未关联

    module::todo::Task linked = created.value();
    linked.docPath = "notes/设计稿.md";
    DOCTEST_REQUIRE(env.svc->updateTask(linked).isOk());

    auto got = env.svc->listTasks(cols.value()[0].id);
    DOCTEST_REQUIRE(got.isOk());
    DOCTEST_REQUIRE_EQ(got.value().size(), 1u);
    DOCTEST_CHECK_EQ(got.value()[0].docPath, std::string("notes/设计稿.md"));

    // 解除关联 = 空串写回
    module::todo::Task cleared = got.value()[0];
    cleared.docPath.clear();
    DOCTEST_REQUIRE(env.svc->updateTask(cleared).isOk());

    auto got2 = env.svc->listTasks(cols.value()[0].id);
    DOCTEST_REQUIRE(got2.isOk());
    DOCTEST_CHECK(got2.value()[0].docPath.empty());
    return true;
})

// 防的事故：历史库中手工置 NULL 的 doc_path 行把任务列表整批读炸（详情打不开）。
REGISTER_TEST(todo_doclink_svc_null_tolerated,
              "TodoService: doc_path NULL 行读回空串（不炸解析）", {
    TestEnv env("null_doc");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto created = env.svc->createTask(cols.value()[0].id, "NULL 行任务");
    DOCTEST_REQUIRE(created.isOk());

    DOCTEST_REQUIRE(env.handle().exec(
        "UPDATE todo_tasks SET doc_path = NULL WHERE id = " +
        std::to_string(created.value().id)).isOk());

    auto tasks = env.svc->listTasks(cols.value()[0].id);
    DOCTEST_REQUIRE(tasks.isOk());
    DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
    DOCTEST_CHECK(tasks.value()[0].docPath.empty());
    return true;
})

// —— 迁移层 ——

// 防的事故：老库升级丢任务行 / 重复 ALTER 报 duplicate column 炸启动 /
// 已关联值被迁移二次覆盖。
REGISTER_TEST(todo_doclink_migration_idempotent,
              "schema: 老库补 doc_path 列幂等 + 行数据保留", {
    auto path = test_support::tempDbPath(kDbPrefix, "migrate");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();
        // 退回 H31 之前老库形态：doc_path 列不存在
        DOCTEST_REQUIRE(db.exec(
            "ALTER TABLE todo_tasks DROP COLUMN doc_path").isOk());
        DOCTEST_REQUIRE(db.exec(
            "INSERT INTO todo_tasks (column_id, title, priority, position, created_at, updated_at) "
            "VALUES ((SELECT id FROM todo_columns WHERE kind = 'todo'), '老任务', 0, 0, 0, 0)").isOk());
    }

    {
        // 启动路径 migrate → 补列
        auto r = core::Database::open(path);
        DOCTEST_REQUIRE(r.isOk());
        auto& db = r.value().db();

        auto countDocCol = [&db]() -> int64_t {
            auto p = db.prepare(
                "SELECT COUNT(*) FROM pragma_table_info('todo_tasks') "
                "WHERE name = 'doc_path'");
            if (p.isFail()) return -1;
            auto rows = p.value().fetchAll();
            if (rows.isFail() || rows.value().empty()) return -1;
            return std::get<int64_t>(rows.value()[0].values[0]);
        };
        DOCTEST_CHECK_EQ(countDocCol(), 1);

        // 老行保留且默认未关联
        auto prep = db.prepare("SELECT doc_path FROM todo_tasks WHERE title = '老任务'");
        DOCTEST_REQUIRE(prep.isOk());
        auto rows = prep.value().fetchAll();
        DOCTEST_REQUIRE(rows.isOk());
        DOCTEST_REQUIRE_EQ(rows.value().size(), 1u);
        DOCTEST_CHECK_EQ(rows.value()[0].getString("doc_path").valueOr("?"),
                         std::string(""));

        // 置值后再次 migrate：幂等（列不重复）且值不丢
        DOCTEST_REQUIRE(db.exec(
            "UPDATE todo_tasks SET doc_path = 'notes/老文档.md' "
            "WHERE title = '老任务'").isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_REQUIRE(r.value().migrate().isOk());
        DOCTEST_CHECK_EQ(countDocCol(), 1);

        auto prep2 = db.prepare("SELECT doc_path FROM todo_tasks WHERE title = '老任务'");
        DOCTEST_REQUIRE(prep2.isOk());
        auto rows2 = prep2.value().fetchAll();
        DOCTEST_REQUIRE(rows2.isOk());
        DOCTEST_CHECK_EQ(rows2.value()[0].getString("doc_path").valueOr("?"),
                         std::string("notes/老文档.md"));
    }

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
    return true;
})

// —— 渲染层 ——

// 防的事故：图标常显（无关联也画，误导）或漏显（有关联不画，用户看不到关联
// 存在）；图标误挂点击（跳转入口泄露到卡片正面，违背「跳转只从详情进」）。
REGISTER_TEST(todo_doclink_card_icon_only_when_linked,
              "todo_ui: 卡片正面文档图标有关联才出现且不可点", {
    TestEnv env("card_icon");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto a = env.svc->createTask(cols.value()[0].id, "无关联卡");
    auto b = env.svc->createTask(cols.value()[0].id, "有关联卡");
    DOCTEST_REQUIRE(a.isOk() && b.isOk());
    {
        module::todo::Task t = b.value();
        t.docPath = "notes/看板说明.md";
        DOCTEST_REQUIRE(env.svc->updateTask(t).isOk());
    }

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame frame(mod.get());
        // 默认待办列 = col.0；无关联卡不出图标
        DOCTEST_CHECK(frame.ui.find("todo.col.0.card.0.docicon") == nullptr);
        const auto* icon = frame.ui.find("todo.col.0.card.1.docicon");
        DOCTEST_REQUIRE(icon != nullptr);
        // 图标只提示不可点：不挂 onClick（跳转只从详情进）
        DOCTEST_CHECK(icon->onClick == nullptr);
    }
    mod->onStop();
    return true;
})

// —— 详情表单纯状态 ——

// 防的事故：reset/collect 漏 docPath（打开再保存关联被静默清空）；dirty 漏比
// docPath（解除关联后 backdrop 守卫不报警，用户以为改动已存）。
REGISTER_TEST(todo_doclink_detail_reset_collect_dirty,
              "DetailDialog: reset/collect/dirty 携带 docPath", {
    module::todo::Task src;
    src.id = 1;
    src.title = "任务";
    src.docPath = "notes/x.md";

    module::todo::ui::DetailDialog d;
    d.reset(src);
    DOCTEST_CHECK_EQ(d.docPath, std::string("notes/x.md"));

    std::vector<module::todo::Task> tasks{src};

    // 解除后 collect 必须携带空串（保存即落库解除）
    d.docPath.clear();
    d.docTitle.clear();
    auto out = d.collect(tasks);
    DOCTEST_CHECK(out.docPath.empty());
    // 与库差异可检出（守卫不失灵）
    DOCTEST_CHECK(d.dirty(tasks));

    // 改回一致 → 不脏
    d.docPath = "notes/x.md";
    DOCTEST_CHECK(!d.dirty(tasks));
    return true;
})

// —— 全链：选文档 → 跳转负载；删文档 → 灰态 ——

// 防的事故：跳转负载不带目标路径（Markdown 侧打开错文档或打不开）；文档删
// 除/改名后详情仍可点跳转（跳空）；灰态自动解除关联（用户数据被静默改）。
REGISTER_TEST(todo_doclink_detail_jump_and_missing_gray,
              "todo_ui: 详情点关联文档 → open: 负载；失联 → 灰态禁点不解除", {
    TestEnv env("detail_chain");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto created = env.svc->createTask(cols.value()[0].id, "链任务");
    DOCTEST_REQUIRE(created.isOk());
    {
        module::todo::Task t = created.value();
        t.docPath = "notes/存在.md";
        DOCTEST_REQUIRE(env.svc->updateTask(t).isOk());
    }

    // 路由桩：注册 Markdown 页占位模块，验证负载可达（openLinkedDoc 经 appRouter）
    StubMdModule stubMd;
    ::core::Router router;
    router.registerModule(&stubMd);
    ::core::registerAppRouter(&router);

    StubNotesSource source;
    source.notes = {{"notes/存在.md", "存在文档"}};
    ::core::ScopedService<::core::IMarkdownNotesSource> reg(&source);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame frame(mod.get());
        DOCTEST_CHECK(clickCardAndOpenDetail(frame, "todo.col.0.card.0.bg"));

        // 已关联且存在：doc.open 可点，点击 → open:<相对路径> 负载挂到 Markdown 页
        const auto* open = frame.ui.find("todo.dialog.detail.doc.open.bg");
        DOCTEST_REQUIRE(open != nullptr);
        DOCTEST_REQUIRE(open->onClick != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.doc.missing") == nullptr);
        open->onClick();
        DOCTEST_CHECK_EQ(router.consumePendingPayload(::core::PageId::Markdown),
                         std::string("open:notes/存在.md"));

        // 文档失联（删除/改名）：下一帧灰态 + 点击禁用，库中关联仍在
        source.missingPath = "notes/存在.md";
        frame.refresh();
        const auto* missing = frame.ui.find("todo.dialog.detail.doc.missing");
        DOCTEST_REQUIRE(missing != nullptr);
        const auto* open2 = frame.ui.find("todo.dialog.detail.doc.open.bg");
        DOCTEST_REQUIRE(open2 != nullptr);
        DOCTEST_CHECK(open2->onClick == nullptr);  // 灰态禁点

        auto tasks = env.svc->listTasks(cols.value()[0].id);
        DOCTEST_REQUIRE(tasks.isOk());
        DOCTEST_CHECK_EQ(tasks.value()[0].docPath, std::string("notes/存在.md"));
    }
    mod->onStop();
    ::core::registerAppRouter(nullptr);
    return true;
})

// 防的事故：选择器选中不生效（详情仍显示未关联 / 保存落错路径）；清单不新鲜
//（改名/删除后仍能选到旧文档）。
REGISTER_TEST(todo_doclink_picker_select_and_save,
              "todo_ui: 详情选文档（选择器行点击）→ 保存落库 docPath", {
    TestEnv env("picker");
    DOCTEST_REQUIRE(env.ready);

    auto cols = env.svc->listColumns();
    DOCTEST_REQUIRE(cols.isOk());
    auto created = env.svc->createTask(cols.value()[0].id, "选文档任务");
    DOCTEST_REQUIRE(created.isOk());

    StubNotesSource source;
    source.notes = {{"n/a.md", "A 文档"}, {"n/b.md", "B 文档"}};
    ::core::ScopedService<::core::IMarkdownNotesSource> reg(&source);

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();
    {
        Frame frame(mod.get());
        DOCTEST_CHECK(clickCardAndOpenDetail(frame, "todo.col.0.card.0.bg"));

        // 未关联 → 选择入口；点击 → 选择器开（清单来自提供方）
        const auto* pick = frame.ui.find("todo.dialog.detail.doc.pick.bg");
        DOCTEST_REQUIRE(pick != nullptr);
        DOCTEST_REQUIRE(pick->onClick != nullptr);
        pick->onClick();
        frame.refresh();

        const auto* item1 = frame.ui.find("todo.dialog.docpick.item.1");
        DOCTEST_REQUIRE(item1 != nullptr);
        DOCTEST_REQUIRE(item1->onClick != nullptr);
        item1->onClick();  // 选「B 文档」
        frame.refresh();

        // 详情切换到已关联形态（解除按钮出现）
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.doc.open.bg") != nullptr);
        DOCTEST_CHECK(frame.ui.find("todo.dialog.detail.doc.unlink") != nullptr);

        // 保存 → docPath 落库
        const auto* save = frame.ui.find("todo.dialog.detail.save.bg");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_REQUIRE(save->onClick != nullptr);
        save->onClick();

        auto tasks = env.svc->listTasks(cols.value()[0].id);
        DOCTEST_REQUIRE(tasks.isOk());
        DOCTEST_REQUIRE_EQ(tasks.value().size(), 1u);
        DOCTEST_CHECK_EQ(tasks.value()[0].docPath, std::string("n/b.md"));
    }
    mod->onStop();
    return true;
})
