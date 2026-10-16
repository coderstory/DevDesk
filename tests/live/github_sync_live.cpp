// tests/live/github_sync_live.cpp
// GitHub Sync **真机验收驱动**：逐场景驱动真同步引擎（SettingsModule 的同步路径）
// 打真 GitHub API，断言 + 打印结构化结果。
//
// 为什么不进 CMake / 不进默认门禁：真网络 + 真 token + 真远端仓库，CI 与离线
// 环境必红。编译与运行入口见 scripts/run-ghsync-live.sh（形态与
// tools/quota_live_probe.cpp 一致：手工链接已构建的静态库，不碰 configure 缓存）。
//
// 凭据纪律：token 只从 DEVDESK_GITHUB_TOKEN 环境变量读，只进 Authorization 头
//（由 platform::github::GithubApi 保证），本文件不打印、不落盘、不进命令行。
//
// **一进程一场景**：本地数据根由 scripts/run-ghsync-live.sh 每个场景换一个新的
//（DEVDESK_DATA_ROOT），否则场景间的库状态会互相污染（`all` 参数仅供人工快查，
// 它会带着上一场景的库往下跑）。
//
// 用法：github_sync_live <scenario>
#include "core/database.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/result.h"

#include "github_sync_config.h"
#include "github_sync_logic.h"
#include "github_sync_test_seam.h"
#include "local_git_probe.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestUpdate/openWindow 等内联实现：
                                      // settings/pet 模块引用这些符号，主 app TU 不在链接集

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>

// 无头驱动没有真窗口：给最小 DslAppConfig（探针同款，见 module_toggle_sidebar_probe.cpp）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}.title("github_sync_live");
    return config;
}
}  // namespace app

namespace {

namespace ms = module::settings;

constexpr const char* kRepoMain =
    "https://github.com/coderstory/devdesk-sync-test";
constexpr const char* kRepoDestroy =
    "https://github.com/coderstory/devdesk-sync-test-destroy";
constexpr const char* kRepoMissing =
    "https://github.com/coderstory/devdesk-sync-test-nonexistent-xyz";
constexpr const char* kRepoNoAccess = "https://github.com/octocat/Hello-World";

std::string gFixture;   // scripts/ghsync-live-fixture.sh 绝对路径

// ── 场景判定累加器 ──
struct Report {
    int checks = 0;
    int failed = 0;
    void check(bool cond, const std::string& what) {
        ++checks;
        std::printf("  %s %s\n", cond ? "✓" : "✗", what.c_str());
        if (!cond) ++failed;
    }
    void note(const std::string& s) { std::printf("  · %s\n", s.c_str()); }
    void evidence(const std::string& s) {
        std::printf("  · 证据: %s\n", s.c_str());
    }
};

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// popen 会把「脚本打印空行」也收进来；判存在性前先去掉首尾空白。
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

struct CmdResult {
    int code = -1;
    std::string out;
};

CmdResult runShell(const std::string& cmd) {
    CmdResult r;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) return r;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) r.out += buf;
    const int st = ::pclose(p);
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

// 调远端夹具脚本。token 不在命令行里——子进程继承本进程的 DEVDESK_GITHUB_TOKEN。
// branch 非空 → 经 DEVDESK_GH_BRANCH 覆盖脚本的目标分支（cat / get-sha 用）。
CmdResult fixture(const std::vector<std::string>& args,
                  const std::string& branch = "") {
    std::string cmd;
    if (!branch.empty()) cmd += "DEVDESK_GH_BRANCH=" + shellQuote(branch) + " ";
    cmd += "bash " + shellQuote(gFixture);
    for (const auto& a : args) cmd += " " + shellQuote(a);
    cmd += " 2>&1";
    return runShell(cmd);
}

// ── 本地状态操作 ──
std::filesystem::path notesDir() { return core::paths::markdownDir(); }

bool writeNote(const std::string& name, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(notesDir(), ec);
    std::ofstream out(notesDir() / name, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return out.good();
}

std::string readNote(const std::string& name) {
    std::ifstream in(notesDir() / name, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void execSql(const std::string& sql) {
    auto* db = core::appDatabase();
    if (db == nullptr) return;
    auto r = db->exec(sql);
    if (r.isFail()) std::printf("  ! SQL 失败: %s\n", r.errorMessage().c_str());
}

int64_t scalarInt(const std::string& sql) {
    auto* db = core::appDatabase();
    if (db == nullptr) return -1;
    auto stmt = db->prepare(sql);
    if (stmt.isFail()) return -1;
    auto& st = stmt.value();
    if (st.step().isFail()) return -1;
    auto row = st.currentRow();
    if (row.isFail() || row.value().values.empty()) return -1;
    if (auto* v = std::get_if<int64_t>(&row.value().values[0])) return *v;
    return -1;
}

// 标记行用 todo_columns（无外键、不在 defaultSkipTables 里 → 会随 dump 走）。
void insertMarker(const std::string& title) {
    execSql("INSERT INTO todo_columns(title, position, created_at) "
            "VALUES('" + title + "', 0, 0)");
}

int64_t markerCount(const std::string& title) {
    return scalarInt("SELECT COUNT(*) FROM todo_columns WHERE title = '" +
                     title + "'");
}

// 清掉 database.sql 的远端基线 → 模拟「这台机器从未同步过库」，让 pull 走导入分支。
void clearSqlBaseline() {
    execSql("DELETE FROM github_sync_state WHERE key='sha:data/database.sql'");
}

// ── 引擎装配 ──
void saveConfig(const std::string& repoUrl, const std::string& branch) {
    auto* db = core::appDatabase();
    ms::GithubSyncStore store(db);
    ms::GithubSyncConfig cfg;
    cfg.enabled = true;
    cfg.repoUrl = repoUrl;
    cfg.branch = branch;
    cfg.autoPush = true;
    auto r = store.save(cfg);
    if (r.isFail()) {
        std::printf("  ! 配置落库失败: %s\n", r.errorMessage().c_str());
    }
}

std::string remoteCat(const std::string& path, const std::string& branch) {
    CmdResult r = fixture({"cat", path}, branch);
    return r.code == 0 ? r.out : std::string();
}

ms::GithubSyncTestResult run(ms::GithubSyncTestAction a, bool manual,
                             int timeoutMs = 60000) {
    return ms::githubSyncRunForTest(a, manual, timeoutMs);
}

bool resetMain(Report& rep) {
    CmdResult r = fixture({"reset-main"});
    if (r.code != 0) {
        rep.check(false, "夹具 reset-main 失败: " + r.out);
        return false;
    }
    return true;
}

std::string writeTemp(const std::string& name, const std::string& content) {
    const std::string p = "/tmp/ghsync-live-" + name;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << content;
    return p;
}

// ═══════════════════════════ 场景 ═══════════════════════════

// 1 全新初始化：空仓 + 首次启用（FirstSync 方向判定）
void scenarioInit(Report& rep) {
    if (!resetMain(rep)) return;
    rep.check(writeNote("init1.md", "INIT-1\n"), "写本地笔记 init1.md");
    auto r = run(ms::GithubSyncTestAction::FirstSync, true);
    rep.check(r.ran, "动作跑完（未超时）");
    rep.check(r.ok, "FirstSync 成功: " + r.summary);
    rep.check(r.summary.find("首次上传完成") != std::string::npos,
              "方向判定 = Push（摘要含「首次上传完成」）");
    rep.check(remoteCat("notes/init1.md", "main") == "INIT-1\n",
              "远端 notes/init1.md 内容正确");
    CmdResult sha = fixture({"get-sha", "data/database.sql"});
    rep.check(sha.code == 0 && !trim(sha.out).empty(),
              "远端 data/database.sql 已创建");
    rep.evidence("summary=" + r.summary);
}

// 2 上传：本地笔记 + 库 → push 成功，远端出现 notes/*.md + data/database.sql
void scenarioUpload(Report& rep) {
    if (!resetMain(rep)) return;
    rep.check(writeNote("upload2.md", "UPLOAD-2\n"), "写本地笔记 upload2.md");
    insertMarker("MARKER-2");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran && r.ok, "Push 成功: " + r.summary);
    rep.check(remoteCat("notes/upload2.md", "main") == "UPLOAD-2\n",
              "远端 notes/upload2.md 内容正确");
    CmdResult db = fixture({"cat", "data/database.sql"});
    rep.check(db.code == 0 && db.out.find("MARKER-2") != std::string::npos,
              "远端 data/database.sql 含本地标记行 MARKER-2");
    rep.evidence("summary=" + r.summary);
}

// 3 下载同步：远端变更 → pull 正确落盘 + 本地库更新
void scenarioDownload(Report& rep) {
    if (!resetMain(rep)) return;
    insertMarker("MARKER-3");
    auto p1 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(p1.ran && p1.ok, "前置 Push 成功（带标记的库已推远端）");
    rep.check(markerCount("MARKER-3") == 1, "本地标记行存在（前置条件）");

    execSql("DELETE FROM todo_columns WHERE title='MARKER-3'");
    clearSqlBaseline();
    rep.check(markerCount("MARKER-3") == 0, "本地标记行已删（模拟另一台设备）");

    const std::string tmp = writeTemp("remote3.md", "REMOTE-3\n");
    rep.check(fixture({"set-note", "remote3.md", tmp}).code == 0,
              "夹具写入远端笔记 notes/remote3.md");

    auto r = run(ms::GithubSyncTestAction::Pull, true);
    rep.check(r.ran && r.ok, "Pull 成功: " + r.summary);
    rep.check(readNote("remote3.md") == "REMOTE-3\n",
              "远端笔记已落盘且内容正确");
    rep.check(r.dbImported, "本轮确实导入了远端 database.sql");
    rep.check(markerCount("MARKER-3") == 1, "本地库已更新（标记行回来了）");
    rep.evidence("summary=" + r.summary);
}

// 4 上传冲突：本地登记 sha 后远端被改 → 409 → 覆盖清单 + 冲突路径（不静默覆盖）
void scenarioPushConflict(Report& rep) {
    if (!resetMain(rep)) return;
    rep.check(writeNote("conflict4.md", "LOCAL-A\n"), "写本地笔记 conflict4.md");
    auto p1 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(p1.ran && p1.ok, "前置 Push 成功（登记基线 sha）");

    const std::string tmp = writeTemp("remote4.md", "REMOTE-B\n");
    rep.check(fixture({"set-note", "conflict4.md", tmp}).code == 0,
              "夹具改远端 notes/conflict4.md（另一台设备）");
    rep.check(writeNote("conflict4.md", "LOCAL-C\n"), "本地再改 conflict4.md");

    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran, "动作跑完");
    rep.check(!r.ok, "Push 被拒（未静默覆盖）");
    rep.check(r.conflict, "走了 409 冲突路径");
    rep.check(!r.overwriteFiles.empty(),
              "覆盖清单非空（" + std::to_string(r.overwriteFiles.size()) + " 项）");
    rep.check(remoteCat("notes/conflict4.md", "main") == "REMOTE-B\n",
              "远端内容未被本地覆盖");
    rep.evidence("summary=" + r.summary + " | error=" + r.error);
    std::string list;
    for (const auto& f : r.overwriteFiles) list += f + " ";
    rep.evidence("overwrite=" + list);
}

// 5 下载冲突：本地改 + 远端改 → pull 决策正确（按 spec 远端覆盖本地）
void scenarioPullConflict(Report& rep) {
    if (!resetMain(rep)) return;
    rep.check(writeNote("d5.md", "BASE\n"), "写本地笔记 d5.md");
    auto p1 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(p1.ran && p1.ok, "前置 Push 成功（登记基线）");

    const std::string tmp = writeTemp("remote5.md", "REMOTE-NEW\n");
    rep.check(fixture({"set-note", "d5.md", tmp}).code == 0, "夹具改远端 d5.md");
    rep.check(writeNote("d5.md", "LOCAL-NEW\n"), "本地也改 d5.md");

    auto r = run(ms::GithubSyncTestAction::Pull, true);
    rep.check(r.ran && r.ok, "Pull 成功: " + r.summary);
    rep.check(readNote("d5.md") == "REMOTE-NEW\n",
              "Diverged → 远端内容覆盖本地（spec 语义）");
    rep.check(remoteCat("notes/d5.md", "main") == "REMOTE-NEW\n",
              "pull 不写远端（远端保持 REMOTE-NEW）");

    auto r2 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r2.ran && r2.ok && !r2.conflict,
              "拉取后再推通过（无死锁）: " + r2.summary);
    rep.evidence("pull=" + r.summary + " | push=" + r2.summary);
}

// 6 分支不存在：目标分支在远端不存在 → 建分支后继续
void scenarioBranchMissing(Report& rep) {
    if (!resetMain(rep)) return;
    fixture({"del-branch", "feature-missing"});
    rep.check(writeNote("b6.md", "B6\n"), "写本地笔记 b6.md");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran && r.ok, "Push 成功: " + r.summary);
    rep.check(!r.branchHint.empty(), "给出了建分支提示: " + r.branchHint);
    rep.check(remoteCat("notes/b6.md", "feature-missing") == "B6\n",
              "新分支上出现 notes/b6.md");
    rep.evidence("summary=" + r.summary);
    fixture({"del-branch", "feature-missing"});
}

// 7 分支存在：正常同步
void scenarioBranchExists(Report& rep) {
    if (!resetMain(rep)) return;
    CmdResult eb = fixture({"ensure-branch", "feature-exists"});
    rep.check(eb.code == 0, "夹具建分支 feature-exists");
    rep.check(writeNote("b7.md", "B7\n"), "写本地笔记 b7.md");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran && r.ok, "Push 成功: " + r.summary);
    rep.check(r.branchHint.empty(), "未误报「分支已创建」");
    rep.check(remoteCat("notes/b7.md", "feature-exists") == "B7\n",
              "笔记落在既有分支上");
    rep.evidence("summary=" + r.summary);
    fixture({"del-branch", "feature-exists"});
}

// 8 分支被意外删除：之后分支消失 → 可恢复（下次同步重建）
void scenarioBranchDeleted(Report& rep) {
    if (!resetMain(rep)) return;
    rep.check(writeNote("b8.md", "B8\n"), "写本地笔记 b8.md");
    auto r1 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r1.ran && r1.ok, "首次 Push 成功（分支已建）: " + r1.summary);

    rep.check(fixture({"del-branch", "feature-gone"}).code == 0,
              "夹具删掉远端分支 feature-gone");
    auto r2 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r2.ran && r2.ok, "分支消失后同步自愈（重建）: " + r2.summary);
    rep.check(!r2.branchHint.empty(), "提示分支被重建: " + r2.branchHint);
    rep.check(remoteCat("notes/b8.md", "feature-gone") == "B8\n",
              "重建后笔记在远端");
    rep.evidence("summary=" + r2.summary);
    fixture({"del-branch", "feature-gone"});
    rep.note("「同步中途」删分支未单独计时：与「之后删」同一 404→createRef 代码路径");
}

// 9 仓库不存在：owner/repo 不存在 → 404 → 文案正确
void scenarioRepoMissing(Report& rep) {
    rep.check(writeNote("r9.md", "R9\n"), "写本地笔记 r9.md");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran, "动作跑完（未挂死）");
    rep.check(!r.ok, "Push 失败（未假成功）");
    rep.check(!r.error.empty(), "卡片红字非空");
    rep.evidence("error=" + r.error);
}

// 10 仓库被删除：同步中途仓库消失 → 404 处理、不脏本地
void scenarioRepoDeleted(Report& rep) {
    CmdResult ed = fixture({"reset-destroy"});
    rep.check(ed.code == 0, "夹具准备一次性仓: " + ed.out);
    rep.check(writeNote("r10.md", "R10\n"), "写本地笔记 r10.md");
    insertMarker("MARKER-10");
    auto r1 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r1.ran && r1.ok, "首次 Push 成功: " + r1.summary);

    rep.check(fixture({"delete-destroy"}).code == 0, "夹具删除整个仓");
    auto r2 = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r2.ran, "仓库消失后动作跑完（未挂死）");
    rep.check(!r2.ok, "Push 失败（未假成功）");
    rep.check(!r2.error.empty(), "卡片红字非空");
    rep.check(readNote("r10.md") == "R10\n", "本地笔记未被破坏");
    rep.check(markerCount("MARKER-10") == 1, "本地库未被破坏");
    rep.evidence("error=" + r2.error);
    fixture({"ensure-destroy"});
}

// 11 账户无权限：无写权限的仓 → 403/404 → 文案正确
void scenarioNoPermission(Report& rep) {
    rep.check(writeNote("n11.md", "N11\n"), "写本地笔记 n11.md");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran, "动作跑完（未挂死）");
    rep.check(!r.ok, "Push 失败（未假成功）");
    rep.check(!r.error.empty(), "卡片红字非空");
    rep.evidence("error=" + r.error);
}

// 12 仓库损坏：远端 data/database.sql 是垃圾 → 导入失败 → 卡片红字 + 本地库无损
void scenarioCorruptDb(Report& rep) {
    if (!resetMain(rep)) return;
    insertMarker("MARKER-12");
    rep.check(markerCount("MARKER-12") == 1, "本地有标记行（前置条件）");

    const std::string garbage =
        writeTemp("garbage.sql", "THIS IS NOT SQL AT ALL;\n<<<garbage>>>\n");
    rep.check(fixture({"set-db", garbage}).code == 0, "夹具把垃圾内容推到远端库路径");
    clearSqlBaseline();

    auto r = run(ms::GithubSyncTestAction::Pull, true);
    rep.check(r.ran, "动作跑完");
    rep.check(!r.ok, "Pull 失败（未静默吞掉）");
    rep.check(!r.error.empty(), "卡片红字非空");
    rep.check(!r.dbImported, "未标记为「已导入」");
    rep.check(markerCount("MARKER-12") == 1, "本地库无损（标记行仍在）");
    rep.evidence("error=" + r.error);

    // 变体：合法 SQL 但把 sqlite_master 写坏（writable_schema）→ 打 integrity_check 判定分支
    const std::string badschema = writeTemp(
        "badschema.sql",
        "PRAGMA writable_schema=ON;\n"
        "UPDATE sqlite_master SET sql='THIS IS NOT VALID DDL' "
        "WHERE type='table' AND name='todo_columns';\n");
    rep.check(fixture({"set-db", badschema}).code == 0, "夹具推「坏 schema」库");
    clearSqlBaseline();
    auto r2 = run(ms::GithubSyncTestAction::Pull, true);
    rep.note("坏 schema 变体: ok=" + std::string(r2.ok ? "true" : "false") +
             " dbImported=" + std::string(r2.dbImported ? "true" : "false") +
             " error=" + r2.error);
    rep.check(markerCount("MARKER-12") == 1,
              "坏 schema 变体后本地库仍可读（标记行仍在）");
}

// 13 本地 git 可用 → 探测显示可用，同步不受影响
void scenarioGitAvailable(Report& rep) {
    if (!resetMain(rep)) return;
    ms::localGitProbeOverrideForTest() = -1;   // 走真探测
    const int probe = ms::githubSyncLocalGitProbeForTest();
    rep.check(probe == 1,
              "真探测判定 git 可用（probe=" + std::to_string(probe) + "）");
    rep.check(writeNote("g13.md", "G13\n"), "写本地笔记 g13.md");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran && r.ok, "git 可用时同步正常: " + r.summary);
}

// 14 本地 git 不可用（注入失败态）→ 灰字警告 + 同步仍正常（不阻断）
void scenarioGitUnavailable(Report& rep) {
    if (!resetMain(rep)) return;
    ms::localGitProbeOverrideForTest() = 0;    // 注入「不可用」
    const int probe = ms::githubSyncLocalGitProbeForTest();
    rep.check(probe == 0,
              "注入后探测判定 git 不可用（probe=" + std::to_string(probe) + "）");
    rep.check(writeNote("g14.md", "G14\n"), "写本地笔记 g14.md");
    auto r = run(ms::GithubSyncTestAction::Push, true);
    rep.check(r.ran && r.ok, "git 不可用不阻断同步: " + r.summary);
    rep.check(remoteCat("notes/g14.md", "main") == "G14\n", "笔记确实推到远端");
    ms::localGitProbeOverrideForTest() = -1;   // 复位
}

// 15 本地 git 版本历史（决策#15）——未实现
void scenarioGitHistory(Report& r) {
    r.note("N/A：功能未实现（无本地 git 版本历史），无可测对象");
}

struct Scenario {
    const char* id;
    const char* title;
    const char* repoUrl;   // 启动前写入配置（引擎只在 onStart 时读一次）
    const char* branch;
    void (*fn)(Report&);
};

const Scenario kScenarios[] = {
    {"init", "1 全新初始化（FirstSync 方向判定）", kRepoMain, "main", &scenarioInit},
    {"upload", "2 上传（笔记 + 库 → 远端）", kRepoMain, "main", &scenarioUpload},
    {"download", "3 下载同步（远端变更 → 落盘 + 库更新）", kRepoMain, "main",
     &scenarioDownload},
    {"push-conflict", "4 上传冲突（409 + 覆盖清单）", kRepoMain, "main",
     &scenarioPushConflict},
    {"pull-conflict", "5 下载冲突（本地改 + 远端改）", kRepoMain, "main",
     &scenarioPullConflict},
    {"branch-missing", "6 分支不存在（自动创建）", kRepoMain, "feature-missing",
     &scenarioBranchMissing},
    {"branch-exists", "7 分支存在（正常同步）", kRepoMain, "feature-exists",
     &scenarioBranchExists},
    {"branch-deleted", "8 分支被意外删除（可恢复）", kRepoMain, "feature-gone",
     &scenarioBranchDeleted},
    {"repo-missing", "9 仓库不存在（404 文案）", kRepoMissing, "main",
     &scenarioRepoMissing},
    {"repo-deleted", "10 仓库被删除（404 且不脏本地）", kRepoDestroy, "main",
     &scenarioRepoDeleted},
    {"no-permission", "11 账户无权限（403/404 文案）", kRepoNoAccess, "main",
     &scenarioNoPermission},
    {"corrupt-db", "12 仓库损坏（垃圾 database.sql）", kRepoMain, "main",
     &scenarioCorruptDb},
    {"git-available", "13 本地 git 可用", kRepoMain, "main", &scenarioGitAvailable},
    {"git-unavailable", "14 本地 git 不可用（不阻断同步）", kRepoMain, "main",
     &scenarioGitUnavailable},
    {"git-history", "15 本地 git 版本历史", kRepoMain, "main", &scenarioGitHistory},
};

const Scenario* findScenario(const std::string& id) {
    for (const auto& s : kScenarios) {
        if (id == s.id) return &s;
    }
    return nullptr;
}

bool setupEngine() {
    std::error_code ec;
    const auto dbFile = core::paths::sqliteDbFile();
    std::filesystem::create_directories(dbFile.parent_path(), ec);
    auto opened = core::Database::open(dbFile);
    if (opened.isFail()) {
        std::printf("❌ 打开本地库失败: %s\n", opened.errorMessage().c_str());
        return false;
    }
    static core::Database db = std::move(opened.value());
    // Database::open 只开连接不落 schema（schema 由 migrate() 单独应用）。
    auto mig = db.migrate();
    if (mig.isFail()) {
        std::printf("❌ 应用 schema 失败: %s\n", mig.errorMessage().c_str());
        return false;
    }
    core::registerAppDatabase(&db.db());
    std::filesystem::create_directories(core::paths::markdownDir(), ec);
    return true;
}

}  // namespace

extern "C" ::core::Module* create_settings_module();

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: %s <scenario|all>\n", argv[0]);
        return 2;
    }
    const char* fixtureEnv = std::getenv("DEVDESK_GH_FIXTURE");
    gFixture = fixtureEnv != nullptr ? fixtureEnv : "";
    if (gFixture.empty()) {
        std::printf(
            "❌ 需要 DEVDESK_GH_FIXTURE=<scripts/ghsync-live-fixture.sh 路径>\n");
        return 2;
    }
    if (!ms::githubSyncTestSeamAvailable()) {
        std::printf("❌ 测试门未开：需要 DEVDESK_GITHUB_TEST_MODE=1（防生产误用）\n");
        return 2;
    }
    if (std::getenv("DEVDESK_GITHUB_TOKEN") == nullptr) {
        std::printf("❌ 需要 DEVDESK_GITHUB_TOKEN\n");
        return 2;
    }
    if (std::getenv("DEVDESK_DATA_ROOT") == nullptr) {
        std::printf("❌ 需要 DEVDESK_DATA_ROOT（隔离本地数据，绝不碰真实数据根）\n");
        return 2;
    }

    const std::string want = argv[1];
    std::vector<const Scenario*> todo;
    if (want == "all") {
        for (const auto& s : kScenarios) todo.push_back(&s);
    } else if (const Scenario* s = findScenario(want); s != nullptr) {
        todo.push_back(s);
    } else {
        std::printf("❌ 未知场景: %s\n", want.c_str());
        return 2;
    }

    if (!setupEngine()) return 2;

    int failedScenarios = 0;
    for (const Scenario* s : todo) {
        std::printf("\n=== SCENARIO %s — %s ===\n", s->id, s->title);
        saveConfig(s->repoUrl, s->branch);   // 必须先于 onStart：引擎只在启动时读配置
        ::core::Module* mod = create_settings_module();
        mod->onStart();
        Report rep;
        s->fn(rep);
        mod->onStop();
        delete mod;
        const bool pass = rep.failed == 0;
        if (!pass) ++failedScenarios;
        std::printf("RESULT %s %s (%d/%d checks)\n", s->id,
                    pass ? "PASS" : "FAIL", rep.checks - rep.failed, rep.checks);
    }
    std::printf("\n==== 汇总：%d 场景，失败 %d ====\n",
                static_cast<int>(todo.size()), failedScenarios);
    return failedScenarios;
}
