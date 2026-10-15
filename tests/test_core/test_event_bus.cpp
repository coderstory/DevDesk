// tests/test_core/test_event_bus.cpp
// EventBus（M4 Task 1）：进程内同步发布/订阅。契约（spec 2026-10-agent-roadmap-decisions §3）：
//   - emit 同步调用 handler（emit 即派发，无队列无延迟）；
//   - 无订阅者的 emit = no-op 不崩；订阅了别的事件名的 handler 不收；
//   - 多订阅者全部收到；
//   - subscribe 返回 RAII token，token 析构即退订（防订阅方生命周期后回调悬空）；
//   - 无持久化、无补发（进程不在就错过，不落盘不重放——由「没有存储接口」结构性保证）。
// 事件名常量 kTodoCreated / kTodoCompleted / kMdSaved 是发布方（module UI 层）与订阅方
// （module_agent）共用的唯一真源，防止字符串漂移。
#include "doctest.h"

#include "core/event_bus.h"

#include <string>
#include <vector>

REGISTER_TEST(event_bus_emit_delivers_to_subscriber,
              "event_bus: emit 同步送达订阅者（收到事件名）", {
    int got = 0;
    std::string gotName;
    {
        auto token = ::core::event::bus().subscribe(
            ::core::event::kTodoCreated,
            [&](const std::string& name) { ++got; gotName = name; });
        DOCTEST_CHECK(token);   // 有效订阅
        ::core::event::bus().emit(::core::event::kTodoCreated);
    }
    DOCTEST_CHECK_EQ(got, 1);   // emit 返回时已同步送达
    DOCTEST_CHECK_EQ(gotName, std::string("todo.created"));
    return true;
})

REGISTER_TEST(event_bus_emit_without_subscribers_noop,
              "event_bus: 无订阅者 emit = no-op 不崩；别的事件名不串扰", {
    // 无任何订阅者：不得崩（空 handler 表）
    ::core::event::bus().emit(::core::event::kTodoCompleted);
    ::core::event::bus().emit("不存在的.事件名");

    // 订阅 A 事件，emit B 事件：不得收到
    int got = 0;
    {
        auto token = ::core::event::bus().subscribe(
            ::core::event::kTodoCompleted,
            [&](const std::string&) { ++got; });
        ::core::event::bus().emit(::core::event::kMdSaved);
        ::core::event::bus().emit(::core::event::kTodoCreated);
    }
    DOCTEST_CHECK_EQ(got, 0);
    return true;
})

REGISTER_TEST(event_bus_multiple_subscribers_all_receive,
              "event_bus: 同名多订阅者全部收到，互不影响", {
    int a = 0, b = 0;
    {
        auto ta = ::core::event::bus().subscribe(
            ::core::event::kMdSaved, [&](const std::string&) { ++a; });
        {
            auto tb = ::core::event::bus().subscribe(
                ::core::event::kMdSaved, [&](const std::string&) { ++b; });
            ::core::event::bus().emit(::core::event::kMdSaved);
            DOCTEST_CHECK_EQ(a, 1);
            DOCTEST_CHECK_EQ(b, 1);
        }
        // 第二个 token 析构后只剩第一个订阅
        ::core::event::bus().emit(::core::event::kMdSaved);
        DOCTEST_CHECK_EQ(a, 2);
        DOCTEST_CHECK_EQ(b, 1);
    }
    // 两个 token 都析构：再 emit 无人收到
    ::core::event::bus().emit(::core::event::kMdSaved);
    DOCTEST_CHECK_EQ(a, 2);
    DOCTEST_CHECK_EQ(b, 1);
    return true;
})

REGISTER_TEST(event_bus_token_destructor_unsubscribes,
              "event_bus: token 析构即退订（作用域外 emit 不回调，不悬空）", {
    int got = 0;
    {
        auto token = ::core::event::bus().subscribe(
            ::core::event::kTodoCreated,
            [&](const std::string&) { ++got; });
        ::core::event::bus().emit(::core::event::kTodoCreated);
    }   // token 析构 = 退订
    ::core::event::bus().emit(::core::event::kTodoCreated);
    ::core::event::bus().emit(::core::event::kTodoCreated);
    DOCTEST_CHECK_EQ(got, 1);   // 只有退订前那一次
    return true;
})
