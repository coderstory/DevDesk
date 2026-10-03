// settings_cards_probes.cpp — DevDesk probes：设置页卡片断言（probes spec
// 2026-09-28-devdesk-probes-design.md §3-probe-full 的 settings_cards 组）。
//
// 覆盖三类：
//   §A = 类 8(f) / 9(c) 皮肤下拉 clip·openUp：桌宠卡皮肤下拉是六处下拉里
//        唯一曾无 openUp 防护的（settings_ui_cards.cpp:445-457 已修：≥3 皮肤时
//        弹层向下铺开越出 scrollView 视口底被 clip 裁掉末项）。探针：最小窗
//        （--size 1200x720）下开下拉 → 弹层 frame 必须 ⊆ 卡片区 scrollView
//        视口 frame（settings.cards.scroll，scrollview.h:85-88 根即裁剪口）。
//        回归撤销 openUp 判定（dropdownPopupOverflows → 恒 false）后，4 皮肤
//        弹层底 776 > 视口底 ~680，frame 断言必须红 —— 证伪门禁同
//        settings_dropdown_z_probe。
//   §B = 类 4(b) 按钮 theme 泄漏：历史上三个按钮只 .colors() 不 .theme() →
//        ButtonStyle 落到 EUI-NEO 默认深色主题（#3871E0 蓝描边 + 0.22 重阴影，
//        components/theme.h:168-178 默认主色 / button.h:14-15 默认样式）。
//        已修（settings_ui_cards.cpp:506-509 同款注释）。探针：结构发现全部
//        按钮（stack 根 + 直子 <id>.bg（rect）/ <id>.content（row），
//        button.h:135-203），断言 border 色 ∈ {t.colors.primary,
//        t.colors.borderDefault}（≠ EUI 默认蓝）且 shadow alpha ≤ 0.11
//        （浅色档 buttonShadow = 0.10，components/theme.h:262-264）。
//   §C = 类 6(d) 降级提示行：窗口效果降级文案窄窗溢出（settings_ui_cards.cpp:
//        200-220，.effect.hint 无 clip/wrap）→ 断言 hint 文本元素 clip==true
//        或 wrap==true（判别字段 = 修复落点，落点未定则两择一红；当前 master
//        未加 → 红 = 证伪门禁，spec §6(d) 判定口径）。
//
// 搭法前提（确定性都与机器本地配置无关）：
//   - 配置预置写进 app_config 表（批次 E2 之后配置不再有 JSON 文件）——写的是
//     ScopedConfigPath 已注册为进程级 appDatabase 的那份临时库，即生产模块
//     onStart 的**同一条读路径**（settings_module.cpp:193 经 core::appDatabase()）。
//     内容 = themeMode=light 锁浅色 token；windowEffect=frosted 令降级提示非空；
//     enabledModules 显式含 pet/markdown/todo/claude_proxy —— pet 默认关闭，
//     不加则桌宠卡整卡隐藏、皮肤下拉不存在；pet_config 行指长名皮肤（§③）。
//     每处预置都**当场读回校验**，失败即置红退出，不带着没落地的配置往下跑断言
//     （E2 回归当初正是预置静默失效、症状报在「card.pet missing」上）；
//   - DEVDESK_PETS_DIR 指到临时目录 + 4 个含 pet.json 的空皮肤目录
//     （discoverSkins 只要求 pet.json 存在，pet_skin.cpp:210-228）——
//     4 皮肤让弹层 4×34+12=148px 高（settings_ui_logic.h:101-103），
//     最小窗下判定几何明确越界（触发 openUp）。
//
// 运行：settings_cards_probes.exe [--size 1200x720]（最小窗 1200×720 是
// §A/§C 的判别尺寸；默认 1200×1000 下弹层向下展开也在视口内，同样绿）。
//   --dump <prefix>：设置页缩放两档落图（§D）——通用 tab 在 fontScale 100% /
//   130% 各真渲染一帧，GL 后备缓冲 glReadPixels 读回写 <prefix>_100/_130.ppm
//   （P6，PIL 可读；about_probe 同款手法，观感复核不依赖 screencapture——
//   OpenGL 窗口 screencapture 有旧帧坑且锁屏下不可用）。

// GL / GLFW 头由 probe_harness 统一给（2026-10-03 帧捕获批次：harness 改走 glad
// 路线直接提供 glReadPixels 等 GL 符号，而 glad 与平台 GL 头互斥——同时包含会
// 撞 glad.h 的 "OpenGL header already included" #error。这里不再单独 include
// GLFW：harness 已带 <GLFW/glfw3.h> + <glad/glad.h>，--dump 的 glReadPixels 照常可用）
#include "probes/probe_harness.h"
#include "test_support.h"

#include "settings_config.h"     // SettingsStore::keyNamespace()（配置键前缀单一来源）
#include "settings_ui_logic.h"  // ②③ 判据：文案宽度估算 / 下拉 chrome 常量
#include "module_pet/pet_config.h"  // kPetConfigKey（桌宠配置行键单一来源）

#include "app_registry.h"
#include "core/module_interface.h"
#include "core/render/render_backend.h"  // §D --dump：manual 渲染
#include "core/theme.h"

#include <eui/app.h>
#include <eui/detail/dsl_app_impl.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// app:: 层实现（openWindow/activeWindowEffect 等随 dsl_app_impl.h 自给，
// 与 test_settings.cpp:42-49 同款注入；glfw_app_main.cpp 只编进 DevDesk 主程序）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_settings_cards_probe");
    return config;
}
// app::compose —— 应用级 UI 补全。探针不建主窗口应用树（compose 走
// probe::composeModuleFrame 直接驱动 runtime），空实现只满足 dsl_app_impl.h
// 头内 emit 的函数体对该符号的引用（链接需要）
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

namespace {

// ---- 断言小件 ----

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

// RGB 三通道近似（忽略 alpha——primary 按钮描边是 withAlpha(primary,0.58)）
bool channelsClose(const eui::Color& a, const eui::Color& b, float eps) {
    return std::fabs(a.r - b.r) < eps && std::fabs(a.g - b.g) < eps &&
           std::fabs(a.b - b.b) < eps;
}

// EUI-NEO 默认主色（components/theme.h:141-143 defaultPrimary = #3871E0）
const eui::Color kEuiDefaultBlue(0.2196f, 0.4431f, 0.8784f, 1.0f);

// bool containsStr(const std::string& haystack, const std::string& needle) {
//     return haystack.find(needle) != std::string::npos;
// }

// ---- 探针夹具 ----

// 写探针配置：浅色档（确定性 token）+ frosted（降级提示非空）+ 四模块启用
// **落点是 app_config 表**（批次 E2 之后配置不再有 config.json 文件）：一段
// 整份 JSON 文本按顶层键分发进 SettingsStore 命名空间的各行，写进已注册为
// 进程级 appDatabase 的临时库 —— 生产 settings 模块 onStart 正是经
// core::appDatabase() 读这些行（settings_module.cpp:193），故这是**生产读
// 路径本身**，不是模拟。前缀取 SettingsStore::keyNamespace() 而非写死字面量：
// 键前缀改名时探针跟着走，不留一处「注释说改了、探针还写旧前缀」的漂移。
bool writeProbeConfig() {
    return test_support::seedAppConfigJson(
        module::settings::SettingsStore::keyNamespace(),
        "{\"themeMode\":\"light\",\"windowEffect\":\"frosted\","
        "\"enabledModules\":[\"pet\",\"markdown\",\"todo\",\"claude_proxy\"]}");
}

// 探针皮肤夹具：DEVDESK_PETS_DIR 下建 4 个含 pet.json 的皮肤目录
bool makeFakeSkins(const std::filesystem::path& petsDir) {
    std::error_code ec;
    std::filesystem::create_directories(petsDir, ec);
    if (ec) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        const std::string name =
            "skin-" + std::string(1, static_cast<char>('a' + i));
        const std::filesystem::path dir = petsDir / name;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return false;
        }
        std::ofstream f(dir / "pet.json", std::ios::binary | std::ios::trunc);
        if (!f) {
            return false;
        }
        f << "{}";
        if (!f.good()) {
            return false;
        }
    }
    return true;
}

// ---- 按钮结构发现与主题断言（§B）----
// 按钮 = stack 根，直子同时含 <id>.bg（rect，边框/阴影挂它身上）与
// <id>.content（row）。settings 页其它组件（switch/checkbox/input/dropdown）
// 都没有这个双子组合（switch.h:89-114 / checkbox.h:130-161 / input.h:189-199）。
void walkButtons(const core::dsl::Element& node,
                 std::vector<const core::dsl::Element*>& buttons) {
    bool hasBg = false;
    bool hasContent = false;
    for (const auto& child : node.children) {
        if (child->id == node.id + ".bg") {
            hasBg = true;
        }
        if (child->id == node.id + ".content") {
            hasContent = true;
        }
        walkButtons(*child, buttons);
    }
    if (hasBg && hasContent) {
        buttons.push_back(&node);
    }
}

bool verifyButtonTheme(const core::dsl::Element& button,
                       const core::theme::Tokens& t) {
    const core::dsl::Element* bg = nullptr;
    for (const auto& child : button.children) {
        if (child->id == button.id + ".bg") {
            bg = child.get();
            break;
        }
    }
    if (bg == nullptr) {
        std::cerr << "settings_cards_probes: button '" << button.id
                  << "' has no '.bg' child\n";
        return false;
    }
    bool ok = true;
    if (bg->border.width <= 0.0f) {
        std::cerr << "settings_cards_probes: button '" << button.id
                  << "' border width=" << bg->border.width
                  << " (expected > 0)\n";
        ok = false;
    }
    const eui::Color& bc = bg->border.color;
    const bool isThemePrimary = channelsClose(bc, t.colors.primary, 0.02f);
    const bool isThemeBorder =
        channelsClose(bc, t.colors.borderDefault, 0.02f);
    const bool isEuiBlue = channelsClose(bc, kEuiDefaultBlue, 0.02f);
    if (!(isThemePrimary || isThemeBorder)) {
        std::cerr << "settings_cards_probes: button '" << button.id
                  << "' border color (" << bc.r << "," << bc.g << "," << bc.b
                  << ") is neither t.colors.primary (" << t.colors.primary.r
                  << "," << t.colors.primary.g << "," << t.colors.primary.b
                  << ") nor t.colors.borderDefault ("
                  << t.colors.borderDefault.r << "," << t.colors.borderDefault.g
                  << "," << t.colors.borderDefault.b
                  << ") — EUI-NEO default theme leaked into the button (blue "
                     "border " << bc.r << "," << bc.g << "," << bc.b << ")\n";
        ok = false;
    }
    if (isEuiBlue) {
        std::cerr << "settings_cards_probes: button '" << button.id
                  << "' border is EUI default blue #3871E0 (" << bc.r << ","
                  << bc.g << "," << bc.b << ")\n";
        ok = false;
    }
    if (!bg->shadow.enabled || bg->shadow.color.a > 0.11f) {
        std::cerr << "settings_cards_probes: button '" << button.id
                  << "' shadow enabled=" << (bg->shadow.enabled ? 1 : 0)
                  << " alpha=" << bg->shadow.color.a
                  << " (light token expects ~0.10, <= 0.11; EUI default "
                     "buttonShadow is 0.22)\n";
        ok = false;
    }
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 配置隔离：预置浅色 + frosted + 四模块启用（落点 app_config 表，见
    // writeProbeConfig），与机器本地配置无关。预置失败即退出 3——带着没落地的
    // 配置往下跑，症状会报在「桌宠卡不在树上」这类下游元素上（E2 回归现场）。
    test_support::ScopedConfigPath scopedCfg(
        test_support::tempConfigFile("pfx_", "cfg"));
    if (!writeProbeConfig()) {
        std::cerr << "settings_cards_probes: 配置预置失败 —— app_config 表的 '"
                  << module::settings::SettingsStore::keyNamespace()
                  << "' 命名空间（themeMode/windowEffect/enabledModules）没写进去\n";
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }

    // 皮肤夹具：DEVDESK_PETS_DIR → 临时目录（4 个空皮肤），析构还原并清理
    const std::filesystem::path petsDir =
        test_support::makeTempDir("pfx_", "pets");
    if (!makeFakeSkins(petsDir)) {
        std::cerr << "settings_cards_probes: 皮肤夹具创建失败\n";
        std::error_code ec;
        std::filesystem::remove_all(petsDir, ec);
        return 4;
    }
    test_support::setEnvVar("DEVDESK_PETS_DIR", petsDir.string());

    // ③ 长名皮肤夹具（2026-09-29 真机反馈「桌宠设置的下拉框不应该换行」）：
    // 25 字符名在 240 宽下拉里文本会画出控件右边缘（真机复现），故额外造一个
    // 长名皮肤并把当前皮肤指向它。**当前皮肤的落点是 app_config 的 pet_config
    // 行**（批次 E2 之后 petsDir()/pet_config.json 已退役：不读、不写、不迁移），
    // 键名取 kPetConfigKey 常量而非字面量，与生产 currentSkinName() 同一来源。
    // 预置失败即退出 5：长名皮肤没生效时，下面「字段宽 ≥ 最长名所需宽」那条断言
    // 会拿短名去比，看着像布局回归，实则是夹具没生效。
    const std::string kLongSkin = "super-long-skin-name-here";
    {
        std::error_code skinEc;
        std::filesystem::create_directories(petsDir / kLongSkin, skinEc);
        std::ofstream(petsDir / kLongSkin / "pet.json", std::ios::binary)
            << "{}";
        if (!test_support::seedAppConfigRow(
                module::pet::kPetConfigKey,
                "{\"skinName\":\"" + kLongSkin + "\",\"x\":-1,\"y\":-1}")) {
            std::cerr << "settings_cards_probes: 桌宠配置预置失败 —— app_config "
                         "表的 '"
                      << module::pet::kPetConfigKey << "' 行没写进去\n";
            std::error_code ec;
            std::filesystem::remove_all(petsDir, ec);
            core::releaseInputQueue(window);
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 5;
        }
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

    // ============ §C 类 6(d)：降级提示行（通用 tab 默认页） ============
    composeFrame();
    {
        const core::dsl::Element* hint =
            ui != nullptr ? ui->find("settings.card.appearance.effect.hint")
                          : nullptr;
        if (hint == nullptr) {
            // cfg.windowEffect=frosted + 探针 app::activeWindowEffect()=None
            // → windowEffectDegradedHint 恒非空（settings_config.cpp:201-232）——
            // 提示行不在树里 = 降级文案通道断掉（该类断言前提失效也应红）
            std::cerr << "settings_cards_probes: '"
                         "settings.card.appearance.effect.hint' not found "
                         "(frosted config + app default None → hint 应非空)\n";
            passed = false;
        } else {
            // 判别字段 = 修复落点：加 .clip() → clip==true（dsl.h:124 默认
            // false）；加 .wrap() → wrap==true（dsl.h:149 默认 false）。
            // 当前 master 两者皆 false → 本组红 = 证伪门禁（spec 类 6(d)）
            if (!hint->clip && !hint->wrap) {
                std::cerr << "settings_cards_probes: 降级提示行 "
                          << "'settings.card.appearance.effect.hint'"
                          << " clip=" << (hint->clip ? 1 : 0)
                          << " wrap=" << (hint->wrap ? 1 : 0)
                          << "（窄窗可用宽不足时无裁切/换行 → 溢出；修复应置 "
                             "其一为 true）\n";
                passed = false;
            }
        }
    }

    // ============ §A 类 8(f)/9(c)：皮肤下拉 openUp（模块 tab） ============
    const core::dsl::Element* tabModules = nullptr;
    {
        composeFrame();
        tabModules = ui != nullptr ? ui->find("settings.tabs.hit.1") : nullptr;
    }
    if (tabModules == nullptr) {
        std::cerr << "settings_cards_probes: tab 'settings.tabs.hit.1' "
                     "(模块) not found\n";
        passed = false;
    } else {
        clickElement(*tabModules);
        composeFrame();
        const core::dsl::Element* petCard =
            ui != nullptr ? ui->find("settings.card.pet") : nullptr;
        if (petCard == nullptr) {
            std::cerr << "settings_cards_probes: 'settings.card.pet' not on "
                         "Modules tab（enabledModules 缺 pet？）\n";
            passed = false;
        } else {
            const std::string ddId = "settings.card.pet.skin.dd";
            const std::string popupId = ddId + ".popup";
            const core::dsl::Element* field =
                ui != nullptr ? ui->find(ddId + ".field") : nullptr;
            if (field == nullptr) {
                std::cerr << "settings_cards_probes: '" << ddId
                          << ".field' not found\n";
                passed = false;
            } else {
                // ③ 单行不溢出（2026-09-29 真机反馈「桌宠设置的下拉框不应该
                //    换行」）：EUI-NEO text 默认 wrap=false，超宽名不折行而是画
                //    出控件右边缘。判据 = ①控件宽度按最长皮肤名放宽（≥ 估算宽 +
                //    字段 chrome，改回固定 240 即红）；②字段文案估算宽 ≤ 可用宽；
                //    ③文案无换行符。估算口径与生产同一纯件（settings_ui_logic）。
                const core::dsl::Element* skinLabel =
                    ui != nullptr ? ui->find(ddId + ".label") : nullptr;
                if (skinLabel == nullptr) {
                    std::cerr << "settings_cards_probes: '" << ddId
                              << ".label' not found\n";
                    passed = false;
                } else {
                    namespace ms = module::settings;
                    // ③-0 夹具生效性守卫（E2 回归修复新增，变异验证的反面
                    //     教材）：皮肤字段文案**必须**等于预置的长名。它同时是
                    //     「生产 currentSkinName() 真从 app_config 的
                    //     pet_config 行读到了我们写的那一行」的唯一证据。
                    //     缺这条，长名夹具就是装饰品——预置一旦失效，label
                    //     退回默认皮肤（短名），下面 ③②「文案估算宽 ≤ 可用宽」
                    //     就变成拿 6 字符去比 25 字符的余量，**恒绿**：探针在
                    //     它最该抓的那个回归（长名画出控件右边缘，真机复现）
                    //     面前退化成恒真断言。恒真即假绿，必须重写而非删。
                    if (skinLabel->text != kLongSkin) {
                        std::cerr << "settings_cards_probes: 皮肤字段文案 '"
                                  << skinLabel->text << "' != 预置长名 '"
                                  << kLongSkin
                                  << "' —— app_config 的 pet_config 行没被生产"
                                     "读出（currentSkinName 回落到默认皮肤），"
                                     "长名夹具失效\n";
                        passed = false;
                    }
                    const float fieldW = field->frame.width;
                    const float needW =
                        ms::estimatedTextWidth(kLongSkin, skinLabel->fontSize) +
                        ms::kDropdownFieldChrome;
                    if (fieldW < needW - 0.5f) {
                        std::cerr << "settings_cards_probes: 皮肤下拉宽 "
                                  << fieldW << " < 最长皮肤名所需 " << needW
                                  << "（长名会画出控件外）\n";
                        passed = false;
                    }
                    if (fieldW > ms::kPetSkinFieldMaxWidth + 0.5f) {
                        std::cerr << "settings_cards_probes: 皮肤下拉宽 "
                                  << fieldW << " 超上限 "
                                  << ms::kPetSkinFieldMaxWidth << "\n";
                        passed = false;
                    }
                    const float labelW = ms::estimatedTextWidth(
                        skinLabel->text, skinLabel->fontSize);
                    if (skinLabel->text.find('\n') != std::string::npos ||
                        labelW > fieldW - ms::kDropdownFieldChrome + 0.5f) {
                        std::cerr << "settings_cards_probes: 皮肤下拉字段文案 '"
                                  << skinLabel->text << "' 估算宽 " << labelW
                                  << " > 可用宽 "
                                  << (fieldW - ms::kDropdownFieldChrome)
                                  << "（溢出 = 压 chevron / 读出「换行」）\n";
                        passed = false;
                    }
                    for (std::size_t i = 0;; ++i) {
                        const core::dsl::Element* item = ui->find(
                            ddId + ".item.label." + std::to_string(i));
                        if (item == nullptr) {
                            break;
                        }
                        const float itemW = ms::estimatedTextWidth(
                            item->text, item->fontSize);
                        if (itemW > fieldW - ms::kDropdownItemChrome + 0.5f) {
                            std::cerr << "settings_cards_probes: 皮肤选项 '"
                                      << item->text << "' 估算宽 " << itemW
                                      << " 超出弹层项可用宽 "
                                      << (fieldW - ms::kDropdownItemChrome)
                                      << "\n";
                            passed = false;
                        }
                    }
                }
                // 开下拉（点 field 中心 → onOpenChange(true)）
                clickElement(*field);
                composeFrame();
                const core::dsl::Element* popup =
                    ui != nullptr ? ui->find(popupId) : nullptr;
                if (popup == nullptr) {
                    std::cerr << "settings_cards_probes: '" << popupId
                              << "' not in tree after open\n";
                    passed = false;
                } else {
                    if (!floatEq(popup->opacity, 1.0f)) {
                        std::cerr << "settings_cards_probes: '" << popupId
                                  << "' opacity=" << popup->opacity
                                  << " (expected visible 1.0)\n";
                        passed = false;
                    }
                    const float right = popup->frame.x + popup->frame.width;
                    const float bottom = popup->frame.y + popup->frame.height;
                    if (popup->frame.x < -0.5f || popup->frame.y < -0.5f ||
                        right > w + 0.5f || bottom > h + 0.5f) {
                        std::cerr << "settings_cards_probes: '" << popupId
                                  << "' frame (" << popup->frame.x << ","
                                  << popup->frame.y << ", "
                                  << popup->frame.width << "x"
                                  << popup->frame.height
                                  << ") escapes window " << w << "x" << h
                                  << "\n";
                        passed = false;
                    }
                    // 类 8(f) 判据：弹层 frame ⊆ 卡片区 scrollView 视口
                    //（settings.cards.scroll 根即裁剪口，scrollview.h:85-88）。
                    // 最小窗 1200×720 下若无 openUp，4 皮肤弹层向下铺到
                    // 底 776 > 视口底 ~680 → 越出 → 本断言之红即抓回归
                    const core::dsl::Element* viewport =
                        ui != nullptr ? ui->find("settings.cards.scroll")
                                      : nullptr;
                    if (viewport == nullptr) {
                        std::cerr << "settings_cards_probes: '"
                                     "settings.cards.scroll' not found\n";
                        passed = false;
                    } else if (popup->frame.x < viewport->frame.x - 0.5f ||
                               popup->frame.y < viewport->frame.y - 0.5f ||
                               right > viewport->frame.x +
                                           viewport->frame.width + 0.5f ||
                               bottom > viewport->frame.y +
                                            viewport->frame.height + 0.5f) {
                        std::cerr << "settings_cards_probes: '" << popupId
                                  << "' frame (" << popup->frame.x << ","
                                  << popup->frame.y << ", "
                                  << popup->frame.width << "x"
                                  << popup->frame.height
                                  << ") escapes tab viewport ("
                                  << viewport->frame.x << ","
                                  << viewport->frame.y << ", "
                                  << viewport->frame.width << "x"
                                  << viewport->frame.height
                                  << ") — openUp/up 方向防护失效？\n";
                        passed = false;
                    }
                    // L2 z 碰撞扫描（皮肤卡整卡 z40 抬层，settings_ui_cards.cpp:
                    // 406-410）：弹层链上界 40 盖过下方目录/代理卡
                    if (!probe::assertNoEqualZOverlap(*ui, popupId)) {
                        std::cerr << "settings_cards_probes: L2 z-collision "
                                     "scan FAILED for '"
                                  << popupId << "'\n";
                        passed = false;
                    }
                }
                // 关下拉（再点 field → onOpenChange(false)）
                const core::dsl::Element* fieldAfter =
                    ui != nullptr ? ui->find(ddId + ".field") : nullptr;
                if (fieldAfter == nullptr) {
                    std::cerr << "settings_cards_probes: '" << ddId
                              << ".field' gone after open\n";
                    passed = false;
                } else {
                    clickElement(*fieldAfter);
                    composeFrame();
                    const core::dsl::Element* closedPopup =
                        ui != nullptr ? ui->find(popupId) : nullptr;
                    const core::dsl::Element* closedHit =
                        closedPopup != nullptr
                            ? ui->find(popupId + ".hit")
                            : nullptr;
                    // 不残留判据（MVP 同款）：opacity==0 或 .popup.hit disabled
                    const bool hidden =
                        (closedPopup != nullptr &&
                         closedPopup->opacity <= 0.5f) ||
                        (closedHit != nullptr && closedHit->disabled);
                    if (closedPopup == nullptr || !hidden) {
                        std::cerr << "settings_cards_probes: '" << popupId
                                  << "' still visible after close (opacity="
                                  << (closedPopup != nullptr
                                          ? closedPopup->opacity
                                          : -1.0f)
                                  << ")\n";
                        passed = false;
                    }
                }
            }
        }

        // ============ §B 类 4(b)：按钮 theme（模块 tab 4 个按钮） ============
        if (ui != nullptr) {
            const core::theme::Tokens& t = core::theme::current();
            std::vector<const core::dsl::Element*> buttons;
            for (const auto& root : ui->roots()) {
                walkButtons(*root, buttons);
            }
            std::cerr << "settings_cards_probes: 扫描到 " << buttons.size()
                      << " 个按钮（模块 tab）\n";
            if (buttons.size() < 4) {
                std::cerr << "settings_cards_probes: 期望 ≥4 个按钮"
                             "（pet.reset/pet.show/directory.open/proxy.apply）"
                             "，实际 " << buttons.size() << "\n";
                passed = false;
            }
            for (const core::dsl::Element* b : buttons) {
                if (!verifyButtonTheme(*b, t)) {
                    passed = false;
                }
            }
        }
    }

    // 日志 tab 的「刷新」按钮（log 查看卡，类 4(b) 第三处原 bug 位
    // settings_ui_cards.cpp:938）
    {
        composeFrame();
        const core::dsl::Element* tabLog =
            ui != nullptr ? ui->find("settings.tabs.hit.2") : nullptr;
        if (tabLog == nullptr) {
            std::cerr << "settings_cards_probes: tab 'settings.tabs.hit.2' "
                         "(日志) not found\n";
            passed = false;
        } else {
            clickElement(*tabLog);
            composeFrame();

            // ② 日志卡两行同列 + 标签单行（2026-09-29 真机反馈「设置-日志：
            //    日志和日志级别这 2 个条件 错位了啊 日志显示 2 行」）。
            //    判据：两下拉 frame.x 相等（改前 28 vs 124）、标签同列、
            //    标签→下拉间距一致、标签框高 ≤ 单行行高且文本放得进框。
            if (ui != nullptr) {
                namespace ms = module::settings;
                const core::dsl::Element* logF1 =
                    ui->find("settings.card.log.dd.field");
                const core::dsl::Element* logF2 =
                    ui->find("settings.card.log.level.dd.field");
                const core::dsl::Element* logL1 =
                    ui->find("settings.card.log.label");
                const core::dsl::Element* logL2 =
                    ui->find("settings.card.log.level.label");
                if (logF1 == nullptr || logF2 == nullptr ||
                    logL1 == nullptr || logL2 == nullptr) {
                    std::cerr << "settings_cards_probes: 日志卡两行缺元素"
                                 "（label/dd.field；行 1 标签是本次补上的）\n";
                    passed = false;
                } else {
                    if (!floatEq(logF1->frame.x, logF2->frame.x)) {
                        std::cerr << "settings_cards_probes: 日志两下拉错位 "
                                  << logF1->frame.x << " vs " << logF2->frame.x
                                  << "\n";
                        passed = false;
                    }
                    if (!floatEq(logL1->frame.x, logL2->frame.x)) {
                        std::cerr << "settings_cards_probes: 日志两标签错位 "
                                  << logL1->frame.x << " vs " << logL2->frame.x
                                  << "\n";
                        passed = false;
                    }
                    const float gap1 =
                        logF1->frame.x - (logL1->frame.x + logL1->frame.width);
                    const float gap2 =
                        logF2->frame.x - (logL2->frame.x + logL2->frame.width);
                    if (!floatEq(gap1, gap2) || gap1 <= 0.0f) {
                        std::cerr << "settings_cards_probes: 标签→下拉间距不一致 "
                                  << gap1 << " vs " << gap2 << "\n";
                        passed = false;
                    }
                    for (const core::dsl::Element* label : {logL1, logL2}) {
                        const float labelW = ms::estimatedTextWidth(
                            label->text, label->fontSize);
                        if (label->frame.height > 20.5f ||
                            label->text.find('\n') != std::string::npos ||
                            labelW > label->frame.width + 0.5f) {
                            std::cerr << "settings_cards_probes: 日志标签 '"
                                      << label->text << "' 高 "
                                      << label->frame.height << " 文本宽 "
                                      << labelW << " 框宽 " << label->frame.width
                                      << "（折成两行 / 溢出）\n";
                            passed = false;
                        }
                    }
                    if (std::string(logL1->text) != "日志保留" ||
                        std::string(logL2->text) != "日志级别") {
                        std::cerr << "settings_cards_probes: 日志卡标签文案异常 '"
                                  << logL1->text << "' / '" << logL2->text
                                  << "'\n";
                        passed = false;
                    }
                }
            }
            if (ui != nullptr) {
                const core::theme::Tokens& t = core::theme::current();
                std::vector<const core::dsl::Element*> buttons;
                for (const auto& root : ui->roots()) {
                    walkButtons(*root, buttons);
                }
                if (buttons.empty()) {
                    std::cerr << "settings_cards_probes: 日志 tab 无按钮（期望 "
                                 "log.refresh）\n";
                    passed = false;
                }
                for (const core::dsl::Element* b : buttons) {
                    if (!verifyButtonTheme(*b, t)) {
                        passed = false;
                    }
                }
            }
        }
    }

    // ============ §D --dump <prefix>：设置页缩放两档落图（2026-09-30 缩放派生审查）====
    // 通用 tab 在 fontScale 100% / 130% 各真渲染一帧，GL 后备缓冲 glReadPixels
    // 读回写 <prefix>_100/_130.ppm。about_probe.cpp 同款手法（P6 PPM；锁屏 /
    // screencapture 旧帧坑都不依赖）。dump 帧取在 present() **之前**：swap 后
    // 后备缓冲内容未定义（about_probe.cpp:425 同一条教训）
    std::string dumpPrefix;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--dump") {
            dumpPrefix = argv[i + 1];
        }
    }
    if (!dumpPrefix.empty()) {
        // 回通用 tab（§A-§C 把 tab 切到了日志）：dump 对象是外观卡所在的
        // 通用页——本期修（行盒随生效字号派生）的主断言面
        const core::dsl::Element* tabGeneral =
            ui != nullptr ? ui->find("settings.tabs.hit.0") : nullptr;
        if (tabGeneral != nullptr) {
            clickElement(*tabGeneral);
        }
        auto* glfwWin = static_cast<GLFWwindow*>(window);
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(glfwWin, &fbW, &fbH);
        float scaleX = 1.0f, scaleY = 1.0f;
        glfwGetWindowContentScale(glfwWin, &scaleX, &scaleY);
        const float renderScale = scaleX > 0.0f ? scaleX : 1.0f;
        glfwShowWindow(glfwWin);
        glfwFocusWindow(glfwWin);
        std::cout << "settings_cards_probes: dump framebuffer " << fbW << "x"
                  << fbH << " / scale " << renderScale << "\n";
        const eui::Color clearColor = core::theme::current().colors.background;
        // runtime.render 走 activeRenderBackend()——不建后端/不激活时渲染是
        // 空操作（窗口全黑）。镜像 app：createRenderBackend + 每帧 Scoped 激活
        auto renderBackend = core::render::createRenderBackend(window);
        if (renderBackend != nullptr && !renderBackend->initialize()) {
            std::cerr << "settings_cards_probes: renderBackend init 失败\n";
            renderBackend.reset();
        }
        if (renderBackend == nullptr) {
            std::cerr << "settings_cards_probes: 无渲染后端，--dump 不可用\n";
            passed = false;
        }
        const auto dumpFrame = [fbW, fbH, &passed](const std::string& path) {
            std::vector<unsigned char> px(
                static_cast<std::size_t>(fbW) *
                static_cast<std::size_t>(fbH) * 3u);
            // macOS 把 OpenGL API 标 deprecated（-Werror 下变错）：逐语句窄
            // 抑制（about_probe.cpp:378 同款，不用文件级屏蔽）
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbW, fbH, GL_RGB, GL_UNSIGNED_BYTE, px.data());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
            std::ofstream ofs(path, std::ios::binary);
            if (!ofs) {
                // 打开失败（路径不可写/磁盘满）不能静默假成功——视觉证据
                // 没落盘就得置红，与 writeProbeConfig 的流校验同口径
                std::cerr << "settings_cards_probes: dump 打开失败: " << path
                          << "\n";
                passed = false;
                return;
            }
            ofs << "P6\n" << fbW << " " << fbH << "\n255\n";
            for (int row = fbH - 1; row >= 0; --row) {  // GL 左下原点 → 翻转
                ofs.write(reinterpret_cast<const char*>(
                              px.data() + static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(fbW) *
                                              3u),
                          static_cast<std::streamsize>(fbW) * 3);
            }
            ofs.close();  // 显式 close 后判流状态：写失败/flush 失败在这里暴露
            if (!ofs) {
                std::cerr << "settings_cards_probes: dump 写入失败: " << path
                          << "\n";
                passed = false;
            } else {
                std::cout << "settings_cards_probes: dump " << path << "\n";
            }
        };
        // 每档：置缩放 → warmup 120 帧（跨档重 compose 的 transition 落定）→
        // 末帧 render 后 present 前 dump。有界（2×120 帧）必退出
        const float prevScale = core::theme::fontScale();
        const auto dumpAtScale = [&](float scale, const char* suffix) {
            core::theme::setFontScale(scale);
            for (int i = 0; i < 120 && renderBackend != nullptr; ++i) {
                glfwPollEvents();
                composeFrame();
                core::render::ScopedRenderBackend scoped(*renderBackend);
                renderBackend->beginFrame(core::render::RenderSurface{
                    window, core::window::nativeWindowInfo(window), fbW, fbH,
                    renderScale});
                runtime.render(fbW, fbH, renderScale, clearColor);
                if (i == 119) {
                    dumpFrame(dumpPrefix + suffix);
                }
                renderBackend->present();
            }
        };
        dumpAtScale(1.0f, "_100.ppm");
        dumpAtScale(1.3f, "_130.ppm");
        core::theme::setFontScale(prevScale);
    }

    mod->onStop();
    runtime.shutdown(false);
    test_support::setEnvVar("DEVDESK_PETS_DIR", "");
    std::error_code ec;
    std::filesystem::remove_all(petsDir, ec);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}