// tests/test_modules/test_github_sync_engine.cpp
// GithubSyncEngine 编排层单测（编排层零覆盖欠账的偿还，见
// docs/superpowers/plans/2026-10-16-sync-engine-extraction-plan.md ⑤）。
// 零 UI、零真线程（run() 同步阻塞）、零真网：假发送器（GithubApi::setSender
// 实例缝，实例随用例析构，静态默认发送器全程不碰）+ 临时库 + 假时钟 + 临时目录。
// 覆盖 T4 审计点过的：失败语义（c57bdff 的引擎输入面）、LocalDeleted 远端未变
// →Skip / 远端变→Download（56bafe7 死锁闭环）、RemoteModified 远端无文件→Put、
// branch 全链显式传递、409 冲突中止、删除安全栏、备份先行于覆盖、aborted 收束。
// 另加 >1MB 笔记的端到端往返（.scratch/sync-product-gaps/issues/03 ①）：推送侧
// 整篇 base64 进 Contents API PUT、拉取侧元数据 + Accept raw 二跳直取原文。
//
// 描述统一以 "gsync" 开头：--filter gsync 即本文件全量（与 github_sync 纯件/
// 卡片用例、settings 黑盒用例互不重叠）。
#include "doctest.h"
#include "test_support.h"

#include "github_sync_config.h"
#include "github_sync_engine.h"
#include "github_sync_logic.h"
#include "platform/github_api.h"
#include "platform/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace g = module::settings::gsync;
using module::settings::GithubSyncEngine;
using module::settings::GithubSyncStore;
using module::settings::SyncAction;
using module::settings::SyncEngineDeps;
using module::settings::SyncRunResult;

namespace {

using Res = core::Result<platform::http::Response>;

Res okResp(int status, std::string body = "") {
    platform::http::Response r;
    r.statusCode = status;
    r.body = std::move(body);
    return Res::ok(std::move(r));
}

// 测试内联 base64（getFile 响应的 content 字段；标准字母表 + padding，
// GithubApi::getFile 的 base64Decode 按标准表解码）。
std::string b64(const std::string& raw) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(raw[i]);
        const unsigned b1 = i + 1 < raw.size()
                                ? static_cast<unsigned char>(raw[i + 1])
                                : 0;
        const unsigned b2 = i + 2 < raw.size()
                                ? static_cast<unsigned char>(raw[i + 2])
                                : 0;
        out += kTable[b0 >> 2];
        out += kTable[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += i + 1 < raw.size() ? kTable[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
        out += i + 2 < raw.size() ? kTable[b2 & 0x3F] : '=';
    }
    return out;
}

struct Recorded {
    std::string method;
    std::string url;
    std::string body;
    // 请求头一并录下：>1MB 拉取的 raw 二跳只有 Accept 头能区分（URL 与元数据跳完全相同），
    // 不录头就只能靠响应体反推，那是在断言假发送器自己的路由字段（恒真）。
    std::map<std::string, std::string> headers;
};

// 大小写不敏感取头（GithubApi 写的是 "Accept"；不赌拼写大小写）。
std::string headerOf(const Recorded& r, const std::string& key) {
    for (const auto& [k, v] : r.headers) {
        if (k.size() != key.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < k.size(); ++i) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(k[i])));
            const char b = static_cast<char>(
                std::tolower(static_cast<unsigned char>(key[i])));
            if (a != b) { same = false; break; }
        }
        if (same) return v;
    }
    return std::string();
}

// 按 (method + url 子串) 路由的假发送器：命中即回放该路由队首响应（多条时
// 逐条弹出，最后一条常驻）；多个路由同时命中取 urlPart 最长者（"/git/ref/heads"
// 不被 "/repos/owner/repo" 抢走）。未命中回 404（getFile 404 → exists=false；
// listTree 404 → NotFound → push 走分支自动创建）。
struct FakeGithub {
    struct Route {
        std::string method;
        std::string urlPart;
        std::vector<Res> plan;
        bool endsWith = false;   // true = url 必须**恰好以** urlPart 结尾
    };
    std::vector<Recorded> reqs;
    std::vector<Route> routes;

    Res send(const platform::http::Request& r) {
        reqs.push_back({r.method, r.url, r.body, r.headers});
        Route* best = nullptr;
        for (auto& route : routes) {
            if (route.method != r.method) continue;
            if (route.endsWith) {
                if (!r.url.ends_with(route.urlPart)) continue;
            } else if (r.url.find(route.urlPart) == std::string::npos) {
                continue;
            }
            if (best == nullptr ||
                route.urlPart.size() > best->urlPart.size()) {
                best = &route;
            }
        }
        if (best == nullptr) return okResp(404, "{}");
        if (best->plan.empty()) return okResp(500, "empty route plan");
        if (best->plan.size() > 1) {
            Res res = std::move(best->plan.front());
            best->plan.erase(best->plan.begin());
            return res;
        }
        return best->plan.front();   // 最后一条常驻：拷贝回放（不搬空重放面）
    }

    int count(const std::string& method, const std::string& urlPart) const {
        int n = 0;
        for (const auto& req : reqs) {
            if (req.method == method &&
                req.url.find(urlPart) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }
};

std::string repoJson(bool isPrivate) {
    return std::string(
               "{\"full_name\":\"owner/repo\",\"default_branch\":\"main\","
               "\"private\":") +
           (isPrivate ? "true" : "false") +
           ",\"permissions\":{\"push\":true}}";
}

std::string treeJson(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out = "{\"sha\":\"t0\",\"truncated\":false,\"tree\":[";
    bool first = true;
    for (const auto& [path, sha] : entries) {
        if (!first) out += ",";
        first = false;
        out += "{\"path\":\"" + path + "\",\"sha\":\"" + sha +
               "\",\"type\":\"blob\",\"size\":3}";
    }
    return out + "]}";
}

std::string fileJson(const std::string& sha, const std::string& content) {
    return "{\"sha\":\"" + sha + "\",\"content\":\"" + b64(content) +
           "\",\"encoding\":\"base64\"}";
}

void writeNote(const std::filesystem::path& dir, const std::string& name,
               const std::string& content) {
    std::filesystem::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary) << content;
}

// 测试内联 base64 解码（b64() 的逆）：>1MB 用例要对**真实发出的请求体**做逐字节比对，
// 只断长度区分不了「整篇正确」与「中段错位 / 尾部丢 '=' 填充」。查表一次建好，
// 1.4MB 的输入走线性扫描（逐字符 find 在 Debug 下太慢）。
int b64Index(unsigned char c) {
    static const std::array<int, 256> kTable = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        const std::string a =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(a[static_cast<std::size_t>(i)])] = i;
        }
        return t;
    }();
    return kTable[c];
}

std::string b64Decode(const std::string& in) {
    std::string out;
    out.reserve(in.size() / 4 * 3);
    int buf = 0;
    int bits = 0;
    for (const char ch : in) {
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') continue;
        const int v = b64Index(static_cast<unsigned char>(ch));
        if (v < 0) return std::string();   // 非法字符 = 解码失败（调用方按空串断言不上即红）
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// >1MB 大笔记（sync-03 ① ）：首 / 中 / 尾三处不同标记的确定性内容。只断长度的话
//「尾部截断」与「中段错位」都能蒙混过关（长度相同 ≠ 字节相同），故三处都要比。
constexpr char kHugeHead[] = "HEAD<大笔记往返>";
constexpr char kHugeMid[] = "MID<大笔记往返>";
constexpr char kHugeTail[] = "TAIL<大笔记往返>";

std::string makeHugeNote(std::size_t bytes) {
    std::string out;
    out.reserve(bytes + 64);
    out += kHugeHead;
    while (out.size() < bytes) out += "0123456789abcdef";
    out.resize(bytes);   // 精确到 bytes：非 3 的倍数顺带覆盖 base64 尾部的 '=' 填充
    out.replace(bytes / 2, sizeof(kHugeMid) - 1, kHugeMid);
    out.replace(bytes - (sizeof(kHugeTail) - 1), sizeof(kHugeTail) - 1, kHugeTail);
    return out;
}

// 大笔记内容自检：长度 + 首/中/尾逐字节一致。刻意不用 `==` 断言整串——失败时 doctest 会把
// 两边 1.4MB 全打进输出；分段比对既能定位（头/中/尾 分别红），输出又只有一行。
// 返回 bool 是断言宏的形状要求（DOCTEST_CHECK* 内部 `return false`，不能用在 void 函数里）。
bool checkHugeNote(const std::string& got, std::size_t bytes) {
    DOCTEST_CHECK_EQ(got.size(), bytes);
    if (got.size() != bytes) return false;   // 长度不符时按位比对无意义
    DOCTEST_CHECK_EQ(got.compare(0, sizeof(kHugeHead) - 1, kHugeHead), 0);
    DOCTEST_CHECK_EQ(got.compare(bytes / 2, sizeof(kHugeMid) - 1, kHugeMid), 0);
    DOCTEST_CHECK_EQ(got.compare(bytes - (sizeof(kHugeTail) - 1),
                                 sizeof(kHugeTail) - 1, kHugeTail),
                     0);
    return true;
}

// 引擎夹具：临时库 + 临时目录 + 假时钟 + 假发送器；引擎零全局注册，
// 用例间零串扰（TestDb/TempDir 析构自清）。
struct EngineFixture {
    test_support::TempDir tmp{"gsengine"};
    test_support::TestDb tdb{"devdesk_test_gsyncengine_", "db"};
    GithubSyncStore store{&tdb.handle()};
    FakeGithub gh;
    std::vector<std::string> progress;
    int64_t clockNow = 1700000000;
    bool abortFlag = false;

    EngineFixture() {
        std::filesystem::create_directories(tmp.path / "dump");
        std::filesystem::create_directories(tmp.path / "notes");
    }

    std::filesystem::path notesDir() const { return tmp.path / "notes"; }
    std::filesystem::path appDataDir() const { return tmp.path / "appdata"; }

    SyncEngineDeps makeDeps() {
        SyncEngineDeps d;
        d.db = &tdb.handle();
        d.store = &store;
        d.nowSec = [this] { return clockNow; };
        d.progress = [this](const std::string& s) { progress.push_back(s); };
        d.aborted = [this] { return abortFlag; };
        d.notesDir = notesDir();
        d.appDataDir = appDataDir();
        d.sqlDumpFile = tmp.path / "dump" / "database.sql";
        d.dbFile = tdb.path;
        d.apiFactory = [this](const std::string& token,
                              const std::string& owner,
                              const std::string& repo) {
            // attempts=1：假发送器响应即终态，重试/退避路径不进本文件用例
            //（GithubApi 自身的重试分类已由 test_github_api.cpp 覆盖）。
            auto api = std::make_unique<platform::github::GithubApi>(
                token, owner, repo, platform::github::RetryPolicy{1, {}});
            api->setSender([this](const platform::http::Request& r) {
                return gh.send(r);
            });
            return api;
        };
        return d;
    }

    std::unique_ptr<platform::github::GithubApi> makeApi() {
        return makeDeps().apiFactory("tok", "owner", "repo");
    }

    GithubSyncEngine engine() { return GithubSyncEngine(makeDeps()); }

    void seedTree(std::vector<std::pair<std::string, std::string>> entries) {
        gh.routes.push_back({"GET", "/git/trees/trunk",
                             {okResp(200, treeJson(std::move(entries)))}});
    }

    // getRepo 路由：URL 恰好以 /repos/<owner>/<repo> 结尾（listTree 等深路径
    // 同含该前缀，必须锚定结尾才不会被抢占）。
    void seedRepo(bool isPrivate = true) {
        gh.routes.push_back({"GET", "/repos/owner/repo",
                             {okResp(200, repoJson(isPrivate))}, true});
    }
};

} // namespace

// ── push ──

REGISTER_TEST(gsync_engine_push_new_note_and_sql,
              "gsync 引擎: push 首推（New→Put 无 sha 创建 + sql 登记 + 阶段进度）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "a.md", "A\n");
    fx.seedTree({});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"sqlnewsha"}})")}});
    fx.gh.routes.push_back({"PUT", "/contents/notes/a.md",
                            {okResp(201, R"({"content":{"sha":"anewsha"}})")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_EQ(r.summary, std::string("已推送 database.sql 与 1 篇笔记"));
    DOCTEST_CHECK(!r.conflict);
    DOCTEST_CHECK(r.overwriteFiles.empty());
    DOCTEST_CHECK(r.deleteSkipHint.empty());
    DOCTEST_CHECK(!r.dbImported);
    DOCTEST_CHECK(r.branchHint.empty());
    // 新建：树里无现值、无登记 → 无 sha 创建；分支显式定向
    int sqlPuts = 0;
    for (const auto& req : fx.gh.reqs) {
        if (req.method != "PUT") continue;
        if (req.url.find("/contents/data/database.sql") != std::string::npos) {
            ++sqlPuts;
            DOCTEST_CHECK(req.body.find("\"sha\"") == std::string::npos);
            DOCTEST_CHECK(req.body.find("\"branch\":\"trunk\"") !=
                          std::string::npos);
        }
        if (req.url.find("/contents/notes/a.md") != std::string::npos) {
            DOCTEST_CHECK(req.body.find("\"sha\"") == std::string::npos);
            DOCTEST_CHECK(req.body.find("\"branch\":\"trunk\"") !=
                          std::string::npos);
        }
    }
    DOCTEST_CHECK_EQ(sqlPuts, 1);
    DOCTEST_CHECK_EQ(fx.gh.count("DELETE", "/contents/"), 0);
    // 登记落库：push 的可观测收尾
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_REQUIRE(records.value().count("a.md") == 1);
    DOCTEST_CHECK_EQ(records.value().at("a.md").sha, std::string("anewsha"));
    DOCTEST_CHECK_EQ(records.value().at("a.md").contentHash,
                     g::contentHash("A\n"));
    auto sqlSha = fx.store.syncedSqlSha();
    DOCTEST_REQUIRE(sqlSha.isOk());
    DOCTEST_CHECK_EQ(sqlSha.value(), std::string("sqlnewsha"));
    // 阶段进度面：四阶段文案全部发出
    DOCTEST_REQUIRE_EQ(fx.progress.size(), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(fx.progress[0], std::string("① 导出数据库…"));
    DOCTEST_CHECK_EQ(fx.progress[1], std::string("② 校验分支…"));
    DOCTEST_CHECK_EQ(fx.progress[2], std::string("④ 上传 database.sql…"));
    DOCTEST_CHECK_EQ(fx.progress[3], std::string("⑤ 同步笔记…"));
    return true;
})

REGISTER_TEST(gsync_engine_push_local_modified_uses_recorded_sha,
              "gsync 引擎: push 本地改动（LocalModified→Put shaIfMatch=登记 sha，真并发检测）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "a.md", "B\n");
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("a.md", "sha-old", g::contentHash("A\n"))
            .isOk());
    DOCTEST_REQUIRE(fx.store.recordSyncedSqlSha("sha-sql-old").isOk());
    fx.seedTree(
        {{"notes/a.md", "sha-old"}, {"data/database.sql", "sha-sql-old"}});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"sha-sql-new"}})")}});
    fx.gh.routes.push_back({"PUT", "/contents/notes/a.md",
                            {okResp(201, R"({"content":{"sha":"sha-new"}})")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    for (const auto& req : fx.gh.reqs) {
        if (req.method != "PUT") continue;
        if (req.url.find("/contents/notes/a.md") != std::string::npos) {
            // 登记在案 → PUT 带登记 sha：远端动过即 409（冲突检测语义）
            DOCTEST_CHECK(req.body.find("\"sha\":\"sha-old\"") !=
                          std::string::npos);
        }
        if (req.url.find("/contents/data/database.sql") != std::string::npos) {
            DOCTEST_CHECK(req.body.find("\"sha\":\"sha-sql-old\"") !=
                          std::string::npos);
        }
    }
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK_EQ(records.value().at("a.md").sha, std::string("sha-new"));
    DOCTEST_CHECK_EQ(records.value().at("a.md").contentHash,
                     g::contentHash("B\n"));
    auto sqlSha = fx.store.syncedSqlSha();
    DOCTEST_REQUIRE(sqlSha.isOk());
    DOCTEST_CHECK_EQ(sqlSha.value(), std::string("sha-sql-new"));
    return true;
})

REGISTER_TEST(gsync_engine_push_delete_propagation,
              "gsync 引擎: push 删除传播（LocalDeleted+远端未变→DELETE 登记sha + 清登记）", {
    EngineFixture fx;
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("gone.md", "gonesha", "0deadbeef").isOk());
    fx.seedTree({{"notes/gone.md", "gonesha"}});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"s2"}})")}});
    fx.gh.routes.push_back({"DELETE", "/contents/notes/gone.md",
                            {okResp(200, R"({"commit":{"sha":"d1"}})")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_EQ(r.summary,
                     std::string("已推送 database.sql 与 0 篇笔记（删除 1 篇）"));
    DOCTEST_CHECK(r.deleteSkipHint.empty());
    DOCTEST_CHECK_EQ(fx.gh.count("DELETE", "/contents/notes/gone.md"), 1);
    for (const auto& req : fx.gh.reqs) {
        if (req.method == "DELETE") {
            // 删除定向：树里现值作 shaIfMatch（判态已确认 == 登记 sha，C6）
            DOCTEST_CHECK(req.body.find("\"sha\":\"gonesha\"") !=
                          std::string::npos);
            DOCTEST_CHECK(req.body.find("\"branch\":\"trunk\"") !=
                          std::string::npos);
        }
    }
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK(records.value().empty());   // 清登记：下轮不再判 LocalDeleted
    return true;
})

REGISTER_TEST(gsync_engine_push_delete_safety_skip,
              "gsync 引擎: push 删除安全栏（本地仍在但读失败→不删远端+黄字提示，T4 A5）", {
    EngineFixture fx;
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("stuck.md", "s1", "0deadbeef").isOk());
    writeNote(fx.notesDir(), "stuck.md", "x");
    // 本地读失败：collectLocalNotes 拿不到内容，但文件仍在磁盘 → 安全栏生效
    std::filesystem::permissions(fx.notesDir() / "stuck.md",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::remove);
    fx.seedTree({{"notes/stuck.md", "s1"}});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"s2"}})")}});
    fx.gh.routes.push_back({"DELETE", "/contents/notes/stuck.md",
                            {okResp(200, "{}")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    // 权限恢复先行（不依赖断言走向）
    std::filesystem::permissions(fx.notesDir() / "stuck.md",
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);   // 跳过不是失败
    DOCTEST_CHECK(!r.deleteSkipHint.empty());
    DOCTEST_CHECK(r.deleteSkipHint.find("1 篇") != std::string::npos);
    DOCTEST_CHECK_EQ(fx.gh.count("DELETE", "/contents/"), 0);   // 远端不动
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK(records.value().count("stuck.md") == 1);   // 登记不清，下轮再判
    return true;
})

REGISTER_TEST(gsync_engine_push_conflict_diverged_aborts,
              "gsync 引擎: push 判态冲突（Diverged→中止整轮，零 PUT/DELETE + 覆盖清单）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "a.md", "B\n");   // 本地 hash ≠ 登记
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("a.md", "asha-old", g::contentHash("A\n"))
            .isOk());
    fx.seedTree({{"notes/a.md", "asha-new"}});   // 远端 sha ≠ 登记

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(!r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_MESSAGE(r.conflict, "summary=" + r.summary);
    DOCTEST_CHECK(r.summary.find("本地与远端都有改动（409 冲突）") !=
                  std::string::npos);
    // 覆盖清单 = database.sql + 本轮 **Put 集**（Diverged 走 Conflict 动作，
    // 不进清单）→ 单条 database.sql
    DOCTEST_REQUIRE_EQ(r.overwriteFiles.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(r.overwriteFiles[0],
                     std::string("data/database.sql（数据库）"));
    // 冲突在判态后、任何写入前中止：database.sql 也不 PUT
    DOCTEST_CHECK_EQ(fx.gh.count("PUT", "/contents/"), 0);
    DOCTEST_CHECK_EQ(fx.gh.count("DELETE", "/contents/"), 0);
    return true;
})

REGISTER_TEST(gsync_engine_push_conflict_sql_409,
              "gsync 引擎: push sql 409 竞态（PUT database.sql 撞 Conflict→冲突路径不推笔记）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "a.md", "A\n");   // InSync：本地、登记、远端全一致
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("a.md", "asha-1", g::contentHash("A\n"))
            .isOk());
    fx.seedTree({{"notes/a.md", "asha-1"}, {"data/database.sql", "sqltree"}});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(409, R"({"message":"conflict"})")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(!r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_MESSAGE(r.conflict, "summary=" + r.summary);
    DOCTEST_CHECK_EQ(r.summary,
                     std::string("远端 database.sql 已被改动（409 冲突）"));
    // 覆盖清单只剩 database.sql（putNames 空：InSync 无 Put 集）
    DOCTEST_REQUIRE_EQ(r.overwriteFiles.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(r.overwriteFiles[0],
                     std::string("data/database.sql（数据库）"));
    DOCTEST_CHECK_EQ(fx.gh.count("PUT", "/contents/notes/"), 0);
    return true;
})

REGISTER_TEST(gsync_engine_push_branch_missing_auto_create,
              "gsync 引擎: push 分支不存在→按默认分支自动创建（branchHint + createRef 显式定向）", {
    EngineFixture fx;
    fx.seedRepo();
    // 第一次 listTree 404（分支缺）→ createRef → 第二次 200（空树）
    fx.gh.routes.push_back({"GET", "/git/trees/trunk",
                            {okResp(404, R"({"message":"no commit")"),
                             okResp(200, treeJson({}))}});
    fx.gh.routes.push_back(
        {"GET", "/git/ref/heads/main",
         {okResp(200, R"({"object":{"sha":"basesha"}})")}});
    fx.gh.routes.push_back({"POST", "/git/refs", {okResp(201, "{}")}});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"s2"}})")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_EQ(r.branchHint, g::branchCreatedHint("trunk"));
    DOCTEST_CHECK_EQ(fx.gh.count("POST", "/git/refs"), 1);
    for (const auto& req : fx.gh.reqs) {
        if (req.method == "POST") {
            DOCTEST_CHECK(req.body.find("\"ref\":\"refs/heads/trunk\"") !=
                          std::string::npos);
            DOCTEST_CHECK(req.body.find("\"sha\":\"basesha\"") !=
                          std::string::npos);
        }
    }
    return true;
})

REGISTER_TEST(gsync_engine_branch_explicit_on_every_request,
              "gsync 引擎: branch 全链显式传递（push+pull 每条分支敏感请求都带配置分支）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "a.md", "A\n");
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("gone.md", "gonesha", "0deadbeef").isOk());
    fx.seedTree({{"notes/gone.md", "gonesha"}});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"s2"}})")}});
    fx.gh.routes.push_back({"PUT", "/contents/notes/a.md",
                            {okResp(201, R"({"content":{"sha":"s3"}})")}});
    fx.gh.routes.push_back({"DELETE", "/contents/notes/gone.md",
                            {okResp(200, "{}")}});
    // push 清登记后，pull 对「远端有、登记无」的 gone.md 判 New→Download
    fx.gh.routes.push_back({"GET", "/contents/notes/gone.md",
                            {okResp(200, fileJson("gonesha", "GONE\n"))}});

    auto api = fx.makeApi();
    fx.seedRepo();
    const SyncRunResult pushed = fx.engine().push(*api, "trunk");
    DOCTEST_CHECK_MESSAGE(pushed.ok, "push summary=" + pushed.summary);
    const SyncRunResult pulled = fx.engine().pull(*api, "trunk");
    DOCTEST_CHECK_MESSAGE(pulled.ok, "pull summary=" + pulled.summary);

    // 全链断言：分支敏感 GET（contents/trees）必带 ref= / trees/<branch>；
    // 全部 PUT/DELETE 必带 body.branch。任何一条漏带 = 读写落在仓库默认分支
    //（推拉分叉回归，ebf815c 语义）。getRepo 是仓库元数据，无分支参数，豁免。
    DOCTEST_REQUIRE(fx.gh.reqs.size() >= 6);
    for (const auto& req : fx.gh.reqs) {
        if (req.method == "GET") {
            if (req.url.find("/contents/") == std::string::npos &&
                req.url.find("/git/trees/") == std::string::npos) {
                continue;
            }
            const bool viaUrl =
                req.url.find("ref=trunk") != std::string::npos ||
                req.url.find("/git/trees/trunk?recursive=1") !=
                    std::string::npos;
            DOCTEST_CHECK_MESSAGE(viaUrl, "GET 缺 ref=trunk: " + req.url);
        } else {
            DOCTEST_CHECK_MESSAGE(
                req.body.find("\"branch\":\"trunk\"") != std::string::npos,
                req.method + " 缺 branch=trunk: " + req.body);
        }
    }
    return true;
})

// ── pull ──

REGISTER_TEST(gsync_engine_pull_imports_changed_sql,
              "gsync 引擎: pull 库变更→备份先行+导入+登记（dbImported=true）", {
    EngineFixture fx;
    DOCTEST_REQUIRE(fx.store.recordSyncedSqlSha("sql-old").isOk());
    fx.seedTree({{"data/database.sql", "sql-new"}});
    fx.gh.routes.push_back({"GET", "/contents/data/database.sql",
                            {okResp(200, fileJson("sql-new", "SELECT 1;\n"))}});

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK(r.dbImported);
    // 摘要必须同时点名**两个**备份：笔记目录名 + 「覆盖本地库前」落盘的那份库文件名。
    // 只报笔记备份会让用户以为库没备份 —— backups/ 里两种备份同目录混放，真出事时
    // 不知道该拿哪一份回滚（这正是本用例从「只报 notes-…」改判的口径）。
    DOCTEST_CHECK_EQ(
        r.summary,
        std::string("已导入 database.sql；下载 0 篇笔记（本地备份：库 "
                    "devdesk-1700000000.db、笔记 notes-1700000000）"));
    auto sqlSha = fx.store.syncedSqlSha();
    DOCTEST_REQUIRE(sqlSha.isOk());
    DOCTEST_CHECK_EQ(sqlSha.value(), std::string("sql-new"));
    // 库备份已落盘（假时钟定名 devdesk-1700000000.db）
    const auto backup = fx.appDataDir() / "backups" / "devdesk-1700000000.db";
    DOCTEST_CHECK(std::filesystem::exists(backup));
    // 阶段序：备份文案先于下载导入（备份先行于覆盖的可观测面）
    const auto itBk = std::find(fx.progress.begin(), fx.progress.end(),
                                std::string("② 备份本地数据…"));
    const auto itDl = std::find(fx.progress.begin(), fx.progress.end(),
                                std::string("③ 下载并导入数据库…"));
    DOCTEST_REQUIRE(itBk != fx.progress.end());
    DOCTEST_REQUIRE(itDl != fx.progress.end());
    DOCTEST_CHECK(itBk < itDl);
    return true;
})

REGISTER_TEST(gsync_engine_pull_skips_unchanged_sql,
              "gsync 引擎: pull 库未变→跳过导入（零下载请求、无备份、dbImported=false）", {
    EngineFixture fx;
    DOCTEST_REQUIRE(fx.store.recordSyncedSqlSha("sql-new").isOk());
    fx.seedTree({{"data/database.sql", "sql-new"}});

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK(!r.dbImported);
    DOCTEST_CHECK_EQ(r.summary,
                     std::string("database.sql 未变，未导入；下载 0 篇笔记"));
    DOCTEST_CHECK_EQ(fx.gh.count("GET", "/contents/"), 0);
    DOCTEST_CHECK(!std::filesystem::exists(fx.appDataDir() / "backups"));
    return true;
})

REGISTER_TEST(gsync_engine_pull_remote_note_changed_downloads,
              "gsync 引擎: pull 笔记远端变→下载覆盖+登记（含笔记目录备份）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "b.md", "OLD");
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("b.md", "b-old", g::contentHash("OLD"))
            .isOk());
    fx.seedTree({{"notes/b.md", "b-new"}});
    fx.gh.routes.push_back({"GET", "/contents/notes/b.md",
                            {okResp(200, fileJson("b-new", "REMOTE\n"))}});

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK(!r.dbImported);
    DOCTEST_CHECK(r.summary.find("下载 1 篇笔记") != std::string::npos);
    // 对照组（与 gsync_engine_pull_imports_changed_sql 互证）：只下笔记、没导库 →
    // 摘要只点名笔记备份，**不出现**库备份名 ——「库 …」那段只在真的备份过库时才有
    //（防「不分情形恒拼库名」的过度修复）。
    DOCTEST_CHECK(r.summary.find("notes-1700000000") != std::string::npos);
    DOCTEST_CHECK(r.summary.find("devdesk-") == std::string::npos);
    DOCTEST_CHECK_EQ(fx.tmp.read(fx.notesDir() / "b.md"),
                     std::string("REMOTE\n"));
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK_EQ(records.value().at("b.md").sha, std::string("b-new"));
    DOCTEST_CHECK_EQ(records.value().at("b.md").contentHash,
                     g::contentHash("REMOTE\n"));
    // 笔记目录备份：覆盖前的原内容留在 notes-<ts>/
    const auto backupDir = fx.appDataDir() / "backups" / "notes-1700000000";
    DOCTEST_CHECK(std::filesystem::exists(backupDir / "b.md"));
    DOCTEST_CHECK_EQ(fx.tmp.read(backupDir / "b.md"), std::string("OLD"));
    return true;
})

// ── 嵌套笔记（子目录）三处口径必须一致 ──
// 笔记目录允许子目录：markdown 索引侧本就**递归**扫描（markdown_service.cpp 的
// recursive_directory_iterator），所以 `notes/sub/a.md` 是用户真能拥有的合法笔记
//（手放进子目录 / 外部编辑器创建）。改判前 Sync 侧三处都是单层或不建父目录：
// ①收集（push）单层 → 子目录笔记对同步**完全隐身**，用户以为「同步成功」但它们
// 从未上传；②备份（拉取前「整目录备份」）单层 → 注释说的整目录是假的，子目录笔记
// 不在回滚点里；③落盘（pull）不建父目录 → 远端有嵌套笔记时整个拉取中止。
// 三条用例各盯一处，都是「用户可见的静默缺口」，不是风格问题。
REGISTER_TEST(gsync_engine_push_collects_nested_note,
              "gsync 引擎: push 递归收集子目录笔记（sub/a.md → 远端 notes/sub/a.md）", {
    EngineFixture fx;
    writeNote(fx.notesDir() / "sub", "a.md", "NESTED\n");
    fx.seedTree({});
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"sqlnewsha"}})")}});
    fx.gh.routes.push_back({"PUT", "/contents/notes/sub/a.md",
                            {okResp(201, R"({"content":{"sha":"nestedsha"}})")}});

    fx.seedRepo();
    const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_EQ(r.summary, std::string("已推送 database.sql 与 1 篇笔记"));
    DOCTEST_CHECK_EQ(fx.gh.count("PUT", "/contents/notes/sub/a.md"), 1);
    // 登记名是**相对**笔记目录的 '/'-分隔路径：与远端名口径一致（远端名 = 树路径
    // 去掉 `notes/` 前缀）。用 filename() 单层收集会漏掉这条，且 Windows 上反斜杠
    // 会漂进远端路径。
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK(records.value().count("sub/a.md") == 1u);
    return true;
})

REGISTER_TEST(gsync_engine_pull_writes_nested_note,
              "gsync 引擎: pull 远端子目录笔记可落盘（按需建父目录，不再整拉取中止）", {
    EngineFixture fx;
    fx.seedTree({{"notes/sub/a.md", "nested-new"}});
    fx.gh.routes.push_back({"GET", "/contents/notes/sub/a.md",
                            {okResp(200, fileJson("nested-new", "NESTED\n"))}});

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    // 改判前：写 notes/sub/a.md 时父目录不存在 → 整个 pull 以
    // 「写入笔记文件失败：sub/a.md」中止（一篇嵌套笔记能挡住整次拉取）。
    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK_EQ(fx.tmp.read(fx.notesDir() / "sub" / "a.md"),
                     std::string("NESTED\n"));
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK_EQ(records.value().at("sub/a.md").sha,
                     std::string("nested-new"));
    return true;
})

REGISTER_TEST(gsync_engine_notes_backup_includes_nested,
              "gsync 引擎: 拉取前笔记备份含子目录（notes/sub/a.md → backups/notes-<ts>/sub/a.md）", {
    EngineFixture fx;
    DOCTEST_REQUIRE(fx.store.recordSyncedNote("b.md", "b-old",
                                              g::contentHash("B\n"))
                        .isOk());
    writeNote(fx.notesDir(), "b.md", "B\n");
    writeNote(fx.notesDir() / "sub", "a.md", "NESTED-OLD\n");
    fx.seedTree({{"notes/b.md", "b-new"}});
    fx.gh.routes.push_back({"GET", "/contents/notes/b.md",
                            {okResp(200, fileJson("b-new", "REMOTE\n"))}});

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    const auto backupDir = fx.appDataDir() / "backups" / "notes-1700000000";
    DOCTEST_CHECK(std::filesystem::exists(backupDir / "b.md"));
    // 改判前：备份只拷顶层 → 子目录笔记不在回滚点里，而 pull 的删除传播
    //（一旦嵌套笔记也被登记）会真删它们。
    DOCTEST_CHECK_EQ(fx.tmp.read(backupDir / "sub" / "a.md"),
                     std::string("NESTED-OLD\n"));
    return true;
})

REGISTER_TEST(gsync_engine_pull_local_deleted_both_branches,
              "gsync 引擎: pull 本地删双分支（远端未变→Skip 待传播；远端变→Download 恢复，56bafe7）", {
    // 分支 A：远端未变（sha==登记）→ Skip：不下载、登记保留、本地保持已删
    {
        EngineFixture fx;
        DOCTEST_REQUIRE(
            fx.store.recordSyncedNote("c.md", "c-old", "0deadbeef").isOk());
        fx.seedTree({{"notes/c.md", "c-old"}});

        const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

        DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
        DOCTEST_CHECK(r.summary.find("下载 0 篇笔记") != std::string::npos);
        DOCTEST_CHECK_EQ(fx.gh.count("GET", "/contents/notes/c.md"), 0);
        auto records = fx.store.syncedNoteRecords();
        DOCTEST_REQUIRE(records.isOk());
        DOCTEST_CHECK(records.value().count("c.md") == 1);
        DOCTEST_CHECK(!std::filesystem::exists(fx.notesDir() / "c.md"));
    }
    // 分支 B：远端也变过（sha≠登记）→ Download 恢复本地 + 更新登记
    //（409 死锁闭环的断点：恢复后重判 InSync，push 不再撞 Conflict）
    {
        EngineFixture fx;
        DOCTEST_REQUIRE(
            fx.store.recordSyncedNote("c.md", "c-old", "0deadbeef").isOk());
        fx.seedTree({{"notes/c.md", "c-new"}});
        fx.gh.routes.push_back({"GET", "/contents/notes/c.md",
                                {okResp(200, fileJson("c-new", "RESTORED\n"))}});

        const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

        DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
        DOCTEST_CHECK(r.summary.find("下载 1 篇笔记") != std::string::npos);
        DOCTEST_CHECK(std::filesystem::exists(fx.notesDir() / "c.md"));
        auto records = fx.store.syncedNoteRecords();
        DOCTEST_REQUIRE(records.isOk());
        DOCTEST_CHECK_EQ(records.value().at("c.md").sha,
                         std::string("c-new"));
        // 恢复后状态机闭合：InSync / push 侧 Skip（不再 Conflict）
        const auto state =
            g::classifyNote(true, true,
                            records.value().at("c.md").contentHash ==
                                g::contentHash("RESTORED\n"),
                            true);
        DOCTEST_CHECK(state == g::NoteSyncState::InSync);
        DOCTEST_CHECK(g::pushActionForState(state, true, true) ==
                      g::PushAction::Skip);
    }
    return true;
})

REGISTER_TEST(gsync_engine_pull_remote_deleted_keeps_local,
              "gsync 引擎: pull 远端已删→KeepLocal+清登记（保守语义，本地文件不动）", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "d.md", "LOCAL-D\n");
    DOCTEST_REQUIRE(
        fx.store.recordSyncedNote("d.md", "d1", g::contentHash("LOCAL-D\n"))
            .isOk());
    fx.seedTree({});   // 远端树里没有 d.md，也没有 sql（不触发导入面）

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK(r.summary.find("远端已删 1 篇，本地保留") != std::string::npos);
    DOCTEST_CHECK(std::filesystem::exists(fx.notesDir() / "d.md"));   // 不删本地
    auto records = fx.store.syncedNoteRecords();
    DOCTEST_REQUIRE(records.isOk());
    DOCTEST_CHECK(records.value().empty());   // 清登记：防 push 失效 sha 死循环
    return true;
})

REGISTER_TEST(gsync_engine_pull_backup_failure_aborts,
              "gsync 引擎: pull 备份失败→中止（不导入不下载，绝不在无退路时覆盖）", {
    EngineFixture fx;
    // appDataDir 位置被一个常规文件占住 → 建备份目录必失败
    writeNote(fx.tmp.path, "appdata", "not a dir");
    writeNote(fx.notesDir(), "a.md", "x");
    DOCTEST_REQUIRE(fx.store.recordSyncedSqlSha("sql-old").isOk());
    fx.seedTree({{"data/database.sql", "sql-new"}});

    const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

    DOCTEST_CHECK(!r.ok);
    DOCTEST_CHECK(r.summary.find("笔记备份失败，已中止拉取：") == 0);
    DOCTEST_CHECK(!r.dbImported);
    // 中止点在备份：零下载、零导入，登记不动
    DOCTEST_CHECK_EQ(fx.gh.count("GET", "/contents/"), 0);
    auto sqlSha = fx.store.syncedSqlSha();
    DOCTEST_REQUIRE(sqlSha.isOk());
    DOCTEST_CHECK_EQ(sqlSha.value(), std::string("sql-old"));
    return true;
})

REGISTER_TEST(gsync_engine_pull_backup_prune_keeps_five,
              "gsync 引擎: pull 连续导入 7 轮→库备份滚动保留 5 份（假时钟定序）", {
    EngineFixture fx;
    for (int i = 1; i <= 7; ++i) {
        const std::string sha = "sql-" + std::to_string(i);
        DOCTEST_REQUIRE(fx.store.recordSyncedSqlSha("sql-old").isOk());
        fx.gh.routes.clear();
        fx.gh.reqs.clear();
        fx.seedTree({{"data/database.sql", sha}});
        fx.gh.routes.push_back({"GET", "/contents/data/database.sql",
                                {okResp(200, fileJson(sha, "SELECT 1;\n"))}});
        const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");
        DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
        fx.clockNow += 1;
    }
    int count = 0;
    bool oldestAlive = false;
    bool newestAlive = false;
    for (const auto& e :
         std::filesystem::directory_iterator(fx.appDataDir() / "backups")) {
        const auto name = e.path().filename().string();
        if (name.rfind("devdesk-", 0) == 0) {
            ++count;
            if (name == "devdesk-1700000000.db") oldestAlive = true;
            if (name == "devdesk-1700000006.db") newestAlive = true;
        }
    }
    DOCTEST_CHECK_EQ(count, 5);
    DOCTEST_CHECK(!oldestAlive);
    DOCTEST_CHECK(newestAlive);
    return true;
})

// ── firstsync ──

REGISTER_TEST(gsync_engine_firstsync_remote_has_data_pull_then_push,
              "gsync 引擎: firstsync 远端有库→三步校验过闸+先拉后推（publicRepo/dbImported 透传）", {
    EngineFixture fx;
    // 测试连接三步：GET /user → GET /repos → createRef+PUT 探针+DELETE
    fx.gh.routes.push_back(
        {"GET", "/user", {okResp(200, R"({"login":"me"})")}});
    fx.seedRepo(/*isPrivate=*/false);
    fx.gh.routes.push_back(
        {"GET", "/git/ref/heads/main",
         {okResp(200, R"({"object":{"sha":"basesha"}})")}});
    fx.gh.routes.push_back({"POST", "/git/refs", {okResp(201, "{}")}});
    fx.gh.routes.push_back({"PUT", "/contents/devdesk-probe.txt",
                            {okResp(201, R"({"content":{"sha":"p"}})")}});
    fx.gh.routes.push_back({"DELETE", "/git/refs/heads/devdesk-test-",
                            {okResp(200, "{}")}});
    // 首次方向探测 + pull：远端有 database.sql
    fx.gh.routes.push_back({"GET", "/contents/data/database.sql",
                            {okResp(200, fileJson("sql-remote", "SELECT 1;\n"))}});
    fx.seedTree({{"data/database.sql", "sql-remote"}});
    // pull 后的 push：PUT sql（树 sha 作冲突检测）
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"sql-put"}})")}});

    const SyncRunResult r = fx.engine().run(SyncAction::FirstSync,
                                            "https://github.com/owner/repo",
                                            "tok", "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK(r.publicRepo);   // private:false → 黄字提醒透传
    DOCTEST_CHECK(r.dbImported);
    DOCTEST_CHECK(r.summary.find("校验通过；已用远端数据覆盖本地并推送：") == 0);
    DOCTEST_CHECK_EQ(fx.gh.count("PUT", "/contents/data/database.sql"), 1);
    auto sqlSha = fx.store.syncedSqlSha();
    DOCTEST_REQUIRE(sqlSha.isOk());
    DOCTEST_CHECK_EQ(sqlSha.value(), std::string("sql-put"));
    return true;
})

REGISTER_TEST(gsync_engine_firstsync_remote_empty_pushes_full,
              "gsync 引擎: firstsync 远端无库（404→exists=false）→首次上传", {
    EngineFixture fx;
    writeNote(fx.notesDir(), "a.md", "A\n");
    fx.seedRepo();
    fx.seedTree({});   // 空树：无 sql → Push 分支；分支在 → 无 createRef
    fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                            {okResp(201, R"({"content":{"sha":"s2"}})")}});
    fx.gh.routes.push_back({"PUT", "/contents/notes/a.md",
                            {okResp(201, R"({"content":{"sha":"s3"}})")}});
    // data/database.sql 的 GET 落空 → 默认 404 → exists=false → missing

    const SyncRunResult r = fx.engine().firstSync(*fx.makeApi(), "trunk");

    DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
    DOCTEST_CHECK(r.summary.find("首次上传完成：已推送 database.sql 与 1 篇笔记") ==
                  0);
    DOCTEST_CHECK(!r.conflict);
    return true;
})

REGISTER_TEST(gsync_engine_firstsync_probe_failure_no_action,
              "gsync 引擎: firstsync 探测失败（非 NotFound）→Fail 不动作", {
    EngineFixture fx;
    fx.gh.routes.push_back(
        {"GET", "/contents/data/database.sql", {okResp(500, "boom")}});

    const SyncRunResult r = fx.engine().firstSync(*fx.makeApi(), "trunk");

    DOCTEST_CHECK(!r.ok);
    DOCTEST_CHECK(r.summary.find("探测远端失败：") == 0);
    DOCTEST_CHECK_EQ(fx.gh.count("PUT", "/contents/"), 0);
    DOCTEST_CHECK_EQ(fx.gh.count("DELETE", "/contents/"), 0);
    DOCTEST_CHECK_EQ(fx.gh.count("POST", "/git/refs"), 0);
    return true;
})

// ── 取消位 / 失败语义 ──

REGISTER_TEST(gsync_engine_abort_entry_short_circuit,
              "gsync 引擎: 取消位起跑即真→run 早退（默认失败结果，零请求零进度）", {
    EngineFixture fx;
    fx.abortFlag = true;
    const SyncRunResult r = fx.engine().run(
        SyncAction::Push, "https://github.com/owner/repo", "tok", "trunk");
    DOCTEST_CHECK(!r.ok);
    DOCTEST_CHECK(r.summary.empty());
    DOCTEST_CHECK(fx.gh.reqs.empty());
    DOCTEST_CHECK(fx.progress.empty());
    return true;
})

REGISTER_TEST(gsync_engine_abort_at_stage_boundary,
              "gsync 引擎: 进度回调内置位取消→dump 后的阶段边界收束（零 API）", {
    EngineFixture fx;
    fx.abortFlag = false;
    SyncEngineDeps deps = fx.makeDeps();
    deps.aborted = [&fx] {
        return fx.progress.size() >= 2;   // ② 文案出现即视为退出中
    };
    GithubSyncEngine eng(std::move(deps));

    const SyncRunResult r = eng.run(SyncAction::Push,
                                    "https://github.com/owner/repo", "tok",
                                    "trunk");

    DOCTEST_CHECK(!r.ok);
    DOCTEST_CHECK_EQ(r.summary, std::string("同步已中止（退出中）"));
    // 中止发生在 ② 校验分支边界（getRepo 之前）：树请求都没发出
    DOCTEST_CHECK_EQ(fx.gh.count("GET", "/git/trees/"), 0);
    DOCTEST_CHECK_EQ(fx.gh.count("PUT", "/contents/"), 0);
    DOCTEST_CHECK_EQ(fx.progress.size(), static_cast<size_t>(2));
    return true;
})

REGISTER_TEST(gsync_engine_failure_semantics_throttle_input,
              "gsync 引擎: 失败语义钉死（c57bdff 输入面：ok=false+summary 非空+日志记录；连跑两次同败）", {
    EngineFixture fx;
    fx.gh.routes.push_back(
        {"GET", "/git/trees/trunk", {okResp(500, "boom")}});

    // 失败节流（c57bdff）的判定输入 = 引擎失败结果：ok=false + 非空 summary。
    // harvest 据此记 lastCommitSec（dirty 不清）→ 5 分钟窗内自动重启被拦。
    // 引擎本身不带节流状态：同输入同输出，重复跑同败（节流归模块侧）。
    auto runOnce = [&fx]() {
        GithubSyncEngine eng(fx.makeDeps());
        return eng.run(SyncAction::Pull, "https://github.com/owner/repo",
                       "tok", "trunk");
    };
    const SyncRunResult r1 = runOnce();
    fx.gh.reqs.clear();
    const SyncRunResult r2 = runOnce();

    for (const SyncRunResult* r : {&r1, &r2}) {
        DOCTEST_CHECK(!r->ok);
        DOCTEST_CHECK(!r->summary.empty());
        DOCTEST_CHECK(r->summary.find("列举远端文件树失败：") == 0);
        // 弹窗日志源：失败请求进环形日志（UI 详情可审计）
        DOCTEST_CHECK(!r->logs.empty());
        bool saw500 = false;
        for (const auto& e : r->logs) {
            if (e.statusCode == 500) saw500 = true;
        }
        DOCTEST_CHECK(saw500);
    }
    return true;
})

// ── >1MB 笔记端到端往返（.scratch/sync-product-gaps/issues/03 ① ）──

REGISTER_TEST(
    gsync_engine_huge_note_roundtrip_push_then_pull,
    "gsync 引擎: >1MB 笔记端到端往返（push 整篇 base64 → 异设备 pull raw 直取，逐字节一致）", {
    // 两端各钉一处已合入的分叉：
    //   推送侧 —— 引擎本身没有 1MB 分叉、也没有单篇笔记体积闸：整篇 base64 进
    //             Contents API PUT 的 JSON 体（只有 database.sql 有 73MB 闸）；
    //   拉取侧 —— Contents API 对 1–100MB 文件不回 base64 正文（encoding="none"），
    //             API 层改用 Accept: application/vnd.github.raw 二跳直取原文，
    //             sha 仍取第一跳元数据。
    // 判据落在**真实请求**（假发送器录下的 method/url/headers/body）与**真实落盘内容**上；
    // 假发送器自己的路由字段（routes/plan）一概不断言（那是恒真）。
    // 阈值取十进制 1.4e6（> 1MiB = 1048576）：确保走的是 >1MB 口径，而非边界态。
    constexpr std::size_t kBytes = 1400 * 1000;
    const std::string note = makeHugeNote(kBytes);
    DOCTEST_REQUIRE(note.size() > 1024u * 1024u);

    // ── 设备 A：推送（本地有、远端空树 → New→Put）──
    std::string pushedBytes;   // 真实发上去的字节：设备 B 的「远端原文」就用它
    {
        EngineFixture fx;
        writeNote(fx.notesDir(), "big.md", note);
        fx.seedTree({});
        fx.gh.routes.push_back({"PUT", "/contents/data/database.sql",
                                {okResp(201, R"({"content":{"sha":"sqlsha"}})")}});
        fx.gh.routes.push_back({"PUT", "/contents/notes/big.md",
                                {okResp(201, R"({"content":{"sha":"bigsha"}})")}});
        fx.seedRepo();

        const SyncRunResult r = fx.engine().push(*fx.makeApi(), "trunk");

        DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
        DOCTEST_CHECK_EQ(r.summary, std::string("已推送 database.sql 与 1 篇笔记"));
        // 形态：恰好一条 PUT notes/big.md，体是 JSON（Content-Type + branch 显式）
        DOCTEST_REQUIRE_EQ(fx.gh.count("PUT", "/contents/notes/big.md"), 1);
        DOCTEST_REQUIRE_EQ(fx.gh.count("DELETE", "/contents/"), 0);
        std::string putBody;
        for (const auto& req : fx.gh.reqs) {
            if (req.method != "PUT") continue;
            if (req.url.find("/contents/notes/big.md") == std::string::npos) continue;
            putBody = req.body;
            DOCTEST_CHECK_EQ(headerOf(req, "Content-Type"),
                             std::string("application/json"));
            DOCTEST_CHECK(req.body.find("\"branch\":\"trunk\"") != std::string::npos);
        }
        DOCTEST_REQUIRE(!putBody.empty());
        const auto parsed = platform::json::parse(putBody);
        DOCTEST_REQUIRE(parsed.isOk());
        const platform::json::JsonValue content =
            parsed.value().asObject().get("content");
        DOCTEST_REQUIRE(content.isString());
        pushedBytes = b64Decode(content.asString());
        // 无截断：请求体里解出来的整篇与本地原文件逐字节一致（长度 + 首/中/尾三处）
        DOCTEST_CHECK_MESSAGE(checkHugeNote(pushedBytes, kBytes),
                              "推送请求体解码后 != 本地整篇");
        DOCTEST_CHECK_EQ(g::contentHash(pushedBytes), g::contentHash(note));
        // 引擎侧无「单篇笔记闸」的可观测面：1.4MB 全文进了请求体（base64 后更大）；
        // 真有 1MB 闸只会是「跳过该篇 / 整轮失败」，而这里 PUT 成功且体量 > 原文
        DOCTEST_CHECK(putBody.size() > note.size());
        // 登记面：sha 取 PUT 响应，hash 是**整篇**的（截断会让下轮判态误短路）
        auto records = fx.store.syncedNoteRecords();
        DOCTEST_REQUIRE(records.isOk());
        DOCTEST_REQUIRE(records.value().count("big.md") == 1);
        DOCTEST_CHECK_EQ(records.value().at("big.md").sha, std::string("bigsha"));
        DOCTEST_CHECK_EQ(records.value().at("big.md").contentHash,
                         g::contentHash(note));
    }

    // ── 设备 B：拉取（空笔记目录 + 空登记 = 新设备，远端有该篇 → Download）──
    {
        EngineFixture fx;
        fx.seedTree({{"notes/big.md", "bigsha"}});
        // 一跳（Accept: json）回元数据：encoding="none" + content 空（GitHub 对
        // 1–100MB 文件的响应形态）；二跳（Accept: raw）才回原文。路由 plan 两条：
        // 首条弹出、末条常驻，两条 GET 各得其所。
        fx.gh.routes.push_back(
            {"GET", "/contents/notes/big.md",
             {okResp(200,
                     R"({"sha":"bigsha","size":1400000,"encoding":"none","content":""})"),
              okResp(200, pushedBytes)}});

        const SyncRunResult r = fx.engine().pull(*fx.makeApi(), "trunk");

        DOCTEST_CHECK_MESSAGE(r.ok, "summary=" + r.summary);
        DOCTEST_CHECK(r.summary.find("下载 1 篇笔记") != std::string::npos);
        // 请求序即语义：先元数据（拿 sha）再 raw 直取（拿原文）；两跳都必须显式带分支
        //（URL 与元数据跳完全相同，区分只能靠 Accept 头——所以假发送器连头一起录）
        std::vector<Recorded> gets;
        for (const auto& req : fx.gh.reqs) {
            if (req.method == "GET" &&
                req.url.find("/contents/notes/big.md") != std::string::npos) {
                gets.push_back(req);
            }
        }
        DOCTEST_REQUIRE_EQ(gets.size(), static_cast<std::size_t>(2));
        DOCTEST_CHECK_EQ(headerOf(gets[0], "Accept"),
                         std::string("application/vnd.github+json"));
        DOCTEST_CHECK_EQ(headerOf(gets[1], "Accept"),
                         std::string("application/vnd.github.raw"));
        DOCTEST_CHECK(gets[0].url.find("ref=trunk") != std::string::npos);
        DOCTEST_CHECK(gets[1].url.find("ref=trunk") != std::string::npos);
        // 落盘：真实文件长度 + 逐字节 == 设备 A 发上去的那篇（往返闭合）
        const auto onDiskPath = fx.notesDir() / "big.md";
        std::error_code ec;
        DOCTEST_REQUIRE(std::filesystem::exists(onDiskPath, ec));
        DOCTEST_CHECK_EQ(
            static_cast<std::size_t>(std::filesystem::file_size(onDiskPath, ec)),
            kBytes);
        const std::string onDisk = fx.tmp.read(onDiskPath);
        DOCTEST_CHECK_MESSAGE(checkHugeNote(onDisk, kBytes),
                              "落盘内容 != 推送上去的整篇");
        DOCTEST_CHECK_EQ(g::contentHash(onDisk), g::contentHash(note));
        // 登记面：树 sha + 整篇 hash（下次 push 据此 InSync 短路）
        auto records = fx.store.syncedNoteRecords();
        DOCTEST_REQUIRE(records.isOk());
        DOCTEST_REQUIRE(records.value().count("big.md") == 1);
        DOCTEST_CHECK_EQ(records.value().at("big.md").sha, std::string("bigsha"));
        DOCTEST_CHECK_EQ(records.value().at("big.md").contentHash,
                         g::contentHash(note));
    }
    return true;
})
