// settings_md_assoc_probe.cpp — F-10「关联 .md 文件」卡：几何 + 四态截图探针
//
// 形态 = 卡直绘（不经真模块）：构造 SettingsCardsIn 直调 drawMdAssocCard，
// 四态注入可控（真模块态来自真注册表，probe 不可控也不可写——测试红线：
// 用例禁触开发机真实 .md 登记；isDefaultForMd 只读，本探针零写路径）。
//
// 覆盖判据（need260 设计 §2.3 / PRD §八 状态集）：
//   - 卡宽 = 半宽列（1000 窗档 (712−8)/2 = 352，need260 §7 配对行）
//   - 标题带：.title（19/600）存在——原 13px label 已退役
//   - 开关几何：wswitch track = 44×24 字面值、钉卡右缘（x = 卡宽−72，
//     右缘 = 卡宽 − padX 28，全 tab 开关右锚单一口径）
//   - 状态带四态：关+从未开=无状态带元素（.status/.status.dot 均缺）/
//     开+ours=「已关联」successText / 开+非ours=「未生效」warningText+引导带 /
//     关+曾开=「已停止自动维护」textTertiary
//   - 四态各落一张 PNG（--screenshot 目录前缀 + 态名），执行归汇总段
//     （本探针只提供源码与断言，不在编码段运行——汇总编译模式）。
//
// 一处 fail-fast 不可改成「打一行 cerr 就往下跑」。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"

#include "md_assoc_card.h"
#include "md_assoc_logic.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "platform/open_settings.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层实现随 dsl_app_impl.h 自给（glfw_app_main.cpp 只编进主程序）；
// 探针直绘卡片不走 app::compose，空实现仅为满足链接。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const app::DslAppConfig config =
        DslAppConfig{}.title("devdesk_md_assoc_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

constexpr const char* kCardId = "settings.card.md_assoc";
// 半宽列（need260 §7）：1000 窗档页宽 792 → contentW 712 → colW=(712−8)/2=352
constexpr float kCardW = 352.0f;

struct CaseSpec {
    const char* name;
    bool enabled;
    bool ever;
    platform::file_association::AssocState state;
    bool expectStatus;   // 行尾状态字元素应存在
    bool expectGuide;    // 引导行应存在（未生效态）
    const char* expectStatusText;
};

const CaseSpec kCases[] = {
    // 关 + 从未开过 → 无状态字
    {"off_never", false, false, platform::file_association::AssocState::Unknown,
     false, false, ""},
    // 开 + 回读归本 → 「已关联」
    {"on_ours", true, true, platform::file_association::AssocState::Ours,
     true, false, "已关联"},
    // 开 + 回读他者 → 「未生效」+ 引导行
    {"on_others", true, true, platform::file_association::AssocState::Others,
     true, true, "未生效"},
    // 关 + 曾开过 → 「已停止自动维护」
    {"off_ever", false, true, platform::file_association::AssocState::Unknown,
     true, false, "已停止自动维护"},
};

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("md_assoc_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    // 临时库（不触开发机真库）；本探针零注册表写路径（测试红线）
    test_support::TestEnv env("devdesk_md_assoc_probe_", "cfg", true);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    bool passed = true;

    for (const CaseSpec& c : kCases) {
        module::settings::SettingsConfig cfg;
        cfg.mdAssocEnabled = c.enabled;
        cfg.mdAssocEverEnabled = c.ever;
        std::vector<std::string> noLog;
        std::string noDraft;
        module::settings::SettingsCardsIn in{cfg, noDraft, noLog};
        in.mdAssocState = c.state;
        module::settings::SettingsCardActions a;  // 直绘不交互，回调空
        const bool guide = module::settings::mdAssocStatusKind(
                               c.enabled, c.ever, c.state) ==
                           module::settings::MdAssocStatusKind::NotEffective;
        const float h = std::round(module::settings::mdAssocCardHeight(
            guide, platform::open_settings::oneClickAvailable()));

        int failures = 0;
        auto expect = [&](bool ok, const std::string& what) {
            if (!ok) {
                std::cerr << "md_assoc_probe[" << c.name << "]: " << what
                          << "\n";
                ++failures;
            }
        };
        eui::Ui* uiPtr = nullptr;
        probe::composeModuleFrame(
            runtime, window, "md_assoc_probe",
            static_cast<float>(args.windowW), static_cast<float>(args.windowH),
            [&](eui::Ui& ui, const eui::Screen& screen) {
                (void)screen;
                uiPtr = &ui;
                core::theme::Tokens t = core::theme::current();
                module::settings::CardDrawArgs cardArgs{ui, kCardId, kCardW, h,
                                                        t, 0.0f, 0.0f, in, a};
                module::settings::drawMdAssocCard(cardArgs);
            });

        // 几何断言：标题带存在（need260 新增，原 13px label 退役）；
        // 开关 44×24 字面值、右缘 = 卡宽 − padX 28（x = 卡宽−72 右锚单一口径）
        const core::dsl::Element* title =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".title")
                             : nullptr;
        expect(title != nullptr, "标题带元素缺失（.title）");
        if (title != nullptr) {
            expect(title->text == "关联 .md 文件", "标题文案应为「关联 .md 文件」");
        }
        const core::dsl::Element* track =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".sw.track")
                             : nullptr;
        expect(track != nullptr, "开关轨道元素缺失");
        if (track != nullptr) {
            expect(floatEq(track->frame.width, 44.0f), "开关轨道宽应为 44");
            expect(floatEq(track->frame.height, 24.0f), "开关轨道高应为 24");
            expect(floatEq(track->frame.x + track->frame.width,
                           kCardW - 28.0f),
                   "开关应钉卡右缘（右缘 = 卡宽 − padX 28）");
        }
        // 状态带四态：点 + 字存在性 + 文案（None 整带不渲染）
        const core::dsl::Element* status =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".status")
                             : nullptr;
        expect(c.expectStatus ? status != nullptr : status == nullptr,
               c.expectStatus ? "状态字元素应存在" : "状态字元素应不存在");
        const core::dsl::Element* statusDot =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".status.dot")
                             : nullptr;
        expect(c.expectStatus ? statusDot != nullptr : statusDot == nullptr,
               c.expectStatus ? "状态点元素应存在" : "状态点元素应不存在");
        // 引导行四态：未生效态存在、其余态不存在
        const core::dsl::Element* guideText =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".guide.text")
                             : nullptr;
        expect(c.expectGuide ? guideText != nullptr : guideText == nullptr,
               c.expectGuide ? "引导行应存在（未生效态）"
                             : "引导行应不存在");
        if (failures != 0) passed = false;

        // 四态截图（执行归汇总段：--screenshot <前缀> 时逐态落图）
        if (!args.screenshotPath.empty()) {
            const std::string path =
                args.screenshotPath + std::string("_") + c.name + ".png";
            if (!probe::captureFramePng(window, runtime, path)) {
                std::cerr << "md_assoc_probe: 截图落盘失败: " << path << "\n";
                passed = false;
            }
        }
    }

    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "md_assoc_probe: FAILED\n";
        return 1;
    }
    std::cout << "md_assoc_probe: OK（四态几何+截图判据全过）\n";
    return 0;
}
