// tests/test_core/test_router.cpp
// Router 模块列表只读访问（modules() getter，侧边栏渲染数据源）+
// compose 只渲染当前页（侧边栏布局的前提语义）+
// 跨模块跳转负载（payload）设/取/一次性消费语义/默认空/页面隔离/纯导航不清负载
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

// Router 跨模块跳转负载（payload）单元测试：
// 设/取/一次性消费语义/默认空/页面隔离/纯导航不清负载。

REGISTER_TEST(router_navigate_default_no_payload, "router: 默认导航无负载", {
    core::Router r;
    r.navigateTo(core::PageId::Markdown);
    DOCTEST_CHECK_EQ(r.currentPage(), core::PageId::Markdown);
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Markdown).empty());
    return true;
})

REGISTER_TEST(router_payload_set_and_consume, "router: 设负载并取出", {
    core::Router r;
    r.navigateTo(core::PageId::Markdown, "open:notes/a.md");
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Markdown),
                     std::string("open:notes/a.md"));
    return true;
})

REGISTER_TEST(router_payload_consume_once, "router: 负载一次性消费", {
    core::Router r;
    r.navigateTo(core::PageId::Todo, "action:new_task");
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Todo),
                     std::string("action:new_task"));
    // 取过一次后再次调用返回空（compose 每帧调用不会重复触发）
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Todo).empty());
    return true;
})

REGISTER_TEST(router_payload_page_isolated, "router: 负载页面隔离", {
    core::Router r;
    r.navigateTo(core::PageId::Markdown, "action:new_note");
    // 别页取不走，也不清掉本页挂起的负载
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Todo).empty());
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Home).empty());
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Markdown),
                     std::string("action:new_note"));
    return true;
})

REGISTER_TEST(router_plain_navigate_keeps_pending, "router: 纯导航不清已挂起负载", {
    core::Router r;
    r.navigateTo(core::PageId::Markdown, "open:x.md");
    // 中途的纯导航（默认空 payload）不清除挂起负载
    r.navigateTo(core::PageId::Home);
    r.navigateTo(core::PageId::Markdown);
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Markdown),
                     std::string("open:x.md"));
    return true;
})

REGISTER_TEST(router_payload_overwrite, "router: 新负载覆盖旧负载", {
    core::Router r;
    r.navigateTo(core::PageId::Markdown, "open:old.md");
    r.navigateTo(core::PageId::Markdown, "open:new.md");
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Markdown),
                     std::string("open:new.md"));
    return true;
})

// Router::dispatchKeyEvent：把焦点元素没消费的按键派发到当前页模块，
// 模块返回 true = 已消费；非当前页模块不收；模块默认实现返回 false。

namespace {

class KeyStubModule : public ::core::Module {
public:
    KeyStubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }
    bool onKeyEvent(const eui::KeyEvent& key) override {
        lastKey_ = key.key;
        return consume_;
    }
    ::eui::InputKey lastKey_ = ::eui::InputKey::Unknown;
    bool consume_ = false;

private:
    ::core::PageId page_;
    std::string name_;
};

} // namespace

REGISTER_TEST(router_dispatch_key_event, "router: dispatchKeyEvent 派发到当前页模块", {
    ::core::Router router;
    KeyStubModule md(::core::PageId::Markdown, "md");
    StubModule home(::core::PageId::Home, "home");
    router.registerModule(&md);
    router.registerModule(&home);

    eui::KeyEvent key;
    key.key = ::eui::InputKey::P;
    key.modifiers.control = true;
    key.modifiers.shift = true;

    // 默认 current_=Home → Home 模块无 onKeyEvent 覆盖（默认 false）
    DOCTEST_CHECK(!router.dispatchKeyEvent(key));
    DOCTEST_CHECK(md.lastKey_ == ::eui::InputKey::Unknown);  // 非当前页不收

    router.navigateTo(::core::PageId::Markdown);
    md.consume_ = true;
    DOCTEST_CHECK(router.dispatchKeyEvent(key));
    DOCTEST_CHECK(md.lastKey_ == ::eui::InputKey::P);

    md.consume_ = false;
    DOCTEST_CHECK(!router.dispatchKeyEvent(key));
    return true;
})
