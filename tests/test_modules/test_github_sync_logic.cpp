// tests/test_modules/test_github_sync_logic.cpp
// GitHub Sync 编排纯件单测：状态机 / 内容哈希 / 体积上限 / 变更集 / 备份名。
// 零模块实例化、零网络、零 DB（github_sync_logic.h 的边界承诺）。
#include "doctest.h"
#include "test_support.h"

#include "core/logging.h"
#include "github_sync_logic.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace g = module::settings::gsync;

REGISTER_TEST(github_sync_classify_note_all_states,
              "github_sync 状态机: 7 态布尔组合全表", {
    using g::NoteSyncState;
    using g::classifyNote;
    // 记录无 + 本地有 → New（本地领先；远端两参无记录可比，不影响判定）
    DOCTEST_CHECK(classifyNote(false, true, false, false) ==
                  NoteSyncState::New);
    DOCTEST_CHECK(classifyNote(false, true, true, true) == NoteSyncState::New);
    // 记录无 + 本地无 → RemoteNew（调用方枚举域保证该输入来自远端树）
    DOCTEST_CHECK(classifyNote(false, false, false, false) ==
                  NoteSyncState::RemoteNew);
    // 记录有 + 本地无 → 删除传播（远端参数无关）
    DOCTEST_CHECK(classifyNote(true, false, false, false) ==
                  NoteSyncState::LocalDeleted);
    DOCTEST_CHECK(classifyNote(true, false, false, true) ==
                  NoteSyncState::LocalDeleted);
    // 记录有 + 本地有：本地 hash × 远端 sha 四象限
    DOCTEST_CHECK(classifyNote(true, true, true, true) ==
                  NoteSyncState::InSync);
    DOCTEST_CHECK(classifyNote(true, true, true, false) ==
                  NoteSyncState::RemoteModified);
    DOCTEST_CHECK(classifyNote(true, true, false, true) ==
                  NoteSyncState::LocalModified);
    DOCTEST_CHECK(classifyNote(true, true, false, false) ==
                  NoteSyncState::Diverged);
    return true;
})

REGISTER_TEST(github_sync_content_hash_fnv1a,
              "github_sync 内容哈希: FNV-1a 64 标准向量 + 稳定性", {
    // FNV-1a 64bit 公开测试向量（draft-eastlake-fnv）
    DOCTEST_CHECK_EQ(g::contentHash(""), std::string("cbf29ce484222325"));
    DOCTEST_CHECK_EQ(g::contentHash("a"), std::string("af63dc4c8601ec8c"));
    DOCTEST_CHECK_EQ(g::contentHash("foobar"),
                     std::string("85944171f73967e8"));
    // 同输入恒同输出（跨重启/跨设备可复现）；异输入异输出；定长 16 位 hex
    DOCTEST_CHECK_EQ(g::contentHash("笔记内容-123"),
                     g::contentHash("笔记内容-123"));
    DOCTEST_CHECK(g::contentHash("x") != g::contentHash("y"));
    DOCTEST_CHECK_EQ(g::contentHash("abc").size(), static_cast<size_t>(16));
    return true;
})

// 变异判据：把 makeLocalNote 改回 LocalNote{std::move(content), contentHash(content)}
// 这条必红 —— 聚合初始化按序初始化成员，hash 会在 content 被掏空后才算，
// 登记进基线的就是 hash("")（= 空串基值），localHashMatches 恒真 → 本地改动被判
// InSync 静默不推（真机 push-conflict 场景抓到的就是它）。
REGISTER_TEST(github_sync_make_local_note_hash_before_move,
              "github_sync makeLocalNote: hash 在 content 被移动前算好", {
    g::LocalNote note = g::makeLocalNote("LOCAL-A\n");
    DOCTEST_CHECK_EQ(note.content, std::string("LOCAL-A\n"));
    DOCTEST_CHECK_EQ(note.hash, g::contentHash("LOCAL-A\n"));
    // 非空内容不得退化成空串基值（hash("") = cbf29ce484222325）
    DOCTEST_CHECK(note.hash != std::string("cbf29ce484222325"));
    // 空内容合法：hash 恰为基值
    DOCTEST_CHECK_EQ(g::makeLocalNote("").hash,
                     std::string("cbf29ce484222325"));
    return true;
})

REGISTER_TEST(github_sync_dump_size_limit,
              "github_sync 体积上限: F9 原始体积超限判定（边界 + 注入小上限）", {
    DOCTEST_CHECK_EQ(g::kMaxDumpRawBytes, static_cast<uint64_t>(73'000'000));
    // 注入小上限：恰等于上限不超（100MB 请求含 base64 膨胀余量），+1 即超
    DOCTEST_CHECK(!g::dumpTooLargeForRemote(100, 100));
    DOCTEST_CHECK(g::dumpTooLargeForRemote(101, 100));
    // 默认上限形态：73,000,000 之下不拦，之上拦
    DOCTEST_CHECK(!g::dumpTooLargeForRemote(g::kMaxDumpRawBytes));
    DOCTEST_CHECK(g::dumpTooLargeForRemote(g::kMaxDumpRawBytes + 1));
    return true;
})

REGISTER_TEST(github_sync_notes_backup_prune,
              "github_sync 逻辑: 笔记备份目录名 + 目录型 prune（与库备份前缀隔离、独立计数）", {
    DOCTEST_CHECK_EQ(g::notesBackupDirName(1700000000),
                     std::string("notes-1700000000"));
    test_support::TempDir d("gsnotesbk");
    for (int i = 0; i < 7; ++i) {
        const auto dir = d.path / g::notesBackupDirName(1700000000 + i);
        std::filesystem::create_directories(dir);
        std::ofstream o(dir / "a.md", std::ios::binary | std::ios::trunc);
        o << "x";
    }
    // 混入库备份（不同前缀，不得被 notes prune 波及）与无关文件
    std::ofstream(d.path / "devdesk-1700000000.db", std::ios::binary | std::ios::trunc)
        << "x";
    std::ofstream(d.path / "notes.md", std::ios::binary | std::ios::trunc) << "x";

    const int removed = g::pruneBackups(d.path, g::kNotesBackupPrefix,
                                        g::kBackupKeep, "");
    DOCTEST_CHECK_EQ(removed, 2);   // 7 - 5
    // 前缀隔离：库备份与无关文件健在
    DOCTEST_CHECK(std::filesystem::exists(d.path / "devdesk-1700000000.db"));
    DOCTEST_CHECK(std::filesystem::exists(d.path / "notes.md"));
    // 最旧两份目录被删，最新五份健在且内容仍在
    DOCTEST_CHECK(!std::filesystem::exists(d.path / g::notesBackupDirName(1700000000)));
    DOCTEST_CHECK(!std::filesystem::exists(d.path / g::notesBackupDirName(1700000001)));
    DOCTEST_CHECK(std::filesystem::is_directory(
        d.path / g::notesBackupDirName(1700000006)));
    DOCTEST_CHECK(std::filesystem::exists(
        d.path / g::notesBackupDirName(1700000006) / "a.md"));
    return true;
})

// 权限位可用的平台。Windows 是 ACL 模型，std::filesystem::permissions 造不出 POSIX
// 拒绝 → 失败档在那边跳过（判据本身仍由 macOS/Linux 侧钉住）。
bool permBitsApplicable() {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
}

// 备份滚动删除的**失败档**：remove_all 出错时返回 (uintmax_t)-1（非 0）——按「> 0 即
// 计数」会把失败计成成功、且错误分支成死代码（WARN 永不打印）。两档都钉：
// 全失败（一份都没删掉）与部分失败（删掉一份、下一份失败）；WARN 逐次打印，不是只打一次。
REGISTER_TEST(github_sync_backup_prune_failure_visible,
              "github_sync 备份滚动删除: 删失败不计数 + WARN 显形（全失败 / 部分失败两档）", {
    if (!permBitsApplicable()) {
        DOCTEST_SKIP("Windows 权限模型是 ACL，std::filesystem::permissions 造不出删除拒绝");
    }
    namespace fs = std::filesystem;
    test_support::TempDir logs("gsprunefail_log");
    const auto logPath = logs.file("prune.log");
    core::log::addFileOutput(logPath);   // 日志捕获（core::log 无关闭接口，沿用既有做法）
    auto countWarns = [&] {
        std::ifstream f(logPath, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string t = ss.str();
        size_t n = 0;
        for (size_t pos = t.find("滚动删除备份失败"); pos != std::string::npos;
             pos = t.find("滚动删除备份失败", pos + 1)) {
            ++n;
        }
        return n;
    };
    const auto readOnly = fs::perms::owner_read | fs::perms::owner_exec;
    const auto writable = fs::perms::owner_all;

    // 前提（判据本身的地基）：失败时返回 (uintmax_t)-1 且 ec 非空。平台若改了这个
    // 语义，本断言先红 → 提示生产代码的判据要重审，而不是让失败档悄悄失效。
    {
        test_support::TempDir p("gsprunefail_premise");
        const auto locked = p.path / "locked";
        fs::create_directories(locked);
        p.write(locked / "f.md", "x");
        fs::permissions(p.path, readOnly, fs::perm_options::replace);
        std::error_code ec;
        const auto n = fs::remove_all(locked, ec);
        DOCTEST_CHECK_EQ(n, static_cast<std::uintmax_t>(-1));
        DOCTEST_CHECK(static_cast<bool>(ec));
        fs::permissions(p.path, writable, fs::perm_options::replace);
    }
    DOCTEST_CHECK_EQ(countWarns(), 0);   // 前置：日志里还没有本判据的行

    // 档 A 部分失败：第一份删得掉；第二份内含 0500 子目录 → 删不掉（不计数）
    {
        test_support::TempDir d("gsprunefailA");
        auto mk = [&](int i, bool lockInner) {
            const auto dir = d.path / g::notesBackupDirName(1700000000 + i);
            fs::create_directories(dir);
            d.write(dir / "a.md", "x");
            if (lockInner) {
                const auto inner = dir / "locked";
                fs::create_directories(inner);
                d.write(inner / "b.md", "x");
                fs::permissions(inner, readOnly, fs::perm_options::replace);
            }
        };
        mk(0, false);
        mk(1, true);
        mk(2, false);
        mk(3, false);   // 留 2 份 → 待删 2 份

        const int removed = g::pruneBackups(d.path, g::kNotesBackupPrefix, 2, "");
        DOCTEST_CHECK_EQ(removed, 1);   // 只算真删掉的那一份
        DOCTEST_CHECK(!fs::exists(d.path / g::notesBackupDirName(1700000000)));
        DOCTEST_CHECK(fs::exists(d.path / g::notesBackupDirName(1700000001)));
        DOCTEST_CHECK_EQ(countWarns(), 1);
        fs::permissions(d.path / g::notesBackupDirName(1700000001) / "locked",
                        writable, fs::perm_options::replace);   // 让 TempDir 析构清得掉
    }

    // 档 B 全失败：父目录只读 → 待删的两份都删不掉（removed 必须为 0，且逐次 WARN）
    {
        test_support::TempDir d("gsprunefailB");
        for (int i = 0; i < 3; ++i) {
            const auto dir = d.path / g::notesBackupDirName(1700000000 + i);
            fs::create_directories(dir);
            d.write(dir / "a.md", "x");
        }
        fs::permissions(d.path, readOnly, fs::perm_options::replace);
        const int removed = g::pruneBackups(d.path, g::kNotesBackupPrefix, 1, "");
        DOCTEST_CHECK_EQ(removed, 0);   // 全失败：一份都不许计成成功
        fs::permissions(d.path, writable, fs::perm_options::replace);
        DOCTEST_CHECK(fs::exists(d.path / g::notesBackupDirName(1700000000)));
        DOCTEST_CHECK(fs::exists(d.path / g::notesBackupDirName(1700000001)));
        DOCTEST_CHECK_EQ(countWarns(), 3);   // 1（档 A）+ 2（档 B 两次失败各一条）
    }
    return true;
})

REGISTER_TEST(github_sync_exit_push_due,
              "github_sync 退出推送: dirty 且距上次 commit ≥60s 才推（边界 + 时钟回拨）", {
    DOCTEST_CHECK_EQ(g::kExitPushMinGapSec, static_cast<int64_t>(60));
    // 从未 commit → 到点；窗口内 → 不到点；恰 60s → 到点
    DOCTEST_CHECK(g::exitPushDue(0, 1'000'000));
    DOCTEST_CHECK(!g::exitPushDue(1'000'000, 1'000'000 + g::kExitPushMinGapSec - 1));
    DOCTEST_CHECK(g::exitPushDue(1'000'000, 1'000'000 + g::kExitPushMinGapSec));
    // 时钟回拨：不卡死
    DOCTEST_CHECK(g::exitPushDue(1'000'000, 999'990));
    return true;
})

REGISTER_TEST(github_sync_auto_push_due,
              "github_sync 自动推送闸: 启用 + 自动推送开关 + dirty + 节流窗口四者全真才放行", {
    // 全真 → 放行（从未 commit = 到点）
    DOCTEST_CHECK(g::autoPushDue(true, true, true, 0, 1'000'000));
    // 四条件任一为假 → 不放行（含关掉「自动推送」开关）
    DOCTEST_CHECK(!g::autoPushDue(false, true, true, 0, 1'000'000));
    DOCTEST_CHECK(!g::autoPushDue(true, false, true, 0, 1'000'000));
    DOCTEST_CHECK(!g::autoPushDue(true, true, false, 0, 1'000'000));
    DOCTEST_CHECK(!g::autoPushDue(true, true, true, 1'000'000,
                                  1'000'000 + g::kCommitThrottleSec - 1));
    DOCTEST_CHECK(g::autoPushDue(true, true, true, 1'000'000,
                                 1'000'000 + g::kCommitThrottleSec));
    return true;
})

REGISTER_TEST(github_sync_action_tables_from_state,
              "github_sync 动作表: classifyNote 7 态 → push/pull 动作（唯一语义源）", {
    using g::NoteSyncState;
    using g::PushAction;
    using g::PullDecision;
    // push：InSync/RemoteModified 不推；New/LocalModified → Put；Diverged → 冲突
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::InSync, true, true) ==
                  PushAction::Skip);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::RemoteModified, true, false) ==
                  PushAction::Skip);
    // RemoteModified 且远端根本没这文件（分支被删重建）→ 直接重传，不白等 pull
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::RemoteModified, false, false) ==
                  PushAction::Put);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::New, false, false) ==
                  PushAction::Put);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::New, true, false) ==
                  PushAction::Put);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::LocalModified, true, true) ==
                  PushAction::Put);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::Diverged, true, false) ==
                  PushAction::Conflict);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::Diverged, false, false) ==
                  PushAction::Conflict);
    // LocalDeleted：远端没动/已无 → 删除传播；远端被改过 → 冲突（C6）
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::LocalDeleted, true, true) ==
                  PushAction::Delete);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::LocalDeleted, false, false) ==
                  PushAction::Delete);
    DOCTEST_CHECK(g::pushActionForState(NoteSyncState::LocalDeleted, true, false) ==
                  PushAction::Conflict);
    // pull：远端变过或无基线 → 下载；远端没变的本地改动与本地删除 → 跳过
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::New, false) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::RemoteNew, false) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::RemoteModified, false) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::Diverged, false) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::InSync, false) ==
                  PullDecision::Skip);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::LocalModified, true) ==
                  PullDecision::Skip);
    // LocalDeleted：远端没动 → Skip（本地删除待 push 传播）；
    // 远端也变过 → Download（不让位就丢远端新内容，且 push 会永远 409 死锁）
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::LocalDeleted, true) ==
                  PullDecision::Skip);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::LocalDeleted, false) ==
                  PullDecision::Download);
    return true;
})

REGISTER_TEST(github_sync_pull_note_actions,
              "github_sync 拉取动作: 状态机驱动——远端变过/无基线才下载；登记有远端无 → 本地保留", {
    using Kind = g::PullAction::Kind;
    using g::LocalNote;
    using g::SyncedNoteRecord;
    const std::string hx = g::contentHash("x");
    // 本地：a 与登记一致（未变）；b 本地改过
    const std::map<std::string, LocalNote> local{{"a.md", {"x", hx}},
                                                 {"b.md", {"new", g::contentHash("new")}}};
    const std::map<std::string, SyncedNoteRecord> synced{
        {"a.md", {"sa", hx}},                    // InSync（远端未变）
        {"b.md", {"sb-old", g::contentHash("old")}},  // LocalModified（远端未变）
        {"c.md", {"sc", hx}},                    // 远端已删 → KeepLocal
    };
    // 远端树：a 未变；b 远端也变了（Diverged → 下载）；d 全新（RemoteNew → 下载）
    const std::map<std::string, std::string> remote{{"a.md", "sa"},
                                                    {"b.md", "sb-new"},
                                                    {"d.md", "sd"}};
    const auto actions = g::pullNoteActions(remote, local, synced);
    DOCTEST_REQUIRE_EQ(actions.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(actions[0].kind == Kind::Download);
    DOCTEST_CHECK_EQ(actions[0].fileName, std::string("b.md"));
    DOCTEST_CHECK(actions[1].kind == Kind::Download);
    DOCTEST_CHECK_EQ(actions[1].fileName, std::string("d.md"));
    DOCTEST_CHECK(actions[2].kind == Kind::KeepLocal);
    DOCTEST_CHECK_EQ(actions[2].fileName, std::string("c.md"));

    // 新设备模拟：空基线 + 有远端 → 全部下载（不会误判「未变」）
    const auto fresh = g::pullNoteActions({{"n.md", "s"}}, {}, {});
    DOCTEST_REQUIRE_EQ(fresh.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(fresh[0].kind == Kind::Download);

    // 本地删除（LocalDeleted）+ 远端未变 → skip（待 push 传播）
    const std::map<std::string, LocalNote> localDel{};
    const std::map<std::string, SyncedNoteRecord> syncedDel{
        {"b.md", {"sb-old", g::contentHash("old")}}};
    const auto delUnchanged =
        g::pullNoteActions({{"b.md", "sb-old"}}, localDel, syncedDel);
    DOCTEST_CHECK(delUnchanged.empty());

    // 死锁路径：本地已删 + 远端已改。pull 必须下载恢复——
    // 若 Skip，push 对 LocalDeleted+远端变 判 Conflict 并中止整轮 → 409 弹窗
    // 「拉取后再推」原地打转，所有推送被一篇笔记永久卡死。
    const auto delChanged =
        g::pullNoteActions({{"b.md", "sb-new"}}, localDel, syncedDel);
    DOCTEST_REQUIRE_EQ(delChanged.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(delChanged[0].kind == Kind::Download);
    DOCTEST_CHECK_EQ(delChanged[0].fileName, std::string("b.md"));

    // 下载恢复后的下一轮：本地内容 = 远端内容、登记已被下载路径更新为
    // (树 sha, 内容 hash) → 重判 InSync，push 不再 Conflict（闭环断开）
    const std::string remoteContent = "restored";
    const std::map<std::string, LocalNote> localAfter{
        {"b.md", {remoteContent, g::contentHash(remoteContent)}}};
    const std::map<std::string, SyncedNoteRecord> syncedAfter{
        {"b.md", {"sb-new", g::contentHash(remoteContent)}}};
    const auto stateAfter = g::classifyNote(
        /*hasRecord=*/true, /*localExists=*/true,
        /*localHashMatches=*/true, /*remoteShaMatches=*/true);
    DOCTEST_CHECK(stateAfter == g::NoteSyncState::InSync);
    DOCTEST_CHECK(g::pushActionForState(stateAfter, true, true) ==
                  g::PushAction::Skip);
    DOCTEST_CHECK(g::pullNoteActions({{"b.md", "sb-new"}}, localAfter,
                                     syncedAfter)
                      .empty());

    // 双空 → 空；顺序稳定（下载名序在前，保留名序在后）
    DOCTEST_CHECK(g::pullNoteActions({}, {}, {}).empty());
    const auto ordered = g::pullNoteActions(
        {{"z.md", "s"}, {"a.md", "s"}}, {},
        {{"m.md", {"s", ""}}, {"b.md", {"s", ""}}});
    DOCTEST_REQUIRE_EQ(ordered.size(), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(ordered[0].fileName, std::string("a.md"));
    DOCTEST_CHECK_EQ(ordered[1].fileName, std::string("z.md"));
    DOCTEST_CHECK_EQ(ordered[2].fileName, std::string("b.md"));
    DOCTEST_CHECK_EQ(ordered[3].fileName, std::string("m.md"));
    return true;
})
