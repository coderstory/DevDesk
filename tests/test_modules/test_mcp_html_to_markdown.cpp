// HTML→Markdown 转换器(B2,spec §7 与 §10 批次 2):结构转换、实体、容错、截断。
// 每条用例注明防的事故;描述含 ASCII "mcp" 供 --filter 定向。
#include "doctest.h"

#include <cstddef>
#include <cstdio>
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
              "mcp html: 嵌套列表 4 空格缩进、ol 序号(防嵌套层级与序号丢失)", {
    const std::string out = htmlToMarkdown(
        "<ul><li>a<ul><li>a1</li><li>a2</li></ul></li><li>b</li></ul>"
        "<ol><li>x</li><li>y</li></ol>");
    DOCTEST_CHECK_EQ(out,
                     std::string("- a\n    - a1\n    - a2\n- b\n\n1. x\n2. y"));
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
    // 闭合链接独立成块 = 高链接密度短块,主干模式按 R2 判样板丢弃;关断保底留旧口径
    DOCTEST_CHECK(htmlToMarkdown("<a href='q'>y</a>", false).find("[y](q)") != std::string::npos);
    DOCTEST_CHECK(htmlToMarkdown("<a href='q'>y</a>").find("[y](q)") == std::string::npos);
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

REGISTER_TEST(mcp_html_inline_markdown_escape_table,
              "mcp html: 行内转义表(turndown 口径)——起点行首标记与任意位置标记符,防正文被下游渲染器重解释", {
    struct Case {
        const char* html;
        const char* markdown;
    };
    const Case cases[] = {
        // 片段起点行首标记
        {"<p>- item</p>", "\\- item"},
        {"<p>+ plus</p>", "\\+ plus"},
        {"<p>3. install</p>", "3\\. install"},
        {"<p># heading</p>", "\\# heading"},
        {"<p>> quote</p>", "\\> quote"},
        {"<p>=== divider</p>", "\\=== divider"},
        {"<p>~~~fence</p>", "\\~~~fence"},
        // 任意位置标记符
        {"<p>*em*</p>", "\\*em\\*"},
        {"<p>a_b_c</p>", "a\\_b\\_c"},
        {"<p>[link](x)</p>", "\\[link\\](x)"},
        // 起点规则不误伤
        {"<p>3.14 与 -1</p>", "3.14 与 -1"},          // 无后随空格/非起点
        {"<p>#tag</p>", "#tag"},                        // 井号后无空格
        // 码 span 内容不转义(CommonMark 码 span 内反斜杠不生效)
        {"<p>use <code>a*b_c</code> now</p>", "use `a*b_c` now"},
        // 中文正文零触碰：转义表只含 ASCII 标记符；# 不在节点起点则不在
        // ^(#{1,6})␣ 规则覆盖内，原样保留（同 turndown）
        {"<p>中文标题</p>", "中文标题"},
        {"<p>中文 # 标记 - 列表</p>", "中文 # 标记 - 列表"},
    };
    for (const Case& c : cases) {
        const std::string out = htmlToMarkdown(c.html);
        if (out != std::string(c.markdown)) {
            std::fprintf(stderr, "html=[%s] got=[%s] want=[%s]\n", c.html,
                         out.c_str(), c.markdown);
            DOCTEST_REQUIRE(false);
        }
    }
    return true;
})

REGISTER_TEST(mcp_html_nested_ordered_list_4space_indent,
              "mcp html: 嵌套有序列表 4 空格缩进(2 空格在严格 CommonMark 渲染器下被展平)", {
    const std::string out = htmlToMarkdown(
        "<ol><li>one<ol><li>one-one<ol><li>deep</li></ol></li>"
        "<li>one-two</li></ol></li><li>two</li></ol>");
    DOCTEST_CHECK_EQ(out,
                     std::string("1. one\n    1. one-one\n        1. deep\n"
                                 "    2. one-two\n2. two"));
    // ul 嵌套同口径,混排时层级可预期
    const std::string mixed = htmlToMarkdown(
        "<ol><li>keep<ul><li>sub</li></ul></li></ol>");
    DOCTEST_CHECK_EQ(mixed, std::string("1. keep\n    - sub"));
    return true;
})

// ── 正文主干提取(CE 批,spec §7;规则与词表出处 = 蓝图
// docs/superpowers/research/2026-10-27-mcp-content-extraction-review.md §3)──
// 蓝图 §5 六金样(文档页/列表页/裸页/病理/链接密集/类名冲突)内嵌于本文件
// (批次范围不新增 fixture 目录)。每条金样双口径:extractMainContent=false 的
// 输出 = 引入本参数前的基线(逐字节差分保底,防"比现在差");= true 的输出 =
// 主干口径。描述含 ASCII "mcp" 供 --filter 定向。

REGISTER_TEST(mcp_html_ce_fixture_docs_content,
              "mcp html: CE 金样·文档页——R1 语义剔除、锚系数(0.3)保 TOC、pre 不受扰", {
    const std::string docs =
        "<html><head><title>工具调用指南</title></head><body>"
        "<header><a href=\"/\">首页</a><a href=\"/docs\">文档</a></header>"
        "<main><article><h1>工具调用指南</h1>"
        "<p>本文介绍工具调用。</p>"
        "<div class=\"toc\"><a href=\"#s1\">准备工作</a><a href=\"#s2\">开始调用</a></div>"
        "<h2 id=\"s1\">准备工作</h2>"
        "<p>先申请密钥。</p>"
        "<pre><code>curl -s https://api.example.com</code></pre>"
        "</article></main>"
        "<footer><p>版权所有 Example Inc</p><a href=\"/about\">关于我们</a></footer>"
        "</body></html>";
    // 关断保底:与基线逐字节一致
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(docs, false),
        std::string("[首页](/)[文档](/docs)\n\n"
                    "# 工具调用指南\n\n"
                    "本文介绍工具调用。\n\n"
                    "[准备工作](#s1)[开始调用](#s2)\n\n"
                    "## 准备工作\n\n"
                    "先申请密钥。\n\n"
                    "```\ncurl -s https://api.example.com\n"
                    "```\n\n"
                    "版权所有 Example Inc\n\n"
                    "[关于我们](/about)"));
    // 主干:站头/页脚整体消失;TOC 锚链接密度 0.3≤0.5 存活;正文与代码块完整
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(docs),
        std::string("# 工具调用指南\n\n"
                    "本文介绍工具调用。\n\n"
                    "[准备工作](#s1)[开始调用](#s2)\n\n"
                    "## 准备工作\n\n"
                    "先申请密钥。\n\n"
                    "```\ncurl -s https://api.example.com\n```"));
    return true;
})

REGISTER_TEST(mcp_html_ce_fixture_portal_list,
              "mcp html: CE 金样·列表页——R1b 类名剔除、R3 锚定保列表项、同名 div 嵌套计数", {
    const std::string portal =
        "<html><head><title>开源资讯</title></head><body>"
        "<div id=\"site-header\"><a href=\"/\"><img src=\"/logo.png\" alt=\"LOGO\"></a>"
        "<div class=\"nav-menu\"><a href=\"/news\">资讯</a><a href=\"/blog\">博客</a></div></div>"
        "<main><h1>开源资讯</h1>"
        "<ul class=\"news-list\">"
        "<li><a href=\"/p/1\">新版本发布</a><p>社区迎来新版本。</p></li>"
        "<li><a href=\"/p/2\">大会开幕</a><p>年度大会今日开幕。</p></li>"
        "</ul></main>"
        "<div id=\"site-footer\"><a href=\"/about\">关于</a> <a href=\"/links\">友情链接</a></div>"
        "</body></html>";
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(portal, false),
        std::string("[![LOGO](/logo.png)](/)\n\n"
                    "[资讯](/news)[博客](/blog)\n\n"
                    "# 开源资讯\n\n"
                    "- [新版本发布](/p/1)\n\n"
                    "- 社区迎来新版本。\n\n"
                    "- [大会开幕](/p/2)\n\n"
                    "- 年度大会今日开幕。\n\n"
                    "[关于](/about) [友情链接](/links)"));
    // 主干:列表项密度 1.0 但在 <main> 锚定区内(R2 暂停)→ 存活;站头页脚(类名
    // header/footer 命中)消失;内层 nav-menu </div> 不提前退栈(同名嵌套计数)
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(portal),
        std::string("# 开源资讯\n\n"
                    "- [新版本发布](/p/1)\n\n"
                    "- 社区迎来新版本。\n\n"
                    "- [大会开幕](/p/2)\n\n"
                    "- 年度大会今日开幕。"));
    return true;
})

REGISTER_TEST(mcp_html_ce_fixture_bare_page,
              "mcp html: CE 金样·裸页——无语义无类名线索,规则不触发,开关两态逐字节一致", {
    const std::string bare =
        "<h1>标题一</h1><p>第一段文字。</p>"
        "<p>第二段<a href=\"https://e.test/a\">内链</a>正文继续,足够长不影响判定,"
        "这里补足字数让密度降到阈值之下。</p>";
    // 回退保证:规则全不命中时开与关输出一致(蓝图判收:bare-page 零变化)
    DOCTEST_CHECK_EQ(htmlToMarkdown(bare), htmlToMarkdown(bare, false));
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(bare, false),
        std::string("# 标题一\n\n"
                    "第一段文字。\n\n"
                    "第二段[内链](https://e.test/a)正文继续,足够长不影响判定,"
                    "这里补足字数让密度降到阈值之下。"));
    return true;
})

REGISTER_TEST(mcp_html_ce_fixture_malformed,
              "mcp html: CE 金样·病理——未闭合 header 吞区遇 main 信号释放、乱序闭合不漏样板", {
    const std::string malformed =
        "<header><span>未闭合的站头导航文字</span>\n"
        "<main><h1>真正文标题</h1><p>正文第一段。</p></main>";
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(malformed, false),
        std::string("未闭合的站头导航文字\n\n# 真正文标题\n\n正文第一段。"));
    // 双保险之一:抑制中遇 <main>(正文信号)→ 判抑制起点残缺,清栈放行正文
    DOCTEST_CHECK_EQ(htmlToMarkdown(malformed),
                     std::string("# 真正文标题\n\n正文第一段。"));
    // 乱序闭合:</nav> 无匹配层不动栈,</div> 才退栈,样板不漏出
    const std::string disordered = "<div id=\"footer\">样板内容</nav></div>正文";
    DOCTEST_CHECK_EQ(htmlToMarkdown(disordered, false),
                     std::string("样板内容\n\n正文"));
    DOCTEST_CHECK_EQ(htmlToMarkdown(disordered), std::string("正文"));
    return true;
})

REGISTER_TEST(mcp_html_ce_fixture_link_heavy,
              "mcp html: CE 金样·链接密集——超 150 字符块与含图块存活,开关两态零变化", {
    std::string heavy = "<h1>更新日志</h1><p>";
    std::string linksMd;
    for (int i = 0; i < 30; ++i) {
        const std::string n = std::to_string(i);
        heavy += "<a href=\"/t/" + n + "\">话题条目四字</a>";
        linksMd += "[话题条目四字](/t/" + n + ")";
    }
    heavy += "以上为社区讨论入口汇总,供按需取用不再列举。</p>"
             "<p><a href=\"/c\">封面图<img src=\"/cover.png\" alt=\"\"></a></p>";
    // 密度 0.89>0.5 但 202 字符>150(长度保险丝);封面块 3 字符密度 1.0 但含图
    //(视觉保险丝)——蓝图判收:link-heavy 零变化
    DOCTEST_CHECK_EQ(htmlToMarkdown(heavy), htmlToMarkdown(heavy, false));
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(heavy, false),
        std::string("# 更新日志\n\n" + linksMd +
                    "以上为社区讨论入口汇总,供按需取用不再列举。\n\n"
                    "[封面图![](/cover.png)](/c)"));
    return true;
})

REGISTER_TEST(mcp_html_ce_fixture_class_keyword,
              "mcp html: CE 金样·类名冲突——kMaybeTokens 正词保险丝存疑不删、负面词表删", {
    const std::string cls =
        "<div class=\"nav-menu\"><a href=\"/n\">导航链接</a></div>"
        "<div class=\"sidebar-layout main-wrap\"><h1>类名冲突标题</h1>"
        "<p>冲突类名正文保留。</p></div>"
        "<div id=\"copyright\">版权信息行</div>";
    DOCTEST_CHECK_EQ(
        htmlToMarkdown(cls, false),
        std::string("[导航链接](/n)\n\n"
                    "# 类名冲突标题\n\n"
                    "冲突类名正文保留。\n\n"
                    "版权信息行"));
    // sidebar-layout 命中负面词表但同时命中正词 main → 保险丝放行;nav-menu/
    // copyright 无正词 → 抑制
    DOCTEST_CHECK_EQ(htmlToMarkdown(cls),
                     std::string("# 类名冲突标题\n\n冲突类名正文保留。"));
    return true;
})

REGISTER_TEST(mcp_html_ce_r2_ordered_list_renumber,
              "mcp html: R2 丢弃有序列表项回退编号(防跳号)、未闭合链接保险丝", {
    // 交互点②:li1 整块丢弃 → next-- → li2 仍从 1. 起
    const std::string ol = "<ol><li><a href=\"/1\">项一</a></li><li>正文项</li></ol>";
    DOCTEST_CHECK_EQ(htmlToMarkdown(ol, false),
                     std::string("1. [项一](/1)\n2. 正文项"));
    DOCTEST_CHECK_EQ(htmlToMarkdown(ol), std::string("1. 正文项"));
    // 交互点①伴侣:未闭合 <a>(linkDepth_>0)= 病理输入,存疑不删,保持容错契约
    const std::string unclosed = "<ul><li><a href=\"/x\">断链项";
    DOCTEST_CHECK(htmlToMarkdown(unclosed).find("断链项") != std::string::npos);
    return true;
})

REGISTER_TEST(mcp_html_ce_r2_code_span_state_reset,
              "mcp html: R2 丢弃块随块复位 inCodeSpan_(防半截码 span 把后续正文带成不转义)", {
    // 链接内未闭合 <code>:块密度 1.0 → 丢弃;复位后下一块的 * 必须正常转义
    const std::string cs = "<p><a href=\"/x\">工具链<code>abc</a></p><p>*not-em*</p>";
    DOCTEST_CHECK_EQ(htmlToMarkdown(cs, false),
                     std::string("[工具链`abc](/x)\n\n*not-em*"));
    DOCTEST_CHECK_EQ(htmlToMarkdown(cs), std::string("\\*not-em\\*"));
    return true;
})

REGISTER_TEST(mcp_html_ce_r1_form_button_select,
              "mcp html: R1 交互性标签(form/select/button)整块剔除、svg 同口径", {
    const std::string form =
        "<form><input name=\"q\"><select><option>a</option></select>"
        "<button>搜索</button></form><p>保留段</p>";
    DOCTEST_CHECK_EQ(htmlToMarkdown(form, false),
                     std::string("a搜索\n\n保留段"));
    DOCTEST_CHECK_EQ(htmlToMarkdown(form), std::string("保留段"));
    const std::string svg =
        "<svg><text>图标文字</text></svg><p>图外正文</p>";
    DOCTEST_CHECK_EQ(htmlToMarkdown(svg, false),
                     std::string("图标文字\n\n图外正文"));
    DOCTEST_CHECK_EQ(htmlToMarkdown(svg), std::string("图外正文"));
    return true;
})

REGISTER_TEST(mcp_html_ce_r1_unclosed_suppress_64kib_release,
              "mcp html: R1 双保险之二——抑制区超 64KiB 硬释放,后续正文不吞", {
    std::string giant = "<header><nav>导航";
    std::string offBody = "导航";
    for (int i = 0; i < 7000; ++i) {
        giant += "样板文字块";
        offBody += "样板文字块";
    }
    giant += "</nav><p>末尾存活段</p>";
    DOCTEST_CHECK_EQ(htmlToMarkdown(giant, false),
                     offBody + "\n\n末尾存活段");
    DOCTEST_CHECK_EQ(htmlToMarkdown(giant), std::string("末尾存活段"));
    return true;
})

REGISTER_TEST(mcp_html_ce_r4_role_and_aria_hidden,
              "mcp html: R4 role 词表与 aria-hidden/hidden 容器剔除(蓝图 R4,容器闸防栈滞留)", {
    const std::string r4 =
        "<div role=\"navigation\"><a href=\"/r\">角色导航</a></div>"
        "<div aria-hidden=\"true\">隐藏装饰</div>"
        "<p>可见正文</p>";
    DOCTEST_CHECK_EQ(htmlToMarkdown(r4, false),
                     std::string("[角色导航](/r)\n\n隐藏装饰\n\n可见正文"));
    DOCTEST_CHECK_EQ(htmlToMarkdown(r4), std::string("可见正文"));
    return true;
})
