// tests/test_modules/test_md_block_model.cpp
// Markdown 块级元素模型（Phase 3 首块）测试：
//   1. 连续覆盖不变式：块序列无缝无重叠覆盖全文（corpus 性质测试，
//      substr 拼接 == 原文）——分块器最重要的正确性锚
//   2. 首批块类型切分：代码块（```/~~~/未闭合/内部行不误切）、表格
//      （表头+分隔行+表体一段、段落内 | 行保守并入）、标题/列表/引用/
//      段落/分隔线分类；setext 下划线留在段落块（等价性保守规则）；
//      前导空行并入块头（md4c 忽略块前空行，渲染无差）
//   3. 增量：update 相等文本零重算；改一块只脏该块（其余块 stableId
//      继承、后缀平移）；文中新插入表格块只脏新块
//   4. 一鱼三吃一致性：模型 CodeFence 块与 scanFences（大纲/高亮共用）
//      区间一致 → 编辑器行内 runs（A/B/C 高亮）与块模型围栏状态不可能漂移
//   5. CRLF 覆盖不变式 + 性能冒烟：万行文档分块稳态 < 8ms
#include "doctest.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_line_scanner.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

using module::markdown::MdBlock;
using module::markdown::MdBlockKind;
using module::markdown::MdBlockModel;
using module::markdown::segmentMarkdownBlocks;
using module::markdown::scanFences;

namespace {

// 连续覆盖不变式：首块从 0 起、块块相接、末块到文末（substr 拼接还原）
bool checkPartition(const std::string& text, const std::vector<MdBlock>& blocks) {
    std::string joined;
    size_t expect = 0;
    for (const auto& b : blocks) {
        DOCTEST_REQUIRE(b.start == expect);
        DOCTEST_REQUIRE(b.end > b.start);
        DOCTEST_REQUIRE(b.end <= text.size());
        joined += text.substr(b.start, b.end - b.start);
        expect = b.end;
    }
    DOCTEST_REQUIRE(text.empty() || expect == text.size());
    DOCTEST_REQUIRE(joined == text);
    return true;
}

const char* kCorpus[] = {
    "",
    "\n",
    "\n\n\n",
    "single paragraph line\n",
    "no trailing newline",
    "# heading\ntext\n\n## h2\nmore\n",
    "- a\n- b\n\n1. x\n2. y\n",
    "> quote one\n> quote two\n\nplain\n",
    "```cpp\nint x = 1;\n```\nafter\n",
    "~~~\ntilde fence\n~~~\n",
    "```python\n# not a heading\n```\n",
    "```unclosed to EOF\nstill code\n",
    "| a | b |\n| --- | --- |\n| 1 | 2 |\n",
    "text\n| a | b |\n| --- | --- |\ntext2\n",  // 段落内表格：保守并入
    "para\n\n---\n\nafter\n",
    "para\n---\nsetext\n",                       // setext：分隔线不切
    "- a\n  - nested\n    deep\n- b\n",
    "1. a\n\n   lazy cont\n2. b\n",
    "  indented text\n   more\n",
    "a\r\nb\r\n\r\nc\r\n",
    "# h1\n\n```\ncode\n```\n\n| x |\n| --- |\n\n- list\n\n> quote\n\n---\n\ntail\n",
    "```outer\ntext\n```closer\nafter\n",
};

} // namespace

REGISTER_TEST(md_block_model_partition, "md_block_model: 块序列连续覆盖全文（corpus 不变式）", {
    for (const char* c : kCorpus) {
        const std::string text = c;
        const auto blocks = segmentMarkdownBlocks(text);
        if (!checkPartition(text, blocks)) {
            std::cerr << "corpus:\n" << text << std::endl;
            return false;
        }
    }
    return true;
})

REGISTER_TEST(md_block_model_segmentation, "md_block_model: 代码块/表格/标题/列表/引用分类", {
    // 代码块：围栏整块（含闭栏行尾换行），前导空行并入块头
    {
        const std::string t = "intro\n\n```cpp\nint x;\n```\n\nafter\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 3);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::Paragraph);
        DOCTEST_CHECK(b[1].kind == MdBlockKind::CodeFence);
        DOCTEST_CHECK(t.compare(b[1].start, b[1].end - b[1].start,
                                "\n```cpp\nint x;\n```\n") == 0);
        DOCTEST_CHECK(b[2].kind == MdBlockKind::Paragraph);
    }
    // 未闭合围栏吃到 EOF
    {
        const std::string t = "```open\nrest of doc\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 1);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::CodeFence);
        DOCTEST_CHECK(b[0].end == t.size());
    }
    // 围栏内的 # 与表格形态不再分类（与大纲/高亮同源语义）
    {
        const std::string t = "```\n# fake heading\n| fake | table |\n--- |\n```\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 1);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::CodeFence);
    }
    // 表格：独立起点整段成块（表头+分隔+两行表体）
    {
        const std::string t = "para\n\n| a | b |\n| --- | :-: |\n| 1 | 2 |\n| 3 | 4 |\n\nend\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 3);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::Paragraph);
        DOCTEST_CHECK(b[1].kind == MdBlockKind::Table);
        DOCTEST_CHECK(t.compare(b[1].start, b[1].end - b[1].start,
                                "\n| a | b |\n| --- | :-: |\n| 1 | 2 |\n| 3 | 4 |\n") == 0);
        DOCTEST_CHECK(b[2].kind == MdBlockKind::Paragraph);
    }
    // 段落内 | 行（无前置空行）保守并入段落（不赌表格可打断段落的实现分歧）
    {
        const std::string t = "text\n| a | b |\n| --- |\nmore\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 1);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::Paragraph);
    }
    // 标题自足单行；列表/引用/分隔线/尾段落
    {
        const std::string t = "# H1\n## H2\ntext\n\n- a\n- b\n\n> q\n\n---\n\nz\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 7);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::Heading);
        DOCTEST_CHECK(b[1].kind == MdBlockKind::Heading);
        DOCTEST_CHECK(b[1].end - b[1].start == std::string_view("## H2\n").size());
        DOCTEST_CHECK(b[2].kind == MdBlockKind::Paragraph);
        DOCTEST_CHECK(b[3].kind == MdBlockKind::List);
        DOCTEST_CHECK(b[4].kind == MdBlockKind::Quote);
        DOCTEST_CHECK(b[5].kind == MdBlockKind::Divider);
        DOCTEST_CHECK(b[6].kind == MdBlockKind::Paragraph);
    }
    // 标题/列表紧贴段落也能切（ATX 与 bullet 均可打断段落）
    {
        const std::string t = "para\n# head\n- item\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 3);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::Paragraph);
        DOCTEST_CHECK(b[1].kind == MdBlockKind::Heading);
        DOCTEST_CHECK(b[2].kind == MdBlockKind::List);
    }
    // setext 下划线留在段落（切了会错，多并永远对）
    {
        const std::string t = "setext title\n===\nbody\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 1);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::Paragraph);
    }
    // 松散列表：同型列表项跨空行延续（有序编号不重置）
    {
        const std::string t = "1. a\n\n2. b\n\n3. c\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 1);
        DOCTEST_CHECK(b[0].kind == MdBlockKind::List);
    }
    return true;
})

REGISTER_TEST(md_block_model_incremental, "md_block_model: 编辑只脏受影响块（stableId 继承）", {
    const std::string v1 =
        "para A\n\n```py\ncode 1\n```\n\n| h |\n| - |\n| v |\n\npara B\n\npara C\n";
    MdBlockModel m;
    const auto r1 = m.update(v1);
    DOCTEST_CHECK(r1.changed);
    DOCTEST_REQUIRE(m.blocks().size() == 5);
    for (const auto& b : m.blocks()) DOCTEST_CHECK(b.stableId >= 0);
    std::vector<int> ids1(m.blocks().size());
    for (size_t i = 0; i < ids1.size(); ++i) ids1[i] = m.blocks()[i].stableId;

    // 相等文本零重算
    const auto rNoop = m.update(v1);
    DOCTEST_CHECK(!rNoop.changed);
    DOCTEST_CHECK(rNoop.dirtyLast < rNoop.dirtyFirst);

    // 改代码块内部：仅块 1 脏，其余块身份保持，后缀块整体平移
    std::string v2 = v1;
    v2.replace(v2.find("code 1"), 6, "code TWO");
    const auto r2 = m.update(v2);
    DOCTEST_CHECK(r2.changed);
    DOCTEST_CHECK(r2.dirtyFirst == 1 && r2.dirtyLast == 1);
    DOCTEST_REQUIRE(m.blocks().size() == 5);
    std::vector<int> ids2(m.blocks().size());
    for (size_t i = 0; i < ids2.size(); ++i) ids2[i] = m.blocks()[i].stableId;
    DOCTEST_CHECK(ids2[0] == ids1[0]);
    DOCTEST_CHECK(ids2[1] != ids1[1]);        // 脏块换新身份
    for (size_t i = 2; i < 5; ++i) {
        DOCTEST_CHECK(ids2[i] == ids1[i]);    // 未变块继承 stableId
    }
    DOCTEST_CHECK(m.blocks()[2].start == v2.find("| h |") - 1);  // 前导空行起
    checkPartition(v2, m.blocks());

    // 段落 B 前新插入表格块：仅新块脏，其余身份保持
    std::string v3 = v2;
    v3.insert(v3.find("para B"), "| x |\n| - |\n| y |\n\n");
    const auto r3 = m.update(v3);
    DOCTEST_CHECK(r3.changed);
    DOCTEST_REQUIRE(m.blocks().size() == 6);
    DOCTEST_CHECK(m.blocks()[3].kind == MdBlockKind::Table);
    DOCTEST_CHECK(m.blocks()[0].stableId == ids2[0]);
    DOCTEST_CHECK(m.blocks()[1].stableId == ids2[1]);
    DOCTEST_CHECK(m.blocks()[2].stableId == ids2[2]);
    DOCTEST_CHECK(m.blocks()[4].stableId == ids2[3]);  // para B 原身份
    DOCTEST_CHECK(m.blocks()[5].stableId == ids2[4]);  // para C 原身份
    checkPartition(v3, m.blocks());
    return true;
})

REGISTER_TEST(md_block_model_fence_parity, "md_block_model: 围栏块与 scanFences 区间一致（一鱼三吃）", {
    for (const char* c : kCorpus) {
        const std::string text = c;
        const auto fences = scanFences(text);
        const auto blocks = segmentMarkdownBlocks(text);
        size_t fi = 0;
        for (const auto& b : blocks) {
            if (b.kind != MdBlockKind::CodeFence) continue;
            DOCTEST_REQUIRE(fi < fences.size());
            // 块内含开栏行；块末 = 闭栏行行尾（未闭合 = 文末）
            DOCTEST_CHECK(fences[fi].openLineStart >= b.start);
            DOCTEST_CHECK(fences[fi].openLineStart < b.end);
            const size_t closeEnd = fences[fi].contentEnd >= text.size()
                ? text.size()
                : [&] {
                      const size_t le = text.find('\n', fences[fi].contentEnd);
                      return le == std::string::npos ? text.size() : le + 1;
                  }();
            DOCTEST_CHECK(b.end == closeEnd);
            ++fi;
        }
        DOCTEST_CHECK(fi == fences.size());
    }
    return true;
})

REGISTER_TEST(md_block_model_crlf, "md_block_model: CRLF 文档连续覆盖 + 表格/围栏可切", {
    const std::string t = "para\r\n\r\n```\r\ncode\r\n```\r\n\r\n| a |\r\n| - |\r\n\r\nend\r\n";
    const auto b = segmentMarkdownBlocks(t);
    checkPartition(t, b);
    DOCTEST_REQUIRE(b.size() == 4);
    DOCTEST_CHECK(b[0].kind == MdBlockKind::Paragraph);
    DOCTEST_CHECK(b[1].kind == MdBlockKind::CodeFence);
    DOCTEST_CHECK(b[2].kind == MdBlockKind::Table);
    DOCTEST_CHECK(b[3].kind == MdBlockKind::Paragraph);
    return true;
})

REGISTER_TEST(md_block_model_perf, "md_block_model: 万行文档分块稳态 < 8ms", {
    std::string big;
    big.reserve(3 * 1024 * 1024);
    for (int i = 0; i < 1100; ++i) {
        big += "## 段落组 ";
        big += std::to_string(i);
        big += "\n\n这是第 ";
        big += std::to_string(i);
        big += " 段中文内容，用于模拟真实笔记的行内 **粗体** 与 `代码`。\n\n";
        if (i % 20 == 3) {
            big += "```cpp\nint f() { return " + std::to_string(i) + "; }\n```\n\n";
        }
        if (i % 25 == 7) {
            big += "| 列一 | 列二 |\n| --- | --- |\n| a | b |\n\n";
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto blocks = segmentMarkdownBlocks(big);
    const auto coldUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    const auto t1 = std::chrono::steady_clock::now();
    const auto blocks2 = segmentMarkdownBlocks(big);
    const auto warmUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t1).count();
    checkPartition(big, blocks);
    DOCTEST_CHECK(blocks.size() > 2200);
    DOCTEST_CHECK(blocks2.size() == blocks.size());
    DOCTEST_CHECK_MESSAGE(warmUs < 8000,
                          "cold=" + std::to_string(coldUs) +
                          "us warm=" + std::to_string(warmUs) + "us");
    return true;
})
