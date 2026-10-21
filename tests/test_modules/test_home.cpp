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
#include "core/theme.h"
#include "core/time.h"
#include "module_home/home_service.h"
// 跨模块逾期口径一致性用例需要同时问首页与 Todo 两条读取路径
#include "module_todo/todo_service.h"

#include <eui/dsl.h>

#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* kDbPrefix = "devdesk_test_homesvc_";

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
// 玻璃统计卡契约（UAT6 方案一 · 五色软染玻璃，home_module.cpp drawGlassStatCard）
// 防的事故：
//  ① 染色写错卡/漏卡（表驱动五色逐卡 RGB 断言，设计基准拍板色 绿/蓝/橙/紫/青）
//  ② 玻璃层组缺件塌回白卡（染色/纵向高光/横向偏置高光/顶线/三组污渍/交互纱逐层断言）
//  ③ hover 不再整卡或加深不可辨（misc1 语义：press 框 == 卡框 + textPrimary 派生纱）
//  ④ 染色/面透明度调过线压垮文字（三态对比度护栏，effects 批同模型）
// 对比度模型口径（与 test_settings settings_window_effect_param_mapping 钉的
// clearColor 内容纱同源：translucent=0.82 / frosted=0.15 / off=1.0；磨砂黑壁纸底取
// effects 批实机材质输出 M=192）。#999 三级灰在纯白上仅 2.85:1——4.5 对该档数学
// 不可达（W9 既定档），护栏按「数字 ≥4.5 + 灰/红档不劣化」落。
// ============================================================================
namespace {

double glassLuminance(const eui::Color& c) {
    const auto lin = [](double v) {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

double glassContrast(const eui::Color& a, const eui::Color& b) {
    const double la = glassLuminance(a);
    const double lb = glassLuminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// Porter-Duff over：top 的 rgb/alpha 即元素色（sRGB 空间直叠，同渲染端 mix 口径，
// 亦即 soft-light 的浅底近似公式 comp = α·T + (1−α)·face）
eui::Color glassOver(const eui::Color& top, const eui::Color& bottom) {
    eui::Color out;
    const float k = 1.0f - top.a;
    out.r = top.r * top.a + bottom.r * k;
    out.g = top.g * top.a + bottom.g * k;
    out.b = top.b * top.a + bottom.b * k;
    out.a = top.a + bottom.a * k;
    return out;
}

bool glassNear(float a, float b) { return std::fabs(a - b) < 0.5f; }

} // namespace

REGISTER_TEST(home_glass_stat_cards,
              "Home 玻璃统计卡: 五色染色 + 层组完整 + hover 整卡 + 三态对比度护栏", {
    core::proxy_stats::resetForTest();
    core::theme::setLight();  // 对比度模型钉浅色档（dark-WeChat 延后 D-04）

    std::unique_ptr<::core::Module> mod(create_home_module());
    eui::Screen screen;
    screen.width = 1200.0f;
    screen.height = 720.0f;
    eui::Ui ui;
    ui.begin("home_glass");
    mod->compose(ui, screen);
    ui.end();

    const auto& t = core::theme::current();
    const auto& wc = t.colors.wechat;

    // —— ① 五卡染色逐卡断言（绿/蓝/橙/紫/青按统计卡语义顺序）——
    struct ExpTint {
        const char* id;
        std::uint32_t hex;
    };
    const ExpTint kExpTints[] = {
        {"home.stat.tokens", 0x7EE2A8},  // 绿
        {"home.stat.notes", 0x8ECBF5},    // 蓝
        {"home.stat.todo", 0xF5C98E},     // 橙
        {"home.stat.overdue", 0xC8A8F5},  // 紫
        {"home.stat.api", 0x8EF0E2},      // 青
    };
    for (const auto& e : kExpTints) {
        const std::string id(e.id);
        const auto* tintEl = ui.find(id + ".glass.tint");
        DOCTEST_REQUIRE_MESSAGE(tintEl != nullptr,
                                id + ": 缺染色层 .glass.tint");
        const eui::Color exp = eui::Color::fromHex(e.hex);
        DOCTEST_CHECK_MESSAGE(std::fabs(tintEl->color.r - exp.r) < 1e-4f &&
                                  std::fabs(tintEl->color.g - exp.g) < 1e-4f &&
                                  std::fabs(tintEl->color.b - exp.b) < 1e-4f,
                              id + ": 染色 RGB 与设计基准不符（方案一拍板色）");
        // 染色强度带：低过 0.10 玻璃退化成白卡（染色没了，护栏失去意义）
        DOCTEST_CHECK_MESSAGE(tintEl->color.a >= 0.10f &&
                                  tintEl->color.a <= 0.25f,
                              id + ": 染色 alpha 出强度带（soft-light 直叠近似档）");

        // —— ② 层组完整性（五卡共享 helper，塌一层即红）——
        const auto* hlTop = ui.find(id + ".glass.hl.top");
        const auto* hlSide = ui.find(id + ".glass.hl.side");
        const auto* topLine = ui.find(id + ".glass.topline");
        DOCTEST_REQUIRE_MESSAGE(hlTop != nullptr, id + ": 缺纵向高光层");
        DOCTEST_REQUIRE_MESSAGE(hlSide != nullptr, id + ": 缺横向偏置高光层");
        DOCTEST_REQUIRE_MESSAGE(topLine != nullptr, id + ": 缺顶缘 inset 高光线");
        // 白色顶部高光：纵向主带 Vertical、起点亮 alpha、终点 alpha=0（渐弱没顶）
        DOCTEST_CHECK(hlTop->gradient.enabled);
        DOCTEST_CHECK(hlTop->gradient.direction ==
                      eui::GradientDirection::Vertical);
        DOCTEST_CHECK(hlTop->gradient.start.a > 0.20f);
        DOCTEST_CHECK(hlTop->gradient.end.a < 1e-4f);
        // 左上偏置带：横向、弱一档（两带叠出「左上→中右渐弱」观感）
        DOCTEST_CHECK(hlSide->gradient.enabled);
        DOCTEST_CHECK(hlSide->gradient.direction ==
                      eui::GradientDirection::Horizontal);
        DOCTEST_CHECK(hlSide->gradient.start.a > 0.08f);
        DOCTEST_CHECK(hlSide->gradient.end.a < 1e-4f);
        DOCTEST_CHECK_MESSAGE(glassNear(topLine->frame.height, 1.0f),
                              id + ": 顶缘高光线应为 1px 发丝线");
        // 三组污渍（白/暗/白）halo+core 双层圆斑存在
        for (int i = 0; i < 3; ++i) {
            DOCTEST_CHECK(ui.find(id + ".glass.stain." +
                                  std::to_string(i) + ".halo") != nullptr);
            DOCTEST_CHECK(ui.find(id + ".glass.stain." +
                                  std::to_string(i) + ".core") != nullptr);
        }

        // —— ③ hover 仍整卡（misc1 语义）：整卡矩形 + 染色面上加深可辨 ——
        const auto* card = ui.find(id);
        const auto* press = ui.find(id + ".press");
        DOCTEST_REQUIRE(card != nullptr);
        DOCTEST_REQUIRE_MESSAGE(press != nullptr, id + ": 缺整卡交互层 .press");
        DOCTEST_CHECK(press->hasStateColors);
        DOCTEST_CHECK(press->onClick != nullptr);
        // 整卡矩形：press 框 == 卡根框（同位同尺寸）
        DOCTEST_CHECK_MESSAGE(glassNear(press->frame.x, card->frame.x) &&
                                  glassNear(press->frame.y, card->frame.y) &&
                                  glassNear(press->frame.width,
                                            card->frame.width) &&
                                  glassNear(press->frame.height,
                                            card->frame.height),
                              id + ": 交互层必须盖满整卡（misc1 整卡语义）");
        // 深色纱三档：normal 全透明、hover/按压低 alpha 递增、rgb 走 textPrimary 令牌族
        DOCTEST_CHECK(press->color.a < 1e-4f);
        DOCTEST_CHECK_MESSAGE(press->hoverColor.a > 0.0f &&
                                  press->hoverColor.a < 0.2f,
                              id + ": hover 纱应为低 alpha 深色（0.06 档）");
        DOCTEST_CHECK(press->pressedColor.a > press->hoverColor.a);
        DOCTEST_CHECK(std::fabs(press->hoverColor.r - wc.textPrimary.r) <
                      1e-4f);
        // 污渍不出卡界（stack 不裁剪，靠半径钳制兜底）：污渍圆 ⊆ 卡框
        for (int i = 0; i < 3; ++i) {
            for (const char* part : {".halo", ".core"}) {
                const auto* st = ui.find(id + ".glass.stain." +
                                         std::to_string(i) + part);
                DOCTEST_REQUIRE(st != nullptr);
                DOCTEST_CHECK_MESSAGE(
                    st->frame.x >= card->frame.x - 0.5f &&
                        st->frame.y >= card->frame.y - 0.5f &&
                        st->frame.x + st->frame.width <=
                            card->frame.x + card->frame.width + 0.5f &&
                        st->frame.y + st->frame.height <=
                            card->frame.y + card->frame.height + 0.5f,
                    id + ": 污渍 " + std::to_string(i) + part + " 戳出卡界");
            }
        }
    }

    // —— ④ 三态对比度护栏（effects 批同模型；组合元素色从组合结果里实取，
    //    改 alpha 常量时护栏随之重算，不钉死常量本身）——
    // 三态底（window clearColor = theme.background）：
    //   off   = background 实色
    //   半透  = background@0.82 over 纯白壁纸（暗文字最坏 = 最亮底）
    //   磨砂  = background@0.15 over M=192（黑壁纸实机材质输出 = 最暗底）
    const eui::Color bg = t.colors.background;
    const eui::Color whiteWall{1.0f, 1.0f, 1.0f, 1.0f};
    const eui::Color frostedWall{192.0f / 255.0f, 192.0f / 255.0f,
                                 192.0f / 255.0f, 1.0f};
    const eui::Color bases[3] = {
        bg,
        glassOver(eui::Color{bg.r, bg.g, bg.b, 0.82f}, whiteWall),
        glassOver(eui::Color{bg.r, bg.g, bg.b, 0.15f}, frostedWall)};
    const char* stateNames[3] = {"不透明页", "半透窗", "磨砂窗"};

    double minNum = 1e9;
    double minLabel = 1e9;
    double minRed = 1e9;
    for (int stIdx = 0; stIdx < 3; ++stIdx) {
        for (const auto& e : kExpTints) {
            const std::string id(e.id);
            const auto* faceEl = ui.find(id + ".bg");
            const auto* tintEl = ui.find(id + ".glass.tint");
            const auto* stainEl = ui.find(id + ".glass.stain.1.core");
            DOCTEST_REQUIRE(faceEl != nullptr);
            DOCTEST_REQUIRE(tintEl != nullptr);
            DOCTEST_REQUIRE(stainEl != nullptr);
            // worst 文字区 = 卡底暗污渍带、无顶部高光（高光只会把底打得更亮）：
            // 底 → 白面(face) → 染色(tint) → 暗污渍(stain)
            const eui::Color comp =
                glassOver(stainEl->color,
                          glassOver(tintEl->color,
                                    glassOver(faceEl->color, bases[stIdx])));

            const double num = glassContrast(wc.textPrimary, comp);
            const double label = glassContrast(wc.textTertiary, comp);
            const double red = glassContrast(wc.warningRed, comp);
            minNum = std::min(minNum, num);
            minLabel = std::min(minLabel, label);
            minRed = std::min(minRed, red);

            // 数字（textPrimary 档）三态全 ≥4.5:1（WCAG AA）
            DOCTEST_CHECK_MESSAGE(
                num >= 4.5, id + " " + stateNames[stIdx] + ": 数字对比度 " +
                                std::to_string(num) + " < 4.5");
            // 标题/副题（#999 三级灰）：纯白上限 2.85，护栏取不劣化档
            const double labelFloor = (stIdx == 2) ? 2.05 : 2.40;
            DOCTEST_CHECK_MESSAGE(
                label >= labelFloor,
                id + " " + stateNames[stIdx] + ": 标题灰对比度 " +
                    std::to_string(label) + " < " +
                    std::to_string(labelFloor));
            // 逾期卡 warningRed 数字/副题档（磨砂最暗底随 label 同比例放宽一档）
            const double redFloor = (stIdx == 2) ? 2.40 : 2.75;
            DOCTEST_CHECK_MESSAGE(
                red >= redFloor,
                id + " " + stateNames[stIdx] + ": 警示红对比度 " +
                    std::to_string(red) + " < " + std::to_string(redFloor));
            // 玻璃面（未染色）任何状态下不比页面底暗：卡上文字恒不劣于同态页面级
            const eui::Color untinted = glassOver(faceEl->color, bases[stIdx]);
            DOCTEST_CHECK_MESSAGE(
                glassLuminance(untinted) >=
                    glassLuminance(bases[stIdx]) - 1e-6,
                id + " " + stateNames[stIdx] + ": 玻璃面不该比页面底更暗");
        }
    }
    // 轮子真转过（防集合为空导致上面的逐项断言恒真）
    DOCTEST_CHECK(minNum < 1e8);
    DOCTEST_CHECK(minLabel < 1e8);
    DOCTEST_CHECK(minRed < 1e8);

    // —— ⑤ 深色档面透明度分支：dark-WeChat 延后（D-04）wechat 槽位深浅同值，
    //    白面 0.62 在深色窗底会塌成奶灰、灰字对比度跳水——深色档 face alpha
    //    抬到近实白防塌（染色层照常渲染）——
    {
        core::theme::setDark();
        eui::Ui uiD;
        uiD.begin("home_glass_dark");
        mod->compose(uiD, screen);
        uiD.end();
        const auto* faceDark = uiD.find("home.stat.notes.bg");
        DOCTEST_REQUIRE(faceDark != nullptr);
        DOCTEST_CHECK_MESSAGE(
            faceDark->color.a >= 0.90f,
            "深色档玻璃面应近实白（dark-WeChat 延后期的防塌分支）");
        const auto* tintDark = uiD.find("home.stat.notes.glass.tint");
        DOCTEST_REQUIRE(tintDark != nullptr);
        DOCTEST_CHECK(tintDark->color.a >= 0.10f);  // 染色层照常渲染
        core::theme::setLight();                   // 尾态复位
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
        DOCTEST_CHECK(ui.find("home.api.chart.empty") != nullptr);
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
