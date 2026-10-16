// src/module_markdown/md_note_git.cpp
// 笔记目录 git 自动提交实现：后台线程执行 git 命令，失败静默面。
#include "md_note_git.h"

#include "core/logging.h"
#include "core/time.h"
#include "platform/exec_capture.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace module::markdown {

namespace fs = std::filesystem;

// 进程级测试总闸（默认开；语义见头文件注释）
namespace {
std::atomic<bool> g_noteGitEnabledForTest{true};
}  // namespace

void NoteGitAutoCommit::setEnabledForTest(bool enabled) {
    g_noteGitEnabledForTest.store(enabled, std::memory_order_release);
}

bool NoteGitAutoCommit::enabledForTest() {
    return g_noteGitEnabledForTest.load(std::memory_order_acquire);
}

// ---------- 纯函数（节流判定） ----------

bool NoteGitAutoCommit::shouldCommitNow(std::time_t lastCommitAt,
                                        std::time_t now,
                                        std::chrono::seconds interval) {
    // 从未 commit → 总是执行
    if (lastCommitAt <= 0) return true;
    // 距上次 commit 已过 interval → 执行；否则不执行
    return (now - lastCommitAt) >= static_cast<std::time_t>(interval.count());
}

// ---------- 构造 / 析构 ----------

NoteGitAutoCommit::NoteGitAutoCommit(fs::path notesDir)
    : notesDir_(std::move(notesDir)) {}

NoteGitAutoCommit::~NoteGitAutoCommit() {
    // 标记停止：禁止 onFileSaved 派发新后台任务
    stopped_.store(true, std::memory_order_release);
    // join 可能还在跑的最后一个后台线程（避免析构后回调访问已销毁成员）
    std::unique_lock<std::mutex> lock(mu_);
    // 拿锁即保证没有正在运行的 git 操作，且没有新操作可派发（stopped_ = true）
    // bgThread_ 如果在 run，其 std::thread 对象已在调用线程外；这里只 join 本对象持有的句柄
    lock.unlock();
    if (bgThread_ && bgThread_->joinable()) {
        bgThread_->join();
    }
}

// ---------- 启动 ----------

void NoteGitAutoCommit::start() {
    // 测试总闸（headless 夹具关掉时）：直接标禁用——不起线程、不探测 git
    if (!enabledForTest()) {
        disabled_.store(true, std::memory_order_release);
        return;
    }
    // 幂等：已启动或被禁用则空操作
    bool expected = false;
    if (!initStarted_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
        return;  // 已启动
    }
    if (disabled_.load(std::memory_order_acquire)) return;

    // 后台线程：探测 git → init → identity
    // 启动不等待结果（start 调用方无需知道 git 是否就绪）
    {
        std::lock_guard<std::mutex> lock(mu_);
        // 构造期 stopped_ 必为 false，不检查
        bgThread_ = std::make_unique<std::thread>(&NoteGitAutoCommit::initRepo,
                                                  this);
    }
}

// ---------- 写盘触发 ----------

void NoteGitAutoCommit::onFileSaved(std::time_t savedAt) {
    // 快速路径：禁用或已停止 → 空操作
    if (disabled_.load(std::memory_order_acquire)) return;
    if (stopped_.load(std::memory_order_acquire)) return;

    // 节流判定（主线程，原子读）
    const std::time_t lastAt = lastAutoCommitAt_.load(std::memory_order_acquire);
    if (!shouldCommitNow(lastAt, savedAt)) {
        return;  // 距上次 commit 不足 5 分钟，不提交
    }

    // 先推进时间戳（防并发写盘双发），再派发后台 commit
    lastAutoCommitAt_.store(savedAt, std::memory_order_release);

    // 后台线程：git add -A && git commit
    // 如果上一个后台线程还活着（initRepo 仍在跑），join 等它结束再派发。
    // initRepo 执行 <100ms，join 不会造成可感知的 UI 卡顿。
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_.load(std::memory_order_acquire)) return;
        // 等上一个后台线程结束（join），避免线程泄漏
        if (bgThread_ && bgThread_->joinable()) {
            bgThread_->join();
        }
        // 重检 disabled_：initRepo 若在 join 期间发现 git 缺失，则已置 disabled_ = true，
        // 此时不再派发 commit 任务（否则 execCapture 会 -1 失败）
        if (disabled_.load(std::memory_order_acquire)) return;
        bgThread_ = std::make_unique<std::thread>(
            &NoteGitAutoCommit::doCommit, this, savedAt);
    }
}

// ---------- 后台：探测 + init ----------

void NoteGitAutoCommit::initRepo() {
    // 1) 探测 git 是否在 PATH
    if (!platform::executableOnPath("git")) {
        LOG_WARN("笔记自动 git 提交已禁用：git 不在 PATH，笔记功能不受影响");
        disabled_.store(true, std::memory_order_release);
        return;
    }

    const std::string dirStr = notesDir_.string();

    // 2) 检查是否已是 git 仓库（.git 目录或 worktree .git 文件）
    const auto dotGit = notesDir_ / ".git";
    std::error_code ec;
    if (fs::exists(dotGit, ec) && !ec) {
        // 已是仓库：不需要 init，身份兜底照做（可能上次 init 被中断）
        LOG_INFO("笔记目录已是 git 仓库：" + dirStr);
    } else {
        // 3) git init
        int rc = platform::execCapture(dirStr, {"git", "init"});
        if (rc != 0) {
            // git init 失败：可能目录不可写 / 磁盘满 / git 损坏
            // 不重试（init 失败通常不是瞬时错误，重试无意义且制造风暴）
            LOG_WARN("笔记目录 git init 失败，自动提交已禁用（退出码 " +
                     std::to_string(rc) + "，不会重试）: " + dirStr);
            disabled_.store(true, std::memory_order_release);
            return;
        }
        LOG_INFO("笔记目录已初始化为 git 仓库：" + dirStr);
    }

    // 4) identity 兜底：git config user.name 为空 → 写本地 user.name / user.email。
    // 只设 local config（git -C <dir> config --local），不动全局/系统级配置。
    {
        int rc = platform::execCapture(dirStr,
                                       {"git", "config", "user.name"});
        // git config 退出码 1 表示键不存在（stdout 为空），我们不看 stdout，靠退出码判。
        // 键存在时退出码 0，视为已有身份，不覆盖。
        if (rc != 0) {
            // 键不存在或被 git 报错 → 兜底写入 local config
            LOG_INFO("笔记 git 仓库无 user.name，写入本地身份: DevDesk <devdesk@local>");
            platform::execCapture(dirStr,
                                  {"git", "config", "--local", "user.name",
                                   "DevDesk"});
            platform::execCapture(dirStr,
                                  {"git", "config", "--local", "user.email",
                                   "devdesk@local"});
            // 兜底写入失败不阻断：git commit 仍会尝试，后续日志会显示具体错误。
            // 只要 user.name/email 有一个已存在或写入成功，commit 就能过。
        }
        // user.name 存在 → 不做任何改动（尊重用户已有配置）
    }

    LOG_INFO("笔记 git 自动提交已就绪：" + dirStr);
}

// ---------- 后台：commit ----------

void NoteGitAutoCommit::doCommit(std::time_t commitTime) {
    // 构造 commit message: "auto: YYYY-MM-DD HH:MM:SS"（本地时区）
    std::tm tm{};
    core::time::localTime(commitTime, &tm);
    char msgBuf[64];
    std::snprintf(msgBuf, sizeof(msgBuf), "auto: %04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    const std::string commitMsg(msgBuf);

    const std::string dirStr = notesDir_.string();

    // git add -A（暂存所有变化，含新建 / 修改 / 删除）
    // --git-dir + --work-tree 比 -C 更显式：明确 .git 位置与工作树位置。
    // worktree 场景 .git 是文件（指向主仓库），git 自动解析，不传 --git-dir 也正确。
    int rc = platform::execCapture(dirStr, {"git", "add", "-A"});
    if (rc != 0) {
        // git add 失败通常意味着 .git 损坏 / 权限 / 磁盘满
        LOG_WARN("笔记 git add 失败（退出码 " + std::to_string(rc) +
                 "），跳过本次自动提交: " + dirStr);
        return;
    }

    // git commit -m "auto: YYYY-MM-DD HH:MM:SS"
    rc = platform::execCapture(dirStr, {"git", "commit", "-m", commitMsg});
    if (rc == 0) {
        LOG_INFO("笔记自动 git commit: " + commitMsg);
    } else if (rc == 1) {
        // 退出码 1 通常是 "nothing to commit, working tree clean" —— 正常态
        //（git add 成功但内容与上次 commit 相同，无变化可提交）
        // 不作任何日志：这是高频正常态（多次写盘间文件内容无实际变化）。
    } else {
        // 退出码 128 = fatal error（.git 损坏/锁冲突/权限），记录但不重试
        LOG_WARN("笔记 git commit 失败（退出码 " + std::to_string(rc) +
                 "）: " + commitMsg);
    }
}

} // namespace module::markdown