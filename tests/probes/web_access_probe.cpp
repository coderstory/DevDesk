// web_access_probe.cpp — 设置页「网页访问」专区四态探针（need#490 W6）
//
// 运行：web_access_probe [--screenshot <路径>] [--size WxH]
// 不带 --screenshot = 纯 compose + 断言（零文件）。
// 断言：四态卡（关闭 / 错误 / 未绑定 / 已绑定）专区结构存在、开关/端口/地址/
// 二维码/绑定/自诊区齐备、错误态文案、二维码矩阵按 rect 落树；并采样二维码
// 绘制（~数百 rect）与无 QR 卡的 compose 帧耗时差。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>
#include "core/module_interface.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "web_access_card.h"
#include "module_terminal/web/web_terminal_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层桩（probe 不建应用级 UI，compose 走 probe::composeModuleFrame）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_web_access_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

using module::settings::CardDrawArgs;
using module::settings::SettingsCardActions;
using module::settings::SettingsCardsIn;
using module::settings::SettingsConfig;

constexpr float kCardW = 540.0f;

// 卡绘制：构造只读入参/空回调（探针不点击，回调不触发）。
void drawCard(eui::Ui& ui, const std::string& id,
              const core::WebAccessSnapshot& snap, float w, bool confirm) {
    static SettingsConfig cfg;
    static std::string proxyDraft;
    static std::vector<std::string> logTail;
    SettingsCardsIn in{cfg, proxyDraft, logTail};
    in.webAccess = snap;
    in.webBindingConfirm = confirm;
    SettingsCardActions a;
    const auto& t = core::theme::current();
    CardDrawArgs args{ui, id, w,
                      module::settings::webAccessCardHeight(snap, confirm), t,
                      0.0f, 0.0f, in, a, /*narrow=*/false};
    module::settings::drawWebAccessCard(args);
}

int countPrefixInUi(core::dsl::Ui& ui, const std::string& prefix) {
    int n = 0;
    std::function<void(const core::dsl::Element*)> walk =
        [&](const core::dsl::Element* e) {
            if (e == nullptr) return;
            // 元素 id 带页面 id 前缀（<pageId>.<逻辑 id>），按「包含」计数。
            if (e->id.find(prefix) != std::string::npos) ++n;
            for (const auto& c : e->children) walk(c.get());
        };
    for (const auto& root : ui.roots()) walk(root.get());
    return n;
}

bool requireText(core::dsl::Ui& ui, const std::string& id,
                 const std::string& needle, bool* passed) {
    const auto* e = ui.find(id);
    if (e == nullptr) {
        std::cerr << "web_access_probe: FAIL——缺元素 " << id << "\n";
        *passed = false;
        return false;
    }
    if (!needle.empty() && e->text.find(needle) == std::string::npos) {
        std::cerr << "web_access_probe: FAIL——" << id << " 文案=\""
                  << e->text << "\" 不含 \"" << needle << "\"\n";
        *passed = false;
        return false;
    }
    return true;
}

core::WebAccessSnapshot makeUnbound(int qrSize, std::vector<uint8_t> qr) {
    core::WebAccessSnapshot s;
    s.running = true;
    s.port = 7878;
    s.configuredPort = 7878;
    s.bindState = core::WebBindState::Listening;
    s.addresses = {"http://192.168.1.23:7878", "http://127.0.0.1:7878"};
    s.qrSize = qrSize;
    s.qrMatrix = std::move(qr);
    s.tokenRemainingSec = 272;
    return s;
}

template <typename Fn>
double averageComposeMs(core::dsl::Runtime& runtime,
                        core::window::Handle window, const char* page,
                        float w, float h, Fn&& fn, int iterations) {
    double total = 0.0;
    for (int i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        probe::composeModuleFrame(runtime, window, page, w, h, fn);
        const auto end = std::chrono::steady_clock::now();
        total += std::chrono::duration<double, std::milli>(end - start).count();
    }
    return total / static_cast<double>(iterations);
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("web_access_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    core::dsl::Runtime runtime;
    runtime.initialize(window);
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    // 四态快照
    core::WebAccessSnapshot closed;
    closed.configuredPort = 7878;

    core::WebAccessSnapshot error;
    error.configuredPort = 7878;
    error.bindState = core::WebBindState::Error;
    error.error = "端口 7878 被占用，请更换端口后重试";

    // 未绑定态必须来自真实服务快照（不得自造矩阵）：起服务 → snapshot() 取 QR/地址/倒计时。
    module::terminal::web::WebTerminalService service;
    service.setPortForTest(0);
    auto started = service.start();
    if (started.isFail()) {
        std::cerr << "web_access_probe: FAIL——服务启动失败: " << started.errorMessage() << "\n";
        probe::terminateWindowBackend();
        return 1;
    }
    core::WebAccessSnapshot unbound = service.snapshot();
    if (!unbound.running || unbound.qrSize < 21 || unbound.qrMatrix.empty() ||
        unbound.tokenRemainingSec <= 0) {
        std::cerr << "web_access_probe: FAIL——真实服务快照缺二维码/令牌"
                  << " qrSize=" << unbound.qrSize
                  << " token=" << unbound.tokenRemainingSec << "\n";
        passed = false;
    }
    const int qrSize = unbound.qrSize;

    core::WebAccessSnapshot bound = makeUnbound(0, {});
    bound.qrMatrix.clear();
    bound.tokenRemainingSec = 0;
    bound.bindTime = 1763900000;
    bound.lastLogin = 1763903600;
    bound.onlineCount = 2;

    // 2×2 四态图（截图证据）
    core::dsl::Ui* ui = nullptr;
    auto composeGrid = [&](eui::Ui& uiRef, const eui::Screen&) {
        ui = &uiRef;
        const float colW = (w - 48.0f) / 2.0f;
        uiRef.row("probe.grid").size(w, h).gap(16.0f).content([&] {
            uiRef.column("probe.col1").size(colW, h).gap(16.0f).content([&] {
                drawCard(uiRef, "web.closed", closed, colW, false);
                drawCard(uiRef, "web.unbound", unbound, colW, false);
            });
            uiRef.column("probe.col2").size(colW, h).gap(16.0f).content([&] {
                drawCard(uiRef, "web.error", error, colW, false);
                drawCard(uiRef, "web.bound", bound, colW, true);
            });
        });
    };
    probe::composeModuleFrame(runtime, window, "webprobe", w, h, composeGrid);

    if (ui == nullptr) {
        std::cerr << "web_access_probe: FAIL——compose 未建出 UI 树\n";
        return 1;
    }

    // 专区四部分 + 开关/端口/地址（关闭态）
    requireText(*ui, "web.closed.title", "网页访问", &passed);
    requireText(*ui, "web.closed.switch.label", "启用网页访问", &passed);
    ui->find("web.closed.sw.track") != nullptr ||
        (std::cerr << "web_access_probe: FAIL——缺开关轨道\n", passed = false);
    ui->find("web.closed.port.input") != nullptr ||
        (std::cerr << "web_access_probe: FAIL——关闭态端口不可编辑\n",
         passed = false);
    requireText(*ui, "web.closed.addr.empty", "服务未开启", &passed);
    requireText(*ui, "web.closed.bind.empty", "开启后在此显示二维码", &passed);
    requireText(*ui, "web.closed.diag.title", "自诊信息", &passed);

    // 错误态：红字含「端口」
    requireText(*ui, "web.error.error", "端口", &passed);

    // 未绑定态：地址 + 二维码 + 倒计时
    const auto* addr0 = ui->find("web.unbound.addr.0.text");
    if (addr0 == nullptr || addr0->text.empty()) {
        std::cerr << "web_access_probe: FAIL——未绑定态无地址行\n";
        passed = false;
    }
    const auto* qrBg = ui->find("web.unbound.qr.bg");
    if (qrBg == nullptr) {
        std::cerr << "web_access_probe: FAIL——未绑定态缺二维码底\n";
        passed = false;
    } else if (std::fabs(qrBg->frame.width - 160.0f) > 1.0f ||
               std::fabs(qrBg->frame.height - 160.0f) > 1.0f) {
        std::cerr << "web_access_probe: FAIL——二维码尺寸 "
                  << qrBg->frame.width << "x" << qrBg->frame.height
                  << " ≠ 160x160\n";
        passed = false;
    }
    const int qrRects = countPrefixInUi(*ui, "web.unbound.qr.m.");
    if (qrRects <= 0 || ui->find("web.unbound.qr.m.0.0") == nullptr) {
        std::cerr << "web_access_probe: FAIL——二维码未按 rect 落树（"
                  << qrRects << "）\n";
        passed = false;
    }
    requireText(*ui, "web.unbound.qr.countdown", "剩余", &passed);

    // 已绑定态：状态字 + 删除绑定 + 无二维码
    requireText(*ui, "web.bound.bind.state", "验证器已绑定", &passed);
    ui->find("web.bound.unbind") != nullptr ||
        (std::cerr << "web_access_probe: FAIL——已绑定态缺删除按钮\n",
         passed = false);
    if (ui->find("web.bound.qr.bg") != nullptr) {
        std::cerr << "web_access_probe: FAIL——已绑定态仍渲染二维码\n";
        passed = false;
    }
    requireText(*ui, "web.bound.confirm.text", "确认删除绑定", &passed);

    // 截图（渲染当前已断言的四态树）
    probe::captureIfRequested(args, window, runtime, &passed);

    // QR 绘制耗时采样：未绑定卡（含 ~数百 rect）vs 关闭卡
    auto composeUnbound = [&](eui::Ui& u, const eui::Screen&) {
        drawCard(u, "solo.unbound", unbound, kCardW, false);
    };
    auto composeClosed = [&](eui::Ui& u, const eui::Screen&) {
        drawCard(u, "solo.closed", closed, kCardW, false);
    };
    const double msClosed = averageComposeMs(runtime, window, "timing-a",
                                             kCardW, 900.0f, composeClosed, 10);
    const double msUnbound = averageComposeMs(
        runtime, window, "timing-b", kCardW, 900.0f, composeUnbound, 10);
    std::cout << "web_access_probe: qr size=" << qrSize
              << " darkRects=" << qrRects << " compose closed=" << msClosed
              << "ms unbound=" << msUnbound
              << "ms qrCost=" << (msUnbound - msClosed) << "ms\n";

    (void)service.stop();
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "web_access_probe: FAIL\n";
        return 1;
    }
    std::cout << "web_access_probe: PASS\n";
    return 0;
}
