// tests/test_support.h
// 用例共享支撑头（审计 TIN-06）：51 个用例文件此前各自抄一份 test fixture ——
// TestEnv 5 份 / TempDir 2 份 / setEnvVar 7 份，同一语义还有多种入参签名
// （const char* / std::string）；口径漂移是 TIN-07 与历次 flake 的温床。
//
// 这里收四件套（header-only；tests/ 已在 target_include_directories 里，
// 无需改 tests/CMakeLists.txt）：
//   1) 环境变量作用域 —— setEnvVar（两种入参签名）+ ScopedEnv（RAII 还原）
//   2) 临时目录       —— makeTempDir（一次性）+ TempDir（RAII，析构删）
//   3) 数据根隔离     —— AppDataEnv（APPDATA / HOME 重定向到临时目录）
//   4) 应用单例作用域 —— ScopedAppRouter
//
// 迁移约定：只删本地重复脚手架 + 改 include/调用，**不动用例语义**；
// 收编时每个分支逐字保留原样（见各函数上的「原 XXX 同款」出处注释）。
#pragma once

#include "doctest.h" // test_temp::tempPath —— 临时路径唯一化（pid + 进程内序号）

#include "core/router.h" // ScopedAppRouter

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace test_support {

// ==================== 1) 环境变量作用域 ====================
// 两种入参签名都是存量原样收编（同一语义各写一份是本次审计的靶子本身）。
// 分支逐字保留，不要合并：
//   Windows：_putenv_s，传空串即删除变量
//   POSIX  ：空值走 unsetenv ——「未设置」与「空串」语义不同
// 空值判定两版写法不同（std::string::empty() / const char* 判空），各自保真。

// 入参 std::string（原 test_paths.cpp / test_pet.cpp / test_settings.cpp 同款）
inline void setEnvVar(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

// 入参 const char*（原 test_claude_service.cpp / test_http_client.cpp 同款）
inline void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value && *value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

// RAII：作用域内设置环境变量，析构还原原值（原 test_settings.cpp EnvVarGuard
// 同款——构造时快照当前值，「未设置」记为空串，析构时传空串即删除）。
// 用例体内 fail-fast 提前 return false 也会走析构，不留环境泄漏给后续用例。
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        const char* cur = std::getenv(name);
        saved_ = cur ? cur : "";
        setEnvVar(name, value);
    }
    ~ScopedEnv() { setEnvVar(name_.c_str(), saved_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::string saved_;
};

// ==================== 2) 临时目录 ====================
// 一次性临时目录：确保干净后返回路径（**不**自动清理，调用方自己管）。
// 原 test_markdown_service.cpp / test_md_file_open.cpp 的 makeTempDir 与
// test_markdown.cpp / test_md_highlight.cpp / test_md_keyboard.cpp 的
// makeTempNotesDir 合流——三处 body 一字不差，只有 %TEMP% 文件名前缀不同
// （前缀保留，便于辨认残骸是谁留下的）。路径唯一性由 test_temp::tempPath
// 的 pid + 序号兜底，这里的 count 只维持原有命名形态。
inline std::filesystem::path makeTempDir(const std::string& prefix,
                                         const std::string& tag) {
    static int count = 0;
    ++count;
    const std::filesystem::path dir = test_temp::tempPath(
        prefix + tag + "_" + std::to_string(count));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec); // 清掉上次失败留下的残骸
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// RAII 临时目录：构造 = 清残骸 + 建空目录（失败即抛，别让用例跑在
// 不存在的目录上）；析构 = 整棵删掉。原 test_claude_service.cpp 的 TempDir
// 与 test_claude_ui.cpp 的 ImportTempDir 合流。
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& tag,
                     const std::string& prefix = "devdesk_test_") {
        static int count = 0;
        ++count;
        path = test_temp::tempPath(prefix + tag + "_" + std::to_string(count));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
        if (ec) {
            throw std::runtime_error("temp dir create failed");
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path file(const char* name) const { return path / name; }
    void write(const std::filesystem::path& f, const std::string& content) const {
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << content;
    }
    void remove(const std::filesystem::path& f) const {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
    std::string read(const std::filesystem::path& f) const {
        std::ifstream i(f, std::ios::binary);
        std::ostringstream ss;
        ss << i.rdbuf();
        return ss.str();
    }
    bool exists(const std::filesystem::path& f) const {
        std::error_code ec;
        return std::filesystem::exists(f, ec) && !ec;
    }
};

// ==================== 3) 数据根隔离 ====================
// RAII：把「应用数据根目录」重定向到临时目录（原 test_settings.cpp
// AppDataEnv 同款）——日志路径 core::paths::logFile() 的平台来源不同：
//   Windows: %APPDATA%/DevDesk/logs/devdesk.log   （paths_win.cpp）
//   macOS:   $HOME/Library/Application Support/DevDesk/logs/devdesk.log
//   Linux:   $HOME/.local/share/DevDesk/logs/devdesk.log
// 只重定向 APPDATA 的话，macOS/Linux 上用例会把自造日志写进**真实**用户
// 目录；故两套变量都在此隔离（Windows 侧只动 APPDATA，行为与从前一字不差）。
// 析构恢复原值，fail-fast 早退也不影响后续用例。
struct AppDataEnv {
    std::string saved;
#if !defined(_WIN32)
    std::string savedHome; // macOS/Linux 的日志路径来源是 $HOME
#endif
    std::filesystem::path dir;

    explicit AppDataEnv(const std::string& tag) {
        const char* cur = std::getenv("APPDATA");
        saved = cur ? cur : "";
#if !defined(_WIN32)
        const char* curHome = std::getenv("HOME");
        savedHome = curHome ? curHome : "";
#endif
        dir = test_temp::tempPath("devdesk_test_settings_appdata_" + tag);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        setEnvVar("APPDATA", dir.string());
#if !defined(_WIN32)
        setEnvVar("HOME", dir.string());
#endif
    }

    ~AppDataEnv() {
        setEnvVar("APPDATA", saved);
#if !defined(_WIN32)
        setEnvVar("HOME", savedHome);
#endif
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AppDataEnv(const AppDataEnv&) = delete;
    AppDataEnv& operator=(const AppDataEnv&) = delete;
};

// ==================== 4) 应用单例作用域脚手架 ====================
// RAII：作用域内登记进程级 appRouter（assembleAndStart 会把 Router 注册进
// 进程级访问器，armExitHook 的 handler 经它取 Router，而用例里的 Router 是
// 栈对象），析构复位 null——否则进程退出时 atexit 会摸到已析构的 Router。
// 原 test_app_assembly.cpp / test_app_sidebar.cpp / test_module_gating.cpp
// 三份 ScopedAppRouter 与 test_markdown.cpp 用例体内的 RouterGuard 合流；
// 默认构造传 nullptr = 只保留「复位」语义（test_app_assembly 那份本就如此）。
class ScopedAppRouter {
public:
    explicit ScopedAppRouter(::core::Router* router = nullptr) {
        ::core::registerAppRouter(router);
    }
    ~ScopedAppRouter() { ::core::registerAppRouter(nullptr); }
    ScopedAppRouter(const ScopedAppRouter&) = delete;
    ScopedAppRouter& operator=(const ScopedAppRouter&) = delete;
};

} // namespace test_support
