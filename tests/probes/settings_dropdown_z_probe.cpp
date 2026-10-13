// settings_dropdown_z_probe.cpp — settings dropdown 弹层 z 序 / 命中序探针
//
// 覆盖两类同源缺陷：
//   A. 跨卡等值 z 碰撞（外观卡 zIndex 40 vs 编辑器字号卡 30）
//   B. 卡内兄弟等值 z 碰撞（桌宠卡「皮肤」弹层被下方「大小」行盖住）
//
// 判据要点：
//   - 稳定排序按子树 z 上界排兄弟（core/dsl.h rebuildOrderedChildren），上界**相等**
//     时保持 DOM 序 → DOM 序靠后的元素盖住靠前的。两个下拉的等值 z 因此必须靠
//     assertDrawnAbove 在 LCA 处判「严格大于」，光靠叶子扫描抓不住（B 类的相交
//     叶子自身 subtreeMax 常为 0，永不触发）。
//   - dropdown 关闭态 popup 仍 compose 在树里（opacity=0 隐没），所以「已关闭」
//     **不得**用存在性断言；判据 = popup 根 opacity==0 或 .popup.hit disabled。
//   - 弹层链上界需沿祖先链求值（Element 无 parent 指针），harness 内部自建映射。
//   - 输入走真命中分发（press/release → update → 立即 recompose）：弹层在 compose
//     时才按 open 状态进出 DOM，漏 recompose 断言全落空。
//
// 两处 fail-fast 都不可改成「打一行 cerr 就往下跑」：
//   - 皮肤夹具（DEVDESK_PETS_DIR → 临时 4 皮肤目录）。不设则读到本机真实 appData
//     皮肤清单，用例 B 的几何前提（4 项弹层 148px、首项与「大小」行相交）不再
//     成立 → 非 hermetic，夹具失准时探针仍可能绿。创建失败即置红 + 提前结束。
//   - 配置预置（enabledModules 显式含 pet）。预置没落库还往下跑，症状是
//     「settings.card.pet missing」——报错指着几何 / z 判据，真原因在上游预置，
//     排查方向整个跑偏。故当场读回校验，失败即置红 + 提前结束。

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

    // 配置隔离：enabledModules 预置进 app_config 表（不再有 config.json 文件），写进
    // ScopedConfigPath 已注册为进程级 appDatabase 的那份临时库——生产 settings 模块
    // onStart 正是经 core::appDatabase() 取连接，故与生产读路径同一条，不是模拟。
    test_support::ScopedConfigPath scopedCfg(
        test_support::tempConfigFile("pfx_", "cfg"));
    if (!test_support::seedAppConfigJson(
            module::settings::SettingsStore::keyNamespace(),
            "{\"enabledModules\":[\"pet\"]}")) {
        std::cerr << "settings_dropdown_z_probe: 配置预置失败 —— app_config 表的 '"
                  << module::settings::SettingsStore::keyNamespace()
                  << "enabledModules' 行没写进去（用例 5 依赖 enabledModules "
                     "显式含 pet，否则桌宠卡整卡不出现）。置红并中止。\n";
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 5;
    }

    // 皮肤夹具：DEVDESK_PETS_DIR → 临时目录 4 个空皮肤（discoverSkins 只要求
    // pet.json 存在），弹层 4×34+12=148px；不设则读到本机真实 appData 皮肤
    // 清单（用例 5 的弹层高度/首项几何不确定）
    const std::filesystem::path petsDir = test_support::makeTempDir("pfx_", "pets");
    bool petsFixtureOk = true;
    {
        std::error_code ec;
        std::filesystem::create_directories(petsDir, ec);
        petsFixtureOk = !ec;
        for (int i = 0; petsFixtureOk && i < 4; ++i) {
            const std::string name =
                "skin-" + std::string(1, static_cast<char>('a' + i));
            const std::filesystem::path dir = petsDir / name;
            std::filesystem::create_directories(dir, ec);
            petsFixtureOk = !ec;
            if (petsFixtureOk) {
                std::ofstream pf(dir / "pet.json", std::ios::binary);
                pf << "{}";
                petsFixtureOk = pf.good();
            }
        }
    }
    // 夹具失败 = 置红并中止，绝不回落真实数据（见文件头）。清理照做，不留残骸。
    bool fixtureFailed = false;
    if (petsFixtureOk) {
        test_support::setEnvVar("DEVDESK_PETS_DIR", petsDir.string());
    } else {
        std::cerr << "settings_dropdown_z_probe: 皮肤夹具创建失败 —— 用例 5 依赖"
                     "固定 4 皮肤夹具，缺失时只能读到本机真实 appData 皮肤清单"
                     "（非 hermetic）。置红并中止，不回落真实数据。\n";
        std::error_code ec;
        std::filesystem::remove_all(petsDir, ec);
        fixtureFailed = true;
    }
    if (fixtureFailed) {
        test_support::setEnvVar("DEVDESK_PETS_DIR", "");
        core::window::destroyWindow(window);   // 与正常收尾同款（此时 runtime 未起）
        probe::terminateWindowBackend();
        std::cerr << "FAIL\n";
        return 1;
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

    // 3) 外观卡字号下拉：开 → L2 扫描 → 关
    passed = verifyDropdown("settings.card.appearance.font.dd") && passed;

    // 4) 编辑器字号下拉：同卡同 tab，当「编辑器字号」目标开 → L2 扫描 → 关
    passed = verifyDropdown("settings.card.editor.dd") && passed;

    // 5) 桌宠卡双下拉（卡内等值 z 碰撞，见文件头 B 类）：皮肤/大小两个 ddwrap 同为
    //    卡片根直接子级、子树上界同为字段档 20，稳定排序保持 DOM 序 → 后绘的
    //    「大小」行盖住展开的皮肤弹层。卡片级 zIndex(40) 只救跨卡遮挡，叶子扫描的
    //    相交叶子自身 subtreeMax=0 也抓不住。主判据 = assertDrawnAbove（LCA 处两侧
    //    子级子树上界 900 > 20）+ 重叠区真命中路由（点弹层首项 ≠ 被下方字段截获）。
    {
        composeFrame();
        const core::dsl::Element* tabModules =
            ui != nullptr ? ui->find("settings.tabs.hit.1") : nullptr;
        if (tabModules == nullptr) {
            std::cerr << "settings_dropdown_z_probe: tab 'settings.tabs.hit.1' "
                         "(模块) not found\n";
            passed = false;
        } else {
            clickElement(*tabModules);
            composeFrame();
            const std::string skinDd = "settings.card.pet.skin.dd";
            const std::string skinPopupId = skinDd + ".popup";
            const std::string scaleDd = "settings.card.pet.scale.dd";
            const core::dsl::Element* petCard =
                ui != nullptr ? ui->find("settings.card.pet") : nullptr;
            const core::dsl::Element* skinField =
                ui != nullptr ? ui->find(skinDd + ".field") : nullptr;
            const core::dsl::Element* scaleField =
                ui != nullptr ? ui->find(scaleDd + ".field") : nullptr;
            if (petCard == nullptr) {
                std::cerr << "settings_dropdown_z_probe: 'settings.card.pet' "
                             "missing —— 预置已读回校验过 app_config 的 '"
                          << module::settings::SettingsStore::keyNamespace()
                          << "enabledModules' = [\"pet\"]，故此处为空 = "
                             "模块门控与配置读出之间断了（不再是「预置没生效」）\n";
                passed = false;
            } else if (skinField == nullptr || scaleField == nullptr) {
                std::cerr << "settings_dropdown_z_probe: skin/scale field "
                             "missing\n";
                passed = false;
            } else {
                // 开皮肤下拉
                clickElement(*skinField);
                composeFrame();
                // recompose 重建元素树 → 开前捕获的指针全部作废，逐帧重查
                const core::dsl::Element* popup =
                    ui != nullptr ? ui->find(skinPopupId) : nullptr;
                const core::dsl::Element* item0 =
                    ui != nullptr ? ui->find(skinDd + ".item.0") : nullptr;
                const core::dsl::Element* scaleFieldNow =
                    ui != nullptr ? ui->find(scaleDd + ".field") : nullptr;
                if (popup == nullptr || item0 == nullptr ||
                    scaleFieldNow == nullptr) {
                    std::cerr << "settings_dropdown_z_probe: '" << skinPopupId
                              << "' / '.item.0' / scale field not in tree after "
                                 "open\n";
                    passed = false;
                } else {
                    if (!floatEq(popup->opacity, 1.0f)) {
                        std::cerr << "settings_dropdown_z_probe: '" << skinPopupId
                                  << "' opacity=" << popup->opacity
                                  << " (expected visible 1.0)\n";
                        passed = false;
                    }
                    // 前提守卫：弹层首项必须与下方「大小」字段矩形相交（4 皮肤
                    // 弹层 148px 从皮肤字段底 y+48 铺到 y+196，大小行 y+112..152
                    // 恒在带内）——几何漂移到不相交时本断言先响，防 z 判据落空
                    if (!probe::detail::framesIntersect(*item0, *scaleFieldNow)) {
                        std::cerr << "settings_dropdown_z_probe: 前提失效：弹层"
                                     "首项与「大小」字段不相交（几何漂移，z 序"
                                     "判据对象不存在）item0=("
                                  << item0->frame.x << "," << item0->frame.y
                                  << "," << item0->frame.width << "x"
                                  << item0->frame.height << ") scaleField=("
                                  << scaleFieldNow->frame.x << ","
                                  << scaleFieldNow->frame.y << ","
                                  << scaleFieldNow->frame.width << "x"
                                  << scaleFieldNow->frame.height << ")\n";
                        passed = false;
                    }
                    // 主判据 1：绘制序 —— LCA（桌宠卡根）处 popup 侧子级
                    // (skin.ddwrap) 子树上界必须严格大于字段侧 (scale.ddwrap)
                    if (!probe::assertDrawnAbove(*ui, skinPopupId, scaleDd + ".field")) {
                        std::cerr << "settings_dropdown_z_probe: 绘制序断言失败："
                                     "皮肤弹层未压过下方「大小」字段\n";
                        passed = false;
                    }
                    // 主判据 2：重叠区真命中 —— 点「弹层首项 ∩ 大小字段」交集
                    // 中心（保证点在真重叠区内；项中心可能落在交集外，那样
                    // 命中序根本没被考验）→ 选中皮肤项并收起；「大小」下拉不得
                    // 被误开（pre-fix 命中序同病：点击被 scale.dd.field 截获 →
                    // scale 弹层 .hit enabled → 红）
                    const float ox = std::max(item0->frame.x, scaleFieldNow->frame.x);
                    const float oy = std::max(item0->frame.y, scaleFieldNow->frame.y);
                    const float oRight = std::min(
                        item0->frame.x + item0->frame.width,
                        scaleFieldNow->frame.x + scaleFieldNow->frame.width);
                    const float oBottom = std::min(
                        item0->frame.y + item0->frame.height,
                        scaleFieldNow->frame.y + scaleFieldNow->frame.height);
                    clickPoint((ox + oRight) * 0.5f, (oy + oBottom) * 0.5f);
                    composeFrame();
                    const core::dsl::Element* skinHit =
                        ui != nullptr ? ui->find(skinPopupId + ".hit") : nullptr;
                    const core::dsl::Element* scaleHit =
                        ui != nullptr ? ui->find(scaleDd + ".popup.hit") : nullptr;
                    if (skinHit == nullptr || !skinHit->disabled) {
                        std::cerr << "settings_dropdown_z_probe: 皮肤弹层未收起"
                                     "（.hit disabled="
                                  << (skinHit != nullptr && skinHit->disabled ? 1 : 0)
                                  << "）——弹层首项点击未生效\n";
                        passed = false;
                    }
                    if (scaleHit == nullptr || !scaleHit->disabled) {
                        std::cerr << "settings_dropdown_z_probe: 「大小」下拉被"
                                     "误开（其弹层 .hit disabled="
                                  << (scaleHit != nullptr && scaleHit->disabled ? 1 : 0)
                                  << "）——重叠区点击被下方字段截获\n";
                        passed = false;
                    }
                    // 收尾：把「大小」下拉自身也过一遍开/L2/关（同卡下拉无
                    // 相互遮挡回归）
                    passed = verifyDropdown(scaleDd) && passed;
                }
            }
        }
    }

    mod->onStop();
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    test_support::setEnvVar("DEVDESK_PETS_DIR", "");
    {
        std::error_code ec;
        std::filesystem::remove_all(petsDir, ec);
    }
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}