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
#include <cstddef>
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

    // 索引快照 + 直取：稳态下每轮 O(n)（此前每句柄一次 findEntry 是 O(n²)）。
    // 回调内 subscribe/unsubscribe 会改活表：索引失配（条目被删致左移）时按句柄
    // 回查兜底；本轮已被回调注销的句柄回查不到 → 跳过，不悬垂。
    std::vector<std::pair<std::size_t, Handle>> order;
    order.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        order.emplace_back(i, entries[i].handle);
    }

    for (const auto& slot : order) {
        const std::size_t idx = slot.first;
        const Handle handle = slot.second;
        Entry* entry = (idx < entries.size() && entries[idx].handle == handle)
                           ? &entries[idx]
                           : findEntry(handle);
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
        try {
            fn(now, user);
        } catch (...) {
            // 契约：回调不得抛异常。异常路径才回查订阅名（正常路径零拷贝）；
            // 回调可能已注销自己 → 查不到就点名句柄。吞并后继续本轮下一订阅。
            std::string subName =
                "句柄 " + std::to_string(static_cast<unsigned long long>(handle)) +
                "（已注销）";
            for (const Entry& e : registry()) {
                if (e.handle == handle) { subName = e.sub.name; break; }
            }
            LOG_ERROR("Ticker: 订阅 '" + subName + "' 的回调抛出异常，已吞并并继续下一订阅");
        }
    }
}

bool hasSubscribers() { return !registry().empty(); }

void resetForTest() { registry().clear(); }

} // namespace core::ticker