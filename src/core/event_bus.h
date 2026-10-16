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
// 待办写路径：每个成功的写操作发一个（「取消完成」与「标记完成」是不同事件）。
// 订阅方按名精确匹配，新增事件不会惊动既有订阅方。
inline constexpr const char* kTodoCreated = "todo.created";
inline constexpr const char* kTodoUpdated = "todo.updated";
inline constexpr const char* kTodoMoved = "todo.moved";
inline constexpr const char* kTodoCompleted = "todo.completed";
inline constexpr const char* kTodoUncompleted = "todo.uncompleted";
inline constexpr const char* kTodoDeleted = "todo.deleted";
// 待办列写路径：新建 / 重命名 / 删除 / 重排 / 改色共用一个事件名——没有订阅方需要区分
// 这五种列改动，它们对同步是同一件事（列变了）。删除列会级联删掉其下全部任务，
// 故它同时也是「任务数据变更」的触发源，不得因为它不叫 todo.deleted 就漏订。
inline constexpr const char* kTodoColumnChanged = "todo.column_changed";
// 笔记写路径：写盘成功 / 磁盘文件删除成功 / 新建成功 / 重命名成功。
inline constexpr const char* kMdSaved = "md.saved";
inline constexpr const char* kMdDeleted = "md.deleted";
inline constexpr const char* kMdCreated = "md.created";
inline constexpr const char* kMdRenamed = "md.renamed";
// 数据库导入（GitHub Sync 拉取覆盖活库）：begin/end 包住导入全程，
// **end 无论成败都发**（订阅方必须借此恢复被暂停的后台任务）；imported 仅
// 成功后发，订阅方应丢弃内存缓存重查——表被 DROP 重建过，缓存指向旧行
// 即悬空数据。emit 在导入线程同步执行：handler 只做线程安全的动作（停启
// 后台线程、置原子脏标记），不得直接摸 UI 状态。
inline constexpr const char* kDbImportBegin = "db.import_begin";
inline constexpr const char* kDbImportEnd = "db.import_end";
inline constexpr const char* kDbImported = "db.imported";

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
    // 派发序 = 订阅序（entries_[name] 的 vector 保序，锁内 push_back 追加）——是保证，不是巧合；
    // 同名多订阅方按「先订先跑」，claude quota 门控用例靠它把 onStop 插到模块 handler 之前。
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
