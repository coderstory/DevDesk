// tests/test_modules/test_home.cpp
// Home 模块测试：P12 仪表盘真实数据（HomeService）+ 调用日志清理
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/overdue.h"
#include "core/proxy_stats.h"
#include "core/time.h"
#include "module_home/home_service.h"
// 跨模块逾期口径一致性用例（THS-04）需要同时问首页与 Todo 两条读取路径
#include "module_todo/todo_service.h"

#include <eui/dsl.h>

#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

// 临时 db 文件名前缀（共享工厂 test_support::tempDbPath / TestEnv 的第一实参）
constexpr const char* kDbPrefix = "devdesk_test_homesvc_";

} // namespace

REGISTER_TEST(home_pageid_first, "home: PageId Home is first", {
    DOCTEST_CHECK_EQ(static_cast<int>(core::PageId::Home), 0);
    return true;
})

REGISTER_TEST(home_service_real_stats,
              "HomeService: 仪表盘统计聚合 + 纯函数",
              {
    test_support::TestEnv env(kDbPrefix, "stats");
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
    const auto& s = r.value();
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
    test_support::TestEnv env(kDbPrefix, "prune");
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
    DOCTEST_CHECK_EQ(first.value(), static_cast<int64_t>(2));

    // 只剩 5 天前那条
    auto prep = sq.prepare("SELECT COUNT(*) AS n FROM claude_call_logs");
    DOCTEST_REQUIRE(prep.isOk());
    DOCTEST_CHECK(prep.value().step().isOk());
    auto row = prep.value().currentRow();
    DOCTEST_REQUIRE(row.isOk());
    DOCTEST_CHECK_EQ(row.value().getInt64("n").value(),
                     static_cast<int64_t>(1));

    // 幂等：再跑一遍删 0 条
    auto second = env.db->pruneCallLogs(30);
    DOCTEST_REQUIRE(second.isOk());
    DOCTEST_CHECK_EQ(second.value(), static_cast<int64_t>(0));
    return true;
})

// ==================== Token 速率卡片（设计 2026-09-22-token-rate-card） ====================

extern "C" ::core::Module* create_home_module();

REGISTER_TEST(home_token_format_functions,
              "Token 速率卡片: formatTokenCount / formatRateTps 纯函数", {
    using module::home::formatTokenCount;
    using module::home::formatRateTps;
    DOCTEST_CHECK_EQ(formatTokenCount(0), std::string("0"));
    DOCTEST_CHECK_EQ(formatTokenCount(47), std::string("47"));
    DOCTEST_CHECK_EQ(formatTokenCount(999), std::string("999"));
    DOCTEST_CHECK_EQ(formatTokenCount(12400), std::string("12.4k"));
    DOCTEST_CHECK_EQ(formatTokenCount(1000), std::string("1.0k"));
    DOCTEST_CHECK_EQ(formatTokenCount(1234567), std::string("1.2M"));
    DOCTEST_CHECK_EQ(formatRateTps(0.0), std::string("0"));
    DOCTEST_CHECK_EQ(formatRateTps(47.26), std::string("47.3"));
    DOCTEST_CHECK_EQ(formatRateTps(99.94), std::string("99.9"));
    DOCTEST_CHECK_EQ(formatRateTps(123.6), std::string("124"));
    return true;
})

REGISTER_TEST(home_token_rate_card_compose,
              "Token 速率卡片: 无头 compose 冒烟（5 卡 + 三态 + onTimer 元素）", {
    core::proxy_stats::resetForTest();

    // 空闲态：activeStreams=0 → 数字 0、badge 空闲文案
    {
        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Screen screen;
        screen.width = 1200.0f; // 最小窗口宽度（卡片 208px 仍可容纳）
        screen.height = 720.0f;
        eui::Ui ui;
        ui.begin("home_idle");
        mod->compose(ui, screen);
        ui.end();

        DOCTEST_CHECK(ui.find("home.stat.tokens") != nullptr);
        DOCTEST_CHECK(ui.find("home.stat.notes") != nullptr);
        DOCTEST_CHECK(ui.find("home.stat.todo") != nullptr);
        DOCTEST_CHECK(ui.find("home.stat.overdue") != nullptr);
        DOCTEST_CHECK(ui.find("home.stat.api") != nullptr);
        // 卡内 onTimer 驱动元素（0.5s 重绘，设计 §2.6）
        DOCTEST_CHECK(ui.find("home.stat.tokens.timer") != nullptr);
        // 未加载数据（onStart 未跑）→ 错误态 badge 是「加载失败」而非
        // 「尚未加载」（W7：错误态与加载中区分）
        const auto* notesBadge = ui.find("home.stat.notes.badge");
        DOCTEST_REQUIRE(notesBadge != nullptr);
        DOCTEST_CHECK(notesBadge->text.find("加载失败") != std::string::npos);
        const auto* number = ui.find("home.stat.tokens.number");
        DOCTEST_REQUIRE(number != nullptr);
        DOCTEST_CHECK_EQ(number->text, std::string("0"));
        const auto* badge = ui.find("home.stat.tokens.badge");
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK(badge->text.find("空闲") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("累计 0") != std::string::npos);
        mod->onStop();
    }

    // 流式中态：注入 usage → activeStreams=1 → badge 带并发 + 累计
    {
        const uint64_t id = core::proxy_stats::beginStream();
        core::proxy_stats::updateStream(id, 42);

        std::unique_ptr<::core::Module> mod(create_home_module());
        eui::Screen screen;
        screen.width = 1600.0f;
        screen.height = 1000.0f;
        eui::Ui ui;
        ui.begin("home_streaming");
        mod->compose(ui, screen);
        ui.end();

        const auto* badge = ui.find("home.stat.tokens.badge");
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK(badge->text.find("1 路并发") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("累计 42") != std::string::npos);
        mod->onStop();

        core::proxy_stats::endStream(id);
    }

    core::proxy_stats::resetForTest();
    return true;
})

// ==================== 跨模块逾期口径一致性（THS-04） ====================
// 「逾期」的谓词单源在 core/overdue.h：首页 getStats 走 COUNT 聚合、Todo 页
// listOverdue 走列表查询。两条 SQL 用同一份 kWhereSql，本用例拿同一批种子数据
// 同时问两边，把它们钉在同一个数字上——口径再分叉（改列名 / 改比较符 / 只改
// 其中一处）这里就红。
REGISTER_TEST(home_todo_overdue_consistency,
              "HomeService/TodoService: 逾期口径单源一致性",
              {
    // 谓词边界（纯函数，不依赖时钟）：无截止 / 已完成 / 未到期 / 截止当秒
    using core::overdue::isOverdue;
    DOCTEST_CHECK_FALSE(isOverdue(std::nullopt, std::nullopt, 100));
    DOCTEST_CHECK_FALSE(isOverdue(50, 80, 100));             // 已完成
    DOCTEST_CHECK_FALSE(isOverdue(200, std::nullopt, 100));  // 未到期
    DOCTEST_CHECK_FALSE(isOverdue(100, std::nullopt, 100));  // 截止当秒不算
    DOCTEST_CHECK(isOverdue(99, std::nullopt, 100));

    test_support::TestEnv env(kDbPrefix, "overdue_xmod");
    DOCTEST_REQUIRE(env.ready);

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t day = 86400;

    // 种子：覆盖「无截止 / 逾期未完成 / 逾期已完成 / 未到期」四种组合
    // （TIN-08 后 REGISTER_TEST 走 __VA_ARGS__，花括号初始化列表的顶层逗号
    //  不再被当宏实参分隔符，可直接在用例体内初始化）
    struct Seed {
        const char* title;
        std::optional<int64_t> dueAt;
        std::optional<int64_t> completedAt;
    };
    std::vector<Seed> seeds{
        Seed{"s1-no-due", std::nullopt, std::nullopt},
        Seed{"s2-past-open", now - 3600, std::nullopt},
        Seed{"s3-past-done", now - 7200, now - 60},
        Seed{"s4-future", now + day, std::nullopt},
        Seed{"s5-past-open2", now - 7 * day, std::nullopt},
    };

    std::string insert =
        "INSERT INTO todo_tasks (column_id, title, priority, position, due_at, "
        "completed_at, created_at, updated_at) VALUES ";
    int64_t expected = 0;
    for (size_t i = 0; i < seeds.size(); ++i) {
        if (i != 0) insert += ", ";
        insert += "(1, '" + std::string(seeds[i].title) + "', 0, " +
                  std::to_string(i) + ", ";
        insert += seeds[i].dueAt.has_value()
                      ? std::to_string(*seeds[i].dueAt)
                      : std::string("NULL");
        insert += ", ";
        insert += seeds[i].completedAt.has_value()
                      ? std::to_string(*seeds[i].completedAt)
                      : std::string("NULL");
        insert += ", " + std::to_string(now) + ", " + std::to_string(now) + ")";
        // 期望值由 core 谓词对同一批种子值算出（与两侧 SQL 无关的第三只眼）
        if (isOverdue(seeds[i].dueAt, seeds[i].completedAt, now)) ++expected;
    }

    auto& sq = env.db->db();
    DOCTEST_REQUIRE(sq.exec(insert).isOk());

    // 路径 A：首页聚合
    module::home::HomeService home(sq);
    auto stats = home.getStats(5);
    DOCTEST_REQUIRE_MESSAGE(stats.isOk(), "getStats 失败: " + stats.errorMessage());

    // 路径 B：Todo 逾期列表
    module::todo::TodoService todo(sq);
    auto overdue = todo.listOverdue();
    DOCTEST_REQUIRE_MESSAGE(overdue.isOk(),
                            "listOverdue 失败: " + overdue.errorMessage());

    // 三方一致：首页计数 == Todo 条数 == core 谓词判定
    DOCTEST_CHECK_EQ(stats.value().overdueTodos, expected);
    DOCTEST_CHECK_EQ(static_cast<int64_t>(overdue.value().size()), expected);
    return true;
})
