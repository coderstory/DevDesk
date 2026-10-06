// tests/test_platform/test_tray_icon.cpp
// platform::tray::defaultIconPath() 覆盖。
//
// 平台差异是硬约束：EUI-NEO 托盘图标两条加载路各认一种格式——macOS 走 NSImage
// （要 22×22 template 蒙版 PNG），Windows 走 ExtractIconExW（**只认 .ico/.exe
// 资源位**，给 PNG 必然 hIcon == NULL → 托盘栏空图标）。选择已下沉到平台层
// （tray_icon_win/macos.cpp）。
//
// 断言的防线（每条都对「路径写错 / 文件缺失」变红）：
//   1. 返回值非空（否则 EUI-NEO 退回应用图标，Windows 侧又变空图标）；
//   2. 后缀对得上平台（macOS .png / Windows .ico）——防「改回去又忘了分发」；
//   3. 该相对路径在**构建产物**的 assets 目录下真实存在（防 CMake 拷贝规则漏了、
//      或平台实现写了个仓库里没有的文件名）。
// 第 3 条的产物根候选与 EUI-NEO resolveIconPath 同构：exeDir/assets（.app bundle
// 形态，exe 与 assets 同级）与 exeDir/../assets（build 目录形态，无头用例的
// devdesk_tests 在 <build>/tests/，资产在 <build>/assets/）。
#include "doctest.h"

#include "core/paths.h"
#include "platform/tray_icon.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

// 在候选根下找 defaultIconPath() 指向的文件；找不到返回空 path。relPath 自带
// "assets/" 前缀，故直接拼在根上、不再叠一层 assets。
std::filesystem::path findShippedAsset(const std::string& relPath) {
    namespace fs = std::filesystem;
    const fs::path exeDir = core::paths::executableDir();
    if (exeDir.empty()) return {};
    const std::vector<fs::path> roots = {exeDir, exeDir.parent_path()};
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (fs::exists(root / relPath, ec)) return root / relPath;
    }
    return {};
}

// 该平台期望的图标后缀。平台条件断言**允许**出现在测试里（业务零宏铁律只约束
// src/ 业务代码）；依据：Windows 侧 ExtractIconExW 只认 .ico 资源位、macOS 侧要
// template 蒙版 PNG，两者不可互换 —— 后缀本身就是「是否按平台分发」的可观测信号。
// 必须放在宏外的辅助函数里：REGISTER_TEST 的 body 是变参宏，宏参数内嵌 #if 触发
// -Wembedded-directive（-Werror 直接红）。把这段搬进任何 REGISTER_TEST 都会编译失败。
const char* expectedExtension() {
#if defined(_WIN32)
    return ".ico";
#elif defined(__APPLE__)
    return ".png";
#else
    return "";  // 其它 POSIX：tray_icon_posix.cpp 返回空串，不参与后缀断言
#endif
}

bool hasExpectedExtension(const std::string& p) {
    const std::string ext = expectedExtension();
    return ext.empty() || (p.size() >= ext.size() &&
                           p.compare(p.size() - ext.size(), ext.size(), ext) == 0);
}

} // namespace

REGISTER_TEST(tray_icon_path_non_empty,
              "tray: defaultIconPath 非空 + 后缀随平台（macOS png / Win ico）", {
    const std::string p = platform::tray::defaultIconPath();
    DOCTEST_CHECK(!p.empty());
    DOCTEST_CHECK(hasExpectedExtension(p));
    return true;
})

REGISTER_TEST(tray_icon_asset_shipped,
              "tray: defaultIconPath 指向的文件在构建产物 assets 下存在", {
    const std::string p = platform::tray::defaultIconPath();
    DOCTEST_CHECK(!p.empty());
    if (p.empty()) return true;
    const std::filesystem::path hit = findShippedAsset(p);
    DOCTEST_CHECK_MESSAGE(!hit.empty(),
                          "构建产物 assets 下找不到 defaultIconPath() 指向的文件: " + p
                          + "（检查 src/platform/tray_icon_*.cpp 的文件名与 "
                            "CMakeLists.txt 的 POST_BUILD copy_if_different）");
    if (!hit.empty()) {
        std::error_code ec;
        DOCTEST_CHECK(std::filesystem::file_size(hit, ec) > 0);
    }
    return true;
})
