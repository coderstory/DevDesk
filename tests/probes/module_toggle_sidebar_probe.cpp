// module_toggle_sidebar_probe.cpp — 活门控（live-toggle）真窗口 probe（计划 Task 5）
//
// 钉「侧边栏实时反映」在真 Runtime 帧节拍 + 真 DOM 上：真 app::compose
//（本 probe 额外编译 src/app_sidebar.cpp，侧边栏每帧遍历 router->modules()，
// app_sidebar.cpp:183）+ 桩 Router + 桩可开关模块，经真
// app::assembly::dispatchModuleToggle（本 probe 额外编译 src/app_assembly.cpp，
// 链接集照 2026-10-01 评审裁定单独编译进 probe）驱动开关：
//   1) 初始 off（Agent 未注册）→ composeModuleFrame → Agent 条目不存在；
//   2) dispatchModuleToggle("agent", on) → composeModuleFrame → 条目出现，
//      且 y 恰在 Home 条目下方一格（itemH 40 + gap 4）；
//   3) dispatchModuleToggle("agent", off) → composeModuleFrame → 条目消失，
//      Home 条目 y 原样（无挤压错位）；
//   4) 再 on → 条目回来；重复 on（幂等注册）→ DOM 中 Agent 条目恰好 1 份。
//
// 开关点击路径降级说明（brief 明确允许）：probe 直接调 dispatchModuleToggle，
// 不经「设置页开关卡片点击」——点击路径已由无头层钉（Task 2 桥 + Task 3
// dispatcher 的单测）；probe 的价值在真帧节拍下注册表变化当帧进 DOM。
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
#include "app_sidebar.h"              // 真 app::compose（本 probe 编译 app_sidebar.cpp）
#include "settings_config.h"          // kModuleIdAgent

#include "core/module_interface.h"
#include "core/router.h"

#include <algorithm>
#include <cmath>
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
// probe 不断言生命周期计数（那已由无头 test_app_assembly 钉死），只钉
// 「开关 → 下一帧侧边栏 DOM」这一层。
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

// ModuleLookup 是裸函数指针（无捕获约束）→ 经文件级静态中转（形态同
// test_app_assembly.cpp 的计数桩查找缝）
StubPageModule* g_agent = nullptr;
::core::Module* lookupForProbe(::core::PageId page) {
    return page == ::core::PageId::Agent ? g_agent : nullptr;
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

    StubPageModule home(::core::PageId::Home, "首页");
    StubPageModule agent(::core::PageId::Agent, "智能体");
    g_agent = &agent;

    ::core::Router router;
    router.registerModule(&home);
    // Agent 初始不注册（off 态）——由 dispatchModuleToggle on 驱动注册
    ::core::registerAppRouter(&router);

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

    bool passed = true;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) {
            std::cerr << "module_toggle_probe: FAIL " << what << "\n";
            passed = false;
        }
    };

    // ---- 1) 初始 off：Agent 条目不存在，Home 条目在 ----
    composeFrame();
    if (ui == nullptr) {
        std::cerr << "module_toggle_probe: ui 为空（compose 未执行）\n";
        probe::terminateWindowBackend();
        return 3;
    }
    check(ui->find("app.sidebar.item.Agent.hit") == nullptr,
          "初始 off：Agent 条目应不存在");
    const auto* homeHit = ui->find("app.sidebar.item.Home.hit");
    check(homeHit != nullptr, "Home 条目应存在");
    if (homeHit == nullptr) {
        std::cerr << "FAIL\n";
        agent.onStop();
        home.onStop();
        runtime.shutdown(false);
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 1;
    }
    const float homeY = homeHit->frame.y;

    // ---- 2) on → 下一帧条目出现，y = Home 下一格（itemH 40 + gap 4）----
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, true,
                                        router, &lookupForProbe);
    composeFrame();
    const auto* agentHit = ui != nullptr ? ui->find("app.sidebar.item.Agent.hit")
                                         : nullptr;
    check(agentHit != nullptr, "on 后：Agent 条目应出现");
    if (agentHit != nullptr) {
        check(nearEq(agentHit->frame.y, homeY + 44.0f),
              "Agent 条目 y=" + std::to_string(agentHit->frame.y) + " 期望 " +
                  std::to_string(homeY + 44.0f) + "（Home 下一格）");
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
    agent.onStop();
    home.onStop();
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
