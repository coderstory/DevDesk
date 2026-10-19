#include "doctest.h"
#include "test_support.h"
#include "core/paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

// 统一分隔符 + 词法归一：Windows 上 '/' 与 '\' 混写会让朴素字符串前缀判定假阴性
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

// 与实现同源：Windows=USERPROFILE，其余=HOME（见 paths_win.cpp / paths_macos.cpp）。
// 写成函数是因为 REGISTER_TEST 的用例体是宏实参——#if 直接写进宏实参是 UB，
// -Wembedded-directive 在本仓库 -Werror 拦停
const char* homeEnvNameForTest() {
#if defined(_WIN32)
    return "USERPROFILE";
#else
    return "HOME";
#endif
}

// 旧（系统惯例）数据根。macOS 的旧根挂在 home 下，Windows 的旧根挂在 APPDATA 下——
// 两个入参都传，各平台只用自己的那个
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

// 「全部应用数据路径」派生清单逐条断言：新增一条数据路径时必须在这里登记，漏登记等于
// 新路径不受数据根管辖
bool allDataPathsUnder(const std::filesystem::path& root) {
    DOCTEST_CHECK(isUnderOrEqual(core::paths::appDataDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::markdownDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::sqliteDbFile(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::logsDir(), root));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::logFile(), root));
    return true;
}

} // namespace

REGISTER_TEST(paths_app_data_dir, "paths: appDataDir not empty", {
    auto p = core::paths::appDataDir();
    DOCTEST_CHECK_FALSE(p.empty());
    // DEVDESK_DATA_ROOT 覆盖时路径不含 "DevDesk"（测试隔离根在构建树内），
    // 只断言非空即可（不落盘环境可能把值改写成 temp 路径）
    if (!std::getenv("DEVDESK_DATA_ROOT")) {
        DOCTEST_CHECK(p.string().find("DevDesk") != std::string::npos);
    }
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

// DEVDESK_CLAUDE_CONFIG_DIR 的覆盖判定集中在 claudeConfigDirOverride()，三平台的
// claudeSettingsFile() 都走它，所以设（覆盖生效）与清（回落平台 home 默认）两个分支
// 必须同时被锁住
REGISTER_TEST(paths_claude_settings_dir_override,
              "paths: DEVDESK_CLAUDE_CONFIG_DIR 覆盖 claudeSettingsFile（设/清两分支）", {
    // 备份原值：外部环境可能已设，用例末尾必须恢复，否则污染后续用例
    const char* saved = std::getenv("DEVDESK_CLAUDE_CONFIG_DIR");
    const std::string savedValue = saved ? saved : std::string();

    const auto overrideDir = test_temp::tempPath("devdesk_claude_cfg_override");
    test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", overrideDir.string());
    const auto overridden = core::paths::claudeSettingsFile();
    DOCTEST_CHECK(overridden == overrideDir / "settings.json");

    // 清掉变量回落平台 home 默认：setEnvVar 传空串 = 删除该变量
    test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
    const auto fallback = core::paths::claudeSettingsFile();
    DOCTEST_CHECK(fallback.string().find(overrideDir.string()) == std::string::npos);
    DOCTEST_CHECK(fallback.string().find(".claude") != std::string::npos);
    DOCTEST_CHECK(fallback.filename().string() == std::string("settings.json"));

    test_support::setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", savedValue);
    return true;
})

// 取不到可执行文件路径时实现返回空 path，绝不用 cwd 冒充，所以「非空 + 绝对 + 存在」
// 三条都断言；再用「目录里有测试二进制本体」反证返回的不是别的目录
REGISTER_TEST(paths_executable_dir, "paths: executableDir = 当前可执行文件所在目录", {
    auto dir = core::paths::executableDir();
    DOCTEST_CHECK_FALSE(dir.empty());
    DOCTEST_CHECK(dir.is_absolute());
    std::error_code ec;
    DOCTEST_CHECK(std::filesystem::is_directory(dir, ec) && !ec);
    // REGISTER_TEST 宏体走 __VA_ARGS__，声明列表里的逗号可以直接写
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

// 单一数据根契约：只设 DEVDESK_DATA_ROOT 一处，全部应用数据路径都由它派生。各路径不再
// 各自读自己的环境变量，否则漏设一个就会静默读写真实用户数据目录。

REGISTER_TEST(paths_data_root_derives_all,
              "paths: DEVDESK_DATA_ROOT 单一入口派生全部数据路径（CP-04）", {
    // 自足隔离：细粒度单项覆盖一律清掉，只留数据根这一档
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", "");
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", "");

    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());

    // 先证「覆盖真的生效」并与平台默认根不同：否则变量漏设时下面每条断言都还成立，用例会在
    // 真实用户数据目录上假绿
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(tempRoot));
    DOCTEST_CHECK_NE(normPath(core::paths::appDataDir()),
                     normPath(core::paths::platformDefaultAppDataDir()));

    DOCTEST_CHECK(allDataPathsUnder(tempRoot));
    // 派生的是「根下的固定名」而非等于根本身：防止只把 appDataDir 换了、别的路径仍指向真实目录
    DOCTEST_CHECK_EQ(normPath(core::paths::markdownDir()),
                     normPath(tempRoot / "notes"));
    DOCTEST_CHECK_EQ(normPath(core::paths::sqliteDbFile()),
                     normPath(tempRoot / "devdesk.db"));
    DOCTEST_CHECK_EQ(normPath(core::paths::logsDir()),
                     normPath(tempRoot / "logs"));
    DOCTEST_CHECK_EQ(normPath(core::paths::logFile()),
                     normPath(tempRoot / "logs" / "devdesk.log"));
    return true;
})

// 不设数据根时落平台真实默认根。本用例同时充当「没设」与「设了」的区分器：只有两种
// 设置的结果确实不同，两条断言分支才都成立
REGISTER_TEST(paths_data_root_absent_uses_platform_default,
              "paths: 未设 DEVDESK_DATA_ROOT 时落平台默认根（CP-04）", {
    const test_support::ScopedEnv clearRoot("DEVDESK_DATA_ROOT", "");
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", "");
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", "");

    const std::filesystem::path def = core::paths::platformDefaultAppDataDir();
    DOCTEST_CHECK_FALSE(def.empty());
    DOCTEST_CHECK(def.string().find("DevDesk") != std::string::npos);
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(def));
    DOCTEST_CHECK(allDataPathsUnder(def));

    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root_absent");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());
    DOCTEST_CHECK_NE(normPath(core::paths::appDataDir()), normPath(def));
    DOCTEST_CHECK(allDataPathsUnder(tempRoot));
    return true;
})

// 覆盖优先级三档：细粒度单项 > DEVDESK_DATA_ROOT > 平台默认。单项覆盖保留是为向后兼容
// （既有测试按老变量隔离单条路径），但不能因此让数据根失效——这里是两档的边界
REGISTER_TEST(paths_override_precedence,
              "paths: 覆盖优先级 单项 > DEVDESK_DATA_ROOT > 平台默认（CP-04）", {
    const std::filesystem::path tempRoot =
        test_temp::tempPath("devdesk_test_data_root_prec");
    const std::filesystem::path notes =
        test_temp::tempPath("devdesk_test_notes_prec");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           tempRoot.string());
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", notes.string());

    DOCTEST_CHECK_EQ(normPath(core::paths::markdownDir()), normPath(notes));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::appDataDir(), tempRoot));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::sqliteDbFile(), tempRoot));
    DOCTEST_CHECK(isUnderOrEqual(core::paths::logsDir(), tempRoot));

    test_support::setEnvVar("DEVDESK_NOTES_DIR", "");
    DOCTEST_CHECK(allDataPathsUnder(tempRoot));
    return true;
})

// 刻意排除项，不是遗漏：Claude Code 用户级 settings.json 是外部集成点，数据根若把它
// 改到 <根>/.claude 接管功能就失去意义（Claude Code 读的是 ~/.claude）。这一路隔离
// 仍走 DEVDESK_CLAUDE_CONFIG_DIR
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
    // 数据根在同一进程内确实生效，排除「只是没读环境变量」的假象
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(tempRoot));
    return true;
})

// 平台默认根 = <用户主目录>/DevDesk，不用系统惯例位置。断言重点在「恰好主目录下一级」：
// 防的是回落路径里还留着 Library/Application Support 或 AppData/Roaming 中间层这类
// 半吊子改动。主目录变量与实现同源，故三平台共用同一套断言
REGISTER_TEST(paths_default_root_home_devdesk,
              "paths: 平台默认根 = <用户主目录>/DevDesk（不再用系统惯例位置）", {
    const char* homeEnv = std::getenv(homeEnvNameForTest());
    DOCTEST_REQUIRE(homeEnv != nullptr);
    const std::filesystem::path home(homeEnv);
    const std::filesystem::path def = core::paths::platformDefaultAppDataDir();
    DOCTEST_CHECK_EQ(normPath(def), normPath(home / "DevDesk"));
    DOCTEST_CHECK_EQ(def.filename().string(), std::string("DevDesk"));
    // 恰好一级：parent == 用户主目录，不是 <home>/Library/Application Support 这类中间层
    DOCTEST_CHECK_EQ(normPath(def.parent_path()), normPath(home));
    return true;
})

// 新旧根互不影响，本版不做迁移：新根不存在时 appDataDir() 直接返回新路径（纯路径解析、
// 不创建目录）；旧（系统惯例）位置即便有数据也不被读取、不被删除。全程用临时「用户
// 主目录」隔离——绝不碰真实 ~/DevDesk，也绝不碰真实旧位置
REGISTER_TEST(paths_default_root_no_legacy_touch,
              "paths: 默认根=<home>/DevDesk：不创建旧位置、不读不删旧数据", {
    const test_support::TempDir fakeHome("paths_fake_home");
    const test_support::TempDir fakeRoving("paths_fake_roving"); // Windows 旧根所在
    const test_support::ScopedEnv rootEnv("DEVDESK_DATA_ROOT", "");
    const test_support::ScopedEnv notesEnv("DEVDESK_NOTES_DIR", "");
    const test_support::ScopedEnv petsEnv("DEVDESK_PETS_DIR", "");
    // 用户主目录与（Windows 旧根来源）APPDATA 都指到临时目录；非 Windows 上 APPDATA 无消费者，设了也无害
    const test_support::ScopedEnv homeEnv(homeEnvNameForTest(),
                                          fakeHome.path.string());
    const test_support::ScopedEnv rovingEnv("APPDATA", fakeRoving.path.string());
    const std::filesystem::path legacy =
        legacyDefaultAppDataDirForTest(fakeHome.path, fakeRoving.path);
    const std::filesystem::path newRoot = fakeHome.path / "DevDesk";

    // 新根不存在 → appDataDir()/sqliteDbFile() 仍返回新路径：纯解析，不要求存在，也不产生副作用
    std::error_code ec;
    DOCTEST_CHECK_FALSE(std::filesystem::exists(newRoot, ec));
    DOCTEST_CHECK_EQ(normPath(core::paths::appDataDir()), normPath(newRoot));
    DOCTEST_CHECK_EQ(normPath(core::paths::sqliteDbFile()),
                     normPath(newRoot / "devdesk.db"));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(newRoot, ec));
    DOCTEST_CHECK_FALSE(std::filesystem::exists(legacy, ec));

    // 旧位置种一份「像用户数据」的文件：新根路径的解析与建目录都不读它、不删它
    DOCTEST_CHECK(test_support::writeTextFile(legacy / "config.json",
                                              "{\"legacy\":true}"));
    DOCTEST_CHECK(test_support::writeTextFile(legacy / "notes" / "old.md",
                                              "# old note"));
    DOCTEST_CHECK_EQ(normPath(core::paths::sqliteDbFile()),
                     normPath(newRoot / "devdesk.db"));
    core::paths::ensureDirectoriesExist(); // 建的是新根，不是旧位置
    DOCTEST_CHECK(std::filesystem::exists(newRoot, ec));
    DOCTEST_CHECK(std::filesystem::exists(legacy, ec));
    DOCTEST_CHECK_EQ(fakeHome.read(legacy / "config.json"),
                     std::string("{\"legacy\":true}"));
    DOCTEST_CHECK_EQ(fakeHome.read(legacy / "notes" / "old.md"),
                     std::string("# old note"));
    return true;
})

// 测试基建不变量：临时文件基目录必须是**绝对路径**。
// 为什么值得单独立一条：有断言依赖它（test_claude_service 的 run-settings 路径
// `is_absolute()`），而兜底路径曾经是相对 cwd 的 "build/testtmp" —— 同一份用例于是
// 「走脚本全绿、直接跑二进制恒红」，而门禁失败提示教的**恰好**是「直接跑二进制」，
// 一次误导就够浪费半小时。注入（DEVDESK_TEST_TMPDIR）与兜底两条路都必须绝对。
REGISTER_TEST(test_infra_temp_base_dir_is_absolute,
              "test_infra: 临时基目录恒为绝对路径（注入/兜底两条路都查）", {
    DOCTEST_CHECK(test_temp::tempBaseDir().is_absolute());
    // 注入值被**原样采用**（不静默改写）：这条一起钉住，读者才知道「绝对」这份契约
    // 为什么落在这里 —— 相对值只可能是注入方给的，兜底侧负责自己补绝对。
    if (const char* env = std::getenv("DEVDESK_TEST_TMPDIR"); env && *env) {
        DOCTEST_CHECK_EQ(test_temp::tempBaseDir().string(), std::string(env));
    }
    return true;
})