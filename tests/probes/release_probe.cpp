// release_probe.cpp — 上线模块视觉契约「真窗口」探针。
//
// 与无头单测的分工：单测证明纯逻辑与单页组件能跑；本 probe 在真窗口 + 真
// Runtime 帧节拍里渲染上线模块（含真实 app::compose 侧边栏装配），把视觉 token
// 钉成数值断言——不接受「看起来对」。
//
// 数值出处：
//   - 侧边栏行高/缩进：src/app_sidebar.cpp（ui_kit::kNavItemH=40、
//     kChildIndent=16）与 design §10 分组树；
//   - 基础配置：src/module_release/release_config_ui.cpp 顶部视觉契约注释
//     （padding 32、页标题 24、chip 22/11/pill）与 token 文档；
//   - 任务列表：release_list_ui.h 瀑布流几何常量（最小卡宽 320、间距 16、
//     首屏 kReleaseTaskPageSize=10）与 kCardRadius=12；
//   - 执行页：release_detail_draw.cpp（kEnvHeaderH=56、kStepRowH=44（池 53）、
//     kStepDot=18、chip 22/11/pill）。
// 失败信息统一打印「期望 / 实际 / token 出处」三段。
//
// 运行：release_probe.exe [--frames N] [--size 1200x1000]
//                        [--screenshot <png>] [--screenshot-seeded <png>]
//                        [--screenshot-crowded <png>]
//                        [--accept-shot-env-tab-empty <png>]
//                        [--shot-detail <png>] [--shot-track <png>]
//                        [--shot-newtask <png>] [--theme dark]
// 数据：用 test_support::TestEnv 建已 migrate 的临时库并注册为进程级
// appDatabase——不读写用户数据，也不依赖 DEVDESK_DATA_ROOT。
//
// 覆盖分组：
//   A 侧边栏分组树（8 个既有条目 + 「上线」父项 + 2 子项、展开/收起）
//   B 基础配置页（padding / 标题字号 / 类型 chip / 后端 5 tag / 前端无配置项）
//   C 任务列表瀑布流（首屏 10 → 触底 20、卡宽、列/纵间距、圆角）
//   D 执行页（焦点式双栏：索引列分组头 / 服务索引项 / 焦点区服务卡、步骤行、
//     勾选框、状态 chip、模式徽标色值：编辑中性 / 发布中唯一绿染——池 57）
//   D3 执行页挤压态（2 环境 × 3 服务：索引列分组头与条目纵向堆叠不重叠、
//     焦点区仅 1 张服务卡且不越界——池 116，need192 焦点式双栏语义）
//   B2 基础配置「环境定义」Tab 空态（池 116，落图档）
//   E 提醒（到期聚合 → 提醒窗内容）：数据层断言（聚合 / 档位 / 跳转负载）；
//     全屏窗真开仍不可达，见 main() 末尾的说明注释。
#include "probes/probe_harness.h"

#include "test_support.h"
#include "app_registry.h"   // create_release_*_module / kModuleRegistry
#include "app_sidebar.h"    // app::compose / toggleSidebarGroup / resetSidebarGlobals
#include "core/database.h"  // appDatabase（TestEnv 注册）
#include "core/module_interface.h"
#include "core/router.h"
#include "core/theme.h"
#include "module_release/release_config_ui.h"
#include "module_release/release_detail_ui.h"
#include "module_release/release_list_ui.h"
#include "module_release/release_logic.h"  // E 组：dueItems / highestLevel / planDateElapsedMinutes
#include "module_release/release_store.h"
#include "module_release/release_types.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <thread>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// app_sidebar.cpp 经 dsl_app_impl.h 引用 dslAppConfig；probe 链接面没有
// main.cpp，按 module_toggle_sidebar_probe 同款提供最小桩（只用到标题）。
namespace app {
const app::DslAppConfig& dslAppConfig() {
    static const DslAppConfig config =
        DslAppConfig{}.title("devdesk_release_probe");
    return config;
}
}  // namespace app

namespace {

// 侧边栏树里 8 个「改动前既有」的顶级模块（PageId 枚举序 = 侧边栏顺序）
constexpr const char* kExistingSidebarPages[] = {
    "Home", "Markdown", "Todo", "ClaudeProxy", "Agent", "Wallpaper",
    "Settings", "About",
};

// 桩页面模块：侧边栏几何只依赖 pageId/displayName/iconGlyph，页面内容用
// 最小文本占位即可（真实页面已在无头单测覆盖，本 probe 的断言对象是侧栏装配）
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

bool nearPx(float actual, float expected) {
    return std::fabs(actual - expected) <= 0.5f;
}

// 色值断言按 1/255 容差逐通道比较（token 落 element 时无浮点漂移）。
bool colorNear(const eui::Color& actual, const eui::Color& expected) {
    const float tol = 1.0f / 255.0f;
    return std::fabs(actual.r - expected.r) <= tol &&
           std::fabs(actual.g - expected.g) <= tol &&
           std::fabs(actual.b - expected.b) <= tol &&
           std::fabs(actual.a - expected.a) <= tol;
}

// 断言收集器：每条失败都带期望/实际/token 出处，避免「看起来对」。
struct Checks {
    bool passed = true;

    void fail(const std::string& message) {
        std::cerr << "release_probe: FAIL — " << message << "\n";
        passed = false;
    }

    const core::dsl::Element* need(core::dsl::Ui* ui, const std::string& id,
                                   const char* what) {
        const core::dsl::Element* element = ui != nullptr ? ui->find(id) : nullptr;
        if (element == nullptr) {
            fail(std::string("结构缺失：") + what + "（id=" + id + "）");
        }
        return element;
    }

    void expectNear(const char* what, float actual, float expected,
                    const char* tokenSource) {
        if (!nearPx(actual, expected)) {
            std::ostringstream out;
            out << what << "：期望 " << expected << "，实际 " << actual
                << "（token 出处：" << tokenSource << "）";
            fail(out.str());
        }
    }

    void expectText(const core::dsl::Element* element, const char* what,
                    const std::string& expected) {
        if (element == nullptr) return;
        if (element->text != expected) {
            fail(std::string(what) + "：期望文案「" + expected + "」，实际「" +
                 element->text + "」");
        }
    }

    void expectTrue(bool condition, const std::string& what) {
        if (!condition) fail(what);
    }

    void expectColor(const char* what, const eui::Color& actual,
                     const eui::Color& expected, const char* tokenSource) {
        if (!colorNear(actual, expected)) {
            std::ostringstream out;
            out << what << "：期望 rgba(" << expected.r << "," << expected.g
                << "," << expected.b << "," << expected.a << ")，实际 rgba("
                << actual.r << "," << actual.g << "," << actual.b << ","
                << actual.a << "）（token 出处：" << tokenSource << "）";
            fail(out.str());
        }
    }
};

// 瀑布流卡片根节点：id = release.tasks.card.<taskId>（子孙节点多一个 '.'）
std::vector<const core::dsl::Element*> cardRoots(core::dsl::Ui& ui) {
    static const std::string prefix = "release.tasks.card.";
    std::vector<const core::dsl::Element*> out;
    std::function<void(const core::dsl::Element&)> walk =
        [&](const core::dsl::Element& node) {
            if (node.id.compare(0, prefix.size(), prefix) == 0 &&
                node.id.find('.', prefix.size()) == std::string::npos) {
                out.push_back(&node);
            }
            for (const auto& child : node.children) {
                walk(*child);
            }
        };
    for (const auto& root : ui.roots()) {
        walk(*root);
    }
    return out;
}

// 池116②：按 id 规则收集元素树（先根后子），供挤压态几何断言用。
std::vector<const core::dsl::Element*> collectElements(
    core::dsl::Ui& ui, const std::function<bool(const std::string&)>& match) {
    std::vector<const core::dsl::Element*> out;
    std::function<void(const core::dsl::Element&)> walk =
        [&](const core::dsl::Element& node) {
            if (match(node.id)) out.push_back(&node);
            for (const auto& child : node.children) walk(*child);
        };
    for (const auto& root : ui.roots()) walk(*root);
    return out;
}

// 种子数据：2 个基础配置服务（后端 + PC 前端）→ 20 条任务（列表分页口径）→
// 1 条执行页任务（含 1 环境 × 1 后端服务，自动补齐 5 配置项步骤）→
// 1 条挤压态任务（2 环境 × 3 后端服务，池 116②）。
bool seedReleaseData(module::release::ReleaseStore& store,
                     std::int64_t* backendServiceId,
                     std::int64_t* frontendServiceId,
                     std::int64_t* detailTaskId,
                     std::int64_t* crowdedTaskId) {
    using namespace module::release;
    const auto addService = [&](const char* name, ServiceKind kind,
                                const char* branch, int sort) -> std::int64_t {
        ServiceDef def;
        def.name = name;
        def.kind = kind;
        def.defaultBranch = branch;
        def.sort = sort;
        const auto result = store.upsertService(def);
        return result.isOk() ? result.value() : 0;
    };
    *backendServiceId = addService("后端服务", ServiceKind::Backend, "main", 1);
    *frontendServiceId = addService("PC 前端", ServiceKind::PcWeb, "main", 2);
    if (*backendServiceId == 0 || *frontendServiceId == 0) return false;

    const auto addTask = [&](const std::string& planDate) -> std::int64_t {
        Task task;
        task.planDate = planDate;
        task.status = TaskStatus::Draft;
        const auto created = store.createTask(task);
        if (created.isFail()) return 0;
        TaskEnv env;
        env.taskId = created.value();
        env.envName = "EM";
        env.startMinute = 9 * 60;
        env.endMinute = 18 * 60;
        TaskService service;
        service.serviceName = "release-svc";
        service.kind = ServiceKind::Backend;
        service.defaultBranch = "main";
        service.branch = "feature/release";
        const auto added = store.addTaskEnv(env, {service});
        return added.isOk() ? created.value() : 0;
    };

    std::int64_t pendingSeedId = 0;
    std::int64_t runningSeedId = 0;
    for (int i = 1; i <= 20; ++i) {
        char planDate[16] = {};
        std::snprintf(planDate, sizeof(planDate), "2026-09-%02d", i);
        const std::int64_t taskId = addTask(planDate);
        if (taskId == 0) return false;
        if (i == 20) pendingSeedId = taskId;  // 最近两个日程，见下方状态翻转
        if (i == 19) runningSeedId = taskId;
    }
    // 113 批次B：两个种子转 Pending / Running——列表首屏给「进度条 + 下一步」
    // 可见样本。任务总量保持 20，触底分页断言（20）不变。
    {
        auto all = store.listTasks();
        if (all.isFail()) return false;
        for (auto& task : all.value()) {
            if (task.id != pendingSeedId && task.id != runningSeedId) continue;
            task.status = task.id == pendingSeedId ? TaskStatus::Pending
                                                   : TaskStatus::Running;
            if (store.updateTask(task).isFail()) return false;
        }
    }
    // 池116②：挤压态任务 2 环境 × 3 后端服务（每服务带分支 → 合并/部署步骤
    // 齐全）。plan_date 全表唯一：09-01..09-20 已被列表种子占用，压到 08-31
    // 沉底，不干扰 C 组首屏前三行内容断言；总量 21 不影响首屏 10 / 触底 20。
    {
        Task crowded;
        crowded.planDate = "2026-08-31";
        crowded.status = TaskStatus::Draft;
        const auto created = store.createTask(crowded);
        if (created.isFail()) return false;
        const char* envNames[2] = {"EM", "UAT"};
        for (int e = 0; e < 2; ++e) {
            TaskEnv env;
            env.taskId = created.value();
            env.envName = envNames[e];
            env.startMinute = 9 * 60;
            env.endMinute = 18 * 60;
            std::vector<TaskService> services;
            for (int s = 0; s < 3; ++s) {
                const char suffix = static_cast<char>('A' + s);
                TaskService svc;
                svc.serviceName = std::string("crowd-svc-") + suffix;
                svc.kind = ServiceKind::Backend;
                svc.defaultBranch = "main";
                svc.branch = std::string("feature/crowd") + suffix;
                services.push_back(svc);
            }
            if (store.addTaskEnv(env, services).isFail()) return false;
        }
        *crowdedTaskId = created.value();
    }

    *detailTaskId = addTask("2026-10-27");  // 常规发布日（周二）
    return *detailTaskId != 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    // 池55 QA 验收补图参数（parseProbeArgs 不认识，直接扫 argv；未传 = 零开销）：
    //   --accept-shot-tab <png>          「发布中」Tab 激活态（含行卡内容）
    //   --accept-shot-filter-empty <png> 「已归档」Tab 筛选空态（种子无 Done/Voided）
    //   --accept-shot-empty <png>        清库后的「全部」Tab 全空态
    const auto acceptArg = [](int ac, char** av, const char* flag) {
        for (int i = 1; i + 1 < ac; ++i) {
            if (std::string(av[i]) == flag) return std::string(av[i + 1]);
        }
        return std::string{};
    };
    const std::string acceptTabPath = acceptArg(argc, argv, "--accept-shot-tab");
    const std::string acceptFilterEmptyPath =
        acceptArg(argc, argv, "--accept-shot-filter-empty");
    const std::string acceptEmptyPath =
        acceptArg(argc, argv, "--accept-shot-empty");
    // 池116：环境定义 Tab 空态落图（B2）与执行页挤压态落图（D3）
    const std::string acceptEnvTabEmptyPath =
        acceptArg(argc, argv, "--accept-shot-env-tab-empty");
    const std::string crowdedShotPath =
        acceptArg(argc, argv, "--screenshot-crowded");
    // 列表统一批（need 224 批2）before/after 证据落图开关（未传 = 零开销）
    const std::string detailShotPath = acceptArg(argc, argv, "--shot-detail");
    const std::string trackShotPath = acceptArg(argc, argv, "--shot-track");
    const std::string newTaskShotPath = acceptArg(argc, argv, "--shot-newtask");
    if (!probe::initializeWindowBackend()) return 1;
    // 设计语言基线为浅色微信风（池 53/55）；--theme dark 供亮暗两态落图对照
    //（断言全为 token 相对比对，暗色不假红）。
    bool probeDarkTheme = false;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--theme") {
            probeDarkTheme = std::string(argv[i + 1]) == "dark";
        }
    }
    if (probeDarkTheme) {
        core::theme::setDark();
    } else {
        core::theme::setLight();
    }
    auto window = probe::makeWindow("release_probe", args.windowW, args.windowH);
    if (!window) {
        probe::terminateWindowBackend();
        return 2;
    }

    // 进程级确定性：侧栏展开 + 分组默认折叠复位（有显示环境后才碰 GLFW 之外的全局）
    app::resetSidebarGlobals();

    // 数据底座：临时库 + 注册为 appDatabase（release 模块只取已有连接）
    test_support::TestEnv testEnv("relprobe_", "release", true);
    module::release::ReleaseStore store(testEnv.db->db());

    std::int64_t backendServiceId = 0;
    std::int64_t frontendServiceId = 0;
    std::int64_t detailTaskId = 0;
    std::int64_t crowdedTaskId = 0;
    if (!seedReleaseData(store, &backendServiceId, &frontendServiceId,
                         &detailTaskId, &crowdedTaskId)) {
        std::cerr << "release_probe: 种子数据写入失败（临时库迁移或 store 写入异常）\n";
        core::releaseInputQueue(window);
        core::window::destroyWindow(window);
        probe::terminateWindowBackend();
        return 3;
    }

    // 执行页快照所需的环境 / 服务 id（元素 id 由 DB 自增 id 拼出）
    std::int64_t detailEnvId = 0;
    std::int64_t detailServiceId = 0;
    {
        const auto envs = store.listTaskEnvs(detailTaskId);
        if (envs.isFail() || envs.value().empty()) {
            std::cerr << "release_probe: 读取执行页环境快照失败\n";
            core::releaseInputQueue(window);
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        detailEnvId = envs.value().front().id;
        const auto services = store.listTaskServices(detailEnvId);
        if (services.isFail() || services.value().empty()) {
            std::cerr << "release_probe: 读取执行页服务快照失败\n";
            core::releaseInputQueue(window);
            core::window::destroyWindow(window);
            probe::terminateWindowBackend();
            return 3;
        }
        detailServiceId = services.value().front().id;
    }

    std::unique_ptr<::core::Module> configModule(create_release_config_module());
    std::unique_ptr<::core::Module> tasksModule(create_release_tasks_module());
    configModule->onStart();
    tasksModule->onStart();

    // 侧边栏装配：真 Router + 8 个既有页桩 + 上线两个子页桩（父项不注册，按
    // design §10 由登记表 parent 列承载）。注册序 = 新表序（上线子页在 Settings 之前）。
    ::core::Router router;
    std::vector<std::unique_ptr<StubPageModule>> stubs;
    const auto addStub = [&](::core::PageId page, const char* name) {
        stubs.push_back(std::make_unique<StubPageModule>(page, name));
        router.registerModule(stubs.back().get());
    };
    addStub(::core::PageId::Home, "首页");
    addStub(::core::PageId::Markdown, "笔记");
    addStub(::core::PageId::Todo, "待办");
    addStub(::core::PageId::ClaudeProxy, "Claude");
    addStub(::core::PageId::Agent, "智能体");
    addStub(::core::PageId::Wallpaper, "壁纸");
    addStub(::core::PageId::ReleaseTasks, "任务");
    addStub(::core::PageId::ReleaseConfig, "基础配置");
    addStub(::core::PageId::Settings, "设置");
    addStub(::core::PageId::About, "关于");
    ::core::registerAppRouter(&router);

    core::dsl::Runtime runtime;
    runtime.initialize(window);

    // dsl_app_impl.h 的内联定义落地本 TU（app_assembly.cpp 引用它们）；GLFW
    // 初始化后调用安全（只置原子标志 + postEmptyEvent，probe 有窗）
    app::requestUpdate();
    app::requestShow();

    Checks checks;
    core::dsl::Ui* ui = nullptr;

    // 断言基线跟 composePage 同口径（probe_harness bug183/185：composeModuleFrame
    // 按 framebuffer 反推实际逻辑尺寸布局；窗口被工作区钳制后请求值 ≠ 实际值，
    // 「页面下 padding = 窗高−32」按 --size 请求值比对会假红）。取不到（≤1）时
    // 退回请求值，与 harness 兜底一致。
    const auto [logicalW, logicalH] = probe::windowLogicalSize(window);
    const float w = logicalW > 1.0f ? logicalW : static_cast<float>(args.windowW);
    const float h = logicalH > 1.0f ? logicalH : static_cast<float>(args.windowH);
    const auto composePage = [&](const std::string& pageId, float pageW,
                                 float pageH,
                                 const std::function<void(eui::Ui&,
                                                          const eui::Screen&)>&
                                     draw) {
        probe::composeModuleFrame(runtime, window, pageId, pageW, pageH,
                                  [&](eui::Ui& frameUi,
                                      const eui::Screen& screen) {
                                      ui = &frameUi;
                                      draw(frameUi, screen);
                                  });
    };
    const auto composeApp = [&]() {
        composePage("app", w, h, [](eui::Ui& frameUi,
                                    const eui::Screen& screen) {
            app::compose(frameUi, screen);
        });
    };

    // ================= A. 侧边栏分组树 =================
    // 默认折叠（2026-10 新契约）：父项渲染、子项不渲染、箭头朝下；
    // 展开后两个子项缩进比顶级项大 16；收起后子项不渲染、父项仍在；
    // 既有 8 个条目缩进 0 且顺序不变。
    composeApp();
    {
        const auto* parentHit =
            checks.need(ui, "app.sidebar.item.ReleaseGroup.hit", "「上线」父项行");
        const auto* parentLabel = checks.need(
            ui, "app.sidebar.item.ReleaseGroup.label", "「上线」父项文案");
        checks.expectText(parentLabel, "上线父项文案", "上线");
        if (parentHit != nullptr) {
            checks.expectNear("「上线」父项行高", parentHit->frame.height, 40.0f,
                              "ui_kit::kNavItemH（src/app_sidebar.cpp itemH）");
            checks.expectNear("「上线」父项圆角", parentHit->radius, 6.0f,
                              "theme radiusSmall（src/app_sidebar.cpp renderGroupHeader）");
        }
        // 默认折叠：子项不渲染、箭头朝下
        checks.expectTrue(ui != nullptr &&
                              ui->find("app.sidebar.item.ReleaseTasks.hit") == nullptr,
                          "默认折叠下「任务」子项不应渲染");
        checks.expectTrue(ui != nullptr &&
                              ui->find("app.sidebar.item.ReleaseConfig.hit") == nullptr,
                          "默认折叠下「基础配置」子项不应渲染");
        const auto* foldedChevron = checks.need(
            ui, "app.sidebar.item.ReleaseGroup.chevron", "「上线」父项箭头");
        checks.expectText(foldedChevron, "默认折叠箭头", eui::utf8(0xF078));

        const auto* homeHit =
            checks.need(ui, "app.sidebar.item.Home.hit", "既有首页条目");
        const float topIndentX = homeHit != nullptr ? homeHit->frame.x : 0.0f;

        // 既有 8 条：缩进 0（与 Home 同 x）+ 严格按登记表顺序自上而下
        // （上线父项占一行插在 Wallpaper 与 Settings 之间，只令尾部下移，不改相对序）
        float previousY = -1.0e9f;
        for (const char* page : kExistingSidebarPages) {
            const std::string id =
                std::string("app.sidebar.item.") + page + ".hit";
            const auto* item = checks.need(ui, id, "既有侧边栏条目");
            if (item == nullptr) continue;
            checks.expectNear("既有条目缩进", item->frame.x, topIndentX,
                              "设计 §10：8 行既有条目几何保持不变（缩进 0）");
            checks.expectTrue(item->frame.y > previousY + 1.0f,
                              "既有 8 条顺序与改动前不一致（y 非严格递增）：" + id);
            previousY = item->frame.y;
        }

        // 展开（默认态之外需显式 toggle）：子项回来并缩进 16
        app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
        composeApp();
        const auto* openChevron = checks.need(
            ui, "app.sidebar.item.ReleaseGroup.chevron", "「上线」父项箭头");
        checks.expectText(openChevron, "展开箭头", eui::utf8(0xF077));
        const auto* tasksHit = checks.need(
            ui, "app.sidebar.item.ReleaseTasks.hit", "「任务」子项");
        const auto* configHit = checks.need(
            ui, "app.sidebar.item.ReleaseConfig.hit", "「基础配置」子项");
        if (tasksHit != nullptr) {
            checks.expectNear("「任务」子项相对顶级缩进",
                              tasksHit->frame.x - topIndentX, 16.0f,
                              "app_sidebar.cpp kChildIndent=16");
        }
        if (configHit != nullptr) {
            checks.expectNear("「基础配置」子项相对顶级缩进",
                              configHit->frame.x - topIndentX, 16.0f,
                              "app_sidebar.cpp kChildIndent=16");
        }

        // 收起：子项不渲染、父项仍在（收起后即回到默认折叠，无需再还原）
        app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
        composeApp();
        checks.expectTrue(ui != nullptr &&
                              ui->find("app.sidebar.item.ReleaseTasks.hit") == nullptr,
                          "收起后「任务」子项不应渲染");
        checks.expectTrue(ui != nullptr &&
                              ui->find("app.sidebar.item.ReleaseConfig.hit") == nullptr,
                          "收起后「基础配置」子项不应渲染");
        checks.expectTrue(ui != nullptr &&
                              ui->find("app.sidebar.item.ReleaseGroup.hit") != nullptr,
                          "收起后「上线」父项应仍在");
    }

    // ================= B. 基础配置页 =================
    // 页面 padding 32（上 0）、页标题 24；服务类型 chip 22/999/11；后端固定 5
    // 个配置 tag，两类前端显示「无配置项」。
    composePage("release.config", w, h,
                [&](eui::Ui& frameUi, const eui::Screen& screen) {
                    configModule->compose(frameUi, screen);
                });
    {
        const auto* header =
            checks.need(ui, "release.config.header", "基础配置页页头");
        const auto* card = checks.need(ui, "release.config.card", "基础配置列表卡");
        if (header != nullptr) {
            checks.expectNear("页面左 padding", header->frame.x, 32.0f,
                              "release_config_ui.cpp kPagePad=32（上 0）");
            checks.expectNear("页面内容宽（左右各 32）", header->frame.width,
                              w - 64.0f,
                              "release_config_ui.cpp kPagePad=32");
        }
        if (card != nullptr) {
            checks.expectNear("页面左 padding（卡面）", card->frame.x, 32.0f,
                              "release_config_ui.cpp kPagePad=32（上 0）");
            checks.expectNear("页面右 padding（卡面）", card->frame.width,
                              w - 64.0f,
                              "release_config_ui.cpp kPagePad=32");
            checks.expectNear("页面下 padding", card->frame.y + card->frame.height,
                              h - 32.0f,
                              "release_config_ui.cpp kPagePad=32（下 32）");
        }

        const auto* title = checks.need(ui, "release.page.title", "基础配置页标题");
        checks.expectText(title, "页标题文案", "基础配置");
        if (title != nullptr) {
            checks.expectNear("页标题字号", title->fontSize, 24.0f,
                              "theme fontSizePage=24（release_config_ui.cpp drawPageHeader）");
        }

        const std::string backendBase =
            "release.config.service." + std::to_string(backendServiceId);
        const auto* kindBg =
            checks.need(ui, backendBase + ".kind.bg", "后端服务类型徽标底");
        const auto* kindText =
            checks.need(ui, backendBase + ".kind.text", "后端服务类型徽标文字");
        checks.expectText(kindText, "后端服务类型徽标文案", "后端");
        if (kindBg != nullptr) {
            checks.expectNear("服务类型 chip 高", kindBg->frame.height, 22.0f,
                              "release_config_ui.cpp kChipH=22");
            checks.expectNear("服务类型 chip 圆角", kindBg->radius, 999.0f,
                              "theme radiusPill=999（drawChip .radius）");
        }
        if (kindText != nullptr) {
            checks.expectNear("服务类型 chip 字号", kindText->fontSize, 11.0f,
                              "release_config_ui.cpp fontSizeSmall−1=11（drawChip）");
        }

        // 后端固定 5 项配置 tag（nacos/数据库/定时任务/菜单/网关）
        for (int i = 0; i < 5; ++i) {
            checks.need(ui, backendBase + ".tag." + std::to_string(i) + ".bg",
                        "后端服务配置项 tag");
        }
        checks.expectTrue(ui != nullptr &&
                              ui->find(backendBase + ".tag.more.bg") == nullptr,
                          "后端服务应完整显示 5 个配置 tag，不应折叠为 +N");

        // 两类前端恒 0 项：显示「无配置项」，不得出现 tag
        const std::string frontendBase =
            "release.config.service." + std::to_string(frontendServiceId);
        const auto* noConfig = checks.need(ui, frontendBase + ".noConfig",
                                           "前端服务「无配置项」文案");
        checks.expectText(noConfig, "前端服务配置项文案", "无配置项");
        checks.expectTrue(ui != nullptr &&
                              ui->find(frontendBase + ".tag.0.bg") == nullptr,
                          "前端服务不应出现配置项 tag（类型决定配置项，恒 0 项）");
    }

    // ================= B2. 池116①：环境定义 Tab 空态落图 =================
    // 种子只有服务目录（环境定义恒 0 行）：真实 click 切到「环境定义」Tab，
    // 断言统一空态元素已渲染、服务列表已撤下，再落图。
    // --accept-shot-env-tab-empty <png>（未传 = 零开销）。
    if (!acceptEnvTabEmptyPath.empty()) {
        const auto* envTab =
            ui != nullptr ? ui->find("release.config.tab.1") : nullptr;
        if (envTab == nullptr) {
            checks.fail("B2：环境定义 Tab 元素缺失（release.config.tab.1）");
        } else {
            probe::click(window, runtime,
                         static_cast<double>(envTab->frame.x +
                                             envTab->frame.width * 0.5f),
                         static_cast<double>(envTab->frame.y +
                                             envTab->frame.height * 0.5f));
            std::this_thread::sleep_for(
                std::chrono::milliseconds(200));  // wtab 激活态过渡落位
            const auto recompose = [&] {
                composePage("release.config", w, h,
                            [&](eui::Ui& frameUi, const eui::Screen& screen) {
                                configModule->compose(frameUi, screen);
                            });
            };
            recompose();
            recompose();  // onClick 消费帧 + 列表重排帧
            checks.need(ui, "release.config.env.empty", "环境定义 Tab 空态");
            checks.expectTrue(
                ui->find("release.config.service." +
                         std::to_string(frontendServiceId) + ".kind.bg") == nullptr,
                "切到环境定义 Tab 后服务列表不应再渲染");
            if (!probe::captureFramePng(window, runtime,
                                        acceptEnvTabEmptyPath)) {
                std::cerr << "release_probe: --accept-shot-env-tab-empty 落图失败\n";
                checks.passed = false;
            }
        }
    }

    // ================= C. 任务列表（池 55：单列 64px 行卡 + Tab） =================
    // 首屏 10 行（kReleaseTaskPageSize），触底追加一页 → 20；行高 64、内容列
    // ≤1040、外层白卡 16px 圆角（池 53 大卡特例档）。
    // 分页断言口径固定 1200×600：10 行内容高 640 > 视口 491，把「首屏不足一屏
    // 自动续载」的矮窗分支挡在门外，让「首屏 10」是分页契约而不是窗口巧合。
    const float listW = 1200.0f;
    const float listH = 600.0f;
    const auto composeList = [&]() {
        composePage("release.tasks", listW, listH,
                    [&](eui::Ui& frameUi, const eui::Screen& screen) {
                        tasksModule->compose(frameUi, screen);
                    });
    };
    composeList();  // 骨架帧（池 55 D4：首屏 1 帧延迟装填）
    checks.expectTrue(ui->find("release.tasks.skeleton") != nullptr,
                      "首屏骨架帧应可见（D4：本帧画骨架、下一帧装填）");
    composeList();  // 装填帧
    {
        auto cards = cardRoots(*ui);
        checks.expectTrue(cards.size() == std::size_t{10},
                          "首屏行数：期望 10，实际 " +
                              std::to_string(cards.size()) +
                              "（release_list_ui.h kReleaseTaskPageSize=10）");
        if (!cards.empty()) {
            checks.expectNear("行卡高度", cards.front()->frame.height, 64.0f,
                              "release_list_ui.h kReleaseTaskRowH=64（池 55 设计 §3.2）");
            checks.expectTrue(
                cards.front()->frame.width <= 1040.0f + 0.5f,
                "行宽 ≤ 内容列 1040，实际 " +
                    std::to_string(cards.front()->frame.width) +
                    "（release_list_ui.cpp kContentMaxW=1040）");
            const auto* box = checks.need(ui, "release.tasks.cardbox", "外层列表白卡");
            if (box != nullptr) {
                checks.expectNear("列表白卡圆角", box->radius, 16.0f,
                                  "release_list_ui.cpp kListCardRadius=16（池 53 大卡特例档）");
                // 池133 防复发：白卡必须与行内容同顶且覆盖全部行。卡在 scrollView 的
                // content column 里，漏 .ignoreLayout() 会被流式排到 spacer 之后
                //（y=contentH），画面表现为「文本浮在上、白卡在下」。
                checks.expectNear(
                    "列表白卡顶缘", box->frame.y, cards.front()->frame.y,
                    "卡顶应与首行同顶（漏 ignoreLayout 会被流式排到内容下方）");
                checks.expectTrue(
                    box->frame.height + 0.5f >=
                        static_cast<float>(cards.size()) * 64.0f,
                    "白卡高应覆盖全部首屏行（" + std::to_string(cards.size()) +
                        "×64），实际 " + std::to_string(box->frame.height));
            }
        }
        // 113 批次B 内容层断言：列表序 = plan_date DESC，首屏前三行 = detail
        // （10-27，Draft）/ 09-20（Pending）/ 09-19（Running）。Draft 行无进度
        // 元素（release_list_ui.cpp drawTaskRow：Draft 不画 .prog*）；Pending /
        // Running 行有进度条且「下一步」指引文本非空。
        if (cards.size() >= 3) {
            const std::string draftCard = cards[0]->id;
            checks.expectTrue(ui->find(draftCard + ".prog.wrap") == nullptr,
                              "Draft 行不应渲染进度元素（设计 §5.2，无 .prog.wrap）");
            const std::string pendingCard = cards[1]->id;
            checks.expectTrue(ui->find(pendingCard + ".prog.wrap") != nullptr,
                              "Pending 行应渲染进度条（.prog.wrap）");
            const auto* pendingNext = ui->find(pendingCard + ".next");
            checks.expectTrue(pendingNext != nullptr && !pendingNext->text.empty(),
                              "Pending 行「下一步」指引文本应非空");
            const std::string runningCard = cards[2]->id;
            checks.expectTrue(ui->find(runningCard + ".prog.wrap") != nullptr,
                              "Running 行应渲染进度条（.prog.wrap）");
        }

        checks.expectTrue(ui->find("release.tasks.tab.0") != nullptr,
                          "模式筛选 Tab 行应存在（全部/发布中/准备中/已归档）");

        // 模拟滚到底：setScrollOffset 在帧末派发 onChange → requestLoad；帧泵
        // 推进 6 帧后追加一页（release_list_ui.cpp 帧泵语义不变）。
        if (ui != nullptr) {
            ui->setScrollOffset("release.tasks.scroll", 1000000.0f);
        }
        for (int frame = 0; frame < 10; ++frame) {
            composeList();
        }
        const auto afterScroll = cardRoots(*ui);
        checks.expectTrue(afterScroll.size() == std::size_t{20},
                          "触底追加后行数：期望 20，实际 " +
                              std::to_string(afterScroll.size()) +
                              "（release_list_ui.h kReleaseTaskPageSize=10，追加一页）");

        // 20 行状态的中途落图（--screenshot-seeded；未给开关时零开销）
        probe::captureSeedIfRequested(args, window, runtime, &checks.passed);
    }

    // ================= C2. 池55 QA 验收补图（Tab 切换 / 筛选空态） =================
    // QA 独立验收用：真实 click 走输入路径切 Tab（onClick → cache.tab → 内存
    // 筛选），captureFramePng 落图；不参与常规断言。
    if (!acceptTabPath.empty() || !acceptFilterEmptyPath.empty()) {
        // 种子中段一条（listTasks 为 plan_date DESC，下标 10 落在已加载 20 行内、
        // 非 detail 任务）推到 Pending，靠陈旧刷新分支（≥1000ms）重取卡片，
        // 「发布中」Tab 内存筛选即有行卡可画（不走触底补页，避开其空态分支）。
        auto allTasks = store.listTasks();
        if (allTasks.isOk() && !allTasks.value().empty()) {
            const auto pick =
                std::min<std::size_t>(10, allTasks.value().size() - 1);
            module::release::Task task = allTasks.value()[pick];
            task.status = module::release::TaskStatus::Pending;
            (void)store.updateTask(task);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        composeList();
        composeList();
        const auto clickTab = [&](int index) {
            const auto* tab =
                ui->find("release.tasks.tab." + std::to_string(index));
            if (tab == nullptr) return;
            probe::click(window, runtime,
                         static_cast<double>(tab->frame.x +
                                             tab->frame.width * 0.5f),
                         static_cast<double>(tab->frame.y +
                                             tab->frame.height * 0.5f));
        };
        const auto settle = [&] {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(200));  // Tab 下划线 150ms 过渡落位
            for (int i = 0; i < 8; ++i) {
                composeList();  // 走完 6 帧帧泵：Tab 切换后矮窗自动补页完成再落图
            }
        };
        if (!acceptTabPath.empty()) {
            clickTab(1);
            settle();
            if (!probe::captureFramePng(window, runtime, acceptTabPath)) {
                std::cerr << "release_probe: --accept-shot-tab 落图失败\n";
                checks.passed = false;
            }
        }
        if (!acceptFilterEmptyPath.empty()) {
            clickTab(3);  // 已归档组（Done/Voided）种子恒 0 → 筛选空态
            settle();
            if (!probe::captureFramePng(window, runtime,
                                        acceptFilterEmptyPath)) {
                std::cerr << "release_probe: --accept-shot-filter-empty 落图失败\n";
                checks.passed = false;
            }
            clickTab(0);  // 回「全部」，不污染后续场景
            composeList();
        }
    }

    // ================= C3. 新建任务弹窗落图（need 224 批2 材质证据） =================
    if (!newTaskShotPath.empty()) {
        composeList();
        composeList();
        const auto* newBtn = ui->find("release.tasks.new.bg");
        if (newBtn == nullptr) {
            checks.fail("C3：新建任务按钮缺失（release.tasks.new.bg）");
        } else {
            probe::click(window, runtime,
                         static_cast<double>(newBtn->frame.x +
                                             newBtn->frame.width * 0.5f),
                         static_cast<double>(newBtn->frame.y +
                                             newBtn->frame.height * 0.5f));
            for (int i = 0; i < 4; ++i) {
                composeList();  // 打开沿（wasOpen 翻转）+ 弹窗装填帧
            }
            checks.need(ui, "release.newtask.dialog.title", "新建上线任务弹窗已渲染（need156 标准壳）");
            if (!probe::captureFramePng(window, runtime, newTaskShotPath)) {
                std::cerr << "release_probe: --shot-newtask 落图失败\n";
                checks.passed = false;
            }
        }
    }

    // ================= D. 执行页 =================
    // need192 §1.2 焦点式双栏（release_detail_draw.cpp drawFocusLayout:1361-1600）：
    // 左索引列 264 定宽（kIndexW，:1365）+ 右焦点区（kFocusX = kIndexW + 16，:1369）。
    // 环境名 = 索引列分组头（32 高 / fontSizeSmall / 600 / textSecondary），
    // 服务明细唯一渲染在右焦点区（当前聚焦服务 1 张卡，id 与旧清单同构）。
    module::release::ReleaseDetailUiState detailState;
    detailState.taskId = std::to_string(detailTaskId);
    detailState.dirty = true;
    composePage("release.detail", w, h,
                [&](eui::Ui& frameUi, const eui::Screen& screen) {
                    module::release::composeReleaseDetailPage(
                        frameUi, screen, store, detailState);
                });
    {
        const std::string envBase =
            "release.detail.env." + std::to_string(detailEnvId);
        const std::string svcBase =
            envBase + ".svc." + std::to_string(detailServiceId);
        const std::string indexEnvLabel =
            "release.detail.index.env." + std::to_string(detailEnvId) + ".label";
        const std::string indexSvc =
            "release.detail.index.svc." + std::to_string(detailServiceId);

        // 双栏区容器：release.detail.focus（draw.cpp:1486；服务非空的常规路径）。
        // 注：分组区标题 drawSectionTitle("release.detail.focus.title") 只在
        // drawFocusLayout 的空态分支渲染（draw.cpp:1416，entries.empty()），
        // 常规路径无标题节点——旧 D 段的 release.detail.env.title（:1302，已不在
        // 详情页渲染链上）连同标题字号断言一并裁撤，改钉容器存在性。
        checks.need(ui, "release.detail.focus", "双栏区容器");

        // 左索引列底：定宽 264（need192 §1.2 >R6 门槛 200）。出处
        // release_detail_draw.cpp:1365 kIndexW=264、:1490 索引列底节点。
        const auto* indexBg = checks.need(ui, "release.detail.index.bg", "索引列底");
        if (indexBg != nullptr) {
            checks.expectNear("索引列宽", indexBg->frame.width, 264.0f,
                              "release_detail_draw.cpp:1365 kIndexW=264");
        }

        // 环境分组头 = 索引列分组头（旧「环境分组卡 + 19px 标题」已裁撤）：
        // 14px 档 fontSizeSmall(12)/600/textSecondary/垂直居中/行盒 kGroupH。
        // 出处 release_detail_draw.cpp:1368 kGroupH=32、:1384-1396 drawGroupHeader。
        const auto* envLabel = checks.need(ui, indexEnvLabel, "索引列环境分组头");
        if (envLabel != nullptr) {
            checks.expectText(envLabel, "环境分组头文案", "EM");
            checks.expectNear("环境分组头行盒高", envLabel->frame.height, 32.0f,
                              "release_detail_draw.cpp:1368 kGroupH=32");
            checks.expectNear("环境分组头字号", envLabel->fontSize,
                              core::theme::current().sizes.fontSizeSmall,
                              "release_detail_draw.cpp:1388 fontSizeSmall"
                              "（theme.cpp:62 = 12）");
            checks.expectTrue(envLabel->fontWeight == 600,
                              "环境分组头字重应为 600（release_detail_draw.cpp:1394）");
            checks.expectTrue(envLabel->verticalAlign == eui::VerticalAlign::Center,
                              "环境分组头应垂直居中（release_detail_draw.cpp:1393）");
            checks.expectColor("环境分组头字色", envLabel->textColor,
                               core::theme::current().colors.textSecondary,
                               "release_detail_draw.cpp:1395 textSecondary");
        }
        // 「移除环境」入口保留原节点 id（release_detail_draw.cpp:1404）。
        checks.need(ui, envBase + ".remove", "移除环境入口");

        // 服务索引项：选中态（焦点默认兜底首个 live 服务，:1456-1461）含
        // .sel/.sel.bar，双行内容 .name/.envname + 右锚 .count + .mini。
        // 出处 release_detail_draw.cpp:1505-1588（selected → :1510 .sel/:1516 .sel.bar）。
        const auto* indexHit = checks.need(ui, indexSvc + ".hit", "服务索引项命中区");
        checks.need(ui, indexSvc + ".sel", "服务索引项选中底");
        checks.need(ui, indexSvc + ".sel.bar", "服务索引项左竖条");
        checks.need(ui, indexSvc + ".count", "服务索引项进度计数");
        checks.need(ui, indexSvc + ".mini", "服务索引项 mini 进度");
        const auto* indexName = checks.need(ui, indexSvc + ".name", "服务索引项名称");
        checks.expectText(indexName, "服务索引项名称文案", "release-svc");
        checks.need(ui, indexSvc + ".envname", "服务索引项环境名");
        if (indexHit != nullptr) {
            checks.expectNear("服务索引项行高", indexHit->frame.height, 56.0f,
                              "release_detail_draw.cpp:1367 kEntryH=56（§3.1 双行条目）");
        }

        // 焦点区服务卡：服务卡仍在，id 与旧清单同构（release_detail_draw.cpp:1593）
        const auto* svcBg = checks.need(ui, svcBase + ".bg", "焦点区服务卡");
        // 双栏分界：焦点区左缘 = 索引列宽 + 列间距（原「环境分组头高度 56」
        // 断言在新布局下已无对应语义，改钉双栏几何）。
        // 出处 release_detail_draw.cpp:1366 kColumnGap=16、:1369 kFocusX。
        if (indexBg != nullptr && svcBg != nullptr) {
            checks.expectNear("焦点区左缘 = 索引列宽 + 16 列间距",
                              svcBg->frame.x - indexBg->frame.x, 280.0f,
                              "release_detail_draw.cpp:1365/1366/1369 "
                              "kFocusX = kIndexW(264) + kColumnGap(16)");
        }

        const std::string mergeRow = svcBase + ".step.merge";
        const auto* stepLabel =
            checks.need(ui, mergeRow + ".label", "步骤行（合并分支）");
        if (stepLabel != nullptr) {
            checks.expectNear("步骤行高", stepLabel->frame.height, 44.0f,
                              "release_detail_draw.cpp kStepRowH=44（池 53）");
        }
        const auto* stepDot =
            checks.need(ui, mergeRow + ".status.dot", "步骤勾选框");
        if (stepDot != nullptr) {
            checks.expectNear("步骤勾选框宽", stepDot->frame.width, 18.0f,
                              "release_detail_ui.cpp kStepDot=18");
            checks.expectNear("步骤勾选框高", stepDot->frame.height, 18.0f,
                              "release_detail_ui.cpp kStepDot=18");
        }

        const auto* statusBg =
            checks.need(ui, "release.detail.status.bg", "任务状态 chip 底");
        const auto* statusText =
            checks.need(ui, "release.detail.status.text", "任务状态 chip 文字");
        if (statusBg != nullptr) {
            checks.expectNear("状态 chip 高", statusBg->frame.height, 22.0f,
                              "release_detail_ui.cpp kChipH=22");
            checks.expectNear("状态 chip 圆角", statusBg->radius, 999.0f,
                              "theme radiusPill=999（drawChip .radius）");
        }
        if (statusText != nullptr) {
            checks.expectNear("状态 chip 字号", statusText->fontSize, 11.0f,
                              "release_detail_ui.cpp fontSizeSmall−1=11（drawChip）");
        }

        // 池 53 F-11：模式徽标高 22（Draft=准备中，中性灰底、无绿染）。
        const auto* badgeBg =
            checks.need(ui, "release.detail.modebadge.edit", "模式徽标底");
        if (badgeBg != nullptr) {
            checks.expectNear("模式徽标高", badgeBg->frame.height, 22.0f,
                              "release_detail_draw.cpp drawModeBadge kChipH=22");
        }
        // 池 57（edit 侧）：编辑态徽标必须中性（hover 底 / textSecondary 字），
        // 与「发布中=页头唯一绿染」互为对照。
        if (badgeBg != nullptr) {
            const auto& tokens = core::theme::current();
            checks.expectColor(
                "编辑态徽标底色", badgeBg->color, tokens.colors.hover,
                "release_detail_draw.cpp drawModeBadge（非发布态 bg=hover）");
        }
        const auto* badgeText =
            checks.need(ui, "release.detail.modebadge.edit.text", "模式徽标文字");
        if (badgeText != nullptr) {
            checks.expectColor(
                "编辑态徽标字色", badgeText->textColor,
                core::theme::current().colors.textSecondary,
                "release_detail_draw.cpp drawModeBadge（非发布态 fg=textSecondary）");
        }
    }

    // 编辑态详情页落图（need 224 批2：环境卡/前置卡材质对照）
    if (!detailShotPath.empty() &&
        !probe::captureFramePng(window, runtime, detailShotPath)) {
        std::cerr << "release_probe: --shot-detail 落图失败\n";
        checks.passed = false;
    }

    // ================= D2. 发布态分支入口门禁（池 54 F-54-1 / F-03） =================
    // 提交转 Pending：分支输入框必须消失（发布态只读铁律），页面切流程轨道；
    // 落一张发布态基准图后回到 Draft，保证退出落图仍是编辑态（与池 53 基准同态）。
    {
        auto detailTask = store.listTasks();
        if (detailTask.isFail()) {
            std::cerr << "release_probe: 读取任务失败\n";
            checks.passed = false;
        } else {
            for (auto& task : detailTask.value()) {
                if (task.id != detailTaskId) continue;
                task.status = module::release::TaskStatus::Pending;
                if (store.updateTask(task).isFail()) {
                    std::cerr << "release_probe: 切发布态失败\n";
                    checks.passed = false;
                }
                break;
            }
        }
        detailState.dirty = true;
        composePage("release.detail.publish", w, h,
                    [&](eui::Ui& frameUi, const eui::Screen& screen) {
                        module::release::composeReleaseDetailPage(
                            frameUi, screen, store, detailState);
                    });
        // 发布态流程轨道落图（need 224 批2：裸行分隔线对照）
        if (!trackShotPath.empty() &&
            !probe::captureFramePng(window, runtime, trackShotPath)) {
            std::cerr << "release_probe: --shot-track 落图失败\n";
            checks.passed = false;
        }
        const std::string svcBase =
            "release.detail.env." + std::to_string(detailEnvId) +
            ".svc." + std::to_string(detailServiceId);
        if (ui->find(svcBase + ".branch.input.hit") != nullptr) {
            std::cerr << "release_probe: 发布态仍渲染分支输入框（F-03 违规）\n";
            checks.passed = false;
        }
        checks.need(ui, "release.detail.track", "发布态流程轨道");
        // 池 57（publish 侧）：发布中徽标 = 页头唯一绿染（primarySoftBg 底 /
        // primaryText 字）；状态 chip 中性（113-5 主色让给徽标）。
        {
            const auto& tokens = core::theme::current();
            const auto* pubBadgeBg = checks.need(ui, "release.detail.modebadge.publish",
                                                 "发布中模式徽标底");
            const auto* pubBadgeText =
                checks.need(ui, "release.detail.modebadge.publish.text",
                            "发布中模式徽标文字");
            checks.expectText(pubBadgeText, "发布中徽标文案", "发布中");
            if (pubBadgeBg != nullptr) {
                checks.expectNear("发布中徽标高", pubBadgeBg->frame.height, 22.0f,
                                  "release_detail_draw.cpp drawModeBadge kChipH=22");
                checks.expectColor(
                    "发布中徽标底色", pubBadgeBg->color, tokens.colors.primarySoftBg,
                    "release_detail_draw.cpp drawModeBadge（发布态 bg=primarySoftBg）");
            }
            if (pubBadgeText != nullptr) {
                checks.expectColor(
                    "发布中徽标字色", pubBadgeText->textColor,
                    tokens.colors.primaryText,
                    "release_detail_draw.cpp drawModeBadge（发布态 fg=primaryText）");
            }
            const auto* statusChip = ui->find("release.detail.status.bg");
            checks.expectTrue(
                statusChip != nullptr &&
                    colorNear(statusChip->color, tokens.colors.hover),
                "发布态状态 chip 底应中性 hover（页头绿染唯一归模式徽标，113-5）");
        }
        if (!args.screenshotPublishPath.empty()) {
            if (!probe::captureFramePng(window, runtime,
                                        args.screenshotPublishPath)) {
                std::cerr << "release_probe: --screenshot-publish 落图失败: "
                          << args.screenshotPublishPath << "\n";
                checks.passed = false;
            }
        }
        // 回 Draft：退出落图保持编辑态
        auto tasksAgain = store.listTasks();
        if (tasksAgain.isOk()) {
            for (auto& task : tasksAgain.value()) {
                if (task.id != detailTaskId) continue;
                task.status = module::release::TaskStatus::Draft;
                (void)store.updateTask(task);
                break;
            }
        }
        detailState.dirty = true;
        composePage("release.detail", w, h,
                    [&](eui::Ui& frameUi, const eui::Screen& screen) {
                        module::release::composeReleaseDetailPage(
                            frameUi, screen, store, detailState);
                    });
    }

    // ================= D3. 池116②：执行页挤压态（2 环境 × 3 服务） =================
    // need192 §1.2 焦点式双栏后语义重写：不再有横向排列的环境分组卡，也不再
    // 全量渲染 6 张服务卡——左索引列纵向堆叠「环境分组头 + 服务精简条目」，
    // 右焦点区只画当前聚焦的 1 张服务卡。
    // 几何契约：索引列分组头 / 条目纵向不重叠且不越出索引列；焦点卡不越出焦点区；
    // 步骤格不越出焦点服务卡。出处 release_detail_draw.cpp drawFocusLayout:1361-1600。
    // --screenshot-crowded <png> 落挤压态图（未传 = 只断言不落图）。
    if (crowdedTaskId != 0) {
        detailState.taskId = std::to_string(crowdedTaskId);
        detailState.dirty = true;
        composePage("release.detail.crowded", w, h,
                    [&](eui::Ui& frameUi, const eui::Screen& screen) {
                        module::release::composeReleaseDetailPage(
                            frameUi, screen, store, detailState);
                    });
        // composeModuleFrame 给元素 id 加页面命名空间前缀（<pageId>.<id>），
        // ui->find 内部 resolveId 解析，裸前缀收集必须先剥前缀再匹配。
        const std::string pageNs = "release.detail.crowded.";
        const auto stripNs = [&pageNs](const std::string& raw) {
            return raw.compare(0, pageNs.size(), pageNs) == 0
                       ? raw.substr(pageNs.size())
                       : raw;
        };
        const auto allDigits = [](const std::string& s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
                return c >= '0' && c <= '9';
            });
        };
        // 环境分组头 = release.detail.index.env.<digits>.label（draw.cpp:1384）。
        const std::string idxEnvPrefix = "release.detail.index.env.";
        const auto isIndexGroupHeader = [&](const std::string& raw) {
            const std::string& id = stripNs(raw);
            static const std::string suf = ".label";
            if (id.compare(0, idxEnvPrefix.size(), idxEnvPrefix) != 0 ||
                id.rfind(suf) != id.size() - suf.size()) {
                return false;
            }
            return allDigits(
                id.substr(idxEnvPrefix.size(),
                          id.size() - idxEnvPrefix.size() - suf.size()));
        };
        // 服务索引项 = release.detail.index.svc.<digits>.hit（draw.cpp:1507/1522）。
        const std::string idxSvcPrefix = "release.detail.index.svc.";
        const auto isIndexSvcHit = [&](const std::string& raw) {
            const std::string& id = stripNs(raw);
            static const std::string suf = ".hit";
            if (id.compare(0, idxSvcPrefix.size(), idxSvcPrefix) != 0 ||
                id.rfind(suf) != id.size() - suf.size()) {
                return false;
            }
            return allDigits(
                id.substr(idxSvcPrefix.size(),
                          id.size() - idxSvcPrefix.size() - suf.size()));
        };
        // 焦点区服务卡 = release.detail.env.<digits>.svc.<digits>.bg（draw.cpp:1593）。
        const std::string envPrefix = "release.detail.env.";
        const auto isSvcBg = [&](const std::string& raw) {
            const std::string& id = stripNs(raw);
            if (id.compare(0, envPrefix.size(), envPrefix) != 0 ||
                id.rfind(".bg") != id.size() - 3 ||
                id.find(".svc.") == std::string::npos) {
                return false;
            }
            const std::string mid =
                id.substr(envPrefix.size(), id.size() - envPrefix.size() - 3);
            const std::size_t svcSep = mid.find(".svc.");
            return svcSep != std::string::npos &&
                   allDigits(mid.substr(0, svcSep)) &&
                   allDigits(mid.substr(svcSep + 5));
        };
        // 步骤行名 = <svcBase>.step.<rowId>.label（release_detail_draw.cpp:1002/732），
        // <rowId> 是 merge / deploy / verify 或 config.<n>（:929-983）；带其它
        // 额外段的是行尾控件（如 .skip.label，:786 跳过复选框），必须排除。
        const auto isStepLabel = [&](const std::string& raw) {
            const std::string& id = stripNs(raw);
            static const std::string stepMark = ".step.";
            static const std::string suf = ".label";
            static const std::string cfgMark = "config.";
            if (id.compare(0, envPrefix.size(), envPrefix) != 0) return false;
            const std::size_t mark = id.find(stepMark);
            if (mark == std::string::npos) return false;
            if (id.rfind(suf) != id.size() - suf.size()) return false;
            const std::size_t rowStart = mark + stepMark.size();
            const std::string rowId = id.substr(rowStart, id.size() - suf.size() - rowStart);
            if (rowId.compare(0, cfgMark.size(), cfgMark) == 0) {
                return allDigits(rowId.substr(cfgMark.size()));
            }
            return !rowId.empty() && rowId.find('.') == std::string::npos;
        };
        const auto groupHeaders = collectElements(*ui, isIndexGroupHeader);
        const auto indexEntries = collectElements(*ui, isIndexSvcHit);
        const auto svcBgs = collectElements(*ui, isSvcBg);
        const auto stepLabels = collectElements(*ui, isStepLabel);
        const auto* indexBg = ui->find("release.detail.index.bg");

        // 索引列分组头数 == 环境数（2：EM / UAT），纵向堆叠取代横向分组卡。
        checks.expectTrue(
            groupHeaders.size() == detailState.envs.size(),
            "D3：索引列环境分组头数应 == 环境数（" +
                std::to_string(detailState.envs.size()) + "），实际 " +
                std::to_string(groupHeaders.size()) + "（state.envs=" +
                std::to_string(detailState.envs.size()) + "，loadError=" +
                (detailState.loadError.empty() ? "<无>" : detailState.loadError) +
                "）");
        // 索引服务项数 == 全部 live 服务数（2 环境 × 3 服务 = 6），一级概览不裁剪。
        checks.expectTrue(
            indexEntries.size() == std::size_t{6},
            "D3：服务索引项应 == 6（2 环境 × 3 服务），实际 " +
                std::to_string(indexEntries.size()));
        // 焦点区只渲染当前聚焦的 1 张服务卡（draw.cpp:1593 传 focusEnv/focusSvc）。
        checks.expectTrue(svcBgs.size() == std::size_t{1},
                          "D3：焦点区服务卡应 == 1（仅当前聚焦服务），实际 " +
                              std::to_string(svcBgs.size()));

        // 焦点服务卡步骤行数 = 3（合并/部署/校验）+ 配置行数；后端服务的配置项由
        // orderedConfigs 补齐至 5 类（release_detail_logic.cpp:283-292），行装配见
        // release_detail_draw.cpp:926-984。旧断言「≥12 = 6 服务 × 2 步」已失效。
        std::size_t focusConfigs = 0;
        for (const auto& cfg : detailState.configs) {
            if (cfg.taskServiceId == detailState.focusedServiceId) ++focusConfigs;
        }
        const std::size_t focusRows = 3 + (focusConfigs < 5 ? std::size_t{5}
                                                            : focusConfigs);
        checks.expectTrue(stepLabels.size() == focusRows,
                          "D3：焦点服务步骤行应 == " + std::to_string(focusRows) +
                              "（3 + 配置 5），实际 " +
                              std::to_string(stepLabels.size()));

        // 分组头纵向不重叠：按 y 排序后相邻首尾相接（后头顶 >= 前头底，0.5 容差）。
        // kGroupH=32（draw.cpp:1368），组头在索引列内逐 32 下推（:1502-1503）。
        std::vector<const core::dsl::Element*> sortedGroups = groupHeaders;
        std::sort(sortedGroups.begin(), sortedGroups.end(),
                  [](const core::dsl::Element* a, const core::dsl::Element* b) {
                      return a->frame.y < b->frame.y;
                  });
        for (std::size_t i = 1; i < sortedGroups.size(); ++i) {
            const float prevBottom =
                sortedGroups[i - 1]->frame.y + sortedGroups[i - 1]->frame.height;
            checks.expectTrue(
                sortedGroups[i]->frame.y >= prevBottom - 0.5f,
                "D3：索引列分组头纵向重叠（" + sortedGroups[i - 1]->id + " 底 " +
                    std::to_string(prevBottom) + " vs " + sortedGroups[i]->id +
                    " 顶 " + std::to_string(sortedGroups[i]->frame.y) + "）");
        }
        for (const auto* header : groupHeaders) {
            checks.expectNear("D3：索引列分组头行盒高", header->frame.height, 32.0f,
                              "release_detail_draw.cpp:1368 kGroupH=32");
        }

        // 索引服务项纵向不重叠（kEntryH=56 + 6 间距，draw.cpp:1367/1588），
        // 且不越出索引列左右边界（.hit x=4 / w=kIndexW-8，draw.cpp:1522-1524）。
        std::vector<const core::dsl::Element*> sortedEntries = indexEntries;
        std::sort(sortedEntries.begin(), sortedEntries.end(),
                  [](const core::dsl::Element* a, const core::dsl::Element* b) {
                      return a->frame.y < b->frame.y;
                  });
        for (std::size_t i = 1; i < sortedEntries.size(); ++i) {
            const float prevBottom =
                sortedEntries[i - 1]->frame.y + sortedEntries[i - 1]->frame.height;
            checks.expectTrue(
                sortedEntries[i]->frame.y >= prevBottom - 0.5f,
                "D3：索引服务项纵向重叠（" + sortedEntries[i - 1]->id + " 底 " +
                    std::to_string(prevBottom) + " vs " + sortedEntries[i]->id +
                    " 顶 " + std::to_string(sortedEntries[i]->frame.y) + "）");
        }
        const auto withinX = [](const core::dsl::Element* node,
                                const core::dsl::Element* container) {
            return node->frame.x >= container->frame.x - 0.5f &&
                   node->frame.x + node->frame.width <=
                       container->frame.x + container->frame.width + 0.5f;
        };
        checks.expectTrue(indexBg != nullptr, "D3：索引列底缺失（release.detail.index.bg）");
        if (indexBg != nullptr) {
            checks.expectNear("D3：索引列宽", indexBg->frame.width, 264.0f,
                              "release_detail_draw.cpp:1365 kIndexW=264");
            for (const auto* entry : indexEntries) {
                checks.expectTrue(withinX(entry, indexBg),
                                  "D3：服务索引项横向越出索引列：" +
                                      stripNs(entry->id));
            }
            for (const auto* header : groupHeaders) {
                checks.expectTrue(withinX(header, indexBg),
                                  "D3：分组头横向越出索引列：" +
                                      stripNs(header->id));
            }
        }

        // 焦点卡不越出焦点区（焦点区 = 索引列右侧 kColumnGap 起的剩余宽度，
        // draw.cpp:1595 width - kFocusX）：卡左缘 >= 索引列右缘 + 16，右缘 <= 焦点区右缘。
        const auto* focusStack = ui->find("release.detail.focus");
        for (const auto* svc : svcBgs) {
            if (indexBg != nullptr) {
                checks.expectNear(
                    "D3：焦点卡左缘 = 索引列宽 + 列间距",
                    svc->frame.x - indexBg->frame.x, 280.0f,
                    "release_detail_draw.cpp:1365/1366/1369 "
                    "kFocusX = kIndexW(264) + kColumnGap(16)");
            }
            if (focusStack != nullptr) {
                const float focusRight = focusStack->frame.x + focusStack->frame.width;
                checks.expectTrue(
                    svc->frame.x + svc->frame.width <= focusRight + 0.5f,
                    "D3：焦点卡横向越出焦点区：" + stripNs(svc->id));
            }
        }
        // 步骤格不越出所属（焦点区唯一）服务卡。
        for (const auto* step : stepLabels) {
            const std::string stepId = stripNs(step->id);
            const std::string svcId = stepId.substr(0, stepId.find(".step."));
            const auto* svcBg = ui->find(svcId + ".bg");
            checks.expectTrue(svcBg != nullptr && withinX(step, svcBg),
                              "D3：步骤格横向越出所属服务卡：" + stepId);
        }

        if (!crowdedShotPath.empty()) {
            if (!probe::captureFramePng(window, runtime, crowdedShotPath)) {
                std::cerr << "release_probe: --screenshot-crowded 落图失败\n";
                checks.passed = false;
            }
        }
        // 还原常规执行页（收尾 --screenshot 出口仍拍 1 环境×1 服务的基准态）
        detailState.taskId = std::to_string(detailTaskId);
        detailState.dirty = true;
        composePage("release.detail", w, h,
                    [&](eui::Ui& frameUi, const eui::Screen& screen) {
                        module::release::composeReleaseDetailPage(
                            frameUi, screen, store, detailState);
                    });
    }

    // ================= E. 提醒（到期聚合 → 提醒窗内容，数据层断言） =================
    // 提醒窗真开仍不可达（独立 OS 窗口 + app 主循环帧驱动，文末原注释保留）——
    // 按可行范围补数据层契约：dueItems 聚合、一窗档位取最高优先级、「去处理」
    // 跳转负载（taskId + scope，契约见 release_reminder.h ReleaseReminderAction）。
    // 放在 F2 清库之前；窗口渲染与 onHandle→Router 接线仍无自动化断言。
    {
        using namespace module::release;
        const auto allTasks = store.listTasks();
        if (allTasks.isFail()) {
            checks.fail("E 组：读取任务失败：" + allTasks.errorMessage());
        } else {
            const Task* detailTask = nullptr;
            for (const auto& task : allTasks.value()) {
                if (task.id == detailTaskId) detailTask = &task;
            }
            if (detailTask == nullptr) {
                checks.fail("E 组：种子 detail 任务缺失");
            } else {
                const auto envsRes = store.listTaskEnvs(detailTask->id);
                const auto stepsRes = store.listTaskStepsByTask(detailTask->id);
                checks.expectTrue(envsRes.isOk() && stepsRes.isOk(),
                                  "E 组：读取环境/步骤失败");
                std::vector<TaskService> services;
                std::vector<TaskConfig> configs;
                if (envsRes.isOk()) {
                    for (const auto& env : envsRes.value()) {
                        const auto svcRes = store.listTaskServices(env.id);
                        if (svcRes.isFail()) continue;
                        for (const auto& svc : svcRes.value()) {
                            const auto cfgRes = store.listTaskConfigs(svc.id);
                            if (cfgRes.isFail()) continue;
                            services.push_back(svc);
                            for (const auto& cfg : cfgRes.value()) configs.push_back(cfg);
                        }
                    }
                }
                const std::vector<TaskEnv> envs =
                    envsRes.isOk() ? envsRes.value() : std::vector<TaskEnv>{};
                const std::vector<TaskStep> steps =
                    stepsRes.isOk() ? stepsRes.value() : std::vector<TaskStep>{};
                // 时间换算走 planDateElapsedMinutes 唯一口径；种子日期均已过基准
                //（真实时钟晚于全部种子 planDate），条目存在性断言对时钟漂移稳定。
                std::int64_t minute = 0;
                int nowMinute = 0;
                if (planDateElapsedMinutes(detailTask->planDate, std::time(nullptr),
                                           minute)) {
                    nowMinute = static_cast<int>(minute);
                }
                const std::vector<DueItem> items =
                    dueItems(*detailTask, envs, services, configs, steps, nowMinute);
                // 聚合正确（种子：1 环境 EM × 1 后端服务，配置内容全空 → 不欠
                // nacos/db 工单）：橙 2（提测/CAB）+ 黄 1（公告未发）+ 红 2
                // （EM 部署未完成 + 任务未提交）。
                int orange = 0;
                int yellow = 0;
                int red = 0;
                int taskScope = 0;
                int envScope = 0;
                for (const DueItem& item : items) {
                    if (item.level == RemindLevel::Orange) ++orange;
                    if (item.level == RemindLevel::Yellow) ++yellow;
                    if (item.level == RemindLevel::Red) ++red;
                    if (item.scope == "task") ++taskScope;
                    if (item.scope == "EM") ++envScope;
                    checks.expectTrue(item.taskId == detailTaskId,
                                      "E 组跳转负载：条目 taskId 应为所属任务 id（实际 " +
                                          std::to_string(item.taskId) + "）");
                }
                checks.expectTrue(orange == 2 && yellow == 1 && red == 2,
                                  "E 组聚合：期望 橙2/黄1/红2，实际 橙" +
                                      std::to_string(orange) + " 黄" +
                                      std::to_string(yellow) + " 红" +
                                      std::to_string(red));
                checks.expectTrue(taskScope == 4 && envScope == 1,
                                  "E 组跳转负载：期望任务级 4 条 scope=task、环境级 1 条 "
                                  "scope=EM，实际 task=" + std::to_string(taskScope) +
                                      " env=" + std::to_string(envScope));
                // 窗口级色 = 最高优先级（红 > 橙 > 黄，release_logic.h highestLevel）。
                checks.expectTrue(highestLevel(items) == RemindLevel::Red,
                                  "E 组：一窗档位应取最高优先级（红）");
                std::vector<DueItem> probe;
                probe.push_back({RemindLevel::Yellow, 1, "task", "a", "d"});
                checks.expectTrue(highestLevel(probe) == RemindLevel::Yellow,
                                  "E 组：纯黄清单档位应为黄");
                probe.push_back({RemindLevel::Orange, 1, "task", "b", "d"});
                checks.expectTrue(highestLevel(probe) == RemindLevel::Orange,
                                  "E 组：黄+橙清单档位应为橙（优先序）");
            }
        }
    }

    // ================= F2. 池55 空态落图（QA 验收） =================
    // --accept-shot-empty <png>：清空任务表 → 陈旧刷新重锚 → 「全部」Tab 全空态。
    // 放在 D 段之后：清库会破坏 detail 场景，必须最后执行。
    if (!acceptEmptyPath.empty()) {
        auto del = testEnv.db->db().exec(
            "DELETE FROM release_task_step; DELETE FROM release_task_config;"
            " DELETE FROM release_task_service; DELETE FROM release_task_env;"
            " DELETE FROM release_task;");
        if (del.isFail()) {
            std::cerr << "release_probe: --accept-shot-empty 清库失败: "
                      << del.errorMessage() << "\n";
            checks.passed = false;
        } else {
            const auto composeListEnd = [&]() {
                composePage("release.tasks", 1200.0f, 600.0f,
                            [&](eui::Ui& frameUi, const eui::Screen& screen) {
                                tasksModule->compose(frameUi, screen);
                            });
            };
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            composeListEnd();
            composeListEnd();
            composeListEnd();
            if (!probe::captureFramePng(window, runtime, acceptEmptyPath)) {
                std::cerr << "release_probe: --accept-shot-empty 落图失败\n";
                checks.passed = false;
            }
        }
    }

    // 退出前落一张「当前状态图」（--screenshot <路径>；未传 = 零开销）
    probe::captureIfRequested(args, window, runtime, &checks.passed);

    // ================= E. 提醒全屏窗（未覆盖）=================
    // 提醒窗由 release_reminder.cpp 经 app::openWindow(...) 另开一个**独立 OS
    // 窗口**，其 compose 函数 composeReminderWindow 位于该 TU 的匿名命名空间
    // （不导出），且新窗的帧循环由 EUI-NEO app 主循环驱动。本 probe 持有自己的
    // 单窗口 + 单 Runtime 手动帧循环（harness 契约），既拿不到第二个窗口的
    // Runtime，也无法在不启动 app::run 的前提下驱动该窗渲染——强行断言只能
    // 退化成「读代码常量」，那不是真窗口证据。故本块**不覆盖**提醒窗视觉，
    // 留给后续能跑 app 主循环的 live 探针；提醒窗的调度 / 抑制 / 渲染行为
    // 目前没有任何自动化断言（无头测试与探针均不覆盖该独立窗口）。

    configModule->onStop();
    tasksModule->onStop();
    ::core::registerAppRouter(nullptr);
    runtime.shutdown(false);
    core::releaseInputQueue(window);
    core::window::destroyWindow(window);
    probe::terminateWindowBackend();
    if (!checks.passed) {
        std::cerr << "FAIL\n";
    } else {
        std::cout << "release_probe: PASS（" << args.windowW << "x"
                  << args.windowH
                  << "，断言分组 A/B/C/D 全过；E=到期聚合数据层，全屏窗渲染不可达见注释）\n";
    }
    return checks.passed ? 0 : 1;
}