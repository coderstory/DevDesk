#include "doctest.h"
#include "core/paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

// 环境变量通道（与 test_main.cpp 同款：Windows 传空串即删除变量，POSIX 空串走 unsetenv）
void setEnvVar(const char* key, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(key, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(key);
    } else {
        ::setenv(key, value.c_str(), 1);
    }
#endif
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
    setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", overrideDir.string());
    const auto overridden = core::paths::claudeSettingsFile();
    DOCTEST_CHECK(overridden == overrideDir / "settings.json");

    // 分支 2：清掉（空串=删除变量）→ 回落平台 home 默认：~/.claude/settings.json
    setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
    const auto fallback = core::paths::claudeSettingsFile();
    DOCTEST_CHECK(fallback.string().find(overrideDir.string()) == std::string::npos);
    DOCTEST_CHECK(fallback.string().find(".claude") != std::string::npos);
    DOCTEST_CHECK(fallback.filename().string() == std::string("settings.json"));

    // 恢复原值
    setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", savedValue);
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
    // 注意：本宏体内不能出现裸逗号（预处理器按逗号切分实参），迭代器分两句声明
    bool foundSelf = false;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().stem().string() == "devdesk_tests") {
            foundSelf = true;
            break;
        }
    }
    DOCTEST_CHECK(foundSelf);
    return true;
})