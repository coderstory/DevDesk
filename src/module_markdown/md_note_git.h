// src/module_markdown/md_note_git.h
// 笔记目录 git 自动提交：写盘事件触发 + 5 分钟节流，不是盲定时器。
// 主线程做节流判定（读 lastAutoCommitAt），后台线程跑 git 命令。
// git 不在 PATH → 本功能整体禁用（笔记功能不受影响）。
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace module::markdown {

// 单例语义：一个 MarkdownModule 持一个 NoteGitAutoCommit。
// 生命周期：模块 onStart 创建 → onStop 析构（join 后台线程）。
class NoteGitAutoCommit {
public:
    // notesDir: 笔记根目录的绝对路径
    explicit NoteGitAutoCommit(std::filesystem::path notesDir);
    ~NoteGitAutoCommit();

    NoteGitAutoCommit(const NoteGitAutoCommit&) = delete;
    NoteGitAutoCommit& operator=(const NoteGitAutoCommit&) = delete;

    // 启动：后台线程探测 git 可用性 + 初始化仓库（非阻塞，onStart 调用后立刻返回）。
    // 多次调用安全（幂等：已启动或被禁用则空操作）。
    void start();

    // 写盘成功后调用。节流判定在读调用点（主线程原子读），
    // 距上次自动 commit < 5 分钟 → 空操作；否则更新时间戳并派发后台 commit。
    void onFileSaved(std::time_t savedAt);

    // 功能是否已禁用（git 不在 PATH / init 失败 / 析构中）。
    // 禁用后 onFileSaved 恒空操作。
    bool disabled() const { return disabled_.load(std::memory_order_acquire); }

    // ---------------------------------------------------------------
    // 测试接口
    // ---------------------------------------------------------------

    // 纯函数：距上次 commit 是否已过 interval（默认 5 分钟），节流可测。
    // lastCommitAt = 0 表示从未 commit（首次写盘总是 true）。
    static bool shouldCommitNow(std::time_t lastCommitAt, std::time_t now,
                                std::chrono::seconds interval =
                                    std::chrono::minutes(5));

    // 进程级测试总闸（默认开）。headless 夹具构造时关掉：自动提交在测试临时
    // 目录里真跑 git（init/commit 制造 .git 目录与额外文件事件，扰动树扫描/
    // mtime 类用例；git 可用性随环境漂移也让结果不确定）。本类自身的集成用例
    // 不经 start()（直接 runGit），不受影响。
    static void setEnabledForTest(bool enabled);
    static bool enabledForTest();

private:
    std::filesystem::path notesDir_;

    // 功能开关：git 不在 PATH / init 失败 / 析构中 → true。
    // 设值只在后台线程，读值主线程（atomic 够用）。
    std::atomic<bool> disabled_{false};

    // 是否已进入析构（禁止新后台任务派发）。atomic 保证可见性。
    std::atomic<bool> stopped_{false};

    // 初始化是否正在进行/已完成（防重入 start）
    std::atomic<bool> initStarted_{false};

    // 上次自动 commit 时刻（0 = 从未）。主线程读（onFileSaved 节流判定）+ 主线程写
    //（onFileSaved 推进时间戳，在派发后台任务前先推进防双发）。
    // 后台 commit 线程不写此字段——节流判定只由主线程执行。
    std::atomic<std::time_t> lastAutoCommitAt_{0};

    // 后台线程句柄（init / commit 各自最多一个，互斥保证不并发）。
    std::unique_ptr<std::thread> bgThread_;

    // 串行化 init + commit 后台操作（最简单正确：同一时刻最多一个后台 git 调用）。
    // 5 分钟节流下锁竞争极稀疏。
    std::mutex mu_;

    // ---------- 后台线程入口 ----------

    // 探测 git + git init + identity 兜底（后台线程）。
    // git 不在 PATH / init 失败 → disabled_ = true，LOG_WARN 一次。
    void initRepo();

    // git add -A + git commit（后台线程）。
    // 失败静默：nothing to commit（退出码 1）是正常态；其他错误 LOG_WARN。
    void doCommit(std::time_t commitTime);
};

} // namespace module::markdown