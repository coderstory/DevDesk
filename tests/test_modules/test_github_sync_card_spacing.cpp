// tests/test_modules/test_github_sync_card_spacing.cpp
// H28（2026-10-28 两全拍板）双档护栏：
//   ① 关键行间距 ≥ 设计系统 v1.0 间距令牌（4 超小 / 8 小 / 12 中间 / 16 常规）；
//   ② 最小窗 1000×640@100% 档零滚动（UAT6 用户铁律延续）。
//
// 断言写法沿用 test_github_sync_card.cpp 的约定：frame 几何断言必须 raw
// eui::Ui + ui.layout()（HeadlessFrame compose 回调体内取 frame 全是 0）。
// 间距断言用「实际间隙 ≥ 令牌值」而非行 y 精确值——本文件锁设计系统节奏
// 契约，行 y 精确公式由 plan_a_geometry 用例钉。折叠链接与摘要同行使披露区
// 恒 1 行：链接行与启用行不得重叠（S3.7#4 旧独立行未记账行序、与启用行
// 重叠的反回归钉）。
//
// 最小窗无滚动判据：S4.2 后卡区可用高 cardsH =
// 页面高(640−标题条 40=600) − cardsTopY(32+40+16+40+16=144) − bottomHint
// roundFs(20) − spacingLarge 16 = 420（与 settings_module.cpp cardsH 同式实算）。
// 卡区是 scrollView 视口，单卡内容高 = 卡高（column gap 只算行间，日志 tab
//「恰好填满零滚动」同式先例）——roundFs(kGithubSyncCardHeight) ≤ 420 即 100%
// 档零滚动；最坏行序内容底 + 下边距 16 不越卡由本文件最小窗档用例与
// test_github_sync_card.cpp 的 worst_case 用例（1520 宽档）双钉。
#include "doctest.h"

#include "github_sync_card.h"
#include "github_sync_config.h"
#include "settings_ui_cards.h"
#include "settings_ui_logic.h"

#include <core/theme.h>

#include <eui/dsl.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

namespace ms = module::settings;

constexpr const char* kCardId = "settings.card.github_sync";

// 最小窗（1000×640、100% 档）卡区可用高——推导见文件头。
constexpr float kMinWindowCardsH = 420.0f;

// 卡片夹具（只读入参 + 注入回调全部本文件构造，零真网零真线程；与
// test_github_sync_card.cpp 的 CardFixture 同模，本文件只需几何断言）。
struct CardFixture {
    ms::SettingsConfig cfg;
    std::string proxyPortDraft;
    std::vector<std::string> logTail;
    ms::SettingsCardsIn in;
    ms::SettingsCardActions a;

    CardFixture() : in{cfg, proxyPortDraft, logTail} {
        in.githubSyncEnabled = true;   // 默认启用态
    }

    void draw(eui::Ui& ui, float w) {
        ms::CardDrawArgs args{ui, kCardId, w,
                              ms::roundFs(ms::kGithubSyncCardHeight),
                              core::theme::current(), 0.0f, 1000.0f, in, a};
        ms::drawGithubSyncCard(args);
    }
};

} // namespace

REGISTER_TEST(github_sync_card_spacing_rhythm,
              "github_sync H28: 关键行间距 ≥ 设计系统 v1.0 令牌（4/8/12/16）+ 折叠链接并入摘要行不重叠", {
    CardFixture fx;
    fx.in.githubSyncRepoPublic = true;           // 造两条信息行
    fx.in.githubSyncLocalGitUnavailable = true;
    eui::Ui ui;
    fx.draw(ui, 1520.0f);
    ui.layout(1600.0f, 1000.0f);   // frame 断言必须真 layout
    const auto* title = ui.find(std::string(kCardId) + ".title");
    const auto* summary = ui.find(std::string(kCardId) + ".summary");
    const auto* toggle = ui.find(std::string(kCardId) + ".summary.toggle");
    const auto* enSw = ui.find(std::string(kCardId) + ".enabled.sw");
    const auto* autoSw = ui.find(std::string(kCardId) + ".autopush.sw");
    const auto* divHead = ui.find(std::string(kCardId) + ".div.head");
    const auto* url = ui.find(std::string(kCardId) + ".url.input");
    const auto* br = ui.find(std::string(kCardId) + ".branch.input");
    const auto* divAct = ui.find(std::string(kCardId) + ".div.actions");
    const auto* push = ui.find(std::string(kCardId) + ".btn.push.bg");
    const auto* pub = ui.find(std::string(kCardId) + ".publicwarn");
    const auto* git = ui.find(std::string(kCardId) + ".localgit");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_REQUIRE(summary != nullptr);
    DOCTEST_REQUIRE(toggle != nullptr);
    DOCTEST_REQUIRE(enSw != nullptr);
    DOCTEST_REQUIRE(autoSw != nullptr);
    DOCTEST_REQUIRE(divHead != nullptr);
    DOCTEST_REQUIRE(url != nullptr);
    DOCTEST_REQUIRE(br != nullptr);
    DOCTEST_REQUIRE(divAct != nullptr);
    DOCTEST_REQUIRE(push != nullptr);
    DOCTEST_REQUIRE(pub != nullptr);
    DOCTEST_REQUIRE(git != nullptr);

    const float gapOk = 0.01f;   // 100% 档 roundFs 恒等，容差只挡浮点尾差

    // 卡上留白 = 16（常规间距令牌：卡片内边距）
    DOCTEST_CHECK(std::fabs(title->frame.y - 16.0f) < 0.5f);
    // 标题 → 摘要 ≥ 4（超小间距令牌）
    DOCTEST_CHECK(summary->frame.y - (title->frame.y + title->frame.height) >=
                  4.0f - gapOk);
    // 折叠链接并入摘要行（H28）：同行、右缘 = 卡内容右缘 w−28（与错误行
    //「查看详情」同锚），且链接底不得越进启用行——S3.7#4 旧独立行与启用行
    // 重叠的反回归钉
    DOCTEST_CHECK(std::fabs(toggle->frame.y - summary->frame.y) < 0.5f);
    DOCTEST_CHECK(std::fabs(toggle->frame.x + toggle->frame.width -
                            (1520.0f - 28.0f)) < 0.5f);
    DOCTEST_CHECK(toggle->frame.y + toggle->frame.height <=
                  enSw->frame.y + gapOk);
    // 摘要 → 启用行 / 启用 → 自动推送 ≥ 8（小间距令牌；100% 档开关顶 = 行顶）
    DOCTEST_CHECK(enSw->frame.y - (summary->frame.y + summary->frame.height) >=
                  8.0f - gapOk);
    DOCTEST_CHECK(autoSw->frame.y - (enSw->frame.y + enSw->frame.height) >=
                  8.0f - gapOk);
    // 分段线上下留白 ≥ 12（中间距令牌）
    DOCTEST_CHECK(divHead->frame.y - (autoSw->frame.y + autoSw->frame.height) >=
                  12.0f - gapOk);
    DOCTEST_CHECK(url->frame.y - (divHead->frame.y + divHead->frame.height) >=
                  12.0f - gapOk);
    // URL 行 → 分支|Token 行 ≥ 8（小间距令牌）
    DOCTEST_CHECK(br->frame.y - (url->frame.y + url->frame.height) >=
                  8.0f - gapOk);
    // 分支行底 → 第二分段线 ≥ 12；分段线 → 按钮行 ≥ 12（中间距令牌）
    DOCTEST_CHECK(divAct->frame.y - (br->frame.y + br->frame.height) >=
                  12.0f - gapOk);
    DOCTEST_CHECK(push->frame.y - (divAct->frame.y + divAct->frame.height) >=
                  12.0f - gapOk);
    // 按钮行 → 首条信息行 ≥ 8（小间距令牌）；信息行间 ≥ 4（超小间距令牌）
    DOCTEST_CHECK(pub->frame.y - (push->frame.y + push->frame.height) >=
                  8.0f - gapOk);
    DOCTEST_CHECK(git->frame.y - (pub->frame.y + pub->frame.height) >=
                  4.0f - gapOk);
    return true;
})

REGISTER_TEST(github_sync_card_min_window_no_scroll,
              "github_sync H28: 最小窗 1000×640@100% 卡高 ≤ 可用高 420（零滚动）+ 最坏行序不越卡", {
    // ① 卡高护栏：roundFs(kGithubSyncCardHeight) ≤ 最小窗卡区可用高
    //（kMinWindowCardsH，S4.2 式推导见文件头）。卡高超可用高即出滚动条——
    // UAT6 用户铁律「同步 tab 出滚动条完全没必要」的 H28 延续。
    DOCTEST_CHECK_MESSAGE(
        ms::roundFs(ms::kGithubSyncCardHeight) <= kMinWindowCardsH + 0.5f,
        "卡高 " + std::to_string(ms::roundFs(ms::kGithubSyncCardHeight)) +
            " 超出最小窗卡区可用高 " + std::to_string(kMinWindowCardsH) +
            "（1000×640 将出滚动条）");

    // ② 最坏行序（五条信息行全显 + 空闲态折叠链接在场）内容底 + 下边距 16
    // 不越卡——与 test_github_sync_card.cpp 的 worst_case 用例同口径，本处
    // 卡宽取最小窗 1000（卡 728）钉「最小窗」档。
    CardFixture fx;
    fx.in.githubSyncRepoPublic = true;
    fx.in.githubSyncBranchHint = "分支 main 不存在，已按仓库默认分支自动创建";
    fx.in.githubSyncLocalGitUnavailable = true;
    fx.in.githubSyncUnsyncedNotice = true;
    fx.in.githubSyncDeleteSkipHint =
        "有 1 篇本地仍在但读取失败的笔记，已跳过远端删除（下轮重试）";
    eui::Ui ui;
    fx.draw(ui, 1000.0f);
    ui.layout(1000.0f, 640.0f);
    const auto* card = ui.find(kCardId);
    const auto* last = ui.find(std::string(kCardId) + ".deleteskip");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(last != nullptr);
    DOCTEST_CHECK_MESSAGE(
        last->frame.y + last->frame.height + 16.0f <= card->frame.height + 0.5f,
        "最坏行序内容底 " +
            std::to_string(last->frame.y + last->frame.height) +
            " + 下边距 16 越出卡高 " + std::to_string(card->frame.height));
    return true;
})
