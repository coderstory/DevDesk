// module_toggle_sidebar_probe.cpp — 侧边栏活门控（live-toggle）真窗口 probe
//
// 钉「侧边栏实时反映」在真 Runtime 帧节拍 + 真 DOM 上：真 app::compose（本 probe
// 额外编译 src/app_sidebar.cpp，侧边栏每帧遍历 router->modules()）+ 桩 Router +
// 桩可开关模块。两条路径：
//
//   点击路径 = 真 settings 模块实例 + 真卡片 compose，真点 agent 开关
//         （settings.card.modules.sw.4.track 命中区中心，坐标取 compose 后的 DOM
//         frame）→ settings 模块 applyModuleEnabled（persist + onModuleToggled）
//         → app::toggle 桥 → 本 probe 注入的 handler（形态同生产接线）→ 侧边栏
//         当帧反映 + 当前页=Agent 时回首页 redirect。覆盖「redirect 发生在点击
//         派发栈内」的形态——直调路径盖不到。
//   直调路径 = dispatchModuleToggle：off→on→off→on→on(幂等)，断言条目
//         出现 / 消失 / 回来 / 恰好 1 份。
//
// app:: 层链接面：
//   - app::compose 用真实现（app_sidebar.cpp 编译进本 probe），不桩；
//   - app::dslAppConfig() 需桩（main.cpp 不在链接面）；
//   - app::requestShow/requestUpdate 在 main() 里真调一次——GLFW 初始化后安全
//     （只置原子标志 + postEmptyEvent，probe 有窗），同时强制 dsl_app_impl.h 的
//     内联定义落地本 TU，app_assembly.cpp 对这两个符号的引用才能在链接期解析。
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
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>  // 粒子生命期（480ms）壁钟快进

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

// 桩页面模块：Agent 页（活门控目标页）。onStart/onStop 用默认空实现——
// 生命周期计数由无头 test_app_assembly 钉死，本 probe 只钉「开关 → 下一帧
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

// ModuleLookup / ToggleHandler 都是裸函数指针（无捕获约束）→ 经文件级静态中转
StubPageModule* g_agent = nullptr;
::core::Router* g_router = nullptr;
::core::Module* lookupForProbe(::core::PageId page) {
    return page == ::core::PageId::Agent ? g_agent : nullptr;
}
// handler 形态同生产接线：settings 卡片开关 → 桥 → 这里 → dispatchModuleToggle
// （真点击路径靠它闭合）
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
    // 初始 off 态，点击 on / off 全程走真 persist + 真桥
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
    // Agent 初始不注册（off 态）——真点击 / 直调 on 驱动注册
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

    // 真点击路径：设置卡 agent 开关
    composeFrame();
    if (ui == nullptr) {
        std::cerr << "module_toggle_probe: ui 为空（compose 未执行）\n";
        app::toggle::setToggleHandler(nullptr);
        probe::terminateWindowBackend();
        return 3;
    }
    check(ui->find("app.sidebar.item.Agent.hit") == nullptr,
          "§0 初始 off：Agent 条目应不存在");

    // 进设置页 → 切「模块」tab → 真点 agent 开关。当前页=Settings 时设置卡片在
    // DOM，点击派发栈内走 applyModuleEnabled → persist → 桥 → handler →
    // dispatchModuleToggle → 下一 compose 侧边栏当帧反映。
    // 注：「当前页=Agent 时点击开关」在真 UI 结构上不可达——Router 只 compose
    // 当前页，人在 Agent 页时设置卡不在 DOM。redirect 分支用直调
    // dispatchModuleToggle 覆盖（与真点击同一 dispatcher 代码路径）。
    router.navigateTo(::core::PageId::Settings);
    composeFrame();
    const auto* tabModules =
        ui != nullptr ? ui->find("settings.tabs.1.hit") : nullptr;
    check(tabModules != nullptr, "§0 settings.tabs.1.hit（模块 tab）应存在");
    if (tabModules != nullptr) {
        clickElement(*tabModules);
        composeFrame();
        // wswitch（§4.4）clickable 元素 id = <id>.track（W4 改动：toggleSwitch→wswitch）
        const auto* agentSwitch =
            ui != nullptr ? ui->find("settings.card.modules.sw.4.track")
                          : nullptr;
        check(agentSwitch != nullptr,
              "§0 settings.card.modules.sw.4.track（agent 开关）应存在");
        if (agentSwitch != nullptr) {
            // —— 真点击 on：侧边栏条目当帧出现
            clickElement(*agentSwitch);
            composeFrame();
            check(ui != nullptr &&
                      ui->find("app.sidebar.item.Agent.hit") != nullptr,
                  "§0 真点击 on：Agent 条目应当帧出现");
            // —— 真点击 off：侧边栏条目当帧消失
            const auto* switchAgain =
                ui != nullptr ? ui->find("settings.card.modules.sw.4.track")
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

    // redirect-in-dispatch（直调）：当前页=Agent 时 off → navigateTo(Home)
    // + toast 发生在 dispatchModuleToggle 内部；真窗口帧节拍下断言回首页 + toast。
    // 先 on 注册（navigateTo 对未注册页会兜底回 Home），再把当前页切到 Agent。
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

    // 直调 dispatchModuleToggle
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

    // 2) on → 下一帧条目出现，y = 前一条目下一格 + 组间 divider（W3 §4.8）。
    // 注册序 = home, settings, agent；registerModule 按 PageId 序插入，故 Agent(4)
    // 在 Settings(10) 之前，侧边栏序 Home, Agent, [groupdiv], Settings。
    // Agent(work) → Settings(system) 间画 groupdiv（1px + gap），间距按 need171
    // 档位：正常/紧凑档 = itemH40+gap4+1+gap4 = 49；极紧档 = itemH36+gap2+1+gap2
    // = 41（B6：窗口高驱动分档，探针窗 700 逻辑即落极紧档）。
    const float bodyH = static_cast<float>(args.windowH) - 40.0f;  // 自绘标题栏
    const float strideWithDivider =
        bodyH < 680.0f ? 41.0f : 49.0f;
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
        check(nearEq(settingsHit->frame.y,
                     agentHit->frame.y + strideWithDivider),
              "Settings 条目 y=" + std::to_string(settingsHit->frame.y) +
                  " 期望 " +
                  std::to_string(agentHit->frame.y + strideWithDivider) +
                  "（Agent 下一格 + 组间 divider——Agent(work) 在 Settings(system) 之前）");
        check(ui->find("app.sidebar.item.Agent.label") != nullptr,
              "展开态 Agent label 应存在");
    }

    // 3) off → 下一帧条目消失，Home 条目 y 原样
    app::assembly::dispatchModuleToggle(module::settings::kModuleIdAgent, false,
                                        router, &lookupForProbe);
    composeFrame();
    check(ui != nullptr && ui->find("app.sidebar.item.Agent.hit") == nullptr,
          "off 后：Agent 条目应消失");
    const auto* homeHit2 = ui != nullptr ? ui->find("app.sidebar.item.Home.hit")
                                         : nullptr;
    check(homeHit2 != nullptr && nearEq(homeHit2->frame.y, homeY),
          "off 后：Home 条目 y 不应错位");

    // 4) 再 on → 条目回来；重复 on → DOM 恰好 1 份（幂等注册）
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

    // 5) B8 G1/G2：分组父项 chevron 右边距 + 一二级选中条不同时亮
    // 分组树由登记表 parent 列驱动：注册 ReleaseTasks/ReleaseConfig 两个桩页，
    // 侧边栏就会画 ReleaseGroup 父项（父项本身无实例、不进 Router）。
    StubPageModule releaseTasks(::core::PageId::ReleaseTasks, "任务");
    StubPageModule releaseConfig(::core::PageId::ReleaseConfig, "基础配置");
    router.registerModule(&releaseTasks);
    router.registerModule(&releaseConfig);
    router.navigateTo(::core::PageId::ReleaseTasks);
    // 展开组：默认折叠，点父项切展开（holdsCurrent 下父项不再画选中条）
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    composeFrame();
    {
        const auto* chevron =
            ui != nullptr ? ui->find("app.sidebar.item.ReleaseGroup.chevron")
                          : nullptr;
        check(chevron != nullptr, "§5 展开态：分组父项 chevron 应存在");
        if (chevron != nullptr) {
            // G1 判据：chevron 盒右边界距侧栏右缘 ≥ 6px（盒宽 14 + 左移 6，
            // 见 app_sidebar.cpp drawSidebar 分组父项分支）
            const float sidebarW = app::sidebarWidth();
            const float rightGap = sidebarW - (chevron->frame.x + chevron->frame.width);
            check(rightGap >= 6.0f - 0.5f,
                  "§5 G1：chevron 右缘距侧栏右缘 " + std::to_string(rightGap) +
                      "px < 6（应 −padX−14−6 定位）");
            check(chevron->horizontalAlign == eui::HorizontalAlign::Left,
                  "§5 G1：chevron 应左对齐（盒宽 14 内在 20 盒居中会偏左）");
        }
        // G2 判据（展开态）：选中的是组内二级项 → 父项无选中条，二级项有
        check(ui != nullptr &&
                  ui->find("app.sidebar.item.ReleaseGroup.sel") == nullptr,
              "§5 G2：展开态父项不应画选中条（一二级同时亮）");
        check(ui != nullptr &&
                  ui->find("app.sidebar.item.ReleaseTasks.sel.bg") != nullptr,
              "§5 G2：展开态二级项应有选中条");
        // 父项图标仍为主色（轻提示「当前页在此组内」）
        const auto* groupIcon = ui != nullptr
                                    ? ui->find("app.sidebar.item.ReleaseGroup.icon")
                                    : nullptr;
        const auto& colors = core::theme::current().colors;
        check(groupIcon != nullptr &&
                  nearEq(groupIcon->textColor.r, colors.primary.r) &&
                  nearEq(groupIcon->textColor.g, colors.primary.g) &&
                  nearEq(groupIcon->textColor.b, colors.primary.b),
              "§5 G2：展开态父项图标应仍为主色（轻提示不能一起丢）");
    }

    // 折叠侧栏 + 组收起：父项是当前页唯一入口 → 选中条必须仍画
    app::setSidebarCollapsed(true, false);
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);  // 收起组（子项不再列出）
    composeFrame();
    {
        check(ui != nullptr &&
                  ui->find("app.sidebar.item.ReleaseGroup.sel.bg") != nullptr,
              "§5 G2：折叠且组收起时父项必须仍画选中条（唯一入口不能丢指示）");
        check(ui != nullptr &&
                  ui->find("app.sidebar.item.ReleaseGroup.chevron") == nullptr,
              "§5 折叠态：chevron 不应绘制（折叠态只画居中图标）");
    }
    app::setSidebarCollapsed(false, false);
    composeFrame();

    // 6) B6 need172：收缩按钮几何（直径 40 正圆 + 1px 常态描边 + 56 区内居中）+
    //    真点击切换侧栏宽度 + hover 一次性粒子（迸发/自止/重触发）。
    //    hover 驱动走 onHoverChanged 手调（锁屏机无真实指针 move 注入；与真指针
    //    共用同一接线，回调由 EUI 命中派发写入）。
    composeFrame();
    {
        const auto* toggle =
            ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
        check(toggle != nullptr, "§6 收缩按钮应存在");
        if (toggle != nullptr) {
            check(nearEq(toggle->frame.width, 40.0f) &&
                      nearEq(toggle->frame.height, 40.0f),
                  "§6 按钮直径应 40");
            check(nearEq(toggle->radius, 20.0f), "§6 按钮应正圆（radius=20）");
            check(nearEq(toggle->border.width, 1.0f),
                  "§6 常态应有 1px 圆框描边");
            // 56 预留区内垂直居中：按钮底距侧栏底 8px（toggleY=H−48，上下各 8）
            const auto* sidebarEl = ui->find("app.sidebar");
            check(sidebarEl != nullptr &&
                      nearEq(toggle->frame.y + toggle->frame.height + 8.0f,
                             sidebarEl->frame.y + sidebarEl->frame.height),
                  "§6 按钮应在 56 预留区内垂直居中（底距侧栏底 8px）");
            // 真点击（Press/Release 成对排队 → 下一帧消费）→ 侧栏 220→72
            clickElement(*toggle);
            composeFrame();
            check(nearEq(app::sidebarWidth(), 72.0f),
                  "§6 真点击后侧栏应折叠（220→72）");
            const auto* folded =
                ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
            check(folded != nullptr && nearEq(folded->frame.x, 16.0f),
                  "§6 折叠态按钮应水平居中 ((72-40)/2=16)");
            if (folded != nullptr) {
                clickElement(*folded);
                composeFrame();
            }
            check(nearEq(app::sidebarWidth(), 220.0f),
                  "§6 再点击应展开（72→220）");
        }

        // hover 一次性粒子：迸发 8 颗 + 泵挂树 + 描边变 primaryHover
        const auto* t3 =
            ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
        check(t3 != nullptr && t3->onHoverChanged != nullptr,
              "§6 按钮 hover 回调应接线");
        if (t3 != nullptr) t3->onHoverChanged(true);
        composeFrame();
        int particles = 0;
        for (int i = 0; i < 8; ++i) {
            if (ui->find("app.sidebar.toggle.particle." + std::to_string(i)) !=
                nullptr) {
                ++particles;
            }
        }
        check(particles == 8, "§6 hover 后应迸发 8 颗粒子");
        check(ui->find("app.sidebar.toggle.particle.driver") != nullptr,
              "§6 粒子生命泵应挂树");
        const auto* hovBtn = ui->find("app.sidebar.toggle");
        check(hovBtn != nullptr &&
                  nearEq(hovBtn->border.color.r,
                         core::theme::current().colors.primaryHover.r),
              "§6 hover 描边应变 primaryHover");
        // hover 停留超过粒子生命（480ms）→ 自止消失（不循环判据：hover 仍 true）
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        composeFrame();
        check(ui->find("app.sidebar.toggle.particle.0") == nullptr &&
                  ui->find("app.sidebar.toggle.particle.driver") == nullptr,
              "§6 粒子到期应自止（hover 停留不循环）");
        // 移出再进 → 重触发；保持 hover 态给收尾截图（绿框 + 粒子初发段）
        const auto* t4 = ui->find("app.sidebar.toggle");
        if (t4 != nullptr) {
            t4->onHoverChanged(false);
            t4->onHoverChanged(true);
        }
        composeFrame();
        check(ui->find("app.sidebar.toggle.particle.0") != nullptr,
              "§6 移出再进粒子应重触发");
    }

    // 退出前落一张「当前状态图」（--screenshot <路径>）
    // 通用看图口：跑一次 module_toggle_sidebar_probe 就能拿到此刻侧栏长什么样。
    // 不传该开关时 captureIfRequested 立刻返回（零开销 / 不写文件）。
    probe::captureIfRequested(args, window, runtime, &passed);

    // 收尾顺序：模块 onStop → runtime → 窗口 → 后端
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
