// 上线模块骨架冒烟：两个可导航页的注册元数据 + 空态渲染，证明
// 登记表 → 工厂 → Router 页面 → 模块 compose 的链路可达。
// 分组父项 create_release_group_module 按契约返回 nullptr（不可导航、不注册）。
#include "doctest.h"
#include "test_support.h"

#include "app_assembly.h"
#include "app_registry.h"
#include "app_sidebar.h"
#include "core/module_interface.h"
#include "core/router.h"
#include "settings_config.h"

#include <eui/dsl.h>

#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace {

// 侧边栏分组树用例的桩模块：只需要 pageId/短名，不触碰数据层（工厂实例由
// 首个用例单独覆盖）。compose 输出一个无关紧要的占位节点。
class SidebarStubModule final : public ::core::Module {
public:
    SidebarStubModule(::core::PageId page, std::string name)
        : page_(page), name_(std::move(name)) {}
    ::core::PageId pageId() const override { return page_; }
    std::string displayName() const override { return name_; }
    std::string iconGlyph() const override { return eui::utf8(0xF015); }
    void compose(eui::Ui& ui, const eui::Screen&) override {
        ui.stack(std::string("release.stub.") +
                 ::core::pageIdToString(page_))
            .size(1.0f, 1.0f)
            .build();
    }

private:
    ::core::PageId page_;
    std::string name_;
};

// 登记表按 PageId 查行（找不到返回 nullptr）
const ::app::registry::ModuleEntry* registryRow(::core::PageId page) {
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page == page) return &e;
    }
    return nullptr;
}

bool floatEq(float a, float b) { return std::fabs(a - b) < 0.01f; }

// 一帧侧边栏（compose 只建树；本文件的断言全读 .x/.y/.width 字面几何，不读 frame）
void composeFrame(eui::Ui& ui, const eui::Screen& screen) {
    ui.begin("release_sidebar_frame");
    app::compose(ui, screen);
    ui.end();
}

// 侧边栏用例的进程级状态回厂（不给后续用例留折叠/分组收起态）
void resetSidebarTestState() {
    app::resetSidebarGlobals();
    app::resetFluidBackgroundForTests();
    app::setCustomTitlebarMode(std::nullopt);
}

} // namespace

REGISTER_TEST(release_module_registration_and_empty_state,
              "release: 上线模块两页注册元数据（共用 release 开关）+ 空态「暂无数据」渲染", {
    // 分组父项：不可导航、无实例
    DOCTEST_CHECK(create_release_group_module() == nullptr);

    // 任务页
    std::unique_ptr<::core::Module> tasks(create_release_tasks_module());
    DOCTEST_REQUIRE(tasks != nullptr);
    DOCTEST_CHECK(tasks->pageId() == ::core::PageId::ReleaseTasks);
    DOCTEST_CHECK_EQ(tasks->displayName(), std::string("任务"));
    DOCTEST_CHECK_EQ(std::string(::core::pageIdToString(tasks->pageId())),
                     std::string("ReleaseTasks"));

    eui::Screen screen;
    screen.width = 1000.0f;
    screen.height = 640.0f;
    eui::Ui tasksUi;
    tasksUi.begin("release.tasks.page");
    tasks->compose(tasksUi, screen);
    tasksUi.end();
    const auto* tasksEmpty = tasksUi.find("release.tasks.empty");
    DOCTEST_REQUIRE(tasksEmpty != nullptr);
    DOCTEST_CHECK_EQ(tasksEmpty->text, std::string("暂无数据"));
    const auto* tasksTitle = tasksUi.find("release.tasks.title");
    DOCTEST_REQUIRE(tasksTitle != nullptr);
    DOCTEST_CHECK_EQ(tasksTitle->text, std::string("任务"));

    // 基础配置页
    std::unique_ptr<::core::Module> config(create_release_config_module());
    DOCTEST_REQUIRE(config != nullptr);
    DOCTEST_CHECK(config->pageId() == ::core::PageId::ReleaseConfig);
    DOCTEST_CHECK_EQ(config->displayName(), std::string("基础配置"));

    eui::Ui configUi;
    configUi.begin("release.config.page");
    config->compose(configUi, screen);
    configUi.end();
    const auto* configEmpty = configUi.find("release.config.empty");
    DOCTEST_REQUIRE(configEmpty != nullptr);
    DOCTEST_CHECK_EQ(configEmpty->text, std::string("暂无数据"));

    // 登记表：三行同属 release 门控，两个子页 parent 指向分组父项。
    // parent 字段必须一起断言——只断言 gatingKey 时把子项 parent 改错，用例仍绿
    //（审计 M4 的假绿点），侧边栏会静默退化成平铺。
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page != ::core::PageId::ReleaseGroup &&
            e.page != ::core::PageId::ReleaseTasks &&
            e.page != ::core::PageId::ReleaseConfig) {
            continue;
        }
        DOCTEST_REQUIRE(e.gatingKey != nullptr);
        DOCTEST_CHECK_EQ(std::string(e.gatingKey),
                         std::string(module::settings::kModuleIdRelease));
        if (e.page == ::core::PageId::ReleaseGroup) {
            DOCTEST_CHECK_FALSE(e.parent.has_value());
        } else {
            DOCTEST_REQUIRE(e.parent.has_value());
            DOCTEST_CHECK(*e.parent == ::core::PageId::ReleaseGroup);
        }
    }
    DOCTEST_CHECK(::app::registry::isGroupPage(::core::PageId::ReleaseGroup));
    DOCTEST_CHECK(!::app::registry::isGroupPage(::core::PageId::ReleaseTasks));
    return true;
})

// 防（审计 m2）：parent 改错/指向不存在的 PageId 时子项静默平铺为顶级项；
// 分组行工厂被改成返回实例时父项意外可导航。两条都无运行时日志，只能静态锁。
REGISTER_TEST(release_group_metadata_consistency,
              "release: 分组元数据自洽——有 parent 的行指向真实存在且工厂为空的分组行", {
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (!e.parent.has_value()) continue;
        const ::app::registry::ModuleEntry* parent = registryRow(*e.parent);
        DOCTEST_REQUIRE_MESSAGE(parent != nullptr,
                                "parent 指向登记表不存在的 PageId（子项会静默平铺）");
        std::unique_ptr<::core::Module> parentModule(parent->factory());
        DOCTEST_CHECK_MESSAGE(parentModule == nullptr,
                              "分组行工厂必须返回 nullptr（不可导航，不注册进 Router）");
    }
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (!::app::registry::isGroupPage(e.page)) continue;
        std::unique_ptr<::core::Module> groupModule(e.factory());
        DOCTEST_CHECK_MESSAGE(groupModule == nullptr,
                              "分组父项工厂恒为空（按 parent 反推的分组身份）");
    }
    return true;
})

// 防（审计 M4）：分组树退化成平铺——子项缩进丢失/父项不渲染时无人变红。
// 几何契约：顶级项 indent 0（x = padX），子项 16；标签同差 16，命中区同宽减 16。
REGISTER_TEST(release_sidebar_group_geometry,
              "release: 侧边栏父项 + 子项 indent 16 与顶级项 0 的几何差（防分组静默平铺）", {
    app::setCustomTitlebarMode(false);  // 固定系统标题栏路径，几何不随运行平台漂移
    app::resetSidebarGlobals();         // 复位：侧栏展开，分组回到默认折叠
    app::setFluidBackgroundEnabled(false);  // 流质与本用例几何无关

    SidebarStubModule home(::core::PageId::Home, "首页");
    SidebarStubModule tasks(::core::PageId::ReleaseTasks, "任务");
    SidebarStubModule config(::core::PageId::ReleaseConfig, "基础配置");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&tasks);
    router.registerModule(&config);
    test_support::ScopedAppRouter scoped(&router);

    const eui::Screen screen{1600.0f, 1000.0f};

    // 默认折叠（新契约）：子项不渲染、箭头朝下。几何用例要量子项，先验证默认态
    // 再显式展开（旧契约默认展开，重排后这条期望必须反过来）
    {
        eui::Ui collapsedUi;
        composeFrame(collapsedUi, screen);
        DOCTEST_CHECK(collapsedUi.find("app.sidebar.item.ReleaseTasks.hit") ==
                      nullptr);
        DOCTEST_CHECK(collapsedUi.find("app.sidebar.item.ReleaseConfig.hit") ==
                      nullptr);
        const auto* collapsedChevron =
            collapsedUi.find("app.sidebar.item.ReleaseGroup.chevron");
        DOCTEST_REQUIRE(collapsedChevron != nullptr);
        DOCTEST_CHECK_EQ(collapsedChevron->text, eui::utf8(0xF078));  // 折叠 = chevron-down
    }
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    DOCTEST_REQUIRE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));

    eui::Ui ui;
    composeFrame(ui, screen);

    const auto* groupHit = ui.find("app.sidebar.item.ReleaseGroup.hit");
    DOCTEST_REQUIRE(groupHit != nullptr);
    const auto* groupLabel = ui.find("app.sidebar.item.ReleaseGroup.label");
    DOCTEST_REQUIRE(groupLabel != nullptr);
    DOCTEST_CHECK_EQ(groupLabel->text, std::string("上线"));
    const auto* chevron = ui.find("app.sidebar.item.ReleaseGroup.chevron");
    DOCTEST_REQUIRE(chevron != nullptr);
    DOCTEST_CHECK_EQ(chevron->text, eui::utf8(0xF077));  // 展开 = chevron-up

    const auto* homeHit = ui.find("app.sidebar.item.Home.hit");
    const auto* tasksHit = ui.find("app.sidebar.item.ReleaseTasks.hit");
    const auto* configHit = ui.find("app.sidebar.item.ReleaseConfig.hit");
    const auto* homeLabel = ui.find("app.sidebar.item.Home.label");
    const auto* tasksLabel = ui.find("app.sidebar.item.ReleaseTasks.label");
    DOCTEST_REQUIRE(homeHit != nullptr);
    DOCTEST_REQUIRE(tasksHit != nullptr);
    DOCTEST_REQUIRE(configHit != nullptr);
    DOCTEST_REQUIRE(homeLabel != nullptr);
    DOCTEST_REQUIRE(tasksLabel != nullptr);

    // 父项与顶级项同起点（父项本身不缩进）
    DOCTEST_CHECK(floatEq(groupHit->x, homeHit->x));
    // 子项 hit / 标签同步右移 16，命中区同步收窄 16
    DOCTEST_CHECK(floatEq(tasksHit->x - homeHit->x, 16.0f));
    DOCTEST_CHECK(floatEq(configHit->x - homeHit->x, 16.0f));
    DOCTEST_CHECK(floatEq(tasksLabel->x - homeLabel->x, 16.0f));
    DOCTEST_CHECK(floatEq(homeHit->width.value - tasksHit->width.value, 16.0f));

    resetSidebarTestState();
    return true;
})

// 防（审计 M3/M4）：当前页在分组内时收起无效（旧实现本帧强制展开，toggle 写入
// 却画面不动）。收起必须生效：子项隐藏、父项保留并保持「当前页在此组内」的选中态。
// 2026-10 起默认折叠：本用例先显式展开（展开态不再默认），再验证手动收起。
REGISTER_TEST(release_sidebar_group_collapse_holds_current,
              "release: 当前页在组内时手动收起同样生效——子项消失、父项保留且保持选中态", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();
    app::setFluidBackgroundEnabled(false);

    SidebarStubModule home(::core::PageId::Home, "首页");
    SidebarStubModule tasks(::core::PageId::ReleaseTasks, "任务");
    SidebarStubModule config(::core::PageId::ReleaseConfig, "基础配置");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&tasks);
    router.registerModule(&config);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::ReleaseTasks);
    DOCTEST_REQUIRE(router.currentPage() == ::core::PageId::ReleaseTasks);

    const eui::Screen screen{1600.0f, 1000.0f};

    // 默认折叠（旧契约此用例在此断言「展开态：两个子项都在」——期望反转）
    DOCTEST_CHECK_FALSE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") == nullptr);
    }

    // 再展开（默认态之外需显式 toggle，见 release_sidebar_group_click_parent_expands）
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    DOCTEST_REQUIRE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.sel.bg") != nullptr);
    }

    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    DOCTEST_CHECK_FALSE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        // 子项隐藏（当前页在组内也不例外）
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.label") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") == nullptr);
        // 父项保留 + 选中态 + 箭头翻成收起
        const auto* groupHit = ui.find("app.sidebar.item.ReleaseGroup.hit");
        DOCTEST_REQUIRE(groupHit != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.sel.bg") != nullptr);
        const auto* chevron = ui.find("app.sidebar.item.ReleaseGroup.chevron");
        DOCTEST_REQUIRE(chevron != nullptr);
        DOCTEST_CHECK_EQ(chevron->text, eui::utf8(0xF078));  // 收起 = chevron-down
    }

    // 再切一次恢复展开：子项回归（状态判定与 toggle 同一来源）
    app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") != nullptr);
    }

    resetSidebarTestState();
    return true;
})

// 2026-10 新契约：二级菜单默认折叠（旧契约默认展开）。点父项 onClick 必须展开，
// 且展开态与 toggleSidebarGroup 同一状态源（渲染与点击不能各记一份）。
REGISTER_TEST(release_sidebar_group_click_parent_expands,
              "release: 默认折叠 → 点父项后展开（子项出现、箭头翻转、状态源一致）", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();
    app::setFluidBackgroundEnabled(false);

    SidebarStubModule home(::core::PageId::Home, "首页");
    SidebarStubModule tasks(::core::PageId::ReleaseTasks, "任务");
    SidebarStubModule config(::core::PageId::ReleaseConfig, "基础配置");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&tasks);
    router.registerModule(&config);
    test_support::ScopedAppRouter scoped(&router);

    const eui::Screen screen{1600.0f, 1000.0f};

    // 默认折叠：父项在、子项不在、箭头朝下
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK_FALSE(
            app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") == nullptr);
        const auto* groupHit = ui.find("app.sidebar.item.ReleaseGroup.hit");
        DOCTEST_REQUIRE(groupHit != nullptr);
        DOCTEST_REQUIRE(groupHit->onClick != nullptr);
        const auto* chevron = ui.find("app.sidebar.item.ReleaseGroup.chevron");
        DOCTEST_REQUIRE(chevron != nullptr);
        DOCTEST_CHECK_EQ(chevron->text, eui::utf8(0xF078));  // 折叠 = chevron-down
        groupHit->onClick();                                  // 真点父项
    }
    DOCTEST_REQUIRE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));

    // 展开后：子项出现、箭头翻成展开态
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") != nullptr);
        const auto* chevron = ui.find("app.sidebar.item.ReleaseGroup.chevron");
        DOCTEST_REQUIRE(chevron != nullptr);
        DOCTEST_CHECK_EQ(chevron->text, eui::utf8(0xF077));  // 展开 = chevron-up
        // 同一入口再点一次 → 收回
        const auto* groupHit = ui.find("app.sidebar.item.ReleaseGroup.hit");
        DOCTEST_REQUIRE(groupHit != nullptr);
        DOCTEST_REQUIRE(groupHit->onClick != nullptr);
        groupHit->onClick();
    }
    DOCTEST_CHECK_FALSE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") == nullptr);
    }

    resetSidebarTestState();
    return true;
})

// 2026-10 新契约：当前页在组内不自动展开——子项保持隐藏，选中信息只由父项的
// SelectionBar 承载（否则手动收起会被「本帧强制展开」弹回）。
REGISTER_TEST(release_sidebar_group_default_collapsed_holds_current,
              "release: 默认折叠下当前页在组内 → 父项保持选中态（子项隐藏）", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();
    app::setFluidBackgroundEnabled(false);

    SidebarStubModule home(::core::PageId::Home, "首页");
    SidebarStubModule tasks(::core::PageId::ReleaseTasks, "任务");
    SidebarStubModule config(::core::PageId::ReleaseConfig, "基础配置");
    ::core::Router router;
    router.registerModule(&home);
    router.registerModule(&tasks);
    router.registerModule(&config);
    test_support::ScopedAppRouter scoped(&router);
    router.navigateTo(::core::PageId::ReleaseTasks);
    DOCTEST_REQUIRE(router.currentPage() == ::core::PageId::ReleaseTasks);

    const eui::Screen screen{1600.0f, 1000.0f};

    // 默认折叠（未做任何 toggle）：当前页在组内 → 子项隐藏、父项选中
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK_FALSE(
            app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.label") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.sel.bg") != nullptr);
        const auto* chevron = ui.find("app.sidebar.item.ReleaseGroup.chevron");
        DOCTEST_REQUIRE(chevron != nullptr);
        DOCTEST_CHECK_EQ(chevron->text, eui::utf8(0xF078));
    }

    // 选中态由当前页驱动而非父项恒定装饰：离开该组后选中态消失
    router.navigateTo(::core::PageId::Home);
    {
        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.sel.bg") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.Home.sel.bg") != nullptr);
    }

    resetSidebarTestState();
    return true;
})

// 重排只「插入」不「调序」：既有 8 项在渲染序里相对顺序不变，上线父项占一行，
// 令尾部（Settings/About）整体下移；本用例把表序和真实侧栏几何一起锁住。
REGISTER_TEST(release_sidebar_existing_eight_order_and_shifted_down,
              "release: 既有 8 项侧边栏相对顺序不变，上线父项占位令设置/关于整体下移", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();
    app::setFluidBackgroundEnabled(false);

    SidebarStubModule home(::core::PageId::Home, "首页");
    SidebarStubModule markdown(::core::PageId::Markdown, "笔记");
    SidebarStubModule todo(::core::PageId::Todo, "待办");
    SidebarStubModule claude(::core::PageId::ClaudeProxy, "Claude");
    SidebarStubModule agent(::core::PageId::Agent, "智能体");
    SidebarStubModule wallpaper(::core::PageId::Wallpaper, "壁纸");
    SidebarStubModule settings(::core::PageId::Settings, "设置");
    SidebarStubModule about(::core::PageId::About, "关于");
    SidebarStubModule tasks(::core::PageId::ReleaseTasks, "任务");
    SidebarStubModule config(::core::PageId::ReleaseConfig, "基础配置");
    ::core::Router router;
    // 注册序 = 新表序（router 按 PageId 插入，乱序也等价；这里保持真源可读）
    router.registerModule(&home);
    router.registerModule(&markdown);
    router.registerModule(&todo);
    router.registerModule(&claude);
    router.registerModule(&agent);
    router.registerModule(&wallpaper);
    router.registerModule(&tasks);
    router.registerModule(&config);
    router.registerModule(&settings);
    router.registerModule(&about);
    test_support::ScopedAppRouter scoped(&router);

    const eui::Screen screen{1600.0f, 1000.0f};
    eui::Ui ui;
    composeFrame(ui, screen);

    // 既有 8 项完整顺序数组（重排前契约序，必须原样保留）
    const char* const kExistingOrder[] = {
        "Home", "Markdown", "Todo", "ClaudeProxy",
        "Agent", "Wallpaper", "Settings", "About",
    };
    constexpr int kExistingCount =
        static_cast<int>(sizeof(kExistingOrder) / sizeof(kExistingOrder[0]));
    const core::dsl::Element* previous = nullptr;
    for (int i = 0; i < kExistingCount; ++i) {
        const std::string id =
            std::string("app.sidebar.item.") + kExistingOrder[i] + ".hit";
        const auto* item = ui.find(id);
        DOCTEST_REQUIRE(item != nullptr);
        if (previous != nullptr) {
            DOCTEST_CHECK(item->y > previous->y);  // 相对顺序严格不变
        }
        previous = item;
    }

    const auto* wallpaperHit = ui.find("app.sidebar.item.Wallpaper.hit");
    DOCTEST_REQUIRE(wallpaperHit != nullptr);
    const auto* groupHit = ui.find("app.sidebar.item.ReleaseGroup.hit");
    DOCTEST_REQUIRE(groupHit != nullptr);
    const auto* settingsHit = ui.find("app.sidebar.item.Settings.hit");
    DOCTEST_REQUIRE(settingsHit != nullptr);
    const auto* aboutHit = ui.find("app.sidebar.item.About.hit");
    DOCTEST_REQUIRE(aboutHit != nullptr);

    // 父项插在 Wallpaper 与 Settings 之间（组折叠：父项占一行，子项不占高）
    DOCTEST_CHECK(groupHit->y > wallpaperHit->y);
    DOCTEST_CHECK(groupHit->y < settingsHit->y);
    // 父项恰好占一个常规项步距（kNavItemH 40 + gap 4 = 44）→ Settings/About 相对
    // 旧布局整体下移这 44；父项到 Settings 还隔着组间 divider（1 + gap 4 = 5）
    DOCTEST_CHECK(floatEq(groupHit->y - wallpaperHit->y, 44.0f));
    DOCTEST_CHECK(floatEq(settingsHit->y - groupHit->y, 49.0f));
    DOCTEST_CHECK(floatEq(aboutHit->y - settingsHit->y, 44.0f));

    resetSidebarTestState();
    return true;
})

// 防（审计 M4/判据 5）：模块开关只注销部分行时父项残留（或反过来）。装配级：
// 关闭 release 后三行都不进 Router，侧边栏父项与两个子项一起消失。
REGISTER_TEST(release_group_removed_with_module_off,
              "release: 关闭 release 开关后父项与两个子项一起消失（装配级断言）", {
    app::setCustomTitlebarMode(false);
    app::resetSidebarGlobals();
    app::setFluidBackgroundEnabled(false);

    SidebarStubModule home(::core::PageId::Home, "首页");
    SidebarStubModule settings(::core::PageId::Settings, "设置");
    SidebarStubModule about(::core::PageId::About, "关于");
    SidebarStubModule tasks(::core::PageId::ReleaseTasks, "任务");
    SidebarStubModule config(::core::PageId::ReleaseConfig, "基础配置");

    constexpr std::size_t kRegistrySize =
        sizeof(::app::registry::kModuleRegistry) /
        sizeof(::app::registry::kModuleRegistry[0]);
    const eui::Screen screen{1600.0f, 1000.0f};

    // ① release 开：三行过门控（父项装配期无实例，由侧边栏按子行反推绘制）
    {
        module::settings::SettingsConfig cfg;
        cfg.enabledModules = {module::settings::kModuleIdHome,
                              module::settings::kModuleIdSettings,
                              module::settings::kModuleIdRelease};
        const auto gated = app::assembly::buildGatedEntries(
            cfg, ::app::registry::kModuleRegistry, kRegistrySize);
        // home/schedule/settings/about 恒注册 4 行 + 上线三行 = 7
        DOCTEST_REQUIRE_EQ(gated.size(), static_cast<std::size_t>(7));

        ::core::Router router;
        router.registerModule(&home);
        router.registerModule(&settings);
        router.registerModule(&about);
        router.registerModule(&tasks);
        router.registerModule(&config);
        test_support::ScopedAppRouter scoped(&router);

        // 默认折叠：父项画、子项不画；展开后才断言两个子项参与渲染
        {
            eui::Ui collapsedUi;
            composeFrame(collapsedUi, screen);
            DOCTEST_CHECK(collapsedUi.find("app.sidebar.item.ReleaseGroup.hit") !=
                          nullptr);
            DOCTEST_CHECK(collapsedUi.find("app.sidebar.item.ReleaseTasks.hit") ==
                          nullptr);
            DOCTEST_CHECK(collapsedUi.find("app.sidebar.item.ReleaseConfig.hit") ==
                          nullptr);
        }
        app::toggleSidebarGroup(::core::PageId::ReleaseGroup);
        DOCTEST_REQUIRE(app::sidebarGroupExpanded(::core::PageId::ReleaseGroup));

        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") != nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") != nullptr);
    }

    // ② release 关：门控只剩恒注册四行 → 父项与两个子项整棵不出现
    {
        module::settings::SettingsConfig cfg;
        cfg.enabledModules = {module::settings::kModuleIdHome,
                              module::settings::kModuleIdSettings};
        const auto gated = app::assembly::buildGatedEntries(
            cfg, ::app::registry::kModuleRegistry, kRegistrySize);
        // home/schedule/settings/about 恒注册 4 行
        DOCTEST_REQUIRE_EQ(gated.size(), static_cast<std::size_t>(4));

        ::core::Router router;
        router.registerModule(&home);
        router.registerModule(&settings);
        router.registerModule(&about);
        test_support::ScopedAppRouter scoped(&router);

        eui::Ui ui;
        composeFrame(ui, screen);
        DOCTEST_CHECK(ui.find("app.sidebar.item.Home.hit") != nullptr);  // 侧栏确已渲染
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseGroup.label") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseTasks.hit") == nullptr);
        DOCTEST_CHECK(ui.find("app.sidebar.item.ReleaseConfig.hit") == nullptr);
    }

    resetSidebarTestState();
    return true;
})