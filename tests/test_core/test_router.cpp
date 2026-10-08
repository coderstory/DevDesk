// tests/test_core/test_router.cpp
// Router 模块列表只读访问（modules() getter，侧边栏渲染数据源）+
// compose 只渲染当前页（侧边栏布局的前提语义）+
// compose 空态兜底由 app 层注入（core 只派发不绘制，spec §4.2.1 / 审计 CP-01）+
// 跨模块跳转负载（payload）设/取/一次性消费语义/默认空/页面隔离/纯导航不清负载
#include "doctest.h"
#include "test_support.h"   // test_support::pressShortcut —— 快捷键修饰位（平台同源）
#include "core/module_interface.h"
#include "core/router.h"
#include "core/ui_geometry.h"

#include <eui/dsl.h>

#include <limits>
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

// compose 空态兜底（专家审计 2026-09-27 CP-01）：当前页无对应模块时 core 不再
// 自行绘制，改为调用 app 层注入的回调（生产注入点 = app_assembly.cpp 装配段）；
// 未注入则不画、不崩。

REGISTER_TEST(router_compose_calls_injected_fallback,
              "router: 当前页无模块 → 调 app 层注入的兜底回调（只在不匹配时）", {
    core::Router r;
    int calls = 0;
    // 测试用等价回调：记账 + 画一个标记元素（证明回调真被调用且进了 Ui）
    r.setFallbackCompose([&calls](eui::Ui& ui, const eui::Screen&) {
        ++calls;
        ui.text("test.fallback").text("无页面").build();
    });

    eui::Screen screen;
    screen.width = 800.0f;
    screen.height = 600.0f;
    eui::Ui ui;

    // 一个模块都没注册（既有防御分支）→ 兜底回调被调用一次
    ui.begin("f1");
    r.compose(ui, screen);
    ui.end();
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK(ui.find("test.fallback") != nullptr);

    // 有匹配模块 → 模块自己画，兜底回调不再被调用
    StubModule home(core::PageId::Home, "home");
    r.registerModule(&home);
    ui.begin("f2");
    r.compose(ui, screen);
    ui.end();
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK(ui.find("stub.home") != nullptr);
    return true;
})

REGISTER_TEST(router_compose_without_fallback_degrades_silently,
              "router: 未注入兜底 → 不绘制不崩（保持静默降级语义）", {
    core::Router r;  // 无模块 + 未调 setFallbackCompose
    eui::Screen screen;
    screen.width = 800.0f;
    screen.height = 600.0f;
    eui::Ui ui;

    ui.begin("n1");
    r.compose(ui, screen);  // 不崩
    ui.end();
    // core 侧不再画任何东西：app 层占位 id（"empty"）不存在于本帧
    DOCTEST_CHECK(ui.find("empty") == nullptr);
    return true;
})

// Router 跨模块跳转负载（payload）单元测试：
// 设/取/一次性消费语义/默认空/页面隔离/纯导航不清负载。

REGISTER_TEST(router_navigate_default_no_payload, "router: 默认导航无负载", {
    core::Router r;
    // 插拔门控后 navigateTo 校验目标页已注册（未注册回退 Home），先挂桩
    StubModule md(core::PageId::Markdown, "md");
    r.registerModule(&md);
    r.navigateTo(core::PageId::Markdown);
    DOCTEST_CHECK_EQ(r.currentPage(), core::PageId::Markdown);
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Markdown).empty());
    return true;
})

REGISTER_TEST(router_payload_set_and_consume, "router: 设负载并取出", {
    core::Router r;
    StubModule md(core::PageId::Markdown, "md");
    r.registerModule(&md);
    r.navigateTo(core::PageId::Markdown, "open:notes/a.md");
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Markdown),
                     std::string("open:notes/a.md"));
    return true;
})

REGISTER_TEST(router_payload_consume_once, "router: 负载一次性消费", {
    core::Router r;
    StubModule todo(core::PageId::Todo, "todo");
    r.registerModule(&todo);
    r.navigateTo(core::PageId::Todo, "action:new_task");
    DOCTEST_CHECK_EQ(r.consumePendingPayload(core::PageId::Todo),
                     std::string("action:new_task"));
    // 取过一次后再次调用返回空（compose 每帧调用不会重复触发）
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Todo).empty());
    return true;
})

REGISTER_TEST(router_payload_page_isolated, "router: 负载页面隔离", {
    core::Router r;
    StubModule md(core::PageId::Markdown, "md");
    r.registerModule(&md);
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
    StubModule md(core::PageId::Markdown, "md");
    StubModule home(core::PageId::Home, "home");
    r.registerModule(&md);
    r.registerModule(&home);
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
    StubModule md(core::PageId::Markdown, "md");
    r.registerModule(&md);
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

    eui::KeyEvent key = test_support::pressShortcut(eui::InputKey::P, /*shift=*/true);

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

// Router::navigateTo 进页通知：目标模块 onPageEnter 被回调（含同页重入），
// 非目标页模块不收；Home 借此进页重查仪表盘数据。

namespace {

class EnterStubModule : public ::core::Module {
public:
    EnterStubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }
    void onPageEnter() override { ++enterCount_; }
    int enterCount_ = 0;

private:
    ::core::PageId page_;
    std::string name_;
};

} // namespace

REGISTER_TEST(router_navigate_page_enter, "router: navigateTo 回调目标模块 onPageEnter", {
    ::core::Router router;
    EnterStubModule home(::core::PageId::Home, "home");
    EnterStubModule todo(::core::PageId::Todo, "todo");
    router.registerModule(&home);
    router.registerModule(&todo);

    // Home → Todo：目标页收到进页通知，Home 不收
    router.navigateTo(::core::PageId::Todo);
    DOCTEST_CHECK_EQ(todo.enterCount_, 1);
    DOCTEST_CHECK_EQ(home.enterCount_, 0);

    // Todo → Home：重入 Home 触发重查语义
    router.navigateTo(::core::PageId::Home);
    DOCTEST_CHECK_EQ(home.enterCount_, 1);
    DOCTEST_CHECK_EQ(todo.enterCount_, 1);

    // 同页重入（侧边栏再点当前页）也算进页
    router.navigateTo(::core::PageId::Home);
    DOCTEST_CHECK_EQ(home.enterCount_, 2);

    // 带 payload 的导航同样通知
    router.navigateTo(::core::PageId::Todo, "action:new_task");
    DOCTEST_CHECK_EQ(todo.enterCount_, 2);
    DOCTEST_CHECK_EQ(router.consumePendingPayload(::core::PageId::Todo),
                     std::string("action:new_task"));
    return true;
})

// ============ 模块插拔（方案 A）：isRegistered + 未注册页导航兜底 ============

REGISTER_TEST(router_is_registered, "router: isRegistered 反映注册状态", {
    core::Router r;
    DOCTEST_CHECK(!r.isRegistered(core::PageId::Home));
    StubModule home(core::PageId::Home, "home");
    r.registerModule(&home);
    DOCTEST_CHECK(r.isRegistered(core::PageId::Home));
    DOCTEST_CHECK(!r.isRegistered(core::PageId::Todo));
    return true;
})

REGISTER_TEST(router_navigate_unregistered_falls_back_home, "router: 未注册页导航回退 Home（不进空白页）", {
    core::Router r;
    StubModule home(core::PageId::Home, "home");
    r.registerModule(&home);
    r.navigateTo(core::PageId::Home);  // 先离开初始态语义：停在 Home
    // Todo 未注册 → WARN + 回退 Home，current_ 不变成 Todo
    r.navigateTo(core::PageId::Todo);
    DOCTEST_CHECK_EQ(r.currentPage(), core::PageId::Home);
    // 已注册页导航不受影响
    StubModule todo(core::PageId::Todo, "todo");
    r.registerModule(&todo);
    r.navigateTo(core::PageId::Todo);
    DOCTEST_CHECK_EQ(r.currentPage(), core::PageId::Todo);
    return true;
})

REGISTER_TEST(router_navigate_unregistered_payload_dropped, "router: 未注册页导航负载作废", {
    core::Router r;
    StubModule home(core::PageId::Home, "home");
    r.registerModule(&home);
    // 目标模块已关闭 → 负载不落到任何页（别页取不走，Home 也取不走）
    r.navigateTo(core::PageId::Markdown, "open:x.md");
    DOCTEST_CHECK_EQ(r.currentPage(), core::PageId::Home);
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Home).empty());
    DOCTEST_CHECK(r.consumePendingPayload(core::PageId::Markdown).empty());
    return true;
})

REGISTER_TEST(router_navigate_nothing_registered_stays, "router: 全未注册时导航保持当前页（防御兜底）", {
    core::Router r;
    // 一个模块都没注册（Home 也未注册的极端防御分支）：不崩溃、停留当前页
    r.navigateTo(core::PageId::Markdown, "open:x.md");
    DOCTEST_CHECK_EQ(r.currentPage(), core::PageId::Home);
    return true;
})

// ============ 唯一换算 core::windowToCompose（审计 M4）============
//
// 指针空间 → 页面 compose 坐标：compose = 指针 / dpiScale - 页面原点。
// 覆盖 100%/125%/150% 三档 × 侧边栏展开(208)/折叠(72) 两个页面原点。
// 取值为二进制精确值，故直接按 == 断言（100 物理 = 80 逻辑 等换算见下）。

REGISTER_TEST(core_window_to_compose_three_scales_both_origins,
              "core: windowToCompose 三档 DPI × 侧边栏展开/折叠", {
    // 同一 compose 落点（页面内 x=192）在三档缩放下对应的指针 x：
    //   100% → 192 + 208 = 400；125% → 240 + 260 = 500；150% → 288 + 312 = 600
    // 展开（原点 208）：指针 400/500/600 → 192
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 208.0f, 1.0f), 192.0f);
    DOCTEST_CHECK_EQ(core::windowToCompose(500.0f, 208.0f, 1.25f), 192.0f);
    DOCTEST_CHECK_EQ(core::windowToCompose(600.0f, 208.0f, 1.5f), 192.0f);
    // 折叠（原点 72）：同样指针位置整体左移 136 compose px（展开-折叠宽差）
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 72.0f, 1.0f), 328.0f);
    DOCTEST_CHECK_EQ(core::windowToCompose(500.0f, 72.0f, 1.25f), 328.0f);
    DOCTEST_CHECK_EQ(core::windowToCompose(600.0f, 72.0f, 1.5f), 328.0f);
    // 展开/折叠差恒等于原点差（换算对原点只是平移）
    DOCTEST_CHECK_EQ(core::windowToCompose(500.0f, 72.0f, 1.25f) -
                         core::windowToCompose(500.0f, 208.0f, 1.25f),
                     136.0f);
    // y 轴：页面区纵跨整窗，原点恒 0
    DOCTEST_CHECK_EQ(core::windowToCompose(300.0f, 0.0f, 1.25f), 240.0f);

    // ---- U1 尺度契约：dpiScale 入参**取自框架值**，不再由几何反推 ----
    // 主窗口取 app::contentScale()、子窗取自己 compose 收到的 Screen::scale；
    // 二者与指针事件的 PointerEvent/DragEvent::scale 同源同值，恒等于
    // "物理 / 逻辑" 比。这里直接喂真实 Screen 的 scale 字段，锁住"框架值 ⇒
    // 同一 compose 落点"的等价性（手工构造 Screen 未填 scale 时退化为 1.0，
    // 见 core/dsl.h 的 Screen 声明）。
    eui::Screen frame;  // 当帧 compose 的 Screen（真实运行时 scale 由框架填）
    DOCTEST_CHECK_EQ(frame.scale, 1.0f);  // 未填 → 1.0（100% 档退化值）
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 208.0f, frame.scale), 192.0f);
    frame.scale = 1.25f;  // Screen::scale 即"物理 = 逻辑 * scale"的 scale
    DOCTEST_CHECK_EQ(core::windowToCompose(500.0f, 208.0f, frame.scale), 192.0f);
    frame.scale = 1.5f;
    DOCTEST_CHECK_EQ(core::windowToCompose(600.0f, 208.0f, frame.scale), 192.0f);
    return true;
})

REGISTER_TEST(core_window_to_compose_degenerate_and_legacy,
              "core: windowToCompose 退化 dpiScale 归一化 + 旧行为等价", {
    // 无 Router / 无缩放（原点 0、尺度 1）= 旧版 windowToPageX 的直通语义
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 0.0f, 1.0f), 400.0f);
    // 退化尺度（0 / 负数 / NaN）按同尺度处理，不产生 inf/NaN 坐标
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 208.0f, 0.0f), 192.0f);
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 208.0f, -2.0f), 192.0f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    DOCTEST_CHECK_EQ(core::windowToCompose(400.0f, 208.0f, nan), 192.0f);
    return true;
})

// ============ PageId 枚举契约（审计 TIN-09：原在 test_modules/test_todo.cpp，
// 与 todo 无关 → 挪进 core 测试。名字 core_pageid_order 是 module_interface.h
// 中 PageId 声明处的指针注释目标）============

// PageId 枚举序是侧边栏顺序与 Router 分发依赖的契约：重排枚举会静默改变默认
// 导航顺序。原断言 `PageId::Todo >= 0` 对该 enum class 恒真、未验证任何顺序，
// 此处改为显式钉死预期序（严格单调 + 各成员预期编号），任一改动都会被抓到。
REGISTER_TEST(core_pageid_order,
              "core: PageId ordering contract (Home<Markdown<Todo<ClaudeProxy<Settings<About)", {
    using core::PageId;
    const int home = static_cast<int>(PageId::Home);
    const int markdown = static_cast<int>(PageId::Markdown);
    const int todo = static_cast<int>(PageId::Todo);
    const int claude = static_cast<int>(PageId::ClaudeProxy);
    const int settings = static_cast<int>(PageId::Settings);
    const int about = static_cast<int>(PageId::About);

    // 严格单调：Home < Markdown < Todo < ClaudeProxy < Settings < About
    DOCTEST_CHECK(home < markdown);
    DOCTEST_CHECK(markdown < todo);
    DOCTEST_CHECK(todo < claude);
    DOCTEST_CHECK(claude < settings);
    DOCTEST_CHECK(settings < about);

    // 钉死预期取值，防"虽单调但整体平移/插空"绕过顺序检查而悄悄改变路由编号
    DOCTEST_CHECK_EQ(home, 0);
    DOCTEST_CHECK_EQ(markdown, 1);
    DOCTEST_CHECK_EQ(todo, 2);
    DOCTEST_CHECK_EQ(claude, 3);
    DOCTEST_CHECK_EQ(settings, 4);
    DOCTEST_CHECK_EQ(about, 5);
    return true;
})
