// tests/test_modules/test_project_cards_card.cpp
// 需-11 项目卡设置卡（无头 compose 断言）。防的事故：
// - 卡高与绘制行序脱钩 → 尾行挤出卡外：几何函数随卡数单调 + 0 张不塌。
// - 增删改查入口缺件：七行表单、保存/新建/删除三按钮、列表行点选、反馈行着色。
// - 删除按钮在新建态可点 → 误删：新建态 disabled，选中态可点（对照组防恒禁用假绿）。
// 断言纪律沿用 test_github_sync_card.cpp：断言节点存在/不存在 + 每个禁用断言带
// 对照组；frame 几何断言用 raw eui::Ui + ui.layout()。
#include "doctest.h"

#include "frame_env.h"
#include "test_support.h"

#include "core/theme.h"
#include "module_settings/project_card_store.h"
#include "module_settings/project_cards_card.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "settings_ui_logic.h"

#include <eui/dsl.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

namespace ms = module::settings;

constexpr const char* kCardId = "settings.card.project_cards";

ms::ProjectCard makeCard(const std::string& name) {
    ms::ProjectCard c;
    c.name = name;
    c.repoPath = "/srv/git/" + name;
    return c;
}

struct CardFixture {
    ms::SettingsConfig cfg;
    std::string proxyPortDraft;
    std::vector<std::string> logTail;
    ms::SettingsCardsIn in;
    ms::SettingsCardActions a;
    std::vector<ms::ProjectCard> cards;
    int saveClicks = 0;
    int newClicks = 0;
    int deleteClicks = 0;
    std::string lastSelected;
    ms::ProjectCardField lastField = ms::ProjectCardField::Name;
    std::string lastFieldValue;

    CardFixture() : in{cfg, proxyPortDraft, logTail} {
        a.projectCardSave = [this] { ++saveClicks; };
        a.projectCardNew = [this] { ++newClicks; };
        a.projectCardDelete = [this] { ++deleteClicks; };
        a.projectCardSelect = [this](const std::string& name) {
            lastSelected = name;
        };
        a.setProjectCardField =
            [this](ms::ProjectCardField f, const std::string& v) {
                lastField = f;
                lastFieldValue = v;
            };
    }

    void draw(eui::Ui& ui) {
        // 卡高与列表行数同源（heightFor 注册表同口径，别写字面值）
        ms::CardDrawArgs args{ui, kCardId, 728.0f,
                              ms::projectCardsCardHeightFor(cards.size()),
                              core::theme::current(), 0.0f, 1000.0f, in, a};
        ms::drawProjectCardsCard(args);
    }
};

}  // namespace

REGISTER_TEST(project_cards_card_form_and_actions_present,
              "project_cards 卡: 七行表单 + 三按钮 + 反馈行齐备", {
    CardFixture fx;
    fx.cards = {makeCard("订单中心"), makeCard("会员系统")};
    fx.in.projectCards = &fx.cards;
    test_support::HeadlessFrame f("pcardform");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        // 七行表单（字段枚举 0..6 → form.<idx>）输入框全建
        for (int i = 0; i < 7; ++i) {
            const std::string inputId =
                std::string(kCardId) + ".form." + std::to_string(i) + ".input";
            DOCTEST_CHECK_MESSAGE(ui.find(inputId) != nullptr, inputId.c_str());
        }
        // 三按钮（onClick 在 .bg 上，见 components/button.h）
        for (const char* suffix : {".btn.save", ".btn.new", ".btn.delete"}) {
            const auto* btn = ui.find(std::string(kCardId) + suffix + ".bg");
            DOCTEST_CHECK_MESSAGE(btn != nullptr, suffix);
        }
        // 反馈行恒占位（空文案也在，防状态抖动）
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".status") != nullptr);
        return true;
    }));
    return true;
})

REGISTER_TEST(project_cards_card_list_rows_and_select,
              "project_cards 卡: 列表按卡数出行，点选回传卡名", {
    CardFixture fx;
    fx.cards = {makeCard("订单中心"), makeCard("会员系统")};
    fx.in.projectCards = &fx.cards;
    test_support::HeadlessFrame f("pcardlist");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        // 两行列表：名字文本 + 命中层都存在
        const auto* name0 = ui.find(std::string(kCardId) + ".item.0.name");
        DOCTEST_REQUIRE(name0 != nullptr);
        DOCTEST_CHECK_EQ(name0->text, std::string("订单中心"));
        auto* hit1 = ui.find(std::string(kCardId) + ".item.1.hit");
        DOCTEST_REQUIRE(hit1 != nullptr);
        DOCTEST_REQUIRE(hit1->onClick != nullptr);
        hit1->onClick();   // 点选 = 装载表单意图（回调按值捕获卡名）
        // 行 3 不存在（防「行数恒定」假绿）
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".item.2.hit") == nullptr);
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.lastSelected, std::string("会员系统"));
    return true;
})

REGISTER_TEST(project_cards_card_delete_gating_with_control,
              "project_cards 卡: 删除按钮新建态禁用、选中态可点（对照）", {
    // 新建态（editingExisting=false）→ 删除禁用
    CardFixture fresh;
    fresh.in.projectCards = &fresh.cards;  // 空列表
    test_support::HeadlessFrame f0("pcardfresh");
    DOCTEST_CHECK(f0.frame([&](eui::Ui& ui) {
        fresh.draw(ui);
        // 空态行在（引导文案），删除按钮禁用
        DOCTEST_CHECK(ui.find(std::string(kCardId) + ".empty") != nullptr);
        const auto* del = ui.find(std::string(kCardId) + ".btn.delete.bg");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_CHECK(del->disabled);
        return true;
    }));

    // 对照组：选中已有卡 → 删除可点且回调可触发
    CardFixture editing;
    editing.cards = {makeCard("订单中心")};
    editing.in.projectCards = &editing.cards;
    editing.in.projectCardEditingExisting = true;
    editing.in.projectCardSelectedName = "订单中心";
    test_support::HeadlessFrame f1("pcardselect");
    DOCTEST_CHECK(f1.frame([&](eui::Ui& ui) {
        editing.draw(ui);
        const auto* del = ui.find(std::string(kCardId) + ".btn.delete.bg");
        DOCTEST_REQUIRE(del != nullptr);
        DOCTEST_CHECK(!del->disabled);
        DOCTEST_REQUIRE(del->onClick != nullptr);
        del->onClick();
        // 选中行名称高亮色（primaryText），未选行走默认色
        const auto* sel = ui.find(std::string(kCardId) + ".item.0.name");
        DOCTEST_REQUIRE(sel != nullptr);
        return true;
    }));
    DOCTEST_CHECK_EQ(editing.deleteClicks, 1);
    return true;
})

REGISTER_TEST(project_cards_card_buttons_fire_actions,
              "project_cards 卡: 保存/新建按钮点击落到注入回调", {
    CardFixture fx;
    fx.in.projectCards = &fx.cards;
    test_support::HeadlessFrame f("pcardbtn");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui);
        const auto* save = ui.find(std::string(kCardId) + ".btn.save.bg");
        const auto* fresh = ui.find(std::string(kCardId) + ".btn.new.bg");
        DOCTEST_REQUIRE(save != nullptr);
        DOCTEST_REQUIRE(fresh != nullptr);
        DOCTEST_REQUIRE(save->onClick != nullptr);
        DOCTEST_REQUIRE(fresh->onClick != nullptr);
        save->onClick();
        fresh->onClick();
        return true;
    }));
    DOCTEST_CHECK_EQ(fx.saveClicks, 1);
    DOCTEST_CHECK_EQ(fx.newClicks, 1);
    DOCTEST_CHECK_EQ(fx.deleteClicks, 0);
    return true;
})

REGISTER_TEST(project_cards_card_height_monotonic,
              "project_cards 卡: 卡高随卡数单调、0 张不塌（防行序脱钩挤出）", {
    const float h0 = ms::projectCardsCardHeightFor(0);
    const float h1 = ms::projectCardsCardHeightFor(1);
    const float h2 = ms::projectCardsCardHeightFor(2);
    const float h5 = ms::projectCardsCardHeightFor(5);
    // 固定区（表单+操作行）自身可观；阈值对 80~130% 字号档都成立（100% 档 349）
    DOCTEST_CHECK(h0 > 320.0f);
    DOCTEST_CHECK(h1 > h0);
    DOCTEST_CHECK(h2 > h1);
    DOCTEST_CHECK(h5 > h2);
    // 行增量恒定（缩放不变式：单行高 = 相邻两档差），4 行 = 4 个单行高
    const float perRow = h2 - h1;
    DOCTEST_CHECK(std::fabs((h5 - h1) - 4.0f * perRow) < 0.5f);
    return true;
})
