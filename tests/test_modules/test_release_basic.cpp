// 上线模块骨架冒烟：两个可导航页的注册元数据 + 空态渲染，证明
// 登记表 → 工厂 → Router 页面 → 模块 compose 的链路可达。
// 分组父项 create_release_group_module 按契约返回 nullptr（不可导航、不注册）。
#include "doctest.h"

#include "app_registry.h"
#include "core/module_interface.h"
#include "settings_config.h"

#include <eui/dsl.h>

#include <memory>
#include <string>

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

    // 登记表：三行同属 release 门控，两个子页 parent 指向分组父项
    for (const auto& e : ::app::registry::kModuleRegistry) {
        if (e.page != ::core::PageId::ReleaseGroup &&
            e.page != ::core::PageId::ReleaseTasks &&
            e.page != ::core::PageId::ReleaseConfig) {
            continue;
        }
        DOCTEST_REQUIRE(e.gatingKey != nullptr);
        DOCTEST_CHECK_EQ(std::string(e.gatingKey),
                         std::string(module::settings::kModuleIdRelease));
    }
    DOCTEST_CHECK(::app::registry::isGroupPage(::core::PageId::ReleaseGroup));
    DOCTEST_CHECK(!::app::registry::isGroupPage(::core::PageId::ReleaseTasks));
    return true;
})