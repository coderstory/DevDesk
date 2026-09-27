// tests/test_modules/test_theme_hierarchy.cpp
// 主题层级回归（防再漂移）：结构性面板用 background（浅色米色 #FAF9F5 /
// 深色暖墨 #1A1816），浮动卡片/弹窗用 surface（白 / #232120），浮层用
// surfaceElevated。曾发生「结构性面板错用卡片白」bug（侧边栏修复后用户
// 反馈其余面板仍白得刺眼）——本文件对每类代表元素做无头 compose 后的
// 色值断言：渲染色 == 预期 token，改错 token 即红。
// 侧边栏（app_sidebar app.sidebar.bg）同样已删自铺底（磨砂/半透要求透出
// 窗口 clearColor，d923f0f），由 app 层窗口色承担，不在模块断言范围。
#include "doctest.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/theme.h"
#include "module_todo/todo_service.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_claude_module();
extern "C" ::core::Module* create_home_module();

namespace {

bool sameRgb(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// 无头 compose 一帧；theme 每帧从 core::theme::current() 取，
// 切主题后重新 compose 即得新色
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
        ui.begin("theme_hier_frame");
        mod->compose(ui, screen);
        ui.end();
    }
    // 元素渲染色 == 预期 token（找不到元素也判失败，防元素被改名后静默跳过）
    bool colorIs(const std::string& id, const eui::Color& expected) {
        const auto* el = ui.find(id);
        return el != nullptr && sameRgb(el->color, expected);
    }
    // 元素必须不存在（页面级全屏不透明底已删，见下）
    bool absent(const std::string& id) { return ui.find(id) == nullptr; }
};

std::filesystem::path tempDbPath(const char* tag) {
    static int counter = 0;
    ++counter;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "devdesk_test_hier_%s_%d.db", tag, counter);
    return std::filesystem::temp_directory_path() / buf;
}

// DEVDESK_NOTES_DIR 隔离（平台差异收在这里；REGISTER_TEST 宏体内不能
// 嵌 #if 指令——pedantic 直接报错）
void setNotesDirEnv(const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s("DEVDESK_NOTES_DIR", value.c_str());
#else
    if (value.empty()) {
        ::unsetenv("DEVDESK_NOTES_DIR");
    } else {
        ::setenv("DEVDESK_NOTES_DIR", value.c_str(), 1);
    }
#endif
}

// RAII：临时库 + 注册全局 appDatabase（析构注销，防污染其他用例）
struct DbEnv {
    std::filesystem::path path;
    std::unique_ptr<core::Database> db;
    bool ready = false;

    explicit DbEnv(const char* tag) {
        path = tempDbPath(tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) return;
        if (r.unsafeValue().migrate().isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        ::core::registerAppDatabase(&db->db());
        ready = true;
    }
    ~DbEnv() {
        ::core::registerAppDatabase(nullptr);
        db.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
};

} // namespace

// ---- 看板列（结构性容器）/ 卡片 / 弹窗：todo 模块三tier 一次断言 ----
REGISTER_TEST(theme_hierarchy_todo,
              "theme_hierarchy: 看板列=background 卡片/dialog=surface（light+dark）", {
    DbEnv env("todo");
    DOCTEST_REQUIRE(env.ready);

    // 先建列/任务再 onStart（模块 onStart 快照列集；新列 = 迁移默认 4 列后第 5 列）
    module::todo::TodoService svc(env.db->db());
    auto col = svc.createColumn("层级列");
    DOCTEST_REQUIRE(col.isOk());
    auto task = svc.createTask(col.value().id, "层级任务");
    DOCTEST_REQUIRE(task.isOk());

    std::unique_ptr<::core::Module> mod(create_todo_module());
    mod->onStart();

    // checkFrame 内的 DOCTEST_CHECK 带 return false（fail-fast），lambda
    // 必须显式 -> bool；调用点再 DOCTEST_CHECK 汇总
    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        // 页面级全屏不透明底已删（磨砂/半透窗口效果要求主体透出，d923f0f），
        // 页面底色由主窗 clearColor 承担——pagebg 必须不存在
        DOCTEST_CHECK(frame.absent("todo.pagebg"));
        DOCTEST_CHECK(frame.colorIs("todo.col.4.bg", t.colors.background));
        // 卡片 = 浮动卡片 → surface（深色 #232120，曾被抬到 elevated）
        DOCTEST_CHECK(frame.colorIs("todo.col.4.card.0.bg", t.colors.surface));
        // dialog 面板 = 浮层 → surface
        DOCTEST_CHECK(frame.colorIs("todo.dialog.newtask.panel.bg",
                                    t.colors.surface));
        return true;
    };

    core::theme::setLight();
    {
        Frame frame(mod.get());
        DOCTEST_CHECK(checkFrame(frame, core::theme::current()));
        // 打开新建任务弹窗后再验一次（真实交互链路；button 的 onClick
        // 挂在 .bg 子元素上）
        const auto* addBtn = frame.ui.find("todo.btn.add.bg");
        DOCTEST_REQUIRE(addBtn != nullptr && addBtn->onClick != nullptr);
        addBtn->onClick();
        frame.refresh();
        DOCTEST_CHECK(frame.colorIs("todo.dialog.newtask.panel.bg",
                                    core::theme::current().colors.surface));
    }

    core::theme::setDark();
    {
        Frame frame(mod.get());
        DOCTEST_CHECK(checkFrame(frame, core::theme::current()));
        // 深色层次：暖墨底 < 卡片 #232120（列容器不得等于卡片色）
        const auto& t = core::theme::current();
        DOCTEST_CHECK(!sameRgb(t.colors.background, t.colors.surface));
    }

    mod->onStop();
    core::theme::setDark();
    return true;
})

// ---- 左栏树 / 编辑区 / 预览区：markdown 结构性面板 ----
REGISTER_TEST(theme_hierarchy_markdown,
              "theme_hierarchy: md 左栏/编辑/预览面板=background（light+dark）", {
    // 隔离笔记目录（DEVDESK_NOTES_DIR），写一篇笔记保证三栏都渲染
    const auto dir = std::filesystem::temp_directory_path() / "devdesk_hier_md";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        FILE* f = std::fopen((dir / "note.md").string().c_str(), "wb");
        DOCTEST_REQUIRE(f != nullptr);
        std::fputs("# 标题\n正文\n", f);
        std::fclose(f);
    }
    setNotesDirEnv(dir.string());

    DbEnv env("md");
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    // 默认 Edit 态不渲染预览列：Ctrl+Shift+P 切到 Split（编辑+预览双栏全出）
    // —— M1 加第四态「实时」后态序为 编辑→实时→双栏→预览，故按两次到双栏
    eui::KeyEvent p;
    p.key = eui::InputKey::P;
    p.modifiers.control = true;
    p.modifiers.shift = true;
    DOCTEST_CHECK(mod->onKeyEvent(p));  // → 实时（第四态）
    DOCTEST_CHECK(mod->onKeyEvent(p));  // → 双栏

    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        DOCTEST_CHECK(frame.absent("md.pagebg"));  // 页面底由 clearColor 承担
        DOCTEST_CHECK(frame.colorIs("md.tree.bg", t.colors.background));
        DOCTEST_CHECK(frame.colorIs("md.editor.bg", t.colors.background));
        DOCTEST_CHECK(frame.colorIs("md.preview.bg", t.colors.background));
        DOCTEST_CHECK(frame.colorIs("md.status.bg", t.colors.background));
        return true;
    };

    core::theme::setLight();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }
    core::theme::setDark();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }

    mod->onStop();
    setNotesDirEnv("");
    std::filesystem::remove_all(dir, ec);
    core::theme::setDark();
    return true;
})

// ---- 左栏 Profile 列表 / 右栏详情面板：claude 结构性面板 ----
REGISTER_TEST(theme_hierarchy_claude,
              "theme_hierarchy: claude 左栏/详情面板=background（light+dark）", {
    DbEnv env("claude");
    DOCTEST_REQUIRE(env.ready);

    module::claude::ClaudeService svc(env.db->db(),
                                      "DevDeskTestThemeHier");
    module::claude::ClaudeService::setGlobal(&svc);

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        DOCTEST_CHECK(frame.absent("claude.pagebg"));  // 页面底由 clearColor 承担
        DOCTEST_CHECK(frame.colorIs("claude.profiles.bg", t.colors.background));
        DOCTEST_CHECK(frame.colorIs("claude.detail.bg", t.colors.background));
        return true;
    };

    core::theme::setLight();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }
    core::theme::setDark();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }

    mod->onStop();
    module::claude::ClaudeService::setGlobal(nullptr);
    core::theme::setDark();
    return true;
})

// ---- 统计卡：浮动卡片留在 surface（白），页面底 background ----
REGISTER_TEST(theme_hierarchy_home,
              "theme_hierarchy: home 统计卡=surface 页面底无自铺底（light+dark）", {
    core::proxy_stats::resetForTest();

    std::unique_ptr<::core::Module> mod(create_home_module());

    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        DOCTEST_CHECK(frame.absent("home.pagebg"));  // 页面底由 clearColor 承担
        // 统计卡/最近编辑卡是浮动卡片 → surface（浅色白），不得跟风变米色
        DOCTEST_CHECK(frame.colorIs("home.stat.tokens.bg", t.colors.surface));
        DOCTEST_CHECK(frame.colorIs("home.recent.bg", t.colors.surface));
        return true;
    };

    core::theme::setLight();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }
    core::theme::setDark();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }

    core::theme::setDark();
    return true;
})
