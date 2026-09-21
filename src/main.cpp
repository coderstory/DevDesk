// src/main.cpp
// DevDesk 入口 - EUI-NEO 提供 main 函数，我们只需提供
// app::dslAppConfig() 和 app::compose()

#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <memory>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();

namespace {

void setupLogging() {
    core::log::addFileOutput(::core::paths::logFile());
    core::log::addConsoleOutput();
    core::log::setLevel(core::log::Level::Info);
}

::core::Router* getRouter() {
    static ::core::Router router;
    static std::unique_ptr<::core::Module> home(create_home_module());
    static std::unique_ptr<::core::Module> markdown(create_markdown_module());
    static std::unique_ptr<::core::Module> todo(create_todo_module());
    static std::unique_ptr<::core::Module> claude(create_claude_module());

    static bool initialized = false;
    if (!initialized) {
        router.registerModule(home.get());
        router.registerModule(markdown.get());
        router.registerModule(todo.get());
        router.registerModule(claude.get());
        router.startAll();
        initialized = true;
    }
    return &router;
}

} // namespace

namespace app {

// DslAppConfig 在 eui::dsl_app.h 中定义（命名空间 app）
const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("DevDesk")
        .pageId("devdesk")
        .clearColor(eui::Color("#FAF9F5"))
        .windowSize(1600, 1000)
        .minWindowSize(1200, 720)
        .highDpi(true)
        .resizable(true)
        .centerWindow();

    static bool initialized = []() {
        using namespace core;
        paths::ensureDirectoriesExist();
        setupLogging();
        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + paths::appDataDir().string());
        getRouter();
        return true;
    }();
    (void)initialized;

    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    auto* router = getRouter();
    if (router) {
        router->compose(ui, screen);
    }
}

} // namespace app