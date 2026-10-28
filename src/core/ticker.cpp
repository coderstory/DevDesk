// 全局 ticker 实现：主线程独占的订阅表 + 逐帧推进。
//
// 为什么注册表故意泄漏（堆上静态、永不释放）：退出钩子（令 armExitHook 晚于
// 首次 subscribe 注册）会让模块 onStop 在静态局部对象析构之后仍调 unsubscribe；
// 若注册表是函数局部 static，LIFO 销毁顺序是先毁注册表、后跑 exit hook，
// 退出期即 UAF。与 main.cpp getClaudeService / core/tool.cpp 的「进程级状态
// 永不析构」惯用法一致。
#include "ticker.h"

#include "logging.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace core::ticker {

namespace {

struct Entry {
    Handle handle = 0;
    Subscription sub;
    Now lastFired = 0;      // 上次触发时刻（everFired 为 false 时无意义）
    bool everFired = false; // 区分「从未触发」与「在 unix 0 秒触发过」
};

std::vector<Entry>& registry() {
    static auto* entries = new std::vector<Entry>();
    return *entries;
}

std::uint64_t& nextHandle() {
    static auto* next = new std::uint64_t(1);  // 0 保留为无效句柄
    return *next;
}

Entry* findEntry(Handle handle) {
    for (Entry& entry : registry()) {
        if (entry.handle == handle) return &entry;
    }
    return nullptr;
}

} // namespace

Handle subscribe(Subscription sub) {
    const Handle handle = nextHandle()++;
    Entry entry;
    entry.handle = handle;
    entry.sub = std::move(sub);
    registry().push_back(std::move(entry));
    return handle;
}

void unsubscribe(Handle h) {
    auto& entries = registry();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->handle == h) {
            entries.erase(it);
            return;
        }
    }
    // 未知句柄：静默返回（幂等）
}

void pump(Now now) {
    auto& entries = registry();
    if (entries.empty()) return;

    // 1) 过期清理先于一切：now >= expireAt 的订阅不再触发，且本轮即移除，
    //    避免失效条目留在表里被后续 pump 反复检查（不留垃圾）。
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [now](const Entry& entry) {
                                     return entry.sub.expireAt != 0 &&
                                            now >= entry.sub.expireAt;
                                 }),
                  entries.end());
    if (entries.empty()) return;

    // 句柄快照：回调内 subscribe/unsubscribe 会改活表，本轮名单保持不变；
    // 每次调用前按句柄复核活性——自注销/互注销都不会调用到已失效条目。
    std::vector<Handle> order;
    order.reserve(entries.size());
    for (const Entry& entry : entries) order.push_back(entry.handle);

    for (Handle handle : order) {
        Entry* entry = findEntry(handle);
        if (entry == nullptr) continue;  // 本轮已被回调注销
        if (!entry->sub.fn) continue;    // 空 std::function：跳过

        // 2) 未到：firstAt == 0 表示立即，等价于 now < 0 才不触发。
        if (now < entry->sub.firstAt) continue;
        // 3) 节流：intervalSec == 0 表示首次之后每次 pump 都触发。
        if (entry->sub.intervalSec > 0 && entry->everFired &&
            now - entry->lastFired <
                static_cast<Now>(entry->sub.intervalSec)) {
            continue;
        }

        // 4) 触发前先记账：回调里可能注销自己，之后不能再访问 entry。
        entry->everFired = true;
        entry->lastFired = now;
        TickFn fn = entry->sub.fn;         // 拷贝：erase 会使 entry 失效
        void* user = entry->sub.user;
        const std::string subName = entry->sub.name;  // 异常报告用：回调可能注销自己
        try {
            fn(now, user);
        } catch (...) {
            // 契约：回调不得抛异常。吞并并点名订阅，继续本轮的下一订阅。
            LOG_ERROR("Ticker: 订阅 '" + subName + "' 的回调抛出异常，已吞并并继续下一订阅");
        }
    }
}

bool hasSubscribers() { return !registry().empty(); }

void resetForTest() { registry().clear(); }

} // namespace core::ticker