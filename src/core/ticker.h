// 全局 ticker：任何模块注册轻量回调，应用主循环每帧调一次 pump 统一推进。
//
// 线程契约：只在主线程使用——subscribe / unsubscribe / pump 全部在主线程调用；
// 内部不加锁，也不宣称线程安全。后台线程若要触发这里的回调，须先派发回主线程。
//
// 时间维度（firstAt / intervalSec / expireAt，时刻均为 unix 秒）：
//   firstAt      首次触发的绝对时刻；0 = 立即（注册后的下一次 pump 即触发）。
//   intervalSec  首次之后的触发间隔（相对上次触发的秒数）；0 = 每次 pump 都触发。
//   expireAt     失效的绝对时刻；0 = 永不过期。now >= expireAt 时不再触发，
//                该订阅在本次 pump 内被移除（不留下垃圾条目）。
// 每次 pump 对每个订阅按以下顺序判定，先命中先跳过：
//   1) 过期：expireAt != 0 且 now >= expireAt → 不触发，并移除订阅；
//   2) 未到：now < firstAt → 不触发；
//   3) 节流：intervalSec > 0 且距上次触发不足 intervalSec 秒 → 不触发；
//   4) 触发：记下 now 作为新的「上次触发时刻」，然后调用回调。
// 三者关系：firstAt 定「何时开始」，intervalSec 定「开始后多快重复」，
// expireAt 定「何时永久停止」；firstAt / expireAt 是绝对时刻，intervalSec 是间隔。
//
// 回调内允许 subscribe / unsubscribe（含注销自己）：pump 先取句柄快照，
// 每次调用前按句柄复核活性，本轮已被注销的订阅不会再被调用（不崩、不悬垂）。
//
// 回调不得抛异常：违反契约的回调由 pump 捕获、LOG_ERROR 点名后吞并，本轮继续
// 下一订阅——一个坏回调不会中断整轮推进，也不把异常抛回主循环。
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace core::ticker {

using Handle = std::uint64_t;
using Now = std::int64_t;  // unix 秒

// 每次回调拿到的参数：本次 pump 的当前时刻 + 注册时传入的附加参数
using TickFn = std::function<void(Now now, void* user)>;

struct Subscription {
    std::string name;          // 排障用：紧急排查时能看出是谁在跑
    TickFn fn;                 // 回调（空 std::function 会被跳过）
    void* user = nullptr;      // 附加参数：注册方想传的任何上下文
    Now firstAt = 0;           // 首次触发时刻；0 = 立即
    int intervalSec = 0;       // 首次之后每隔多少秒再触发；0 = 每次 pump 都触发
    Now expireAt = 0;          // 到期自动失效；0 = 永不过期
};

// 注册订阅，返回非 0 句柄；name 仅用于排障，不参与调度。
Handle subscribe(Subscription sub);

// 注销订阅；未知句柄（含 0）直接返回，幂等。
void unsubscribe(Handle h);

// 由主循环每帧调用：按各订阅的 firstAt / intervalSec / expireAt 推进并回调。
void pump(Now now);

// 空转判断：注册表为空时主循环可跳过其他帧工作。
bool hasSubscribers();

// 测试缝：清空全部订阅（用例间隔离）。生产路径不调用。
void resetForTest();

} // namespace core::ticker