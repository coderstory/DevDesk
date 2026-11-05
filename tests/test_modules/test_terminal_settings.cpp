// tests/test_modules/test_terminal_settings.cpp
// 终端设置纯函数 + 持久化单测（need250）：钳制/sanitize 枚举收敛/色板解析/目录有效性
// + app_config 单行 JSON 存读往返（缺键/损坏/库未就绪回退默认，等价「重启恢复」）。
#include "doctest.h"

#include "core/app_config.h"
#include "module_terminal/terminal_settings.h"
#include "test_support.h"

#include <filesystem>
#include <string>

namespace {

// 往 app_config 行里塞一段非 JSON 文本（损坏回退默认的注入器）
void writeGarbageRow(core::data::SqliteDb& db, const char* key) {
    test_support::writeRawConfigRow(&db, key, "{ this is ]not json[");
}

} // namespace

REGISTER_TEST(terminalSettingsClamp,
              "terminal settings: 字号钳[10,24]/回滚钳[100,100000]（need250 §4.2）", {
    using namespace terminal;
    DOCTEST_CHECK_EQ(clampedFontSize(9), kFontSizeMin);
    DOCTEST_CHECK_EQ(clampedFontSize(10), 10);
    DOCTEST_CHECK_EQ(clampedFontSize(13), 13);
    DOCTEST_CHECK_EQ(clampedFontSize(24), 24);
    DOCTEST_CHECK_EQ(clampedFontSize(25), kFontSizeMax);
    DOCTEST_CHECK_EQ(clampedFontSize(-3), kFontSizeMin);

    DOCTEST_CHECK_EQ(clampedScrollback(50), kScrollbackMin);
    DOCTEST_CHECK_EQ(clampedScrollback(100), 100);
    DOCTEST_CHECK_EQ(clampedScrollback(12345), 12345);
    DOCTEST_CHECK_EQ(clampedScrollback(100000), 100000);
    DOCTEST_CHECK_EQ(clampedScrollback(1000000), kScrollbackMax);
    return true;
})

REGISTER_TEST(terminalSettingsPaletteResolve,
              "terminal settings: 色板解析（跟随应用随主题/独立锁定，need250 §4.2③）", {
    using namespace terminal;
    // 跟随应用：随应用主题
    DOCTEST_CHECK(gridPaletteDark(true, TerminalColorMode::FollowApp,
                                  TerminalColorPalette::Light));
    DOCTEST_CHECK(!gridPaletteDark(false, TerminalColorMode::FollowApp,
                                   TerminalColorPalette::Dark));
    // 独立：锁定所选色板，不随主题
    DOCTEST_CHECK(gridPaletteDark(false, TerminalColorMode::Independent,
                                  TerminalColorPalette::Dark));
    DOCTEST_CHECK(!gridPaletteDark(true, TerminalColorMode::Independent,
                                   TerminalColorPalette::Light));
    return true;
})

REGISTER_TEST(terminalSettingsSanitize,
              "terminal settings: sanitize（枚举外字体/shell 回空=平台默认 + 数值钳制）", {
    using namespace terminal;
    TerminalSettings s;
    s.fontFamily = "Comic Sans MS"; // 枚举外
    s.shell = "fish";               // 枚举外
    s.fontSize = 99;
    s.scrollback = 1;
    const TerminalSettings out = sanitizedTerminalSettings(s);
    DOCTEST_CHECK(out.fontFamily.empty());
    DOCTEST_CHECK(out.shell.empty());
    DOCTEST_CHECK_EQ(out.fontSize, kFontSizeMax);
    DOCTEST_CHECK_EQ(out.scrollback, kScrollbackMin);

    TerminalSettings ok;
    ok.fontFamily = terminalFontOptionValues().back(); // 表内合法值
    ok.shell = terminalShellOptionValues().back();
    ok.fontSize = 20;
    ok.scrollback = 50000;
    const TerminalSettings out2 = sanitizedTerminalSettings(ok);
    DOCTEST_CHECK_EQ(out2.fontFamily, terminalFontOptionValues().back());
    DOCTEST_CHECK_EQ(out2.shell, terminalShellOptionValues().back());
    DOCTEST_CHECK_EQ(out2.fontSize, 20);
    DOCTEST_CHECK_EQ(out2.scrollback, 50000);
    return true;
})

REGISTER_TEST(terminalSettingsWorkDir,
              "terminal settings: 目录有效性（空=主目录语义有效/不存在回退）", {
    using namespace terminal;
    DOCTEST_CHECK(workDirUsableForSpawn("")); // 空 = 主目录语义
    DOCTEST_CHECK(!workDirUsableForSpawn("Z:/definitely/not/exist/devdesk"));
    const std::filesystem::path tmp = test_temp::tempPath("terminal_settings_wdir");
    std::filesystem::create_directories(tmp);
    DOCTEST_CHECK(workDirUsableForSpawn(tmp.string()));
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    return true;
})

REGISTER_TEST(terminalSettingsPlatformTables,
              "terminal settings: 平台选项表（首项恒空=平台默认语义）", {
    using namespace terminal;
    const auto fonts = terminalFontOptionValues();
    const auto shells = terminalShellOptionValues();
    DOCTEST_CHECK(!fonts.empty());
    DOCTEST_CHECK(fonts.front().empty()); // 首项 = 内置等宽
    DOCTEST_CHECK(!shells.empty());
    DOCTEST_CHECK(shells.front().empty()); // 首项 = 平台默认
    DOCTEST_CHECK(!terminalDefaultShellName().empty());
    return true;
})

REGISTER_TEST(terminalSettingsRoundtrip,
              "terminal settings: app_config 单行 JSON 存读往返（重启恢复）", {
    using namespace terminal;
    test_support::TestDb env("devdesk_test_terminal_settings_", "rt");
    core::data::SqliteDb& db = env.handle();
    TerminalSettings s;
    s.fontFamily = terminalFontOptionValues()[1];
    s.fontSize = 16;
    s.colorMode = TerminalColorMode::Independent;
    s.colorPalette = TerminalColorPalette::Dark;
    s.shell = terminalShellOptionValues()[1];
    s.workDir = test_temp::tempPath("terminal_settings_rt_dir").string();
    s.scrollback = 50000;
    s.cursorStyle = TerminalCursorStyle::Bar;
    s.cursorBlink = false;
    DOCTEST_CHECK(saveTerminalSettings(&db, s).isOk());

    // 重新 load = 模拟重启
    const TerminalSettings loaded = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(loaded.fontFamily, s.fontFamily);
    DOCTEST_CHECK_EQ(loaded.fontSize, 16);
    DOCTEST_CHECK(loaded.colorMode == TerminalColorMode::Independent);
    DOCTEST_CHECK(loaded.colorPalette == TerminalColorPalette::Dark);
    DOCTEST_CHECK_EQ(loaded.shell, s.shell);
    DOCTEST_CHECK_EQ(loaded.workDir, s.workDir);
    DOCTEST_CHECK_EQ(loaded.scrollback, 50000);
    DOCTEST_CHECK(loaded.cursorStyle == TerminalCursorStyle::Bar);
    DOCTEST_CHECK(!loaded.cursorBlink);
    return true;
})

REGISTER_TEST(terminalSettingsFallbacks,
              "terminal settings: 缺键/库未就绪/JSON 损坏 → 默认值（保存先 sanitize）", {
    using namespace terminal;
    // 库未就绪
    const TerminalSettings d1 = loadTerminalSettings(nullptr);
    DOCTEST_CHECK_EQ(d1.fontSize, kFontSizeDefault);
    DOCTEST_CHECK(!saveTerminalSettings(nullptr, TerminalSettings{}).isOk());

    // 键缺失 = 首次使用
    test_support::TestDb env("devdesk_test_terminal_settings_", "fb");
    core::data::SqliteDb& db = env.handle();
    const TerminalSettings d2 = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(d2.fontSize, kFontSizeDefault);
    DOCTEST_CHECK_EQ(d2.scrollback, kScrollbackDefault);
    DOCTEST_CHECK(d2.colorMode == TerminalColorMode::FollowApp);
    DOCTEST_CHECK(d2.cursorBlink);

    // 损坏 JSON → 默认值
    writeGarbageRow(db, "terminal_settings");
    const TerminalSettings d3 = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(d3.fontSize, kFontSizeDefault);
    DOCTEST_CHECK_EQ(d3.scrollback, kScrollbackDefault);

    // 落库值恒 sanitize：写入越界值 → 读回已收敛
    TerminalSettings bad;
    bad.fontSize = 99;
    bad.scrollback = 7;
    bad.shell = "nope";
    DOCTEST_CHECK(saveTerminalSettings(&db, bad).isOk());
    const TerminalSettings back = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(back.fontSize, kFontSizeMax);
    DOCTEST_CHECK_EQ(back.scrollback, kScrollbackMin);
    DOCTEST_CHECK(back.shell.empty());
    return true;
})
