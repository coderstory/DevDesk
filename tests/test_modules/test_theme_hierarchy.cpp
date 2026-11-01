// tests/test_modules/test_theme_hierarchy.cpp
// 主题层级回归：结构性面板=background，浮动卡片/弹窗=surface，浮层=surfaceElevated。
// 对每类代表元素做无头 compose 后的色值断言（渲染色 == 预期 token），改错 token 即红。
// 跨文件前提：
//   - 页面级全屏不铺底已删（磨砂/半透窗口要求主体透出），页面底色由主窗 clearColor
//     承担 → 各模块 pagebg 必须不存在。
//   - 侧边栏（app.sidebar.bg）同理不在模块断言范围，由 app 层窗口色承担。
#include "doctest.h"
#include "test_support.h"

#include "claude_service.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/proxy_stats.h"
#include "core/theme.h"
#include "module_todo/todo_service.h"
#include "ui_kit/card.h"
#include "ui_kit/glass.h"

#include <eui/dsl.h>
#include <eui/types.h>

#include <cmath>
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

// 玻璃卡/面板带 alpha，只比 RGB 会把「半透」与「实色」判成同色（暗色下 RGB 完全相同）
bool sameRgba(const eui::Color& a, const eui::Color& b) {
    return std::fabs(a.r - b.r) < 1e-4f && std::fabs(a.g - b.g) < 1e-4f &&
           std::fabs(a.b - b.b) < 1e-4f && std::fabs(a.a - b.a) < 1e-4f;
}

// 无头 compose 一帧。theme 每帧从 core::theme::current() 取，切主题后必须
// refresh() 才拿到新色
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
    // 玻璃层：RGB+alpha 全等（alpha 是玻璃感的判据，不能被 RGB 相等蒙过去）
    bool rgbaIs(const std::string& id, const eui::Color& expected) {
        const auto* el = ui.find(id);
        return el != nullptr && sameRgba(el->color, expected);
    }
    // 元素必须不存在
    bool absent(const std::string& id) { return ui.find(id) == nullptr; }
};

constexpr const char* kDbPrefix = "devdesk_test_hier_";

} // namespace

REGISTER_TEST(theme_hierarchy_todo,
              "theme_hierarchy: 看板列=background 卡片/dialog=surface（light+dark）", {
    test_support::TestEnv env(kDbPrefix, "todo", true);
    DOCTEST_REQUIRE(env.ready);

    // 先建列/任务再 onStart：模块 onStart 快照列集，新列 = 迁移默认 4 列后第 5 列
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
        DOCTEST_CHECK(frame.absent("todo.pagebg"));
        // 列容器 = glass L1（比卡更透一档，让卡浮在列上形成两层深度）
        DOCTEST_CHECK(frame.rgbaIs("todo.col.4.bg", ui_kit::glassPanelFill(t)));
        DOCTEST_CHECK(frame.colorIs("todo.col.4.bg", ui_kit::glassPanelFill(t)));
        // 任务卡 = glass L2（不再是不透明白卡，否则动态背景透不上来）
        DOCTEST_CHECK(frame.rgbaIs("todo.col.4.card.0.bg", ui_kit::cardSurface(t)));
        // 弹窗骨架 = componentTokensWeChat 的 surface 槽 = surface（popup 根治批迁移）
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
        // 深色层次：列容器 background ≠ 卡片色（列容器不得等于卡片色）
        const auto& t = core::theme::current();
        DOCTEST_CHECK(!sameRgb(t.colors.background, t.colors.surface));
    }

    mod->onStop();
    core::theme::setDark();
    return true;
})

REGISTER_TEST(theme_hierarchy_markdown,
              "theme_hierarchy: md 左栏/编辑/预览面板=background（light+dark）", {
    // 隔离笔记目录，写一篇笔记保证三栏都渲染
    const auto dir = test_temp::tempPath("devdesk_hier_md");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        FILE* f = std::fopen((dir / "note.md").string().c_str(), "wb");
        DOCTEST_REQUIRE(f != nullptr);
        std::fputs("# 标题\n正文\n", f);
        std::fclose(f);
    }
    test_support::setEnvVar("DEVDESK_NOTES_DIR", dir.string());

    test_support::TestEnv env(kDbPrefix, "md", true);
    DOCTEST_REQUIRE(env.ready);

    std::unique_ptr<::core::Module> mod(create_markdown_module());
    mod->onStart();

    // 默认 Edit 态不渲染预览列：Ctrl+Shift+P 循环切态，序为 编辑→实时→双栏→预览，
    // 故按两次才到双栏
    eui::KeyEvent p = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);
    DOCTEST_CHECK(mod->onKeyEvent(p));  // → 实时（第四态）
    DOCTEST_CHECK(mod->onKeyEvent(p));  // → 双栏

    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        // W8：markdown 页改 wechat 令牌（D-04 逐页切换）。
        DOCTEST_CHECK(frame.absent("md.pagebg"));
        // 批M：树区浅灰面板（纸卡分层）
        DOCTEST_CHECK(frame.colorIs("md.tree.bg", t.colors.hover));
        // 编辑列壳 .bg 已恢复（W8 §4.14：右区 background 底 + 白卡浮起）
        DOCTEST_CHECK(frame.colorIs("md.editor.bg", t.colors.background));
        DOCTEST_CHECK(frame.colorIs("md.preview.bg", t.colors.background));
        // 状态条浮在页面底上，走卡面玻璃层（与纸卡同档），不再是实色 surface
        DOCTEST_CHECK(frame.rgbaIs("md.status.bg", ui_kit::cardSurface(t)));
        return true;
    };

    core::theme::setLight();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }
    core::theme::setDark();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }

    mod->onStop();
    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    std::filesystem::remove_all(dir, ec);
    core::theme::setDark();
    return true;
})

REGISTER_TEST(theme_hierarchy_claude,
              "theme_hierarchy: claude 左栏/详情面板=background（light+dark）", {
    test_support::TestEnv env(kDbPrefix, "claude", true);
    DOCTEST_REQUIRE(env.ready);

    module::claude::ClaudeService svc(env.db->db(),
                                      "DevDeskTestThemeHier");
    module::claude::ClaudeService::setGlobal(&svc);

    std::unique_ptr<::core::Module> mod(create_claude_module());
    mod->onStart();

    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        DOCTEST_CHECK(frame.absent("claude.pagebg"));
        // §5/§4.14 面板底换 background（浅灰，白卡浮起）
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

REGISTER_TEST(theme_hierarchy_home,
              "theme_hierarchy: home 统计卡=玻璃半透 + 页面无自铺底（light+dark）", {
    core::proxy_stats::resetForTest();

    std::unique_ptr<::core::Module> mod(create_home_module());

    const auto checkFrame = [&](Frame& frame,
                                const core::theme::Tokens& t) -> bool {
        DOCTEST_CHECK(frame.absent("home.pagebg"));
        // 五色实底退役：卡底统一卡面玻璃层，alpha 是唯一判据（RGB 与 surface 极近）
        DOCTEST_CHECK(frame.rgbaIs("home.stat.tokens.bg", ui_kit::cardSurface(t)));
        const auto* bg = frame.ui.find("home.stat.tokens.bg");
        DOCTEST_REQUIRE(bg != nullptr);
        DOCTEST_CHECK(bg->color.a < 0.99f);   // 半透：动态背景必须能透上来
        return true;
    };

    core::theme::setLight();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }
    core::theme::setDark();
    { Frame frame(mod.get()); DOCTEST_CHECK(checkFrame(frame, core::theme::current())); }

    core::theme::setDark();
    return true;
})
