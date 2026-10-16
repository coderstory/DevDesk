#include "doctest.h"
#include "platform/github_api.h"
#include "platform/json.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

// 全部用例零真网：GithubApi 的发送器与退避 sleep 都被注入为可计数假实现。
// 断言口径——「尝试次数」用假发送器收到的请求数证明，「退避序列」用假 sleep 记录的毫秒证明。
//
// GithubApi 含 std::mutex（不可拷贝/移动），故一律在用例内**就地构造**、经引用接线，不能按值返回。

namespace {

using Resp = platform::http::Response;
using Res = core::Result<platform::http::Response>;

// 「拿到 HTTP 响应」形态（status 可为 200/4xx/5xx）
Res okResp(int status, std::string body = "", std::map<std::string, std::string> headers = {}) {
    Resp r;
    r.statusCode = status;
    r.body = std::move(body);
    r.headers = std::move(headers);
    return Res::ok(std::move(r));
}

// 网络层失败（与真实 platform::http::send 的错误形态一致）
Res failNet(const std::string& msg) {
    return Res::fail(core::ErrorCode::NetworkUnreachable, msg);
}

// 按序回放脚本化响应；脚本用完后重复最后一条。记录每个请求。
struct Recorder {
    std::vector<platform::http::Request> reqs;
    std::vector<Res> plan;

    Res operator()(const platform::http::Request& r) {
        reqs.push_back(r);
        const std::size_t i = reqs.size() - 1;
        if (i < plan.size()) return plan[i];
        if (!plan.empty()) return plan.back();
        return failNet("no scripted response");
    }
};

// 把假发送器接到 api 上
void wire(platform::github::GithubApi& api, Recorder& rec) {
    api.setSender([&rec](const platform::http::Request& r) { return rec(r); });
}

// 注入退避 sleep 并在作用域结束复位（静态注入点，不复位会污染同进程后续用例）
struct SleepGuard {
    explicit SleepGuard(std::vector<int>& sink) {
        platform::github::GithubApi::setSleepFunction([&sink](int ms) { sink.push_back(ms); });
    }
    ~SleepGuard() { platform::github::GithubApi::setSleepFunction(nullptr); }
    SleepGuard(const SleepGuard&) = delete;
    SleepGuard& operator=(const SleepGuard&) = delete;
};

std::string headerOf(const platform::http::Request& r, const std::string& key) {
    const auto it = r.headers.find(key);
    return it == r.headers.end() ? std::string() : it->second;
}

} // namespace

// ---- 重试分类 ----

REGISTER_TEST(github_retry_network_then_ok, "github: 网络错误重试，成功即停", {
    Recorder rec;
    rec.plan.push_back(failNet("connection reset"));
    rec.plan.push_back(failNet("connection reset"));
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isOk());
    DOCTEST_CHECK_EQ(res.value(), std::string("me"));
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(sleeps, (std::vector<int>{10, 20}));
    return true;
})

REGISTER_TEST(github_retry_500, "github: 5xx 重试", {
    Recorder rec;
    rec.plan.push_back(okResp(500, "boom"));
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isOk());
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(sleeps, (std::vector<int>{10}));
    return true;
})

REGISTER_TEST(github_retry_429_honors_retry_after, "github: 429 重试且以 Retry-After 为准", {
    Recorder rec;
    rec.plan.push_back(okResp(429, "slow down", {{"retry-after", "5"}}));
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isOk());
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(sleeps, (std::vector<int>{5000}));   // 5s，而非退避序列的 10ms
    return true;
})

REGISTER_TEST(github_retry_after_capped_60s, "github: Retry-After 上限 60s", {
    Recorder rec;
    rec.plan.push_back(okResp(429, "slow down", {{"retry-after", "120"}}));
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isOk());
    DOCTEST_CHECK_EQ(sleeps, (std::vector<int>{60000}));
    return true;
})

REGISTER_TEST(github_retry_403_only_when_ratelimited, "github: 403 仅在限流态重试", {
    // 限流态（x-ratelimit-remaining: 0）→ 重试
    {
        Recorder rec;
        rec.plan.push_back(okResp(403, "", {{"x-ratelimit-remaining", "0"}}));
        rec.plan.push_back(okResp(200, R"({"login":"me"})"));
        std::vector<int> sleeps;
        platform::github::GithubApi api("tok", "owner", "repo",
                                        platform::github::RetryPolicy{3, {10, 20, 30}});
        wire(api, rec);
        SleepGuard guard(sleeps);
        const auto res = api.getAuthenticatedUser();
        DOCTEST_CHECK(res.isOk());
        DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(2));
    }
    // 普通 403（无权限）→ 不重试
    {
        Recorder rec;
        rec.plan.push_back(okResp(403, "", {{"x-ratelimit-remaining", "55"}}));
        std::vector<int> sleeps;
        platform::github::GithubApi api("tok", "owner", "repo",
                                        platform::github::RetryPolicy{3, {10, 20, 30}});
        wire(api, rec);
        SleepGuard guard(sleeps);
        const auto res = api.getAuthenticatedUser();
        DOCTEST_CHECK(res.isFail());
        DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
        DOCTEST_CHECK(sleeps.empty());
    }
    return true;
})

REGISTER_TEST(github_no_retry_401, "github: 401 不重试", {
    Recorder rec;
    rec.plan.push_back(okResp(401, R"({"message":"Bad credentials"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK_EQ(res.errorCode(), core::ErrorCode::InvalidApiKey);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(sleeps.empty());
    return true;
})

REGISTER_TEST(github_no_retry_404, "github: 404 不重试（getFile 视为不存在）", {
    Recorder rec;
    rec.plan.push_back(okResp(404));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getFile("missing.txt");
    DOCTEST_CHECK(res.isOk());
    DOCTEST_CHECK_FALSE(res.value().exists);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(sleeps.empty());
    return true;
})

REGISTER_TEST(github_409_maps_to_conflict_no_retry, "github: 409 → Conflict 且不重试", {
    Recorder rec;
    rec.plan.push_back(okResp(409, R"({"message":"sha does not match"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.putFile("note.md", "content", "msg", "stale-sha");
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK_EQ(res.errorCode(), core::ErrorCode::Conflict);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(sleeps.empty());
    return true;
})

REGISTER_TEST(github_422_maps_to_validation, "github: 422 → ValidationError", {
    Recorder rec;
    rec.plan.push_back(okResp(422, R"({"message":"invalid"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.putFile("note.md", "content", "msg");
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK_EQ(res.errorCode(), core::ErrorCode::ValidationError);
    return true;
})

REGISTER_TEST(github_backoff_sequence_exhausts_attempts, "github: 全失败时退避序列与尝试次数", {
    Recorder rec;
    rec.plan.push_back(failNet("a"));
    rec.plan.push_back(failNet("b"));
    rec.plan.push_back(failNet("c"));
    rec.plan.push_back(failNet("d"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{4, {1, 2, 3}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK_EQ(res.errorCode(), core::ErrorCode::NetworkUnreachable);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(4));   // attempts = 4
    DOCTEST_CHECK_EQ(sleeps, (std::vector<int>{1, 2, 3}));            // 末次不 sleep
    return true;
})

// ---- 内容编码 ----

REGISTER_TEST(github_base64_roundtrip, "github: putFile 编码与 getFile 解码互为逆运算", {
    Recorder putRec;
    putRec.plan.push_back(okResp(201, R"({"content":{"sha":"newsha"}})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, putRec);
    SleepGuard guard(sleeps);

    const auto put = api.putFile("f.txt", "hello world", "msg");
    DOCTEST_CHECK(put.isOk());
    DOCTEST_CHECK_EQ(put.value(), std::string("newsha"));

    // 请求体里的 content 必须是标准 base64（含 '=' 填充），与 EVP_EncodeBlock 输出逐字一致
    DOCTEST_CHECK_EQ(putRec.reqs.size(), static_cast<std::size_t>(1));
    const auto parsed = platform::json::parse(putRec.reqs[0].body);
    DOCTEST_CHECK(parsed.isOk());
    const platform::json::JsonValue contentField = parsed.value().asObject().get("content");
    DOCTEST_CHECK(contentField.isString());
    DOCTEST_CHECK_EQ(contentField.asString(), std::string("aGVsbG8gd29ybGQ="));

    // 再把同一段 base64 喂给 getFile，验证解码回原文
    Recorder getRec;
    getRec.plan.push_back(
        okResp(200, R"({"sha":"s1","encoding":"base64","content":"aGVsbG8gd29ybGQ="})"));
    wire(api, getRec);

    const auto got = api.getFile("f.txt");
    DOCTEST_CHECK(got.isOk());
    DOCTEST_CHECK(got.value().exists);
    DOCTEST_CHECK_EQ(got.value().content, std::string("hello world"));
    DOCTEST_CHECK_EQ(got.value().sha, std::string("s1"));
    return true;
})

REGISTER_TEST(github_getfile_large_uses_raw_accept, "github: >1MB 走 Accept raw 直取原文", {
    Recorder rec;
    rec.plan.push_back(
        okResp(200, R"({"sha":"bigsha","size":2000000,"encoding":"none","content":""})"));
    rec.plan.push_back(okResp(200, "RAW-BODY"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto got = api.getFile("big.bin");
    DOCTEST_CHECK(got.isOk());
    DOCTEST_CHECK_EQ(got.value().content, std::string("RAW-BODY"));
    DOCTEST_CHECK_EQ(got.value().sha, std::string("bigsha"));
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(2));
    DOCTEST_CHECK_EQ(headerOf(rec.reqs[0], "Accept"), std::string("application/vnd.github+json"));
    DOCTEST_CHECK_EQ(headerOf(rec.reqs[1], "Accept"), std::string("application/vnd.github.raw"));
    return true;
})

// ---- 认证头与 token 安全 ----

REGISTER_TEST(github_token_only_in_header_never_in_url_or_log,
              "github: token 只进 header，URL/日志不含", {
    Recorder rec;
    rec.plan.push_back(okResp(401));

    std::vector<int> sleeps;
    platform::github::GithubApi api("ghp_SECRETTOKEN", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.getAuthenticatedUser();
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(rec.reqs[0].url.find("ghp_SECRETTOKEN") == std::string::npos);
    DOCTEST_CHECK_EQ(headerOf(rec.reqs[0], "Authorization"),
                     std::string("Bearer ghp_SECRETTOKEN"));
    DOCTEST_CHECK_EQ(headerOf(rec.reqs[0], "Accept"), std::string("application/vnd.github+json"));
    DOCTEST_CHECK_EQ(headerOf(rec.reqs[0], "X-GitHub-Api-Version"), std::string("2022-11-28"));

    const auto log = api.recentLog();
    DOCTEST_CHECK_EQ(log.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(log[0].urlMasked.find("ghp_SECRETTOKEN") == std::string::npos);
    DOCTEST_CHECK_EQ(log[0].statusCode, 401L);
    DOCTEST_CHECK_EQ(log[0].method, std::string("GET"));
    return true;
})

// ---- 环形日志 ----

REGISTER_TEST(github_log_ring_capped_at_50, "github: 环形日志上限 50", {
    Recorder rec;
    rec.plan.push_back(okResp(404));   // 每次 getFile 恰好 1 次请求

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    for (int i = 0; i < 60; ++i) {
        const auto f = api.getFile("nope");
        DOCTEST_CHECK(f.isOk());
    }
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(60));
    DOCTEST_CHECK_EQ(api.recentLog().size(), static_cast<std::size_t>(50));
    return true;
})

// ---- maskUrl ----

REGISTER_TEST(github_mask_url_userinfo, "github: maskUrl 掩 scheme userinfo", {
    DOCTEST_CHECK_EQ(platform::github::GithubApi::maskUrl("https://token@host/path"),
                     std::string("https://***@host/path"));
    DOCTEST_CHECK_EQ(platform::github::GithubApi::maskUrl("https://user:pass@github.com/o/r"),
                     std::string("https://***@github.com/o/r"));
    return true;
})

REGISTER_TEST(github_mask_url_query_token, "github: maskUrl 掩查询参数里的 token", {
    DOCTEST_CHECK_EQ(
        platform::github::GithubApi::maskUrl("https://api.github.com/x?token=abc&a=b"),
        std::string("https://api.github.com/x?token=***&a=b"));
    DOCTEST_CHECK_EQ(
        platform::github::GithubApi::maskUrl("https://api.github.com/x?access_token=zzz"),
        std::string("https://api.github.com/x?access_token=***"));
    return true;
})

REGISTER_TEST(github_mask_url_token_prefix_scrub, "github: maskUrl 兜底擦除已知 token 前缀", {
    DOCTEST_CHECK_EQ(
        platform::github::GithubApi::maskUrl("https://api.github.com/ghp_AAAABBBBCCCC"),
        std::string("https://api.github.com/ghp_***"));
    // 无凭据的普通 URL 不被改写
    DOCTEST_CHECK_EQ(platform::github::GithubApi::maskUrl("https://api.github.com/repos/o/r"),
                     std::string("https://api.github.com/repos/o/r"));
    return true;
})

// ---- parseRepoUrl ----

REGISTER_TEST(github_parse_repo_url, "github: parseRepoUrl 处理 .git/末尾斜杠/SSH", {
    const auto https = platform::github::GithubApi::parseRepoUrl("https://github.com/owner/repo");
    DOCTEST_CHECK_EQ(https.first, std::string("owner"));
    DOCTEST_CHECK_EQ(https.second, std::string("repo"));

    const auto dotGit =
        platform::github::GithubApi::parseRepoUrl("https://github.com/owner/repo.git");
    DOCTEST_CHECK_EQ(dotGit.second, std::string("repo"));

    const auto trailing =
        platform::github::GithubApi::parseRepoUrl("https://github.com/owner/repo/");
    DOCTEST_CHECK_EQ(trailing.first, std::string("owner"));
    DOCTEST_CHECK_EQ(trailing.second, std::string("repo"));

    const auto scp = platform::github::GithubApi::parseRepoUrl("git@github.com:owner/repo.git");
    DOCTEST_CHECK_EQ(scp.first, std::string("owner"));
    DOCTEST_CHECK_EQ(scp.second, std::string("repo"));

    const auto ssh =
        platform::github::GithubApi::parseRepoUrl("ssh://git@github.com/owner/repo.git");
    DOCTEST_CHECK_EQ(ssh.first, std::string("owner"));
    DOCTEST_CHECK_EQ(ssh.second, std::string("repo"));

    const auto bare = platform::github::GithubApi::parseRepoUrl("owner/repo");
    DOCTEST_CHECK_EQ(bare.first, std::string("owner"));
    DOCTEST_CHECK_EQ(bare.second, std::string("repo"));

    const auto empty = platform::github::GithubApi::parseRepoUrl("");
    DOCTEST_CHECK_EQ(empty.first, std::string(""));
    DOCTEST_CHECK_EQ(empty.second, std::string(""));
    return true;
})

// ---- testConnection ----

REGISTER_TEST(github_test_connection_short_circuits_on_auth_fail,
              "github: testConnection 首步失败即短路", {
    Recorder rec;
    rec.plan.push_back(okResp(401));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    std::vector<platform::github::TestStep> seen;
    const auto report = api.testConnection(
        [&seen](const platform::github::TestStep& s) { seen.push_back(s); });

    DOCTEST_CHECK_FALSE(report.ok);
    DOCTEST_CHECK_EQ(report.steps.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK_EQ(seen.size(), static_cast<std::size_t>(1));   // onStep 每步回调一次
    DOCTEST_CHECK_FALSE(report.steps[0].ok);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));   // 未进入第 ②③ 步
    DOCTEST_CHECK(report.summary.find("令牌") != std::string::npos);
    return true;
})

REGISTER_TEST(github_test_connection_success_three_steps, "github: testConnection 三步全过（临时分支探针）", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));                       // ① GET /user
    rec.plan.push_back(okResp(                                                  // ② GET /repos
        200, R"({"full_name":"owner/repo","default_branch":"main","permissions":{"push":true}})"));
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha","type":"commit"}})")); // ③a 基点 HEAD
    rec.plan.push_back(okResp(201, R"({})"));                                   // ③b POST 建临时分支
    rec.plan.push_back(okResp(201, R"({"content":{"sha":"newsha"}})"));         // ③c PUT 探针文件
    rec.plan.push_back(okResp(204, ""));                                        // ③d DELETE 删临时分支

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    std::vector<std::string> stepNames;
    const auto report = api.testConnection([&stepNames](const platform::github::TestStep& s) {
        stepNames.push_back(s.name);
    });

    DOCTEST_CHECK(report.ok);
    DOCTEST_CHECK_EQ(report.steps.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK_EQ(stepNames.size(), static_cast<std::size_t>(3));
    for (const auto& s : report.steps) DOCTEST_CHECK(s.ok);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(6));
    // ③a 建分支前先取基点（default_branch = main）HEAD
    DOCTEST_CHECK_EQ(rec.reqs[2].method, std::string("GET"));
    DOCTEST_CHECK(rec.reqs[2].url.find("/git/ref/heads/main") != std::string::npos);
    // ③b POST refs：ref 名 + 基点 sha
    DOCTEST_CHECK_EQ(rec.reqs[3].method, std::string("POST"));
    DOCTEST_CHECK(rec.reqs[3].url.find("/git/refs") != std::string::npos);
    DOCTEST_CHECK(rec.reqs[3].body.find("refs/heads/devdesk-test-") != std::string::npos);
    DOCTEST_CHECK(rec.reqs[3].body.find("basesha") != std::string::npos);
    // ③c 探针写在临时分支上（body 带 branch 字段），不碰默认分支
    DOCTEST_CHECK_EQ(rec.reqs[4].method, std::string("PUT"));
    DOCTEST_CHECK(rec.reqs[4].url.find("/contents/devdesk-probe.txt") != std::string::npos);
    DOCTEST_CHECK(rec.reqs[4].body.find("devdesk-test-") != std::string::npos);
    // ③d 删临时分支
    DOCTEST_CHECK_EQ(rec.reqs[5].method, std::string("DELETE"));
    DOCTEST_CHECK(rec.reqs[5].url.find("/git/refs/heads/devdesk-test-") != std::string::npos);
    DOCTEST_CHECK(report.summary.find("连通性正常") != std::string::npos);
    return true;
})

REGISTER_TEST(github_test_connection_delete_ref_failure_warns_in_summary,
              "github: 删临时分支失败时 summary 提示残留", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));
    rec.plan.push_back(okResp(
        200, R"({"full_name":"owner/repo","default_branch":"main","permissions":{"push":true}})"));
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha"}})"));
    rec.plan.push_back(okResp(201, R"({})"));
    rec.plan.push_back(okResp(201, R"({"content":{"sha":"newsha"}})"));
    rec.plan.push_back(okResp(500, "boom"));   // 删分支失败（attempts=1 不重试）

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto report = api.testConnection();
    // 读写已被证明，连通性成立；但 summary 必须点名残留分支
    DOCTEST_CHECK(report.ok);
    DOCTEST_CHECK_EQ(report.steps.size(), static_cast<std::size_t>(3));
    DOCTEST_CHECK(report.steps[2].ok);
    DOCTEST_CHECK(report.summary.find("连通性正常") != std::string::npos);
    DOCTEST_CHECK(report.summary.find("残留") != std::string::npos);
    DOCTEST_CHECK(report.summary.find("devdesk-test-") != std::string::npos);
    return true;
})

REGISTER_TEST(github_test_connection_put_failure_still_cleans_branch,
              "github: 探针写入失败仍删临时分支", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));
    rec.plan.push_back(okResp(
        200, R"({"full_name":"owner/repo","default_branch":"main","permissions":{"push":true}})"));
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha"}})"));
    rec.plan.push_back(okResp(201, R"({})"));
    rec.plan.push_back(okResp(409, R"({"message":"sha does not match"})"));   // 探针写入失败
    rec.plan.push_back(okResp(204, ""));                                       // 清理仍执行

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto report = api.testConnection();
    DOCTEST_CHECK_FALSE(report.ok);
    DOCTEST_CHECK(report.summary.find("写入探针失败") != std::string::npos);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(6));
    DOCTEST_CHECK_EQ(rec.reqs[5].method, std::string("DELETE"));
    return true;
})

REGISTER_TEST(github_create_delete_ref, "github: createRef 取基点 HEAD 后建分支，deleteRef 删分支", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha"}})"));   // GET 基点 HEAD
    rec.plan.push_back(okResp(201, R"({})"));                             // POST 建分支
    rec.plan.push_back(okResp(204, ""));                                  // DELETE 删分支

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto created = api.createRef("devdesk-test-1", "main");
    DOCTEST_CHECK(created.isOk());
    DOCTEST_CHECK_EQ(rec.reqs[0].method, std::string("GET"));
    DOCTEST_CHECK(rec.reqs[0].url.find("/git/ref/heads/main") != std::string::npos);
    DOCTEST_CHECK_EQ(rec.reqs[1].method, std::string("POST"));
    DOCTEST_CHECK(rec.reqs[1].body.find("refs/heads/devdesk-test-1") != std::string::npos);
    DOCTEST_CHECK(rec.reqs[1].body.find("basesha") != std::string::npos);

    const auto removed = api.deleteRef("feature/x");
    DOCTEST_CHECK(removed.isOk());
    DOCTEST_CHECK(rec.reqs[2].url.find("/git/refs/heads/feature%2Fx") != std::string::npos);
    return true;
})

REGISTER_TEST(github_create_ref_head_404_fails_without_post, "github: 基点 HEAD 404 → 不发建分支请求", {
    Recorder rec;
    rec.plan.push_back(okResp(404, R"({"message":"Not Found"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto created = api.createRef("devdesk-test-1", "no-such-base");
    DOCTEST_CHECK(created.isFail());
    DOCTEST_CHECK_EQ(created.errorCode(), core::ErrorCode::NotFound);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));   // 只取 HEAD，不 POST
    return true;
})

REGISTER_TEST(github_local_error_no_request, "github: 无 token / 空 owner-repo 发送前失败，零请求零日志", {
    Recorder rec;

    std::vector<int> sleeps;
    SleepGuard guard(sleeps);

    platform::github::GithubApi noToken("", "owner", "repo", platform::github::RetryPolicy{1, {}});
    wire(noToken, rec);
    const auto a = noToken.getAuthenticatedUser();
    DOCTEST_CHECK(a.isFail());
    DOCTEST_CHECK_EQ(a.errorCode(), core::ErrorCode::InvalidApiKey);
    DOCTEST_CHECK(rec.reqs.empty());
    DOCTEST_CHECK(noToken.recentLog().empty());

    platform::github::GithubApi noRepo("tok", "", "", platform::github::RetryPolicy{1, {}});
    wire(noRepo, rec);
    const auto b = noRepo.putFile("f.txt", "c", "m");
    DOCTEST_CHECK(b.isFail());
    DOCTEST_CHECK_EQ(b.errorCode(), core::ErrorCode::ValidationError);
    DOCTEST_CHECK(rec.reqs.empty());
    return true;
})

REGISTER_TEST(github_url_encoding, "github: 中文/空格路径与含斜杠 ref 均转义", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"sha":"s1","encoding":"base64","content":"eA=="})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto f = api.getFile("notes/我的 笔记.md", "feature/x");
    DOCTEST_CHECK(f.isOk());
    DOCTEST_CHECK_EQ(f.value().content, std::string("x"));
    const std::string& url = rec.reqs[0].url;
    // 我=E68891 的=E79A84 空格=20 笔=E7AC94 记=E8AEB0；'/' 保留为路径分隔符
    DOCTEST_CHECK(url.find("/contents/notes/%E6%88%91%E7%9A%84%20%E7%AC%94%E8%AE%B0.md") !=
                  std::string::npos);
    DOCTEST_CHECK(url.find("?ref=feature%2Fx") != std::string::npos);
    return true;
})

// ---- ErrorCode 扩展 ----

REGISTER_TEST(github_errorcode_conflict_to_string, "github: ErrorCode::Conflict 可转字符串", {
    DOCTEST_CHECK_EQ(std::string(core::errorCodeToString(core::ErrorCode::Conflict)),
                     std::string("Conflict"));
    return true;
})

// ---- 审计补修：createRef 受理后响应丢失 / owner-repo 白名单 ----

REGISTER_TEST(github_create_ref_422_already_exists_is_success,
              "github: createRef POST 422 already exists 视为成功（响应丢失重试撞车）", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha"}})"));  // GET 基点 HEAD
    rec.plan.push_back(okResp(422, R"({"message":"Reference already exists"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {10, 20, 30}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto created = api.createRef("devdesk-test-1", "main");
    DOCTEST_CHECK(created.isOk());
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(2));  // 422 不重试
    DOCTEST_CHECK(sleeps.empty());
    return true;
})

REGISTER_TEST(github_create_ref_422_other_reason_still_fails,
              "github: createRef POST 422 非 already exists 仍失败", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha"}})"));
    rec.plan.push_back(okResp(422, R"({"message":"Invalid request.","errors":[]})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto created = api.createRef("devdesk-test-1", "main");
    DOCTEST_CHECK(created.isFail());
    DOCTEST_CHECK_EQ(created.errorCode(), core::ErrorCode::ValidationError);
    return true;
})

REGISTER_TEST(github_test_connection_create_fail_delete_fallback_no_residue,
              "github: 建临时分支失败 → 兜底删除；删到 404 = 无残留无提示", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));                       // ①
    rec.plan.push_back(okResp(                                                  // ②
        200, R"({"full_name":"owner/repo","default_branch":"main","permissions":{"push":true}})"));
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha","type":"commit"}})"));  // ③a
    rec.plan.push_back(okResp(500, R"({})"));  // ③b POST 三次全 500（分支可能已建成）
    rec.plan.push_back(okResp(500, R"({})"));
    rec.plan.push_back(okResp(500, R"({})"));
    rec.plan.push_back(okResp(404, R"({"message":"Not Found"})"));  // 兜底 DELETE：没建成 → 404

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {1, 2, 3}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto report = api.testConnection({});
    DOCTEST_CHECK(!report.ok);
    DOCTEST_CHECK(report.summary.find("创建临时分支失败") != std::string::npos);
    // 兜底删除真的发出了（POST×3 + DELETE×1）
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(7));
    DOCTEST_CHECK_EQ(rec.reqs[6].method, std::string("DELETE"));
    DOCTEST_CHECK(rec.reqs[6].url.find("/git/refs/heads/devdesk-test-") != std::string::npos);
    // 404 = 分支没建成，不算残留，summary 不带提示
    DOCTEST_CHECK(report.summary.find("残留") == std::string::npos);
    return true;
})

REGISTER_TEST(github_test_connection_create_fail_delete_fails_hints_residue,
              "github: 建临时分支失败且兜底删除也失败 → summary 提示残留", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));
    rec.plan.push_back(okResp(
        200, R"({"full_name":"owner/repo","default_branch":"main","permissions":{"push":true}})"));
    rec.plan.push_back(okResp(200, R"({"object":{"sha":"basesha","type":"commit"}})"));
    rec.plan.push_back(failNet("connection reset"));  // POST 三次全网络错（分支可能已建成）
    rec.plan.push_back(failNet("connection reset"));
    rec.plan.push_back(failNet("connection reset"));
    rec.plan.push_back(okResp(403, R"({})"));  // 兜底 DELETE 被拒（非 404）→ 真残留

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{3, {1, 2, 3}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto report = api.testConnection({});
    DOCTEST_CHECK(!report.ok);
    DOCTEST_CHECK(report.summary.find("创建临时分支失败") != std::string::npos);
    DOCTEST_CHECK(report.summary.find("残留") != std::string::npos);
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(7));
    return true;
})

REGISTER_TEST(github_validate_config_owner_repo_charset,
              "github: owner/repo 字符白名单——不合法 → ValidationError 零请求", {
    Recorder rec;
    std::vector<int> sleeps;
    SleepGuard guard(sleeps);

    // 断言宏是 `return false` 展开，不能进 auto 推导 lambda——所以 lambda 返回 bool、调用点再 CHECK
    const auto expectRejected = [&rec](const std::string& owner,
                                       const std::string& repo) -> bool {
        platform::github::GithubApi api("tok", owner, repo,
                                        platform::github::RetryPolicy{3, {1, 2, 3}});
        wire(api, rec);
        const auto r = api.getAuthenticatedUser();
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK_EQ(r.errorCode(), core::ErrorCode::ValidationError);
        return true;
    };
    DOCTEST_CHECK(expectRejected("own er", "repo"));        // 空格
    DOCTEST_CHECK(expectRejected("owner", "my repo"));      // repo 空格
    DOCTEST_CHECK(expectRejected("-lead", "repo"));         // 首连字符
    DOCTEST_CHECK(expectRejected("trail-", "repo"));        // 尾连字符
    DOCTEST_CHECK(expectRejected(std::string(40, 'a'), "repo"));  // owner 超 39 上限
    DOCTEST_CHECK(expectRejected("张三", "repo"));           // 非 ASCII
    DOCTEST_CHECK(expectRejected("owner", "repo:x"));       // repo 冒号
    DOCTEST_CHECK(rec.reqs.empty());         // 全部发送前拦截
    DOCTEST_CHECK(sleeps.empty());

    // 合法形态（连字符/点/下划线）放行：请求真的发出
    platform::github::GithubApi api("tok", "octo-cat", "my.repo_name-2",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    rec.plan.push_back(okResp(200, R"({"login":"me"})"));
    const auto good = api.getAuthenticatedUser();
    DOCTEST_CHECK(good.isOk());
    DOCTEST_CHECK_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
    return true;
})

// ---- listTree（F7 拉取全树）----

REGISTER_TEST(github_list_tree_parses_blobs,
              "github listTree: blob 条目解析（path/sha/size），目录条目排除", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({
        "sha": "rootsha",
        "truncated": false,
        "tree": [
          {"path":"notes","mode":"040000","type":"tree","sha":"t1"},
          {"path":"notes/a.md","mode":"100644","type":"blob","sha":"sa","size":3},
          {"path":"notes/b.md","mode":"100644","type":"blob","sha":"sb","size":0},
          {"path":"data/database.sql","mode":"100644","type":"blob","sha":"sc","size":123}
        ]
    })"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.listTree("main");
    DOCTEST_REQUIRE(res.isOk());
    DOCTEST_REQUIRE_EQ(res.value().size(), static_cast<std::size_t>(3));  // tree 目录排除
    // 顺序 = 响应树序（调用方自行筛选，不做路径重排）
    DOCTEST_CHECK_EQ(res.value()[0].path, std::string("notes/a.md"));
    DOCTEST_CHECK_EQ(res.value()[0].sha, std::string("sa"));
    DOCTEST_CHECK_EQ(res.value()[0].size, static_cast<uint64_t>(3));
    DOCTEST_CHECK_EQ(res.value()[2].path, std::string("data/database.sql"));
    DOCTEST_CHECK_EQ(res.value()[2].sha, std::string("sc"));
    DOCTEST_CHECK_EQ(res.value()[2].size, static_cast<uint64_t>(123));
    // 递归 + 分支进 URL，branch 含斜杠按值转义
    DOCTEST_REQUIRE_EQ(rec.reqs.size(), static_cast<std::size_t>(1));
    DOCTEST_CHECK(rec.reqs[0].url.find("/git/trees/main?recursive=1") !=
                  std::string::npos);
    return true;
})

REGISTER_TEST(github_list_tree_truncated_fails,
              "github listTree: 远端树被截断 → 明确失败（不做分页子树补取）", {
    Recorder rec;
    rec.plan.push_back(okResp(200, R"({"sha":"r","truncated":true,"tree":[{"path":"a.md","mode":"100644","type":"blob","sha":"s1","size":1}]})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.listTree("main");
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK(res.errorMessage().find("截断") != std::string::npos);
    return true;
})

REGISTER_TEST(github_list_tree_404_fails_not_found,
              "github listTree: 分支不存在 → NotFound（由调用方走分支创建路径）", {
    Recorder rec;
    rec.plan.push_back(okResp(404, R"({"message":"Not Found"})"));

    std::vector<int> sleeps;
    platform::github::GithubApi api("tok", "owner", "repo",
                                    platform::github::RetryPolicy{1, {}});
    wire(api, rec);
    SleepGuard guard(sleeps);

    const auto res = api.listTree("nope");
    DOCTEST_CHECK(res.isFail());
    DOCTEST_CHECK_EQ(res.errorCode(), core::ErrorCode::NotFound);
    return true;
})
