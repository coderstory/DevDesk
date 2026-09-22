// tests/test_core/test_router.cpp
// Router 模块列表只读访问（modules() getter，侧边栏渲染数据源）+
// compose 只渲染当前页（侧边栏布局的前提语义）无头测试
#include "doctest.h"
#include "core/module_interface.h"
#include "core/router.h"

#include <eui/dsl.h>

#include <string>

namespace {

class StubModule : public ::core::Module {
public:
    StubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }

private:
    ::core::PageId page_;
    std::string name_;
};

} // namespace

REGISTER_TEST(router_modules_lists_registered, "router: modules() exposes registered modules read-only", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule settings(::core::PageId::Settings, "settings");
    router.registerModule(&home);
    router.registerModule(&settings);

    const auto& mods = router.modules();
    DOCTEST_CHECK_EQ(mods.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(mods[0]->displayName(), std::string("home"));
    DOCTEST_CHECK(mods[1]->pageId() == ::core::PageId::Settings);
    // 顺序与注册顺序一致（侧边栏按此渲染导航项）
    DOCTEST_CHECK(mods[0] == &home);
    return true;
})

REGISTER_TEST(router_compose_renders_current_page_only, "router: compose renders current page only", {
    ::core::Router router;
    StubModule home(::core::PageId::Home, "home");
    StubModule todo(::core::PageId::Todo, "todo");
    router.registerModule(&home);
    router.registerModule(&todo);

    eui::Screen screen;
    screen.width = 800.0f;
    screen.height = 600.0f;
    eui::Ui ui;

    ui.begin("t1");
    router.compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("stub.home") != nullptr);
    DOCTEST_CHECK(ui.find("stub.todo") == nullptr);

    router.navigateTo(::core::PageId::Todo);
    ui.begin("t2");
    router.compose(ui, screen);
    ui.end();
    DOCTEST_CHECK(ui.find("stub.todo") != nullptr);
    DOCTEST_CHECK(ui.find("stub.home") == nullptr);
    return true;
})
