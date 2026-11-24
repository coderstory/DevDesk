// 「网页访问」设置卡（need#490 W6）无头 compose 断言：四态 + 地址列表 + 倒计时 + 自诊字段。
// 断言节点存在/不存在，不退回状态枚举；二维码矩阵用真实编码器产出（与探针同源）。
#include "doctest.h"

#include <eui/dsl.h>

#include "frame_env.h"
#include "test_support.h"

#include "core/theme.h"
#include "core/web_terminal_service.h"
#include "module_terminal/web/qr/qr_encode.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "web_access_card.h"

#include <string>
#include <vector>

namespace {

namespace ms = module::settings;

struct CardFixture {
    ms::SettingsConfig cfg;
    std::string proxyDraft;
    std::vector<std::string> logTail;
    ms::SettingsCardsIn in{cfg, proxyDraft, logTail};
    ms::SettingsCardActions a;

    void draw(eui::Ui& ui, const std::string& id, const core::WebAccessSnapshot& snap,
              bool confirm = false) {
        in.webAccess = snap;
        in.webBindingConfirm = confirm;
        ms::CardDrawArgs args{ui, id, 540.0f, ms::webAccessCardHeight(snap, confirm),
                              core::theme::current(), 0.0f, 900.0f, in, a, /*narrow=*/false};
        ms::drawWebAccessCard(args);
    }
};

core::WebAccessSnapshot closedSnapshot() {
    core::WebAccessSnapshot s;
    s.configuredPort = 7878;
    return s;
}

core::WebAccessSnapshot errorSnapshot() {
    core::WebAccessSnapshot s;
    s.configuredPort = 7878;
    s.bindState = core::WebBindState::Error;
    s.error = "端口 7878 被占用，请更换端口后重试";
    return s;
}

core::WebAccessSnapshot unboundSnapshot() {
    module::terminal::web::QrMatrix qr;
    if (!module::terminal::web::encode(
            "http://192.168.1.23:7878/#b=cardtest-token-0123456789",
            module::terminal::web::QrEcLevel::L, qr)) {
        throw std::runtime_error("card fixture: QR encode failed");
    }
    core::WebAccessSnapshot s;
    s.running = true;
    s.port = 7878;
    s.configuredPort = 7878;
    s.bindState = core::WebBindState::Listening;
    s.addresses = {"192.168.1.23", "127.0.0.1"};
    s.qrSize = qr.size;
    s.qrMatrix = qr.modules;
    s.tokenRemainingSec = 272;
    return s;
}

core::WebAccessSnapshot boundSnapshot() {
    core::WebAccessSnapshot s;
    s.running = true;
    s.port = 7878;
    s.configuredPort = 7878;
    s.bindState = core::WebBindState::Listening;
    s.addresses = {"192.168.1.23"};
    s.bindTime = 1763900000;
    s.lastLogin = 1763903600;
    s.onlineCount = 2;
    s.tokenRemainingSec = 0;
    return s;
}

} // namespace

REGISTER_TEST(web_access_card_closed_state,
              "web_access_card: 关闭态显示开关/端口/占位二维码与自诊标题", {
    CardFixture fx;
    const auto snap = closedSnapshot();
    test_support::HeadlessFrame f("webaccessclosed");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, "web.closed", snap);
        const auto* title = ui.find("web.closed.title");
        DOCTEST_REQUIRE(title != nullptr);
        DOCTEST_CHECK(title->text.find("网页访问") != std::string::npos);
        DOCTEST_CHECK(ui.find("web.closed.sw.track") != nullptr);
        DOCTEST_CHECK(ui.find("web.closed.port.input") != nullptr);
        const auto* addr = ui.find("web.closed.addr.empty");
        DOCTEST_REQUIRE(addr != nullptr);
        DOCTEST_CHECK(addr->text.find("服务未开启") != std::string::npos);
        const auto* bind = ui.find("web.closed.bind.empty");
        DOCTEST_REQUIRE(bind != nullptr);
        DOCTEST_CHECK(bind->text.find("开启后在此显示二维码") != std::string::npos);
        const auto* diag = ui.find("web.closed.diag.title");
        DOCTEST_REQUIRE(diag != nullptr);
        DOCTEST_CHECK(diag->text.find("自诊信息") != std::string::npos);
        return true;
    }));
    return true;
})

REGISTER_TEST(web_access_card_unbound_state,
              "web_access_card: 未绑定态列出地址/二维码矩阵/倒计时/自诊三行", {
    CardFixture fx;
    const auto snap = unboundSnapshot();
    test_support::HeadlessFrame f("webaccessunbound");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, "web.unbound", snap);
        const auto* addr0 = ui.find("web.unbound.addr.0.text");
        DOCTEST_REQUIRE(addr0 != nullptr);
        DOCTEST_CHECK(addr0->text.find("192.168.1.23") != std::string::npos);
        const auto* chip = ui.find("web.unbound.addr.1.chip");
        DOCTEST_REQUIRE(chip != nullptr);
        DOCTEST_CHECK(chip->text.find("本机") != std::string::npos);
        DOCTEST_CHECK(ui.find("web.unbound.qr.bg") != nullptr);
        DOCTEST_CHECK(ui.find("web.unbound.qr.m.0.0") != nullptr);
        const auto* countdown = ui.find("web.unbound.qr.countdown");
        DOCTEST_REQUIRE(countdown != nullptr);
        DOCTEST_CHECK(countdown->text.find("剩余") != std::string::npos);
        DOCTEST_CHECK(ui.find("web.unbound.diag.port") != nullptr);
        DOCTEST_CHECK(ui.find("web.unbound.diag.online") != nullptr);
        DOCTEST_CHECK(ui.find("web.unbound.diag.last") != nullptr);
        return true;
    }));
    return true;
})

REGISTER_TEST(web_access_card_bound_state,
              "web_access_card: 已绑定态显示状态字/删除按钮/确认行且不再渲染二维码", {
    CardFixture fx;
    const auto snap = boundSnapshot();
    test_support::HeadlessFrame f("webaccessbound");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, "web.bound", snap, /*confirm=*/true);
        const auto* state = ui.find("web.bound.bind.state");
        DOCTEST_REQUIRE(state != nullptr);
        DOCTEST_CHECK(state->text.find("验证器已绑定") != std::string::npos);
        DOCTEST_CHECK(ui.find("web.bound.unbind") != nullptr);
        DOCTEST_CHECK(ui.find("web.bound.qr.bg") == nullptr);
        const auto* confirm = ui.find("web.bound.confirm.text");
        DOCTEST_REQUIRE(confirm != nullptr);
        DOCTEST_CHECK(confirm->text.find("确认删除绑定") != std::string::npos);
        return true;
    }));
    return true;
})

REGISTER_TEST(web_access_card_error_state,
              "web_access_card: 错误态红字原因含端口提示", {
    CardFixture fx;
    const auto snap = errorSnapshot();
    test_support::HeadlessFrame f("webaccesserror");
    DOCTEST_CHECK(f.frame([&](eui::Ui& ui) {
        fx.draw(ui, "web.error", snap);
        const auto* err = ui.find("web.error.error");
        DOCTEST_REQUIRE(err != nullptr);
        DOCTEST_CHECK(err->text.find("端口") != std::string::npos);
        return true;
    }));
    return true;
})
