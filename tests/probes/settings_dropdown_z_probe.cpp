// settings_dropdown_z_probe.cpp — settings dropdown 弹层 z 序 / 命中序探针
//
// 覆盖缺陷：跨卡等值 z 碰撞（外观卡 zIndex 40 vs 编辑器字号卡 30）
//
// 判据要点：
//   - 稳定排序按子树 z 上界排兄弟（core/dsl.h rebuildOrderedChildren），上界**相等**
//     时保持 DOM 序 → DOM 序靠后的元素盖住靠前的。两个下拉的等值 z 因此必须靠
//     assertDrawnAbove 在 LCA 处判「严格大于」，光靠叶子扫描抓不住。
//     叶子自身 subtreeMax 常为 0，永不触发）。
//   - dropdown 关闭态 popup 仍 compose 在树里（opacity=0 隐没），所以「已关闭」
//     **不得**用存在性断言；判据 = popup 根 opacity==0 或 .popup.hit disabled。
//   - 弹层链上界需沿祖先链求值（Element 无 parent 指针），harness 内部自建映射。
//   - 输入走真命中分发（press/release → update → 立即 recompose）：弹层在 compose
//     时才按 open 状态进出 DOM，漏 recompose 断言全落空。
//
// 一处 fail-fast 不可改成「打一行 cerr 就往下跑」。

#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app:: 层内联实现自给（链接需要）
#include "test_support.h"
#include "app_registry.h"
#include "core/module_interface.h"
#include "settings_config.h"  // SettingsStore::keyNamespace()（配置键前缀单一来源）

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

// app:: 层实现随 dsl_app_impl.h 自给（glfw_app_main.cpp 只编进主程序）。dslAppConfig
// 与 compose 是 dsl_app_impl.h 头内 emit 的函数体所引用的两个符号：探针不建应用级 UI，
// compose 走 probe::composeModuleFrame 直接驱动 runtime，故 compose 空实现仅满足链接。
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

    std::unique_ptr<::core::Module> mod(create_settings_module());
    mod->onStart();

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    const std::string pageId = "settings";
    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    bool passed = true;

    core::dsl::Ui* ui = nullptr;
    // 单帧标准单元：pollOnce（输入派发 + 交互）→ compose（recompose 弹层进出树 + layout）
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, pageId, w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      mod->compose(u, screen);
                                  });
    };
    // 真实命中分发：press + release 排队 → 下一 composeFrame 内 update 消费
    auto clickPoint = [&](float cx, float cy) {
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };
    auto clickElement = [&](const core::dsl::Element& element) {
        clickPoint(element.frame.x + element.frame.width * 0.5f,
                   element.frame.y + element.frame.height * 0.5f);
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
        // subtreeMaxZIndex 必须 < 弹层链上界
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
        // 不残留判据：opacity==0 或 .popup.hit disabled
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

    // 1) 首帧 compose（通用 tab 为默认页）
    composeFrame();
    // 2) 切到外观 tab：真命中点 tab hit 区 → onChange(0)
    // WTab（§4.2）hit id = settings.tabs.N.hit（W4 改动）
    const core::dsl::Element* tabHit =
        ui != nullptr ? ui->find("settings.tabs.0.hit") : nullptr;
    if (tabHit == nullptr) {
        std::cerr << "settings_dropdown_z_probe: tab 'settings.tabs.0.hit' "
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

    // 3) 外观卡字号下拉：开 → L2 扫描 → 关
    passed = verifyDropdown("settings.card.appearance.font.dd") && passed;

    // 4) 编辑器字号下拉：同卡同 tab，当「编辑器字号」目标开 → L2 扫描 → 关
    passed = verifyDropdown("settings.card.editor.dd") && passed;

    // 5) 桌宠卡已于 2026-10-13 整模块摘除，此处原来对应的 z 序断言随之移除

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}