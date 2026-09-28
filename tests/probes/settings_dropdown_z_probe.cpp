// settings_dropdown_z_probe.cpp — DevDesk probes MVP（spec §4，P0）
// 目标：类 1(a) dropdown z 序碰撞（settings 外观卡 vs 编辑器字号卡等值 z）。
// 回归 1379cc1：外观卡 zIndex(40)（settings_ui_cards.cpp:83）被回退成 30 时，
// 与后绘的编辑器字号卡（zIndex(30)，settings_ui_cards.cpp:244）等值 z + frame
// 相交 → 稳定排序（core/dsl.h:1752-1778）下编辑器字号卡盖住外观卡弹层。
//
// 断言机制（probe_harness.h 的 L2 扫描）：
//   assertNoEqualZOverlap(ui, popupId) —— 与弹层 frame 相交的每个非祖先元素
//   的 subtreeMaxZIndex 必须 < 弹层链上界（= max(弹层子树 max, 弹层→根链上
//   每元素 zIndex)，spec §2.2）；≥ 即 FAIL 打印双方 id + z。
//   当前 master：外观卡链上界 40 > 编辑器字号卡子树 max 30 → 绿（1379cc1
//   已修状态）；回退 zIndex(40)→30 后上界 30 ≡ 编辑器卡 30 → 红（证伪门禁）。
//
// 判定细节：
//   - dropdown 关闭态 popup 仍 compose 在树里（opacity=0 隐没，components/
//     dropdown.h:145-148），故「关闭」不得用存在性断言，判据 = popup 根
//     opacity==0 或 .popup.hit disabled（spec §4 不残留判据）。
//   - 弹层链上界需沿祖先链求值（Element 无 parent 指针），harness 内部自建
//     id→父 映射后扫描（spec §2.2）。
//   - 输入走真命中分发（queue press/release → update → 立即 recompose）：弹层
//     在 compose 时才按 open 状态进/出 DOM，漏 recompose 则断言全落空。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

// app:: 层实现（openWindow/activeWindowEffect 等随 dsl_app_impl.h 自给，
// 与 pet_probes.cpp:63-69 同款注入；glfw_app_main.cpp 只编进 DevDesk 主程序）。
// dsl_app_impl.h 头内 emit 的函数体引用 dslAppConfig/compose 两个符号——
// 探针不建应用级 UI（compose 走 probe::composeModuleFrame 直接驱动 runtime），
// compose 空实现仅满足链接
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_settings_dropdown_z_probe");
    return config;
}
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

constexpr float kWindowEdgeTolerance = 0.5f;  // 窗内边界断言容差（逻辑 px）

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 配置隔离：DEVDESK_CONFIG_PATH 指到临时路径（不落盘），探针与机器本地
    // config.json 无关（onStart 吃真机配置会让卡片增减/几何漂移）
    auto cfg = test_support::tempConfigFile("pfx_", "cfg");
    test_support::ScopedConfigPath scopedCfg(cfg);

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "settings";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 弹层
    // 进出树 + layout，帧即有效，runtime_lifecycle.h:30-33）
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };
    // 真实命中分发：press + release 排队 → 下一 composeFrame 内 update 消费
    auto clickElement = [&](const core::dsl::Element& element) {
        const float cx = element.frame.x + element.frame.width * 0.5f;
        const float cy = element.frame.y + element.frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };

    // 开下拉 → 断言（存在 / 可见 / 在窗内 / L2 z 扫描）→ 关下拉 → 断言不可见
    auto verifyDropdown = [&](const std::string& dropdownId) -> bool {
        bool ok = true;
        const std::string popupId = dropdownId + ".popup";
        composeFrame();
        const core::dsl::Element* field =
            ui != nullptr ? ui->find(dropdownId + ".field") : nullptr;
        if (field == nullptr) {
            std::cerr << "settings_dropdown_z_probe: field '" << dropdownId
                      << ".field' not found\n";
            return false;
        }

        // 开：点 field 中心 → onOpenChange(true) → 模块 openDropdown_ 置位
        clickElement(*field);
        composeFrame();
        const core::dsl::Element* popup =
            ui != nullptr ? ui->find(popupId) : nullptr;
        if (popup == nullptr) {
            std::cerr << "settings_dropdown_z_probe: '" << popupId
                      << "' not in tree after open\n";
            return false;
        }
        const float right = popup->frame.x + popup->frame.width;
        const float bottom = popup->frame.y + popup->frame.height;
        if (!floatEq(popup->opacity, 1.0f)) {
            std::cerr << "settings_dropdown_z_probe: '" << popupId
                      << "' opacity=" << popup->opacity
                      << " (expected visible 1.0)\n";
            ok = false;
        }
        if (popup->frame.x < -kWindowEdgeTolerance ||
            popup->frame.y < -kWindowEdgeTolerance ||
            right > w + kWindowEdgeTolerance ||
            bottom > h + kWindowEdgeTolerance) {
            std::cerr << "settings_dropdown_z_probe: '" << popupId
                      << "' frame (" << popup->frame.x << ","
                      << popup->frame.y << ", " << popup->frame.width << "x"
                      << popup->frame.height << ") escapes window " << w << "x"
                      << h << "\n";
            ok = false;
        }
        // L2 z 碰撞扫描（弹层锚定）：与 popup 相交的每个非祖先元素
        // subtreeMaxZIndex 必须 < 弹层链上界。外观卡 40 / 编辑器字号卡 30
        if (!probe::assertNoEqualZOverlap(*ui, popupId)) {
            std::cerr << "settings_dropdown_z_probe: L2 z-collision scan "
                         "FAILED for '"
                      << popupId << "'\n";
            ok = false;
        }

        // 关：再点 field → onOpenChange(false) → 弹层留在树中但不可见
        const core::dsl::Element* fieldAfter =
            ui != nullptr ? ui->find(dropdownId + ".field") : nullptr;
        if (fieldAfter == nullptr) {
            std::cerr << "settings_dropdown_z_probe: '" << dropdownId
                      << ".field' gone after open\n";
            return false;
        }
        clickElement(*fieldAfter);
        composeFrame();
        const core::dsl::Element* closedPopup =
            ui != nullptr ? ui->find(popupId) : nullptr;
        const core::dsl::Element* closedHit =
            closedPopup != nullptr ? ui->find(popupId + ".hit") : nullptr;
        // 不残留判据（spec §4）：opacity==0（dropdown.h:148 visible）或
        // .popup.hit disabled（dropdown.h:168）
        const bool hidden =
            (closedPopup != nullptr && closedPopup->opacity <= 0.5f) ||
            (closedHit != nullptr && closedHit->disabled);
        if (closedPopup == nullptr || !hidden) {
            std::cerr << "settings_dropdown_z_probe: '" << popupId
                      << "' still visible after close (opacity="
                      << (closedPopup != nullptr ? closedPopup->opacity
                                                 : -1.0f)
                      << ")\n";
            ok = false;
        }
        return ok;
    };

    // 1) 首帧 compose（通用 tab 为默认页，settings_module.cpp:935）
    composeFrame();
    // 2) 切到外观 tab（通用）：真命中点 tab hit 区 → onChange(0)
    const core::dsl::Element* tabHit =
        ui != nullptr ? ui->find("settings.tabs.hit.0") : nullptr;
    if (tabHit == nullptr) {
        std::cerr << "settings_dropdown_z_probe: tab 'settings.tabs.hit.0' "
                     "not found\n";
        passed = false;
    } else {
        clickElement(*tabHit);
        composeFrame();
        if (ui->find("settings.card.appearance") == nullptr ||
            ui->find("settings.card.editor") == nullptr) {
            std::cerr << "settings_dropdown_z_probe: appearance/editor "
                         "cards missing on General tab\n";
            passed = false;
        }
    }

    // 3) 外观卡字号下拉（settings_ui_cards.cpp:154，id .font.dd）：开 →
    //    L2 扫描（链上界 40，盖过下方编辑器字号卡 30）→ 关
    passed = verifyDropdown("settings.card.appearance.font.dd") && passed;

    // 4) 编辑器字号下拉（settings_ui_cards.cpp:262，id .dd）：同卡同 tab，
    //    把它当「编辑器字号」目标开 → L2 扫描（链上界 30）→ 关
    passed = verifyDropdown("settings.card.editor.dd") && passed;

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}