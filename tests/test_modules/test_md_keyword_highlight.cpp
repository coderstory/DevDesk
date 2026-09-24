// tests/test_modules/test_md_keyword_highlight.cpp
// Phase 3 ⑤：预览代码块关键字多色词法器——
//   1. C++：关键字/字符串（含转义）/行注释/块注释（跨行）/数字/预处理指令
//   2. Python：关键字/单双引号串/三引号串（跨行）/前缀串（r/b/f/u）/#注释/数字
//   3. fallback：未覆盖 lang / 空 lang → supported=false、runs 空
//   4. 结构不变式：runs 升序不重叠、非空、非空白字节全覆盖（空白 = 间隙）
//   5. 容错：单行串未闭合漏标（不错标）、别名大小写不敏感
//   6. 色表派生：mdHighlightColors 新 4 token = 既有语义色（info/warning/
//      textTertiary/primaryText），浅/深两套同源
#include "doctest.h"
#include "core/theme.h"
#include "module_markdown/md_highlight.h"
#include "module_markdown/md_keyword_highlight.h"

#include <eui/types.h>

#include <string>
#include <string_view>
#include <vector>

using module::markdown::MdHighlightColors;
using module::markdown::MdKeywordHighlight;
using module::markdown::mdHighlightColors;
using module::markdown::mdKeywordHighlight;
using module::markdown::mdKeywordLangSupported;

namespace {

// 可辨测试色（值任意）
const eui::Color kMark{1.0f, 0.0f, 0.0f, 1.0f};
const eui::Color kCode{0.0f, 1.0f, 0.0f, 1.0f};
const eui::Color kLink{0.0f, 0.0f, 1.0f, 1.0f};
const eui::Color kList{1.0f, 1.0f, 0.0f, 1.0f};
const eui::Color kQuote{1.0f, 0.0f, 1.0f, 1.0f};
const eui::Color kKw{0.2f, 0.2f, 0.9f, 1.0f};    // codeKeyword
const eui::Color kStr{0.9f, 0.6f, 0.1f, 1.0f};   // codeString
const eui::Color kCmt{0.5f, 0.5f, 0.5f, 1.0f};   // codeComment
const eui::Color kNum{0.8f, 0.3f, 0.2f, 1.0f};   // codeNumber

MdHighlightColors testColors() {
    return MdHighlightColors{kMark, kCode, kLink, kList, kQuote,
                             kKw,   kStr,  kCmt,  kNum};
}

bool colorEq(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

const eui::TextRun* runAt(const MdKeywordHighlight& h, size_t byte) {
    for (const auto& r : h.runs) {
        if (static_cast<size_t>(r.byteStart) <= byte &&
            byte < static_cast<size_t>(r.byteEnd)) {
            return &r;
        }
    }
    return nullptr;
}

// [from,to) 整段恰为同一个 run 且色 = color
bool exactRun(const MdKeywordHighlight& h, size_t from, size_t to,
              const eui::Color& color) {
    for (const auto& r : h.runs) {
        if (static_cast<size_t>(r.byteStart) == from &&
            static_cast<size_t>(r.byteEnd) == to) {
            return colorEq(r.color, color);
        }
    }
    return false;
}

// 结构不变式：升序、不重叠、非空、非空白字节全覆盖
bool structInvariants(const std::string& code, const MdKeywordHighlight& h) {
    int prevEnd = 0;
    std::vector<char> covered(code.size(), 0);
    for (const auto& r : h.runs) {
        if (r.byteStart >= r.byteEnd) return false;
        if (r.byteStart < prevEnd) return false;
        if (r.byteEnd > static_cast<int>(code.size())) return false;
        prevEnd = r.byteEnd;
        for (int b = r.byteStart; b < r.byteEnd; ++b) covered[b] = 1;
    }
    for (size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        const bool blank = c == ' ' || c == '\t' || c == '\n';
        if (!blank && !covered[i]) return false;
    }
    return true;
}

MdKeywordHighlight lex(std::string_view code, std::string_view lang) {
    return mdKeywordHighlight(code, lang, testColors());
}

} // namespace

REGISTER_TEST(md_keyword_cpp_basic,
              "md_keyword: C++ 关键字/字符串/行注释/数字着色", {
    const std::string code = "int main() { return 42; }";
    const auto h = lex(code, "cpp");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 3, kKw));     // int
    DOCTEST_CHECK(exactRun(h, 4, 12, kCode));  // main() { 整段默认 code 色
    DOCTEST_CHECK(exactRun(h, 13, 19, kKw));  // return
    DOCTEST_CHECK(exactRun(h, 20, 22, kNum)); // 42
    DOCTEST_CHECK(runAt(h, 24) != nullptr);   // } 落默认段
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_cpp_string_comment,
              "md_keyword: C++ 转义串/字符/行注释到行尾", {
    const std::string code = "char c = 'a';\nconst char* s = \"a\\\"b\"; // note";
    const auto h = lex(code, "cpp");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 4, kKw));    // char
    const size_t quote = code.find("'a'");
    DOCTEST_CHECK(exactRun(h, quote, quote + 3, kStr));
    const size_t dq = code.find("\"a\\\"b\"");
    DOCTEST_CHECK(exactRun(h, dq, dq + 6, kStr));  // 转义引号不截断串（6 字符）
    const size_t cmt = code.find("// note");
    DOCTEST_CHECK(exactRun(h, cmt, code.size(), kCmt));  // 行注释到行尾
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_cpp_block_comment,
              "md_keyword: C++ 块注释跨行（按行切同色 run，换行为间隙）", {
    const std::string code = "int a;\n/* two\nlines */\nint b;";
    const auto h = lex(code, "cpp");
    const size_t open = code.find("/*");
    DOCTEST_CHECK(colorEq(runAt(h, open + 1)->color, kCmt));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("lines"))->color, kCmt));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("*/") + 1)->color, kCmt));
    DOCTEST_CHECK(runAt(h, open - 1) == nullptr);  // \n 间隙不被覆盖
    const size_t secondInt = code.find("int b");
    DOCTEST_CHECK(exactRun(h, secondInt, secondInt + 3, kKw));  // 注释后恢复
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_cpp_preproc_number,
              "md_keyword: C++ #include 指令 + 各形态数字", {
    const std::string code = "#include <vector>\nlong v = 0xFF + 1_000 + 3.14 + 42LL;";
    const auto h = lex(code, "cpp");
    DOCTEST_CHECK(exactRun(h, 0, 8, kKw));  // #include（meta 按关键字色）
    for (const char* tok : {"0xFF", "1_000", "3.14", "42LL"}) {
        const size_t at = code.find(tok);
        DOCTEST_REQUIRE(at != std::string::npos);
        DOCTEST_CHECK(exactRun(h, at, at + std::string(tok).size(), kNum));
    }
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_python_basic,
              "md_keyword: Python 关键字/# 注释/缩进无 run", {
    const std::string code = "def foo(x):\n    return x + 1  # add";
    const auto h = lex(code, "python");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 3, kKw));  // def
    const size_t ret = code.find("return");
    DOCTEST_CHECK(exactRun(h, ret, ret + 6, kKw));
    const size_t one = code.find("1  #");
    DOCTEST_CHECK(exactRun(h, one, one + 1, kNum));
    const size_t cmt = code.find("# add");
    DOCTEST_CHECK(exactRun(h, cmt, code.size(), kCmt));
    // 缩进空格不在任何 run 内（「空行无 run」结构同源）
    DOCTEST_CHECK(runAt(h, ret - 1) == nullptr);  // return 前是空格
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_python_triple,
              "md_keyword: Python 三引号串跨行（按行切同色 run）", {
    const std::string code = "s = \"\"\"line1\nline2\"\"\"\nn = 7";
    const auto h = lex(code, "python");
    const size_t open = code.find("\"\"\"");
    DOCTEST_CHECK(colorEq(runAt(h, open + 1)->color, kStr));
    DOCTEST_CHECK(colorEq(runAt(h, open - 2)->color, kCode));  // "s =" 默认段
    DOCTEST_CHECK(colorEq(runAt(h, code.find("line2"))->color, kStr));
    const size_t close = code.find("\"\"\"", open + 3);
    DOCTEST_CHECK(colorEq(runAt(h, close + 2)->color, kStr));
    const size_t seven = code.find("7");
    DOCTEST_CHECK(exactRun(h, seven, seven + 1, kNum));
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_python_prefix,
              "md_keyword: Python r/f/b 前缀串并入 string run", {
    const std::string code = "a = rb\"x\" + f'y' + 1j";
    const auto h = lex(code, "python");
    const size_t rb = code.find("rb\"x\"");
    DOCTEST_CHECK(exactRun(h, rb, rb + 5, kStr));  // 前缀 + 串整段
    const size_t fy = code.find("f'y'");
    DOCTEST_CHECK(exactRun(h, fy, fy + 4, kStr));
    const size_t j = code.find("1j");
    DOCTEST_CHECK(exactRun(h, j, j + 2, kNum));    // 复数字面量
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_fallback,
              "md_keyword: 未覆盖 lang / 空 lang → supported=false 且零 run", {
    const std::string code = "SELECT * FROM t WHERE x = 1; // not cpp";
    for (const char* lang : {"", "text", "ruby", "markdown", "TXT", "pytohn"}) {
        const auto h = lex(code, lang);
        DOCTEST_CHECK(!h.supported);
        DOCTEST_CHECK(h.runs.empty());
        DOCTEST_CHECK(!mdKeywordLangSupported(lang));
    }
    return true;
})

REGISTER_TEST(md_keyword_alias_case,
              "md_keyword: 语言别名大小写不敏感", {
    for (const char* lang : {"cpp", "CPP", "C++", "cc", "hpp", "c", "c++"}) {
        DOCTEST_CHECK(mdKeywordLangSupported(lang));
    }
    for (const char* lang : {"python", "PY", "Python3", "py"}) {
        DOCTEST_CHECK(mdKeywordLangSupported(lang));
    }
    for (const char* lang : {"js", "javascript", "JS", "mjs"}) {
        DOCTEST_CHECK(mdKeywordLangSupported(lang));
    }
    for (const char* lang : {"go", "golang", "GO"}) {
        DOCTEST_CHECK(mdKeywordLangSupported(lang));
    }
    for (const char* lang : {"rs", "rust", "RUST"}) {
        DOCTEST_CHECK(mdKeywordLangSupported(lang));
    }
    for (const char* lang : {"sh", "bash", "shell", "zsh"}) {
        DOCTEST_CHECK(mdKeywordLangSupported(lang));
    }
    return true;
})

REGISTER_TEST(md_keyword_unclosed_string,
              "md_keyword: 单行串未闭合 → 漏标回默认，下一行不受污染", {
    const std::string code = "x = \"oops\ny = 2";
    const auto h = lex(code, "cpp");
    DOCTEST_CHECK(h.supported);
    for (const auto& r : h.runs) {
        DOCTEST_CHECK(!colorEq(r.color, kStr));  // 无 string run（漏标）
    }
    const size_t two = code.find("2");
    DOCTEST_CHECK(exactRun(h, two, two + 1, kNum));  // 次行数字仍正常
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_struct,
              "md_keyword: 混合脏输入 runs 升序不重叠非空白全覆盖", {
    const std::string code = "def ??@@ ## $$ {{ }} /*  \"\"\"  '' \\\n"
                             "x1 = 0x 1e 3.. 'a\" 123 #c\ny = 'z'";
    const auto h = lex(code, "python");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(structInvariants(code, h));
    const auto c = lex(code, "cpp");
    DOCTEST_CHECK(c.supported);
    DOCTEST_CHECK(structInvariants(code, c));
    return true;
})

REGISTER_TEST(md_keyword_color_table,
              "md_keyword: 色表 4 新 token = 既有语义色，浅/深同源派生", {
    module::markdown::MdHighlightColors d{};
    module::markdown::MdHighlightColors l{};
    core::theme::setDark();
    d = mdHighlightColors(core::theme::current());
    core::theme::setLight();
    l = mdHighlightColors(core::theme::current());
    core::theme::setDark();  // 恢复默认深色（与既有测试基线一致）
    const auto dark = core::theme::current();
    DOCTEST_CHECK(colorEq(d.codeKeyword, dark.colors.info));
    DOCTEST_CHECK(colorEq(d.codeString, dark.colors.warning));
    DOCTEST_CHECK(colorEq(d.codeComment, dark.colors.textTertiary));
    DOCTEST_CHECK(colorEq(d.codeNumber, dark.colors.primaryText));
    // 浅色档同样派生且与深色不同（两档自动适配）
    DOCTEST_CHECK(colorEq(l.codeKeyword, l.codeComment) == false);
    DOCTEST_CHECK(colorEq(l.codeKeyword, d.codeKeyword) == false);
    DOCTEST_CHECK(colorEq(l.codeNumber, d.codeNumber) == false);
    return true;
})

REGISTER_TEST(md_keyword_js_basic,
              "md_keyword: JS 关键字/字符串/行注释/反引号串跨行", {
    const std::string code = "let x = \"hi\"; // c\n`tmpl\n` + 42;";
    const auto h = lex(code, "js");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 3, kKw));                       // let
    DOCTEST_CHECK(colorEq(runAt(h, code.find("\"hi\""))->color, kStr));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("// c"))->color, kCmt));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("tmpl"))->color, kStr));  // 反引号串跨行
    DOCTEST_CHECK(exactRun(h, code.find("42"), code.find("42") + 2, kNum));
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_json_basic,
              "md_keyword: JSON 字面量/字符串/数字（无注释语言）", {
    const std::string code = "{\"k\": true, \"n\": null, \"ok\": false, \"v\": 3.14}";
    const auto h = lex(code, "json");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, code.find("true"), code.find("true") + 4, kKw));
    DOCTEST_CHECK(exactRun(h, code.find("null"), code.find("null") + 4, kKw));
    DOCTEST_CHECK(exactRun(h, code.find("false"), code.find("false") + 5, kKw));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("\"k\""))->color, kStr));
    DOCTEST_CHECK(exactRun(h, code.find("3.14"), code.find("3.14") + 4, kNum));
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_java_basic,
              "md_keyword: Java 关键字/字符串/块注释", {
    const std::string code = "public class A { /* c */ int x = 1; }";
    const auto h = lex(code, "java");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 6, kKw));  // public
    DOCTEST_CHECK(colorEq(runAt(h, code.find("class"))->color, kKw));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("/* c */"))->color, kCmt));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("int"))->color, kKw));
    DOCTEST_CHECK(exactRun(h, code.find("1;"), code.find("1;") + 1, kNum));
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_go_backtick,
              "md_keyword: Go 关键字/反引号原始串跨行", {
    const std::string code = "func main() {\n s := `raw\nstr` }";
    const auto h = lex(code, "go");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 4, kKw));   // func
    DOCTEST_CHECK(colorEq(runAt(h, code.find("raw"))->color, kStr));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("str"))->color, kStr));
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_rust_basic,
              "md_keyword: Rust 关键字/字符串/行注释/块注释", {
    const std::string code = "let n = 42; // num\n/* block */ fn";
    const auto h = lex(code, "rust");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 3, kKw));   // let
    DOCTEST_CHECK(exactRun(h, code.find("42"), code.find("42") + 2, kNum));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("num"))->color, kCmt));   // 行注释
    DOCTEST_CHECK(colorEq(runAt(h, code.find("block"))->color, kCmt)); // 块注释
    DOCTEST_CHECK(colorEq(runAt(h, code.find("fn"))->color, kKw));     // 块注释后 fn
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})

REGISTER_TEST(md_keyword_shell_hash,
              "md_keyword: Shell 关键字/# 注释/字符串", {
    const std::string code = "if true; then\n echo \"x\" # c\nfi";
    const auto h = lex(code, "bash");
    DOCTEST_CHECK(h.supported);
    DOCTEST_CHECK(exactRun(h, 0, 2, kKw));   // if
    DOCTEST_CHECK(colorEq(runAt(h, code.find("then"))->color, kKw));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("\"x\""))->color, kStr));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("# c"))->color, kCmt));
    DOCTEST_CHECK(colorEq(runAt(h, code.find("fi"))->color, kKw));
    DOCTEST_CHECK(structInvariants(code, h));
    return true;
})
