// HTML→Markdown 转换器(B2,spec §7 与 §10 批次 2):结构转换、实体、容错、截断。
// 每条用例注明防的事故;描述含 ASCII "mcp" 供 --filter 定向。
#include "doctest.h"

#include <cstddef>
#include <string>

#include "module_mcp/html_to_markdown.h"

namespace {

using module::mcp::htmlToMarkdown;

// 构造超 2 MiB 的输入(转换器应在 2 MiB 处截断)
std::string oversizeInput() {
    return "<p>start</p><!--" + std::string(2 * 1024 * 1024 + 100, 'a') + "-->";
}

} // namespace

REGISTER_TEST(mcp_html_headings_h1_to_h6,
              "mcp html: h1-h6 → #..######(防层级丢失)", {
    const std::string out = htmlToMarkdown(
        "<h1>一</h1><h2>two</h2><h3>t3</h3><h4>t4</h4><h5>t5</h5><h6>t6</h6>");
    DOCTEST_CHECK(out.find("# 一") != std::string::npos);
    DOCTEST_CHECK(out.find("## two") != std::string::npos);
    DOCTEST_CHECK(out.find("### t3") != std::string::npos);
    DOCTEST_CHECK(out.find("#### t4") != std::string::npos);
    DOCTEST_CHECK(out.find("##### t5") != std::string::npos);
    DOCTEST_CHECK(out.find("###### t6") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_html_paragraph_div_br_hr,
              "mcp html: p/div 分段、br 换行、hr 分隔线(防结构拍平)", {
    const std::string out =
        htmlToMarkdown("<p>one</p><div>two</div><p>a<br>b</p><hr>");
    DOCTEST_CHECK_EQ(out, std::string("one\n\ntwo\n\na\nb\n\n---"));
    return true;
})

REGISTER_TEST(mcp_html_lists_nested_indent_and_ol_numbering,
              "mcp html: 嵌套列表 2 空格缩进、ol 序号(防嵌套层级与序号丢失)", {
    const std::string out = htmlToMarkdown(
        "<ul><li>a<ul><li>a1</li><li>a2</li></ul></li><li>b</li></ul>"
        "<ol><li>x</li><li>y</li></ol>");
    DOCTEST_CHECK_EQ(out,
                     std::string("- a\n  - a1\n  - a2\n- b\n\n1. x\n2. y"));
    return true;
})

REGISTER_TEST(mcp_html_blockquote_nested_prefix,
              "mcp html: blockquote 前缀与嵌套重复前缀", {
    const std::string out = htmlToMarkdown(
        "<blockquote>q1<blockquote>q2</blockquote></blockquote>");
    DOCTEST_CHECK(out.find("> q1") != std::string::npos);
    DOCTEST_CHECK(out.find("> > q2") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_html_pre_fence_and_inline_code,
              "mcp html: pre 围栏且内部空白原文保留、code 行内反引号(防代码排版丢失)", {
    const std::string out = htmlToMarkdown(
        "<pre>def f():\n    return 1\n</pre><p>use <code>f()</code> now</p>");
    DOCTEST_CHECK_EQ(out,
                     std::string("```\ndef f():\n    return 1\n```\n\nuse `f()` now"));
    return true;
})

REGISTER_TEST(mcp_html_link_and_image,
              "mcp html: a→[text](href)、img→![alt](src)、实体在属性中解码、相对地址保留", {
    const std::string out = htmlToMarkdown(
        "<a href=\"https://e.test/x?a=1&amp;b=2\">link</a>"
        "<img src=\"/i.png\" alt=\"pic\"><p>t</p>");
    DOCTEST_CHECK(out.find("[link](https://e.test/x?a=1&b=2)") != std::string::npos);
    DOCTEST_CHECK(out.find("![pic](/i.png)") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_html_strong_em_markers,
              "mcp html: strong/b → **、em/i → *", {
    const std::string out = htmlToMarkdown(
        "<p>a <strong>big</strong> and <em>it</em> <b>b</b> <i>i</i></p>");
    DOCTEST_CHECK_EQ(out, std::string("a **big** and *it* **b** *i*"));
    return true;
})

REGISTER_TEST(mcp_html_table_header_separator_pipe_escape,
              "mcp html: 基础 pipe 表、首行表头、单元格 | 转义(防表格结构与内容串列)", {
    const std::string out = htmlToMarkdown(
        "<table><tr><th>h1</th><th>h|2</th></tr><tr><td>a</td><td>c</td></tr>"
        "<tr><td>x</td></tr></table>");
    DOCTEST_CHECK_EQ(out, std::string("| h1 | h\\|2 |\n| --- | --- |\n"
                                     "| a | c |\n| x |  |"));
    return true;
})

REGISTER_TEST(mcp_html_entities_named_and_numeric,
              "mcp html: 命名与数字(十/十六进制)实体解码(防实体原样漏给模型)", {
    const std::string out = htmlToMarkdown(
        "<p>&amp;&lt;&gt;&quot;&#39;&#x27;&#65;&nbsp;end</p>");
    DOCTEST_CHECK_EQ(out,
                  std::string("&<>\"''A\xC2\xA0") + "end");  // 十六进制转义后接字母须断开字面量
    return true;
})

REGISTER_TEST(mcp_html_strips_script_style_comment_keeps_title,
              "mcp html: script/style/注释剔除、head 剔除但保留 title(防标签噪声进上下文)", {
    const std::string out = htmlToMarkdown(
        "<html><head><meta charset=\"utf-8\"><title>My Tit</title>"
        "<style>p{}</style></head><body><script>var x=1<2;</script>"
        "<p>body text</p><!-- hidden --></body></html>");
    DOCTEST_CHECK(out.find("# My Tit") != std::string::npos);
    DOCTEST_CHECK(out.find("body text") != std::string::npos);
    DOCTEST_CHECK(out.find("var x") == std::string::npos);
    DOCTEST_CHECK(out.find("p{}") == std::string::npos);
    DOCTEST_CHECK(out.find("hidden") == std::string::npos);
    DOCTEST_CHECK(out.find("<p>") == std::string::npos);
    return true;
})

REGISTER_TEST(mcp_html_title_fallback_only_without_h1,
              "mcp html: 无 h1 时 title 兜底、有 h1 时不兜底(防标题缺失/重复)", {
    const std::string withFallback = htmlToMarkdown(
        "<html><head><title>Tt</title></head><body><p>x</p></body></html>");
    DOCTEST_CHECK_EQ(withFallback, std::string("# Tt\n\nx"));
    const std::string noFallback = htmlToMarkdown(
        "<html><head><title>Tt</title></head><body><h1>Real</h1><p>x</p></body></html>");
    DOCTEST_CHECK_EQ(noFallback, std::string("# Real\n\nx"));
    return true;
})

REGISTER_TEST(mcp_html_unclosed_tags_never_throw,
              "mcp html: 未闭合/非法标签按文本容错、绝不抛异常(防转换崩溃)", {
    DOCTEST_CHECK_EQ(htmlToMarkdown("<p>abc"), std::string("abc"));
    DOCTEST_CHECK(htmlToMarkdown("<b>x").find("x") != std::string::npos);
    DOCTEST_CHECK_EQ(htmlToMarkdown("a < b and c"), std::string("a < b and c"));
    DOCTEST_CHECK(htmlToMarkdown("<a href='q'>y").find("[y") != std::string::npos);  // 未闭合 a:容错输出,不补全链接
    DOCTEST_CHECK(htmlToMarkdown("<a href='q'>y</a>").find("[y](q)") != std::string::npos);
    DOCTEST_CHECK_EQ(htmlToMarkdown("</div>"), std::string(""));
    DOCTEST_CHECK(htmlToMarkdown("<div><span>z</span>").find("z") != std::string::npos);
    DOCTEST_CHECK_EQ(htmlToMarkdown("<"), std::string("<"));  // 裸 '<' 按文本
    return true;
})

REGISTER_TEST(mcp_html_whitespace_collapsed_and_blank_line_folded,
              "mcp html: 连续空白折叠、空行最多一保留、行尾空白去除(防排版噪声)", {
    const std::string out = htmlToMarkdown(
        "<p>A  \n\t B</p>\n\n\n<p>C</p><p>trailing  </p>");
    DOCTEST_CHECK_EQ(out, std::string("A B\n\nC\n\ntrailing"));
    return true;
})

REGISTER_TEST(mcp_html_input_over_2mib_truncated,
              "mcp html: 输入超 2 MiB 先截断(防超长响应烧爆转换与上下文)", {
    const std::string out = htmlToMarkdown(oversizeInput());
    DOCTEST_CHECK_EQ(out, std::string("start"));
    return true;
})

REGISTER_TEST(mcp_html_empty_input_yields_empty,
              "mcp html: 空输入/纯空白输出空串(由工具层转『未提取到文本』提示)", {
    DOCTEST_CHECK(htmlToMarkdown("").empty());
    DOCTEST_CHECK(htmlToMarkdown("<p>   </p>").empty());
    return true;
})
