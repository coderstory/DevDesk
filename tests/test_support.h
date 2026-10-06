// tests/test_support.h
// 用例共享脚手架（header-only）：env 作用域 / 临时目录 / 数据根隔离 / 单例作用域 /
// 临时库 / 笔记无头环境 / 小工具。
// 所有夹具构造失败即抛 std::runtime_error（fail-fast，不提供 ready=false 态）。
#pragma once

#include "doctest.h" // test_temp::tempPath —— 临时路径唯一化（pid + 进程内序号）

#include "core/app_config.h"
#include "core/database.h"
#include "core/router.h"
#include "platform/json.h"
#include "platform/keychain.h"
// 键盘事件类型 —— 此 "core/" 是 EUI-NEO 的目录，与本仓库 src/core 同名
#include "core/input/input_types.h"

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
#include <utility>

namespace test_support {

// 1) 环境变量作用域
// 空值 = 删除：Windows 走 _putenv_s，POSIX 走 unsetenv ——「未设置」与「空串」
// 语义不同。两种入参签名都在用，各自保真，不要合并成一份。

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

// RAII：作用域内设置环境变量，析构还原原值（「未设置」记为空串，析构传空串即
// 删除）。用例体内 fail-fast 提前 return false 也会走析构，不泄漏给后续用例。
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

// 2) 临时目录
// 一次性临时目录：确保干净后返回路径（**不**自动清理，调用方自己管）。
// 路径唯一性由 test_temp::tempPath 的 pid + 进程内序号兜底，这里的 count
// 只维持命名形态；prefix 保留，便于辨认残骸是谁留下的。
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
// 不存在的目录上）；析构 = 整棵删掉。
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

// 3) 数据根隔离
// RAII：把应用数据根重定向到临时目录，析构恢复原值（fail-fast 早退也不影响
// 后续用例）。日志路径 core::paths::logFile() 派生自**平台用户主目录**
//（Windows: %USERPROFILE%，macOS/Linux: $HOME），故必须重定向主目录变量；
// APPDATA 一并指到临时目录。漏掉主目录会让用例把自造数据写进真实用户目录。
struct AppDataEnv {
    std::string saved;
    std::string savedHome; // 平台用户主目录：Windows=USERPROFILE、其余=HOME
    std::filesystem::path dir;

    explicit AppDataEnv(const std::string& tag) {
        const char* cur = std::getenv("APPDATA");
        saved = cur ? cur : "";
#if defined(_WIN32)
        const char* curHome = std::getenv("USERPROFILE");
#else
        const char* curHome = std::getenv("HOME");
#endif
        savedHome = curHome ? curHome : "";
        dir = test_temp::tempPath("devdesk_test_settings_appdata_" + tag);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        setEnvVar("APPDATA", dir.string());
#if defined(_WIN32)
        setEnvVar("USERPROFILE", dir.string());
#else
        setEnvVar("HOME", dir.string());
#endif
    }

    ~AppDataEnv() {
        setEnvVar("APPDATA", saved);
#if defined(_WIN32)
        setEnvVar("USERPROFILE", savedHome);
#else
        setEnvVar("HOME", savedHome);
#endif
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    AppDataEnv(const AppDataEnv&) = delete;
    AppDataEnv& operator=(const AppDataEnv&) = delete;
};

// 4) 应用单例作用域脚手架
// RAII：作用域内登记进程级 appRouter（assembleAndStart 会把 Router 注册进
// 进程级访问器，armExitHook 的 handler 经它取 Router，而用例里的 Router 是
// 栈对象），析构复位 null——否则进程退出时 atexit 会摸到已析构的 Router。
class ScopedAppRouter {
public:
    explicit ScopedAppRouter(::core::Router* router = nullptr) {
        ::core::registerAppRouter(router);
    }
    ~ScopedAppRouter() { ::core::registerAppRouter(nullptr); }
    ScopedAppRouter(const ScopedAppRouter&) = delete;
    ScopedAppRouter& operator=(const ScopedAppRouter&) = delete;
};

// RAII：作用域内登记/release 进程级 appDatabase（与 ScopedAppRouter 同构）。
// 默认构造传 nullptr = 只保留「复位」语义。
class ScopedAppDatabase {
public:
    explicit ScopedAppDatabase(core::data::SqliteDb& db) {
        ::core::registerAppDatabase(&db);
    }
    ~ScopedAppDatabase() { ::core::registerAppDatabase(nullptr); }
    ScopedAppDatabase(const ScopedAppDatabase&) = delete;
    ScopedAppDatabase& operator=(const ScopedAppDatabase&) = delete;
};

// 5) 临时数据库
// 临时 db 路径工厂（只造路径、不落盘，调用方自己 open）。共用一个 static
// counter；路径唯一性由 test_temp::tempPath 的 pid + 进程内序号兜底，
// 文件名不参与任何断言。
inline std::filesystem::path tempDbPath(const std::string& prefix,
                                        const std::string& tag) {
    static int counter = 0;
    ++counter;
    return test_temp::tempPath(prefix + tag + "_" + std::to_string(counter) +
                               ".db");
}

// RAII：DEVDESK_DATA_ROOT 重定向到临时 .data 目录（core/paths.cpp 的统一覆盖
// 入口，绝不碰真实 %APPDATA%/DevDesk）。析构删目录 + 快照还原进入前值。
// 前置：外部环境未预设非空 DEVDESK_DATA_ROOT（预设时快照还原会把它一并清掉）。
struct DataRootEnv {
    std::string dir;
    std::string prev;
    bool hadPrev = false;
    explicit DataRootEnv(const std::string& prefix, const std::string& tag) {
        dir = tempDbPath(prefix, tag).string() + ".data";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (const char* p = std::getenv("DEVDESK_DATA_ROOT")) {
            hadPrev = true;
            prev = p;
        }
        setEnvVar("DEVDESK_DATA_ROOT", dir);
        std::filesystem::create_directories(dir, ec);
    }
    ~DataRootEnv() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        setEnvVar("DEVDESK_DATA_ROOT", hadPrev ? prev : "");
    }
    DataRootEnv(const DataRootEnv&) = delete;
    DataRootEnv& operator=(const DataRootEnv&) = delete;
};

// RAII：配置存储隔离。配置的落点是 SQLite 的 app_config 表，故本夹具真正起
// 隔离作用的是它持有的那份已 migrate 的临时库（并注册为进程级 appDatabase——
// 生产代码正是经 core::appDatabase() 取连接）。
// 构造入参 cfgPath 是「临时 config.json 路径」约定的遗留形参：没有任何代码写
// 它，本夹具也不把它指给任何环境变量——保留形参只为不动调用点，析构时顺手清
// 一次同名残骸。
// 析构顺序：先注销 appDatabase，再删库文件（反过来会在句柄仍开着时删文件）。
struct ScopedConfigPath {
    std::filesystem::path cfgPath;
    std::unique_ptr<core::Database> db;
    ScopedConfigPath(std::filesystem::path p) : cfgPath(std::move(p)) {
        auto r = core::Database::open(
            test_temp::tempPath("devdesk_test_cfgstore_.db"));
        if (r.isFail()) {
            throw std::runtime_error("ScopedConfigPath: Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::Database>(std::move(r.value()));
        if (db->migrate().isFail()) {
            throw std::runtime_error("ScopedConfigPath: migrate failed");
        }
        ::core::registerAppDatabase(&db->db());
    }
    ~ScopedConfigPath() {
        ::core::registerAppDatabase(nullptr);
        std::error_code ec;
        std::filesystem::remove(cfgPath, ec);
        db.reset();
    }
    ScopedConfigPath(const ScopedConfigPath&) = delete;
    ScopedConfigPath& operator=(const ScopedConfigPath&) = delete;
};

// RAII：已 migrate 的临时数据库。构造失败即抛；析构删库文件与 sqlite 的
// -wal / -shm 伴生文件。
struct TestDb {
    std::filesystem::path path;
    // Database/SqliteDb 不可默认构造（open() 是唯一入口），故用指针表达持有
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

// RAII：SqliteDb::open(":memory:") 的内存库（供不需要 migrate 的纯内存
// 形态）。构造失败即抛；handle() 命名对齐 TestDb::handle()。
struct InMemoryDb {
    std::unique_ptr<core::data::SqliteDb> db;
    InMemoryDb() {
        auto r = core::data::SqliteDb::open(":memory:");
        if (r.isFail()) {
            throw std::runtime_error("SqliteDb::open(:memory:) failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::data::SqliteDb>(std::move(r.value()));
    }
    core::data::SqliteDb& handle() { return *db; }
};

// RAII：已 migrate 的临时库 +（可选）注册为进程级 appDatabase——模块 onStart
// 依赖 appDatabase 的用法传 registerAppDb = true。析构顺序：本类先注销
// appDatabase，再轮到基类销毁库与删文件。
struct TestEnv : TestDb {
    // 构造成功恒为 true：失败已在构造期抛出，不存在 ready=false 态
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

// path「严格位于 ancestor 之下」判定（先各自 lexically_normal，再词法前缀比对；
// child == ancestor 不算「之下」）。词法比较不解 .. 与符号链接，只适用于同根
// 无歧义的测试路径；勿用于需要对付 .. / 符号链接的生产路径校验。
inline bool isPathUnder(const std::filesystem::path& child,
                        const std::filesystem::path& ancestor) {
    std::string c = child.lexically_normal().generic_string();
    std::string a = ancestor.lexically_normal().generic_string();
    while (a.size() > 1 && a.back() == '/') a.pop_back();
    while (c.size() > 1 && c.back() == '/') c.pop_back();
    if (a == "/" ) return c.size() > 1 && c.front() == '/';
    return c.size() > a.size() + 1 && c.compare(0, a.size(), a) == 0 &&
           c[a.size()] == '/';
}

// 6) 笔记无头环境
// RAII：临时笔记目录 + **被监听目录之外**的伴生状态目录（内含已 migrate 的
// 临时库，注册为 appDatabase），并把笔记目录环境变量指到该临时目录，析构
// 撤销环境变量 + 删两棵目录。构造失败即抛。
// 库一律住 stateDir：MarkdownService 构造即对笔记目录起原生 FileWatcher
//（Windows ReadDirectoryChangesW / macOS FSEvents），而 watcher 对监听根下
// **一切**文件变化都上报（不区分扩展名）；库若建在笔记目录**内**（连同
// -wal/-shm 伴生文件），每次落库都是 watcher 事件，撞上模块层 5s 轮询节流的
// 绕过路径，用例中途 rescan+refreshList 的随机竞态。
struct MdHeadlessEnv {
    std::filesystem::path dir;       // 笔记目录（DEVDESK_NOTES_DIR；被 FileWatcher 监听）
    std::filesystem::path stateDir;  // 伴生状态目录（库；刻意在监听范围外）
    std::filesystem::path dbPath;    // 库文件绝对路径（= stateDir / dbName）
    std::unique_ptr<core::Database> db;

    MdHeadlessEnv(const std::string& dirPrefix, const std::string& tag,
                  const std::string& dbName) {
        dir = makeTempDir(dirPrefix, tag);
        stateDir = makeTempDir(dirPrefix, tag + "_state");
        // 守卫（fail-fast）：库不得落在被监听的笔记目录内
        if (isPathUnder(stateDir, dir)) {
            throw std::runtime_error(
                "MdHeadlessEnv: 伴生状态目录不得建在笔记目录（被 FileWatcher "
                "监听）内: " +
                stateDir.string() + " under " + dir.string());
        }
        dbPath = stateDir / dbName;
        auto r = core::Database::open(dbPath);
        if (r.isFail()) {
            throw std::runtime_error("Database::open failed: " +
                                     r.errorMessage());
        }
        db = std::make_unique<core::Database>(std::move(r.value()));
        if (db->migrate().isFail()) {
            throw std::runtime_error("migrate failed");
        }
        ::core::registerAppDatabase(&db->db());
        // 笔记目录隔离：只对本用例生效
        setEnvVar("DEVDESK_NOTES_DIR", dir.string());
    }

    ~MdHeadlessEnv() {
        setEnvVar("DEVDESK_NOTES_DIR", "");
        ::core::registerAppDatabase(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::remove_all(stateDir, ec);
    }
    MdHeadlessEnv(const MdHeadlessEnv&) = delete;
    MdHeadlessEnv& operator=(const MdHeadlessEnv&) = delete;
};

// 7) 小工具
// 写文本文件（覆盖；自动建父目录）；返回 false = 建不了 / 写失败。
inline bool writeTextFile(const std::filesystem::path& path,
                          const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

// 最小 PNG 头（魔数 + IHDR 尺寸 + 8bit RGBA 类型 + 填充字节）——只核头部
// 33 字节；无头用例不做解码，夹具帧只需头部合法，无需完整 IDAT/IEND。
inline void writeMinimalPng(const std::filesystem::path& path, int width,
                            int height) {
    std::ofstream out(path, std::ios::binary);
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G',
                                          0x0D, 0x0A, 0x1A, 0x0A};
    out.write(reinterpret_cast<const char*>(kSig), 8);
    const auto be32 = [](unsigned char* p, std::uint32_t v) {
        p[0] = static_cast<unsigned char>((v >> 24) & 0xFF);
        p[1] = static_cast<unsigned char>((v >> 16) & 0xFF);
        p[2] = static_cast<unsigned char>((v >> 8) & 0xFF);
        p[3] = static_cast<unsigned char>(v & 0xFF);
    };
    unsigned char ihdr[26] = {};
    be32(ihdr + 0, 13);  // IHDR 数据长度
    ihdr[4] = 'I'; ihdr[5] = 'H'; ihdr[6] = 'D'; ihdr[7] = 'R';
    be32(ihdr + 8, static_cast<std::uint32_t>(width));
    be32(ihdr + 12, static_cast<std::uint32_t>(height));
    ihdr[16] = 8;  // bit depth
    ihdr[17] = 6;  // color type RGBA
    out.write(reinterpret_cast<const char*>(ihdr), 18);
    const char pad[8] = {};  // 凑满 33 字节
    out.write(pad, sizeof(pad));
}

// 向候选根写一份 shipped 形态皮肤：pet.json + 四态 13 帧（f0..f5 / p0..p1 /
// s0..s2 / t0..t1，128×128 头部合法帧）。无头用例的 exeDir（build/tests）没有
// 随包资产拷贝，不造夹具时 ensureDefaultSkin 失败 → startPetWindow 不开窗。
inline void writeShippedSkinFixture(const std::filesystem::path& root,
                                    const char* name, const char* jsonText) {
    static const char* kFrameNames[] = {
        "f0.png", "f1.png", "f2.png", "f3.png", "f4.png", "f5.png",
        "p0.png", "p1.png", "s0.png", "s1.png", "s2.png", "t0.png", "t1.png"};
    std::error_code ec;
    std::filesystem::create_directories(root / name, ec);
    writeTextFile(root / name / "pet.json", jsonText);
    for (const char* frame : kFrameNames) {
        writeMinimalPng(root / name / frame, 128, 128);
    }
}

// 配置文件临时路径（只造路径、不落盘、不自动清理）。
inline std::filesystem::path tempConfigFile(const std::string& prefix,
                                            const std::string& name) {
    return test_temp::tempPath(prefix + name + ".json");
}

// 把某 profile 的凭据清空（造「该 profile 没存过 API Key」的形态）。凭据存
// claude_profiles.api_key_encrypted 列，清 Keychain 对它无效。
inline bool clearStoredCredential(core::data::SqliteDb& db, int64_t profileId) {
    return db.exec("UPDATE claude_profiles SET api_key_encrypted = '', "
                   "api_key_hint = '', api_key_check = '' WHERE id = " +
                   std::to_string(profileId))
        .isOk();
}

// 删某个 service 下某 profile 的 Keychain 条目；service 由调用方传自身的测试常量
inline void cleanupProfileKey(const char* service, int64_t profileId) {
    platform::keychain::deleteSecret(service,
                                     "claude:" + std::to_string(profileId));
}

// 头名大小写不敏感取头 / 计数（转发后头名拼写由方言层决定，可能与客户端不同）。
// 别名供 REGISTER_TEST body 使用——宏大括号不保护逗号，带逗号的类型名不能
// 直接写在用例体里。
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

// 9) app_config / sidecar 原文存取
// 配置落点是 app_config 表的一行，用例的「塞一段手写内容、跑一遍、再读回」
// 改成往该行塞原文。空串 = 不存在；原文按字节进出，不做任何规范化。

// 写入整份文档原文（模拟「脏数据/损坏内容」，如 "{broken"）
inline bool writeRawConfigRow(core::data::SqliteDb* db, const char* key,
                              const std::string& text) {
    if (db == nullptr) return false;
    return core::appconfig::set(*db, key, text).isOk();
}

// 读回整份文档原文；键不存在 → 空串
inline std::string readRawConfigRow(core::data::SqliteDb* db, const char* key) {
    if (db == nullptr) return std::string();
    auto v = core::appconfig::get(*db, key);
    return v.isOk() ? v.value() : std::string();
}

// 该行是否存在
inline bool configRowExists(core::data::SqliteDb* db, const char* key) {
    if (db == nullptr) return false;
    auto v = core::appconfig::has(*db, key);
    return v.isOk() && v.value();
}

// 把一段「手改过的配置 JSON」按**顶层键**分发进 app_config 表的各行（顶层键 =
// 行键，值存该键的 JSON 值文本），供「手改成未知模块 ID / 乱类型 / 半截 JSON」
// 的负例使用。顶层不是对象或解析失败 → false（调用方 DOCTEST_REQUIRE 它）。
inline bool seedConfigJsonRows(core::data::SqliteDb* db,
                               const char* keyNamespace,
                               const std::string& jsonText) {
    if (db == nullptr) return false;
    auto parsed = platform::json::parse(jsonText);
    if (parsed.isFail() || !parsed.value().isObject()) return false;
    auto obj = parsed.value().asObject();
    std::vector<core::appconfig::Entry> entries;
    for (const auto& k : obj.keys()) {
        auto text = platform::json::stringify(obj.get(k), /*pretty=*/false);
        if (text.isFail()) return false;
        entries.push_back(
            core::appconfig::Entry{std::string(keyNamespace) + k, text.value(), 0});
    }
    // **整份替换**语义：先清掉本命名空间下已有的行再写。只补不删的话，上一次
    // save 留下的键会活下来，「存量配置只有 theme → 别的字段回落默认」这类用例
    // 就会读到脏数据。两步包在同一个事务里，免得中间态被别的线程读到。
    return db->transaction([&]() -> core::Result<void> {
        auto cleared = core::appconfig::eraseWithPrefixExcept(
            *db, keyNamespace, {});
        if (cleared.isFail()) return cleared;
        return core::appconfig::setMany(*db, entries);
    }).isOk();
}

// 预置进「进程级已注册的那个连接」
//
// 上面三个 helper 收 db 句柄；探针 / 无头用例手上**没有**句柄——它们只有
// ::core::appDatabase()（夹具 ScopedConfigPath 注册进去的那个，生产模块 onStart
// 正是经它取连接）。这两个是它的「已注册连接」入口。
//
// **必须当场读回校验**：预置失败必须在写入点当场报出来（false），由调用方
// fail-fast，绝不带着没落地的配置往下跑断言——否则只会在下游症状上变红。
inline bool seedAppConfigRow(const char* key, const std::string& text) {
    auto* db = ::core::appDatabase();
    if (db == nullptr) return false;
    return writeRawConfigRow(db, key, text) && readRawConfigRow(db, key) == text;
}

// 同上，但输入是一整份 config.json 文本（按顶层键分发进该命名空间的各行）。
// 校验 = 该命名空间下**至少一行**落库且读得回（整份替换语义已清过旧行，空清单
// 就是预置没生效）。
inline bool seedAppConfigJson(const char* keyNamespace,
                              const std::string& jsonText) {
    auto* db = ::core::appDatabase();
    if (db == nullptr) return false;
    if (!seedConfigJsonRows(db, keyNamespace, jsonText)) return false;
    auto rows = core::appconfig::listWithPrefix(*db, keyNamespace);
    return rows.isOk() && !rows.value().empty();
}

// 8) 快捷键修饰键（与实现同源）
// 快捷键的「修饰位」是**平台相关**的 —— EUI-NEO core/input/input_types.h 的
// KeyModifiers::shortcut() 在 __APPLE__ 下读 super（Cmd）、其余平台读 control。
// 用例若写死 modifiers.control = true，macOS 上读到的 shortcut() 恒为 false。
//
// 做法：**不在这里重写平台分支** —— 测试与实现各写一份 #if defined(__APPLE__)
// 就是"同一个平台判断存在两份"，上游平台语义一改立刻又漂。改为把 EUI-NEO 的
// shortcut() 本身当唯一判据：它认哪个位，测试就把哪个位置位。两个单位置探针各问
// 一次，测试侧零平台宏（全仓平台判断仍只有 EUI-NEO 那一处）。
//
// 非 macOS 语义不变（Windows/Linux 路径）：只置 control 时 shortcut() 返 true
// （其实现就是 `return control`）→ 探针选中 control。等价性不靠"我记得 Windows
// 是 Ctrl"，靠的是读同一函数。
//
// 用法：新写用例一律走这两个入口，别再直接碰 modifiers.control / modifiers.super。
inline bool shortcutUsesControl() {
    core::KeyModifiers probe;
    probe.control = true;
    return probe.shortcut();
}

inline bool shortcutUsesSuper() {
    core::KeyModifiers probe;
    probe.super = true;
    return probe.shortcut();
}

// 把「该平台的快捷键修饰位」置为 on（其余位不动）。on=false 用于构造负例
// （裸键 / Release / 非快捷键组合）。
inline void setShortcutModifier(core::KeyModifiers& mods, bool on = true) {
    if (shortcutUsesControl()) mods.control = on;
    if (shortcutUsesSuper()) mods.super = on;
}

// 快捷键按下事件（key + 平台快捷键修饰位 [+ Shift]）——Ctrl/Cmd+P、
// Ctrl/Cmd+Shift+P 一族的构造口。
inline core::KeyEvent pressShortcut(core::InputKey key, bool shift = false) {
    core::KeyEvent e;
    e.key = key;
    e.action = core::KeyAction::Press;
    setShortcutModifier(e.modifiers, true);
    e.modifiers.shift = shift;
    return e;
}

} // namespace test_support
