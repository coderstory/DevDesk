// src/platform/github_api_log.h
// GitHub API 调用审计日志条目（环形缓冲，上限 50 = github_api.cpp 的 kMaxLog）。
// 跨文件不变量：urlMasked 里**绝不出现 token** —— 任何写日志的路径都必须先过 GithubApi::maskUrl。
#pragma once

#include <string>

namespace platform::github {

struct ApiLogEntry {
    std::string method;        // GET/PUT/DELETE
    std::string urlMasked;     // 绝不出现 token
    long statusCode = 0;
    int durationMs = 0;
    std::string bodyTail;      // 响应体末尾 ≤512 字节
    bool networkError = false;
    std::string errorMessage;
};

} // namespace platform::github
