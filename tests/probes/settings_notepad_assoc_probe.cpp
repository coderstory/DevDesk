// settings_notepad_assoc_probe.cpp — F-18「记事本文件关联」卡：几何 + 四态截图
// 探针（need263 批 4 棒 B；接管退役 settings_md_assoc_probe.cpp，几何契约承接）
//
// 形态 = 卡直绘（不经真模块）：构造 SettingsCardsIn 直调 drawNotepadAssocCard，
// 四态注入可控（真模块态来自真注册表，probe 不可控也不可写——测试红线：
// 用例禁触开发机真实关联登记；isDefaultFor/overallState 只读，本探针零写路径）。
//
// 覆盖判据（need260 设计 §2.3 / PRD §八 状态集）：
//   - 卡宽 = 半宽列（1000 窗档 (712−8)/2 = 352，need260 §7 配对行）
//   - 标题带：.title（19/600）存在，文案「记事本文件关联」
//   - 开关几何：wswitch track = 44×24 字面值、钉卡右缘（x = 卡宽−72，
//     右缘 = 卡宽 − padX 28，全 tab 开关右锚单一口径）
//   - 状态带四态：关+从未开=无状态带元素（.status/.status.dot 均缺）/
//     开+ours=「已关联」successText / 开+非ours=「未生效」warningText+引导带
//     （含未生效摘要行）/ 关+曾开=「已停止自动维护」textTertiary
//   - 四态各落一张 PNG（--screenshot 目录前缀 + 态名），执行归汇总段
//
// 一处 fail-fast 不可改成「打一行 cerr 就往下跑」。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"

#include "notepad_assoc_card.h"
#include "notepad_assoc_logic.h"
#include "settings_config.h"
#include "settings_ui_cards.h"
#include "settings_ui_logic.h"  // kReminderCardHeight / roundFs
#include "platform/open_settings.h"

#include <core/theme.h>  // setFontScale（130% 档位注入）

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
        DslAppConfig{}.title("devdesk_notepad_assoc_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

constexpr const char* kCardId = "settings.card.notepad_assoc";
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
    // 开 + 聚合回读归本 → 「已关联」
    {"on_ours", true, true, platform::file_association::AssocState::Ours,
     true, false, "已关联"},
    // 开 + 聚合回读他者 → 「未生效」+ 引导行（含未生效摘要）
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
    auto window = probe::makeWindow("notepad_assoc_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }
    // 临时库（不触开发机真库）；本探针零关联写路径（测试红线）
    test_support::TestEnv env("devdesk_notepad_assoc_probe_", "cfg", true);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    bool passed = true;

    for (const CaseSpec& c : kCases) {
        module::settings::SettingsConfig cfg;
        cfg.notepadAssocEnabled = c.enabled;
        cfg.notepadAssocEverEnabled = c.ever;
        std::vector<std::string> noLog;
        std::string noDraft;
        module::settings::SettingsCardsIn in{cfg, noDraft, noLog};
        in.notepadAssocState = c.state;
        // 未生效摘要（on_others 态注入一份仿真快照，驱动引导文案；其余态留空）
        std::vector<std::string> exts = {".md", ".json"};
        if (c.expectGuide) {
            in.notepadExtStates = {platform::file_association::AssocState::Others,
                                   platform::file_association::AssocState::Ours};
            in.notepadGuideText =
                module::settings::notepadAssocFailingSummary(exts,
                                                             in.notepadExtStates);
        }
        module::settings::SettingsCardActions a;  // 直绘不交互，回调空
        const bool guide = module::settings::notepadAssocStatusKind(
                               c.enabled, c.ever, c.state) ==
                           module::settings::NotepadAssocStatusKind::NotEffective;
        const float h = std::round(module::settings::notepadAssocCardHeight(
            guide, platform::open_settings::oneClickAvailable()));

        int failures = 0;
        auto expect = [&](bool ok, const std::string& what) {
            if (!ok) {
                std::cerr << "notepad_assoc_probe[" << c.name << "]: " << what
                          << "\n";
                ++failures;
            }
        };
        eui::Ui* uiPtr = nullptr;
        probe::composeModuleFrame(
            runtime, window, "notepad_assoc_probe",
            static_cast<float>(args.windowW), static_cast<float>(args.windowH),
            [&](eui::Ui& ui, const eui::Screen& screen) {
                (void)screen;
                uiPtr = &ui;
                core::theme::Tokens t = core::theme::current();
                module::settings::CardDrawArgs cardArgs{ui, kCardId, kCardW, h,
                                                        t, 0.0f, 0.0f, in, a};
                module::settings::drawNotepadAssocCard(cardArgs);
            });

        // 几何断言：标题带存在 + 文案换代；开关 44×24 字面值、右缘 = 卡宽 − 28
        const core::dsl::Element* title =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".title")
                             : nullptr;
        expect(title != nullptr, "标题带元素缺失（.title）");
        if (title != nullptr) {
            expect(title->text == "记事本文件关联",
                   "标题文案应为「记事本文件关联」");
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
        if (c.expectStatus && status != nullptr) {
            expect(status->text == c.expectStatusText, "状态字文案不符");
        }
        const core::dsl::Element* statusDot =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".status.dot")
                             : nullptr;
        expect(c.expectStatus ? statusDot != nullptr : statusDot == nullptr,
               c.expectStatus ? "状态点元素应存在" : "状态点元素应不存在");
        // 引导行四态：未生效态存在（含摘要文案）、其余态不存在
        const core::dsl::Element* guideText =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".guide.text")
                             : nullptr;
        expect(c.expectGuide ? guideText != nullptr : guideText == nullptr,
               c.expectGuide ? "引导行应存在（未生效态）"
                             : "引导行应不存在");
        if (c.expectGuide && guideText != nullptr) {
            // 摘要数据源已注入：引导文案应含逐扩展摘要（「1 项未生效」）
            expect(guideText->text.find("1 项未生效") != std::string::npos,
                   "引导文案应含未生效摘要");
        }
        if (failures != 0) passed = false;

        // 四态截图（执行归汇总段：--screenshot <前缀> 时逐态落图）
        if (!args.screenshotPath.empty()) {
            const std::string path =
                args.screenshotPath + std::string("_") + c.name + ".png";
            if (!probe::captureFramePng(window, runtime, path)) {
                std::cerr << "notepad_assoc_probe: 截图落盘失败: " << path << "\n";
                passed = false;
            }
        }
    }

    // ── 130% 字号档·配对行不错轨（承接 md 卡 review P2-5，验 P1 左轨恒 28）──
    // 直绘提醒卡 + notepad 卡（同原点，帧内相对坐标可比对跨卡同锚）：标题左缘恒 28
    // 字面、两卡必须相等（130% 下 roundFs(28)=36——左轨回退 roundFs 包裹即
    // 错轨，P1 病灶）。开关口径 need132 D-4（2910c4df）起分卡：notepad 卡右锚
    // 卡宽−28；提醒卡左锚 label 列——两卡右缘相等口径已随 D-4 退役。
    {
        core::theme::setFontScale(1.30f);
        int failures = 0;
        auto expect = [&](bool ok, const std::string& what) {
            if (!ok) {
                std::cerr << "notepad_assoc_probe[pair_130]: " << what << "\n";
                ++failures;
            }
        };
        module::settings::SettingsConfig cfg;  // off_never：无引导带，纯格律
        std::vector<std::string> noLog;
        std::string noDraft;
        module::settings::SettingsCardsIn in{cfg, noDraft, noLog};
        in.notepadAssocState = platform::file_association::AssocState::Unknown;
        module::settings::SettingsCardActions a;
        eui::Ui* uiPtr = nullptr;
        probe::composeModuleFrame(
            runtime, window, "notepad_assoc_probe",
            static_cast<float>(args.windowW), static_cast<float>(args.windowH),
            [&](eui::Ui& ui, const eui::Screen& screen) {
                (void)screen;
                uiPtr = &ui;
                core::theme::Tokens t = core::theme::current();
                const float hL =
                    std::round(module::settings::kReminderCardHeight * 1.30f);
                module::settings::CardDrawArgs left{
                    ui, "settings.card.reminder", kCardW, hL, t, 0.0f, 0.0f,
                    in, a};
                module::settings::drawReminderCard(left);
                const float hR = std::round(
                    module::settings::notepadAssocCardHeight(false, false) *
                    1.30f);
                module::settings::CardDrawArgs right{ui, kCardId, kCardW, hR,
                                                     t, 0.0f, 0.0f, in, a};
                module::settings::drawNotepadAssocCard(right);
            });
        const core::dsl::Element* titleL =
            uiPtr != nullptr ? uiPtr->find("settings.card.reminder.title")
                             : nullptr;
        const core::dsl::Element* titleR =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".title")
                             : nullptr;
        expect(titleL != nullptr && titleR != nullptr, "两卡标题元素缺失");
        if (titleL != nullptr && titleR != nullptr) {
            expect(floatEq(titleL->frame.x, titleR->frame.x),
                   "130% 档两卡标题左缘错轨（应恒 28 字面）");
            expect(floatEq(titleL->frame.x, 28.0f),
                   "130% 档标题左缘应为 28 字面（roundFs 包裹即漂到 36）");
        }
        const core::dsl::Element* swR =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".sw.track")
                             : nullptr;
        const core::dsl::Element* swL =
            uiPtr != nullptr
                ? uiPtr->find("settings.card.reminder.popup.sw.track")
                : nullptr;
        expect(swL != nullptr && swR != nullptr, "两卡开关轨道元素缺失");
        if (swL != nullptr && swR != nullptr) {
            // need132 D-4：提醒卡开关列左锚（label 112 + 行内距 16，28 左轨
            // 起），44×24 字面不缩放——不再钉卡右缘。
            expect(floatEq(swL->frame.x,
                           28.0f + module::settings::roundFs(112.0f) +
                               module::settings::roundFs(16.0f)),
                   "130% 档提醒卡开关应左锚 label 列（28+roundFs(112)+"
                   "roundFs(16)）");
            expect(floatEq(swL->frame.width, 44.0f),
                   "130% 档提醒卡开关宽应为 44 字面（不缩放）");
            const float rR = swR->frame.x + swR->frame.width;
            expect(floatEq(rR, kCardW - 28.0f),
                   "130% 档 notepad 卡开关右缘应 = 卡宽 − 28（右锚口径）");
        }
        if (failures != 0) passed = false;
        core::theme::setFontScale(1.0f);  // 档位还原（全局单例，防串后续段）
    }

    // ── mac 文字档 600 窄档/130%：盒高随实测行数派生（承接 md 卡 review P2-1）──
    // Windows 恒 oneClick → 文字档经 drawNotepadAssocCard(args, forceTextGuide)
    // 测试缝强制；600 contentW 档半宽列 = (600−8)/2 = 296，130% 下指引实测超
    // 2 行 → 盒高 = 行数 × roundFs(16)。100% 参考几何契约不破：卡高 = 140。
    {
        constexpr float kNarrowCardW = 296.0f;  // 600 档半宽列
        const std::string bareGuide = module::settings::notepadAssocMacGuideText("");
        const int lines100 =
            module::settings::notepadAssocMacGuideLineCount(kCardW, bareGuide);
        if (lines100 < 2) {
            std::cerr << "notepad_assoc_probe[guide_lines]: 100% 参考几何应至少两行, 实际 "
                      << lines100 << "\n";
            passed = false;
        }
        if (std::round(module::settings::notepadAssocCardHeight(true, false, 2)) !=
            std::round(module::settings::kNotepadAssocCardHeightTextGuide)) {
            std::cerr << "notepad_assoc_probe[guide_lines]: 100% 文字档卡高契约应保持 140\n";
            passed = false;
        }
        core::theme::setFontScale(1.30f);
        int failures = 0;
        auto expect = [&](bool ok, const std::string& what) {
            if (!ok) {
                std::cerr << "notepad_assoc_probe[mac_guide_130]: " << what << "\n";
                ++failures;
            }
        };
        // 行数实测必须与卡片同源：卡片画的是摘要+指引组合串（非裸指引）——
        // 摘要取下方 in.notepadGuideText 同一串
        const int lines130 =
            module::settings::notepadAssocMacGuideLineCount(
                kNarrowCardW,
                module::settings::notepadAssocMacGuideText(
                    "2 项未生效：.md .json"));
        expect(lines130 >= 3, "600 窄档/130% 下指引应超两行（盒高派生路径未生效？）");
        // 卡高与生产同式：roundFs(notepadAssocCardHeight(...))——100% 基准 × 1.30 取整
        const float h = std::round(
            module::settings::notepadAssocCardHeight(true, false, lines130) *
            1.30f);
        expect(h > module::settings::kNotepadAssocCardHeightTextGuide + 0.5f,
               "文字档卡高应随行数抬升（>140）");
        module::settings::SettingsConfig cfg;
        cfg.notepadAssocEnabled = true;
        cfg.notepadAssocEverEnabled = true;
        std::vector<std::string> noLog;
        std::string noDraft;
        module::settings::SettingsCardsIn in{cfg, noDraft, noLog};
        in.notepadAssocState = platform::file_association::AssocState::Others;
        in.notepadGuideText = "2 项未生效：.md .json";
        module::settings::SettingsCardActions a;
        eui::Ui* uiPtr = nullptr;
        probe::composeModuleFrame(
            runtime, window, "notepad_assoc_probe",
            static_cast<float>(args.windowW), static_cast<float>(args.windowH),
            [&](eui::Ui& ui, const eui::Screen& screen) {
                (void)screen;
                uiPtr = &ui;
                core::theme::Tokens t = core::theme::current();
                module::settings::CardDrawArgs cardArgs{ui, kCardId,
                                                        kNarrowCardW, h, t,
                                                        0.0f, 0.0f, in, a};
                module::settings::drawNotepadAssocCard(cardArgs,
                                                       /*forceTextGuide=*/true);
            });
        const core::dsl::Element* guideText =
            uiPtr != nullptr ? uiPtr->find(std::string(kCardId) + ".guide.text")
                             : nullptr;
        expect(guideText != nullptr, "mac 文字档引导行元素缺失");
        if (guideText != nullptr) {
            const float wantBox =
                static_cast<float>(lines130) * std::round(16.0f * 1.30f);
            expect(floatEq(guideText->frame.height, wantBox),
                   "引导盒高应 = 实测行数 × roundFs(16)（固定两行盒即红）");
            expect(guideText->frame.height >
                       2.0f * std::round(16.0f * 1.30f) - 0.5f,
                   "盒高必须超过旧固定两行盒防静默裁切");
            expect(guideText->frame.y + guideText->frame.height <= h + 0.5f,
                   "引导盒底不得超出卡高（卡高须随行数同步派生）");
        }
        if (failures != 0) passed = false;
        // 截图证据（P2-1 验收：130%/600 窄档第三行可见不裁切）
        if (!args.screenshotPath.empty()) {
            const std::string path =
                args.screenshotPath + std::string("_mac_guide_130_narrow.png");
            if (!probe::captureFramePng(window, runtime, path)) {
                std::cerr << "notepad_assoc_probe: 截图落盘失败: " << path << "\n";
                passed = false;
            }
        }
        core::theme::setFontScale(1.0f);
    }

    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "notepad_assoc_probe: FAILED\n";
        return 1;
    }
    std::cout << "notepad_assoc_probe: OK（四态几何+截图判据全过）\n";
    return 0;
}
