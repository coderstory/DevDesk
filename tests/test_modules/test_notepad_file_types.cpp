// tests/test_modules/test_notepad_file_types.cpp
// need:273 批 1 —— core 层扩展名单一真源（src/core/notepad_file_types.h）专测：
// 清单 ↔ 判据一致性（notepadExtensions() 全量逐项过 isNotepadExtension）。
// 装配侧（漏斗/托盘判据）断言在 test_app_assembly.cpp，本文件不重复。
#include "doctest.h"

#include "core/notepad_file_types.h"

#include <string>
#include <vector>

namespace np = core::notepad;

// 清单全量 26 项逐项命中（含 .cc/.cxx/.hpp/.tsx/.mjs/.cjs/.jsx/.yml/.toml/
// .ini/.sql/.xml 长尾）——清单加项而判据漏判时此处必红。
REGISTER_TEST(notepad_file_types_list_hits, "notepad_file_types: 清单 26 项逐项命中", {
    const std::vector<std::string_view>& exts = np::notepadExtensions();
    DOCTEST_CHECK_EQ(exts.size(), static_cast<size_t>(26));
    for (const std::string_view ext : exts) {
        DOCTEST_CHECK_MESSAGE(np::isNotepadExtension("file" + std::string(ext)),
                              "清单项未被判据命中: " + std::string(ext));
    }
    // 长尾显式钉住（清单↔判据一致性的可读锚）
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.cc"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.cxx"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.hpp"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.tsx"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.mjs"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.cjs"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.jsx"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.yml"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.toml"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.ini"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.sql"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("a.xml"));
    return true;
})

// 反例：清单外扩展名 / 无扩展名 / 空串一律不命中。
REGISTER_TEST(notepad_file_types_negatives, "notepad_file_types: 清单外反例不命中", {
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("photo.png"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("blob.bin"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("no_ext"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension(""));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("x.pdf"));
    return true;
})

// 后缀前缀误命中防护：命中串后多一个字符即不得命中（"a.mdx" ≠ .md、
// "a.txtx" ≠ .txt、"a.tsv" ≠ .ts）。
REGISTER_TEST(notepad_file_types_suffix_guard, "notepad_file_types: 后缀前缀误命中防护", {
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("a.mdx"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("a.txtx"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("a.tsv"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("a.htmhtml"));
    return true;
})

// 带目录路径与纯扩展名文件名：目录前缀不干扰；".md"/".txt" 这类整名=扩展名的
// 隐藏式文件名命中。
REGISTER_TEST(notepad_file_types_paths, "notepad_file_types: 目录路径与纯扩展名文件名", {
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("/x/y/note.md"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("C:\\dir\\a.json"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("deep/nested/path/readme.markdown"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension(".md"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension(".txt"));
    return true;
})

// 大小写不敏感（ASCII 折叠，OS 送达扩展名大小写不定）。
REGISTER_TEST(notepad_file_types_case_fold, "notepad_file_types: 大小写不敏感命中", {
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("README.MD"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("NOTES.Markdown"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("data.TXT"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("config.JSON"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("app.Ts"));
    DOCTEST_CHECK_TRUE(np::isNotepadExtension("X.YML"));
    DOCTEST_CHECK_FALSE(np::isNotepadExtension("bad.MDX"));
    return true;
})

// 折叠函数契约：只折 A-Z，其余原样。
REGISTER_TEST(notepad_file_types_ascii_lower, "notepad_file_types: ASCII 折叠契约", {
    DOCTEST_CHECK_EQ(np::notepadAsciiLower('Q'), 'q');
    DOCTEST_CHECK_EQ(np::notepadAsciiLower('q'), 'q');
    DOCTEST_CHECK_EQ(np::notepadAsciiLower('1'), '1');
    DOCTEST_CHECK_EQ(np::notepadAsciiLower('-'), '-');
    return true;
})
