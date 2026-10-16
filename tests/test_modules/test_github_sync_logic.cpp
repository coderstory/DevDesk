// tests/test_modules/test_github_sync_logic.cpp
// GitHub Sync 编排纯件单测：状态机 / 内容哈希 / 体积上限 / 变更集 / 备份名。
// 零模块实例化、零网络、零 DB（github_sync_logic.h 的边界承诺）。
#include "doctest.h"

#include "github_sync_logic.h"

#include <cstdint>
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

REGISTER_TEST(github_sync_pull_note_actions,
              "github_sync 拉取动作: 远端 sha 比登记 → 下载；登记有远端无 → 本地保留", {
    using Kind = g::PullAction::Kind;
    const std::map<std::string, std::string> remote{{"a.md", "sa"},
                                                    {"b.md", "sb-new"}};
    const std::map<std::string, std::string> synced{{"b.md", "sb-old"},
                                                    {"c.md", "sc"}};
    const auto actions = g::pullNoteActions(remote, synced);
    // a：登记无 → 下载；b：远端 sha 变 → 下载；c：登记有远端无 → 保留本地
    DOCTEST_REQUIRE_EQ(actions.size(), static_cast<size_t>(3));
    DOCTEST_CHECK(actions[0].kind == Kind::Download);
    DOCTEST_CHECK_EQ(actions[0].fileName, std::string("a.md"));
    DOCTEST_CHECK(actions[1].kind == Kind::Download);
    DOCTEST_CHECK_EQ(actions[1].fileName, std::string("b.md"));
    DOCTEST_CHECK(actions[2].kind == Kind::KeepLocal);
    DOCTEST_CHECK_EQ(actions[2].fileName, std::string("c.md"));

    // 远端 sha 与登记一致 → 不下载（这是「每轮不再全量拉」的判据）
    const auto quiet = g::pullNoteActions({{"b.md", "sb-old"}}, synced);
    DOCTEST_REQUIRE_EQ(quiet.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(quiet[0].kind == Kind::KeepLocal);   // c 的路径；b 无动作

    // 双空 → 空动作集；顺序稳定（下载按名序在前，保留按名序在后）
    DOCTEST_CHECK(g::pullNoteActions({}, {}).empty());
    const std::map<std::string, std::string> r2{{"z.md", "s"}, {"a.md", "s"}};
    const std::map<std::string, std::string> s2{{"m.md", "s"}, {"b.md", "s"}};
    const auto ordered = g::pullNoteActions(r2, s2);
    DOCTEST_REQUIRE_EQ(ordered.size(), static_cast<size_t>(4));
    DOCTEST_CHECK_EQ(ordered[0].fileName, std::string("a.md"));
    DOCTEST_CHECK_EQ(ordered[1].fileName, std::string("z.md"));
    DOCTEST_CHECK_EQ(ordered[2].fileName, std::string("b.md"));
    DOCTEST_CHECK_EQ(ordered[3].fileName, std::string("m.md"));
    return true;
})
