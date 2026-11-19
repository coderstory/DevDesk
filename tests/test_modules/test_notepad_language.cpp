// tests/test_modules/test_notepad_language.cpp
// need263 批 2：notepad 语言层——10 类映射采样 + 清单外 None + 大小写不敏感
// + provider None 空 / Json 非空与全文档坐标平移。
#include "doctest.h"
#include "module_notepad/notepad_language.h"

#include <eui/types.h>

#include <string>
#include <string_view>

using module::notepad::highlightLine;
using module::notepad::languageForExtension;

namespace {

// 映射采样：{路径, 期望语言}；空串 = None 纯文本。
// .ts→js 与 .md/.markdown→空 是 LangSpec 现表（8 语言）下的收敛口径（见头文件注记）。
const struct {
    const char* path;
    const char* lang;
} kMappingCases[] = {
    {"src/main.cpp", "cpp"},  {"include/a.hpp", "cpp"}, {"b.cc", "cpp"},
    {"c.cxx", "cpp"},         {"d.h", "cpp"},           {"app.py", "python"},
    {"index.js", "js"},       {"app.ts", "js"},         {"main.go", "go"},
    {"lib.rs", "rust"},       {"run.sh", "shell"},      {"run.bash", "shell"},
    {"Main.java", "java"},    {"pkg.json", "json"},     {"readme.md", ""},
    {"notes.markdown", ""},   {"query.sql", "sql"},
};

}  // namespace

REGISTER_TEST(notepad_language_mapping,
              "notepad_language: 扩展名→语言映射采样，清单外→None", {
    for (const auto& tc : kMappingCases) {
        DOCTEST_CHECK_EQ(std::string(languageForExtension(tc.path)),
                         std::string(tc.lang));
    }
    DOCTEST_CHECK(languageForExtension("style.css").empty());
    DOCTEST_CHECK(languageForExtension("notes.txt").empty());
    DOCTEST_CHECK(languageForExtension("config.ini").empty());
    DOCTEST_CHECK(languageForExtension("noext").empty());
    return true;
})

REGISTER_TEST(notepad_language_case_insensitive,
              "notepad_language: 扩展名大小写不敏感", {
    DOCTEST_CHECK_EQ(std::string(languageForExtension("MAIN.PY")), "python");
    DOCTEST_CHECK_EQ(std::string(languageForExtension("pkg.JSON")), "json");
    DOCTEST_CHECK_EQ(std::string(languageForExtension("a.CPP")), "cpp");
    DOCTEST_CHECK(languageForExtension("B.Md").empty());
    return true;
})

REGISTER_TEST(notepad_language_provider_none_vs_json,
              "notepad_language: provider None→空 / Json→非空 + 全文档坐标平移", {
    // None（.txt 映射空 lang）→ 零 runs
    const std::string_view langNone = languageForExtension("notes.txt");
    DOCTEST_CHECK(langNone.empty());
    const auto none = highlightLine("{\"not\": \"json\"}", 100, 0, langNone);
    DOCTEST_CHECK(none.empty());

    // Json → 非空，坐标已平移进全文档域（>= lineStartByte）且 run 有效
    const std::string_view langJson = languageForExtension("pkg.json");
    DOCTEST_CHECK(!langJson.empty());
    const auto runs = highlightLine("{\"k\": true}", 100, 3, langJson);
    DOCTEST_CHECK(!runs.empty());
    for (const eui::TextRun& r : runs) {
        DOCTEST_CHECK(r.byteStart >= 100);
        DOCTEST_CHECK(r.byteEnd > r.byteStart);
    }

    // need375：.sql 已入映射（"SELECT" 关键字 + 大小写不敏感命中）
    const std::string_view langSql = languageForExtension("query.SQL");
    DOCTEST_CHECK_EQ(std::string(langSql), "sql");
    const auto sqlRuns = highlightLine("select id from t;", 0, 0, langSql);
    DOCTEST_CHECK(!sqlRuns.empty());

    // LangSpec 未覆盖语言名（防御面）→ 空
    const auto unknown = highlightLine("SELECT 1", 0, 0, "cobol");
    DOCTEST_CHECK(unknown.empty());
    return true;
})
