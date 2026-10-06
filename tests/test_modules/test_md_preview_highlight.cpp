// tests/test_modules/test_md_preview_highlight.cpp
// 预览代码块高亮单测：逐行 code run / 受支持 lang 多色 / 与编辑器 MdHighlighter 在 fallback
// 路径上同源等价 / 脏块缓存 (stableId, hash) 的命中与作废 / 非单围栏防御 / CRLF /
// info 取首词 / 耗时记录。
// 跨文件前提：
//   - 预览侧对受支持 lang 出多色，编辑器侧 MdHighlighter 仍逐行单色。这是有意分叉不是 bug，
//     「预览 == 编辑器」只在未覆盖 lang 的 fallback 路径成立，不要拿它当回归去对齐编辑器。
#include "doctest.h"
#include "core/theme.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_highlight.h"
#include "module_markdown/md_preview_highlight.h"

#include <eui/types.h>

#include <chrono>
#include <ratio>
#include <string>
#include <vector>

using module::markdown::MdHighlightColors;
using module::markdown::MdHighlighter;
using module::markdown::MdBlockKind;
using module::markdown::MdPreviewCodeCache;
using module::markdown::MdPreviewCodeHighlight;
using module::markdown::mdPreviewCodeHighlight;
using module::markdown::mdHighlightColors;
using module::markdown::segmentMarkdownBlocks;

namespace {

// 测试色表：值本身任意可辨，只要求两两不同
const eui::Color kMark{1.0f, 0.0f, 0.0f, 1.0f};
const eui::Color kCode{0.0f, 1.0f, 0.0f, 1.0f};
const eui::Color kLink{0.0f, 0.0f, 1.0f, 1.0f};
const eui::Color kList{1.0f, 1.0f, 0.0f, 1.0f};
const eui::Color kQuote{1.0f, 0.0f, 1.0f, 1.0f};
// 受支持 lang 的多色 token
const eui::Color kKw{0.2f, 0.2f, 0.9f, 1.0f};
const eui::Color kStr{0.9f, 0.6f, 0.1f, 1.0f};
const eui::Color kCmt{0.5f, 0.5f, 0.5f, 1.0f};
const eui::Color kNum{0.8f, 0.3f, 0.2f, 1.0f};

MdHighlightColors testColors() {
    return MdHighlightColors{kMark, kCode, kLink, kList, kQuote,
                             kKw,   kStr,  kCmt,  kNum};
}

bool colorEq(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t)
        .count();
}

// displayText 中 byte 所在 run（无 = nullptr）
const eui::TextRun* runAtByte(const MdPreviewCodeHighlight& hl, size_t byte) {
    for (const auto& r : hl.runs) {
        if (static_cast<size_t>(r.byteStart) <= byte &&
            byte < static_cast<size_t>(r.byteEnd)) {
            return &r;
        }
    }
    return nullptr;
}

// 块源码（按块模型切出的 CodeFence 块）
std::string fenceSourceOf(const std::string& doc) {
    const auto blocks = segmentMarkdownBlocks(doc);
    for (const auto& b : blocks) {
        if (b.kind == MdBlockKind::CodeFence) {
            return doc.substr(b.start, b.end - b.start);
        }
    }
    return "";
}

// displayText 中恰为 [from,to) 的 run（无 = nullptr）
const eui::TextRun* exactRun(const MdPreviewCodeHighlight& hl, size_t from,
                             size_t to) {
    for (const auto& r : hl.runs) {
        if (static_cast<size_t>(r.byteStart) == from &&
            static_cast<size_t>(r.byteEnd) == to) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

REGISTER_TEST(md_preview_hl_basic,
              "md_preview_highlight: 不显示 lang 标签 + 未覆盖 lang 逐行 code run（fallback）", {
    const std::string src = "```text\nint a;\nint b;\n```\n";
    const auto hl = mdPreviewCodeHighlight(src, testColors());
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK_EQ(hl.lang, std::string("text"));
    DOCTEST_CHECK_EQ(hl.displayText, std::string("int a;\nint b;"));
    // lang 标签不显示：runs[0] 就是首内容行，没有 lang marker 色 run
    DOCTEST_REQUIRE(hl.runs.size() >= 1);
    DOCTEST_CHECK(colorEq(hl.runs[0].color, kCode));
    // 内容行整行 code 色；换行处无 run，"int a;" 占 [0,6)、"int b;" 占 [7,13)
    DOCTEST_CHECK(colorEq(runAtByte(hl, 0)->color, kCode));   // 首字节
    DOCTEST_CHECK(colorEq(runAtByte(hl, 5)->color, kCode));   // 行内
    DOCTEST_CHECK_EQ(runAtByte(hl, 0)->byteStart, 0);
    DOCTEST_CHECK_EQ(runAtByte(hl, 0)->byteEnd, 6);
    DOCTEST_CHECK_EQ(runAtByte(hl, 7)->byteEnd, 13);
    return true;
})

REGISTER_TEST(md_preview_hl_no_lang, "md_preview_highlight: 无 lang 无标签行", {
    const auto hl = mdPreviewCodeHighlight("```\nhello\n```\n", testColors());
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK(hl.lang.empty());
    DOCTEST_CHECK_EQ(hl.displayText, std::string("hello"));
    DOCTEST_REQUIRE_EQ(hl.runs.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(colorEq(hl.runs[0].color, kCode));
    return true;
})

REGISTER_TEST(md_preview_hl_editor_equivalence,
              "md_preview_highlight: 未覆盖 lang 与编辑器 MdHighlighter token 同源等价", {
    // 编辑器走全文档 runsForLine，围栏内容行给整行单 run（colors.code）；预览走块源码 +
    // displayText 坐标，两者行内字节区间可直接比。等价只在 fallback 路径成立：受支持 lang
    // 的预览多色是与编辑器（仍逐行单色）的有意分叉，见 md_preview_hl_cpp_multicolor。
    const std::string doc = "# Title\n\n```text\nx = 1\ny = 2\n```\n\ntail\n";
    const std::string src = fenceSourceOf(doc);
    DOCTEST_REQUIRE(!src.empty());

    MdHighlighter h;
    h.setColors(testColors());
    // "x = 1" 在 doc 里的偏移就是编辑器整行 run 的起点（全文档坐标系）
    const size_t docLineStart = doc.find("x = 1");
    const std::string line = "x = 1";
    const auto editorRuns = h.runsForLine(
        doc, line, static_cast<int>(docLineStart));
    DOCTEST_REQUIRE_EQ(editorRuns.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(colorEq(editorRuns[0].color, kCode));
    DOCTEST_CHECK_EQ(static_cast<size_t>(editorRuns[0].byteStart), docLineStart);
    DOCTEST_CHECK_EQ(static_cast<size_t>(editorRuns[0].byteEnd),
                     docLineStart + line.size());

    const auto hl = mdPreviewCodeHighlight(src, testColors());
    DOCTEST_REQUIRE(hl.ok);
    // displayText = "x = 1\ny = 2"，"x = 1" 的 run 行内区间 = [0, 5)
    const size_t dispStart = hl.displayText.find(line);
    DOCTEST_REQUIRE(dispStart != std::string::npos);
    const eui::TextRun* prevRun = runAtByte(hl, dispStart);
    DOCTEST_REQUIRE(prevRun != nullptr);
    DOCTEST_CHECK(colorEq(prevRun->color, editorRuns[0].color));
    DOCTEST_CHECK_EQ(static_cast<size_t>(prevRun->byteStart), dispStart);
    DOCTEST_CHECK_EQ(static_cast<size_t>(prevRun->byteEnd),
                     dispStart + line.size());

    // 色表映射的纯度检查：同一 theme 连续两次取值必须逐通道相同。它并不证明编辑器与预览
    // 真的同源——那要另去比对 MdHighlighter 内部的色表。
    const auto tokens = core::theme::current();
    const auto c1 = mdHighlightColors(tokens);
    const auto c2 = mdHighlightColors(tokens);
    DOCTEST_CHECK(colorEq(c1.code, c2.code));
    DOCTEST_CHECK(colorEq(c1.marker, c2.marker));
    DOCTEST_CHECK(colorEq(c1.code, tokens.colors.success));
    return true;
})

REGISTER_TEST(md_preview_hl_cpp_multicolor,
              "md_preview_highlight: cpp lang → 关键字/串/注释/数字多色 runs", {
    const std::string src = "```cpp\nint a = \"hi\"; // c\nint b = 7;\n```\n";
    const auto hl = mdPreviewCodeHighlight(src, testColors());
    DOCTEST_CHECK(hl.ok);
    const std::string disp = "int a = \"hi\"; // c\nint b = 7;";
    DOCTEST_CHECK_EQ(hl.displayText, disp);
    DOCTEST_REQUIRE(!hl.runs.empty());
    const size_t kw1 = disp.find("int");              // = 0
    DOCTEST_REQUIRE(kw1 != std::string::npos);
    DOCTEST_CHECK(colorEq(exactRun(hl, kw1, kw1 + 3)->color, kKw));
    const size_t str = disp.find("\"hi\"");           // = 12
    DOCTEST_CHECK(colorEq(exactRun(hl, str, str + 4)->color, kStr));
    const size_t cmt = disp.find("// c");             // = 18
    DOCTEST_CHECK(colorEq(exactRun(hl, cmt, cmt + 4)->color, kCmt));
    const size_t seven = disp.find("7;");             // = 31
    DOCTEST_CHECK(colorEq(exactRun(hl, seven, seven + 1)->color, kNum));
    // 默认段（标识符 / 运算符）= code 色
    const size_t aeq = disp.find("a =");
    DOCTEST_CHECK(colorEq(runAtByte(hl, aeq)->color, kCode));
    // 行末 \n 不进 run，与 fallback 路径同结构
    DOCTEST_CHECK(runAtByte(hl, 3) == nullptr);
    return true;
})

REGISTER_TEST(md_preview_hl_python_multicolor,
              "md_preview_highlight: python lang → 多色 + 编辑器有意分叉（编辑器仍单色）", {
    const std::string src = "```python\ndef f():  # go\n    return \"s\"\n```\n";
    const auto hl = mdPreviewCodeHighlight(src, testColors());
    DOCTEST_CHECK(hl.ok);
    const std::string disp = "def f():  # go\n    return \"s\"";
    DOCTEST_CHECK_EQ(hl.displayText, disp);
    const size_t df = disp.find("def");         // = 0
    DOCTEST_CHECK(colorEq(exactRun(hl, df, df + 3)->color, kKw));
    const size_t cmt = disp.find("# go");       // = 10
    DOCTEST_CHECK(colorEq(exactRun(hl, cmt, cmt + 4)->color, kCmt));
    const size_t ret = disp.find("return");     // = 19
    DOCTEST_CHECK(colorEq(exactRun(hl, ret, ret + 6)->color, kKw));
    const size_t str = disp.find("\"s\"");      // = 26
    DOCTEST_CHECK(colorEq(exactRun(hl, str, str + 3)->color, kStr));
    DOCTEST_CHECK(runAtByte(hl, 14) == nullptr);  // \n 间隙
    DOCTEST_CHECK(runAtByte(hl, 15) == nullptr);  // 缩进空格不入 run
    // 有意分叉：编辑器同文本仍是整行单 code 色（只改了预览侧）
    const std::string doc = "```python\ndef f():  # go\n```\n";
    MdHighlighter h;
    h.setColors(testColors());
    const auto er = h.runsForLine(doc, "def f():  # go", 10);
    DOCTEST_REQUIRE_EQ(er.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(colorEq(er[0].color, kCode));
    return true;
})

REGISTER_TEST(md_preview_hl_cache, "md_preview_highlight: 脏块缓存命中/重算/prune/主题作废", {
    MdPreviewCodeCache cache;
    cache.setColors(testColors());
    const std::string src = "```cpp\nint a;\n```\n";
    const uint64_t hash = module::markdown::mdBlockHash(src);

    DOCTEST_CHECK(cache.get(7, hash) == nullptr);
    cache.put(7, hash, mdPreviewCodeHighlight(src, testColors()));
    DOCTEST_REQUIRE(cache.get(7, hash) != nullptr);
    DOCTEST_CHECK(cache.get(7, hash)->ok);
    DOCTEST_CHECK(cache.get(7, hash + 1) == nullptr);  // hash 变了即脏块，须重算
    DOCTEST_CHECK(cache.get(8, hash) == nullptr);

    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(1));
    cache.put(8, hash + 100, mdPreviewCodeHighlight(src, testColors()));
    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(2));
    cache.pruneActive({7});
    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(cache.get(8, hash + 100) == nullptr);
    DOCTEST_CHECK(cache.get(7, hash) != nullptr);  // prune 只清名单外的块

    // 主题切换（色表变）→ 全部旧色条目作废
    MdHighlightColors other = testColors();
    other.marker.r = 0.5f;  // 任一通道变化即色表变化
    cache.setColors(other);
    DOCTEST_CHECK(cache.get(7, hash) == nullptr);
    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(0));
    // 同色表重复 set 不清
    cache.setColors(other);
    cache.put(9, hash, mdPreviewCodeHighlight(src, testColors()));
    cache.setColors(other);
    DOCTEST_CHECK(cache.get(9, hash) != nullptr);
    return true;
})

REGISTER_TEST(md_preview_hl_defensive, "md_preview_highlight: 非单围栏源码 ok=false", {
    const auto hl = mdPreviewCodeHighlight("just a paragraph\n", testColors());
    DOCTEST_CHECK(!hl.ok);
    const auto two = mdPreviewCodeHighlight("```\na\n```\n\n```\nb\n```\n",
                                            testColors());
    DOCTEST_CHECK(!two.ok);
    return true;
})

REGISTER_TEST(md_preview_hl_crlf, "md_preview_highlight: CRLF 不进显示串与 run", {
    const auto hl = mdPreviewCodeHighlight("```md\r\nline one\r\n\r\nline two\r\n```\r\n",
                                           testColors());
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK(hl.displayText.find('\r') == std::string::npos);
    DOCTEST_CHECK_EQ(hl.displayText, std::string("line one\n\nline two"));
    // 空行无 run（byte 9 是空行位置的换行符，不被任何 run 覆盖）
    DOCTEST_CHECK(runAtByte(hl, 9) == nullptr);
    DOCTEST_CHECK(colorEq(runAtByte(hl, 10)->color, kCode));
    DOCTEST_CHECK_EQ(runAtByte(hl, 10)->byteEnd, 18);
    return true;
})

REGISTER_TEST(md_preview_hl_info_word, "md_preview_highlight: lang 取 info 首词", {
    const auto hl = mdPreviewCodeHighlight("```python title=x\npass\n```\n",
                                           testColors());
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK_EQ(hl.lang, std::string("python"));
    DOCTEST_CHECK_EQ(hl.displayText, std::string("pass"));
    // python 受支持 → pass 走关键字色（lang 标签已不显示，无 marker run）
    DOCTEST_REQUIRE(hl.runs.size() >= 1);
    DOCTEST_CHECK(colorEq(hl.runs[0].color, kKw));
    DOCTEST_CHECK(colorEq(runAtByte(hl, 0)->color, kKw));
    return true;
})

REGISTER_TEST(md_preview_hl_perf,
              "md_preview_highlight: 5000 行围栏脏重算耗时记录 + 缓存命中零重算", {
    std::string body;
    for (int i = 0; i < 5000; ++i) {
        body += "int variable_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    const std::string src = "```cpp\n" + body + "```\n";
    const uint64_t hash = module::markdown::mdBlockHash(src);
    MdPreviewCodeCache cache;
    cache.setColors(testColors());

    // 脏块全量重算一次。8ms 击键预算只作记录、不设墙钟红线：并发编译负载下墙钟不是可靠
    // 的失败判据。
    const auto t0 = std::chrono::steady_clock::now();
    MdPreviewCodeHighlight hl = mdPreviewCodeHighlight(src, testColors());
    const double computeMs = msSince(t0);
    // 这里只断言脏重算这条路径真的跑到了：解析成功 + 产出了着色 run（5000 行 cpp 体必然非空）。
    std::printf(
        "[perf] md_preview_highlight: 5000 行围栏脏重算 %.3f ms"
        "（预算 8ms，设计 §7；记录式，不设墙钟红线）\n",
        computeMs);
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK(!hl.runs.empty());
    cache.put(1, hash, hl);

    // 缓存命中路径：10 万次 get 仅 map 查找 + hash 比较，无字符串重算
    DOCTEST_REQUIRE(cache.get(1, hash) != nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        const MdPreviewCodeHighlight* p = cache.get(1, hash);
        (void)p;
    }
    const double hitMs = msSince(t1);
    // 单次命中的均摊成本须远低于整块重算。比较单位成本而非总量：Debug 构建下 10 万次查找
    // 的总耗时本就可能超过一次重算，用总量判会 flaky。
    DOCTEST_CHECK(hitMs / 100000.0 < computeMs);
    return true;
})
