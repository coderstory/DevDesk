// tests/test_modules/test_md_file_open.cpp
// 「打开方式」文件分发路径解析（审计 2026-09-23 [5]）无头单测：
//   目录内相对化 / 目录外复制捕获 / 撞名编号 / 非法输入拒绝
#include "doctest.h"
#include "test_support.h"
#include "module_markdown/md_file_open.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {
bool writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return f.good();
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

using module::markdown::captureFileIntoNotesDir;

} // namespace

REGISTER_TEST(md_open_relative_inside_root,
              "md_file_open: 笔记目录内的文件直接相对化，不复制", {
    const auto notes = test_support::makeTempDir("devdesk_mdopen_test_", "inside");
    const auto file = notes / "note.md";
    DOCTEST_REQUIRE(writeText(file, "# N\n"));

    auto r = captureFileIntoNotesDir(notes, file.string());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("note.md"));
    // 只解析不复制：目录里仍然只有原文件
    size_t count = 0;
    for (auto it = fs::recursive_directory_iterator(notes);
         it != fs::recursive_directory_iterator(); ++it) ++count;
    DOCTEST_CHECK_EQ(count, 1u);

    fs::remove_all(notes);
    return true;
})

REGISTER_TEST(md_open_relative_inside_nested,
              "md_file_open: 嵌套子目录内相对化（POSIX 分隔符）", {
    const auto notes = test_support::makeTempDir("devdesk_mdopen_test_", "nested");
    const auto file = notes / "inbox" / "idea.md";
    DOCTEST_REQUIRE(writeText(file, "# idea\n"));

    auto r = captureFileIntoNotesDir(notes, file.string());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("inbox/idea.md"));

    fs::remove_all(notes);
    return true;
})

REGISTER_TEST(md_open_external_copied_as_md,
              "md_file_open: 目录外文件复制进笔记根并强制 .md（原文件保留）", {
    const auto notes = test_support::makeTempDir("devdesk_mdopen_test_", "capture");
    const auto srcDir = test_support::makeTempDir("devdesk_mdopen_test_", "capture_src");
    const auto src = srcDir / "report.txt";
    DOCTEST_REQUIRE(writeText(src, "外部正文ABC\n"));

    auto r = captureFileIntoNotesDir(notes, src.string());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("report.md"));
    DOCTEST_CHECK(fs::exists(notes / "report.md"));
    DOCTEST_CHECK_EQ(readText(notes / "report.md"), std::string("外部正文ABC\n"));
    DOCTEST_CHECK(fs::exists(src));  // 复制而非移动

    // 再次打开同一外部文件 → 撞名编号「 2」
    auto r2 = captureFileIntoNotesDir(notes, src.string());
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK_EQ(r2.value(), std::string("report 2.md"));

    fs::remove_all(notes);
    fs::remove_all(srcDir);
    return true;
})

REGISTER_TEST(md_open_rejects_bad_inputs,
              "md_file_open: 不存在/目录/空路径拒绝；笔记目录缺失时自动创建", {
    const auto tmp = test_support::makeTempDir("devdesk_mdopen_test_", "reject");

    // 不存在的路径 → FileNotFound
    DOCTEST_CHECK(captureFileIntoNotesDir(tmp / "notes",
                                          (tmp / "ghost.md").string())
                      .isFail());
    // 目录本身 → 拒绝
    DOCTEST_CHECK(captureFileIntoNotesDir(tmp / "notes", tmp.string()).isFail());
    // 空路径 → 拒绝
    DOCTEST_CHECK(captureFileIntoNotesDir(tmp / "notes", std::string()).isFail());

    // 笔记目录尚未创建 + 有效外部文件 → 自动建目录并捕获成功
    const auto src = tmp / "data.json.md";
    DOCTEST_REQUIRE(writeText(src, "x\n"));
    const auto freshNotes = tmp / "brand_new_notes";
    auto r = captureFileIntoNotesDir(freshNotes, src.string());
    DOCTEST_REQUIRE(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), std::string("data.json.md"));
    DOCTEST_CHECK(fs::is_directory(freshNotes));

    fs::remove_all(tmp);
    return true;
})
