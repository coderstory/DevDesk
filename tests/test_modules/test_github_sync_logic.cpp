// tests/test_modules/test_github_sync_logic.cpp
// GitHub Sync 编排纯件单测：状态机 / 内容哈希 / 体积上限 / 变更集 / 备份名。
// 零模块实例化、零网络、零 DB（github_sync_logic.h 的边界承诺）。
#include "doctest.h"
#include "test_support.h"

#include "github_sync_logic.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
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

REGISTER_TEST(github_sync_diff_notes_by_state,
              "github_sync 推送变更集: InSync 不产出；New/LocalModified → Put；LocalDeleted → Delete", {
    using g::LocalNote;
    using g::SyncedNoteRecord;
    using FC = g::FileChange;
    const std::map<std::string, LocalNote> local{
        {"insync.md", {"A", g::contentHash("A")}},
        {"mod.md", {"A2", g::contentHash("A2")}},
        {"new.md", {"N", g::contentHash("N")}},
    };
    const std::map<std::string, SyncedNoteRecord> synced{
        // insync：登记 hash 与本地一致 → 不产出（每轮不再为未变笔记造远端 commit）
        {"insync.md", {"sha-1", g::contentHash("A")}},
        // mod：登记 hash 与本地不符 → Put（本地改过）
        {"mod.md", {"sha-2", g::contentHash("A-old")}},
        // gone：登记有而本地无 → Delete（删除传播）
        {"gone.md", {"sha-3", g::contentHash("G")}},
    };
    const auto changes = g::diffNotesByState(local, synced);
    DOCTEST_REQUIRE_EQ(changes.size(), static_cast<size_t>(3));
    // 顺序稳定：名序 Put 在前，名序 Delete 在后
    DOCTEST_CHECK(changes[0].kind == FC::Kind::Put);
    DOCTEST_CHECK_EQ(changes[0].path, std::string("notes/mod.md"));
    DOCTEST_CHECK_EQ(changes[0].content, std::string("A2"));
    DOCTEST_CHECK(changes[1].kind == FC::Kind::Put);
    DOCTEST_CHECK_EQ(changes[1].path, std::string("notes/new.md"));
    DOCTEST_CHECK(changes[2].kind == FC::Kind::Delete);
    DOCTEST_CHECK_EQ(changes[2].path, std::string("notes/gone.md"));

    // 全 InSync → 空变更集（T3 审计 finding 4 的核心判据：只推变更）
    const std::map<std::string, LocalNote> same{{"x.md", {"X", g::contentHash("X")}}};
    const std::map<std::string, SyncedNoteRecord> recX{{"x.md", {"s", g::contentHash("X")}}};
    DOCTEST_CHECK(g::diffNotesByState(same, recX).empty());
    // 登记缺 hash（T3 时代旧记录）→ 视为本地改动 → Put（宁可多推一次，不漏推）
    const std::map<std::string, SyncedNoteRecord> noHash{{"x.md", {"s", std::string()}}};
    const auto legacy = g::diffNotesByState(same, noHash);
    DOCTEST_REQUIRE_EQ(legacy.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(legacy[0].kind == FC::Kind::Put);
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
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::New) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::RemoteNew) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::RemoteModified) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::Diverged) ==
                  PullDecision::Download);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::InSync) ==
                  PullDecision::Skip);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::LocalModified) ==
                  PullDecision::Skip);
    DOCTEST_CHECK(g::pullDecisionForState(NoteSyncState::LocalDeleted) ==
                  PullDecision::Skip);
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

    // 新设备模拟（审计 C1）：空基线 + 有远端 → 全部下载（不会误判「未变」）
    const auto fresh = g::pullNoteActions({{"n.md", "s"}}, {}, {});
    DOCTEST_REQUIRE_EQ(fresh.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(fresh[0].kind == Kind::Download);

    // 本地删除（LocalDeleted）→ skip（待 push 传播；远端也变过时走 push 冲突）
    const std::map<std::string, LocalNote> localDel{};   // b 本地已删
    const std::map<std::string, SyncedNoteRecord> syncedDel{
        {"b.md", {"sb-old", g::contentHash("old")}}};
    const auto delActions =
        g::pullNoteActions({{"b.md", "sb-new"}}, localDel, syncedDel);
    DOCTEST_CHECK(delActions.empty());

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
