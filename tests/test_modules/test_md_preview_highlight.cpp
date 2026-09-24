// tests/test_modules/test_md_preview_highlight.cpp
// Phase 3 ③：预览代码块语法高亮——
//   1. md_preview_highlight 纯函数：lang / displayText / 逐行 code run / 颜色断言
//   2. 与编辑器 MdHighlighter 的同源等价：同一围栏块，预览逐行 code run 的
//      行内偏移 + 色表与编辑器全文档 runsForLine 命中内容行的结果一致
//   3. 脏块缓存：(stableId, hash) 命中 / 编辑改块重算 / 块删除 prune /
//      主题切换色表变更整体作废
//   4. 防御：非单围栏块源码 → ok=false（调用方回退 components::markdown）
//   5. CRLF：\r 不进显示串与 run 字节区间
#include "doctest.h"
#include "core/theme.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_highlight.h"
#include "module_markdown/md_preview_highlight.h"

#include <eui/types.h>

#include <chrono>
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

// 与 test_md_highlight 同风格的测试色（值本身任意可辨）
const eui::Color kMark{1.0f, 0.0f, 0.0f, 1.0f};
const eui::Color kCode{0.0f, 1.0f, 0.0f, 1.0f};
const eui::Color kLink{0.0f, 0.0f, 1.0f, 1.0f};
const eui::Color kList{1.0f, 1.0f, 0.0f, 1.0f};
const eui::Color kQuote{1.0f, 0.0f, 1.0f, 1.0f};

MdHighlightColors testColors() {
    return MdHighlightColors{kMark, kCode, kLink, kList, kQuote};
}

bool colorEq(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// 毫秒计时（REGISTER_TEST 宏体内不可出现尖括号模板逗号，helper 放这层）
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

} // namespace

REGISTER_TEST(md_preview_hl_basic, "md_preview_highlight: lang 行 + 逐行 code run", {
    const std::string src = "```cpp\nint a;\nint b;\n```\n";
    const auto hl = mdPreviewCodeHighlight(src, testColors());
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK_EQ(hl.lang, std::string("cpp"));
    DOCTEST_CHECK_EQ(hl.displayText, std::string("cpp\nint a;\nint b;"));
    // lang run：整段 marker 色
    DOCTEST_REQUIRE(hl.runs.size() >= 1);
    DOCTEST_CHECK_EQ(hl.runs[0].byteStart, 0);
    DOCTEST_CHECK_EQ(hl.runs[0].byteEnd, 3);
    DOCTEST_CHECK(colorEq(hl.runs[0].color, kMark));
    // 内容行整行 code 色，空行无 run（"cpp\nint a;\nint b;"：line1 [4,10)、line2 [11,17)）
    DOCTEST_CHECK(colorEq(runAtByte(hl, 4)->color, kCode));   // "int a;" 首字节
    DOCTEST_CHECK(colorEq(runAtByte(hl, 9)->color, kCode));   // "int a;" 行内
    DOCTEST_CHECK_EQ(runAtByte(hl, 4)->byteStart, 4);
    DOCTEST_CHECK_EQ(runAtByte(hl, 4)->byteEnd, 10);
    DOCTEST_CHECK_EQ(runAtByte(hl, 11)->byteEnd, 17);
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
              "md_preview_highlight: 与编辑器 MdHighlighter token 同源等价", {
    // 编辑器：全文档 runsForLine；围栏内容行 → 整行单 run（色 = colors.code）
    // 预览：块源码 → displayText 坐标逐行 code run
    // 等价断言：去掉 lang 标签行后，两者逐行 run 的行内字节区间 + 颜色完全一致
    const std::string doc = "# Title\n\n```python\nx = 1\ny = 2\n```\n\ntail\n";
    const std::string src = fenceSourceOf(doc);
    DOCTEST_REQUIRE(!src.empty());

    MdHighlighter h;
    h.setColors(testColors());
    // 内容行 1 "x = 1"（doc 坐标）：编辑器整行 1 个 code run（全文档坐标系）
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
    // displayText = "python\nx = 1\ny = 2"，"x = 1" 的 run 行内区间 = [0, 5)
    const size_t dispStart = hl.displayText.find(line);
    DOCTEST_REQUIRE(dispStart != std::string::npos);
    const eui::TextRun* prevRun = runAtByte(hl, dispStart);
    DOCTEST_REQUIRE(prevRun != nullptr);
    DOCTEST_CHECK(colorEq(prevRun->color, editorRuns[0].color));
    DOCTEST_CHECK_EQ(static_cast<size_t>(prevRun->byteStart), dispStart);
    DOCTEST_CHECK_EQ(static_cast<size_t>(prevRun->byteEnd),
                     dispStart + line.size());

    // 主题 token 路径也同源：编辑器与预览色表来自同一 mdHighlightColors
    const auto tokens = core::theme::current();
    const auto c1 = mdHighlightColors(tokens);
    const auto c2 = mdHighlightColors(tokens);
    DOCTEST_CHECK(colorEq(c1.code, c2.code));
    DOCTEST_CHECK(colorEq(c1.marker, c2.marker));
    DOCTEST_CHECK(colorEq(c1.code, tokens.colors.success));
    return true;
})

REGISTER_TEST(md_preview_hl_cache, "md_preview_highlight: 脏块缓存命中/重算/prune/主题作废", {
    MdPreviewCodeCache cache;
    cache.setColors(testColors());
    const std::string src = "```cpp\nint a;\n```\n";
    const uint64_t hash = module::markdown::mdBlockHash(src);

    DOCTEST_CHECK(cache.get(7, hash) == nullptr);  // 冷 miss
    cache.put(7, hash, mdPreviewCodeHighlight(src, testColors()));
    DOCTEST_REQUIRE(cache.get(7, hash) != nullptr);
    DOCTEST_CHECK(cache.get(7, hash)->ok);
    DOCTEST_CHECK(cache.get(7, hash + 1) == nullptr);  // 脏块重算
    DOCTEST_CHECK(cache.get(8, hash) == nullptr);      // 别的块

    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(1));
    cache.put(8, hash + 100, mdPreviewCodeHighlight(src, testColors()));
    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(2));
    cache.pruneActive({7});
    DOCTEST_CHECK_EQ(cache.size(), static_cast<size_t>(1));
    DOCTEST_CHECK(cache.get(8, hash + 100) == nullptr);
    DOCTEST_CHECK(cache.get(7, hash) != nullptr);  // 7 不受 prune 影响

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
    DOCTEST_CHECK_EQ(hl.displayText, std::string("md\nline one\n\nline two"));
    // 空行无 run（byte 12 为空行位置的换行符，不被任何 run 覆盖）
    DOCTEST_CHECK(runAtByte(hl, 12) == nullptr);
    DOCTEST_CHECK(colorEq(runAtByte(hl, 13)->color, kCode));
    DOCTEST_CHECK_EQ(runAtByte(hl, 13)->byteEnd, 21);
    return true;
})

REGISTER_TEST(md_preview_hl_info_word, "md_preview_highlight: lang 取 info 首词", {
    const auto hl = mdPreviewCodeHighlight("```python title=x\npass\n```\n",
                                           testColors());
    DOCTEST_CHECK(hl.ok);
    DOCTEST_CHECK_EQ(hl.lang, std::string("python"));
    DOCTEST_CHECK_EQ(hl.displayText, std::string("python\npass"));
    return true;
})

REGISTER_TEST(md_preview_hl_perf,
              "md_preview_highlight: 5000 行围栏脏重算 <8ms + 缓存命中零重算", {
    std::string body;
    for (int i = 0; i < 5000; ++i) {
        body += "int variable_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    const std::string src = "```cpp\n" + body + "```\n";
    const uint64_t hash = module::markdown::mdBlockHash(src);
    MdPreviewCodeCache cache;
    cache.setColors(testColors());

    // 脏块全量重算一次（击键预算 8ms 红线，与设计 §7 / md_highlight_perf 同标）
    const auto t0 = std::chrono::steady_clock::now();
    cache.put(1, hash, mdPreviewCodeHighlight(src, testColors()));
    const double computeMs = msSince(t0);
    DOCTEST_CHECK(computeMs < 8.0);

    // 缓存命中路径：10 万次 get 仅 map 查找 + hash 比较，无字符串重算
    DOCTEST_REQUIRE(cache.get(1, hash) != nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        const MdPreviewCodeHighlight* p = cache.get(1, hash);
        (void)p;
    }
    const double hitMs = msSince(t1);
    // 单次命中均摊成本须远低于整块重算（Debug 构建下 10 万次查找总耗时
    // 本就可能超过一次重算，故比较单位成本而非总量，防 flaky）
    DOCTEST_CHECK(hitMs / 100000.0 < computeMs);
    return true;
})
