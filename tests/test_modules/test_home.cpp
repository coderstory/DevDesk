// tests/test_modules/test_home.cpp
// Home 模块测试：仪表盘真实数据聚合 + 格式化纯函数 + Token 速率卡片口径。
// 跨文件前提：
//   - 「逾期」的谓词单源在 core/overdue.h：首页 getStats 走 COUNT 聚合、Todo 页
//     listOverdue 走列表查询，两条 SQL 用同一份 kWhereSql。口径分叉（改列名 /
//     改比较符 / 只改其中一处）由本文件的一致性用例钉住。
//   - Token 数字口径 = 最近一次成功调用的速率（core/call_rate.h 的 tps 单源），
//     非累计、非实时；无数据时是空态「--」。
#include "doctest.h"
#include "test_support.h"
#include "core/call_rate.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/overdue.h"
#include "core/proxy_stats.h"
#include "core/router.h"
#include "core/theme.h"
#include "core/time.h"
#include "module_home/home_service.h"
// 跨模块逾期口径一致性用例需要同时问首页与 Todo 两条读取路径
#include "module_todo/todo_service.h"

#include <eui/dsl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_homesvc_";

// 模块注册门控用例的最小占位模块（只为让 Router 记住「这些页开着」）。
class GatingStubModule : public ::core::Module {
public:
    GatingStubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return "•"; }
    void compose(eui::Ui&, const eui::Screen&) override {}

private:
    ::core::PageId page_;
    std::string name_;
};

} // namespace

// PageId 顺序不变量（PageId::Home == 0）只在 test_todo.cpp 的 todo_pageid 钉一份，
// 本文件刻意不重复。

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

    // H21 待办口径只认列 kind（schema.h 禁止按标题匹配）：t5（kind='doing'）计入；
    // t6(kind='done')/t7(kind='archive')/t8 与 t9（kind NULL 的自定义列，其一标题
    // 就叫「进行中」）均不计。五行 due_at 均为 NULL，不影响 overdueTodos。
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO todo_tasks (column_id, title, priority, position, due_at, completed_at, archived_at, created_at, updated_at) VALUES "
        "(2, 't5', 0, 4, NULL, NULL, NULL, " + std::to_string(now) + ", " + std::to_string(now) + "), "
        "(3, 't6', 0, 5, NULL, NULL, NULL, " + std::to_string(now) + ", " + std::to_string(now) + "), "
        "(4, 't7', 0, 6, NULL, NULL, " + std::to_string(now) + ", " + std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());

    // 自定义普通列：kind 为空——必须被口径排除；其一标题叫「进行中」，
    // 钉住「不得按标题兜底」（schema.h 契约）。
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO todo_columns (title, position, color, created_at, kind) VALUES "
        "('自定义', 4, NULL, " + std::to_string(now) + ", NULL), "
        "('进行中', 5, NULL, " + std::to_string(now) + ", NULL)"
    ).isOk());
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO todo_tasks (column_id, title, priority, position, due_at, completed_at, created_at, updated_at) VALUES "
        "((SELECT id FROM todo_columns WHERE title = '自定义'), 't8', 0, 7, NULL, NULL, " + std::to_string(now) + ", " + std::to_string(now) + "), "
        "((SELECT id FROM todo_columns WHERE title = '进行中' AND kind IS NULL), 't9', 0, 8, NULL, NULL, " + std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());

    // claude_call_logs：先建 profile（FK ON），2 条今天 + 1 条昨天
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, api_key_check, "
        "model, created_at, updated_at) VALUES "
        "('p1', 'http://x', 'sk-***', 'chk', 'm', " +
        std::to_string(now) + ", " + std::to_string(now) + ")"
    ).isOk());
    DOCTEST_CHECK(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) VALUES "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 30) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - 60) + "), "
        "(1, 'POST', '/v1/messages', " + std::to_string(now - day) + ")"
    ).isOk());

    module::home::HomeService svc(sq);
    auto r = svc.getStats(5);
    DOCTEST_REQUIRE_MESSAGE(r.isOk(), "getStats 失败: " + r.errorMessage());
    const auto& s = r.value();
    DOCTEST_CHECK_EQ(s.todayNotes, 2);        // a + b
    // 待办口径 = 待办列 + 进行中列 的未完成任务：t1 t2 t3（待办列）+ t5（进行中列）
    // t4 已完成；t6 在 kind='done' 列（completed_at 脏 NULL 也不放宽）；
    // t7 归档列；t8/t9 在 kind 为空的自定义列（t9 标题为「进行中」也不按标题计入）
    DOCTEST_CHECK_EQ(s.pendingTodos, 4);
    DOCTEST_CHECK_EQ(s.overdueTodos, 1);      // t2（t4 已完成；t5-t9 无截止时间）
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
        "model, created_at, updated_at) VALUES "
        "('p1', 'http://x', 'sk-***', 'chk', 'm', " +
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
        // 空闲态（无采样/无流）不挂周期 timer：常挂 onTimer 会驱动整页 2Hz 重排
        // 重绘 + 常驻 animating_，反复打断 5 卡 hover 0.1s 过渡致闪烁；空闲静默后
        // 过渡完整跑完、hover 命中缓存恢复有效
        DOCTEST_CHECK(ui.find("home.stat.tokens.timer") == nullptr);
        // 未 onStart（整页没加载成功）→ 错误态 badge 是「加载失败」，与「加载中」区分
        const auto* notesBadge = ui.find("home.stat.notes.badge");
        DOCTEST_REQUIRE(notesBadge != nullptr);
        DOCTEST_CHECK(notesBadge->text.find("加载失败") != std::string::npos);
        const auto* number = ui.find("home.stat.tokens.number");
        DOCTEST_REQUIRE(number != nullptr);
        // 数字来自最近一次成功调用；未加载到数据时是空态「--」，不是实时速率「0」
        DOCTEST_CHECK_EQ(number->text, std::string("--"));
        const auto* badge = ui.find("home.stat.tokens.badge");
        DOCTEST_REQUIRE(badge != nullptr);
        // 同 notes 卡口径「加载失败」，不冒充「暂无成功调用」
        DOCTEST_CHECK(badge->text.find("加载失败") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("累计") == std::string::npos);
        mod->onStop();
    }

    // 流式中态：注入 usage → activeStreams=1 → badge 带并发数 + 「最近一次」
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
        DOCTEST_CHECK(badge->text.find("1 路") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("生成中") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("累计") == std::string::npos);
        // 本节未 onStart（整页没加载）→ 数字是空态，副标题不承诺「最近一次」
        //（该字样由已加载档位断言，见下方 home_last_rate_and_hourly_tokens）
        DOCTEST_CHECK(badge->text.find("最近一次") == std::string::npos);
        // 流式中：周期重绘挂载（1s、仅活动期）
        DOCTEST_CHECK(ui.find("home.stat.tokens.timer") != nullptr);
        mod->onStop();

        core::proxy_stats::endStream(id);
    }

    core::proxy_stats::resetForTest();
    return true;
})

// ============================================================================
// S3.1 / S6.6 实底统计卡契约（home_module.cpp drawSolidStatCard）
// 防的事故：
//  ① 五卡背景色写错/漏卡（逐卡 RGB 断言，S6.6 五色：cardGreen/cardBlue/cardOrange/cardPurple/cardCyan）
//  ② 玻璃层组残留（.glass.* 节点应全部缺位）
//  ③ 点击层仍可用 + 文字层级完整（label/number/badge 三行）
//  ④ 对比度门槛：数字 textPrimary on 辅色底 ≥4.5:1（WCAG AA），三级灰档 S6.6 预设
// ============================================================================
namespace {

double solidLuminance(const eui::Color& c) {
    const auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

double solidContrast(const eui::Color& a, const eui::Color& b) {
    const double la = solidLuminance(a);
    const double lb = solidLuminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

} // namespace

REGISTER_TEST(home_solid_stat_cards,
              "Home 实底统计卡: S6.6 五色实底 + 无玻璃残留 + 点击 + 对比度护栏", {
    core::proxy_stats::resetForTest();
    core::theme::setLight();

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 720.0f;
    eui::Ui ui;
    ui.begin("home_solid");
    mod->compose(ui, screen);
    ui.end();

    const auto& t = core::theme::current();

    // —— ① 五卡实底色逐卡断言（S6.6：绿/蓝/橙/紫/青）——
    struct ExpBg {
        const char* id;
        std::uint32_t hex;
        float r, g, b;
    };
    const ExpBg kExpBgs[] = {
        {"home.stat.tokens", 0xE6FBF2, 0.902f, 0.984f, 0.949f},   // cardGreen
        {"home.stat.notes", 0xE0F4FF, 0.878f, 0.957f, 1.000f},     // cardBlue
        {"home.stat.todo", 0xFFF1E0, 1.000f, 0.945f, 0.878f},      // cardOrange
        {"home.stat.overdue", 0xF3E8FF, 0.953f, 0.910f, 1.000f},   // cardPurple
        {"home.stat.api", 0xDFFAF6, 0.875f, 0.980f, 0.965f},       // cardCyan
    };
    for (const auto& e : kExpBgs) {
        const std::string id(e.id);
        const auto* bgEl = ui.find(id + ".bg");
        DOCTEST_REQUIRE_MESSAGE(bgEl != nullptr, id + ": 缺卡片底色 .bg");
        const eui::Color exp = eui::Color::fromHex(e.hex);
        DOCTEST_CHECK_MESSAGE(
            std::fabs(bgEl->color.r - exp.r) < 1e-4f &&
                std::fabs(bgEl->color.g - exp.g) < 1e-4f &&
                std::fabs(bgEl->color.b - exp.b) < 1e-4f,
            id + ": 底色 RGB 与 S6.6 设计基准不符");
        DOCTEST_CHECK_MESSAGE(bgEl->color.a >= 0.99f,
                              id + ": 底应为实色（a≈1），非半透");

        // —— ② 玻璃层组已退役（S6.1）：.glass.* 节点全部缺位 ——
        DOCTEST_CHECK(ui.find(id + ".glass.tint") == nullptr);
        DOCTEST_CHECK(ui.find(id + ".glass.hl.top") == nullptr);
        DOCTEST_CHECK(ui.find(id + ".glass.hl.side") == nullptr);
        DOCTEST_CHECK(ui.find(id + ".glass.topline") == nullptr);
        DOCTEST_CHECK(ui.find(id + ".glass.stain.0.halo") == nullptr);

        // —— ③ 文字层级完整 + 点击层可用 ——
        DOCTEST_CHECK(ui.find(id + ".label") != nullptr);
        DOCTEST_CHECK(ui.find(id + ".number") != nullptr);
        DOCTEST_CHECK(ui.find(id + ".badge") != nullptr);
        const auto* press = ui.find(id + ".press");
        DOCTEST_REQUIRE_MESSAGE(press != nullptr, id + ": 缺整卡点击层 .press");
        DOCTEST_CHECK(press->onClick != nullptr);
        DOCTEST_CHECK(press->hasStateColors);
        // normal 态全透明
        DOCTEST_CHECK(press->color.a < 1e-4f);
    }

    // —— ④ 对比度护栏（Solid底——alpha=1.0，只有 light/dark 两态，无窗透态）——
    struct AuxC {
        std::uint32_t bgHex;
        const char* name;
    };
    const AuxC auxBg[] = {
        {0xE6FBF2, "cardGreen"}, {0xE0F4FF, "cardBlue"},
        {0xFFF1E0, "cardOrange"}, {0xF3E8FF, "cardPurple"},
        {0xDFFAF6, "cardCyan"},
    };
    double minNumCt = 1e9;
    double minLabelCt = 1e9;
    double minRedCt = 1e9;
    for (const auto& ac : auxBg) {
        const eui::Color bgC = eui::Color::fromHex(ac.bgHex);
        const double num = solidContrast(t.colors.textPrimary, bgC);
        const double label = solidContrast(t.colors.textTertiary, bgC);
        // 红字数字（逾期卡 accent 态）走 errorText 档，护栏随文字 token 走
        const double red = solidContrast(t.colors.errorText, bgC);
        minNumCt = std::min(minNumCt, num);
        minLabelCt = std::min(minLabelCt, label);
        minRedCt = std::min(minRedCt, red);
        DOCTEST_CHECK_MESSAGE(num >= 4.5,
            std::string(ac.name) + ": 数字对比度 " + std::to_string(num) + " < 4.5");
    }
    DOCTEST_CHECK(minNumCt < 1e8);
    DOCTEST_CHECK(minLabelCt < 1e8);
    DOCTEST_CHECK(minRedCt < 1e8);

    // —— ⑤ 深色模式：卡底不透明，文字可读 ——
    {
        core::theme::setDark();
        eui::Ui uiD;
        uiD.begin("home_solid_dark");
        mod->compose(uiD, screen);
        uiD.end();
        const auto* bgD = uiD.find("home.stat.notes.bg");
        DOCTEST_REQUIRE(bgD != nullptr);
        DOCTEST_CHECK(bgD->color.a >= 0.99f);  // 暗色下亦实色
        core::theme::setLight();
    }

    mod->onStop();
    core::proxy_stats::resetForTest();
    return true;
})

// 「最近一次调用速率」/「每小时 token 合计」是「手算值与查询值必须相等」的数值断言：
// 把速率算成累计/实时值、把分布图退回百分比或按绝对小时号取下标，本用例就红。
REGISTER_TEST(home_last_rate_and_hourly_tokens,
              "HomeService: 最近一次成功调用速率 + 逐时 token 合计（手算对照）",
              {
    test_support::TestEnv env(kDbPrefix, "rate_tokens", true);  // 注册 appDatabase
    DOCTEST_REQUIRE(env.ready);
    auto& sq = env.db->db();
    const int64_t dayStart = module::home::startOfDayEpoch(std::time(nullptr));
    DOCTEST_REQUIRE(sq.exec(
        "INSERT INTO claude_profiles (name, base_url, api_key_hint, "
        "api_key_check, model, created_at, updated_at) "
        "VALUES ('p1', 'http://x', 'sk-***', 'chk', 'm', 1000, 1000)")
                        .isOk());

    // 阶段 A：只有老行（status_code / output_tokens 全 NULL）→ 没有成功调用
    DOCTEST_REQUIRE(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, started_at) "
        "VALUES (1, 'POST', '/v1/messages', " +
        std::to_string(dayStart + 3600) + ")").isOk());
    {
        module::home::HomeService svc(sq);
        auto lr = svc.getLastCallRate();
        DOCTEST_REQUIRE(lr.isOk());
        // NULL（未采集）≠ 0 个 token，也不算「成功调用」（status NULL）
        DOCTEST_CHECK_FALSE(lr.value().hasValue);

        auto series = svc.getTodayHourlyTokenTotals(24);
        DOCTEST_REQUIRE(series.isOk());
        int64_t total = 0;
        for (const int64_t v : series.value()) total += v;
        DOCTEST_CHECK_EQ(total, static_cast<int64_t>(0));
        DOCTEST_CHECK_FALSE(module::home::hourlyDataRange(series.value()).hasData);

        // UI 空态：数字「--」+ 副标题「暂无成功调用」+ 分布图空态（不画空轴）
        std::unique_ptr<::core::Module> mod(create_home_module());
        mod->onStart();
        eui::Screen screen{1200.0f, 720.0f};
        eui::Ui ui;
        ui.begin("home_rate_empty");
        mod->compose(ui, screen);
        ui.end();
        const auto* number = ui.find("home.stat.tokens.number");
        const auto* badge = ui.find("home.stat.tokens.badge");
        DOCTEST_REQUIRE(number != nullptr);
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK_EQ(number->text, std::string("--"));
        DOCTEST_CHECK_EQ(badge->text, std::string("暂无成功调用"));
        // 待办卡副题与 H21 口径一致（待办列 + 进行中列）
        const auto* todoBadge = ui.find("home.stat.todo.badge");
        DOCTEST_REQUIRE(todoBadge != nullptr);
        DOCTEST_CHECK_EQ(todoBadge->text, std::string("待办+进行中未完成"));
        DOCTEST_CHECK(ui.find("home.api.chart.empty.title") != nullptr);
        DOCTEST_CHECK(ui.find("home.api.chart.bars.bar.0") == nullptr);
        mod->onStop();
    }

    // 阶段 B：只有一条「2xx + output>0 但 latency_ms=0」的行 → 仍无速率
    // （除零兜底：耗时未采集时不能伪造速率）
    DOCTEST_REQUIRE(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, status_code, "
        "latency_ms, output_tokens, started_at) VALUES "
        "(1, 'POST', '/v1/messages', 200, 0, 120, " +
        std::to_string(dayStart + 3600 * 4) + ")").isOk());
    {
        module::home::HomeService svc(sq);
        auto lr = svc.getLastCallRate();
        DOCTEST_REQUIRE(lr.isOk());
        DOCTEST_CHECK_FALSE(lr.value().hasValue);
        DOCTEST_CHECK_EQ(lr.value().outputTokens, static_cast<int64_t>(120));
        DOCTEST_CHECK_EQ(lr.value().latencyMs, static_cast<int64_t>(0));
    }

    // 阶段 C：补齐三维（成功 / 非 2xx / 昨天）→ 手算值对照
    //   2 时  200 / 1200ms / out  60  → 速率 50.0
    //   3 时  500 /  100ms / out NULL → 非 2xx + 无用量：速率与合计都不参与
    //   5 时  200 / 2400ms / out 240  → 速率 100.0（最近一次成功调用）
    //   昨天  200 / 1000ms / out 777  → 日界外：不进逐时合计、也不是「最近」
    DOCTEST_REQUIRE(sq.exec(
        "INSERT INTO claude_call_logs (profile_id, method, path, status_code, "
        "latency_ms, output_tokens, started_at) VALUES "
        "(1, 'POST', '/v1/messages', 200, 1200, 60, " +
        std::to_string(dayStart + 3600 * 2) + "), "
        "(1, 'POST', '/v1/messages', 500, 100, NULL, " +
        std::to_string(dayStart + 3600 * 3) + "), "
        "(1, 'POST', '/v1/messages', 200, 2400, 240, " +
        std::to_string(dayStart + 3600 * 5) + "), "
        "(1, 'POST', '/v1/messages', 200, 1000, 777, " +
        std::to_string(dayStart - 3600 * 2) + ")").isOk());
    {
        module::home::HomeService svc(sq);
        // ① 手算：240 / (2400/1000) = 100.0（5 时那条：2xx 且 output>0 中 started_at
        // 最大；3 时的 500 更晚但非成功、昨天的更早）
        auto lr = svc.getLastCallRate();
        DOCTEST_REQUIRE(lr.isOk());
        DOCTEST_CHECK(lr.value().hasValue);
        DOCTEST_CHECK_EQ(lr.value().outputTokens, static_cast<int64_t>(240));
        DOCTEST_CHECK_EQ(lr.value().latencyMs, static_cast<int64_t>(2400));
        DOCTEST_CHECK(std::fabs(lr.value().tps - 100.0) < 1e-9);
        DOCTEST_CHECK_EQ(core::call_rate::formatTps(lr.value().tps),
                         std::string("100.0"));

        // ③ 手算：2 时 60 / 4 时 120（阶段 B 那条，latency=0 不影响合计）
        //        / 5 时 240；昨天 777 不进；区间 [2, 5] → 4 根柱
        auto series = svc.getTodayHourlyTokenTotals(24);
        DOCTEST_REQUIRE(series.isOk());
        DOCTEST_CHECK_EQ(series.value().size(), 24u);
        int64_t total = 0;
        for (const int64_t v : series.value()) total += v;
        DOCTEST_CHECK_EQ(total, static_cast<int64_t>(420));  // 60+120+240
        DOCTEST_CHECK_EQ(series.value()[2], 60);
        DOCTEST_CHECK_EQ(series.value()[3], 0);   // 非 2xx + NULL 用量
        DOCTEST_CHECK_EQ(series.value()[4], 120);
        DOCTEST_CHECK_EQ(series.value()[5], 240);
        const auto range = module::home::hourlyDataRange(series.value());
        DOCTEST_CHECK(range.hasData);
        DOCTEST_CHECK_EQ(range.first, static_cast<size_t>(2));
        DOCTEST_CHECK_EQ(range.last, static_cast<size_t>(5));  // 昨天 777 不扩尾

        // 单源口径（core/call_rate.h）边界：非 2xx / 0 token / 0 耗时都无速率
        double tps = 0.0;
        DOCTEST_CHECK_FALSE(core::call_rate::tps(500, 240, 2400, tps));
        DOCTEST_CHECK_FALSE(core::call_rate::tps(200, 0, 2400, tps));
        DOCTEST_CHECK_FALSE(core::call_rate::tps(200, 240, 0, tps));
        DOCTEST_CHECK_FALSE(core::call_rate::tps(199, 240, 2400, tps));
        DOCTEST_CHECK_FALSE(core::call_rate::tps(300, 240, 2400, tps));
        DOCTEST_CHECK(core::call_rate::tps(299, 1, 2000, tps));
        DOCTEST_CHECK(std::fabs(tps - 0.5) < 1e-9);
        // 恒 1 位小数（不随量级切换成整数）
        DOCTEST_CHECK_EQ(core::call_rate::formatTps(0.5), std::string("0.5"));
        DOCTEST_CHECK_EQ(core::call_rate::formatTps(123.44),
                         std::string("123.4"));
    }

    // 流式档 + 已加载：数字仍是「最近一次」的速率 → 副标题必须说出来
    //（流式只影响副标题的实时状态前缀，不改数字口径）
    {
        core::proxy_stats::resetForTest();
        const uint64_t sid = core::proxy_stats::beginStream();
        core::proxy_stats::updateStream(sid, 42);
        std::unique_ptr<::core::Module> mod(create_home_module());
        mod->onStart();
        eui::Screen screen{1200.0f, 720.0f};
        eui::Ui ui;
        ui.begin("home_rate_streaming");
        mod->compose(ui, screen);
        ui.end();
        const auto* number = ui.find("home.stat.tokens.number");
        const auto* badge = ui.find("home.stat.tokens.badge");
        DOCTEST_REQUIRE(number != nullptr);
        DOCTEST_REQUIRE(badge != nullptr);
        DOCTEST_CHECK_EQ(number->text, std::string("100.0"));
        DOCTEST_CHECK(badge->text.find("1 路生成中") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("最近一次") != std::string::npos);
        DOCTEST_CHECK(badge->text.find("累计") == std::string::npos);
        mod->onStop();
        core::proxy_stats::endStream(sid);
        core::proxy_stats::resetForTest();
    }
    return true;
})
// H18 首页纵向弹性双档（真实页高）：600 档 = 最小窗 1000×640 减标题条（窗高封顶
// 已废止 → fs=1）；960 档 = 1600×1000 减标题条。弹性基准 = 内容刚好填满页高（全部令牌派生），
// 余高 18% 给统计行、其余归图表卡；底部仅留 0.5px 防浮点越界出滚条。
REGISTER_TEST(home_stat_chart_dual_tier,
              "home_module: H18 统计卡/图表卡纵向弹性双档（页高 600 / 960）", {
    core::proxy_stats::resetForTest();
    core::theme::setLight();
    // 字号档是进程级全局态：用户档固定 100%（窗高封顶已废止 → 600 档 fs=1）
    core::theme::setFontScale(1.0f);

    std::unique_ptr<::core::Module> mod(create_home_module());

    // 600 档：真实最小页高，fs=1 下验证弹性基准几何
    {
        core::theme::setWindowHeight(600.0f);
        const float fs = core::theme::fontScale();
        DOCTEST_REQUIRE_MESSAGE(std::fabs(fs - 1.0f) < 1e-4f,
                                "600 档生效字号应为用户档 1.0（窗高封顶已废止），实际 " +
                                    std::to_string(fs));
        eui::Ui ui;
        eui::Screen screen{1000.0f, 600.0f};
        ui.begin("home_tier_600");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const auto* stat = ui.find("home.stat.notes");
        const auto* chart = ui.find("home.api.chart");
        const auto* number = ui.find("home.stat.notes.number");
        const auto* desc = ui.find("home.stat.notes.badge");
        const auto* root = ui.find("home.root");
        DOCTEST_REQUIRE(stat != nullptr);
        DOCTEST_REQUIRE(chart != nullptr);
        DOCTEST_REQUIRE(number != nullptr);
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_REQUIRE(root != nullptr);
        // 基准 = pad×2(64)+页头40+统计144+gap×3(60)+图220+操作44 = 572；
        // 余高 600−572−0.5=27.5 → 统计 148.95，图表 242.55
        DOCTEST_CHECK_MESSAGE(
            std::fabs(stat->frame.height - 160.47f) < 0.5f,
            "600 档统计卡高应为 148.95，实际 " +
                std::to_string(stat->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(chart->frame.height - 295.03f) < 0.5f,
            "600 档图表卡高应为 242.55，实际 " +
                std::to_string(chart->frame.height));
        // 卡内三行随卡高派生：仍在卡内、number 与 desc 不重叠
        const float cardTop = stat->frame.y;
        const float cardBottom = cardTop + stat->frame.height;
        DOCTEST_CHECK(number->frame.y >= cardTop);
        DOCTEST_CHECK(number->frame.y + number->frame.height <= cardBottom);
        DOCTEST_CHECK(desc->frame.y >= number->frame.y + number->frame.height);
        DOCTEST_CHECK(desc->frame.y + desc->frame.height <= cardBottom);
        // 内容刚好填满页高：基准以上余高被统计行/图表卡全部吸收
        const float bottomMargin =
            screen.height - (root->frame.y + root->frame.height);
        DOCTEST_CHECK_MESSAGE(
            bottomMargin <= 1.0f,
            "600 档内容应填满页高（底部余量 ≤1），实际 " +
                std::to_string(bottomMargin));
    }

    // 960 档（1600×1000 真实页高，fs=1）：基准 572、余高 388−0.5=387.5 →
    // 统计卡 213.75，图表卡 537.75（写死具体值，不用恒等式）
    {
        core::theme::setWindowHeight(960.0f);
        const float fs = core::theme::fontScale();
        DOCTEST_REQUIRE_MESSAGE(std::fabs(fs - 1.0f) < 1e-4f,
                                "960 档应保持用户档 1.0，实际 " +
                                    std::to_string(fs));
        eui::Ui ui;
        eui::Screen screen{1600.0f, 960.0f};
        ui.begin("home_tier_960");
        mod->compose(ui, screen);
        ui.end();
        ui.layout(screen.width, screen.height);
        const auto* stat = ui.find("home.stat.notes");
        const auto* chart = ui.find("home.api.chart");
        const auto* number = ui.find("home.stat.notes.number");
        const auto* desc = ui.find("home.stat.notes.badge");
        const auto* root = ui.find("home.root");
        DOCTEST_REQUIRE(stat != nullptr);
        DOCTEST_REQUIRE(chart != nullptr);
        DOCTEST_REQUIRE(number != nullptr);
        DOCTEST_REQUIRE(desc != nullptr);
        DOCTEST_REQUIRE(root != nullptr);
        DOCTEST_CHECK_MESSAGE(
            std::fabs(stat->frame.height - 225.27f) < 0.5f,
            "960 档统计卡高应为 213.75，实际 " +
                std::to_string(stat->frame.height));
        DOCTEST_CHECK_MESSAGE(
            std::fabs(chart->frame.height - 590.23f) < 0.5f,
            "960 档图表卡高应为 537.75，实际 " +
                std::to_string(chart->frame.height));
        // 卡高增长后卡内三行仍居中分布、在卡内且不重叠
        const float cardTop = stat->frame.y;
        const float cardBottom = cardTop + stat->frame.height;
        DOCTEST_CHECK(number->frame.y >= cardTop);
        DOCTEST_CHECK(number->frame.y + number->frame.height <= cardBottom);
        DOCTEST_CHECK(desc->frame.y >= number->frame.y + number->frame.height);
        DOCTEST_CHECK(desc->frame.y + desc->frame.height <= cardBottom);
        // 内容刚好填满页高：基准以上余高被统计行/图表卡全部吸收
        const float bottomMargin =
            screen.height - (root->frame.y + root->frame.height);
        DOCTEST_CHECK_MESSAGE(
            bottomMargin <= 1.0f,
            "960 档内容应填满页高（底部余量 ≤1），实际 " +
                std::to_string(bottomMargin));
    }

    // 复位进程级全局态，避免污染其它用例
    core::theme::setWindowHeight(1000.0f);
    mod->onStop();
    core::proxy_stats::resetForTest();
    return true;
})

// 关闭 ClaudeProxy：图表卡不渲染时余高必须由统计行吸收，页底不留死区。
// 基准（无图表卡）= 64 + 页头40 + 统计144 + gap×2(40) + 操作44 = 332。
REGISTER_TEST(home_no_chart_extra_to_stats,
              "home_module: ClaudeProxy 关闭时余高全给统计行（无图表卡）", {
    core::theme::setFontScale(1.0f);
    core::theme::setWindowHeight(960.0f);  // 窗高不封顶 → fs=1

    ::core::Router router;
    GatingStubModule home(::core::PageId::Home, "home");
    GatingStubModule markdown(::core::PageId::Markdown, "markdown");
    GatingStubModule todo(::core::PageId::Todo, "todo");
    router.registerModule(&home);
    router.registerModule(&markdown);
    router.registerModule(&todo);
    test_support::ScopedAppRouter scoped(&router);

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen{1600.0f, 960.0f};
    eui::Ui ui;
    ui.begin("home_no_chart");
    mod->compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);

    DOCTEST_CHECK(ui.find("home.api.chart") == nullptr);
    const auto* stat = ui.find("home.stat.notes");
    const auto* root = ui.find("home.root");
    DOCTEST_REQUIRE(stat != nullptr);
    DOCTEST_REQUIRE(root != nullptr);
    // 余高 960−332−0.5=627.5 全给统计行 → 144+627.5=771.5；内容填满页高
    DOCTEST_CHECK_MESSAGE(
        std::fabs(stat->frame.height - 835.5f) < 0.5f,
        "无图表卡时统计行应吸收全部余高（期望 771.5），实际 " +
            std::to_string(stat->frame.height));
    const float bottomMargin =
        screen.height - (root->frame.y + root->frame.height);
    DOCTEST_CHECK_MESSAGE(
        bottomMargin <= 1.0f,
        "无图表卡时页底不应留死区（余量 ≤1），实际 " +
            std::to_string(bottomMargin));

    core::theme::setWindowHeight(1000.0f);
    mod->onStop();
    return true;
})

// 同一批种子数据同时问两条读取路径，把它们钉在同一个数字上——口径分叉即红
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

    // 种子：覆盖「无截止 / 逾期未完成 / 逾期已完成 / 未到期」四种组合。
    // REGISTER_TEST 走 __VA_ARGS__，花括号初始化列表的顶层逗号不会被当宏实参
    // 分隔符，可直接在用例体内初始化
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
