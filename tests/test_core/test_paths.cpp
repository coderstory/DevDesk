#include "doctest.h"
#include "test_support.h"
#include "core/paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

// ---- 审计 CP-04：单一数据根 DEVDESK_DATA_ROOT 的断言辅助 ----
// 统一分隔符 + 词法归一（Windows 上 '/' 与 '\' 混写会让朴素字符串前缀判定
// 假阴性），再判前缀
std::string normPath(const std::filesystem::path& p) {
    return p.lexically_normal().generic_string();
}

// p 落在 root 之下（或就是 root 本身）
bool isUnderOrEqual(const std::filesystem::path& p,
                    const std::filesystem::path& root) {
    std::string s = normPath(p);
    std::string r = normPath(root);
    if (!r.empty() && r.back() == '/') {
        r.pop_back();
    }
    if (s == r) {
        return true;
    }
    return s.size() > r.size() && s.compare(0, r.size(), r) == 0 &&
           s[r.size()] == '/';
}

// 平台「用户主目录」环境变量名（与实现同源：Windows=USERPROFILE，其余=HOME；
// 见 paths_win.cpp / paths_macos.cpp）。写成函数是因为 REGISTER_TEST 的用例体
// 是宏实参——#if 直接写进宏实参是 UB（-Wembedded-directive，本仓库 -Werror 拦停）
const char* homeEnvNameForTest() {
#if defined(_WIN32)
    return "USERPROFILE";
#else
    return "HOME";
#endif
}

// 旧（系统惯例）数据根：macOS/Linux = <home>/Library/Application Support/DevDesk
// 与 <home>/.local/share/DevDesk，Windows = <roving>/DevDesk（%APPDATA%）。
// 注意 macOS 的旧根挂在 home 下、Windows 的旧根挂在 APPDATA 下——两个入参都传，
// 各平台只用自己的那个
std::filesystem::path legacyDefaultAppDataDirForTest(
    const std::filesystem::path& home, const std::filesystem::path& roving) {
#if defined(_WIN32)
    (void)home;
    return roving / "DevDesk";
#else
    (void)roving;
    return home / "Library" / "Application Support" / "DevDesk";
#endif
}

// 「全部应用数据路径」派生清单逐条断言：新增一条数据路径时在这里登记——
// 漏登记等于新路径不受数据根管辖，正是本审计项要堵的缝
bool allDataPathsUnder(const std::filesystem::path& root) {
    DOCTEST_CHECK(isUnderOrEqual(core::paths::appDataDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::configFile(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::markdownDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::petsDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::sqliteDbFile(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::logsDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::logFile(), root));
    return true;
}

} // namespace

REGISTER_TEST(paths_app_data_dir, "paths: appDataDir not empty", {
    auto p = core::paths::appDataDir();
    DOCTEST_CHECK_FALSE(p.empty());
    DOCTEST_CHECK(p.string().find("DevDesk") != std::string::npos);
    return true;
})

REGISTER_TEST(paths_config_file, "paths: configFile in appDataDir", {
    auto root = core::paths::appDataDir();
    auto cfg = core::paths::configFile();
    // 简单检查 cfg 路径以 root 开头（macOS equivalent 不支持）
    auto cfg_str = cfg.string();
    auto root_str = root.string();
    DOCTEST_CHECK(cfg_str.find(root_str) == 0);
    DOCTEST_CHECK(cfg_str.find("config.json") != std::string::npos);
    return true;
})

REGISTER_TEST(paths_sqlite_ext, "paths: sqliteDbFile is .db", {
    auto f = core::paths::sqliteDbFile();
    DOCTEST_CHECK_EQ(f.extension().string(), std::string(".db"));
    return true;
})

REGISTER_TEST(paths_log_file, "paths: logFile in logs dir", {
    auto lf = core::paths::logFile();
    DOCTEST_CHECK(lf.string().find("logs") != std::string::npos);
    return true;
})

// 审计 CP-09：DEVDESK_CLAUDE_CONFIG_DIR 覆盖分支已下沉到 paths.cpp 的
// claudeConfigDirOverride()，三平台 claudeSettingsFile() 共用——本用例锁死
// 设（覆盖生效）/ 清（回落平台 home 默认）两分支的行为，防下沉改坏语义。
REGISTER_TEST(paths_claude_settings_dir_override,
              "paths: DEVDESK_CLAUDE_CONFIG_DIR 覆盖 claudeSettingsFile（设/清两分支）", {
    // 备份原值（外部环境可能已设；用例末尾恢复，防污染后续用例）
    const char* saved = std::getenv("DEVDESK_CLAUDE_CONFIG_DIR");
    const std::string savedValue = saved ? saved : std::string();

    // 分支 1：环境变量已设 → <env>/settings.json
    const auto overrideDir = test_temp::tempPath("devdesk_claude_cfg_override");
    test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", overrideDir.string());
    const auto overridden = core::paths::claudeSettingsFile();
    DOCTEST_CHECK(overridden == overrideDir / "settings.json");

    // 分支 2：清掉（空串=删除变量）→ 回落平台 home 默认：~/.claude/settings.json
    test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
    const auto fallback = core::paths::claudeSettingsFile();
    DOCTEST_CHECK(fallback.string().find(overrideDir.string()) == std::string::npos);
    DOCTEST_CHECK(fallback.string().find(".claude") != std::string::npos);
    DOCTEST_CHECK(fallback.filename().string() == std::string("settings.json"));

    // 恢复原值
    test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", savedValue);
    return true;
})

// 平台 API 真机断言（Windows 走 GetModuleFileNameW；macOS/POSIX 同语义）：
// 失败时必须返回空 path，绝不用 cwd 冒充 —— 所以"非空 + 绝对 + 存在"三条
// 都断言；再用"目录里有测试二进制本体"反证返回的不是别的目录
REGISTER_TEST(paths_executable_dir, "paths: executableDir = 当前可执行文件所在目录", {
    auto dir = core::paths::executableDir();
    DOCTEST_CHECK_FALSE(dir.empty());
    DOCTEST_CHECK(dir.is_absolute());
    std::error_code ec;
    DOCTEST_CHECK(std::filesystem::is_directory(dir, ec) && !ec);
    // TIN-08 后宏体走 __VA_ARGS__，声明列表里的逗号可直接写
    bool foundSelf = false;
    std::filesystem::directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().stem().string() == "devdesk_tests") {
            foundSelf = true;
            break;
        }
    }
    DOCTEST_CHECK(foundSelf);
    return true;
})

// ==================== 审计 CP-04：单一数据根 DEVDESK_DATA_ROOT ====================
// 旧形态：configFile / markdownDir / petsDir 各自读自己的环境变量，隔离缝散落
// 三层且无中心入口——漏设一个就静默读写真实 %APPDATA%（污染真实用户数据）。
// 新契约：只设 DEVDESK_DATA_ROOT 一处，全部应用数据路径都由它派生。

// ① 只设数据根 → 全部派生路径落在其下
REGISTER_TEST(paths_data_root_derives_all,
              "paths: DEVDESK_DATA_ROOT 单一入口派生全部数据路径（CP-04）", {
    // 自足隔离：细粒度单项覆盖一律清掉（空串 = 删除变量），只留数据根
    const test_support::ScopedEnv cfgEnv("DEVDESK_CONFIG_PATH", "");
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", "");
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", "");

    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());

    // 先证「覆盖真的生效」并与平台默认根不同——否则变量漏设时下面每条断言
    // 都还成立（用例会在真实 %APPDATA% 上假绿），这正是要防的静默
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(tempRoot));
    DOCTEST_CHECK_NE(normPath(core::paths::appDataDir()),
                     normPath(core::paths::platformDefaultAppDataDir()));

    DOCTEST_CHECK(allDataPathsUnder(tempRoot));
    // 派生的是「根下的固定名」，不是等于根本身（防止只把 appDataDir 换了、
    // 别的路径仍指向真实目录这种半吊子改动）
    DOCTEST_CHECK_EQ(normPath(core::paths::configFile()),
                     normPath(tempRoot / "config.json"));
    DOCTEST_CHECK_EQ(normPath(core::paths::markdownDir()),
                     normPath(tempRoot / "notes"));
    DOCTEST_CHECK_EQ(normPath(core::paths::petsDir()),
                     normPath(tempRoot / "pets"));
    DOCTEST_CHECK_EQ(normPath(core::paths::sqliteDbFile()),
                     normPath(tempRoot / "devdesk.db"));
    DOCTEST_CHECK_EQ(normPath(core::paths::logsDir()),
                     normPath(tempRoot / "logs"));
    DOCTEST_CHECK_EQ(normPath(core::paths::logFile()),
                     normPath(tempRoot / "logs" / "devdesk.log"));
    return true;
})

// ② 不设数据根 → 落平台真实默认根（本用例自身就是「没设」与「设了」的区分器：
// 两条路径走不同断言分支，只有两者结果确实不同才都成立）
REGISTER_TEST(paths_data_root_absent_uses_platform_default,
              "paths: 未设 DEVDESK_DATA_ROOT 时落平台默认根（CP-04）", {
    const test_support::ScopedEnv clearRoot("DEVDESK_DATA_ROOT", "");
    const test_support::ScopedEnv cfgEnv("DEVDESK_CONFIG_PATH", "");
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", "");
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", "");

    const std::filesystem::path def = core::paths::platformDefaultAppDataDir();
    DOCTEST_CHECK_FALSE(def.empty());
    DOCTEST_CHECK(def.string().find("DevDesk") != std::string::npos);
    // 没设 → 恰好是平台默认根（不是任何临时目录、不是空 path）
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(def));
    DOCTEST_CHECK(allDataPathsUnder(def));

    // 「设了」的一侧（同进程内对照）：结果必须与默认根不同
    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root_absent");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());
    DOCTEST_CHECK_NE(normPath(core::paths::appDataDir()), normPath(def));
    DOCTEST_CHECK(allDataPathsUnder(tempRoot));
    return true;
})

// ③ 覆盖优先级三档：细粒度单项 > DEVDESK_DATA_ROOT > 平台默认
// 单项覆盖保留是向后兼容（既有测试按老变量隔离单条路径），但不能因此让数据根
// 失效——这里是两档的边界
REGISTER_TEST(paths_override_precedence,
              "paths: 覆盖优先级 单项 > DEVDESK_DATA_ROOT > 平台默认（CP-04）", {
    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root_prec");
    const std::filesystem::path notes =
        test_temp::tempPath("devdesk_test_notes_prec");
    const std::filesystem::path config =
        test_temp::tempPath("devdesk_test_config_prec");
    const std::filesystem::path pets = test_temp::tempPath("devdesk_test_pets_prec");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", notes.string());
    const test_support::ScopedEnv cfgEnv("DEVDESK_CONFIG_PATH", config.string());
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", pets.string());

    // 单项覆盖压过数据根
    DOCTEST_CHECK_EQ(normPath(core::paths::markdownDir()), normPath(notes));
    DOCTEST_CHECK_EQ(normPath(core::paths::configFile()), normPath(config));
    DOCTEST_CHECK_EQ(normPath(core::paths::petsDir()), normPath(pets));
    // 没有单项覆盖的路径照常跟数据根
    DOCTEST_CHECK(isUnderOrEqual(core::paths::appDataDir(), tempRoot));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::sqliteDbFile(), tempRoot));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::logsDir(), tempRoot));

    // 清掉单项覆盖 → 三条立即回落到数据根派生（三档顺序里的第二档）
    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    test_support::setEnvVar("DEVDESK_CONFIG_PATH", "");
    test_support::setEnvVar("DEVDESK_PETS_DIR", "");
    DOCTEST_CHECK(allDataPathsUnder(tempRoot));
    return true;
})

// ④ 刻意排除项（边界锁定，不是遗漏）：Claude Code 用户级 settings.json 是
// 外部集成点——数据根若把它改到 <根>/.claude，接管功能就失去意义（Claude Code
// 读的是 ~/.claude）。这一路隔离仍走 DEVDESK_CLAUDE_CONFIG_DIR。
REGISTER_TEST(paths_claude_config_outside_data_root,
              "paths: claudeSettingsFile 不随 DEVDESK_DATA_ROOT 迁移（CP-04）", {
    const test_support::ScopedEnv claudeEnv("DEVDESK_CLAUDE_CONFIG_DIR", "");
    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root_claude");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());

    const std::filesystem::path claude = core::paths::claudeSettingsFile();
    DOCTEST_CHECK_FALSE(isUnderOrEqual(claude, tempRoot));
    DOCTEST_CHECK_EQ(claude.filename().string(), std::string("settings.json"));
    DOCTEST_CHECK(claude.string().find(".claude") != std::string::npos);
    // 数据根在同一进程内确实生效（排除「只是没读环境变量」的假象）
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(tempRoot));
    return true;
})

// ⑤ 平台默认根 = <用户主目录>/DevDesk（2026-09-28 决策：不再用系统惯例位置）。
// 断言重点在「恰好主目录下一级」——防的就是回落路径里还留着
// Library/Application Support 或 AppData/Roaming 中间层这类半吊子改动。
// 主目录变量与实现同源（macOS/POSIX 是 HOME、Windows 是 USERPROFILE），
// 故三平台共用同一套断言（与 test_support::AppDataEnv 的隔离口径一致）。
REGISTER_TEST(paths_default_root_home_devdesk,
              "paths: 平台默认根 = <用户主目录>/DevDesk（不再用系统惯例位置）", {
    const char* homeEnv = std::getenv(homeEnvNameForTest());
    DOCTEST_REQUIRE(homeEnv != nullptr);
    const std::filesystem::path home(homeEnv);
    const std::filesystem::path def = core::paths::platformDefaultAppDataDir();
    DOCTEST_CHECK_EQ(normPath(def), normPath(home / "DevDesk"));
    DOCTEST_CHECK_EQ(def.filename().string(), std::string("DevDesk"));
    // 恰好一级：parent == 用户主目录（不是 <home>/Library/Application Support 等）
    DOCTEST_CHECK_EQ(normPath(def.parent_path()), normPath(home));
    return true;
})

// ⑥ 新旧根互不影响（本版不做迁移，2026-09-28 用户决策）：新根不存在时
// appDataDir() 直接返回新路径（纯路径解析、不创建目录）；旧（系统惯例）位置
// 即便有数据也不被读取、不被删除。全程用临时「用户主目录」隔离——绝不碰
// 真实 ~/DevDesk，也绝不碰真实旧位置（Windows 旧根来源 APPDATA 同样指到临时目录）。
REGISTER_TEST(paths_default_root_no_legacy_touch,
              "paths: 默认根=<home>/DevDesk：不创建旧位置、不读不删旧数据", {
    const test_support::TempDir fakeHome("paths_fake_home");
    const test_support::TempDir fakeRoving("paths_fake_roving"); // Windows 旧根所在
    // 数据根与细粒度单项覆盖一律清空：只留平台默认根这一档
    const test_support::ScopedEnv rootEnv("DEVDESK_DATA_ROOT", "");
    const test_support::ScopedEnv cfgEnv("DEVDESK_CONFIG_PATH", "");
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", "");
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", "");
    // 用户主目录与（Windows 旧根来源）APPDATA 都指到临时目录；非 Windows 上
    // APPDATA 无消费者，设了也无害
    const test_support::ScopedEnv homeEnv(homeEnvNameForTest(),
                                          fakeHome.path.string());
    const test_support::ScopedEnv rovingEnv("APPDATA", fakeRoving.path.string());
    const std::filesystem::path legacy =
        legacyDefaultAppDataDirForTest(fakeHome.path, fakeRoving.path);
    const std::filesystem::path newRoot = fakeHome.path / "DevDesk";

    // 新根不存在 → appDataDir()/configFile() 仍返回新路径（纯解析，不要求存在，
    // 也不产生副作用：新旧位置都还没被创建）
    std::error_code ec;
    DOCTEST_CHECK_FALSE(std::filesystem::exists(newRoot, ec));
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(newRoot));
    DOCTEST_CHECK_EQ(normPath(core::paths::configFile()),
                     normPath(newRoot / "config.json"));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(newRoot, ec));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(legacy, ec));

    // 旧位置种一份「像用户数据」的文件（config + 笔记）：新根路径的解析与建目录
    // 都不读它、不删它（迁移不做，旧数据原样留在原处）
    DOCTEST_CHECK(test_support::writeTextFile(legacy / "config.json",
                                              "{\"legacy\":true}"));
    DOCTEST_CHECK(test_support::writeTextFile(legacy / "notes" / "old.md",
                                              "# old note"));
    DOCTEST_CHECK_EQ(normPath(core::paths::configFile()),
                     normPath(newRoot / "config.json"));
    core::paths::ensureDirectoriesExist(); // 建的是新根，不是旧位置
    DOCTEST_CHECK(std::filesystem::exists(newRoot, ec));
    DOCTEST_CHECK(std::filesystem::exists(legacy, ec));
    DOCTEST_CHECK_EQ(fakeHome.read(legacy / "config.json"),
                     std::string("{\"legacy\":true}"));
    DOCTEST_CHECK_EQ(fakeHome.read(legacy / "notes" / "old.md"),
                     std::string("# old note"));
    return true;
})