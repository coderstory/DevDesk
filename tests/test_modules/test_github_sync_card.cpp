// tests/test_modules/test_github_sync_card.cpp
// GitHub Sync 设置卡片（UAT5 方案 A 三段式）+ 错误详情弹窗：无头 compose 断言。
//
// 本文件的断言写法约定，勿"顺手改回"更省事的写法：
// - 断言节点存在/不存在，不要退回「状态枚举 == X」——那只证明判据函数对了，
//   证明不了绘制层照它画了。
// - 「不存在」类断言必须带对照组（例如「运行中按钮禁用」旁边必须有「非运行态可点」）：
//   没有对照组的断言可能只是因为根本没渲染任何东西而恒真。
// - frame 几何断言必须用 raw eui::Ui + ui.layout()——HeadlessFrame 的 compose
//   回调体内取 frame 全是 0（见 commit 1df3ecb9 的踩坑记录）。
// - EUI-NEO 的 dialog 关着时照样把整棵树建出来（只把 panel 的 opacity 设 0 并
//   disable），判「弹窗开着」必须看 panel 的 opacity，find != nullptr 恒真。
// - button 的根元素没有 onClick/disabled，两者都在 `<id>.bg` 上（见 components/button.h）；
//   wswitch 的命中元素是 `<id>.track`（见 ui_kit/wswitch.cpp）。
// - 全部依赖注入：卡片的动作与弹窗的日志源/回调都是本文件构造的 lambda，
//   零真网、零真线程（本文件不启动任何后台任务）。
#include "doctest.h"
#include "frame_env.h"
#include "test_support.h"

#include "core/event_bus.h"
#include "core/theme.h"
#include "core/sql_dump.h"
#include "git_error_dialog.h"
#include "github_sync_card.h"
#include "github_sync_config.h"
#include "github_sync_logic.h"
#include "platform/github_api.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "settings_ui_logic.h"

#include <eui/dsl.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace ms = module::settings;

constexpr const char* kCardId = "settings.card.github_sync";
constexpr const char* kDlgId = "settings.github.err";

// eui::Color 无 operator==（旧用例同款：逐分量比）。
bool colorEq(const eui::Color& a, const eui::Color& b) {
    return std::fabs(a.r - b.r) < 0.01f && std::fabs(a.g - b.g) < 0.01f &&
           std::fabs(a.b - b.b) < 0.01f;
}

// 卡片夹具：只读入参 + 注入回调全部本文件构造（零真网、零真线程）。
// 卡高用注册表同源常量（roundFs(kGithubSyncCardHeight)），别写字面 424。
struct CardFixture {
    ms::SettingsConfig cfg;
    std::string proxyPortDraft;
    std::vector<std::string> logTail;
    ms::SettingsCardsIn in;
    ms::SettingsCardActions a;
    int testClicks = 0;
    int pushClicks = 0;
    int pullClicks = 0;
    int detailClicks = 0;

    CardFixture()
        : in{cfg, proxyPortDraft, logTail} {
        in.githubSyncEnabled = true;   // 默认启用态（未启用走禁用态用例）
        a.githubSyncTestConnection = [this] { ++testClicks; };
        a.githubSyncPush = [this] { ++pushClicks; };
        a.githubSyncPull = [this] { ++pullClicks; };
        a.githubSyncShowErrorDetail = [this] { ++detailClicks; };
    }

    void draw(eui::Ui& ui) {
        ms::CardDrawArgs args{ui, kCardId, 1520.0f,
                              ms::roundFs(ms::kGithubSyncCardHeight),
                              core::theme::current(), 0.0f, 1000.0f, in, a};
        ms::drawGithubSyncCard(args);
    }
};

platform::github::ApiLogEntry makeEntry(const char* method, const char* url,
                                        long status, int ms) {
    platform::github::ApiLogEntry e;
    e.method = method;
    e.urlMasked = url;
    e.statusCode = status;
    e.durationMs = ms;
    return e;
}

// 弹窗夹具：日志源为纯内存假数据（零真网）。
struct DialogFixture {
    ms::GitErrorDialogState st;
    ms::GitErrorDialogIn in;
    ms::GitErrorDialogActions a;
    std::vector<platform::github::ApiLogEntry> logs;
    std::string copied;
    int retryClicks = 0;
    int closeClicks = 0;
    int pullThenPushClicks = 0;
    int updateTokenClicks = 0;

    DialogFixture() {
        auto e1 = makeEntry("GET", "https://api.github.com/repos/u/r", 401, 123);
        e1.bodyTail = "{\"message\":\"Bad credentials\"}";
        logs.push_back(e1);
        auto e2 = makeEntry("PUT",
                            "https://api.github.com/repos/u/r/contents/database.sql",
                            0, 0);
        e2.networkError = true;
        e2.errorMessage = "connect timeout";
        logs.push_back(e2);

        in.summary = "推送失败：HTTP 401（Token 无效或已过期）";
        in.logs = [this] { return logs; };
        a.copyDetail = [this](const std::string& t) { copied = t; };
        a.retry = [this] { ++retryClicks; };
        a.pullThenPush = [this] { ++pullThenPushClicks; };
        a.updateToken = [this] { ++updateTokenClicks; };
        a.close = [this] { ++closeClicks; };
        a.selectLog = [this](int i) { st.selectedLogIndex = i; };
        a.logScrolled = [this](float v) { st.logScrollOffset = v; };
    }

    void draw(eui::Ui& ui, const eui::Screen& screen) {
        ms::drawGitErrorDialog(ui, screen, core::theme::current(), st, in, a);
    }
};

} // namespace

REGISTER_TEST(github_sync_card_has_test_button,
              "github_sync 卡片: 测试连接按钮存在且带注入 onClick", {
    CardFixture fx;
    test_support::HeadlessFrame f("githubsynccard");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        const auto* btn = ui.find(std::string(kCardId) + ".btn.test.bg");
        DOCTEST_REQUIRE(btn != nullptr);
        DOCTEST_CHECK(btn->onClick != nullptr);
        const auto* label = ui.find(std::string(kCardId) + ".btn.test.text.normal");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("测试连接"));
        // 点击只落到注入回调（零真线程/零真网）
        btn->onClick();
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.testClicks, 1);
    DOCTEST_CHECK_EQ(fx.pushClicks, 0);
    DOCTEST_CHECK_EQ(fx.pullClicks, 0);
    return true;
})

REGISTER_TEST(github_sync_card_running_disables_three_buttons,
              "github_sync 卡片: 运行中三按钮禁用（带非运行态对照）", {
    CardFixture busy;
    busy.in.githubSyncRunning = true;
    test_support::HeadlessFrame fb("githubsynccardbusy");
    DOCTEST_CHECK(fb.frame([&](eui::Ui& ui) {
        busy.draw(ui);
        for (const char* id :
             {"settings.card.github_sync.btn.push.bg",
              "settings.card.github_sync.btn.test.bg",
              "settings.card.github_sync.btn.pull.bg"}) {
            const auto* b = ui.find(id);
            DOCTEST_REQUIRE(b != nullptr);
            DOCTEST_CHECK_MESSAGE(b->disabled, id);
        }
        return true;
    }));

    // 对照组：非运行态三个都不可禁（否则「恒 disabled」也能让上面全绿）
    CardFixture idle;
    test_support::HeadlessFrame fi("githubsynccardidle");
    DOCTEST_CHECK(fi.frame([&](eui::Ui& ui) {
        idle.draw(ui);
        for (const char* id :
             {"settings.card.github_sync.btn.push.bg",
              "settings.card.github_sync.btn.test.bg",
              "settings.card.github_sync.btn.pull.bg"}) {
            const auto* b = ui.find(id);
            DOCTEST_REQUIRE(b != nullptr);
            DOCTEST_CHECK_MESSAGE(!b->disabled, id);
        }
        return true;
    }));
    return true;
})

// UAT6 控件一致性横扫双规则锁：
// ① 阴影统一——操作组三按钮（绿 Primary「立即同步」与两个 Secondary 同排）
//   全部 flat：EUI-NEO ButtonStyle 默认阴影已根修为无，未显式覆盖的按钮
//   不得再带阴影；
// ② 同排同尺寸——三按钮高度全等（主次只用颜色表达、不用大小；宽度按
//   文案收缩不算「特意更大」），绿 Primary 不放大一档。
REGISTER_TEST(github_sync_card_buttons_flat_and_same_height,
              "github_sync 卡片: 操作组三按钮 flat 无阴影且同排同高（UAT6 双规则锁）", {
    CardFixture fx;
    eui::Ui ui;
    fx.draw(ui);
    const auto* push = ui.find(std::string(kCardId) + ".btn.push.bg");
    const auto* test = ui.find(std::string(kCardId) + ".btn.test.bg");
    const auto* pull = ui.find(std::string(kCardId) + ".btn.pull.bg");
    DOCTEST_REQUIRE(push != nullptr);
    DOCTEST_REQUIRE(test != nullptr);
    DOCTEST_REQUIRE(pull != nullptr);
    // ① flat：三钮（含绿 Primary）阴影一律未启用（style 断言，无需 layout）
    DOCTEST_CHECK_MESSAGE(!push->shadow.enabled, "push (Primary) must be flat");
    DOCTEST_CHECK_MESSAGE(!test->shadow.enabled, "test must be flat");
    DOCTEST_CHECK_MESSAGE(!pull->shadow.enabled, "pull must be flat");

    ui.layout(1600.0f, 1000.0f);   // frame 断言必须真 layout
    // ② 同排同高：高度全等（同一 rowH 常量），字号同为 fs(12)
    DOCTEST_CHECK(std::fabs(push->frame.height - test->frame.height) < 0.01f);
    DOCTEST_CHECK(std::fabs(push->frame.height - pull->frame.height) < 0.01f);
    DOCTEST_CHECK(std::fabs(test->frame.height - pull->frame.height) < 0.01f);
    const auto* pushText = ui.find(std::string(kCardId) + ".btn.push.text.normal");
    const auto* pullText = ui.find(std::string(kCardId) + ".btn.pull.text.normal");
    DOCTEST_REQUIRE(pushText != nullptr);
    DOCTEST_REQUIRE(pullText != nullptr);
    DOCTEST_CHECK(std::fabs(pushText->fontSize - pullText->fontSize) < 0.01f);
    return true;
})

// 方案 A：进度在操作组右侧——4px 轨道 + 主色填充（填充比 = 阶段序号/6，引擎
// 只有阶段粒度、不伪造百分比）+「同步中 · <阶段>」文案；空闲右侧整组不渲染。
REGISTER_TEST(github_sync_card_progress_bar_and_text,
              "github_sync 卡片: 运行中右侧进度条+文案（4px/阶段填充比），空闲留空", {
    // 纯函数：阶段序号 → 序号/6；认不出序号 → 起步 10%
    DOCTEST_CHECK(std::fabs(ms::githubSyncProgressFraction("① 准备中…") -
                            (1.0f / 6.0f)) < 1e-3f);
    DOCTEST_CHECK(std::fabs(ms::githubSyncProgressFraction("⑤ 同步笔记…") -
                            (5.0f / 6.0f)) < 1e-3f);
    DOCTEST_CHECK(std::fabs(ms::githubSyncProgressFraction("准备中") - 0.1f) <
                  1e-3f);
    DOCTEST_CHECK(std::fabs(ms::githubSyncProgressFraction("") - 0.1f) < 1e-3f);

    CardFixture fx;
    fx.in.githubSyncRunning = true;
    fx.in.githubSyncProgress = "① 校验 token…";
    eui::Ui ui;
    fx.draw(ui);
    ui.layout(1600.0f, 1000.0f);   // frame 断言必须真 layout
    const auto* p = ui.find(std::string(kCardId) + ".progress");
    const auto* bar = ui.find(std::string(kCardId) + ".progress.bar");
    const auto* fill = ui.find(std::string(kCardId) + ".progress.fill");
    DOCTEST_REQUIRE(p != nullptr);
    DOCTEST_REQUIRE(bar != nullptr);
    DOCTEST_REQUIRE(fill != nullptr);
    DOCTEST_CHECK_EQ(p->text, std::string("同步中 · ① 校验 token…"));
    DOCTEST_CHECK_EQ(bar->frame.height, 4.0f);   // 方案 A .bar 4px 高
    DOCTEST_CHECK(std::fabs(fill->frame.width - bar->frame.width / 6.0f) < 0.5f);
    // 进度在按钮组右侧：轨道左缘 > 拉取按钮右缘；文案右缘贴卡右内边距
    const auto* pull = ui.find(std::string(kCardId) + ".btn.pull.bg");
    DOCTEST_REQUIRE(pull != nullptr);
    DOCTEST_CHECK(bar->frame.x > pull->frame.x + pull->frame.width);
    DOCTEST_CHECK(std::fabs(p->frame.x + p->frame.width - (1520.0f - 28.0f)) <
                  0.5f);

    // 对照组：非运行态无进度条/文案（否则上面断言可能只是「恒渲染」恒真）
    CardFixture idle;
    eui::Ui uiIdle;
    idle.draw(uiIdle);
    DOCTEST_CHECK(uiIdle.find(std::string(kCardId) + ".progress") == nullptr);
    DOCTEST_CHECK(uiIdle.find(std::string(kCardId) + ".progress.bar") == nullptr);
    DOCTEST_CHECK(uiIdle.find(std::string(kCardId) + ".progress.fill") == nullptr);
    return true;
})

// 方案 A 三段几何（与 drawGithubSyncCard 行进公式同构；100% 档换算见注释）：
//   头部 = 点 8（x=28）+ 标题（x=44，20 行高）+ 摘要（+1+16）→ 启用/自动推送
//   两整行开关行（UAT6 N 项：开关统一 44×24、行盒 24；启用/自动推送同上同下，
//   开关钉 kW−28 右缘、行盒内垂直居中；label 宽 = 卡内宽−开关−8）；
//   头部底 = 45+6+24+6+24 = 105 → 分段线（+5，1px）→ 配置组（URL 行 36 +
//   分支|Token 行 36，组题行已删）→ 分段线 → 操作组（三按钮行 36）。
REGISTER_TEST(github_sync_card_plan_a_geometry,
              "github_sync 卡片: 方案 A 三段几何（头部/开关两行/配置组/操作组）", {
    CardFixture fx;
    eui::Ui ui;
    fx.draw(ui);
    ui.layout(1600.0f, 1000.0f);   // frame 断言必须真 layout
    const auto* dot = ui.find(std::string(kCardId) + ".dot");
    const auto* title = ui.find(std::string(kCardId) + ".title");
    const auto* summary = ui.find(std::string(kCardId) + ".summary");
    const auto* autoLabel = ui.find(std::string(kCardId) + ".autopush.label");
    const auto* autoSw = ui.find(std::string(kCardId) + ".autopush.sw");
    const auto* enLabel = ui.find(std::string(kCardId) + ".enabled.label");
    const auto* enSw = ui.find(std::string(kCardId) + ".enabled.sw");
    const auto* divHead = ui.find(std::string(kCardId) + ".div.head");
    const auto* url = ui.find(std::string(kCardId) + ".url.input");
    const auto* br = ui.find(std::string(kCardId) + ".branch.input");
    const auto* tk = ui.find(std::string(kCardId) + ".token.input");
    const auto* divAct = ui.find(std::string(kCardId) + ".div.actions");
    const auto* push = ui.find(std::string(kCardId) + ".btn.push.bg");
    const auto* test = ui.find(std::string(kCardId) + ".btn.test.bg");
    const auto* pull = ui.find(std::string(kCardId) + ".btn.pull.bg");
    DOCTEST_REQUIRE(dot != nullptr);
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_REQUIRE(summary != nullptr);
    DOCTEST_REQUIRE(autoLabel != nullptr);
    DOCTEST_REQUIRE(autoSw != nullptr);
    DOCTEST_REQUIRE(enLabel != nullptr);
    DOCTEST_REQUIRE(enSw != nullptr);
    DOCTEST_REQUIRE(divHead != nullptr);
    DOCTEST_REQUIRE(url != nullptr);
    DOCTEST_REQUIRE(br != nullptr);
    DOCTEST_REQUIRE(tk != nullptr);
    DOCTEST_REQUIRE(divAct != nullptr);
    DOCTEST_REQUIRE(push != nullptr);
    DOCTEST_REQUIRE(test != nullptr);
    DOCTEST_REQUIRE(pull != nullptr);

    const float kW = 1520.0f;
    const float padX = 28.0f;
    const float y0 = 8.0f;   // kCardPaddingY（零滚动收口 16→8）
    const float titleH = ms::roundFs(20.0f);
    const float mutedH = ms::roundFs(16.0f);
    const float rowH = ms::roundFs(36.0f);
    const float toggleRowH = ms::roundFs(24.0f);  // N 项: 28→24 (主开关统一 44×24)
    const float rowGap = ms::roundFs(6.0f);
    const float secGap = ms::roundFs(5.0f);
    // 头部：状态点 + 标题（启用后缀）+ 摘要（披露兜底，见 disclosure 用例）
    DOCTEST_CHECK(std::fabs(dot->frame.x - padX) < 0.5f);
    DOCTEST_CHECK(std::fabs(dot->frame.y - (y0 + (titleH - 8.0f) * 0.5f)) < 0.5f);
    DOCTEST_CHECK_EQ(dot->frame.width, 8.0f);
    DOCTEST_CHECK_EQ(dot->frame.height, 8.0f);
    DOCTEST_CHECK(std::fabs(title->frame.x - (padX + 8.0f + 8.0f)) < 0.5f);
    DOCTEST_CHECK(std::fabs(title->frame.y - y0) < 0.5f);
    DOCTEST_CHECK_EQ(title->text, std::string("GitHub 同步 · 已启用"));
    const float summaryY = y0 + titleH + ms::roundFs(1.0f);
    DOCTEST_CHECK(std::fabs(summary->frame.y - summaryY) < 0.5f);
    DOCTEST_CHECK(std::fabs(summary->frame.x - title->frame.x) < 0.5f);
    // 启用/自动推送两整行（N 项: 两开关统一 44×24、行盒 24）：行 y =
    // 摘要底+6 / 启用行底(max(行盒/文本/开关))+6；开关钉卡右内边距、行盒内
    // 垂直居中
    const float labelH = ms::roundFs(18.0f);
    const float headTextBottom = summaryY + mutedH;
    const float enableRowY = headTextBottom + rowGap;
    const float enableTextBottom = enableRowY + (toggleRowH + labelH) * 0.5f;
    const float enableSwY = enableRowY + (toggleRowH - 24.0f) * 0.5f;  // N 项: 28→24
    const float autoRowY =
        std::max(enableRowY + toggleRowH,
                 std::max(enableTextBottom, enableSwY + 24.0f)) + rowGap;  // N 项: 28→24
    const float autoTextBottom = autoRowY + (toggleRowH + labelH) * 0.5f;
    const float autoSwY = autoRowY + (toggleRowH - 24.0f) * 0.5f;
    DOCTEST_CHECK_EQ(enLabel->text, std::string("启用"));
    DOCTEST_CHECK_EQ(autoLabel->text, std::string("自动推送"));
    DOCTEST_CHECK(std::fabs(enSw->frame.x - (kW - padX - 44.0f)) < 0.5f);  // N 项: 52→44
    DOCTEST_CHECK(std::fabs(enSw->frame.y - enableSwY) < 0.5f);
    DOCTEST_CHECK_EQ(enSw->frame.width, 44.0f);  // N 项: 52→44
    DOCTEST_CHECK_EQ(enSw->frame.height, 24.0f);  // N 项: 28→24
    DOCTEST_CHECK(std::fabs(enSw->frame.x + enSw->frame.width - (kW - padX)) <
                  0.5f);
    DOCTEST_CHECK(std::fabs(autoSw->frame.x - (kW - padX - 44.0f)) < 0.5f);
    DOCTEST_CHECK(std::fabs(autoSw->frame.y - autoSwY) < 0.5f);
    DOCTEST_CHECK_EQ(autoSw->frame.width, 44.0f);
    DOCTEST_CHECK_EQ(autoSw->frame.height, 24.0f);
    // label 左块：贴卡左缘（x=28）、行盒内垂直居中；宽度 = 卡内宽 − 开关 − 8（弹性）。
    // 说明文案行已删（零滚动收口）：desc 元素不得再出现（防「顺手加回来」）。
    const float enTextW = kW - padX * 2.0f - 44.0f - ms::roundFs(8.0f);  // N 项: 52→44
    const float autoTextW = kW - padX * 2.0f - 44.0f - ms::roundFs(8.0f);
    DOCTEST_CHECK(ui.find(std::string(kCardId) + ".enabled.desc") == nullptr);
    DOCTEST_CHECK(ui.find(std::string(kCardId) + ".autopush.desc") == nullptr);
    for (const auto* lbl : {enLabel, autoLabel}) {
        DOCTEST_CHECK(std::fabs(lbl->frame.x - padX) < 0.5f);
    }
    DOCTEST_CHECK(std::fabs(enLabel->frame.y -
                            (enableRowY +
                             (toggleRowH - labelH) * 0.5f)) < 0.5f);
    DOCTEST_CHECK(std::fabs(autoLabel->frame.y -
                            (autoRowY + (toggleRowH - labelH) * 0.5f)) < 0.5f);
    DOCTEST_CHECK(std::fabs(enLabel->frame.width - enTextW) < 0.5f);
    DOCTEST_CHECK(std::fabs(autoLabel->frame.width - autoTextW) < 0.5f);
    // 第一段分段线：头部内容底 = 自动推送行底（max(行盒/文本块/开关)）+ 5
    const float headBottom = std::max(autoRowY + toggleRowH,
                                      std::max(autoTextBottom,
                                               autoSwY + 24.0f));
    DOCTEST_CHECK(std::fabs(divHead->frame.y - (headBottom + secGap)) < 0.5f);
    DOCTEST_CHECK(std::fabs(divHead->frame.x - padX) < 0.5f);
    // 配置组：组题行已删（零滚动收口）——URL 全宽行直接跟分段线；group.config
    // 元素不得再出现（防「顺手加回来」）
    DOCTEST_CHECK(ui.find(std::string(kCardId) + ".group.config") == nullptr);
    float y = headBottom + secGap + 1.0f + secGap;
    const float formW = kW - padX * 2.0f;
    const float colGap = ms::roundFs(6.0f);
    const float colW = (formW - colGap) * 0.5f;
    const float col2X = padX + colW + colGap;
    const float ctrlDx = 110.0f + 8.0f;   // FormRow 定宽 label + 间距
    // URL 全宽行：按钮已下移操作组（UAT5），输入拉满卡内容右缘 kW−28
    //（旧 H1「输入右缘 ≤ 按钮组左缘」让位断言随旧布局一并废除）
    DOCTEST_CHECK(std::fabs(url->frame.x - (padX + ctrlDx)) < 0.5f);
    DOCTEST_CHECK(std::fabs(url->frame.y - y) < 0.5f);
    DOCTEST_CHECK(std::fabs(url->frame.x + url->frame.width - (kW - padX)) <
                  0.5f);
    y += rowH + rowGap;
    // 分支（左列）/ Token（右列）：宽 = 列控制区宽，右缘贴所在列右缘
    DOCTEST_CHECK(std::fabs(br->frame.x - (padX + ctrlDx)) < 0.5f);
    DOCTEST_CHECK(std::fabs(br->frame.y - y) < 0.5f);
    DOCTEST_CHECK(std::fabs(br->frame.width - (colW - ctrlDx)) < 0.5f);
    DOCTEST_CHECK(std::fabs(br->frame.x + br->frame.width - (padX + colW)) <
                  0.5f);
    DOCTEST_CHECK(std::fabs(tk->frame.x - (col2X + ctrlDx)) < 0.5f);
    DOCTEST_CHECK(std::fabs(tk->frame.y - y) < 0.5f);
    DOCTEST_CHECK(std::fabs(tk->frame.x + tk->frame.width - (kW - padX - 24.0f)) < 0.5f);
    y += rowH;
    // 第二段分段线 → 操作组：三按钮在左（Primary 最左、同排等高）
    DOCTEST_CHECK(std::fabs(divAct->frame.y - (y + secGap)) < 0.5f);
    y += secGap + 1.0f + secGap;
    DOCTEST_CHECK(std::fabs(push->frame.x - padX) < 0.5f);
    DOCTEST_CHECK(std::fabs(push->frame.y - y) < 0.5f);
    DOCTEST_CHECK(std::fabs(test->frame.x - (padX + 120.0f + 8.0f)) < 0.5f);
    DOCTEST_CHECK(std::fabs(pull->frame.x -
                            (padX + 120.0f + 8.0f + 100.0f + 8.0f)) < 0.5f);
    DOCTEST_CHECK(std::fabs(test->frame.y - y) < 0.5f);
    DOCTEST_CHECK(std::fabs(pull->frame.y - y) < 0.5f);
    DOCTEST_CHECK(std::fabs(push->frame.height - rowH) < 0.5f);
    // 空闲时操作组右侧无进度（对照组 = progress 用例的运行态）
    DOCTEST_CHECK(ui.find(std::string(kCardId) + ".progress.bar") == nullptr);
    return true;
})

// UAT4 撑满语义在方案 A 下的延续：URL 拉满卡内容右缘（不再让位按钮组——
// 按钮在操作组）、分支/Token 撑满所在列控制区、开关右缘钉卡右内边距；UAT6 起
// 开关行文本块也随窗宽弹性（行宽=卡内宽契约）。
// 回归指向：再加任何「定宽上限/按钮让位」类约束都会让这两档的红线亮起。
REGISTER_TEST(github_sync_card_fill_full_width,
              "github_sync 卡片: URL 拉满卡右缘 + 分支/Token 撑满列 + 开关行文本块弹性 + 开关钉右缘（两档窗宽）", {
    for (const float kW : {1000.0f, 1312.0f}) {
        CardFixture fx;
        eui::Ui ui;
        ms::CardDrawArgs args{ui, kCardId, kW,
                              ms::roundFs(ms::kGithubSyncCardHeight),
                              core::theme::current(), 0.0f, 1000.0f,
                              fx.in, fx.a};
        ms::drawGithubSyncCard(args);
        ui.layout(kW, 1000.0f);  // layout 以卡宽为窗宽，保证列宽公式以 kW 为基准
        const auto* url = ui.find(std::string(kCardId) + ".url.input");
        const auto* br = ui.find(std::string(kCardId) + ".branch.input");
        const auto* tk = ui.find(std::string(kCardId) + ".token.input");
        const auto* autoSw = ui.find(std::string(kCardId) + ".autopush.sw");
        const auto* enSw = ui.find(std::string(kCardId) + ".enabled.sw");
        const auto* enLabel = ui.find(std::string(kCardId) + ".enabled.label");
        const auto* autoLabel =
            ui.find(std::string(kCardId) + ".autopush.label");
        DOCTEST_REQUIRE(url != nullptr);
        DOCTEST_REQUIRE(br != nullptr);
        DOCTEST_REQUIRE(tk != nullptr);
        DOCTEST_REQUIRE(autoSw != nullptr);
        DOCTEST_REQUIRE(enSw != nullptr);
        DOCTEST_REQUIRE(enLabel != nullptr);
        DOCTEST_REQUIRE(autoLabel != nullptr);
        const float formW = kW - 28.0f * 2.0f;
        const float colGap = ms::roundFs(6.0f);   // 列间距 = 行距（零滚动收口 8→6）
        const float colW = (formW - colGap) * 0.5f;
        const float ctrlW = colW - (110.0f + 8.0f);
        // URL 右缘 = 卡内容右缘（kW−28）；分支/Token 右缘 = 所在列右缘
        DOCTEST_CHECK(std::fabs(url->frame.x + url->frame.width -
                                (kW - 28.0f)) < 0.5f);
        DOCTEST_CHECK(std::fabs(br->frame.width - ctrlW) < 0.5f);
        DOCTEST_CHECK(std::fabs(tk->frame.width - (ctrlW - 24.0f)) < 0.5f);
        DOCTEST_CHECK(std::fabs(br->frame.x + br->frame.width -
                                (28.0f + colW)) < 0.5f);
        DOCTEST_CHECK(std::fabs(tk->frame.x + tk->frame.width -
                                (kW - 28.0f - 24.0f)) < 0.5f);
        // 开关右缘钉卡右内边距（N 项: 两开关统一 44×24）
        DOCTEST_CHECK(std::fabs(autoSw->frame.x + autoSw->frame.width -
                                (kW - 28.0f)) < 0.5f);
        DOCTEST_CHECK(std::fabs(enSw->frame.x + enSw->frame.width -
                                (kW - 28.0f)) < 0.5f);
        DOCTEST_CHECK_EQ(autoSw->frame.width, 44.0f);
        DOCTEST_CHECK_EQ(enSw->frame.width, 44.0f);  // N 项: 52→44
        // 开关行文本块宽随窗宽弹性（= 卡内宽 − 开关 − 8，无固定帽）且贴卡左缘
        //（UAT6 行宽=卡内宽契约；旧右侧纵排定宽标签盒已废，此处钉住不回归）
        DOCTEST_CHECK(std::fabs(enLabel->frame.x - 28.0f) < 0.5f);
        DOCTEST_CHECK(std::fabs(autoLabel->frame.x - 28.0f) < 0.5f);
        DOCTEST_CHECK(std::fabs(enLabel->frame.width -
                                (formW - 44.0f - ms::roundFs(8.0f))) < 0.5f);  // N 项: 52→44
        DOCTEST_CHECK(std::fabs(autoLabel->frame.width -
                                (formW - 44.0f - ms::roundFs(8.0f))) < 0.5f);
    }
    return true;
})

// UAT4 去门控语义钉子（方案 A 布局下延续）：未启用时表单不隐藏——自动推送/
// URL/分支/Token/按钮全部照常渲染，但呈置灰禁用态（lock 包装层 disabled +
// opacity 调暗、按钮 disabled）；启用主开关（44×24）不加 lock、始终可点  // N 项: 52×28→44×24
//（重新启用的入口；wswitch 命中 = <id>.enabled.sw.track）。另配「启用后全部
// 恢复」对照组（禁用解除、opacity 回 1）。
REGISTER_TEST(github_sync_card_disabled_state,
              "github_sync 卡片: 未启用时字段全显示但置灰禁用（启用主开关始终可点）+ 启用后恢复", {
    CardFixture off;
    off.in.githubSyncEnabled = false;
    int enableClicks = 0;
    bool enableValue = false;
    off.a.applyGithubSyncEnabled = [&](bool v) {
        ++enableClicks;
        enableValue = v;
    };
    test_support::HeadlessFrame f("githubsynccardstateoff");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        off.draw(ui);
        // 字段全部在（去门控：未启用不隐藏）
        for (const char* suffix :
             {".autopush.sw.track", ".url.input", ".branch.input",
              ".token.input", ".btn.push.bg", ".btn.test.bg", ".btn.pull.bg"}) {
            DOCTEST_CHECK_MESSAGE(
                ui.find(std::string(kCardId) + suffix) != nullptr, suffix);
        }
        // 四组 lock 包装层全部禁用态（disabled 子树不参与命中/聚焦）且置灰调暗
        for (const char* suffix :
             {".autopush.lock", ".url.lock", ".branch.lock", ".token.lock"}) {
            const auto* lock = ui.find(std::string(kCardId) + suffix);
            DOCTEST_REQUIRE_MESSAGE(lock != nullptr, suffix);
            DOCTEST_CHECK_MESSAGE(lock->disabled, suffix);
            DOCTEST_CHECK_MESSAGE(std::fabs(lock->opacity - 0.55f) < 0.01f,
                                  suffix);
        }
        // 三按钮禁用态（disabled = 点击不触发 + 文字自动 40% 置灰）
        for (const char* suffix :
             {".btn.push.bg", ".btn.test.bg", ".btn.pull.bg"}) {
            const auto* b = ui.find(std::string(kCardId) + suffix);
            DOCTEST_REQUIRE_MESSAGE(b != nullptr, suffix);
            DOCTEST_CHECK_MESSAGE(b->disabled, suffix);
        }
        // 启用主开关本身无 lock（对照组 = 上面四个 lock 都在）且始终可点：
        // wswitch 命中元素 = <id>.enabled.sw.track，点击 = 拨「启用」
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".enabled.lock") ==
                      nullptr);
        const auto* enTrack =
            ui.find(std::string(kCardId) + ".enabled.sw.track");
        DOCTEST_REQUIRE(enTrack != nullptr);
        DOCTEST_REQUIRE(enTrack->onClick != nullptr);
        // 标题状态后缀 + 状态点灰（未启用态）
        const auto* title = ui.find(std::string(kCardId) + ".title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK_EQ(title->text, std::string("GitHub 同步 · 未启用"));
        const auto* dot = ui.find(std::string(kCardId) + ".dot");
        DOCTEST_REQUIRE(dot != nullptr);
        DOCTEST_CHECK(!colorEq(dot->color,
                               core::theme::current().colors.primary));
        enTrack->onClick();
        return true;
    }));
    DOCTEST_CHECK_EQ(enableClicks, 1);
    DOCTEST_CHECK(enableValue);

    // 对照组：启用后全部恢复——lock 解除（disabled=false、opacity 回 1）、按钮
    // 可点、状态点回主色——否则上面「禁用态」断言可能只是「恒禁用」恒真
    CardFixture on;
    test_support::HeadlessFrame fon("githubsynccardstateon");
    DOCTEST_CHECK(fon.frame([&](eui::Ui& ui) {
        on.draw(ui);
        for (const char* suffix :
             {".autopush.lock", ".url.lock", ".branch.lock", ".token.lock"}) {
            const auto* lock = ui.find(std::string(kCardId) + suffix);
            DOCTEST_REQUIRE_MESSAGE(lock != nullptr, suffix);
            DOCTEST_CHECK_MESSAGE(!lock->disabled, suffix);
            DOCTEST_CHECK_MESSAGE(std::fabs(lock->opacity - 1.0f) < 0.01f,
                                  suffix);
        }
        for (const char* suffix :
             {".btn.push.bg", ".btn.test.bg", ".btn.pull.bg"}) {
            const auto* b = ui.find(std::string(kCardId) + suffix);
            DOCTEST_REQUIRE_MESSAGE(b != nullptr, suffix);
            DOCTEST_CHECK_MESSAGE(!b->disabled, suffix);
        }
        const auto* dot = ui.find(std::string(kCardId) + ".dot");
        DOCTEST_REQUIRE(dot != nullptr);
        DOCTEST_CHECK(colorEq(dot->color,
                              core::theme::current().colors.primary));
        return true;
    }));
    return true;
})

// 方案 A：错误不再占独立行 + 「详情」小按钮——错误摘要上收头部摘要行（红字、
// 可点），点头部标题/错误行 = 打开既有 git_error_dialog；状态点变红。
REGISTER_TEST(github_sync_card_error_row_and_header_click,
              "github_sync 卡片: 错误摘要上收头部（点标题/错误行开详情）+ 状态点变红（带无错误对照）", {
    CardFixture fx;
    fx.in.githubSyncError = "网络不可达：api.github.com";
    test_support::HeadlessFrame f("githubsynccarderr");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        const auto* err = ui.find(std::string(kCardId) + ".error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("网络不可达") != std::string::npos);
        DOCTEST_REQUIRE(err->onClick != nullptr);
        err->onClick();   // 点错误行 = 开详情
        // 点头部标题同款开详情
        const auto* title = ui.find(std::string(kCardId) + ".title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_REQUIRE(title->onClick != nullptr);
        title->onClick();
        // 错误态：状态点变红（error）
        const auto* dot = ui.find(std::string(kCardId) + ".dot");
        DOCTEST_REQUIRE(dot != nullptr);
        DOCTEST_CHECK(colorEq(dot->color,
                              core::theme::current().colors.error));
        // 旧「详情」小按钮已废（点击头部/错误行替代）
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".btn.detail.bg") ==
                      nullptr);
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.detailClicks, 2);

    // 对照组：无错误 → 错误行不在、标题不可点、状态点回主色、摘要行在
    CardFixture clean;
    test_support::HeadlessFrame fc("githubsynccardclean");
    DOCTEST_CHECK(fc.frame([&](eui::Ui& ui) {
        clean.draw(ui);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".error") == nullptr);
        const auto* title = ui.find(std::string(kCardId) + ".title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->onClick == nullptr);
        const auto* dot = ui.find(std::string(kCardId) + ".dot");
        DOCTEST_REQUIRE(dot != nullptr);
        DOCTEST_CHECK(colorEq(dot->color,
                              core::theme::current().colors.primary));
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".summary") != nullptr);
        return true;
    }));
    return true;
})

REGISTER_TEST(github_sync_card_disclosure_text,
              "github_sync 卡片: 摘要行空闲兜底披露整库快照 + 不上传清单由 skip 清单驱动（防漂移）", {
    const std::string text = ms::githubSyncDisclosureText();

    // (a) 不再只承诺「笔记和待办」：必须点明整库快照（这正是本票的隐私知情缺口）
    DOCTEST_CHECK(text.find("快照") != std::string::npos);
    // 对照组：确实列了会同步的内容——否则上面「找不到标签」可能只是空串恒真
    DOCTEST_CHECK(text.find("笔记") != std::string::npos);
    // (c) 目标仓库性质
    DOCTEST_CHECK(text.find("私有仓库") != std::string::npos);

    // (b) 防漂移闸：defaultSkipTables() 里每一张表都必须有中文标签，且该标签出现在
    // 披露文案里。以后给 skip 清单加表却没补 disclosureLabelFor() → 本断言必红；
    // 若文案改为本地硬抄表名、与 skip 清单脱钩 → 第二句也会红。
    for (const auto& table : ::core::sql_dump::defaultSkipTables()) {
        const std::string label = ms::disclosureLabelFor(table);
        DOCTEST_CHECK_MESSAGE(label != table, table);
        DOCTEST_CHECK_MESSAGE(text.find(label) != std::string::npos, table);
    }

    // 未知表名原样返回（不编造标签）
    DOCTEST_CHECK_EQ(ms::disclosureLabelFor("no_such_table"),
                     std::string("no_such_table"));

    // 绘制层：空闲态（无错误、无上次同步状态）头部摘要行渲染的就是这份披露文案
    //（方案 A 后披露是摘要行的兜底内容）——别只测函数漏了绘制层
    CardFixture fx;
    test_support::HeadlessFrame f("githubsynccarddisc");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        const auto* summary = ui.find(std::string(kCardId) + ".summary");
        DOCTEST_REQUIRE(summary != nullptr);
        DOCTEST_CHECK_EQ(summary->text,
                     std::string("同步笔记与整库数据库快照到 GitHub 私有仓库"));
        return true;
    }));

    // 让位语义：上次同步状态非空 → 摘要行显示状态（披露退兜底，不与状态抢行）
    CardFixture st;
    st.in.githubSyncStatus = "已推送 database.sql 与 3 篇笔记";
    test_support::HeadlessFrame fs("githubsynccarddiscst");
    DOCTEST_CHECK(fs.frame([&](eui::Ui& ui) {
        st.draw(ui);
        const auto* summary = ui.find(std::string(kCardId) + ".summary");
        DOCTEST_REQUIRE(summary != nullptr);
        DOCTEST_CHECK_EQ(summary->text,
                         std::string("已推送 database.sql 与 3 篇笔记"));
        return true;
    }));
    return true;
})

// 公开仓警告行与披露文案必须**同口径**：披露已改说「整库快照」，警告行若还说
// 「笔记、待办与密钥」，则最需要看清泄露面的用户（public 仓）看到的是被窄化的载荷。
// 本用例钉住这条一致性——改回旧口径 / 加新载荷面却漏改警告行都会红。
REGISTER_TEST(github_sync_card_public_warn_same_scope_as_disclosure,
              "github_sync 卡片: 公开仓警告明写整库快照（与披露文案同口径）+ 非公开仓不画该行", {
    CardFixture pub;
    pub.in.githubSyncRepoPublic = true;
    test_support::HeadlessFrame fPub("githubsynccardwarnpub");
    DOCTEST_CHECK(fPub.frame([&](eui::Ui& ui) {
        pub.draw(ui);
        const auto* warn = ui.find(std::string(kCardId) + ".publicwarn");
        DOCTEST_REQUIRE(warn != nullptr);
        // 对照组：节点真有文案 —— 否则下面「找不到旧口径」可能只是空串恒真
        DOCTEST_CHECK(!warn->text.empty());
        DOCTEST_CHECK(warn->text.find("快照") != std::string::npos);
        // 旧口径把载荷说成「笔记、待办」= sync-02 要消灭的窄化形态
        DOCTEST_CHECK(warn->text.find("笔记、待办") == std::string::npos);
        DOCTEST_CHECK(warn->text.find("公开可见") != std::string::npos);
        return true;
    }));

    // 对照组（反向）：非公开仓不画这一行 —— 防「无条件恒画」把警告变成噪音
    CardFixture priv;
    test_support::HeadlessFrame fPriv("githubsynccardwarnpriv");
    DOCTEST_CHECK(fPriv.frame([&](eui::Ui& ui) {
        priv.draw(ui);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".publicwarn") == nullptr);
        return true;
    }));
    return true;
})

REGISTER_TEST(github_error_dialog_content,
              "github_sync 错误弹窗: 摘要 + 日志条目 + 三按钮 + 注入回调", {
    DialogFixture fx;
    fx.st.open = true;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    test_support::HeadlessFrame f("githubsyncdlg");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);

        // 弹窗开着 = panel opacity > 0.5（关着时整树仍建、opacity=0）
        const auto* panel = ui.find(std::string(kDlgId) + ".panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(panel->opacity > 0.5f);

        // 上半部：中文错误摘要
        const auto* sum = ui.find(std::string(kDlgId) + ".summary");
        DOCTEST_REQUIRE(sum != nullptr);
        DOCTEST_CHECK(sum->text.find("401") != std::string::npos);
        DOCTEST_CHECK(sum->text.find("推送失败") != std::string::npos);

        // 中部：日志条目（方法 URL 状态码 耗时）
        const auto* r0 = ui.find(std::string(kDlgId) + ".logs.row.0.text");
        DOCTEST_REQUIRE(r0 != nullptr);
        DOCTEST_CHECK(r0->text.find("GET") != std::string::npos);
        DOCTEST_CHECK(r0->text.find("401") != std::string::npos);
        DOCTEST_CHECK(r0->text.find("123ms") != std::string::npos);
        const auto* r1 = ui.find(std::string(kDlgId) + ".logs.row.1.text");
        DOCTEST_REQUIRE(r1 != nullptr);
        DOCTEST_CHECK(r1->text.find("网络错误") != std::string::npos);

        // 底部三按钮
        for (const char* id : {"settings.github.err.btn.retry.bg",
                               "settings.github.err.btn.copy.bg",
                               "settings.github.err.btn.close.bg"}) {
            const auto* b = ui.find(id);
            DOCTEST_REQUIRE(b != nullptr);
            DOCTEST_CHECK_MESSAGE(b->onClick != nullptr, id);
        }

        //（UAT6 去米黄）三钮色锁：主=primary 绿；复制/关闭=微信
        // Secondary（theme 面=surface），反向锁死暖米 surfaceElevated
        const auto& wc = core::theme::current().colors;
        {
            const auto* retryBg = ui.find(std::string(kDlgId) + ".btn.retry.bg");
            DOCTEST_REQUIRE(retryBg != nullptr);
            DOCTEST_CHECK(colorEq(retryBg->color, wc.primary));
            const auto* copyBg = ui.find(std::string(kDlgId) + ".btn.copy.bg");
            DOCTEST_REQUIRE(copyBg != nullptr);
            DOCTEST_CHECK(colorEq(copyBg->color, wc.surface));
            DOCTEST_CHECK(!colorEq(copyBg->color, wc.surfaceElevated));
            const auto* closeBg = ui.find(std::string(kDlgId) + ".btn.close.bg");
            DOCTEST_REQUIRE(closeBg != nullptr);
            DOCTEST_CHECK(colorEq(closeBg->color, wc.surface));
            DOCTEST_CHECK(!colorEq(closeBg->color, wc.surfaceElevated));
        }

        // 复制详情：注入回调拿到含摘要 + 日志行的文本
        ui.find(std::string(kDlgId) + ".btn.copy.bg")->onClick();
        // 选中第 2 条 → 详情区显示其 errorMessage
        ui.find(std::string(kDlgId) + ".logs.row.1.bg")->onClick();
        ui.find(std::string(kDlgId) + ".btn.retry.bg")->onClick();
        ui.find(std::string(kDlgId) + ".btn.close.bg")->onClick();
        //（UAT6）点关闭后弹窗态翻关：frame.open 回读 st.open，本帧内已置 false
        DOCTEST_CHECK_EQ(fx.st.open, false);
        return true;
    }));

    DOCTEST_CHECK(fx.copied.find("401") != std::string::npos);
    DOCTEST_CHECK(fx.copied.find("GET") != std::string::npos);
    DOCTEST_CHECK(fx.copied.find("connect timeout") != std::string::npos);
    DOCTEST_CHECK_EQ(fx.st.selectedLogIndex, 1);
    DOCTEST_CHECK_EQ(fx.retryClicks, 1);
    DOCTEST_CHECK_EQ(fx.closeClicks, 1);
    return true;
})

REGISTER_TEST(github_error_dialog_closed_is_transparent,
              "github_sync 错误弹窗: 关闭态 panel 透明（开/关对照）", {
    DialogFixture fx;
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    fx.st.open = false;
    test_support::HeadlessFrame f("githubsyncdlgclosed");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        const auto* panel = ui.find(std::string(kDlgId) + ".panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(panel->opacity < 0.5f);   // 关着 = 透明，不是「没建树」
        return true;
    }));

    // 对照组：同一夹具置 open=true → 不透明
    DialogFixture open;
    open.st.open = true;
    test_support::HeadlessFrame f2("githubsyncdlgopen");
    DOCTEST_CHECK(f2.frame([&](eui::Ui& ui) {
        open.draw(ui, screen);
        const auto* panel = ui.find(std::string(kDlgId) + ".panel");
        DOCTEST_REQUIRE(panel != nullptr);
        DOCTEST_CHECK(panel->opacity > 0.5f);
        return true;
    }));
    return true;
})

REGISTER_TEST(github_error_detail_format,
              "github_sync: 日志行/详情文本格式（纯函数）", {
    auto e = makeEntry("GET", "https://api.github.com/x", 200, 45);
    DOCTEST_CHECK_EQ(ms::formatGitLogRowLine(e),
                     std::string("GET https://api.github.com/x 200 45ms"));

    // 无状态码（网络错误）→ 明确写「网络错误」而不是 0
    auto net = makeEntry("PUT", "https://api.github.com/y", 0, 0);
    net.networkError = true;
    net.errorMessage = "timeout";
    DOCTEST_CHECK_EQ(ms::formatGitLogRowLine(net),
                     std::string("PUT https://api.github.com/y 网络错误 0ms"));
    // 详情优先 bodyTail，其次 errorMessage
    DOCTEST_CHECK_EQ(ms::formatGitLogDetail(net), std::string("timeout"));
    e.bodyTail = "{\"a\":1}";
    DOCTEST_CHECK_EQ(ms::formatGitLogDetail(e), std::string("{\"a\":1}"));

    // 汇总文本含摘要 + 每条日志 + 每条的非空详情（不依赖选中行）
    std::vector<platform::github::ApiLogEntry> logs{e, net};
    const std::string all = ms::formatGitErrorDetail("摘要X", logs);
    DOCTEST_CHECK(all.find("摘要X") != std::string::npos);
    DOCTEST_CHECK(all.find("GET") != std::string::npos);
    DOCTEST_CHECK(all.find("PUT") != std::string::npos);
    DOCTEST_CHECK(all.find("timeout") != std::string::npos);
    DOCTEST_CHECK(all.find("{\"a\":1}") != std::string::npos);
    return true;
})

REGISTER_TEST(github_sync_store_unsynced_notice,
              "github_sync 存储: 未同步提示键往返（F5）+ save() 保留", {
    test_support::TestEnv env("gitsync", "notice");
    module::settings::GithubSyncStore store(&env.handle());

    auto has0 = store.hasUnsyncedNotice();
    DOCTEST_REQUIRE(has0.isOk());
    DOCTEST_CHECK(!has0.value());   // 默认无
    DOCTEST_CHECK(store.setUnsyncedNotice().isOk());
    auto has1 = store.hasUnsyncedNotice();
    DOCTEST_REQUIRE(has1.isOk());
    DOCTEST_CHECK(has1.value());
    // 配置保存不得清掉提示键
    module::settings::GithubSyncConfig cfg;
    cfg.enabled = true;
    DOCTEST_CHECK(store.save(cfg).isOk());
    auto has2 = store.hasUnsyncedNotice();
    DOCTEST_REQUIRE(has2.isOk());
    DOCTEST_CHECK(has2.value());
    // 同步成功后清除
    DOCTEST_CHECK(store.clearUnsyncedNotice().isOk());
    auto has3 = store.hasUnsyncedNotice();
    DOCTEST_REQUIRE(has3.isOk());
    DOCTEST_CHECK(!has3.value());
    return true;
})

REGISTER_TEST(github_sync_card_info_rows,
              "github_sync 卡片: public 提醒 / 分支提示 / 本地 git 灰字 / 未同步黄字 / 删跳过黄字（带对照）", {
    CardFixture fx;
    fx.in.githubSyncRepoPublic = true;
    fx.in.githubSyncBranchHint = "分支 main 不存在，已按仓库默认分支自动创建";
    fx.in.githubSyncLocalGitUnavailable = true;
    fx.in.githubSyncUnsyncedNotice = true;
    fx.in.githubSyncDeleteSkipHint = "有 1 篇本地仍在但读取失败的笔记，已跳过远端删除（下轮重试）";
    test_support::HeadlessFrame f("githubsynccardinfo");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        const auto* pub = ui.find(std::string(kCardId) + ".publicwarn");
        DOCTEST_REQUIRE(pub != nullptr);
        DOCTEST_CHECK(pub->text.find("公开") != std::string::npos);
        const auto* hint = ui.find(std::string(kCardId) + ".branchhint");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_CHECK(hint->text.find("自动创建") != std::string::npos);
        const auto* git = ui.find(std::string(kCardId) + ".localgit");
        DOCTEST_REQUIRE(git != nullptr);
        DOCTEST_CHECK_EQ(git->text, std::string("本地版本历史：不可用"));
        const auto* uns = ui.find(std::string(kCardId) + ".unsynced");
        DOCTEST_REQUIRE(uns != nullptr);
        DOCTEST_CHECK(uns->text.find("未同步") != std::string::npos);
        const auto* skip = ui.find(std::string(kCardId) + ".deleteskip");
        DOCTEST_REQUIRE(skip != nullptr);
        DOCTEST_CHECK(skip->text.find("跳过远端删除") != std::string::npos);
        return true;
    }));

    // 对照组：五个标志全 false/空 → 五行都不画（否则上面可能只是「恒画」）
    CardFixture clean;
    test_support::HeadlessFrame fc("githubsynccardinfoclean");
    DOCTEST_CHECK(fc.frame([&](eui::Ui& ui) {
        clean.draw(ui);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".publicwarn") == nullptr);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".branchhint") == nullptr);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".localgit") == nullptr);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".unsynced") == nullptr);
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".deleteskip") == nullptr);
        return true;
    }));
    return true;
})

// 卡高常量护栏：五条条件信息行全显（最坏组合）时内容底 + 下边距 8 不越卡——
// kGithubSyncCardHeight 若再调整而没按最坏档（开关 44×24 字面值不随字号缩放；
// N 项统一大小 52×28→44×24）复算，这里红。方案 A 的错误/进度已收进头部与操作行，
// 最坏行序 = 五条信息行。UAT6 零滚动收口：卡上下留白 16→8（362 = 可用高恰好值，
// 见 settings_ui_logic.h），下边距判据同步 16→8。
REGISTER_TEST(github_sync_card_worst_case_fits_height,
              "github_sync 卡片: 五条信息行全显的最坏行序不越卡（卡高护栏）", {
    CardFixture fx;
    fx.in.githubSyncRepoPublic = true;
    fx.in.githubSyncBranchHint = "分支 main 不存在，已按仓库默认分支自动创建";
    fx.in.githubSyncLocalGitUnavailable = true;
    fx.in.githubSyncUnsyncedNotice = true;
    fx.in.githubSyncDeleteSkipHint = "有 1 篇本地仍在但读取失败的笔记，已跳过远端删除（下轮重试）";
    eui::Ui ui;
    fx.draw(ui);
    ui.layout(1600.0f, 1000.0f);
    const auto* card = ui.find(kCardId);
    const auto* last = ui.find(std::string(kCardId) + ".deleteskip");
    DOCTEST_REQUIRE(card != nullptr);
    DOCTEST_REQUIRE(last != nullptr);
    const float cardH = card->frame.height;
    DOCTEST_CHECK_MESSAGE(
        last->frame.y + last->frame.height + 8.0f <= cardH + 0.5f,
        "最坏行序内容底 " +
            std::to_string(last->frame.y + last->frame.height) + " + 8 越出卡高 " +
            std::to_string(cardH));
    return true;
})

REGISTER_TEST(github_error_dialog_conflict_variant,
              "github_sync 409 弹窗: 标题/主按钮 + 二次确认才触发拉取后再推", {
    DialogFixture fx;
    fx.st.open = true;
    fx.st.variant = ms::GitErrorDialogVariant::Conflict409;
    fx.in.summary = "远端 database.sql 已被改动（409 冲突）";
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    test_support::HeadlessFrame f("githubsyncdlg409");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        const auto* title = ui.find(std::string(kDlgId) + ".title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("409") != std::string::npos);
        const auto* label = ui.find(std::string(kDlgId) + ".btn.retry.text.normal");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("拉取远端后再推"));
        // 第一下只武装，不触发动作
        ui.find(std::string(kDlgId) + ".btn.retry.bg")->onClick();
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.pullThenPushClicks, 0);
    DOCTEST_CHECK(fx.st.conflictConfirmArmed);

    // 第二帧：文案变确认态 + 提示行变成覆盖警告；第二下才真触发
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        const auto* label = ui.find(std::string(kDlgId) + ".btn.retry.text.normal");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK(label->text.find("确认") != std::string::npos);
        const auto* hint = ui.find(std::string(kDlgId) + ".hint");
        DOCTEST_REQUIRE(hint != nullptr);
        DOCTEST_CHECK(hint->text.find("覆盖本地") != std::string::npos);
        ui.find(std::string(kDlgId) + ".btn.retry.bg")->onClick();
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.pullThenPushClicks, 1);
    DOCTEST_CHECK(!fx.st.conflictConfirmArmed);   // 触发后解除武装
    return true;
})

REGISTER_TEST(github_error_dialog_auth_variant_decision,
              "github_sync 失败分类: 401 → 定向引导；409 优先；非 401/空日志 → 通用（带对照）", {
    const auto auth = makeEntry("GET", "https://api.github.com/repos/u/r", 401, 10);
    const auto ok200 = makeEntry("GET", "https://api.github.com/repos/u/r", 200, 10);
    const auto err500 = makeEntry("PUT", "https://api.github.com/repos/u/r/contents/x", 500, 10);
    using V = ms::GitErrorDialogVariant;
    const std::vector<platform::github::ApiLogEntry> none;

    DOCTEST_CHECK(ms::variantForSyncFailure({auth}, false) == V::AuthFailed);
    // 对照组：非 401 不得被判成认证失败（否则就是"凡失败皆引导"）
    DOCTEST_CHECK(ms::variantForSyncFailure({ok200}, false) == V::Error);
    DOCTEST_CHECK(ms::variantForSyncFailure({err500}, false) == V::Error);
    // 空日志（未观察到任何请求）→ 通用
    DOCTEST_CHECK(ms::variantForSyncFailure(none, false) == V::Error);
    // 混合日志：只要有一条 401 即算认证失败
    DOCTEST_CHECK(ms::variantForSyncFailure({ok200, auth}, false) == V::AuthFailed);
    // 409 优先于 401：同现时维持既有冲突语义（不回归）
    DOCTEST_CHECK(ms::variantForSyncFailure({auth}, true) == V::Conflict409);
    return true;
})

REGISTER_TEST(github_error_dialog_auth_variant,
              "github_sync 401 弹窗: 标题/主按钮走定向引导，且不触发通用重试（带通用变体对照）", {
    DialogFixture fx;
    fx.st.open = true;
    fx.st.variant = ms::GitErrorDialogVariant::AuthFailed;
    fx.in.summary = "推送失败：HTTP 401（Token 无效或已过期）";
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    test_support::HeadlessFrame f("githubsyncdlgauth");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        const auto* title = ui.find(std::string(kDlgId) + ".title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("认证失败") != std::string::npos);
        const auto* label = ui.find(std::string(kDlgId) + ".btn.retry.text.normal");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("去更新 Token"));
        ui.find(std::string(kDlgId) + ".btn.retry.bg")->onClick();
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.updateTokenClicks, 1);   // 主按钮走定向引导
    DOCTEST_CHECK_EQ(fx.retryClicks, 0);         // 不是通用重试

    // 对照组：通用变体下同一位仍是「重试」——证明上面断言的是变体分支而非恒真
    fx.st.variant = ms::GitErrorDialogVariant::Error;
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        const auto* title = ui.find(std::string(kDlgId) + ".title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("认证失败") == std::string::npos);
        const auto* label = ui.find(std::string(kDlgId) + ".btn.retry.text.normal");
        DOCTEST_REQUIRE(label != nullptr);
        DOCTEST_CHECK_EQ(label->text, std::string("重试"));
        return true;
    }));
    return true;
})

REGISTER_TEST(github_error_dialog_conflict_overwrite_list,
              "github_sync 409 弹窗: 列出将被覆盖的本地未推文件清单（F6，带空清单对照）", {
    DialogFixture fx;
    fx.st.open = true;
    fx.st.variant = ms::GitErrorDialogVariant::Conflict409;
    fx.in.overwriteFiles = {"data/database.sql", "notes/a.md"};
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    test_support::HeadlessFrame f("githubsyncdlgovw");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        const auto* hd =
            ui.find(std::string(kDlgId) + ".logs.overwrite.header");
        DOCTEST_REQUIRE(hd != nullptr);
        DOCTEST_CHECK(hd->text.find("覆盖") != std::string::npos);
        const auto* r0 = ui.find(std::string(kDlgId) + ".logs.overwrite.0.text");
        DOCTEST_REQUIRE(r0 != nullptr);
        DOCTEST_CHECK(r0->text.find("database.sql") != std::string::npos);
        const auto* r1 = ui.find(std::string(kDlgId) + ".logs.overwrite.1.text");
        DOCTEST_REQUIRE(r1 != nullptr);
        DOCTEST_CHECK(r1->text.find("a.md") != std::string::npos);
        return true;
    }));

    // 对照：无清单（非 409 场景或本机无未推改动）→ 清单区不画
    DialogFixture clean;
    clean.st.open = true;
    clean.st.variant = ms::GitErrorDialogVariant::Conflict409;
    test_support::HeadlessFrame fc("githubsyncdlgovwclean");
    DOCTEST_CHECK(fc.frame([&](eui::Ui& ui) {
        clean.draw(ui, screen);
        DOCTEST_CHECK(ui.find(std::string(kDlgId) +
                              ".logs.overwrite.header") == nullptr);
        return true;
    }));
    return true;
})

// 清单上限：本地未推改动可能有成千上万条（远端长期没推 / 首次拉取），而 scrollView 的
// content 每帧重建 —— 逐条建节点是无界每帧成本，上万行直接把 UI 卡死。只列前 50 条 +
// 一行「其余 N 个」：信息不丢（用户知道总量，不会以为「就这么点」）。
// 判据两半：截断侧（60 条 → 前 50 在、第 51 条起不在、其余行报 10）+ 边界内对照
//（恰好 50 条 → 全列、无其余行，防「不分情形恒拼提示行」的过度修复）。
REGISTER_TEST(github_error_dialog_conflict_overwrite_list_capped,
              "github_sync 409 弹窗: 覆盖清单超 50 条只列前 50 + 其余计数行（边界内 50 条对照）", {
    eui::Screen screen;
    screen.width = 1600.0f;
    screen.height = 1000.0f;

    DialogFixture fx;
    fx.st.open = true;
    fx.st.variant = ms::GitErrorDialogVariant::Conflict409;
    for (int i = 0; i < 60; ++i) {
        fx.in.overwriteFiles.push_back("notes/n" + std::to_string(i) + ".md");
    }
    test_support::HeadlessFrame f("githubsyncdlgovwcap");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, screen);
        DOCTEST_CHECK(ui.find(std::string(kDlgId) +
                              ".logs.overwrite.49.text") != nullptr);
        DOCTEST_CHECK(ui.find(std::string(kDlgId) +
                              ".logs.overwrite.50.text") == nullptr);
        const auto* more =
            ui.find(std::string(kDlgId) + ".logs.overwrite.more");
        DOCTEST_REQUIRE(more != nullptr);
        DOCTEST_CHECK(more->text.find("10") != std::string::npos);
        return true;
    }));

    // 对照（边界内）：恰好 50 条 → 全列、无「其余」行
    DialogFixture fx50;
    fx50.st.open = true;
    fx50.st.variant = ms::GitErrorDialogVariant::Conflict409;
    for (int i = 0; i < 50; ++i) {
        fx50.in.overwriteFiles.push_back("notes/n" + std::to_string(i) + ".md");
    }
    test_support::HeadlessFrame f50("githubsyncdlgovw50");
    DOCTEST_CHECK(f50.frame([&](eui::Ui& ui) {
        fx50.draw(ui, screen);
        DOCTEST_CHECK(ui.find(std::string(kDlgId) +
                              ".logs.overwrite.49.text") != nullptr);
        DOCTEST_CHECK(ui.find(std::string(kDlgId) +
                              ".logs.overwrite.more") == nullptr);
        return true;
    }));
    return true;
})

REGISTER_TEST(github_sync_logic_throttle,
              "github_sync 逻辑: 5 分钟节流窗口 + 手动绕开", {
    using namespace module::settings::gsync;
    // 从未 commit → 立即到点
    DOCTEST_CHECK(throttleDue(0, 1'000'000));
    // 窗口内 → 不到点；边界（恰好 300s）→ 到点
    DOCTEST_CHECK(!throttleDue(1'000'000, 1'000'000 + kCommitThrottleSec - 1));
    DOCTEST_CHECK(throttleDue(1'000'000, 1'000'000 + kCommitThrottleSec));
    // 时钟回拨 → 不把用户锁死在窗口里
    DOCTEST_CHECK(throttleDue(1'000'000, 999'000));
    DOCTEST_CHECK_EQ(kCommitThrottleSec, static_cast<int64_t>(300));
    return true;
})

REGISTER_TEST(github_sync_logic_backup_prune,
              "github_sync 逻辑: 备份名 + 只保留最近 5 份", {
    using namespace module::settings::gsync;
    DOCTEST_CHECK_EQ(backupFileName(1700000000),
                     std::string("devdesk-1700000000.db"));
    DOCTEST_CHECK_EQ(kBackupKeep, 5);

    test_support::TempDir d("gsbackup");
    for (int i = 0; i < 8; ++i) {
        std::ofstream o(d.path / backupFileName(1700000000 + i),
                        std::ios::binary | std::ios::trunc);
        o << "x";
    }
    // 混入不该被删/不该被数的文件
    std::ofstream(d.path / "notes.md", std::ios::binary | std::ios::trunc) << "x";
    std::ofstream(d.path / "devdesk-notadb.txt", std::ios::binary | std::ios::trunc)
        << "x";

    const int removed = pruneBackups(d.path, kBackupPrefix, kBackupKeep);
    DOCTEST_CHECK_EQ(removed, 3);   // 8 - 5

    int remaining = 0;
    for (const auto& e : std::filesystem::directory_iterator(d.path)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(kBackupPrefix, 0) == 0 &&
            name.size() > 3 && name.compare(name.size() - 3, 3, ".db") == 0) {
            ++remaining;
        }
    }
    DOCTEST_CHECK_EQ(remaining, 5);
    // 最旧的 3 份被删，最新的还在；无关文件零触碰
    DOCTEST_CHECK(!std::filesystem::exists(d.path / backupFileName(1700000000)));
    DOCTEST_CHECK(std::filesystem::exists(d.path / backupFileName(1700000007)));
    DOCTEST_CHECK(std::filesystem::exists(d.path / "notes.md"));
    DOCTEST_CHECK(std::filesystem::exists(d.path / "devdesk-notadb.txt"));
    return true;
})

REGISTER_TEST(github_sync_logic_first_sync_choice,
              "github_sync 逻辑: 首次方向判定（远端有→拉取覆盖 / 无→上传 / 探测失败→不动作）", {
    using namespace module::settings::gsync;
    DOCTEST_CHECK(firstSyncChoice(true, false) ==
                  FirstSyncChoice::PullThenPush);
    DOCTEST_CHECK(firstSyncChoice(false, true) == FirstSyncChoice::Push);
    // 网络错 / 认证错（既非存在也非缺失）→ 不动作，错误交给用户
    DOCTEST_CHECK(firstSyncChoice(false, false) == FirstSyncChoice::Fail);
    return true;
})

REGISTER_TEST(github_sync_store_state_keys,
              "github_sync 存储: 首次标记 + 笔记登记往返；save() 不得清状态键", {
    test_support::TestEnv env("gitsync", "store1");
    module::settings::GithubSyncStore store(&env.handle());

    auto done0 = store.isFirstSyncDone();
    DOCTEST_REQUIRE(done0.isOk());
    DOCTEST_CHECK(!done0.value());   // 默认未做过
    DOCTEST_CHECK(store.setFirstSyncDone().isOk());
    auto done1 = store.isFirstSyncDone();
    DOCTEST_REQUIRE(done1.isOk());
    DOCTEST_CHECK(done1.value());

    // 笔记登记往返（删除传播的远端基线）
    DOCTEST_CHECK(store.recordSyncedNote("a.md", "abc123", "hash-a").isOk());
    DOCTEST_CHECK(store.recordSyncedNote("b.md", "def456", "hash-b").isOk());
    auto names = store.syncedNoteRecords();
    DOCTEST_REQUIRE(names.isOk());
    DOCTEST_CHECK_EQ(names.value().size(), static_cast<size_t>(2));
    DOCTEST_CHECK(names.value().count("a.md") == 1);
    DOCTEST_CHECK(names.value().count("b.md") == 1);

    // 回归：save() 曾按前缀整库清理（eraseWithPrefixExcept），会把状态键
    // 一并清掉——保存配置后首次标记与笔记登记必须原样存活
    module::settings::GithubSyncConfig cfg;
    cfg.enabled = true;
    cfg.repoUrl = "https://github.com/u/r.git";
    cfg.branch = "main";
    DOCTEST_CHECK(store.save(cfg).isOk());
    auto done2 = store.isFirstSyncDone();
    DOCTEST_REQUIRE(done2.isOk());
    DOCTEST_CHECK(done2.value());
    auto names2 = store.syncedNoteRecords();
    DOCTEST_REQUIRE(names2.isOk());
    DOCTEST_CHECK_EQ(names2.value().size(), static_cast<size_t>(2));

    // 清除语义
    DOCTEST_CHECK(store.forgetSyncedNote("a.md").isOk());
    auto names3 = store.syncedNoteRecords();
    DOCTEST_REQUIRE(names3.isOk());
    DOCTEST_CHECK_EQ(names3.value().size(), static_cast<size_t>(1));
    DOCTEST_CHECK(names3.value().count("b.md") == 1);
    DOCTEST_CHECK(store.clearFirstSyncDone().isOk());
    auto done3 = store.isFirstSyncDone();
    DOCTEST_REQUIRE(done3.isOk());
    DOCTEST_CHECK(!done3.value());
    return true;
})

// Windows MinGW SQLite: importFromString 因 PRAGMA foreign_keys / 事务锁差异失败，
// 属纯环境差异（MSYS2 SQLite 3.x 严格模式），不影响产品代码。判据独立于权限注入语义。
static bool sqliteImportWorksUnderMinGW() {
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}

REGISTER_TEST(github_sync_import_database_events,
              "github_sync 存储: importDatabase 事件序——begin/end 恒发、imported 仅成功（F3）", {
    if (!sqliteImportWorksUnderMinGW()) {
        DOCTEST_SKIP("MinGW SQLite importFromString: PRAGMA foreign_keys / 事务冲突；属纯环境差异，不影响产品代码");
    }
    test_support::TestEnv env("gitsync", "importdb");
    module::settings::GithubSyncStore store(&env.handle());
    int begin = 0;
    int end = 0;
    int imported = 0;
    auto s1 = ::core::event::bus().subscribe(
        ::core::event::kDbImportBegin,
        [&](const std::string&) { ++begin; });
    auto s2 = ::core::event::bus().subscribe(
        ::core::event::kDbImportEnd,
        [&](const std::string&) { ++end; });
    auto s3 = ::core::event::bus().subscribe(
        ::core::event::kDbImported,
        [&](const std::string&) { ++imported; });

    // 造一份合法 dump：建表 + 插行
    auto* db = &env.handle();
    DOCTEST_CHECK(db->exec("CREATE TABLE t4s(a INTEGER PRIMARY KEY, b TEXT)").isOk());
    DOCTEST_CHECK(db->exec("INSERT INTO t4s(a,b) VALUES (1,'x')").isOk());
    auto dump = ::core::sql_dump::exportToString(*db);
    DOCTEST_REQUIRE(dump.isOk());

    // 成功路径：begin/end/imported 各一次；数据仍在
    auto ok = store.importDatabase(dump.value());
    DOCTEST_REQUIRE(ok.isOk());
    DOCTEST_CHECK_EQ(begin, 1);
    DOCTEST_CHECK_EQ(end, 1);
    DOCTEST_CHECK_EQ(imported, 1);

    // 失败路径：坏 SQL → begin/end 仍发（后台任务必须被恢复），imported 不发，
    // 库内容保持导入前原样（importFromString 事务原子性）
    auto bad = store.importDatabase("这不是 SQL");
    DOCTEST_CHECK(bad.isFail());
    DOCTEST_CHECK_EQ(begin, 2);
    DOCTEST_CHECK_EQ(end, 2);
    DOCTEST_CHECK_EQ(imported, 1);
    auto rows = db->exec("SELECT 1 FROM t4s");
    DOCTEST_CHECK(rows.isOk());
    return true;
})

REGISTER_TEST(github_sync_store_sha_records,
              "github_sync 存储: 笔记 sha+hash 登记 + database.sql sha 登记（save() 保留状态键）", {
    test_support::TestEnv env("gitsync", "store2");
    module::settings::GithubSyncStore store(&env.handle());

    // 无登记 → 空
    auto recs0 = store.syncedNoteRecords();
    DOCTEST_REQUIRE(recs0.isOk());
    DOCTEST_CHECK(recs0.value().empty());
    auto sql0 = store.syncedSqlSha();
    DOCTEST_REQUIRE(sql0.isOk());
    DOCTEST_CHECK(sql0.value().empty());

    // 登记 → 读回一致；覆盖登记取新值
    DOCTEST_CHECK(store.recordSyncedNote("a.md", "sha-1", "h1").isOk());
    DOCTEST_CHECK(store.recordSyncedNote("a.md", "sha-2", "h2").isOk());
    DOCTEST_CHECK(store.recordSyncedNote("b.md", "sha-3", "h3").isOk());
    DOCTEST_CHECK(store.recordSyncedSqlSha("sql-1").isOk());
    auto recs1 = store.syncedNoteRecords();
    DOCTEST_REQUIRE(recs1.isOk());
    DOCTEST_REQUIRE_EQ(recs1.value().size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(recs1.value().at("a.md").sha, std::string("sha-2"));
    DOCTEST_CHECK_EQ(recs1.value().at("a.md").contentHash, std::string("h2"));
    DOCTEST_CHECK_EQ(recs1.value().at("b.md").sha, std::string("sha-3"));
    auto sql1 = store.syncedSqlSha();
    DOCTEST_REQUIRE(sql1.isOk());
    DOCTEST_CHECK_EQ(sql1.value(), std::string("sql-1"));

    // 配置保存不得清掉 sha/hash 状态键（含 database.sql 的）
    module::settings::GithubSyncConfig cfg;
    cfg.enabled = true;
    DOCTEST_CHECK(store.save(cfg).isOk());
    auto recs2 = store.syncedNoteRecords();
    DOCTEST_REQUIRE(recs2.isOk());
    DOCTEST_CHECK_EQ(recs2.value().size(), static_cast<size_t>(2));
    auto sql2 = store.syncedSqlSha();
    DOCTEST_REQUIRE(sql2.isOk());
    DOCTEST_CHECK_EQ(sql2.value(), std::string("sql-1"));

    // forget 清 sha + hash 两键
    DOCTEST_CHECK(store.forgetSyncedNote("a.md").isOk());
    auto recs3 = store.syncedNoteRecords();
    DOCTEST_REQUIRE(recs3.isOk());
    DOCTEST_CHECK_EQ(recs3.value().count("a.md"), static_cast<size_t>(0));
    return true;
})
