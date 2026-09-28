// pet_probes.cpp — DevDesk probes：桌宠组（probes spec
// 2026-09-28-devdesk-probes-design.md §3-probe-full 的 pet 组）。
//
// §A = 类 12 click-away 结构断言（P2 档，回归锁）：
//   bug：pet 右键 styled 菜单唯一关闭机制是失焦自关，Windows 点桌面不换
//   前台窗 → 菜单常驻（pet_window.cpp:407；window_backend.cpp:936-947）。
//   EUI-NEO ca40ff2 已修「失焦检查饿死」一半（focus 回调 + requestUiUpdate
//   闭环），spec 类 12(a) 的**结构修复落点** = 菜单窗 compose 树内全窗
//   dismiss 元素（id 含 ".dismiss"、size == 窗尺寸、z 最低——仿
//   components/contextmenu.h:166-173）。当前 styledmenu.h 菜单树内无该元素
//   → 本组红 = 证伪门禁（同 settings_cards_probes.cpp §C 口径：修复落点未
//   落地先红作门禁，修复落地自动转绿；日后撤销 dismiss 修复则必被本探针
//   抓回）。
//
// 搭法（桌宠不是 Router 页面模块——app_registry.h 六个工厂全是 Router 页，
// 没有 create_pet_module()；桌宠装配入口 = 生产同款 startPetWindow，
// main.cpp:634 同型调用）：
//   - DEVDESK_PETS_DIR → 临时目录 + robot pet.json 夹具（loadSkin 只校验
//     pet.json 存在且可解析，pet_skin.cpp:144-166；帧 PNG 可缺——probe 不
//     render，缺文件不影响建树）；
//   - startPetWindow(PetWindowConfig{}) → app::openWindow 入队 → 探针消费
//     DslWindowRequest（test_pet.cpp:398-438 同款消费），按请求建真实 GLFW
//     窗（128×128 无边置顶）并调 onWindowCreated 回填 PetRuntime.handle
//     ——生产环路里这一步在主循环做，probe 自己补位；
//   - rtPet 驱动该窗：composeModuleFrame 成对（update+recompose），
//     queuePointerButton(Right, Press+Release) 真实命中 pet.sprite →
//     onContextMenu（runtime_input.h:416-444：press 置 contextActive、
//     release 且未拖拽才回调）→ showPetStyledMenu → 菜单窗请求入队；
//   - 消费菜单请求，用独立 Runtime 直接 compose 菜单 compose 一帧（不建
//     真实菜单窗：失焦自关检查 handle==nullptr 短路 + focusGraceFrames=3
//     兜底，P2 结构断言单窗即可，spec §5 备注口径）→ 对 "styledMenu.pet"
//     树做 类 12(a) 断言。
//
// 运行：pet_probes.exe [--size WxH]（默认 1200×1000；WxH 只作用于 harness
// 主窗，桌宠窗尺寸由皮肤夹具决定，菜单窗由菜单内容决定）。
//
// P3 行为档（类 12(b)：注入菜单窗内非行区点击 → 菜单关闭/隐藏）需要
// harness 多窗注入能力（spec §5「多窗 stub」/§7 风险），未就绪，留 TODO。

#include "probes/probe_harness.h"
#include "test_support.h"

#include "app_registry.h"
#include "core/module_interface.h"
#include "module_pet/pet_window.h"

#include <components/styledmenu.h>

#include <eui/app.h>
#include <eui/detail/dsl_app_impl.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// app:: 层实现（openWindow/consumeWindowRequests 等随 dsl_app_impl.h 自给，
// 与 settings_cards_probes.cpp:60-66 同款注入；glfw_app_main.cpp 只编进
// DevDesk 主程序）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_pet_probe");
    return config;
}
// app::compose —— 应用级 UI 补全。探针不建主窗口应用树（compose 走
// probe::composeModuleFrame 直接驱动 runtime），空实现只满足 dsl_app_impl.h
// 头内 emit 的函数体对该符号的引用（链接需要）
void compose(eui::Ui&, const eui::Screen&) {}
}  // namespace app

// 注：petCompose 定义于 pet_window.cpp 文件内匿名命名空间（内部链接），
// 探针不直接 extern——桌宠窗树的驱动走 startPetWindow 注册到请求里的
// compose（std::function 在 pet_window.cpp 内部构造，内部链接无碍）。

namespace {

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.5f; }

// 机器人皮肤夹具（同 test_pet.cpp:146 kRobotFixtureJson：frameSize
// 128×128 + idle 帧表；loadSkin 只解析 pet.json，PNG 帧缺文件不影响建树）
const char* kRobotFixtureJson =
    "{"
    "  \"name\": \"robot\","
    "  \"frameSize\": [128, 128],"
    "  \"frameRate\": 4,"
    "  \"phrases\": [\"滴——系统正常\"],"
    "  \"states\": {"
    "    \"idle\": [\"f0.png\"],"
    "    \"poked\": [\"p0.png\"],"
    "    \"sleep\": [\"s0.png\"],"
    "    \"typing\": [\"t0.png\"]"
    "  }"
    "}";

// 临时皮肤目录：DEVDESK_PETS_DIR/robot/pet.json
bool makeSkinFixture(const std::filesystem::path& petsDir) {
    std::error_code ec;
    std::filesystem::create_directories(petsDir / "robot", ec);
    if (ec) {
        return false;
    }
    std::ofstream f(petsDir / "robot" / "pet.json",
                    std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f << kRobotFixtureJson;
    return f.good();
}

// 深度遍历收集全部元素（dismiss 搜索与 z 全序比较用）
void collectElements(const core::dsl::Element& node,
                     std::vector<const core::dsl::Element*>& out) {
    out.push_back(&node);
    for (const auto& child : node.children) {
        collectElements(*child, out);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    // harness 主窗（--size 可调；只作骨架展示窗，断言不读它）
    auto window = probe::makeWindow("probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 皮肤夹具：DEVDESK_PETS_DIR → 临时目录（robot），结束还原并清理
    const std::filesystem::path petsDir =
        test_support::makeTempDir("pfx_", "pets");
    if (!makeSkinFixture(petsDir)) {
        std::cerr << "pet_probes: 皮肤夹具创建失败\n";
        std::error_code ec;
        std::filesystem::remove_all(petsDir, ec);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }
    test_support::setEnvVar("DEVDESK_PETS_DIR", petsDir.string());

    core::dsl::Runtime rtMain;  // 骨架主窗（本组不注入，只随骨架初始化）
    core::dsl::Runtime rtPet;   // 桌宠窗（真实 GLFW 窗，右键注入目标）
    core::dsl::Runtime rtMenu;  // 菜单窗 compose 树（结构断言对象）
    core::window::Handle petWindow = nullptr;
    bool passed = true;

    auto cleanup = [&]() {
        components::closeStyledMenu();  // styled 菜单 runtime 单实例，主动关防悬挂
        rtMenu.shutdown(false);
        rtPet.shutdown(false);
        rtMain.shutdown(false);
        if (petWindow != nullptr) {
            core::releaseInputQueue(petWindow);
            core::window::destroyWindow(petWindow);
        }
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        test_support::setEnvVar("DEVDESK_PETS_DIR", "");
        std::error_code ec;
        std::filesystem::remove_all(petsDir, ec);
        probe::terminateWindowBackend();
    };

    rtMain.initialize(window);

    // ============ §A 类 12 click-away：真实右键路径触发菜单 ============
    module::pet::startPetWindow(module::pet::PetWindowConfig{});
    std::vector<app::DslWindowRequest> petReqs = app::consumeWindowRequests();
    if (petReqs.size() != 1) {
        std::cerr << "pet_probes: 期望 1 个桌宠开窗请求，实际 "
                  << petReqs.size() << "\n";
        cleanup();
        return 4;
    }
    const app::DslWindowRequest& petReq = petReqs[0];
    if (petReq.pageId != "pet") {
        std::cerr << "pet_probes: 桌宠窗 pageId='" << petReq.pageId
                  << "'（期望 'pet'）\n";
        passed = false;
    }
    // 冒烟：与 test_pet pet_start_window_request 同款夹具断言（128×128
    // robot；也保证桌宠不是 0 尺寸窗——命中坐标才有意义）
    if (petReq.width != 128 || petReq.height != 128) {
        std::cerr << "pet_probes: 桌宠窗尺寸 " << petReq.width << "x"
                  << petReq.height << "（夹具期望 128x128）\n";
        passed = false;
    }
    if (!static_cast<bool>(petReq.compose) ||
        !static_cast<bool>(petReq.onWindowCreated)) {
        std::cerr << "pet_probes: 桌宠请求缺 compose/onWindowCreated"
                     "（右键路径不成立）\n";
        cleanup();
        return 5;
    }
    const float petW = static_cast<float>(petReq.width);
    const float petH = static_cast<float>(petReq.height);

    // 按请求建真实桌宠窗（生产环路里主循环据此 createWindow + 回填回调）
    if (petReq.width <= 0 || petReq.height <= 0) {
        std::cerr << "pet_probes: 桌宠窗尺寸非法 " << petReq.width << "x"
                  << petReq.height << "\n";
        cleanup();
        return 6;
    }
    {
        core::window::WindowCreateRequest createRequest;
        createRequest.title = "DevDesk Pet";
        createRequest.width = petReq.width;
        createRequest.height = petReq.height;
        createRequest.x = petReq.x;
        createRequest.y = petReq.y;
        createRequest.positionSet = petReq.positionSet;
        createRequest.decorated = false;  // 与请求一致：无边置顶小窗
        createRequest.alwaysOnTop = true;
        createRequest.resizable = false;
        createRequest.focusOnShow = false;  // 显示不抢焦点
        createRequest.transparentFramebuffer = true;
        createRequest.renderApi = probe::configuredRenderApi();
        petWindow = core::window::createWindow(createRequest);
        if (petWindow == nullptr) {
            std::cerr << "pet_probes: 桌宠窗创建失败\n";
            cleanup();
            return 7;
        }
        // onWindowCreated 回填 PetRuntime.handle——右键守卫（pet_window.cpp:
        // 414「handle==nullptr return」）与锚点取位的前提
        petReq.onWindowCreated(petWindow);
    }

    rtPet.initialize(petWindow);
    const std::string petPage = "pet";
    auto petFrame = [&]() {
        // 桌宠窗 compose = startPetWindow 注册的请求 compose（生产同款，
        // pet_window.cpp 内部构造的 std::function，内含匿名命名空间的
        // petCompose）
        probe::composeModuleFrame(rtPet, petWindow, petPage, petW, petH,
                                  [&](eui::Ui& u, const eui::Screen& s) {
                                      petReq.compose(u, s);
                                  });
    };

    // 帧 1：建树（sprite 就位，供命中）
    petFrame();
    // 帧 2：右键注入（press 置 contextActive → release 未拖拽触发
    // onContextMenu，runtime_input.h:416-444），菜单窗请求随帧入队
    const float cx = petW * 0.5f;
    const float cy = petH * 0.5f;
    core::queuePointerButton(petWindow, cx, cy, core::PointerButton::Right,
                             core::PointerAction::Press, {});
    core::queuePointerButton(petWindow, cx, cy, core::PointerButton::Right,
                             core::PointerAction::Release, {});
    petFrame();

    std::vector<app::DslWindowRequest> menuReqs = app::consumeWindowRequests();
    if (menuReqs.size() != 1) {
        std::cerr << "pet_probes: 期望 1 个 styled 菜单开窗请求（右键已注入 "
                  << "sprite），实际 " << menuReqs.size() << "\n";
        passed = false;
        cleanup();
        return 8;
    }
    const app::DslWindowRequest& menuReq = menuReqs[0];
    if (menuReq.pageId != "styledMenu.pet") {
        std::cerr << "pet_probes: 菜单窗 pageId='" << menuReq.pageId
                  << "'（期望 'styledMenu.pet'）\n";
        passed = false;
    }
    if (!static_cast<bool>(menuReq.compose)) {
        std::cerr << "pet_probes: 菜单请求无 compose\n";
        cleanup();
        return 9;
    }
    const float menuW = static_cast<float>(menuReq.width);
    const float menuH = static_cast<float>(menuReq.height);

    // 菜单窗树：独立 Runtime 直接 compose 菜单窗 compose（不建真实窗口——
    // 失焦自关检查 handle==nullptr 短路、focusGraceFrames=3 兜底；
    // P2 结构断言单窗即可，spec §5 备注）
    rtMenu.initialize();
    core::dsl::Ui* menuUi = nullptr;
    rtMenu.compose(menuReq.pageId, menuW, menuH,
                   [&](eui::Ui& u, const eui::Screen& screen) {
                       menuUi = &u;
                       menuReq.compose(u, screen);
                   });

    // ============ 类 12(a) 主断言：菜单树 + 全窗 dismiss ============
    if (menuUi == nullptr) {
        std::cerr << "pet_probes: 菜单树未 compose\n";
        passed = false;
    } else {
        const core::dsl::Element* root = menuUi->find("styledMenu.pet");
        if (root == nullptr) {
            std::cerr << "pet_probes: 菜单根 'styledMenu.pet' 不在树"
                         "（菜单未构建？）\n";
            passed = false;
        } else {
            // 全窗根层：menu 根 stack size == 菜单窗尺寸（styledmenu.h:509-510）
            if (!floatEq(root->frame.x, 0.0f) || !floatEq(root->frame.y, 0.0f) ||
                !floatEq(root->frame.width, menuW) ||
                !floatEq(root->frame.height, menuH)) {
                std::cerr << "pet_probes: 菜单根 frame (" << root->frame.x << ","
                          << root->frame.y << ", " << root->frame.width << "x"
                          << root->frame.height << ") ≠ 菜单窗尺寸 " << menuW
                          << "x" << menuH << "\n";
                passed = false;
            }
            if (menuUi->find("styledMenu.pet.panel") == nullptr) {
                std::cerr << "pet_probes: 菜单面板 'styledMenu.pet.panel' "
                             "不在树\n";
                passed = false;
            }
            if (menuUi->find("styledMenu.pet.row.0.hit") == nullptr) {
                std::cerr << "pet_probes: 菜单首行命中区 "
                             "'styledMenu.pet.row.0.hit' 不在树（右键菜单未"
                             "展开？）\n";
                passed = false;
            }

            // ---- 类 12(a) 主断言：全窗 dismiss 元素 ----
            // 修复落点 = styledmenu.h 菜单根内先于面板建 .dismiss（仿
            // contextmenu.h:166-173：ui_.rect(id_ + ".dismiss").size(窗尺寸)
            // .onClick(requestDismiss)）。当前 master 树内无该元素 → 红 =
            // 证伪门禁（修复落地转绿；撤销 dismiss 则死灰复燃）。
            std::vector<const core::dsl::Element*> all;
            collectElements(*root, all);
            const core::dsl::Element* dismiss = nullptr;
            for (const core::dsl::Element* e : all) {
                if (e->id.find(".dismiss") != std::string::npos) {
                    dismiss = e;
                    break;
                }
            }
            if (dismiss == nullptr) {
                std::cerr << "pet_probes: 类 12(a) 菜单窗树无 '*.dismiss' "
                             "元素——证伪门禁红：修复落点 = styled 菜单根内"
                             "先于面板建全窗 dismiss rect（仿 components/"
                             "contextmenu.h:166-173：size == 窗尺寸、z 最低、"
                             "onClick → requestDismiss），落地后本组转绿\n";
                passed = false;
            } else {
                // size == 窗尺寸（全窗：点任意非行区都是 dismiss 命中）
                if (!floatEq(dismiss->frame.x, 0.0f) ||
                    !floatEq(dismiss->frame.y, 0.0f) ||
                    !floatEq(dismiss->frame.width, menuW) ||
                    !floatEq(dismiss->frame.height, menuH)) {
                    std::cerr << "pet_probes: dismiss '" << dismiss->id
                              << "' frame (" << dismiss->frame.x << ","
                              << dismiss->frame.y << ", " << dismiss->frame.width
                              << "x" << dismiss->frame.height << ") ≠ 菜单窗尺寸 "
                              << menuW << "x" << menuH
                              << "（应全窗，点任意非行区 = dismiss）\n";
                    passed = false;
                }
                // z 最低：树内其余元素 zIndex 都 >= dismiss（dismiss 垫底，
                // 面板/行盖在上面才点得到行、非行区才漏给 dismiss）
                int minOthers = 0;
                bool minSet = false;
                for (const core::dsl::Element* e : all) {
                    if (e == dismiss) {
                        continue;
                    }
                    if (!minSet || e->zIndex < minOthers) {
                        minOthers = e->zIndex;
                        minSet = true;
                    }
                }
                if (minSet && dismiss->zIndex > minOthers) {
                    std::cerr << "pet_probes: dismiss z=" << dismiss->zIndex
                              << " 高于树内其它元素最低 z=" << minOthers
                              << "（应 z 最低垫底）\n";
                    passed = false;
                }
            }
        }
    }

    // P3 行为档 TODO（类 12(b)，spec §3 类 12(b)/§5「多窗 stub」备注）：
    // 注入菜单窗内非行区点击（dismiss 层命中）→ 菜单窗关闭/隐藏。
    // 前置：harness 多窗注入能力（第二窗 create + 独立 Runtime + 跨窗
    // 注入），未就绪不实现；结构断言 (a) P2 先行（本文件 §A）。
    // 另待 P3/P2 后续：类 9(d) 右键锚点回归、类 9(e) 退出确认锚点、
    // 类 8(e) 小皮肤气泡（spec §5 改动清单 pet_probes.cpp 行）。

    cleanup();
    if (!passed) std::cerr << "FAIL\n";
    return passed ? 0 : 1;
}