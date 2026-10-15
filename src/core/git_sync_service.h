// src/core/git_sync_service.h
// Git 同步服务（core 层，通过 popen() 调系统 git 命令）。
// 支持 init / clone / add / commit / push / pull / status / conflict check。
// 认证：HTTPS URL 中嵌 PAT（https://<token>@github.com/user/repo.git）。
// 线程安全：所有操作持内部 mutex，一次只跑一个 git 命令。
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "result.h"

namespace core::git_sync {

// Git 操作结果
struct GitResult {
    bool success = false;
    std::string output;       // stdout + stderr
    int exitCode = 0;
    std::vector<std::string> conflictFiles;  // git status --porcelain 中 UU 状态的文件
};

// 仓库状态
struct RepoStatus {
    bool isRepo = false;        // 是否是 git 仓库
    bool hasRemote = false;     // 是否配置了远程仓库
    std::string remoteUrl;      // 远程仓库 URL
    std::string branch;         // 当前分支
    bool hasUncommitted = false;// 有未提交的变更
    bool hasUnpushed = false;   // 有未推送的提交
    bool hasConflicts = false;  // 有冲突
    std::vector<std::string> conflictFiles;
};

// 设置项（调用方按需传入，内部不持久化）
struct GitSyncSettings {
    std::string repoUrl;        // https://github.com/user/repo.git
    std::string token;          // PAT（从 Keychain 读取）
    std::string branch = "main";// 远程分支名
    bool autoPush = true;       // commit 后自动 push
};

// 进度回调：phase = "init"/"add"/"commit"/"push"/"pull"，message = 当前步骤描述
using ProgressCallback = std::function<void(const std::string& phase,
                                            const std::string& message)>;

class GitSyncService {
public:
    explicit GitSyncService(const std::filesystem::path& repoDir);
    ~GitSyncService() = default;

    GitSyncService(const GitSyncService&) = delete;
    GitSyncService& operator=(const GitSyncService&) = delete;

    // 初始化仓库（git init + git remote add origin <url>）；已是仓库则跳过 init
    Result<GitResult> initRepo(const std::string& remoteUrl,
                                const std::string& branch = "main");

    // 克隆远程仓库到本地（仅当本地目录不存在 .git 时调用）
    Result<GitResult> cloneRepo(const std::string& remoteUrl,
                                 const std::string& token,
                                 const std::string& branch = "main");

    // git add <file>（相对路径，从 repoDir 算起）
    Result<GitResult> addFile(const std::string& relativePath);

    // git commit -m <message>
    Result<GitResult> commit(const std::string& message);

    // git push origin <branch>
    Result<GitResult> push(const std::string& remoteUrl,
                            const std::string& token,
                            const std::string& branch);

    // git pull origin <branch>（先 fetch 再 merge）
    Result<GitResult> pull(const std::string& remoteUrl,
                            const std::string& token,
                            const std::string& branch);

    // git status --porcelain → 检查是否有未提交/冲突
    Result<RepoStatus> status();

    // 自动同步流程：dump SQL → add changed files → commit → push
    // sqlDumpFunc: 调用方提供的 SQL dump 函数（exportToFile）
    // fileChangedFunc: 调用方提供的「是否有文件变更」检查
    Result<GitResult> autoSync(
        const std::string& remoteUrl,
        const std::string& token,
        const std::string& branch,
        const std::string& commitMessage,
        std::function<Result<void>()> sqlDumpFunc);

    // 检查 git 是否可用（git --version）
    static bool isGitAvailable();

    // 从远程 URL + token 构造认证 URL
    static std::string buildAuthUrl(const std::string& repoUrl,
                                    const std::string& token);

    const std::filesystem::path& repoDir() const { return repoDir_; }

private:
    // 执行 git 命令，返回 {exitCode, stdout+stderr}
    GitResult runGit(const std::vector<std::string>& args,
                      const std::string& authUrl = "");

    // 解析 git status --porcelain 输出中的冲突文件
    static std::vector<std::string> parseConflictFiles(const std::string& statusOutput);

    std::filesystem::path repoDir_;
    std::mutex mutex_;  // 一次只跑一个 git 命令
};

} // namespace core::git_sync