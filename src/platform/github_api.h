// src/platform/github_api.h
// GitHub Contents / Git refs API 客户端（libcurl，复用 platform::http::send）。
// 不依赖外部 git 可执行文件；token 只进 Authorization 头，绝不进 URL / argv / 日志。
#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"
#include "platform/github_api_log.h"
#include "platform/http_client.h"

namespace platform::github {

struct RepoInfo { std::string fullName, defaultBranch; bool canPush = false; bool isPrivate = false; };
struct FileInfo { bool exists = false; std::string content; std::string sha; };
// git/trees 条目（F7 拉取全树）。type=blob 才有 size；目录条目 size 恒 0。
struct TreeEntry { std::string path; std::string sha; uint64_t size = 0; };
struct TestStep { std::string name; bool ok = false; int durationMs = 0; std::string detail; };
struct TestReport { bool ok = false; std::vector<TestStep> steps; std::string summary; };
struct RetryPolicy { int attempts = 3; std::vector<int> backoffMs{1000, 3000, 9000}; };

class GithubApi {
public:
    // 发送器签名：默认 = platform::http::send。测试注入假发送器时**零真网**。
    using Sender = std::function<core::Result<platform::http::Response>(const platform::http::Request&)>;

    GithubApi(std::string token, std::string owner, std::string repo, RetryPolicy policy = {});

    // 注入点（测试用）：发送器默认真实 send；退避 sleep 默认 std::this_thread::sleep_for，
    // 传 nullptr 复位。均为**静态**，用例结束必须复位，否则污染同进程后续用例。
    void setSender(Sender sender);
    static void setSleepFunction(std::function<void(int)> fn);

    core::Result<RepoInfo> getRepo();
    core::Result<std::string> getAuthenticatedUser();                 // GET /user → login
    core::Result<FileInfo> getFile(const std::string& path, const std::string& ref = "");
    // F7：GET /git/trees/{branch}?recursive=1 全树（含子目录）。响应带 truncated=true
    // → fail 不补取分页子树（dump+notes 仓库极小，截断实不可达；真出现时明确报错优于静默漏文件）。
    core::Result<std::vector<TreeEntry>> listTree(const std::string& branch);
    core::Result<std::string> putFile(const std::string& path, const std::string& content,
                                      const std::string& message, const std::string& shaIfMatch = "");
    core::Result<void> deleteFile(const std::string& path, const std::string& message,
                                  const std::string& shaIfMatch);
    // git refs：测试连接用（建临时分支 → 写探针 → 删分支）。fromBranch = 基点分支（取其 HEAD sha）。
    core::Result<void> createRef(const std::string& branch, const std::string& fromBranch);
    core::Result<void> deleteRef(const std::string& branch);
    TestReport testConnection(std::function<void(const TestStep&)> onStep = {});
    std::vector<ApiLogEntry> recentLog() const;                        // 环形，上限 50
    static std::pair<std::string,std::string> parseRepoUrl(const std::string& url); // → owner, repo
    static std::string maskUrl(const std::string& url);                // 兜底：任何 token/凭据段 → ***

private:
    // 一次逻辑请求的最终态：拿到 HTTP 响应（resp.statusCode 有效）或网络层失败。
    struct Attempt {
        platform::http::Response resp;
        bool networkError = false;
        bool localError = false;              // 发送前本地校验失败：不发请求、不重试、不进日志
        core::ErrorCode errorCode = core::ErrorCode::Ok;
        std::string errorMessage;
    };

    // 本地错误（无 token / owner-repo 为空）在**发请求之前**返回，故天然不重试、不进日志。
    core::Result<void> validateConfig() const;

    // 带重试/退避的发送：仅网络类传输错误 / 5xx / 429（及限流态 403）重试；每次尝试都进环形日志。
    Attempt doRequest(const std::string& method, const std::string& url,
                      const std::string& body, const std::string& accept);
    // 分支定向 PUT：公开 putFile 不含 branch 参数，testConnection 的临时分支探针走这里。
    core::Result<std::string> putFileOnBranch(const std::string& path, const std::string& content,
                                              const std::string& message,
                                              const std::string& shaIfMatch,
                                              const std::string& branch);
    int backoffFor(int attemptIndex) const;
    void appendLog(const ApiLogEntry& entry);
    std::string contentsUrl(const std::string& path) const;

    static std::function<void(int)>& sleepFn();
    static void sleepMs(int ms);

    std::string token_, owner_, repo_;
    RetryPolicy policy_;
    Sender sender_;

    mutable std::mutex logMutex_;
    std::deque<ApiLogEntry> log_;
};

} // namespace platform::github
