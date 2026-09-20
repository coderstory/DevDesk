// src/main.cpp
// DevDesk 入口
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"
#include "core/sqlite_db.h"

#include <eui/dsl_app.h>

#include <exception>
#include <iostream>
#include <memory>

extern "C" core::Module* create_home_module();
extern "C" core::Module* create_markdown_module();
extern "C" core::Module* create_todo_module();
extern "C" core::Module* create_claude_module();

namespace {

void setupLogging() {
    core::log::addFileOutput(core::paths::logFile());
    core::log::addConsoleOutput();
    core::log::setLevel(core::log::Level::Info);
}

core::Result<core::data::SqliteDb> setupDatabase() {
    return core::data::SqliteDb::open(core::paths::sqliteDbFile());
}

eui::DslAppConfig buildAppConfig() {
    return eui::DslAppConfig{}
        .title("DevDesk")
        .windowSize(1600, 1000)
        .minWindowSize(1200, 720)
        .fps(90.0)
        .textFont("fonts/Inter-Regular.ttf")
        .iconFont("fonts/Font Awesome 7 Free-Solid-900.otf");
}

} // namespace

int main() {
    try {
        core::paths::ensureDirectoriesExist();
        setupLogging();

        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + core::paths::appDataDir().string());

        // 初始化数据库
        auto dbResult = setupDatabase();
        if (dbResult.isFail()) {
            LOG_ERROR("数据库初始化失败: " + dbResult.errorMessage());
            return 1;
        }

        // 注册模块
        core::Router router;
        std::unique_ptr<core::Module> home(create_home_module());
        std::unique_ptr<core::Module> markdown(create_markdown_module());
        std::unique_ptr<core::Module> todo(create_todo_module());
        std::unique_ptr<core::Module> claude(create_claude_module());

        router.registerModule(home.get());
        router.registerModule(markdown.get());
        router.registerModule(todo.get());
        router.registerModule(claude.get());

        router.startAll();
        LOG_INFO("DevDesk 已启动");

        // 运行 EUI-NEO 应用
        auto config = buildAppConfig();
        eui::run(config, [&](eui::Ui& ui, const eui::Screen& screen) {
            router.compose(ui, screen);
        });

        router.stopAll();
        LOG_INFO("DevDesk 退出");
        return 0;
    } catch (const std::exception& e) {
        LOG_FATAL(std::string("致命错误: ") + e.what());
        std::cerr << "Fatal: " << e.what() << std::endl;
        return 1;
    }
}