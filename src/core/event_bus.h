// core/event_bus.h — 进程内事件总线（M4 Task 1）
// 契约（docs/superpowers/plans/2026-10-agent-m4-plan.md Task 1）：
//   - 纯进程内：无持久化、无补发（模块未加载就错过，不落盘不重放——没有存储接口，结构性保证）。
//   - emit 同步派发：emit 返回前所有 handler 已执行完（不排队、不跨线程投递）。
//   - subscribe 返回 RAII token：token 析构即退订。bus 侧只持 weak 引用，token 悬空后
//     回调不会被调用——订阅方（如 module_agent）生命周期结束无需手动 unregister。
//   - 无订阅者的 emit = no-op；handler 抛异常不外溢（catch + LOG_ERROR 点名事件名）。
// 并发约束：handler 复制后在锁外执行；同一 handler 可能被不同线程的并发 emit 同时进入
// （实际使用是 UI 主线程单发射方，无此竞争；契约按「emit 内同步」理解即可）。
// module_* 之间不得互相依赖：发布方与订阅方都只 include 本 core 头（module_agent 订阅
// todo/md 的事件走这里，不直接 include 兄弟模块）。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"

namespace core::event {

// 事件名常量（发布方/订阅方唯一真源；改字符串必须同步改所有 emit/subscribe 点）
inline constexpr const char* kTodoCreated = "todo.created";
inline constexpr const char* kTodoCompleted = "todo.completed";
inline constexpr const char* kMdSaved = "md.saved";

class EventBus {
public:
    using Handler = std::function<void(const std::string& name)>;

    // RAII 退订 token：析构释放控制块，bus 侧 weak_ptr 随之失效（下一轮 emit 惰性清掉）。
    // 不可复制（退订责任唯一），可移动。默认构造的空 token 不订阅任何事件。
    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& other) noexcept = default;
        Subscription& operator=(Subscription&& other) noexcept = default;
        ~Subscription() = default;   // token 死 = 退订，无需回调 bus
        explicit operator bool() const { return token_ != nullptr; }

    private:
        friend class EventBus;
        explicit Subscription(std::shared_ptr<int> alive) : token_(std::move(alive)) {}
        std::shared_ptr<int> token_;
    };

    // 订阅：name 精确匹配；handler 在 emit 的调用线程同步执行。
    // token 必须保存，丢弃即退订——所以 [[nodiscard]]。
    [[nodiscard]] Subscription subscribe(const std::string& name, Handler handler) {
        auto alive = std::make_shared<int>(0);
        {
            std::lock_guard<std::mutex> lock(mu_);
            entries_[name].push_back(Entry{alive, std::move(handler)});
        }
        return Subscription(std::move(alive));
    }

    // 同步发布：拷出存活 handler（锁内），锁外逐个调用。无订阅者直接返回。
    void emit(const std::string& name) {
        std::vector<Handler> live;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(name);
            if (it == entries_.end()) return;
            auto& entries = it->second;
            std::vector<Entry> kept;
            kept.reserve(entries.size());
            for (auto& e : entries) {
                if (!e.alive.expired()) {
                    live.push_back(e.handler);
                    kept.push_back(std::move(e));
                }
            }
            entries = std::move(kept);   // 惰性清理已退订项（token 析构不动表）
            if (entries.empty()) entries_.erase(it);
        }
        for (auto& h : live) {
            try {
                h(name);
            } catch (...) {
                // 契约：回调抛异常不外溢（不炸发布方 UI 路径）；点名事件方便排查
                LOG_ERROR("EventBus: 事件 '" + name + "' 的订阅回调抛出异常，已吞并不外溢");
            }
        }
    }

private:
    struct Entry {
        std::weak_ptr<int> alive;   // token 控制块；expired = 已退订
        Handler handler;
    };

    mutable std::mutex mu_;
    std::map<std::string, std::vector<Entry>> entries_;
};

// 进程内单例访问器（全局唯一总线；与 appDatabase 同生命周期语义）
inline EventBus& bus() {
    static EventBus instance;
    return instance;
}

} // namespace core::event
