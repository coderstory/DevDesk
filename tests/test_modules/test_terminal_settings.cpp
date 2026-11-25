// tests/test_modules/test_terminal_settings.cpp
// 终端设置纯函数 + 持久化单测（need250）：钳制/sanitize 枚举收敛/色板解析/目录有效性
// + app_config 单行 JSON 存读往返（缺键/损坏/库未就绪回退默认，等价「重启恢复」）。
#include "doctest.h"

#include "core/app_config.h"
#include "module_terminal/terminal_settings.h"
#include "module_terminal/terminal_dir_menu.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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
    const TerminalSettings out = sanitizedTerminalSettings(s);
    DOCTEST_CHECK(out.fontFamily.empty());
    DOCTEST_CHECK(out.shell.empty());
    DOCTEST_CHECK_EQ(out.fontSize, kFontSizeMax);

    TerminalSettings ok;
    ok.fontFamily = terminalFontOptionValues().back(); // 表内合法值
    ok.shell = terminalShellOptionValues().back();
    ok.fontSize = 20;
    const TerminalSettings out2 = sanitizedTerminalSettings(ok);
    DOCTEST_CHECK_EQ(out2.fontFamily, terminalFontOptionValues().back());
    DOCTEST_CHECK_EQ(out2.shell, terminalShellOptionValues().back());
    DOCTEST_CHECK_EQ(out2.fontSize, 20);
    return true;
})

REGISTER_TEST(terminalSettingsWorkDir,
              "terminal settings: 目录有效性（空=主目录/不存在/是文件/无权限均拒绝）", {
    using namespace terminal;
    DOCTEST_CHECK(workDirUsableForSpawn("")); // 空 = 主目录语义
    DOCTEST_CHECK(!workDirUsableForSpawn("Z:/definitely/not/exist/devdesk"));
    const std::filesystem::path tmp = test_temp::tempPath("terminal_settings_wdir");
    std::filesystem::create_directories(tmp);
    DOCTEST_CHECK(workDirUsableForSpawn(tmp.string()));

    // 是文件不是目录（P-14：只 exists 不够）
    const std::filesystem::path file = tmp / "not-a-dir.txt";
    { std::ofstream out(file); out << "x"; }
    DOCTEST_CHECK(!workDirUsableForSpawn(file.string()));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    return true;
})

#if !defined(_WIN32)
REGISTER_TEST(terminalSettingsWorkDirNoPermission,
              "terminal settings: 无权限进入的目录拒绝（POSIX access+opendir）", {
    using namespace terminal;
    if (::geteuid() == 0) return true;  // root 不受权限位约束
    const std::filesystem::path dir = test_temp::tempPath("terminal_settings_noperm");
    std::filesystem::create_directories(dir);
    std::filesystem::permissions(dir, std::filesystem::perms::none);
    DOCTEST_CHECK(!workDirUsableForSpawn(dir.string()));
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return true;
})
#endif

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
    DOCTEST_CHECK(saveTerminalSettings(&db, s).isOk());

    // 重新 load = 模拟重启
    const TerminalSettings loaded = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(loaded.fontFamily, s.fontFamily);
    DOCTEST_CHECK_EQ(loaded.fontSize, 16);
    DOCTEST_CHECK(loaded.colorMode == TerminalColorMode::Independent);
    DOCTEST_CHECK(loaded.colorPalette == TerminalColorPalette::Dark);
    DOCTEST_CHECK_EQ(loaded.shell, s.shell);
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
    DOCTEST_CHECK(d2.colorMode == TerminalColorMode::FollowApp);

    // 损坏 JSON → 默认值
    writeGarbageRow(db, "terminal_settings");
    const TerminalSettings d3 = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(d3.fontSize, kFontSizeDefault);

    // 落库值恒 sanitize：写入越界值 → 读回已收敛
    TerminalSettings bad;
    bad.fontSize = 99;
    bad.shell = "nope";
    DOCTEST_CHECK(saveTerminalSettings(&db, bad).isOk());
    const TerminalSettings back = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(back.fontSize, kFontSizeMax);
    DOCTEST_CHECK(back.shell.empty());
    return true;
})


// ---- need452：常用目录列表（迁移/上限/去重/默认顺延 + 弹层几何 + 路径截断）----

REGISTER_TEST(terminalSettingsStartupDirsMigration,
              "terminal settings: need452 迁移收敛（旧单值与列表并入/默认归属/空列表）", {
    using namespace terminal;
    // 旧单值非空、无列表 → 列表 = [旧值]，默认 = 旧值（老用户首开即见原目录）
    TerminalSettings legacy;
    legacy.startupDir = "  D:\\proj  ";
    const TerminalSettings m1 = sanitizedTerminalSettings(legacy);
    DOCTEST_CHECK_EQ(m1.startupDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(m1.startupDirs[0], "D:\\proj");
    DOCTEST_CHECK_EQ(m1.startupDir, "D:\\proj");

    // 皆空 → 空列表、主目录语义
    const TerminalSettings m2 = sanitizedTerminalSettings(TerminalSettings{});
    DOCTEST_CHECK(m2.startupDirs.empty());
    DOCTEST_CHECK(m2.startupDir.empty());

    // 列表非空、默认空 → 默认 = 首项并回写镜像
    TerminalSettings noDef;
    noDef.startupDirs = {"D:\\a", "D:\\b"};
    const TerminalSettings m3 = sanitizedTerminalSettings(noDef);
    DOCTEST_CHECK_EQ(m3.startupDir, "D:\\a");

    // 默认不在列表（网页改写 = 设默认）→ 插首位并设默认
    TerminalSettings moved;
    moved.startupDirs = {"D:\\a", "D:\\b"};
    moved.startupDir = "D:\\c";
    const TerminalSettings m4 = sanitizedTerminalSettings(moved);
    DOCTEST_CHECK_EQ(m4.startupDirs.size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(m4.startupDirs[0], "D:\\c");
    DOCTEST_CHECK_EQ(m4.startupDir, "D:\\c");
    return true;
})

REGISTER_TEST(terminalSettingsStartupDirsDedupe,
              "terminal settings: need452 上限12/trim空串丢弃/平台规则去重/默认优先保位", {
    using namespace terminal;
    // trim 与去空
    StartupDirConvergence c = convergeStartupDirs({"  a  ", "", "   ", "b"}, "");
    DOCTEST_CHECK_EQ(c.dirs.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(c.dirs[0], "a");
    DOCTEST_CHECK_EQ(c.dirs[1], "b");

    // 平台去重规则（Win 大小写不敏感 / POSIX 敏感）
    c = convergeStartupDirs({"D:\\A", "d:\\a", "D:\\B"}, "");
    DOCTEST_CHECK_EQ(c.dirs.size(), startupDirPathEqual("D:\\A", "d:\\a")
                                        ? static_cast<size_t>(2)
                                        : static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(c.dirs[0], "D:\\A");  // 保留首次出现的写法

    // 上限 12：13 条截断到 12
    std::vector<std::string> many;
    for (int i = 0; i < 13; i++) many.push_back("D:\\dir" + std::to_string(i));
    c = convergeStartupDirs(many, "");
    DOCTEST_CHECK_EQ(c.dirs.size(), static_cast<size_t>(kStartupDirsMax));

    // 默认项必占一席：满 12 且默认不在列表 → 插首位后仍 12（挤掉末项）
    c = convergeStartupDirs(many, "D:\\new");
    DOCTEST_CHECK_EQ(c.dirs.size(), static_cast<size_t>(kStartupDirsMax));
    DOCTEST_CHECK_EQ(c.dirs[0], "D:\\new");
    DOCTEST_CHECK_EQ(c.defaultDir, "D:\\new");
    return true;
})

REGISTER_TEST(terminalSettingsStartupDirsRoundtrip,
              "terminal settings: need452 落库往返 + 旧 JSON 迁移（列表+默认镜像）", {
    using namespace terminal;
    test_support::TestDb env("devdesk_test_terminal_settings_", "dirs");
    core::data::SqliteDb& db = env.handle();
    TerminalSettings s;
    s.startupDirs = {"D:\\a", "D:\\b"};
    s.startupDir = "D:\\b";
    DOCTEST_CHECK(saveTerminalSettings(&db, s).isOk());
    const TerminalSettings back = loadTerminalSettings(&db);  // 重新 load = 重启
    DOCTEST_CHECK_EQ(back.startupDirs.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(back.startupDirs[1], "D:\\b");
    DOCTEST_CHECK_EQ(back.startupDir, "D:\\b");

    // 旧结构（只有 startupDir 键）→ 首开迁移为单条列表
    test_support::writeRawConfigRow(&db, "terminal_settings",
                                    R"({"startupDir":"D:\\legacy"})");
    const TerminalSettings migrated = loadTerminalSettings(&db);
    DOCTEST_CHECK_EQ(migrated.startupDirs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(migrated.startupDir, "D:\\legacy");
    return true;
})

REGISTER_TEST(terminalDirMenuGeometry,
              "terminal dir menu: need452 弹层几何（右缘锚定/触底上翻/左右钳制/宽min320）", {
    using namespace terminal;
    // 宽窗 1600：高 = 134+36n
    DOCTEST_CHECK_EQ(dirMenuHeight(0), 134.0f);
    DOCTEST_CHECK_EQ(dirMenuHeight(12), 566.0f);
    // 右缘锚定新建钮右缘：x = anchor − 320（1600 时未触钳）
    const DirMenuGeometry g1 = dirMenuGeometry(1600.0f, 1000.0f, 1588.0f, dirMenuHeight(3));
    DOCTEST_CHECK_EQ(g1.width, 320.0f);
    DOCTEST_CHECK_EQ(g1.x, 1268.0f);
    DOCTEST_CHECK_EQ(g1.y, 50.0f);
    DOCTEST_CHECK(!g1.flipped);
    // 最小窗 1000×640：n=12 高 566 ≤ 下方可用 578 → 向下展开
    const DirMenuGeometry g2 = dirMenuGeometry(1000.0f, 640.0f, 940.0f, 566.0f);
    DOCTEST_CHECK(!g2.flipped);
    DOCTEST_CHECK_EQ(g2.y, 50.0f);
    // 可用高不足 → 向上翻转且顶缘钳 ≥12
    const DirMenuGeometry g3 = dirMenuGeometry(1000.0f, 500.0f, 940.0f, 566.0f);
    DOCTEST_CHECK(g3.flipped);
    DOCTEST_CHECK_EQ(g3.y, 12.0f);
    // 右缘越界 → 左钳；窄窗宽 = page−24
    const DirMenuGeometry g4 = dirMenuGeometry(1000.0f, 1000.0f, 300.0f, 134.0f);
    DOCTEST_CHECK_EQ(g4.x, 12.0f);
    const DirMenuGeometry g5 = dirMenuGeometry(300.0f, 1000.0f, 288.0f, 134.0f);
    DOCTEST_CHECK_EQ(g5.width, 276.0f);
    DOCTEST_CHECK_EQ(g5.x, 12.0f);
    // 空态链接：上方居中
    const DirMenuGeometry g6 =
        dirMenuGeometryAbove(1000.0f, 640.0f, 500.0f, 300.0f, dirMenuHeight(2));
    DOCTEST_CHECK(g6.flipped);
    DOCTEST_CHECK_EQ(g6.x, 340.0f);
    DOCTEST_CHECK_EQ(g6.y, 300.0f - 6.0f - dirMenuHeight(2));
    return true;
})

REGISTER_TEST(terminalStartupDirsBlockHeight,
              "terminal settings: need452 常用目录区块高（空态86/n条44n+8(n-1)+12+32）", {
    using namespace terminal;
    DOCTEST_CHECK_EQ(startupDirsBlockHeight(0), 86.0f);
    DOCTEST_CHECK_EQ(startupDirsBlockHeight(1), 44.0f + 12.0f + 32.0f);
    DOCTEST_CHECK_EQ(startupDirsBlockHeight(3), 3 * 44.0f + 2 * 8.0f + 12.0f + 32.0f);
    return true;
})

REGISTER_TEST(terminalPathElide,
              "terminal settings: need452 长路径截断（~前缀/中段…末2段/不退化变长）", {
    using namespace terminal;
    const auto m = [](const std::string& s) { return static_cast<float>(s.size()) * 7.0f; };
    // ① 主目录前缀 → ~（前缀后必须紧跟分隔符）
    DOCTEST_CHECK_EQ(elidedPathForDisplay("C:\\Users\\x\\proj\\a",
                                          "C:\\Users\\x", 9999.0f, m),
                     std::string("~\\proj\\a"));
    DOCTEST_CHECK_EQ(elidedPathForDisplay("C:\\Users\\x2\\p", "C:\\Users\\x",
                                          9999.0f, m),
                     std::string("C:\\Users\\x2\\p"));
    // ② 超宽 → 首段 + … + 末 2 段
    const std::string deep = "D:\\project\\zems\\infomedia\\rs.host";
    const std::string cut = elidedPathForDisplay(deep, "", 60.0f, m);
    DOCTEST_CHECK_EQ(cut, std::string("D:\\…\\infomedia\\rs.host"));
    // ③ 段数不足/不省长度 → 原样（尾部省略兜底）
    DOCTEST_CHECK_EQ(elidedPathForDisplay("D:\\a", "", 10.0f, m), std::string("D:\\a"));
    DOCTEST_CHECK_EQ(elidedPathForDisplay("", "", 10.0f, m), std::string(""));
    return true;
})
