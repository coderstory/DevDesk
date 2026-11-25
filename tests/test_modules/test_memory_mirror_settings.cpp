// tests/test_modules/test_memory_mirror_settings.cpp
// bug476：安装镜像源设置的持久化往返——两项落盘/重读全等；旧文件缺键 = 空
//（行为与现状一致）；清空覆盖后为空。数据根经 DataRootEnv 隔离，不碰真实目录。
#include "doctest.h"

#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "module_memory/engine/telemem_config.h"
#include "module_memory/panel/settings_store.h"

using module::memory::panel::MemoryPanelSettings;
using module::memory::panel::loadPanelSettings;
using module::memory::panel::savePanelSettings;

REGISTER_TEST(memory_mirror_settings_round_trip,
              "bug476: 镜像源两项落盘/重读全等；缺键旧文件 = 空；清空覆盖后为空", {
    test_support::DataRootEnv root("bug476", "mirror_settings");
    // 旧版本写下的 panel_settings.json（无镜像键）：读成空 = 行为不变
    const auto path =
        module::memory::engine::memoryDataDir() / "panel_settings.json";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        DOCTEST_REQUIRE(f.is_open());
        f << "{\"sanitizeEnabled\":false}";
    }
    const auto legacy = loadPanelSettings();
    DOCTEST_CHECK_FALSE(legacy.sanitizeEnabled);
    DOCTEST_CHECK(legacy.pipIndexUrl.empty());
    DOCTEST_CHECK(legacy.pipExtraIndexUrl.empty());

    // 往返：两项写入后原样读回
    MemoryPanelSettings s;
    s.pipIndexUrl = "https://mirrors.example/simple";
    s.pipExtraIndexUrl = "https://extra.example/simple";
    DOCTEST_REQUIRE(savePanelSettings(s).isOk());
    const auto back = loadPanelSettings();
    DOCTEST_CHECK_EQ(back.pipIndexUrl, s.pipIndexUrl);
    DOCTEST_CHECK_EQ(back.pipExtraIndexUrl, s.pipExtraIndexUrl);

    // 清空覆盖（UI「清除」= 两项空串）：重读为空
    DOCTEST_REQUIRE(savePanelSettings(MemoryPanelSettings{}).isOk());
    const auto cleared = loadPanelSettings();
    DOCTEST_CHECK(cleared.pipIndexUrl.empty());
    DOCTEST_CHECK(cleared.pipExtraIndexUrl.empty());
    return true;
})