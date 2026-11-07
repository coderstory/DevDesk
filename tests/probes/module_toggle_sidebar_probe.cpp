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
#ifdef _WIN32
#include <windows.h>  // §6b 真指针注入（SetCursorPos/ClientToScreen/POINT/HWND）
#endif
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

// §6b TRACE 用（172 诊断临时，随结论落定后撤）：GLFW 单回调槽——链式转发须把
// EUI 原回调存文件级静态（同 g_agent/g_router 无捕获约束先例）。
// 唯一使用点在同一 _WIN32 段内（真指针链只在 Windows 跑），非 Windows 不声明防 -Wunused。
#ifdef _WIN32
GLFWcursorposfun g_prevCursorCb = nullptr;
#endif

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
    bool realPointerChain = false;  // --real-pointer：§6b 真指针命中链（见段注）
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--real-pointer") realPointerChain = true;
    }
    (void)realPointerChain;  // 非 Windows 构建不跑 §6b
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
    // §7 bug168 用：补桩模块把导航内容高撑过 viewport（触发真实溢出/滚动到底）
    StubPageModule markdown(::core::PageId::Markdown, "笔记");
    StubPageModule todo(::core::PageId::Todo, "待办");
    StubPageModule schedule(::core::PageId::Schedule, "日程");
    StubPageModule proxy(::core::PageId::ClaudeProxy, "代理");
    StubPageModule memory(::core::PageId::Memory, "记忆");
    StubPageModule wallpaper(::core::PageId::Wallpaper, "壁纸");
    StubPageModule mcp(::core::PageId::Mcp, "MCP");
    StubPageModule terminal(::core::PageId::Terminal, "终端");
    StubPageModule about(::core::PageId::About, "关于");

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

    // 6) B6 need172 v2：收缩按钮几何（直径按档 40/36 正圆 + 1px 常态描边 + 区 56/52
    //    上偏 4px 定位 + 热区外扩 4px；2026-11-06 bug146 dd0f28db 定稿）+ 真点击切换
    //    侧栏宽度 + hover 一次性粒子 v2
    //（12 颗 / 4~6px / 0.75 / 800ms+延迟 0~80ms；迸发/自止/重触发）。
    //    hover 驱动走热区 onHoverChanged 手调（锁屏机无真实指针 move 注入；与真指针
    //    共用同一接线，回调由 EUI 命中派发写入）。
    composeFrame();
    {
        // 档位几何按 §10.2（availH<680 极紧 / <712 紧凑 / 其余正常）。判据跟产品
        // 同源：availH = 侧栏根实高（bodyScreen = compose 高 − 40 自绘标题条），
        // 不能按 args.windowH−40 推——窗口被工作区钳制后请求值 ≠ 实际值
        //（probe_harness bug183/185 口径），本机 1000 请求钳到 ~726 实际即落紧凑档。
        const auto* sidebarRoot = ui != nullptr ? ui->find("app.sidebar") : nullptr;
        const float availHActual =
            sidebarRoot != nullptr ? sidebarRoot->frame.height : bodyH;
        const bool tierNormal = availHActual >= 712.0f;
        // 2026-11-06 老板三轮实测定稿（bug146，dd0f28db）：正常档 32→36→40、
        // 紧凑/极紧 28→36；预留区 56/52（随钮 +4）。
        const float expToggleSize = tierNormal ? 40.0f : 36.0f;
        const float expRadius = expToggleSize * 0.5f;
        const auto* toggle =
            ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
        check(toggle != nullptr, "§6 收缩按钮应存在");
        if (toggle != nullptr) {
            check(nearEq(toggle->frame.width, expToggleSize) &&
                      nearEq(toggle->frame.height, expToggleSize),
                  "§6 按钮直径应 " + std::to_string(expToggleSize));
            check(nearEq(toggle->radius, expRadius), "§6 按钮应正圆");
            check(nearEq(toggle->border.width, 1.0f),
                  "§6 常态应有 1px 圆框描边");
            // 预留区内定位（bug146 dd0f28db）：toggleY = H − zone + 4（区内上偏
            // 4px）→ 按钮底距侧栏底 = zone − 4 − 钮径 = 12（正常 56/紧凑 52 同值）
            const auto* sidebarEl = ui->find("app.sidebar");
            check(sidebarEl != nullptr &&
                      nearEq(toggle->frame.y + toggle->frame.height + 12.0f,
                             sidebarEl->frame.y + sidebarEl->frame.height),
                  "§6 按钮应在预留区内（底距侧栏底 12px，dd0f28db）");
            // 热区 = 绘制区外扩 4px（§10.2）
            const auto* hot = ui->find("app.sidebar.toggle.hot");
            check(hot != nullptr, "§6 热区元素应存在");
            if (hot != nullptr) {
                check(nearEq(hot->frame.width, expToggleSize + 8.0f) &&
                          nearEq(hot->frame.height, expToggleSize + 8.0f),
                      "§6 热区应 48/44（绘制区外扩 4px，dd0f28db）");
                check(hot->onClick != nullptr, "§6 点击回调应在热区");
            }
            // 真点击（Press/Release 成对排队 → 下一帧消费）→ 侧栏 220→72。
            // 点击热区中心（回调已迁热区）
            if (hot != nullptr) {
                clickElement(*hot);
                composeFrame();
                check(nearEq(app::sidebarWidth(), 72.0f),
                      "§6 真点击后侧栏应折叠（220→72）");
                const auto* folded =
                    ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
                const float expFoldX =
                    (72.0f - expToggleSize) * 0.5f;  // 16（正常）/ 18（矮窗档）
                check(folded != nullptr &&
                          nearEq(folded->frame.x, expFoldX),
                      "§6 折叠态按钮应水平居中 ((72-尺寸)/2=" +
                          std::to_string(expFoldX) + ")");
                const auto* foldedHot =
                    ui != nullptr ? ui->find("app.sidebar.toggle.hot") : nullptr;
                if (foldedHot != nullptr) {
                    clickElement(*foldedHot);
                    composeFrame();
                }
                check(nearEq(app::sidebarWidth(), 220.0f),
                      "§6 再点击应展开（72→220）");
            }
        }

        // hover 一次性粒子 v2：迸发 12 颗（延迟 0~80ms，先候 120ms 全起爆）+
        // 泵挂树 + 描边变 primaryHover
        const auto* t3 =
            ui != nullptr ? ui->find("app.sidebar.toggle.hot") : nullptr;
        check(t3 != nullptr && t3->onHoverChanged != nullptr,
              "§6 热区 hover 回调应接线");
        if (t3 != nullptr) t3->onHoverChanged(true);
        composeFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        composeFrame();
        int particles = 0;
        for (int i = 0; i < 12; ++i) {
            if (ui->find("app.sidebar.toggle.particle." + std::to_string(i)) !=
                nullptr) {
                ++particles;
            }
        }
        check(particles == 12, "§6 hover 后应迸发 12 颗粒子");
        check(ui->find("app.sidebar.toggle.particle.driver") != nullptr,
              "§6 粒子生命泵应挂树");
        const auto* hovBtn = ui->find("app.sidebar.toggle");
        check(hovBtn != nullptr &&
                  nearEq(hovBtn->border.color.r,
                         core::theme::current().colors.primaryHover.r),
              "§6 hover 描边应变 primaryHover");
        // hover 停留超过粒子生命（v3.1 ≤1760ms）→ 自止消失（不循环判据：hover 仍 true）
        std::this_thread::sleep_for(std::chrono::milliseconds(1900));
        composeFrame();
        check(ui->find("app.sidebar.toggle.particle.0") == nullptr &&
                  ui->find("app.sidebar.toggle.particle.driver") == nullptr,
              "§6 粒子到期应自止（hover 停留不循环）");
        // 移出再进 → 重触发；保持 hover 态给收尾截图（绿框 + 粒子初发段）
        const auto* t4 = ui->find("app.sidebar.toggle.hot");
        if (t4 != nullptr) {
            t4->onHoverChanged(false);
            t4->onHoverChanged(true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));  // > hash 延迟上限 80ms
        composeFrame();
        check(ui->find("app.sidebar.toggle.particle.0") != nullptr,
              "§6 移出再进粒子应重触发");
    }

    // §7 bug168：滚到底后末行文本不得压进悬浮收缩按钮区（need186 §11.2 契约：
    // viewport 底 = H−toggleZoneH、按钮顶 = H−zone+4 → 恒 ≥4px 空隙，viewport
    // .clip() 保证行物理画不进预留区）。补桩模块撑爆内容高 + 预置超界 offset
    //（drawSidebar 侧按 [0, contentH−viewportH] clamp 收敛），断言末行文本底
    // ≤ 按钮顶，且内容溢出时末行确实贴到裁剪边界（不是靠内容太短蒙混）。
    {
        router.registerModule(&markdown);
        router.registerModule(&todo);
        router.registerModule(&schedule);
        router.registerModule(&proxy);
        router.registerModule(&memory);
        router.registerModule(&wallpaper);
        router.registerModule(&mcp);
        router.registerModule(&terminal);
        router.registerModule(&about);
        // 展开 ReleaseGroup（§5 收起过）→ 子行列出，矮窗下内容高必超 viewport
        app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
        app::setNavScrollOffsetForTests(1.0e6f);
        composeFrame();
        // 真用户流（滚轮滚到底）：setNavScrollOffsetForTests 强制预置会与滚动态
        // 回写环互搏（强制值被回写清零，测试钩子 quirk 非产品路径）——改走真输入
        // 链：指针移进导航区 → 滚轮向下 12 格（step=itemH+gap）→ 壁钟等过 120ms
        // 滚动过渡（scrollview.h transition_ 默认档）→ 稳帧。
        composeFrame();
        const auto* navView0 =
            ui != nullptr ? ui->find("app.sidebar.nav") : nullptr;
        check(navView0 != nullptr, "§7 滚动区应存在");
        if (navView0 != nullptr) {
            const float cx =
                navView0->frame.x + navView0->frame.width * 0.5f;
            const float cy =
                navView0->frame.y + navView0->frame.height * 0.5f;
            core::queuePointerMotion(window, cx, cy, {}, {});
            composeFrame();
            for (int i = 0; i < 12; ++i) {
                probe::queueScroll(window, runtime, 0.0, -1.0);
                composeFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            composeFrame();
            composeFrame();
        }
        // 断言（用户可见面，bug168 验收口径）：
        // ① §11.2 区位契约：viewport 底 = 按钮顶 − 4（预留区让位恒有空隙）；
        // ② 任何 label 的可见底（frame ∩ viewport 裁剪域）≤ 按钮顶——frame 越
        //    界段被 scissor 裁剪不参与视觉（runtime_render clip 段），不存在
        //    「文本画进按钮区」的像素事实。
        const auto* navView =
            ui != nullptr ? ui->find("app.sidebar.nav") : nullptr;
        const auto* canvas =
            ui != nullptr ? ui->find("app.sidebar.nav.canvas") : nullptr;
        const auto* toggle7 = ui != nullptr ? ui->find("app.sidebar.toggle")
                                            : nullptr;
        check(navView != nullptr && canvas != nullptr && toggle7 != nullptr,
              "§7 滚动区/画布/按钮应存在");
        if (navView != nullptr && canvas != nullptr && toggle7 != nullptr) {
            const float viewBottom =
                navView->frame.y + navView->frame.height;
            const float toggleTop = toggle7->frame.y;
            const bool overflow =
                canvas->frame.height > navView->frame.height + 0.5f;
            check(std::fabs(viewBottom - (toggleTop - 4.0f)) <= 0.5f,
                  "§7 §11.2 区位契约：viewport 底 " +
                      std::to_string(viewBottom) + " 应 = 按钮顶 " +
                      std::to_string(toggleTop) + " − 4");
            const core::dsl::Element* last = nullptr;
            float lastBottom = -1.0f;
            for (const auto& root : ui->roots()) {
                std::vector<const core::dsl::Element*> stack{root.get()};
                while (!stack.empty()) {
                    const core::dsl::Element* node = stack.back();
                    stack.pop_back();
                    const std::string& id = node->id;
                    if (id.rfind("app.sidebar.item.", 0) == 0 &&
                        id.size() > 6 &&
                        id.compare(id.size() - 6, 6, ".label") == 0) {
                        const float bottom =
                            node->frame.y + node->frame.height;
                        if (bottom > lastBottom) {
                            lastBottom = bottom;
                            last = node;
                        }
                    }
                    for (const auto& child : node->children) {
                        stack.push_back(child.get());
                    }
                }
            }
            check(last != nullptr, "§7 应存在侧栏条目 label（展开态）");
            if (last != nullptr) {
                const float visibleBottom = std::min(lastBottom, viewBottom);
                check(visibleBottom <= toggleTop + 0.5f,
                      "§7 末行文本可见底 " + std::to_string(visibleBottom) +
                          " 不得低于按钮顶 " + std::to_string(toggleTop) +
                          "（文本不许被按钮遮住，bug168）");
                std::cout << "§7 bug168 末行frame底=" << lastBottom
                          << " 可见底=" << visibleBottom
                          << " viewport底=" << viewBottom
                          << " 按钮顶=" << toggleTop
                          << " overflow=" << (overflow ? 1 : 0) << "\n";
            }
        }
    }

    // §6b --real-pointer：真指针命中链（bug150 口径盲区补课，池172）。§6 的 hover
    // 断言是 onHoverChanged 手调，从未验证「真 OS 指针 → WM_MOUSEMOVE → GLFW
    // CursorPosCallback → queuePointerMotion → runtime.update 命中派发」段——
    // 池172 真机「特效全缺席、探针全绿」的探针侧盲区正是这里。本段默认关
    //（无头/锁屏环境跑不了：需真桌面开窗 + 移动真实光标），显式 --real-pointer
    // 才跑；并临时装主窗同款自绘壳子类（WM_NCHITTEST caption/边缘带），复刻
    // 真机窗口壳层、排除「壳层吃消息」变量。
    // ⚠ 度量纪律：GLFW glfwInit 自提 PerMonitor DPI 感知（无 manifest 也生效），
    // 缩放屏上窗口/事件/帧缓冲全是物理像素——本段不走 pollOnce/update(1,1) 的
    // 探针假想世界，逐帧读真窗口 metrics、按真 app 同款流驱动（update(cs,ps) +
    // compose(fbw/cs, fbh/cs, cs)）；元素树每次 compose 重建，指针一律即取即用，
    // 禁跨 compose 缓存（首版工装栽在悬垂 base->frame 上，坐标成垃圾值）。
#ifdef _WIN32
    if (realPointerChain) {
        auto* glfwWin = static_cast<GLFWwindow*>(window);
        auto liveContentScale = [&]() {
            float csx = 1.0f, csy = 1.0f;
            glfwGetWindowContentScale(glfwWin, &csx, &csy);
            return (csx + csy) * 0.5f;
        };
        const float cs0 = liveContentScale();
        core::window::setWindowCaptionHitTestHandler(
            window,
            [cs0](int x, int y, int width, int height, bool maximized) {
                // 复刻 app_sidebar.cpp titlebarCaptionHit（物理→逻辑后判顶部 40
                // 条带；EUI 回调给物理像素，真 app 在 glfw_app_main 包同款换算）
                (void)x;
                (void)width;
                (void)height;
                (void)maximized;
                const float s = cs0 > 0.0f ? cs0 : 1.0f;
                const float ly = static_cast<float>(y) / s;
                return ly >= 0.0f && ly < 40.0f;
            },
            /*resizable=*/true);
        glfwShowWindow(glfwWin);
        glfwFocusWindow(glfwWin);  // 置顶：真 WM_MOUSEMOVE 只派发给光标下的窗口

        // 真 app 同款度量流单帧：update(cs, ps) + compose(物理/cs 逻辑尺寸, cs)
        auto composeReal = [&]() {
            int ww = 0, wh = 0, fw = 0, fh = 0;
            glfwGetWindowSize(glfwWin, &ww, &wh);
            glfwGetFramebufferSize(glfwWin, &fw, &fh);
            const float cs = liveContentScale();
            const float ps = (ww > 0 && wh > 0)
                                 ? (fw / static_cast<float>(ww) +
                                    fh / static_cast<float>(wh)) * 0.5f
                                 : 1.0f;
            runtime.update(window, 1.0f / 60.0f, ps, cs);  // 签名序：pointerScale 在前、dpiScale 在后
            runtime.compose("app", static_cast<float>(fw) / cs,
                            static_cast<float>(fh) / cs, cs,
                            [&](eui::Ui& u, const eui::Screen& screen) {
                                ui = &u;
                                app::compose(u, screen);
                            });
        };
        auto pumpRealFrames = [&](int totalMs) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(totalMs);
            while (std::chrono::steady_clock::now() < deadline) {
                glfwPollEvents();  // 真 WM_MOUSEMOVE → GLFW 回调 → 输入队列
                composeReal();     // update（命中派发）+ compose（树反映）
            }
        };

        const HWND hwnd = static_cast<HWND>(
            core::window::nativeWindowInfo(window).platformWindow);
        POINT origin{0, 0};
        check(ClientToScreen(hwnd, &origin) != FALSE,
              "§6b ClientToScreen 应成功");
        int fbw0 = 0, fbh0 = 0;
        glfwGetFramebufferSize(glfwWin, &fbw0, &fbh0);
        // 窗内远角（右缘内收 80 / 底缘内收 140 物理像素：避开边缘 resize 带与
        // 底部按钮区）
        const int farX = origin.x + fbw0 - 80;
        const int farY = origin.y + fbh0 - 140;
        POINT saved{};
        GetCursorPos(&saved);

        SetCursorPos(farX, farY);
        pumpRealFrames(1900);  // > 粒子生命 v3.1 ≤1760ms：清掉 §6 手调遗留态做干净基线
        const auto* base = ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
        check(base != nullptr, "§6b toggle 应存在");
        check(ui->find("app.sidebar.toggle.particle.0") == nullptr,
              "§6b 基线（指针在远角）应无粒子");
        // 链路分段诊断 A：合成 motion（物理坐标）经同一条 cs 流 update 能否点燃
        // hover——燃 = 队列→派发段健康，断点在 GLFW/消息层；不燃 = 派发段问题。
        {
            const float lcx = base->frame.x + base->frame.width * 0.5f;
            const float lcy = base->frame.y + base->frame.height * 0.5f;
            core::queuePointerMotion(window, lcx * cs0, lcy * cs0, {}, {});
            composeReal();
            const auto* st = ui->find("app.sidebar.toggle");
            std::cout << "§6b diag A synthMotion(physical) hoverBorder="
                      << (st != nullptr &&
                                  nearEq(st->border.color.r,
                                         core::theme::current()
                                             .colors.primaryHover.r)
                              ? "ON"
                              : "OFF")
                      << "\n";
            core::queuePointerMotion(
                window, static_cast<double>(fbw0 - 80),
                static_cast<double>(fbh0 - 140), {}, {});
            composeReal();  // 移出复位
        }
        // 诊断：坐标世界 + 双尺度命中对照（分辨率真机上 update(dpi=1) 的逻辑
        // 边界与物理事件坐标错位会让本段伪红——故本段走 cs 流，此处打印留痕）
        // 即取即用：diag A 已连续 compose 两帧、元素树每帧重建，baseline 段取的
        // base 已悬垂（首版工装同坑——读到的是旧树残骸的垃圾坐标，两次跑漂移
        // 130,313 / 1040,675 即此）——这里重取当帧元素。
        const auto* live =
            ui != nullptr ? ui->find("app.sidebar.toggle") : nullptr;
        if (live != nullptr) {
            const float lcx = live->frame.x + live->frame.width * 0.5f;
            const float lcy = live->frame.y + live->frame.height * 0.5f;
            std::cout << "§6b diag fb=" << fbw0 << "x" << fbh0
                      << " cs=" << cs0 << " toggleLogicalCenter=(" << lcx << ","
                      << lcy << ") hit(dpi=cs)="
                      << (runtime.hitTestInteractiveAt(lcx, lcy, cs0).empty()
                              ? "MISS"
                              : "HIT")
                      << "\n";
            // 防污染：diag A 合成迸发的粒子 v3.1 ≤1760ms 生命——先泵 1900ms 清零再进真
            // 指针段（首版工装的粒子计数被合成残留顶成假绿的坑）。
            pumpRealFrames(1900);
            // TRACE（172 诊断临时）：链式光标回调——真实 motion 到达即打印坐标再
            // 转发 EUI 原回调。断点分判：无打印=WM_MOUSEMOVE/GLFW 层断；打印坐标
            // ≈逻辑值（130/313 级）=DPI 虚拟化错位；坐标=物理值（176/424 级）仍不
            // 燃=队列/派发段对真事件另有蹊跷。
            g_prevCursorCb = glfwSetCursorPosCallback(
                glfwWin, [](GLFWwindow* w, double x, double y) {
                    std::cout << "§6b TRACE realMotion " << x << "," << y
                              << "\n";
                    if (g_prevCursorCb != nullptr) {
                        g_prevCursorCb(w, x, y);
                    }
                });
            // 真指针移入按钮中心（逻辑中心 × cs → 物理）→ hover 应由真实命中
            // 派发点燃
            const int bx =
                origin.x + static_cast<int>(lcx * cs0);
            const int by =
                origin.y + static_cast<int>(lcy * cs0);
            // 顶层置顶（工装强化）：SetForegroundWindow 有前台权限限制——bash/CI
            // 派生的探针进程常常夺不动前台，被运行中的窗口盖住时真指针事件按
            // z 序派发给别人=链路假断（windowFromPoint=other 即此伪影）。§6b
            // 期间临时 TOPMOST 保证探针窗真在指针之下，段尾还原 NOTOPMOST。
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            glfwShowWindow(glfwWin);
            glfwFocusWindow(glfwWin);
            pumpRealFrames(80);  // 置顶/焦点落地
            // TRACE：消息归属——光标点下的窗口/前台窗是否本探针窗（被更顶层窗口
            // 盖住时 WM_MOUSEMOVE 归别人，链路本身未必断）。
            {
                const POINT probePt{bx, by};
                std::cout << "§6b TRACE windowFromPoint="
                          << (WindowFromPoint(probePt) == hwnd ? "probe"
                                                               : "other")
                          << " foreground="
                          << (GetForegroundWindow() == hwnd ? "probe"
                                                            : "other")
                          << "\n";
            }
            SetCursorPos(bx, by);
            {
                double gx = 0.0, gy = 0.0;
                glfwGetCursorPos(glfwWin, &gx, &gy);
                std::cout << "§6b TRACE glfwCursorPos=" << gx << "," << gy
                          << " expect物理≈" << lcx * cs0 << "," << lcy * cs0
                          << "\n";
            }
            pumpRealFrames(300);  // hover 过渡 150ms 内 + 粒子 1760ms 窗口内计数
            int burst = 0;
            for (int i = 0; i < 12; ++i) {
                if (ui->find("app.sidebar.toggle.particle." + std::to_string(i)) !=
                    nullptr) {
                    ++burst;
                }
            }
            check(burst == 12, "§6b 真指针 hover 应迸发 12 颗粒子");
            const auto* hov = ui->find("app.sidebar.toggle");
            check(hov != nullptr &&
                      nearEq(hov->border.color.r,
                             core::theme::current().colors.primaryHover.r),
                  "§6b 真指针 hover 描边应变 primaryHover");
            // 移出 → hover 离开 + 粒子自止（泵 1900ms > v3.1 生命 ≤1760ms）
            SetCursorPos(farX, farY);
            pumpRealFrames(1900);
            check(ui->find("app.sidebar.toggle.particle.0") == nullptr &&
                      ui->find("app.sidebar.toggle.particle.driver") == nullptr,
                  "§6b 移出后粒子应自止");
            SetCursorPos(saved.x, saved.y);  // 还原光标
            glfwSetCursorPosCallback(glfwWin, g_prevCursorCb);  // TRACE 收尾还原
            ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // 顶层还原
        }
        core::window::setWindowCaptionHitTestHandler(window, nullptr, true);
        composeReal();
    }
#endif

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
