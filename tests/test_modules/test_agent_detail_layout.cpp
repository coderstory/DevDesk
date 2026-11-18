// tests/test_modules/test_agent_detail_layout.cpp
// 智能体详情弹窗布局壳三尺寸无头用例（UAT6 方案 A：单列三档 tab 一套几何走全尺寸）。
// 钉死三档 geometries：弹性档 1600 屏 → 800×660、窄窗钳制 700×560 → 652×512、
// 矮窗钳制 900×600 → 720×552（宽 = max(720, 窗口/2)，高 660，均经上游 overlay 钳制）。
//
// 过滤子串：detail_layout（ASCII 可 grep）
// 预检：构建 ./build/tests/devdesk_tests && 运行时过滤 detail_layout 只跑本文件用例。
#include "doctest.h"
#include "test_support.h"

#include "core/theme.h"
#include "core/tool.h"
#include "module_agent/agent_module.h"
#include "module_agent/agent_store.h"
#include "platform/http_client.h"

#include <eui/dsl.h>
#include <ui_kit/glass.h>   // glassBorder（池162 摘要三卡卡语言断言）

#include <algorithm>
#include <cmath>
#include <string>

namespace {

using namespace module::agent;

const eui::Screen kScreenBig{1600.0f, 1000.0f};   // 弹性档：max(720, 800) = 800

void composeTo(eui::Ui& ui, AgentModule& mod, const eui::Screen& screen,
               const char* pageId) {
    ui.begin(pageId);
    mod.compose(ui, screen);
    ui.end();
    ui.layout(screen.width, screen.height);
}

bool nearPx(float a, float b) { return std::fabs(a - b) <= 0.5f; }

bool clickId(eui::Ui& ui, const std::string& id) {
    auto* el = ui.find(id);
    if (el == nullptr) return false;
    el->onClick();
    return true;
}

// 打开某任务的详情弹窗（点列表行「详情」chip），返回是否点开
bool openDetailDialog(eui::Ui& ui, AgentModule& mod,
                      const std::string& tid) {
    return clickId(ui, "agent.task." + tid + ".detail");
}

// 共用装配：建一个带消息的 completed run 的任务 + 模块实例
struct DetailFixture {
    test_support::TestDb tdb;
    test_support::FakeProfileSource profile;
    AgentStore store;
    AgentModule::Deps deps;
    AgentModule mod;
    int64_t taskId = 0;

    static AgentModule::Deps makeDeps(test_support::TestDb& db,
                                      test_support::FakeProfileSource& prof) {
        AgentModule::Deps deps;
        deps.db = &db.handle();
        deps.profileSource = &prof;
        deps.http = [](const platform::http::Request&) {
            platform::http::Response res;
            res.statusCode = 200;
            res.body = "{}";
            return core::Result<platform::http::Response>::ok(res);
        };
        return deps;
    }

    DetailFixture(const char* dbTag, const char* dbName)
        : tdb(dbTag, dbName),
          store(tdb.handle()),
          deps(makeDeps(tdb, profile)),
          mod(deps) {
        mod.onStart();
    }
    ~DetailFixture() { mod.onStop(); }

    bool seedTask(const char* name) {
        auto created =
            store.createTask(name, "布局验证目标", "manual", "default", 10, false);
        if (created.isFail()) return false;
        taskId = created.value();
        auto r = store.startRun(taskId);
        if (r.isFail()) return false;
        // 种一条消息：msgscroll 相关档位断言要有真实内容才有判别力
        if (!store.appendMessage(r.value(), 0, "user",
                                 R"([{"type":"text","text":"布局探针"}])",
                                 std::nullopt)
                 .isOk()) {
            return false;
        }
        return store.finishRun(r.value(), "completed", 1, std::nullopt,
                               std::nullopt, std::nullopt)
            .isOk();
    }
};

// ---------- 三尺寸 test ----------

REGISTER_TEST(agent_ui_detail_layout_elastic_1600x1000,
              "detail_layout: 弹性档 800×660——max(720,窗口/2)、三卡横排、segment 三档薄荷绿 primary（D-12 令牌统一）、单列全宽", {
    DetailFixture fx("agentdl", "wide");
    DOCTEST_REQUIRE(fx.seedTask("弹性档布局任务"));

    eui::Ui ui;
    composeTo(ui, fx.mod, kScreenBig, "layout_elastic_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, fx.mod, std::to_string(fx.taskId)));

    eui::Ui ui2;
    composeTo(ui2, fx.mod, kScreenBig, "layout_elastic_2");

    // 面板 = 弹性宽 max(720, 1600/2)=800 × 552（UAT6 弹窗收口，原 660）
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr, "弹性档缺 panel");
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 800.0f) &&
                              nearPx(panel->frame.height, 552.0f),
                          "弹性档面板应 800×552，实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));
    // 头部 = 真实任务名（方案 A：不再恒「任务详情」）
    const auto* title = ui2.find("agent.dialog.detail.title");
    DOCTEST_REQUIRE(title != nullptr);
    DOCTEST_CHECK_EQ(title->text, std::string("弹性档布局任务"));

    // 三卡常驻横排：同 y、等宽、水平间隙 8
    const auto* lastCard = ui2.find("agent.dialog.detail.summary.last");
    const auto* nextCard = ui2.find("agent.dialog.detail.summary.next");
    const auto* statsCard = ui2.find("agent.dialog.detail.summary.stats");
    DOCTEST_REQUIRE_MESSAGE(lastCard != nullptr && nextCard != nullptr &&
                                statsCard != nullptr,
                            "详情弹窗缺摘要三卡");
    DOCTEST_CHECK(nearPx(lastCard->frame.y, nextCard->frame.y));
    DOCTEST_CHECK(nearPx(nextCard->frame.x -
                             (lastCard->frame.x + lastCard->frame.width),
                         8.0f));
    DOCTEST_CHECK(nearPx(lastCard->frame.width, nextCard->frame.width));
    DOCTEST_CHECK(nearPx(lastCard->frame.width, statsCard->frame.width));
    // 三卡补齐全局卡语言（池162 §3.3）：1px glassBorder 高光边 + cardSurface 不变
    const auto* lastBg = ui2.find("agent.dialog.detail.summary.last.bg");
    DOCTEST_REQUIRE(lastBg != nullptr);
    {
        const auto& tokEarly = core::theme::current();
        DOCTEST_CHECK_MESSAGE(
            nearPx(lastBg->border.width, 1.0f) &&
                nearPx(lastBg->border.color.r, ui_kit::glassBorder(tokEarly).r) &&
                nearPx(lastBg->border.color.a, ui_kit::glassBorder(tokEarly).a),
            "摘要卡 .bg 应有 1px glassBorder（池162 卡语言对齐），实测 border.width=" +
                std::to_string(lastBg->border.width));
    }

    // segment 三档：选中块 = 薄荷绿 primary（D-12 迁移后 primary 槽位即 #36D399，
    // 与产品码 t.colors.primary 同源）；未选块灰底
    const auto& tok = core::theme::current();
    const auto* navOn = ui2.find("agent.dialog.detail.nav.item.0.bg");
    const auto* navOff = ui2.find("agent.dialog.detail.nav.item.1.bg");
    DOCTEST_REQUIRE_MESSAGE(navOn != nullptr && navOff != nullptr,
                            "segment 缺 .nav.item.<i>.bg");
    DOCTEST_CHECK_MESSAGE(
        nearPx(navOn->color.r, tok.colors.primary.r) &&
            nearPx(navOn->color.g, tok.colors.primary.g) &&
            nearPx(navOn->color.b, tok.colors.primary.b),
        "segment 选中块应为薄荷绿 primary 底");
    // 旧「反向锁死暖橙」断言已删：与正向断言共用同一 primary 令牌自相矛盾恒红；
    // 暖橙旧槽在 D-12 迁移中退役（primary = #36D399），无可锁对象。
    DOCTEST_CHECK_MESSAGE(
        nearPx(navOff->color.r, tok.colors.background.r) &&
            nearPx(navOff->color.g, tok.colors.background.g) &&
            nearPx(navOff->color.b, tok.colors.background.b),
        "segment 未选中块应为 background 灰底");
    // nav 全宽 = innerW（裁定 2：内容区双侧 24px，800−48=752）
    const auto* nav = ui2.find("agent.dialog.detail.nav");
    DOCTEST_REQUIRE(nav != nullptr);
    DOCTEST_CHECK(nearPx(nav->frame.width, 752.0f));

    // tab 0 默认：runlist 单列全宽、互斥（详情/配置退场）
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(runlist != nullptr);
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.width, 752.0f),
        "执行记录应单列全宽 752（旧左右分栏已退役），实测 " +
            std::to_string(runlist->frame.width));
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.scroll") == nullptr);
    // 无节点越出面板底（runlist 底 ≤ panel 底，底部按钮带在其下）
    const float panelBot = panel->frame.y + panel->frame.height;
    const float runlistBot = runlist->frame.y + runlist->frame.height;
    DOCTEST_CHECK_MESSAGE(runlistBot <= panelBot + 0.5f,
                          "runlist 底不应越出 panel，panel 底=" +
                              std::to_string(panelBot) +
                              " runlist 底=" + std::to_string(runlistBot));
    // 底部按钮带（关闭 | 编辑任务）在场且收在面板内
    const auto* footerClose = ui2.find("agent.dialog.detail.footer.close.bg");
    const auto* footerEdit = ui2.find("agent.dialog.detail.footer.edit.bg");
    DOCTEST_REQUIRE(footerClose != nullptr && footerEdit != nullptr);
    DOCTEST_CHECK(footerClose->frame.x + footerClose->frame.width <=
                  footerEdit->frame.x + 0.5f);
    DOCTEST_CHECK(footerClose->frame.y + footerClose->frame.height <=
                  panelBot + 0.5f);
    return true;
})

REGISTER_TEST(agent_ui_detail_layout_narrow_700x560,
              "detail_layout: 窄窗 700×560——panel 652×512、同一套三档 nav（旧窄屏 2-tab 分叉退役）、单列几何同公式", {
    DetailFixture fx("agentdl", "narrow");
    DOCTEST_REQUIRE(fx.seedTask("窄窗测试"));

    const eui::Screen kScreen700{700.0f, 560.0f};

    eui::Ui ui;
    composeTo(ui, fx.mod, kScreen700, "layout_narrow_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, fx.mod, std::to_string(fx.taskId)));

    eui::Ui ui2;
    composeTo(ui2, fx.mod, kScreen700, "layout_narrow_2");

    // 面板经钳制: w = min(max(720, 350), 700-48) = 652, h = min(660, 560-48) = 512
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr, "窄窗 700×560 缺 panel");
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 652.0f) &&
                              nearPx(panel->frame.height, 512.0f),
                          "窄窗面板应 652×512（钳制后），实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));

    // 方案 A：窄窗同一套三档 nav（旧「窄屏 segmented 2 tab」分叉退役——存在即退役证明）
    const auto* nav = ui2.find("agent.dialog.detail.nav");
    DOCTEST_REQUIRE_MESSAGE(nav != nullptr, "窄窗缺 segment nav（三档应全尺寸同一套）");
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.0.bg") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.1.bg") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.2.bg") != nullptr);
    // 单列几何与弹性档同一公式——need339① 起 summaryY = 头部行底（44+lhS）+ 12，
    // 随字号派生，不再钉字面 48/143.5/183.5（旧字面压住头部状态行盒）
    const auto& tokN = core::theme::current();
    const float lhSN =
        static_cast<float>(std::lround(tokN.sizes.fontSizeSmall * 1.4f));
    const float lhBN =
        static_cast<float>(std::lround(tokN.sizes.fontSizeBody * 1.4f));
    const float summaryHN =
        std::max(81.0f, 2.0f * 10.0f + lhSN + lhBN + lhSN + 4.0f);
    const float summaryYN = 44.0f + lhSN + 12.0f;
    const float navYN = summaryYN + summaryHN + 14.0f;
    const float contentYN = navYN + 32.0f + 8.0f;
    const auto* lastCard = ui2.find("agent.dialog.detail.summary.last");
    const auto* runlist = ui2.find("agent.dialog.detail.runlist");
    DOCTEST_REQUIRE(lastCard != nullptr && runlist != nullptr);
    DOCTEST_CHECK_MESSAGE(
        nearPx(lastCard->frame.y - panel->frame.y, summaryYN),
        "摘要卡应 +44+lhS+12（行底让位公式），实测 " +
            std::to_string(lastCard->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(nearPx(nav->frame.y - panel->frame.y, navYN),
                          "nav 应 +summaryY+summaryH+14，实测 " +
                              std::to_string(nav->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.x - panel->frame.x, 24.0f) &&
            nearPx(runlist->frame.width, 604.0f),
        "窄窗主内容应单列全宽（x0=24、宽 604，kDialogInsetX 28→24），实测 x=" +
            std::to_string(runlist->frame.x - panel->frame.x) + " 宽 " +
            std::to_string(runlist->frame.width));
    DOCTEST_CHECK_MESSAGE(
        nearPx(runlist->frame.y - panel->frame.y, contentYN),
        "窄窗内容应 +contentY（navY+40），实测 " +
            std::to_string(runlist->frame.y - panel->frame.y));
    DOCTEST_CHECK_MESSAGE(runlist->frame.y + runlist->frame.height <=
                              panel->frame.y + panel->frame.height + 0.5f,
                          "窄窗内容溢出面板底");
    // 三卡常驻 + 互斥（tab 0 默认：无详情块/配置滚动区）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.next") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.stats") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.detail") == nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.config.scroll") == nullptr);

    // 切 tab 1 → 详情列上桌；切 tab 2 → 配置滚动区上桌（三档互斥钉死）
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui3;
    composeTo(ui3, fx.mod, kScreen700, "layout_narrow_3");
    DOCTEST_REQUIRE(ui3.find("agent.dialog.detail.detail") != nullptr);
    DOCTEST_CHECK(ui3.find("agent.dialog.detail.runlist") == nullptr);
    DOCTEST_CHECK(ui3.find("agent.dialog.detail.config.scroll") == nullptr);
    DOCTEST_REQUIRE(clickId(ui3, "agent.dialog.detail.nav.item.2.bg"));
    eui::Ui ui4;
    composeTo(ui4, fx.mod, kScreen700, "layout_narrow_4");
    DOCTEST_REQUIRE(ui4.find("agent.dialog.detail.config.scroll") != nullptr);
    DOCTEST_CHECK(ui4.find("agent.dialog.detail.runlist") == nullptr);
    DOCTEST_CHECK(ui4.find("agent.dialog.detail.detail") == nullptr);
    return true;
})

REGISTER_TEST(agent_ui_detail_layout_shortwide_900x600,
              "detail_layout: 矮窗 900×600——panel 720×552（宽 720 最小档不钳）、三档同 nav、固定区消息模式", {
    DetailFixture fx("agentdl", "shortw");
    DOCTEST_REQUIRE(fx.seedTask("矮窗测试"));

    const eui::Screen kScreen900{900.0f, 600.0f};

    eui::Ui ui;
    composeTo(ui, fx.mod, kScreen900, "layout_shortw_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, fx.mod, std::to_string(fx.taskId)));

    eui::Ui ui2;
    composeTo(ui2, fx.mod, kScreen900, "layout_shortw_2");

    // 面板经钳制: w = min(max(720, 450), 900-48) = 720（最小档即最终值），
    // h = min(660, 600-48) = 552
    const auto* panel = ui2.find("agent.dialog.detail.panel");
    DOCTEST_REQUIRE_MESSAGE(panel != nullptr, "矮窗 900×600 缺 panel");
    DOCTEST_CHECK_MESSAGE(nearPx(panel->frame.width, 720.0f) &&
                              nearPx(panel->frame.height, 552.0f),
                          "矮窗面板应 720×552（钳制后），实测 " +
                              std::to_string(panel->frame.width) + "×" +
                              std::to_string(panel->frame.height));

    // 三档同 nav + 三卡常驻（同弹性档，无分叉）
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.nav.item.2.bg") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.summary.last") != nullptr);
    DOCTEST_CHECK(ui2.find("agent.dialog.detail.runlist") != nullptr);

    // 无错误的 run：矮窗固定区模式仍成立（详情块预算 120，余高 129 ≥ 96）——
    // 折叠态无 msgscroll / detail.scroll；展开后 msgscroll 在场（降级断言归
    // test_agent_ui 的矮窗长错误用例）
    DOCTEST_REQUIRE(clickId(ui2, "agent.dialog.detail.nav.item.1.bg"));
    eui::Ui ui2n;
    composeTo(ui2n, fx.mod, kScreen900, "layout_shortw_2n");
    DOCTEST_CHECK(ui2n.find("agent.dialog.detail.detail.scroll") == nullptr);
    DOCTEST_CHECK(ui2n.find("agent.dialog.detail.msgscroll") == nullptr);
    DOCTEST_REQUIRE(ui2n.find("agent.dialog.detail.detail") != nullptr);
    DOCTEST_REQUIRE(clickId(ui2n, "agent.dialog.detail.msgs.toggle"));
    eui::Ui ui3;
    composeTo(ui3, fx.mod, kScreen900, "layout_shortw_3");
    const auto* scroll = ui3.find("agent.dialog.detail.msgscroll");
    DOCTEST_REQUIRE_MESSAGE(scroll != nullptr, "矮窗展开消息后缺 .msgscroll");
    DOCTEST_CHECK(scroll->frame.height > 0.0f);
    DOCTEST_CHECK_MESSAGE(
        scroll->frame.y + scroll->frame.height <=
            panel->frame.y + panel->frame.height - 23.5f,
        "消息滚动区溢出面板底：底 " +
            std::to_string(scroll->frame.y + scroll->frame.height) +
            " 面板底 " +
            std::to_string(panel->frame.y + panel->frame.height));
    return true;
})

REGISTER_TEST(agent_ui_detail_layout_need339_header_clear_of_cards,
              "detail_layout: need339——头部状态/元信息行完整露在摘要三卡上方（行底 ≤ 卡顶）+ 执行记录行卡范式密度（pad14/gap10/focus 描边）", {
    DetailFixture fx("agentdl", "ovl");
    DOCTEST_REQUIRE(fx.seedTask("遮挡回归任务"));

    eui::Ui ui;
    composeTo(ui, fx.mod, kScreenBig, "layout_ovl_1");
    DOCTEST_REQUIRE(openDetailDialog(ui, fx.mod, std::to_string(fx.taskId)));
    eui::Ui ui2;
    composeTo(ui2, fx.mod, kScreenBig, "layout_ovl_2");

    // ① 头部三段行（kicker/状态/meta，bug154② 落 y=44，行高 lhS）必须整体在
    // 摘要三卡上方：行盒底 ≤ 卡顶。历史缺陷：卡顶写字面 48 压进行盒（44+lhS，
    // 默认档行底 61），卡片在 stack 里后画，把状态/元信息行下半截盖住。
    const auto* kicker = ui2.find("agent.dialog.detail.header.kicker");
    const auto* status = ui2.find("agent.dialog.detail.header.status");
    const auto* meta = ui2.find("agent.dialog.detail.header.meta");
    const auto* lastCard = ui2.find("agent.dialog.detail.summary.last");
    DOCTEST_REQUIRE_MESSAGE(
        kicker != nullptr && status != nullptr && meta != nullptr &&
            lastCard != nullptr,
        "缺头部行或摘要卡");
    const float headerBottom =
        std::max(std::max(kicker->frame.y + kicker->frame.height,
                          status->frame.y + status->frame.height),
                 meta->frame.y + meta->frame.height);
    DOCTEST_CHECK_MESSAGE(
        headerBottom <= lastCard->frame.y + 0.5f,
        "头部文本行底 " + std::to_string(headerBottom) + " 侵入摘要三卡顶 " +
            std::to_string(lastCard->frame.y));

    // ② 行卡范式（schedule 运行历史行卡密度）：行盒 = 2*pad14 + 状态行
    // max(22,lhS)；圆点 x = 内边距 14；行卡与上方日期头间距 = 范式卡距 10；
    // 默认选中行 = .focus 1px primary 描边覆面（替代旧 primarySoftBg 填充 + 左缘条）
    const auto* run0 = ui2.find("agent.dialog.detail.runlist.0");
    const auto* dot0 = ui2.find("agent.dialog.detail.runlist.0.dot");
    const auto* day0 = ui2.find("agent.dialog.detail.runlist.day.0");
    const auto* focus0 = ui2.find("agent.dialog.detail.runlist.0.focus");
    DOCTEST_REQUIRE_MESSAGE(run0 != nullptr && dot0 != nullptr && day0 != nullptr,
                            "缺 runlist 行/圆点/日期头");
    const auto& tokO = core::theme::current();
    const float lhSO =
        static_cast<float>(std::lround(tokO.sizes.fontSizeSmall * 1.4f));
    const float wantRowH = 28.0f + std::max(22.0f, lhSO);
    DOCTEST_CHECK_MESSAGE(nearPx(run0->frame.height, wantRowH),
                          "行卡高应=2*14+状态行（范式密度 " +
                              std::to_string(wantRowH) + "），实测 " +
                              std::to_string(run0->frame.height));
    DOCTEST_CHECK_MESSAGE(nearPx(dot0->frame.x - run0->frame.x, 14.0f),
                          "圆点应落范式内边距 14，实测 " +
                              std::to_string(dot0->frame.x - run0->frame.x));
    DOCTEST_CHECK_MESSAGE(
        nearPx(run0->frame.y - (day0->frame.y + day0->frame.height), 10.0f),
        "行卡间距应=范式 10，实测 " +
            std::to_string(run0->frame.y -
                           (day0->frame.y + day0->frame.height)));
    DOCTEST_REQUIRE_MESSAGE(focus0 != nullptr, "默认选中行缺 .focus 描边覆面");
    return true;
})

}  // namespace
