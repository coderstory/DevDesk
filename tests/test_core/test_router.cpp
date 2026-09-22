// tests/test_core/test_router.cpp
// Router 跨模块跳转负载（payload）单元测试：
// 设/取/一次性消费语义/默认空/页面隔离/纯导航不清负载。
#include "doctest.h"
#include "core/router.h"

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
