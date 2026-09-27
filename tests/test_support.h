// tests/test_support.h
// 用例共享支撑头（审计 TIN-06 首批 + TIN-06b 第二批）：51 个用例文件此前各自
// 抄一份 test fixture —— TestEnv 5 份 / TempDir 2 份 / setEnvVar 7 份 /
// TestDb 4 份 / tempDbPath 10 份 / writeTextFile 6 份 …，同一语义还有多种入参
// 签名（const char* / std::string）；口径漂移是 TIN-07 与历次 flake 的温床。
//
// 这里收七件套（header-only；tests/ 已在 target_include_directories 里，
// 无需改 tests/CMakeLists.txt）：
//   1) 环境变量作用域 —— setEnvVar（两种入参签名）+ ScopedEnv（RAII 还原）
//   2) 临时目录       —— makeTempDir（一次性）+ TempDir（RAII，析构删）
//   3) 数据根隔离     —— AppDataEnv（APPDATA / HOME 重定向到临时目录）
//   4) 应用单例作用域 —— ScopedAppRouter
//   5) 已 migrate 的临时库 —— tempDbPath / TestDb / TestEnv（TIN-06b）
//   6) 笔记无头环境   —— MdHeadlessEnv（笔记目录 + 库 + appDatabase + 环境隔离）
//   7) 小工具         —— writeTextFile / tempConfigFile / cleanupProfileKey /
//                        HeaderMap + headerCI + countCI（TIN-06b）
//
// 迁移约定：只删本地重复脚手架 + 改 include/调用，**不动用例语义**；
// 收编时每个分支逐字保留原样（见各函数上的「原 XXX 同款」出处注释）。
//
// ==================== 失败口径（TIN-06b 统一决策）====================
// **构造失败即抛**（throw std::runtime_error）。存量两派 —— TestDb 派构造失败
// 就抛；TestEnv 派设 ready=false 不抛、由用例 DOCTEST_REQUIRE(env.ready) 判红。
// 统一到「抛」，理由：
//   1) 夹具没建成 = 用例后续断言必然连锁失真，fail-fast 符合本仓库 doctest 的
//      既有约定（doctest.h 头注释「断言三条」第 2 条）；
//   2) 抛是构造器唯一诚实的出口：不抛就得给对象一个「存在但不可用」态，
//      调用方要逐处记得查 ready，漏查即崩在半初始化的对象上；
//   3) 与首批收编的 TempDir 一致（其构造失败即 throw std::runtime_error）；
//   4) 记账等价 —— test_main 逐用例 catch（test_main.cpp「单个测试的异常…
//      不炸掉整个套件」段）：构造抛异常与该用例 DOCTEST_REQUIRE 判红**同样记
//      一条 Failed**，只是失败机制从「断言 false」换成「未捕获异常」，
//      PASS/FAIL 结果不变（受影响的四个文件：test_home / test_todo_service /
//      test_todo_ui / test_theme_hierarchy，本批已复跑全绿）。
// 兼容：TestEnv 保留 ready 成员（构造成功恒 true），
// 既有 `DOCTEST_REQUIRE(env.ready)` 断言行不需要改动。
#pragma once

#include "doctest.h" // test_temp::tempPath —— 临时路径唯一化（pid + 进程内序号）

#include "core/database.h" // TestDb / TestEnv / MdHeadlessEnv
#include "core/router.h"   // ScopedAppRouter
#include "platform/keychain.h" // cleanupProfileKey

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

// ==================== 5) 已 migrate 的临时数据库（TIN-06b）====================
// 临时 db 路径工厂（只造路径、不落盘，调用方自己 open）。原 test_migrations /
// test_claude_service / test_claude_ui / test_home / test_provider_config /
// test_proxy_router / test_settings_sync / test_theme_hierarchy /
// test_todo_service / test_todo_ui 十份同款工厂合流——原先每份各带自己的
// static counter，合流后共用一个计数；路径唯一性由 test_temp::tempPath 的
// pid + 进程内序号兜底，文件名不参与任何断言。
inline std::filesystem::path tempDbPath(const std::string& prefix,
                                        const std::string& tag) {
    static int counter = 0;
    ++counter;
    return test_temp::tempPath(prefix + tag + "_" + std::to_string(counter) +
                               ".db");
}

// RAII：已 migrate 的临时数据库（原 test_claude_service / test_provider_config /
// test_proxy_router / test_settings_sync 四份 TestDb 合流）。构造失败即抛
// （见文件头「失败口径」）；析构删库文件与 sqlite 的 -wal / -shm 伴生文件。
struct TestDb {
    std::filesystem::path path;
    // 审计 CP-07：Database/SqliteDb 已不可默认构造（open() 是唯一入口），
    // 「先占位、后 move 进来」改用指针表达
    std::unique_ptr<core::Database> db;

    TestDb(const std::string& prefix, const std::string& tag) {
        path = tempDbPath(prefix, tag);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        auto r = core::Database::open(path);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::Database>(std::move(r.value()));
        auto m = db->migrate();
        if (m.isFail()) {
            throw std::runtime_error("migrate failed: " + m.errorMessage());
        }
    }
    ~TestDb() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
    TestDb(const TestDb&) = delete;
    TestDb& operator=(const TestDb&) = delete;

    core::data::SqliteDb& handle() { return db->db(); }
};

// RAII：已 migrate 的临时库 +（可选）注册为进程级 appDatabase——模块 onStart
// 依赖 appDatabase 的用法传 registerAppDb = true（原 test_home /
// test_todo_service / test_todo_ui 的 TestEnv 与 test_theme_hierarchy 的 DbEnv
// 同款）。析构顺序：本类先注销 appDatabase，再轮到基类销毁库与删文件。
struct TestEnv : TestDb {
    // 兼容既有 `DOCTEST_REQUIRE(env.ready)` 断言行：构造成功恒为 true
    //（构造失败在构造期已抛，不再存在「对象在、ready=false」态）
    bool ready = true;
    bool appDbRegistered = false;

    TestEnv(const std::string& prefix, const std::string& tag,
            bool registerAppDb = false)
        : TestDb(prefix, tag), appDbRegistered(registerAppDb) {
        if (appDbRegistered) {
            ::core::registerAppDatabase(&db->db());
        }
    }
    ~TestEnv() {
        if (appDbRegistered) {
            ::core::registerAppDatabase(nullptr);
        }
    }
    TestEnv(const TestEnv&) = delete;
    TestEnv& operator=(const TestEnv&) = delete;
};

// ==================== 6) 笔记无头环境（TIN-06b）====================
// RAII：临时笔记目录 + 目录内已 migrate 的临时库（注册为 appDatabase），并把
// 视图状态文件 / 笔记目录两个环境变量指到该临时目录，析构撤销环境变量 + 删
// 整棵目录。原 test_markdown.cpp 的 MdUiEnv / test_md_highlight.cpp 的 MdHlEnv
// / test_md_keyboard.cpp 的 MdKbEnv 三份合流——三份 body 一字不差，只有临时目录
// 前缀与库文件名不同（前缀保留，便于辨认残骸是谁留下的）。构造失败即抛。
struct MdHeadlessEnv {
    std::filesystem::path dir;
    std::unique_ptr<core::Database> db;

    MdHeadlessEnv(const std::string& dirPrefix, const std::string& tag,
                  const std::string& dbName) {
        dir = makeTempDir(dirPrefix, tag);
        auto r = core::Database::open(dir / dbName);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::Database>(std::move(r.value()));
        if (db->migrate().isFail()) {
            throw std::runtime_error("migrate failed");
        }
        ::core::registerAppDatabase(&db->db());
        // 视图状态文件隔离（审计 [23]）：无头用例不碰真实 %APPDATA% 状态
        setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH",
                  (dir / "view_state.json").string());
        // 笔记目录隔离：只对本用例生效
        setEnvVar("DEVDESK_NOTES_DIR", dir.string());
    }

    ~MdHeadlessEnv() {
        setEnvVar("DEVDESK_NOTES_DIR", "");
        setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH", "");
        ::core::registerAppDatabase(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    MdHeadlessEnv(const MdHeadlessEnv&) = delete;
    MdHeadlessEnv& operator=(const MdHeadlessEnv&) = delete;
};

// ==================== 7) 小工具（TIN-06b）====================
// 写文本文件（覆盖；自动建父目录——支持子目录笔记）；返回 false = 建不了 / 写
// 失败。原 test_markdown.cpp / test_md_highlight.cpp / test_md_keyboard.cpp /
// test_markdown_service.cpp / test_file_watcher.cpp 的 writeTextFile 与
// test_md_file_open.cpp 的 writeText 六份合流（后一份用流 `<<` 写入且不先建父
// 目录，对既有调用点行为等价）。
inline bool writeTextFile(const std::filesystem::path& path,
                          const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

// 配置文件临时路径（只造路径、不落盘、不自动清理）。原 test_app_sidebar.cpp /
// test_module_gating.cpp / test_settings.cpp 三份 tempConfigFile 合流，只有
// 前缀不同（前缀保留）。
inline std::filesystem::path tempConfigFile(const std::string& prefix,
                                            const std::string& name) {
    return test_temp::tempPath(prefix + name + ".json");
}

// 清掉某个测试 Keychain service 下某 profile 的密钥（原 test_claude_service.cpp
// / test_claude_ui.cpp 两份 cleanupKey 合流；service 由调用方传自身的测试常量）
inline void cleanupProfileKey(const char* service, int64_t profileId) {
    platform::keychain::deleteSecret(service,
                                     "claude:" + std::to_string(profileId));
}

// 头名大小写不敏感取头 / 计数（转发后头名拼写由方言层决定，可能与客户端不同）。
// 原 test_claude_service.cpp / test_proxy_router.cpp / test_upstream_dialect.cpp
// 三份同款合流。别名供 REGISTER_TEST body 使用——宏大括号不保护逗号，带逗号的
// 类型名不能直接写在用例体里。
using HeaderMap = std::map<std::string, std::string>;

inline std::string headerCI(const HeaderMap& h, const std::string& lowerKey) {
    for (const auto& kv : h) {
        std::string k = kv.first;
        for (auto& c : k) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (k == lowerKey) return kv.second;
    }
    return std::string();
}

// 统计同名头（大小写不敏感）出现次数：>1 说明大小写变体并存（上游会收到重复头）
inline int countCI(const HeaderMap& h, const std::string& lowerKey) {
    int n = 0;
    for (const auto& kv : h) {
        std::string k = kv.first;
        for (auto& c : k) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (k == lowerKey) ++n;
    }
    return n;
}

} // namespace test_support
