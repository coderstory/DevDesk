// tests/test_modules/test_text_highlight_lexers.cpp
// ui_kit/text_highlight 五类代码词法扫描器（need:10 批次三）：java/js/ts/sql/json
//   的 keyword/string/comment/number 四类 span、跨行状态（块注释/反引号/文本块）、
//   漏标回默认色不错标（未闭合串不产 span）、扩展名映射、行内偏移升序不重叠。
// 跨文件前提：span 坐标 = 行内字节偏移（行内容不含行尾符）；同文件逐行驱动。
#include "doctest.h"
#include "test_support.h"
#include "core/database.h"
#include "core/module_interface.h"
#include "core/theme.h"
#include "ui_kit/text_highlight/code_lexers.h"

#include <string>
#include <vector>

using ui_kit::text_highlight::CodeHighlighter;
using ui_kit::text_highlight::CodeLang;
using ui_kit::text_highlight::HlKind;
using ui_kit::text_highlight::HlSpan;

namespace {

// 期望 span 是否精确在场（含 kind）
bool hasSpan(const std::vector<HlSpan>& spans, int start, int end, HlKind kind) {
    for (const HlSpan& s : spans) {
        if (s.start == start && s.end == end && s.kind == kind) return true;
    }
    return false;
}

bool hasAny(const std::vector<HlSpan>& spans, HlKind kind) {
    for (const HlSpan& s : spans) {
        if (s.kind == kind) return true;
    }
    return false;
}

// 升序不重叠不变量（push 合并与漏标纪律的兜底断言）
bool ascending(const std::vector<HlSpan>& spans) {
    int prevEnd = 0;
    for (const HlSpan& s : spans) {
        if (s.start < prevEnd || s.end <= s.start) return false;
        prevEnd = s.end;
    }
    return true;
}

constexpr bool colorEq(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

} // namespace

REGISTER_TEST(text_highlight_java, "text_highlight: java 四类 span + 块注释跨行", {
    CodeHighlighter hl;
    hl.setLang(CodeLang::Java);
    {
        // public class A { // 行注释尾
        const auto s = hl.spansForLine("public class A { // 行注释尾");
        DOCTEST_CHECK(hasSpan(s, 0, 6, HlKind::Keyword));      // public
        DOCTEST_CHECK(hasSpan(s, 7, 12, HlKind::Keyword));     // class
        DOCTEST_CHECK(hasSpan(s, 17, 32, HlKind::Comment));    // // 行注释尾（4 汉字×3B）
        DOCTEST_CHECK(ascending(s));
    }
    {
        // String s = "a\"b"; int n = 0x1F;
        const auto s = hl.spansForLine("String s = \"a\\\"b\"; int n = 0x1F;");
        DOCTEST_CHECK(hasSpan(s, 11, 17, HlKind::String));     // "a\"b" 含引号
        DOCTEST_CHECK(hasSpan(s, 19, 22, HlKind::Keyword));    // int
        DOCTEST_CHECK(hasSpan(s, 27, 31, HlKind::Number));     // 0x1F
        DOCTEST_CHECK(ascending(s));
    }
    {
        // 块注释开（未闭合）→ 本行全注释
        const auto s = hl.spansForLine("int a; /* start");
        DOCTEST_CHECK(hasSpan(s, 0, 3, HlKind::Keyword));
        DOCTEST_CHECK(hasSpan(s, 7, 15, HlKind::Comment));
        DOCTEST_CHECK(ascending(s));
    }
    {
        // 跨行块注释中段：整行注释
        const auto s = hl.spansForLine("  still comment");
        DOCTEST_CHECK(hasSpan(s, 0, 15, HlKind::Comment));
    }
    {
        // 块注释闭合行：闭合后恢复正常扫描
        const auto s = hl.spansForLine("end */ int b;");
        DOCTEST_CHECK(hasSpan(s, 0, 6, HlKind::Comment));
        DOCTEST_CHECK(hasSpan(s, 7, 10, HlKind::Keyword));     // int
        DOCTEST_CHECK(ascending(s));
    }
    return true;
})

REGISTER_TEST(text_highlight_js_ts, "text_highlight: js/ts 关键字与反引号模板串跨行", {
    CodeHighlighter js;
    js.setLang(CodeLang::JavaScript);
    {
        // const x = `template   ← 反引号未闭合，跨行
        const auto s = js.spansForLine("const x = `template");
        DOCTEST_CHECK(hasSpan(s, 0, 5, HlKind::Keyword));      // const
        DOCTEST_CHECK(hasSpan(s, 10, 19, HlKind::String));
    }
    {
        const auto s = js.spansForLine("still open");
        DOCTEST_CHECK(hasSpan(s, 0, 10, HlKind::String));
    }
    {
        // close` + 42;
        const auto s = js.spansForLine("close` + 42;");
        DOCTEST_CHECK(hasSpan(s, 0, 6, HlKind::String));
        DOCTEST_CHECK(hasSpan(s, 9, 11, HlKind::Number));
        DOCTEST_CHECK(ascending(s));
    }
    {
        // 未闭合单引号：不产 span（漏标回默认）
        const auto s = js.spansForLine("let s = 'unclosed;");
        DOCTEST_CHECK(hasSpan(s, 0, 3, HlKind::Keyword));      // let
        DOCTEST_CHECK(!hasAny(s, HlKind::String));
    }

    CodeHighlighter ts;
    ts.setLang(CodeLang::TypeScript);
    {
        // interface Foo { readonly id: number }
        const auto s = ts.spansForLine("interface Foo { readonly id: number }");
        DOCTEST_CHECK(hasSpan(s, 0, 9, HlKind::Keyword));      // interface
        DOCTEST_CHECK(hasSpan(s, 16, 24, HlKind::Keyword));    // readonly
        DOCTEST_CHECK(hasSpan(s, 29, 35, HlKind::Keyword));    // number
        DOCTEST_CHECK(ascending(s));
    }
    return true;
})

REGISTER_TEST(text_highlight_sql_json, "text_highlight: sql 大小写不敏感/''转义 + json 字面量", {
    CodeHighlighter sql;
    sql.setLang(CodeLang::Sql);
    {
        // SELECT id FROM t -- 取数   （大小写不敏感关键字 + -- 行注释）
        const auto s = sql.spansForLine("SELECT id FROM t -- 取数");
        DOCTEST_CHECK(hasSpan(s, 0, 6, HlKind::Keyword));      // SELECT
        DOCTEST_CHECK(hasSpan(s, 10, 14, HlKind::Keyword));    // FROM
        DOCTEST_CHECK(hasSpan(s, 17, 26, HlKind::Comment));    // -- 取数（含 --）
        DOCTEST_CHECK(ascending(s));
    }
    {
        // WHERE name = 'it''s' AND n = 100
        const auto s = sql.spansForLine("WHERE name = 'it''s' AND n = 100");
        DOCTEST_CHECK(hasSpan(s, 0, 5, HlKind::Keyword));      // WHERE
        DOCTEST_CHECK(hasSpan(s, 13, 20, HlKind::String));     // 'it''s'
        DOCTEST_CHECK(hasSpan(s, 29, 32, HlKind::Number));     // 100
        DOCTEST_CHECK(ascending(s));
    }
    {
        // 未闭合串：漏标回默认（SQL 串不跨行，mode 不残留）
        const auto s = sql.spansForLine("WHERE v = 'open");
        DOCTEST_CHECK(hasSpan(s, 0, 5, HlKind::Keyword));
        DOCTEST_CHECK(!hasAny(s, HlKind::String));
        const auto next = sql.spansForLine("SELECT 1");
        DOCTEST_CHECK(hasSpan(next, 0, 6, HlKind::Keyword));   // mode 未被污染
    }

    CodeHighlighter json;
    json.setLang(CodeLang::Json);
    {
        // {"k": "v\"x", n: [1, 2.5e3], ok: true, x: null}
        const auto s = json.spansForLine(
            "{\"k\": \"v\\\"x\", n: [1, 2.5e3], ok: true, x: null}");
        DOCTEST_CHECK(hasSpan(s, 1, 4, HlKind::String));       // "k"
        DOCTEST_CHECK(hasSpan(s, 6, 12, HlKind::String));      // "v\"x"
        DOCTEST_CHECK(hasSpan(s, 18, 19, HlKind::Number));     // 1
        DOCTEST_CHECK(hasSpan(s, 21, 26, HlKind::Number));     // 2.5e3
        DOCTEST_CHECK(hasSpan(s, 33, 37, HlKind::Keyword));    // true
        DOCTEST_CHECK(hasSpan(s, 42, 46, HlKind::Keyword));    // null
        DOCTEST_CHECK(ascending(s));
    }
    {
        // JSON 无注释语法：// 不产 span（漏标）
        const auto s = json.spansForLine("a // not comment");
        DOCTEST_CHECK(!hasAny(s, HlKind::Comment));
    }
    return true;
})

REGISTER_TEST(text_highlight_ext_map, "text_highlight: 扩展名映射 + 主题色映射四槽位", {
    CodeLang lang = CodeLang::Json;
    DOCTEST_CHECK(ui_kit::text_highlight::codeLangFromExtension("A.java", lang));
    DOCTEST_CHECK(lang == CodeLang::Java);
    DOCTEST_CHECK(ui_kit::text_highlight::codeLangFromExtension("x.JS", lang));
    DOCTEST_CHECK(lang == CodeLang::JavaScript);
    DOCTEST_CHECK(ui_kit::text_highlight::codeLangFromExtension("a/b/c.tsx", lang));
    DOCTEST_CHECK(lang == CodeLang::TypeScript);
    DOCTEST_CHECK(ui_kit::text_highlight::codeLangFromExtension("q.Sql", lang));
    DOCTEST_CHECK(lang == CodeLang::Sql);
    DOCTEST_CHECK(ui_kit::text_highlight::codeLangFromExtension("package.json", lang));
    DOCTEST_CHECK(lang == CodeLang::Json);
    DOCTEST_CHECK(!ui_kit::text_highlight::codeLangFromExtension("readme.md", lang));
    DOCTEST_CHECK(!ui_kit::text_highlight::codeLangFromExtension("noext", lang));

    // 主题映射：keyword/string 两槽位语义色互异（--hl-keyword=info、--hl-string=绿）
    const auto tokens = core::theme::current();  // 默认 Dark
    const auto c = ui_kit::text_highlight::codeTokenColors(tokens);
    DOCTEST_CHECK(colorEq(c.keyword, tokens.colors.info));
    DOCTEST_CHECK(colorEq(c.string_, tokens.colors.primaryText));
    DOCTEST_CHECK(colorEq(c.comment, tokens.colors.textTertiary));
    DOCTEST_CHECK(colorEq(c.number, tokens.colors.warning));
    DOCTEST_CHECK(colorEq(ui_kit::text_highlight::colorOf(HlKind::Number, c),
                          tokens.colors.warning));
    return true;
})

REGISTER_TEST(text_highlight_span_merge, "text_highlight: 相邻同 kind span 合并 + reset 清态", {
    CodeHighlighter hl;
    hl.setLang(CodeLang::JavaScript);
    {
        // /*a*/for/*b*/while
        const auto s = hl.spansForLine("/*a*/for/*b*/while");
        DOCTEST_CHECK(ascending(s));
        DOCTEST_CHECK(hasSpan(s, 0, 5, HlKind::Comment));
        DOCTEST_CHECK(hasSpan(s, 5, 8, HlKind::Keyword));      // for
        DOCTEST_CHECK(hasSpan(s, 13, 18, HlKind::Keyword));    // while
    }
    {
        // 跨行残留态 → reset 后按 Normal 扫（错标防线）
        hl.spansForLine("/* open");
        hl.reset();
        const auto s = hl.spansForLine("int after reset");
        DOCTEST_CHECK(!hasAny(s, HlKind::Comment));
    }
    return true;
})
