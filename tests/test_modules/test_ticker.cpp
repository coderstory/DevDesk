// 全局 ticker 契约：时间维度（firstAt / intervalSec / expireAt）、注销、自注销、
// 空表空转、user 透传。
//
// 注册表是进程级全局，跨用例存活；每个用例用 RAII 守卫清空（resetForTest），
// 断言 fail-fast 早退时也不会把订阅留给后续用例（防「上一用例残留订阅触发、
// 下一用例计数翻车」）。
#include "doctest.h"
#include "core/ticker.h"

#include <cstdint>

namespace {

struct Recorder {
    int calls = 0;
    core::ticker::Now lastNow = 0;
    void* lastUser = nullptr;
};

void record(Recorder& rec, core::ticker::Now now, void* user) {
    ++rec.calls;
    rec.lastNow = now;
    rec.lastUser = user;
}

struct TickerResetGuard {
    ~TickerResetGuard() { core::ticker::resetForTest(); }
};

} // namespace

// 防：订阅注册/注销语义退化（未注册也触发、注销后仍触发、未知句柄崩溃）
// 以及 user 指针在回调里丢失/串味（附加参数传错，调用方上下文全错）。
REGISTER_TEST(ticker_subscribe_unsubscribe_and_user,
              "ticker: 注册后 pump 触发且 user 原样回传；注销后不再触发且幂等", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    Recorder rec;

    const core::ticker::Handle handle = core::ticker::subscribe(
        {.name = "probe.user",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &rec,
         .firstAt = 0,
         .intervalSec = 0,
         .expireAt = 0});
    DOCTEST_CHECK(handle != 0);
    DOCTEST_CHECK(core::ticker::hasSubscribers());

    core::ticker::pump(100);
    DOCTEST_CHECK_EQ(rec.calls, 1);
    DOCTEST_CHECK_EQ(rec.lastNow, static_cast<core::ticker::Now>(100));
    DOCTEST_CHECK(rec.lastUser == &rec);

    core::ticker::unsubscribe(handle);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    core::ticker::pump(101);
    DOCTEST_CHECK_EQ(rec.calls, 1);  // 注销后不再触发

    // 幂等：重复注销与未知句柄都静默返回，不崩
    core::ticker::unsubscribe(handle);
    core::ticker::unsubscribe(999999);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    return true;
})

// 防：空注册表时 pump 仍遍历/做事（主循环空转成本失控），或 hasSubscribers 误报。
REGISTER_TEST(ticker_empty_registry_spins_free,
              "ticker: 空注册表 hasSubscribers=false 且 pump 零回调", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    core::ticker::pump(12345);  // 不崩、无事发生即通过
    core::ticker::pump(12346);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    return true;
})

// 防：firstAt 被忽略（注册即触发，提前弹提醒/提前跑调度）。
REGISTER_TEST(ticker_first_at_gates_first_trigger,
              "ticker: now < firstAt 不触发，now == firstAt 首次触发，interval=0 之后每次 pump 都触发", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    Recorder rec;
    core::ticker::subscribe(
        {.name = "probe.firstAt",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &rec,
         .firstAt = 200,
         .intervalSec = 0,
         .expireAt = 0});

    core::ticker::pump(199);
    DOCTEST_CHECK_EQ(rec.calls, 0);  // 未到点：不触发
    core::ticker::pump(200);
    DOCTEST_CHECK_EQ(rec.calls, 1);  // 到点：触发
    core::ticker::pump(201);
    DOCTEST_CHECK_EQ(rec.calls, 2);  // interval=0：之后每次 pump 都触发
    return true;
})

// 防：过期订阅仍被回调（提醒/调度在失效后继续跑），以及失效条目留在表里
// 反复检查（hasSubscribers 永远为真、空转判断失效）。
REGISTER_TEST(ticker_expire_stops_and_cleans_up,
              "ticker: now >= expireAt 不再触发且订阅被清理；未到首触发的过期订阅也清理", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    Recorder rec;
    core::ticker::subscribe(
        {.name = "probe.expire",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &rec,
         .firstAt = 0,
         .intervalSec = 0,
         .expireAt = 200});

    core::ticker::pump(199);
    DOCTEST_CHECK_EQ(rec.calls, 1);
    core::ticker::pump(200);  // now >= expireAt：不触发，本次 pump 内移除
    DOCTEST_CHECK_EQ(rec.calls, 1);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());  // 已清理，不留垃圾
    core::ticker::pump(201);
    DOCTEST_CHECK_EQ(rec.calls, 1);

    // 从未首触发就过期：同样不触发且被清理
    Recorder neverFired;
    core::ticker::subscribe(
        {.name = "probe.expire.beforeFirst",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &neverFired,
         .firstAt = 500,
         .intervalSec = 0,
         .expireAt = 200});
    core::ticker::pump(200);
    DOCTEST_CHECK_EQ(neverFired.calls, 0);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    return true;
})

// 防：intervalSec 节流失效（每秒快照/轮询类订阅被同一秒内多次 pump 放大成高频）。
REGISTER_TEST(ticker_interval_throttles,
              "ticker: intervalSec 内不重复触发，满间隔才再次触发；interval=0 每次 pump 都触发", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    Recorder throttled;
    Recorder everyFrame;
    core::ticker::subscribe(
        {.name = "probe.interval",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &throttled,
         .firstAt = 0,
         .intervalSec = 5,
         .expireAt = 0});
    core::ticker::subscribe(
        {.name = "probe.everypump",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &everyFrame,
         .firstAt = 0,
         .intervalSec = 0,
         .expireAt = 0});

    core::ticker::pump(100);
    DOCTEST_CHECK_EQ(throttled.calls, 1);
    DOCTEST_CHECK_EQ(everyFrame.calls, 1);
    core::ticker::pump(102);  // 距上次 2s < 5s：节流
    DOCTEST_CHECK_EQ(throttled.calls, 1);
    DOCTEST_CHECK_EQ(everyFrame.calls, 2);
    core::ticker::pump(104);
    DOCTEST_CHECK_EQ(throttled.calls, 1);
    core::ticker::pump(105);  // 满 5s：再次触发
    DOCTEST_CHECK_EQ(throttled.calls, 2);
    DOCTEST_CHECK_EQ(everyFrame.calls, 4);
    return true;
})

// 防：回调内注销自己导致迭代器失效/悬垂（崩溃或继续调用已注销回调）。
REGISTER_TEST(ticker_self_unsubscribe_in_callback,
              "ticker: 回调内注销自己安全，本轮之后不再触发", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    int calls = 0;
    core::ticker::Handle handle = 0;
    handle = core::ticker::subscribe(
        {.name = "probe.self.unsubscribe",
         .fn = [&calls, &handle](core::ticker::Now, void*) {
             ++calls;
             core::ticker::unsubscribe(handle);
         },
         .user = nullptr,
         .firstAt = 0,
         .intervalSec = 0,
         .expireAt = 0});

    core::ticker::pump(100);
    DOCTEST_CHECK_EQ(calls, 1);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    core::ticker::pump(101);
    DOCTEST_CHECK_EQ(calls, 1);  // 已注销，不再调用
    return true;
})

// 防：三个时间维度各自独立实现、组合时判定顺序错（先在未到点/已过期时触发，
// 或首触发后再按错误基线算节流）。
REGISTER_TEST(ticker_first_interval_expire_combined,
              "ticker: firstAt/intervalSec/expireAt 组合按 过期→未到→节流 的顺序生效", {
    TickerResetGuard guard;
    core::ticker::resetForTest();
    Recorder rec;
    core::ticker::subscribe(
        {.name = "probe.combined",
         .fn = [](core::ticker::Now now, void* user) {
             record(*static_cast<Recorder*>(user), now, user);
         },
         .user = &rec,
         .firstAt = 100,
         .intervalSec = 10,
         .expireAt = 130});

    core::ticker::pump(99);   // 未到 firstAt
    DOCTEST_CHECK_EQ(rec.calls, 0);
    core::ticker::pump(100);  // 首触发
    DOCTEST_CHECK_EQ(rec.calls, 1);
    core::ticker::pump(105);  // 节流（5 < 10）
    DOCTEST_CHECK_EQ(rec.calls, 1);
    core::ticker::pump(109);  // 节流（9 < 10）
    DOCTEST_CHECK_EQ(rec.calls, 1);
    core::ticker::pump(110);  // 满间隔：第二次
    DOCTEST_CHECK_EQ(rec.calls, 2);
    core::ticker::pump(120);  // 满间隔：第三次
    DOCTEST_CHECK_EQ(rec.calls, 3);
    core::ticker::pump(129);  // 节流（9 < 10），且尚未过期
    DOCTEST_CHECK_EQ(rec.calls, 3);
    core::ticker::pump(130);  // now >= expireAt：不触发并清理
    DOCTEST_CHECK_EQ(rec.calls, 3);
    DOCTEST_CHECK_FALSE(core::ticker::hasSubscribers());
    core::ticker::pump(140);
    DOCTEST_CHECK_EQ(rec.calls, 3);
    return true;
})