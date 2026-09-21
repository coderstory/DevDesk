// tests/test_modules/test_home.cpp
// Home 模块测试：P12 仪表盘真实数据（HomeService）+ 调用日志清理
#include "doctest.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/time.h"
#include "module_home/home_service.h"

#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

namespace {

std::filesystem::path tempDbPath(const std::string& tag) {
    static int counter = 0;
    ++counter;
    auto name = "devdesk_test_homesvc_" + tag + "_" +
                std::to_string(counter) + ".db";
    return std::filesystem::temp_directory_path() / name;
}

// 一次性环境：开临时 db → migrate（默认列 todo_columns 1..4 已就位）
struct TestEnv {
    std::filesystem::path path;
    std::unique_ptr<core::Database> db;
    bool ready = false;

    explicit TestEnv(const std::string& tag) {
        path = tempDbPath(tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) return;
        if (r.unsafeValue().migrate().isFail()) return;
        db = std::make_unique<core::Database>(std::move(r.unsafeValue()));
        ready = true;
    }

    ~TestEnv() {
        db.reset();
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

REGISTER_TEST(home_pageid_first, "home: PageId Home is first", {
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::Home), 0);
    return true;
})

REGISTER_TEST(home_service_real_stats,
              "HomeService: 仪表盘统计聚合 + 纯函数",
              {
    TestEnv env("stats");
    DOCTEST_REQUIRE(env.ready);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t day = 86400;

    // 种子数据：markdown_index（a/b 今天，c 三天前）
    auto& sq = env.db->db();
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO markdown_index (path, title, size_bytes, modified_at, created_at) VALUES "
        "('a.md', '笔记A', 10, " + std::to_string(now - 60) + ", " + std::to_string(now - 3600) + "), "
        "('b.md', '笔记B', 20, " + std::to_string(now - 120) + ", " + std::to_string(now - 3600) + "), "
        "('c.md', '笔记C', 30, " + std::to_string(now - 3 * day) + ", " + std::to_string(now - 4 * day) + ")"
    ).isOk());

    // todo_tasks：未完成无截止 / 未完成已逾期 / 未完成未来截止 / 已完成
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO todo_tasks (column_id, title, priority, position, due_at, completed_at, created_at, updated_at) VALUES "
        "(1, 't1', 0, 0, NULL, NULL, " + std::to_string(now) + ", " + std::to_string(now) + "), "
        "(1, 't2', 0, 1, " + std::to_string(now - 3600) + ", NULL, " + std::to_string(now) + ", " + std::to_string(now) + "), "
        "(1, 't3', 0, 2, " + std::to_string(now + day) + ", NULL, " + std::to_string(now) + ", " + std::to_string(now) + "), "
        "(1, 't4', 0, 3, " + std::to_string(now - 7200) + ", " + std::to_string(now) + ", " + std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());

    // claude_call_logs：先建 profile（FK ON），2 条今天 + 1 条昨天
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, created_at, updated_at) VALUES "
        "('p1', 'http://x', 'sk-***', 'chk', 'acct', 'm', " +
        std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) VALUES "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 30) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 60) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - day) + ")"
    ).isOk());

    // getStats 聚合
    module::home::HomeService svc(sq);
    auto r = svc.getStats(5);
    DOCTEST_REQUIRE_MESSAGE(r.isOk(), "getStats 失败: " + r.errorMessage());
    const auto& s = r.unsafeValue();
    DOCTEST_CHECK_EQ(s.todayNotes, 2);        // a + b
    DOCTEST_CHECK_EQ(s.pendingTodos, 3);      // t1 t2 t3
    DOCTEST_CHECK_EQ(s.overdueTodos, 1);      // t2（t4 已完成不计）
    DOCTEST_CHECK_EQ(s.todayApiCalls, 2);
    DOCTEST_CHECK_EQ(s.recentNotes.size(), static_cast<size_t>(3));
    if (!s.recentNotes.empty()) {
        // mtime 倒序：a.md(now-60) 最新
        DOCTEST_CHECK_EQ(s.recentNotes[0].relativePath, std::string("a.md"));
        DOCTEST_CHECK_EQ(s.recentNotes[0].title, std::string("笔记A"));
    }

    // 纯函数：formatRelativeTime（锚定本地正午，避免跨零点抖动）
    const int64_t noon = module::home::startOfDayEpoch(
                             static_cast<std::time_t>(now)) + 12 * 3600;
    DOCTEST_CHECK_EQ(module::home::formatRelativeTime(noon - 30, noon),
                     std::string("刚刚"));
    DOCTEST_CHECK_EQ(module::home::formatRelativeTime(noon - 5 * 60, noon),
                     std::string("5 分钟前"));
    DOCTEST_CHECK_EQ(module::home::formatRelativeTime(noon - 2 * 3600, noon),
                     std::string("2 小时前"));
    DOCTEST_CHECK_EQ(module::home::formatRelativeTime(noon - 26 * 3600, noon),
                     std::string("昨天"));
    DOCTEST_CHECK_EQ(module::home::formatRelativeTime(noon - 3 * 86400, noon),
                     std::string("3 天前"));
    const std::string old = module::home::formatRelativeTime(
        noon - 10 * 86400, noon);
    DOCTEST_CHECK_EQ(old.size(), static_cast<size_t>(10));  // YYYY-MM-DD
    DOCTEST_CHECK_EQ(old[4], '-');
    DOCTEST_CHECK_EQ(old[7], '-');

    // 纯函数：startOfDayEpoch 是本地零点
    const int64_t todayStart = module::home::startOfDayEpoch(
        static_cast<std::time_t>(now));
    DOCTEST_CHECK(todayStart <= now);
    DOCTEST_CHECK(now - todayStart < 86400);
    std::tm midnight{};
    core::time::localTime(static_cast<std::time_t>(todayStart), &midnight);
    DOCTEST_CHECK_EQ(midnight.tm_hour, 0);
    DOCTEST_CHECK_EQ(midnight.tm_min, 0);
    DOCTEST_CHECK_EQ(midnight.tm_sec, 0);
    return true;
})

REGISTER_TEST(home_prune_call_logs,
              "Database::pruneCallLogs: 30 天滚动清理 + 幂等",
              {
    TestEnv env("prune");
    DOCTEST_REQUIRE(env.ready);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    auto& sq = env.db->db();
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, api_key_check, "
        "keychain_account, model, created_at, updated_at) VALUES "
        "('p1', 'http://x', 'sk-***', 'chk', 'acct', 'm', " +
        std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());
    // 40 天前 / 35 天前 / 5 天前 各一条
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) VALUES "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 40LL * 86400) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 35LL * 86400) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 5LL * 86400) + ")"
    ).isOk());

    // 第一次清理：删 2 条（40 天 / 35 天前）
    auto first = env.db->pruneCallLogs(30);
    DOCTEST_REQUIRE_MESSAGE(first.isOk(), "pruneCallLogs 失败: " + first.errorMessage());
    DOCTEST_CHECK_EQ(first.unsafeValue(), static_cast<int64_t>(2));

    // 只剩 5 天前那条
    auto prep = sq.prepare("SELECT COUNT(*) AS n FROM claude_call_logs");
    DOCTEST_REQUIRE(prep.isOk());
    DOCTEST_CHECK(prep.value().step().isOk());
    auto row = prep.value().currentRow();
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.unsafeValue().getInt64("n").unsafeValue(),
                     static_cast<int64_t>(1));

    // 幂等：再跑一遍删 0 条
    auto second = env.db->pruneCallLogs(30);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_EQ(second.unsafeValue(), static_cast<int64_t>(0));
    return true;
})
