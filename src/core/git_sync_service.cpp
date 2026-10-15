// src/core/git_sync_service.cpp
// Git 同步服务实现：通过 popen() 调系统 git，线程安全。
#include "git_sync_service.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "logging.h"
#include "paths.h"
#include "result.h"

namespace core::git_sync {

namespace {

// 执行命令并捕获 stdout+stderr。返回 {exitCode, output}。
GitResult execCommand(const std::string& cmd) {
    GitResult result;
    // Windows: popen 在 MinGW 下工作正常
    std::array<char, 4096> buffer;
    // 合并 stderr 到 stdout：2>&1
    std::string fullCmd = cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    if (pipe == nullptr) {
        result.success = false;
        result.output = "popen 失败";
        result.exitCode = -1;
        return result;
    }
    std::ostringstream out;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out << buffer.data();
    }
    int status = pclose(pipe);
    result.output = out.str();
    // Windows: _pclose 返回的 exit code
#ifdef _WIN32
    result.exitCode = status;
#else
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = status;
    }
#endif
    result.success = (result.exitCode == 0);
    return result;
}

// 构建 git 命令（带 -C repoDir 指定工作目录）
std::string gitCmd(const std::filesystem::path& repoDir,
                    const std::vector<std::string>& args) {
    std::ostringstream cmd;
    cmd << "git -C \"" << repoDir.string() << "\"";
    for (const auto& a : args) {
        cmd << " " << a;
    }
    return cmd.str();
}

// 构建带认证 URL 和 SSL 后端的 push/pull/clone 命令
std::string gitAuthCmd(const std::filesystem::path& repoDir,
                        const std::string& authUrl,
                        const std::vector<std::string>& args) {
    std::ostringstream cmd;
    // Windows: 强制使用 OpenSSL 后端（schannel 在非交互环境下可能失败）
#ifdef _WIN32
    cmd << "git -c http.sslBackend=openssl -c credential.helper= ";
#else
    cmd << "git ";
#endif
    if (!repoDir.empty()) {
        cmd << "-C \"" << repoDir.string() << "\" ";
    }
    for (const auto& a : args) {
        cmd << a << " ";
    }
    // URL 作为最后一个位置参数
    cmd << "\"" << authUrl << "\"";
    return cmd.str();
}

} // namespace

GitSyncService::GitSyncService(const std::filesystem::path& repoDir)
    : repoDir_(repoDir) {}

std::string GitSyncService::buildAuthUrl(const std::string& repoUrl,
                                          const std::string& token) {
    // https://github.com/user/repo.git → https://<token>@github.com/user/repo.git
    if (token.empty()) return repoUrl;
    const std::string prefix = "https://";
    if (repoUrl.find(prefix) != 0) return repoUrl;
    return prefix + token + "@" + repoUrl.substr(prefix.size());
}

bool GitSyncService::isGitAvailable() {
    auto r = execCommand("git --version");
    return r.success;
}

GitResult GitSyncService::runGit(const std::vector<std::string>& args,
                                   const std::string& authUrl) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string cmd;
    if (!authUrl.empty()) {
        cmd = gitAuthCmd(repoDir_, authUrl, args);
    } else {
        cmd = gitCmd(repoDir_, args);
    }
    LOG_INFO("git_sync: 执行命令");
    return execCommand(cmd);
}

std::vector<std::string> GitSyncService::parseConflictFiles(
    const std::string& statusOutput) {
    std::vector<std::string> files;
    std::istringstream iss(statusOutput);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() >= 2 &&
            (line[0] == 'U' || line[1] == 'U' ||
             (line[0] == 'D' && line[1] == 'D') ||
             (line[0] == 'A' && line[1] == 'A'))) {
            // UU, DD, AA = 冲突。文件路径从第 3 个字符开始
            if (line.size() > 3) {
                files.push_back(line.substr(3));
            }
        }
    }
    return files;
}

Result<GitResult> GitSyncService::initRepo(const std::string& remoteUrl,
                                             const std::string& branch) {
    // 先检查是否已是 git 仓库
    auto s = status();
    if (s.isFail()) {
        return Result<GitResult>::fail(s.errorCode(), s.errorMessage());
    }
    if (s.value().isRepo) {
        GitResult r;
        r.success = true;
        r.output = "已是 Git 仓库，跳过 init";
        // 检查并更新 remote
        if (!s.value().hasRemote) {
            auto addRemote = runGit({"remote", "add", "origin", remoteUrl});
            if (!addRemote.success) {
                return Result<GitResult>::fail(
                    ErrorCode::Internal,
                    "添加远程仓库失败: " + addRemote.output);
            }
            LOG_INFO("git_sync: 已添加 remote origin → " + remoteUrl);
        }
        return Result<GitResult>::ok(std::move(r));
    }

    // git init
    auto init = runGit({"init"});
    if (!init.success) {
        return Result<GitResult>::fail(
            ErrorCode::Internal, "git init 失败: " + init.output);
    }
    LOG_INFO("git_sync: git init 完成");

    // git remote add origin
    if (!remoteUrl.empty()) {
        auto addRemote = runGit({"remote", "add", "origin", remoteUrl});
        if (!addRemote.success) {
            // 不 fail：用户可能还没配置 URL
            LOG_WARN("git_sync: 添加 remote 失败（可能已存在）: " + addRemote.output);
        } else {
            LOG_INFO("git_sync: remote origin → " + remoteUrl);
        }
    }

    // 设置默认分支名
    runGit({"checkout", "-b", branch});

    // git config（基本身份）
    runGit({"config", "user.email", "devdesk@local"});
    runGit({"config", "user.name", "DevDesk"});

    GitResult result;
    result.success = true;
    result.output = init.output;
    return Result<GitResult>::ok(std::move(result));
}

Result<GitResult> GitSyncService::cloneRepo(const std::string& remoteUrl,
                                              const std::string& token,
                                              const std::string& branch) {
    std::string authUrl = buildAuthUrl(remoteUrl, token);

    // clone 到临时目录，再搬内容
    // 简化：如果 repoDir_ 已存在 .git → 走 pull
    auto s = status();
    if (s.isOk() && s.value().isRepo) {
        return pull(remoteUrl, token, branch);
    }

    // 否则直接 clone
    // git clone --branch <branch> <authUrl> <repoDir>
    std::ostringstream cmd;
#ifdef _WIN32
    cmd << "git -c http.sslBackend=openssl -c credential.helper= clone --branch "
        << branch << " \"" << authUrl << "\" \"" << repoDir_.string() << "\"";
#else
    cmd << "git clone --branch " << branch << " \""
        << authUrl << "\" \"" << repoDir_.string() << "\"";
#endif
    auto r = execCommand(cmd.str());
    if (!r.success) {
        return Result<GitResult>::fail(
            ErrorCode::NetworkUnreachable,
            "git clone 失败: " + r.output);
    }
    LOG_INFO("git_sync: clone 完成");
    return Result<GitResult>::ok(std::move(r));
}

Result<GitResult> GitSyncService::addFile(const std::string& relativePath) {
    auto r = runGit({"add", "--", relativePath});
    if (!r.success) {
        return Result<GitResult>::fail(
            ErrorCode::Internal,
            "git add " + relativePath + " 失败: " + r.output);
    }
    return Result<GitResult>::ok(std::move(r));
}

Result<GitResult> GitSyncService::commit(const std::string& message) {
    auto r = runGit({"commit", "-m", message});
    if (!r.success) {
        // "nothing to commit" 不算失败
        if (r.output.find("nothing to commit") != std::string::npos ||
            r.output.find("nothing added to commit") != std::string::npos) {
            r.success = true;
            return Result<GitResult>::ok(std::move(r));
        }
        return Result<GitResult>::fail(
            ErrorCode::Internal,
            "git commit 失败: " + r.output);
    }
    LOG_INFO("git_sync: commit 完成: " + message);
    return Result<GitResult>::ok(std::move(r));
}

Result<GitResult> GitSyncService::push(const std::string& remoteUrl,
                                         const std::string& token,
                                         const std::string& branch) {
    std::string authUrl = buildAuthUrl(remoteUrl, token);
    auto r = runGit({"push", "origin", branch}, authUrl);
    if (!r.success) {
        return Result<GitResult>::fail(
            ErrorCode::NetworkUnreachable,
            "git push 失败: " + r.output);
    }
    LOG_INFO("git_sync: push 完成 → " + branch);
    return Result<GitResult>::ok(std::move(r));
}

Result<GitResult> GitSyncService::pull(const std::string& remoteUrl,
                                         const std::string& token,
                                         const std::string& branch) {
    std::string authUrl = buildAuthUrl(remoteUrl, token);
    auto r = runGit({"pull", "origin", branch}, authUrl);
    // 检查是否有冲突
    if (!r.success) {
        auto s = runGit({"status", "--porcelain"});
        r.conflictFiles = parseConflictFiles(s.output);
        if (!r.conflictFiles.empty()) {
            LOG_WARN("git_sync: pull 有冲突，涉及 " +
                     std::to_string(r.conflictFiles.size()) + " 个文件");
        }
    }
    if (r.exitCode != 0 && r.conflictFiles.empty()) {
        return Result<GitResult>::fail(
            ErrorCode::NetworkUnreachable,
            "git pull 失败: " + r.output);
    }
    // 有冲突 → 返回 success=false 但带 conflictFiles
    LOG_INFO("git_sync: pull 完成");
    return Result<GitResult>::ok(std::move(r));
}

Result<RepoStatus> GitSyncService::status() {
    RepoStatus s;

    // 检查是否是 git 仓库
    auto revParse = runGit({"rev-parse", "--git-dir"});
    s.isRepo = revParse.success;

    if (!s.isRepo) {
        return Result<RepoStatus>::ok(s);
    }

    // 检查 remote
    auto remote = runGit({"remote", "get-url", "origin"});
    s.hasRemote = remote.success;
    if (s.hasRemote) {
        // trim newline
        s.remoteUrl = remote.output;
        while (!s.remoteUrl.empty() &&
               (s.remoteUrl.back() == '\n' || s.remoteUrl.back() == '\r')) {
            s.remoteUrl.pop_back();
        }
    }

    // 当前分支
    auto branch = runGit({"branch", "--show-current"});
    if (branch.success) {
        s.branch = branch.output;
        while (!s.branch.empty() &&
               (s.branch.back() == '\n' || s.branch.back() == '\r')) {
            s.branch.pop_back();
        }
    }

    // git status --porcelain
    auto st = runGit({"status", "--porcelain"});
    if (st.success) {
        s.hasUncommitted = !st.output.empty() &&
            st.output.find_first_not_of(" \t\n\r") != std::string::npos;
        s.conflictFiles = parseConflictFiles(st.output);
        s.hasConflicts = !s.conflictFiles.empty();
    }

    // 检查是否有未推送的提交
    if (s.hasRemote && !s.branch.empty()) {
        auto ahead = runGit({"rev-list", "--count",
                              "origin/" + s.branch + ".." + s.branch});
        if (ahead.success) {
            try {
                int count = std::stoi(ahead.output);
                s.hasUnpushed = (count > 0);
            } catch (...) {
                s.hasUnpushed = false;
            }
        }
    }

    return Result<RepoStatus>::ok(s);
}

Result<GitResult> GitSyncService::autoSync(
    const std::string& remoteUrl,
    const std::string& token,
    const std::string& branch,
    const std::string& commitMessage,
    std::function<Result<void>()> sqlDumpFunc) {

    // 1. 确保是 git 仓库
    auto s = status();
    if (s.isFail()) {
        return Result<GitResult>::fail(s.errorCode(), s.errorMessage());
    }
    if (!s.value().isRepo) {
        auto init = initRepo(remoteUrl, branch);
        if (init.isFail()) return init;
    }

    // 2. 导出 SQL dump
    if (sqlDumpFunc) {
        auto dumpResult = sqlDumpFunc();
        if (dumpResult.isFail()) {
            LOG_WARN("git_sync: SQL dump 失败: " + dumpResult.errorMessage());
            // 不阻断：dump 失败不影响 md 文件同步
        }
    }

    // 3. git add 所有变更
    auto add = runGit({"add", "-A"});
    if (!add.success) {
        return Result<GitResult>::fail(
            ErrorCode::Internal,
            "git add 失败: " + add.output);
    }

    // 4. git commit
    auto commit = runGit({"commit", "-m", commitMessage});
    if (!commit.success &&
        commit.output.find("nothing to commit") == std::string::npos &&
        commit.output.find("nothing added to commit") == std::string::npos) {
        return Result<GitResult>::fail(
            ErrorCode::Internal,
            "git commit 失败: " + commit.output);
    }
    // "nothing to commit" → 不 push，不算失败
    if (commit.output.find("nothing to commit") != std::string::npos ||
        commit.output.find("nothing added to commit") != std::string::npos) {
        GitResult result;
        result.success = true;
        result.output = "没有需要提交的变更";
        return Result<GitResult>::ok(std::move(result));
    }

    // 5. git push
    if (!token.empty() && !remoteUrl.empty()) {
        auto pushResult = push(remoteUrl, token, branch);
        if (pushResult.isFail()) {
            // push 失败不丢 commit（下次会推上去）
            LOG_WARN("git_sync: push 失败（commit 已保留）: " +
                     pushResult.errorMessage());
        }
    }

    return Result<GitResult>::ok(std::move(commit));
}

} // namespace core::git_sync