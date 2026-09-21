// src/main.cpp
// DevDesk 入口 - EUI-NEO 提供 main 函数，我们只需提供
// app::dslAppConfig() 和 app::compose()

#include "core/database.h"
#include "core/logging.h"
#include "core/module_interface.h"
#include "core/paths.h"
#include "core/router.h"

#include <eui/dsl.h>
#include <eui/dsl_app.h>

#include <memory>
#include <cstdlib>
#include <string>

extern "C" ::core::Module* create_home_module();
extern "C" ::core::Module* create_markdown_module();
extern "C" ::core::Module* create_todo_module();
extern "C" ::core::Module* create_claude_module();

// macOS 私有 API：获取进程的 argv（EUI-NEO 的 main 不接 argc/argv）
// 这是 Apple 长期稳定的 API（自 10.0 起），用于替代 main 签名无法扩展的场景
#if defined(__APPLE__)
extern "C" char** _NSGetArgv(void);
extern "C" int _NSGetArgc(void);
#endif

namespace {

// 全局：Finder "打开方式" 传入的文件路径
// MVP：仅记录 + log，不实际打开（架构准备好）
// 后续 P 阶段在 Router 启动后根据扩展名 dispatch 到对应模块
std::string& pendingOpenFilePath() {
    static std::string path;
    return path;
}

// 从 argv 捕获要打开的文件（仅在第一次 dslAppConfig() 调用时执行）
// macOS: 使用 _NSGetArgv 读取命令行
// 其他平台: 无操作（依赖 Finder open with + EUI-NEO 上游支持）
void captureOpenFileFromArgv() {
#if defined(__APPLE__)
    static bool captured = false;
    if (captured) return;
    captured = true;

    int argc = _NSGetArgc();
    char** argv = _NSGetArgv();
    if (argc < 2 || argv == nullptr) return;

    // argv[1] 是文件路径（Finder 双击时 macOS 自动传入）
    const char* path = argv[1];
    if (path == nullptr || path[0] == '\0') return;

    // 跳过 flag 类参数（以 '-' 开头）
    if (path[0] == '-') return;

    pendingOpenFilePath() = std::string(path);
    LOG_INFO("捕获到待打开文件: " + pendingOpenFilePath());
#endif
}

// 根据文件扩展名返回目标 PageId（架构准备：MVP 只返回目标，不实际加载内容）
// 后续 P 阶段：让对应模块读 pendingOpenFilePath() 并实际渲染文件
::core::PageId dispatchPageForFile(const std::string& path) {
    auto endsWith = [&](const char* suffix) {
        size_t n = std::string(suffix).size();
        return path.size() >= n &&
               path.compare(path.size() - n, n, suffix) == 0;
    };
    if (endsWith(".md") || endsWith(".markdown")) return ::core::PageId::Markdown;
    if (endsWith(".json")) return ::core::PageId::ClaudeProxy;
    if (endsWith(".js") || endsWith(".html") || endsWith(".htm") ||
        endsWith(".txt")) return ::core::PageId::Markdown;  // 文本类用 Markdown 模块查看
    return ::core::PageId::Home;
}

void setupLogging() {
    core::log::addFileOutput(::core::paths::logFile());
    core::log::addConsoleOutput();
    core::log::setLevel(core::log::Level::Info);
}

// 打开应用数据库并跑 migrations
// 注意：main.cpp 用 -fno-exceptions 编译，所以用 unsafeValue() 而非 .value()
::core::Database& openAppDatabase() {
    static std::unique_ptr<::core::Database> db;
    static bool initialized = false;
    if (!initialized) {
        auto r = ::core::Database::open(::core::paths::sqliteDbFile());
        if (r.isFail()) {
            LOG_FATAL("无法打开数据库: " + r.errorMessage());
            std::abort();
        }
        // unsafeValue() 是 Result 的非抛出版本，返回 T&。再用 std::move 转成 xvalue
        // 触发 Database 的 move ctor。
        auto raw = new ::core::Database(std::move(r.unsafeValue()));
        db.reset(raw);
        auto migrateResult = db->migrate();
        if (migrateResult.isFail()) {
            LOG_FATAL("数据库迁移失败: " + migrateResult.errorMessage());
            std::abort();
        }
        LOG_INFO("数据库已就绪: " + ::core::paths::sqliteDbFile().string());
        initialized = true;
    }
    return *db;
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

        // 架构准备：如果有 pending 文件，dispatch 到对应 Page
        // MVP：仅切换页面；实际加载文件由对应模块在 onStart/P 阶段实现
        const std::string& pendingFile = pendingOpenFilePath();
        if (!pendingFile.empty()) {
            ::core::PageId target = dispatchPageForFile(pendingFile);
            LOG_INFO("MVP dispatch: 切换到 " +
                     std::string(pageIdToString(target)) +
                     "（实际文件加载留待 P 阶段）");
            router.navigateTo(target);
        }
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
        captureOpenFileFromArgv();
        paths::ensureDirectoriesExist();
        setupLogging();
        LOG_INFO("DevDesk 启动中...");
        LOG_INFO("应用数据目录: " + paths::appDataDir().string());
        openAppDatabase();
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