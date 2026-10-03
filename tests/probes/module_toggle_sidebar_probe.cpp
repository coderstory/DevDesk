// module_toggle_sidebar_probe.cpp — 活门控（live-toggle）真窗口 probe（计划 Task 5 + fix 轮 [4]）
//
// 钉「侧边栏实时反映」在真 Runtime 帧节拍 + 真 DOM 上：真 app::compose
//（本 probe 额外编译 src/app_sidebar.cpp，侧边栏每帧遍历 router->modules()，
// app_sidebar.cpp:183）+ 桩 Router + 桩可开关模块。
//
// §0 = 真 点击路径（fix 轮 [4]，终审要求）：真 settings 模块实例 + 真卡片
//   compose，queuePointerButton 真点 agent 开关（settings.card.modules.sw.4.hit
//   命中区中心，坐标取 compose 后 DOM frame）→ settings_module
//   applyModuleEnabled（settings_module.cpp:584 persist + onModuleToggled）
//   → app::toggle 桥 → probe 注入的 handler（形态照 main.cpp:192-194 生产接线
//   = dispatchModuleToggle）→ 侧边栏当帧反映 + 当前页=Agent 时回首页 redirect。
//   覆盖「redirect 发生在点击派发栈内」的形态——直调路径（§1-§4）盖不到。
// §1-§4 = 直调 dispatchModuleToggle（Task 5 原有覆盖）：
//   off→on→off→on→on(幂等)，断言条目出现/消失/回来/恰好 1 份。
//
// app:: 层链接说明（照 about_probe.cpp:49-56 的 app::stub 模式）：
//   - app::compose 用真实现（app_sidebar.cpp 编译进本 probe），不桩；
//   - app::dslAppConfig() 仍需桩（main.cpp 不在链接面）；
//   - app::requestShow/requestUpdate 在 main() 里真调一次（GLFW 初始化后，
//     只置原子标志，无副作用）——同时强制 dsl_app_impl.h 的内联定义落地本
//     TU，app_assembly.cpp 对这两个符号的引用才能在链接期解析。
#include "probes/probe_harness.h"
#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现
#include "app_assembly.h"             // app::assembly::dispatchModuleToggle
#include "app_registry.h"             // create_settings_module
#include "app_sidebar.h"              // 真 app::compose + appNoticeVisible
#include "module_toggle_bridge.h"     // app::toggle::setToggleHandler（handler 注入）
#include "settings_config.h"          // kModuleIdAgent
#include "test_support.h"             // tempConfigFile / ScopedConfigPath

#include "core/module_interface.h"
#include "core/router.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// app:: 层链接桩（dsl_app_impl.h 引用 dslAppConfig；app::compose 用真实现，
// 来自本 probe 编译的 src/app_sidebar.cpp，不桩）
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_module_toggle_probe");
    return config;
}
}  // namespace app

namespace {

// 桩页面模块：Agent 页（活门控目标页）。onStart/onStop 默认空实现够用——
// 生命周期计数已由无头 test_app_assembly 钉死，probe 只钉「开关 → 下一帧
// 侧边栏 DOM」这一层。
class StubPageModule : public ::core::Module {
public:
    StubPageModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return eui::utf8(0xF015); }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.text("stub." + name_).text(name_).size(120.0f, 20.0f).build();
    }

private:
    ::core::PageId page_;
    std::string name_;
};

// ModuleLookup / ToggleHandler 都是裸函数指针（无捕获约束）→ 经文件级静态
// 中转（形态同 test_app_assembly.cpp 的计数桩查找缝）
StubPageModule* g_agent = nullptr;
::core::Router* g_router = nullptr;
::core::Module* lookupForProbe(::core::PageId page) {
    return page == ::core::PageId::Agent ? g_agent : nullptr;
}
// handler 形态照 main.cpp:192-194 生产接线：settings 卡片开关 → 桥 → 这里
// → dispatchModuleToggle（§0 真点击路径靠它闭合）
void probeToggleHandler(const std::string& moduleId, bool on) {
    if (g_router != nullptr) {
        app::assembly::dispatchModuleToggle(moduleId, on, *g_router,
                                            &lookupForProbe);
    }
}

bool nearEq(float a, float b) { return std::fabs(a - b) <= 0.5f; }

// 数 DOM 里同名 id 出现次数（幂等注册断言用：重复注册不得产生重复条目）
int countId(core::dsl::Ui& ui, const std::string& id) {
    int count = 0;
    for (const auto& root : ui.roots()) {
        std::vector<const core::dsl::Element*> stack{root.get()};
        while (!stack.empty()) {
            const core::dsl::Element* node = stack.back();
            stack.pop_back();
            if (node->id == id) ++count;
            for (const auto& child : node->children) {
                stack.push_back(child.get());
            }
        }
    }
    return count;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
    auto window = probe::makeWindow("module_toggle_probe", args.windowW,
                                    args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 配置隔离（settings 模块 onStart 吃它）：agent 不在 enabledModules =
    // 初始 off 态，§0 点击 on / off 全程走真 persist + 真桥
    const auto cfgFile = test_support::tempConfigFile("pfx_", "cfg");
    {
        std::ofstream f(cfgFile, std::ios::binary | std::ios::trunc);
        f << "{\"themeMode\":\"light\","
             "\"enabledModules\":[\"markdown\",\"todo\",\"claude_proxy\"]}";
    }
    test_support::ScopedConfigPath scopedCfg(cfgFile);

    StubPageModule home(::core::PageId::Home, "首页");
    StubPageModule agent(::core::PageId::Agent, "智能体");
    g_agent = &agent;

    std::unique_ptr<::core::Module> settings(create_settings_module());
    settings->onStart();  // 读 ScopedConfigPath 指到的临时 config（settings_cards_probes 同序）

    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(settings.get());
    // Agent 初始不注册（off 态）——§0 真点击 / §1 直调 on 驱动注册
    ::core::registerAppRouter(&router);
    g_router = &router;
    app::toggle::setToggleHandler(&probeToggleHandler);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // 内联定义落地本 TU（见文件头说明）：GLFW 初始化后调用安全
    //（只置原子标志 + postEmptyEvent，probe 有窗）
    app::requestUpdate();
    app::requestShow();

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
    core::dsl::Ui* ui = nullptr;
    auto composeFrame = [&]() {
        probe::composeModuleFrame(runtime, window, "app", w, h,
                                  [&](eui::Ui& u, const eui::Screen& screen) {
                                      ui = &u;
                                      app::compose(u, screen);
                                  });
    };
    // 真实命中分发（settings_cards_probes 同款）：press + release 排队 →
    // 下一 composeFrame 的 update 消费（命中 → onClick → 派发栈内完成
    // persist / 注册表 / 重定向）
    auto clickElement = [&](const core::dsl::Element& element) {
        const float cx = element.frame.x + element.frame.width * 0.5f;
        const float cy = element.frame.y + element.frame.height * 0.5f;
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Press, {});
        core::queuePointerButton(window, cx, cy, core::PointerButton::Left,
                                 core::PointerAction::Release, {});
    };

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "module_toggle_probe: FAIL " << what << "\n";
            passed = false;
        }
    };

    // ============ §0 真点击路径：设置卡 agent 开关（终审 fix 轮 [4]） ============
    composeFrame();
    if (ui == nullptr) {
        std::cerr << "module_toggle_probe: ui 为空（compose 未执行）\n";
        app::toggle::setToggleHandler(nullptr);
        probe::terminateWindowBackend();
        return 3;
    }
    check(ui->find("app.sidebar.item.Agent.hit") == nullptr,
          "§0 初始 off：Agent 条目应不存在");

    // 进设置页 → 切「模块」tab → 真点 agent 开关。当前页=Settings 时设置卡片
    // 在 DOM，点击派发栈内走 applyModuleEnabled → persist → 桥 → handler →
    // dispatchModuleToggle → 下一 compose 侧边栏当帧反映。
    // 注：「当前页=Agent 时点击开关」在真 UI 结构上不可达（Router 只 compose
    // 当前页，人在 Agent 页时设置卡不在 DOM）——redirect 分支由 §0b 用直调
    // dispatchModuleToggle 覆盖（与真点击同一 dispatcher 代码路径）。
    router.navigateTo(::core::PageId::Settings);
    composeFrame();
    const auto* tabModules =
        ui != nullptr ? ui->find("settings.tabs.hit.1") : nullptr;
    check(tabModules != nullptr, "§0 settings.tabs.hit.1（模块 tab）应存在");
    if (tabModules != nullptr) {
        clickElement(*tabModules);
        composeFrame();
        const auto* agentSwitch =
            ui != nullptr ? ui->find("settings.card.modules.sw.4.hit")
                          : nullptr;
        check(agentSwitch != nullptr,
              "§0 settings.card.modules.sw.4.hit（agent 开关）应存在");
        if (agentSwitch != nullptr) {
            // —— 真点击 on：侧边栏条目当帧出现
            clickElement(*agentSwitch);
            composeFrame();
            check(ui != nullptr &&
                      ui->find("app.sidebar.item.Agent.hit") != nullptr,
                  "§0 真点击 on：Agent 条目应当帧出现");
            // —— 真点击 off：侧边栏条目当帧消失
            const auto* switchAgain =
                ui != nullptr ? ui->find("settings.card.modules.sw.4.hit")
                              : nullptr;
            if (switchAgain != nullptr) {
                clickElement(*switchAgain);
                composeFrame();
                check(ui != nullptr &&
                          ui->find("app.sidebar.item.Agent.hit") == nullptr,
                      "§0 真点击 off：Agent 条目应当帧消失");
            } else {
                check(false, "§0 off 点击前开关元素丢失");
            }
        }
    }

    // §0b redirect-in-dispatch（直调）：当前页=Agent 时 off → navigateTo(Home)
    // + toast 发生在 dispatchModuleToggle 内部；真窗口帧节拍下断言回首页 +
    // toast。先 on 注册（navigateTo 对未注册页会兜底回 Home，router.cpp:42-51），
    // 再把当前页切到 Agent。
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &lookupForProbe);
    router.navigateTo(::core::PageId::Agent);
    composeFrame();
    check(ui != nullptr && ui->find("stub.智能体") != nullptr,
          "§0b 前置：当前页=Agent，Agent 页内容应渲染");
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent,
                                        false, router, &lookupForProbe);
    composeFrame();
    check(ui != nullptr &&
              ui->find("app.sidebar.item.Agent.hit") == nullptr,
          "§0b off 后：Agent 条目应消失");
    check(ui != nullptr && ui->find("stub.首页") != nullptr,
          "§0b off 后（当前页=Agent）：应回首页（首页内容渲染）");
    check(app::appNoticeVisible(), "§0b off 后：关闭 toast 应可见");
    check(ui != nullptr && ui->find("app.sidebar.item.Home.hit") != nullptr,
          "§0b off 后：Home 条目应存在");

    // ============ §1-§4 直调 dispatchModuleToggle（Task 5 原有覆盖） ============
    const auto* homeHit = ui != nullptr ? ui->find("app.sidebar.item.Home.hit")
                                        : nullptr;
    check(homeHit != nullptr, "Home 条目应存在");
    if (homeHit == nullptr) {
        std::cerr << "FAIL\n";
        settings->onStop();
        agent.onStop();
        home.onStop();
        app::toggle::setToggleHandler(nullptr);
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 1;
    }
    const float homeY = homeHit->frame.y;

    // ---- 2) on → 下一帧条目出现，y = 前一条目（Home）下一格（itemH 40 + gap 4）----
    //（注册序 = home, settings, agent；registerModule 按 PageId 序插入 →
    // batch-C C-1 起 Agent(4) 在 Settings(5) 之前，侧边栏序 Home, Agent, Settings；
    //  上一格锚取 Home 条目）
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &lookupForProbe);
    composeFrame();
    const auto* agentHit = ui != nullptr ? ui->find("app.sidebar.item.Agent.hit")
                                         : nullptr;
    const auto* settingsHit =
        ui != nullptr ? ui->find("app.sidebar.item.Settings.hit") : nullptr;
    check(agentHit != nullptr, "on 后：Agent 条目应出现");
    check(settingsHit != nullptr, "on 后：Settings 条目应存在（y 锚）");
    if (agentHit != nullptr && settingsHit != nullptr) {
        check(nearEq(settingsHit->frame.y, agentHit->frame.y + 44.0f),
              "Settings 条目 y=" + std::to_string(settingsHit->frame.y) +
                  " 期望 " + std::to_string(agentHit->frame.y + 44.0f) +
                  "（Agent 下一格——Agent 在 Settings 之前）");
        check(ui->find("app.sidebar.item.Agent.label") != nullptr,
              "展开态 Agent label 应存在");
    }

    // ---- 3) off → 下一帧条目消失，Home 条目 y 原样 ----
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, false,
                                        router, &lookupForProbe);
    composeFrame();
    check(ui != nullptr && ui->find("app.sidebar.item.Agent.hit") == nullptr,
          "off 后：Agent 条目应消失");
    const auto* homeHit2 = ui != nullptr ? ui->find("app.sidebar.item.Home.hit")
                                         : nullptr;
    check(homeHit2 != nullptr && nearEq(homeHit2->frame.y, homeY),
          "off 后：Home 条目 y 不应错位");

    // ---- 4) 再 on → 条目回来；重复 on → DOM 恰好 1 份（幂等注册）----
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &lookupForProbe);
    composeFrame();
    check(ui != nullptr &&
              ui->find("app.sidebar.item.Agent.hit") != nullptr,
          "再 on：Agent 条目应回来");
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &lookupForProbe);
    composeFrame();
    check(ui != nullptr &&
              countId(*ui, "app.sidebar.item.Agent.hit") == 1,
          "重复 on：Agent 条目应恰好 1 份（幂等注册，不产生重复条目）");

    // 收尾（照 about_probe.cpp:449-453 顺序：模块 onStop → runtime → 窗口 → 后端）
    settings->onStop();
    agent.onStop();
    home.onStop();
    app::toggle::setToggleHandler(nullptr);
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "module_toggle_probe: PASS（" << args.windowW << "x"
                  << args.windowH << "）\n";
    }
    return passed ? 0 : 1;
}
