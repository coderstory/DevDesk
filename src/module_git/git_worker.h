// src/module_git/git_worker.h
// git 管理器后台任务队列（need:7 批次二；技术方案 §3 线程模型）：
// 单 worker 线程串行消费任务；Repo handle 按 repo 路径缓存（批次一 POC 实测
// open 均价 1300µs > 1ms 阈值，备选策略转正）。UI 线程零 libgit2 调用。
// 结果回投：任务闭包自行捕获结果槽（controller 的 inbox），worker 线程只负责执行。
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

#include "platform/git/git_repo.h"

namespace module::git {

class GitWorker {
public:
    // repo == nullptr 表示按目标路径 open 失败，任务自行降级/回错误。
    using RepoJob = std::function<void(platform::git::Repo* repo)>;
    using RawJob = std::function<void()>;

    GitWorker() = default;
    ~GitWorker();
    GitWorker(const GitWorker&) = delete;
    GitWorker& operator=(const GitWorker&) = delete;

    void start();
    void stop();  // join；丢弃未执行任务并释放缓存 handle

    // 派发需要 Repo 的任务。repoPathUtf8 ≠ 当前缓存路径时旧 handle 释放重开
    //（切仓语义）。未 start() 时空操作。
    void postWithRepo(const std::string& repoPathUtf8, RepoJob job);

    // 派发裸任务（不碰缓存 handle）。未 start() 时空操作。
    void post(RawJob job);

    // worker 线程内确保缓存句柄指向 repoPathUtf8（切仓=释放重开）。
    // open 失败返回 nullptr 且不保留半打开句柄。仅 worker 线程可调。
    platform::git::Repo* ensureHandle(const std::string& repoPathUtf8);

    // 测试观察位：缓存句柄当前路径（空 = 无缓存）。仅 worker 空闲时可读。
    const std::string& cachedRepoPathForTest() const { return cachedPath_; }

private:
    void run();

    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    bool stop_ = false;
    bool started_ = false;

    // handle 缓存：只在 worker 线程触碰（mu_ 只保护队列与 stop_ 标志）
    std::optional<platform::git::Repo> cached_;
    std::string cachedPath_;
};

} // namespace module::git
