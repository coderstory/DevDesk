// src/module_git/git_worker.cpp
#include "git_worker.h"

#include "platform/git/git_libgit2.h"

#include <utility>

namespace module::git {

GitWorker::~GitWorker() { stop(); }

void GitWorker::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (started_) return;
    started_ = true;
    stop_ = false;
    thread_ = std::thread([this] { run(); });
}

void GitWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_) return;
        stop_ = true;
        // 队列清空：未执行任务直接丢弃（调用方结果槽不受影响，UI 保持既有数据）
        std::queue<std::function<void()>> dropped;
        jobs_.swap(dropped);
    }
    cv_.notify_all();  // 唤醒 run() 的 wait（pred 见 stop_，即退）
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

void GitWorker::post(RawJob job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_ || stop_) return;
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void GitWorker::postWithRepo(const std::string& repoPathUtf8, RepoJob job) {
    // 路径与闭包一起进队列：worker 线程执行时先 ensureHandle 再移交 repo 指针。
    // [this] 安全性：stop() 先清队列再 join，闭包不会在析构后执行。
    post([this, repoPathUtf8, job = std::move(job)] {
        platform::git::Repo* repo = ensureHandle(repoPathUtf8);
        if (job) job(repo);
    });
}

platform::git::Repo* GitWorker::ensureHandle(const std::string& repoPathUtf8) {
    if (cached_ && cachedPath_ == repoPathUtf8) {
        return &*cached_;
    }
    cached_.reset();
    cachedPath_.clear();
    auto opened = platform::git::Repo::open(
        platform::git::utf8ToPath(repoPathUtf8));
    if (opened.isFail()) {
        return nullptr;  // 任务侧按 nullptr 降级（目标路径失效，不缓存半打开句柄）
    }
    cached_.emplace(std::move(opened.value()));
    cachedPath_ = repoPathUtf8;
    return &*cached_;
}

void GitWorker::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        job();
    }
}

} // namespace module::git
