// system_font_gate.cpp — 系统字体族枚举本端门禁 doctest（need:266 交付回补 /
// clue 270 遗留恢复，need:263 批 3 F-09 字体下拉依赖）。
//
// 三件断言（与 EUI-NEO tests/unit/system_font_families.cpp 断言同构，本端
// doctest 形态）：①枚举非空（macOS/Windows 必有系统字体）②含平台常见默认
// 家族任一 ③抽 3 个族 resolveFontPath 必须解析到真实存在的字体文件（只列名
// 不解析 = 假功能，不通过）。2026-11-16 曾因依赖未合入 main 的
// feat/system-font-enum 分支而停用（登记注释），need266 合入后恢复（EUI
// commit dadd536，DevDesk 5d205f9c 指针已推进）。--filter system_font 命中。
#include "doctest.h"

#include "core/render/text.h"

#include <filesystem>
#include <string>
#include <vector>

namespace {

bool hasFontFile(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

}  // namespace

REGISTER_TEST(system_font_gate_families, "system_font: 枚举非空且含平台常见家族", {
    const std::vector<std::string> families =
        core::TextPrimitive::systemFontFamilies();
    // ① macOS/Windows 必有系统字体；空表 = 枚举通路断了
    DOCTEST_CHECK_TRUE(!families.empty());
    // ② 平台常见默认/预装家族任一命中（双平台并集采样，不钉死单一族名）
    bool foundCommon = false;
    for (const std::string& family : families) {
        if (family == "Helvetica" || family == "Arial" ||
            family == "Menlo" || family == "Segoe UI" ||
            family == "Microsoft YaHei" || family == "Cascadia Code" ||
            family == "PingFang SC" || family == "Times New Roman") {
            foundCommon = true;
            break;
        }
    }
    DOCTEST_CHECK_TRUE(foundCommon);
    return true;
})

REGISTER_TEST(system_font_gate_resolve, "system_font: 枚举族按名 resolve 到真实字体文件", {
    const std::vector<std::string> families =
        core::TextPrimitive::systemFontFamilies();
    // 空表时上一用例已红；这里直接短路避免越界采样
    if (families.empty()) return true;
    // ③ 抽 3 个族按名 resolve（取位 1/4、1/2、3/4 避免只抽到头部/尾部特殊族）
    const size_t samples[3] = {families.size() / 4, families.size() / 2,
                               families.size() * 3 / 4};
    for (const size_t index : samples) {
        const std::string& family = families[index];
        const std::string path = core::TextPrimitive::resolveFontPath(family, 400);
        DOCTEST_CHECK_TRUE(hasFontFile(path));
    }
    return true;
})
