// tests/test_platform/test_paths_utf8.cpp
// core::paths::toUtf8 覆盖（bug362）：fs::path → UTF-8 std::string 的跨平台口径。
//
// 守的不变式：喂给「要求 UTF-8 的 C API」（sqlite3_open_v2 / mpv JSON 命令 / libcurl）
// 的路径字节必须是 UTF-8。Windows 上 path::string() 按 ACP 转换，中文用户名会变成乱码
// 路径（打开另一个库 / CANTOPEN），故本工具的字节口径必须与平台 locale 无关。
#include "doctest.h"

#include "core/paths.h"

#include <filesystem>
#include <string>

REGISTER_TEST(to_utf8_preserves_ascii,
              "paths: toUtf8 对纯 ASCII 路径逐字节原样", {
    const std::string got = core::paths::toUtf8(std::filesystem::path("/tmp/a.mp4"));
    DOCTEST_CHECK_EQ(got, std::string("/tmp/a.mp4"));
    return true;
})

REGISTER_TEST(to_utf8_preserves_chinese,
              "paths: toUtf8 输出 UTF-8 字节序列（中文路径不随平台 locale 乱码）", {
    // 用 u8 字面量建 path（UTF-8 → 原生编码），不走窄字面量：后者在 Windows 先按 ACP
    // 解码，测的就不是本工具的输出侧了，也会依赖平台 locale
    const std::filesystem::path p(u8"/tmp/中文目录/视频.mp4");
    // 逐字节硬编码：中=E4B8AD 文=E69687 目=E79BAE 录=E5BD95 视=E8A786 频=E9A291
    const std::string expected =
        "/tmp/\xE4\xB8\xAD\xE6\x96\x87\xE7\x9B\xAE\xE5\xBD\x95/"
        "\xE8\xA7\x86\xE9\xA2\x91.mp4";
    DOCTEST_CHECK_EQ(core::paths::toUtf8(p), expected);
    return true;
})
