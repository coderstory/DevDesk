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
// 数据：用 test_support::TestEnv 建已 migrate 的临时库并注册为进程级
// appDatabase——不读写用户数据，也不依赖 DEVDESK_DATA_ROOT。
//
// 覆盖分组：
//   A 侧边栏分组树（8 个既有条目 + 「上线」父项 + 2 子项、展开/收起）
//   B 基础配置页（padding / 标题字号 / 类型 chip / 后端 5 tag / 前端无配置项）
//   C 任务列表瀑布流（首屏 10 → 触底 20、卡宽、列/纵间距、圆角）
//   D 执行页（环境分组头、步骤行、勾选框、状态 chip）
//   E 提醒全屏窗——**未覆盖**，原因见 main() 末尾的说明注释。
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
#include "module_release/release_store.h"
#include "module_release/release_types.h"

#include <eui/detail/dsl_app_impl.h>  // app::requestShow/requestUpdate 内联实现

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

// 种子数据：2 个基础配置服务（后端 + PC 前端）→ 20 条任务（列表分页口径）→
// 1 条执行页任务（含 1 环境 × 1 后端服务，自动补齐 5 配置项步骤）。
bool seedReleaseData(module::release::ReleaseStore& store,
                     std::int64_t* backendServiceId,
                     std::int64_t* frontendServiceId,
                     std::int64_t* detailTaskId) {
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

    for (int i = 1; i <= 20; ++i) {
        char planDate[16] = {};
        std::snprintf(planDate, sizeof(planDate), "2026-09-%02d", i);
        if (addTask(planDate) == 0) return false;
    }
    *detailTaskId = addTask("2026-10-27");  // 常规发布日（周二）
    return *detailTaskId != 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = probe::parseProbeArgs(argc, argv);
    if (!probe::initializeWindowBackend()) return 1;
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
    if (!seedReleaseData(store, &backendServiceId, &frontendServiceId,
                         &detailTaskId)) {
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

    const float w = static_cast<float>(args.windowW);
    const float h = static_cast<float>(args.windowH);
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

    // ================= C. 任务列表瀑布流 =================
    // 首屏 10 张卡（kReleaseTaskPageSize），触底追加一页 → 20；卡宽 ≥ 320、
    // 列间距 16、列内垂直间距 16、卡片圆角 12。
    // 分页断言口径固定 1200×600（3 列）：10 张卡内容高 640 > 视口 512，把
    // 「首屏不足一屏自动续载」的矮窗分支挡在门外，让「首屏 10」是分页契约而
    // 不是窗口尺寸的巧合（4 列时 10 卡不足一屏会自动补页，混淆 kPageSize 契约）。
    const float listW = 1200.0f;
    const float listH = 600.0f;
    const auto composeList = [&]() {
        composePage("release.tasks", listW, listH,
                    [&](eui::Ui& frameUi, const eui::Screen& screen) {
                        tasksModule->compose(frameUi, screen);
                    });
    };
    composeList();
    {
        auto cards = cardRoots(*ui);
        checks.expectTrue(cards.size() == std::size_t{10},
                          "首屏卡片数：期望 10，实际 " +
                              std::to_string(cards.size()) +
                              "（release_list_ui.h kReleaseTaskPageSize=10）");
        if (!cards.empty()) {
            const auto* firstBg =
                checks.need(ui, cards.front()->id + ".bg", "任务卡背景");
            if (firstBg != nullptr) {
                checks.expectNear("任务卡圆角", firstBg->radius, 12.0f,
                                  "release_list_ui.cpp kCardRadius=12（视觉规范）");
            }
            checks.expectTrue(
                cards.front()->frame.width >= 320.0f - 0.5f,
                "任务卡最小宽：期望 ≥320，实际 " +
                    std::to_string(cards.front()->frame.width) +
                    "（release_list_ui.h kReleaseTaskCardMinWidth=320）");
        }
        // 列间距：瀑布流第 0/1 列的 x 差 = 卡宽 + 列间距
        if (cards.size() >= std::size_t{3}) {
            checks.expectNear("瀑布流列间距",
                              cards[1]->frame.x - cards[0]->frame.x -
                                  cards[0]->frame.width,
                              16.0f,
                              "release_list_ui.h kReleaseTaskCardGap=16");
        }
        // 列内垂直间距：第 4 张卡回到第 0 列（等高卡片）
        if (cards.size() >= std::size_t{4}) {
            checks.expectNear("瀑布流纵向间距",
                              cards[3]->frame.y -
                                  (cards[0]->frame.y + cards[0]->frame.height),
                              16.0f,
                              "release_list_ui.h kReleaseTaskCardGap=16");
        }

        // 模拟滚到底：setScrollOffset 在帧末派发 onChange → requestLoad；帧泵
        // 推进 6 帧后追加一页（release_list_ui.cpp advanceLoad）。
        if (ui != nullptr) {
            ui->setScrollOffset("release.tasks.scroll", 1000000.0f);
        }
        for (int frame = 0; frame < 10; ++frame) {
            composeList();
        }
        const auto afterScroll = cardRoots(*ui);
        checks.expectTrue(afterScroll.size() == std::size_t{20},
                          "触底追加后卡片数：期望 20，实际 " +
                              std::to_string(afterScroll.size()) +
                              "（release_list_ui.h kReleaseTaskPageSize=10，追加一页）");

        // 20 卡状态的中途落图（--screenshot-seeded；未给开关时零开销）
        probe::captureSeedIfRequested(args, window, runtime, &checks.passed);
    }

    // ================= D. 执行页 =================
    // 环境分组头高 56、标题 18；步骤行高 44（池 53 设计档）、勾选框 18；状态 chip 22/11/999。
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
        const auto* envBg = checks.need(ui, envBase + ".bg", "环境分组卡");
        const auto* envName = checks.need(ui, envBase + ".name", "环境分组标题");
        const auto* svcBg = checks.need(ui, svcBase + ".bg", "服务卡");
        checks.expectText(envName, "环境名文案", "EM");
        if (envName != nullptr) {
            checks.expectNear("环境分组标题字号", envName->fontSize, 18.0f,
                              "theme fontSizeLarge=18（release_detail_ui.cpp drawEnvCard .name）");
        }
        if (envBg != nullptr && svcBg != nullptr) {
            checks.expectNear("环境分组头高度",
                              svcBg->frame.y - envBg->frame.y, 56.0f,
                              "release_detail_ui.cpp kEnvHeaderH=56");
        }

        const auto* sectionTitle =
            checks.need(ui, "release.detail.env.title", "环境分组区标题");
        if (sectionTitle != nullptr) {
            checks.expectNear("环境分组区标题字号", sectionTitle->fontSize, 18.0f,
                              "release_detail_ui.cpp drawSectionTitle fontSizeLarge=18");
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
                  << args.windowH << "，断言分组 A/B/C/D 全过，E 未覆盖见注释）\n";
    }
    return checks.passed ? 0 : 1;
}