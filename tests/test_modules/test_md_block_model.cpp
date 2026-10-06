// tests/test_modules/test_md_block_model.cpp
// Markdown 块级切分模型单测：连续覆盖不变式 / 块型分类 / 增量只脏受影响块（stableId 继承）/
// 块模型与 scanFences 区间一致 / 逐块渲染与整篇重算结构等价 / CRLF / 性能冒烟。
// 跨文件前提：
//   - 块模型的 CodeFence 区间与 scanFences（大纲/高亮共用）必须同源一致，否则行内高亮与
//     围栏状态会漂。
//   - EUI_HAS_MD4C 未定义时上游解析退化为单段落，结构等价比对无意义，函数恒真短路。
#include "doctest.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_line_scanner.h"

// 结构等价比对直接消费上游 md4c 解析器。EUI_HAS_MD4C 是 eui_neo 的 PUBLIC 编译
// 定义，随链接传播，本 TU 看不到就等于上游没启用。
#if defined(EUI_HAS_MD4C)
#include <components/markdown.h>
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using module::markdown::MdBlock;
using module::markdown::MdBlockKind;
using module::markdown::MdBlockModel;
using module::markdown::mdFindBlockByContent;
using module::markdown::segmentMarkdownBlocks;
using module::markdown::scanFences;

namespace {

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
    "[foo]: http://example.com\n\nSee [foo] here.\n",  // 链接引用定义=全文档上下文
    "定义在前\n\n[a]: /u1\n[b]: /u2\n\n用 [a] 和 [b]\n",
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
    // refdef = 全文档上下文 → 整篇单块退化（保等价）；围栏内 refdef 行不触发
    {
        const std::string t = "para one\n\n[foo]: http://x\n\nsee [foo]\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_REQUIRE(b.size() == 1);
        DOCTEST_CHECK(b[0].start == 0 && b[0].end == t.size());
    }
    {
        const std::string t = "```\n[fake]: /not-a-refdef\n```\n\nreal para\n\nnext\n";
        const auto b = segmentMarkdownBlocks(t);
        DOCTEST_CHECK(b.size() > 1);  // 围栏内形似 refdef 的文本不触发退化
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

// 结构等价比对主体。#if 分支必须留在宏外：REGISTER_TEST 实参内嵌预处理指令会触发
// -Wpedantic -Werror。无 md4c 构建时上游解析退化为单段落、比对无意义 → 恒真短路。
static bool mdBlockModelRenderEquivalence() {
#if !defined(EUI_HAS_MD4C)
    return true;
#else
    using components::detail::MarkdownBlock;
    using components::detail::MarkdownBlockKind;
    using components::detail::parseMarkdownBlocks;

    // 块签名：md4c 类型 + 影响渲染的关键状态（标题层级 / 有序编号 / 表格列 /
    // run 文本拼接）。逐块渲染 == 整篇渲染 的充要条件是块序列一致。
    auto signature = [](const MarkdownBlock& b) {
        std::string runsText;
        for (const auto& r : b.runs) runsText += r.text;
        std::string cellsText;
        for (const auto& c : b.cells) {
            for (const auto& r : c.runs) cellsText += r.text;
            cellsText += "\x1F";
        }
        return std::to_string(static_cast<int>(b.kind)) + '|' +
               std::to_string(b.headingLevel) + '|' +
               std::to_string(b.ordered ? 1 : 0) + std::to_string(b.number) + '|' +
               std::to_string(b.tableHeader ? 1 : 0) + '|' +
               runsText + '|' + cellsText;
    };
    auto signatures = [&](const std::string& text) {
        std::vector<std::string> out;
        for (const auto& b : parseMarkdownBlocks(text)) out.push_back(signature(b));
        return out;
    };

    // corpus 全覆盖（各块型 + 围栏内噪声行）
    for (const char* c : kCorpus) {
        const std::string text = c;
        const auto whole = signatures(text);
        std::vector<std::string> perBlock;
        for (const auto& b : segmentMarkdownBlocks(text)) {
            const auto s = signatures(text.substr(b.start, b.end - b.start));
            perBlock.insert(perBlock.end(), s.begin(), s.end());
        }
        if (!(whole == perBlock)) {
            std::cerr << "等价破口 corpus:\n" << text << std::endl;
            for (size_t i = 0; i < whole.size(); ++i) {
                std::cerr << "whole[" << i << "]=" << whole[i] << std::endl;
            }
            for (size_t i = 0; i < perBlock.size(); ++i) {
                std::cerr << "blocks[" << i << "]=" << perBlock[i] << std::endl;
            }
            return false;  // 逐块解析与整篇不一致 = 切分器等价破口
        }
    }

    // 混合富文档：7 种块型 + 行内 runs（粗体/内联代码）+ 表格对齐 + 未闭合围栏
    {
        const std::string rich =
            "# 标题 Alpha\n\n"
            "**行内粗体** 和 `内联代码` 的段落，跨多行\n软换行合并同段。\n\n"
            "```cpp\nint main() { return 0; }\n```\n\n"
            "| 列A | 列B |\n| :-- | --: |\n| 1 | 2 |\n| 三 | 四 |\n\n"
            "- 项目一\n- 项目二\n  - 嵌套项\n\n"
            "> 引用第一行\n> 引用第二行\n\n"
            "---\n\n"
            "```python\n# 围栏内的 # 与 | 行是噪声\n```\n\n"
            "结尾段落。\n";
        const auto whole = signatures(rich);
        std::vector<std::string> perBlock;
        for (const auto& b : segmentMarkdownBlocks(rich)) {
            const auto s = signatures(rich.substr(b.start, b.end - b.start));
            perBlock.insert(perBlock.end(), s.begin(), s.end());
        }
        DOCTEST_CHECK(whole == perBlock);
        DOCTEST_CHECK(whole.size() >= 8);  // 富样例确有多样块
    }

    // CJK + CRLF 富文档
    {
        const std::string t = "标题一\r\n\r\n```sh\r\ngit status\r\n```\r\n\r\n| 甲 | 乙 |\r\n| - | - |\r\n| 1 | 2 |\r\n\r\n- 中文项\r\n> 中文引用\r\n\r\n结束\r\n";
        const auto whole = signatures(t);
        std::vector<std::string> perBlock;
        for (const auto& b : segmentMarkdownBlocks(t)) {
            const auto s = signatures(t.substr(b.start, b.end - b.start));
            perBlock.insert(perBlock.end(), s.begin(), s.end());
        }
        DOCTEST_CHECK(whole == perBlock);
    }
        return true;
#endif
}

REGISTER_TEST(md_block_model_render_equivalence, "md_block_model: 逐块渲染与整篇重算逐块一致（md4c 结构等价）", {
    return mdBlockModelRenderEquivalence();
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

REGISTER_TEST(md_block_model_find_by_content,
              "md_block_model: mdFindBlockByContent 重载活动块内容映射（MD-01）", {
    // 空文档（无块）→ npos
    {
        const auto blocks = segmentMarkdownBlocks("");
        DOCTEST_REQUIRE(blocks.empty());
        DOCTEST_CHECK_EQ(mdFindBlockByContent(blocks, MdBlockKind::Paragraph, 1,
                                              7, 0),
                         std::string::npos);
    }

    // 三元组 (kind,hash,stableId) 唯一命中 → 返回该下标（内容未变、stableId 保留的平移场景）
    {
        const std::string doc = "alpha\n\nbravo\n\ncharlie\n";
        MdBlockModel m;
        m.update(doc);
        const auto& bs = m.blocks();
        DOCTEST_REQUIRE(bs.size() == 3);
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[1].kind, bs[1].hash, bs[1].stableId,
                                 bs[1].start),
            static_cast<size_t>(1));
        // 三元失配后的回退规则：kind 参与匹配。stableId 错配 → (kind,hash) 仍唯一命中；
        // kind 错配 → 回退也无命中
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[1].kind, bs[1].hash, 9999, bs[1].start),
            static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, MdBlockKind::Heading, bs[1].hash,
                                 bs[1].stableId, bs[1].start),
            std::string::npos);
        // hash 错配 → 等价类也无命中 → npos
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[1].kind, 1, bs[1].stableId, bs[1].start),
            std::string::npos);
    }

    // 三元组无命中、(kind,hash) 唯一命中：模拟「不相邻双改」——头尾都变、中间块
    // 字节未变，但落入变化窗口而拿到新 stableId（尾改/双改使 stableId 失效）
    {
        MdBlockModel m;
        m.update("alpha\n\nbravo\n\ncharlie\n");
        const auto& old = m.blocks();
        const MdBlock b = old[1];   // bravo（原身份）
        DOCTEST_REQUIRE(old.size() == 3);
        m.update("alpha changed\n\nbravo\n\ncharlie!\n");
        const auto& nw = m.blocks();
        DOCTEST_REQUIRE(nw.size() == 3);
        // 头尾都改 → 前后缀对齐全断 → bravo 在新序列里拿新 id（字节未变）
        DOCTEST_CHECK(nw[1].kind == b.kind);
        DOCTEST_CHECK(nw[1].hash == b.hash);
        DOCTEST_CHECK(nw[1].stableId != b.stableId);
        // 三元失配（stableId 换了）→ (kind,hash) 唯一命中 bravo
        DOCTEST_CHECK_EQ(mdFindBlockByContent(nw, b.kind, b.hash, b.stableId,
                                              b.start),
                         static_cast<size_t>(1));
    }

    // 重复块 fixture。必须用字节全同的非空块：空行会被并入邻块头部，造不出同 hash 的
    // 独立块。「\n```\n\n```\n」围栏连接 3 遍 → 3 个全同 CodeFence 块（同 kind 同 hash、
    // stableId 各异；围栏内容吞掉块间空行，整段字节全同）
    {
        const std::string unit = "\n```\n\n```\n";
        const std::string doc = unit + unit + unit;
        MdBlockModel m;
        m.update(doc);
        const auto& bs = m.blocks();
        DOCTEST_REQUIRE(bs.size() == 3);
        DOCTEST_REQUIRE(bs[0].kind == MdBlockKind::CodeFence);
        DOCTEST_REQUIRE(bs[1].kind == MdBlockKind::CodeFence);
        DOCTEST_REQUIRE(bs[2].kind == MdBlockKind::CodeFence);
        DOCTEST_REQUIRE(bs[0].hash == bs[1].hash);
        DOCTEST_REQUIRE(bs[1].hash == bs[2].hash);
        DOCTEST_REQUIRE(bs[0].stableId != bs[1].stableId);
        DOCTEST_REQUIRE(bs[1].stableId != bs[2].stableId);
        // 三元组命中精确实例：各实例各自返回自己的下标（重复块也区分实例）
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[0].kind, bs[0].hash, bs[0].stableId,
                                 bs[0].start),
            static_cast<size_t>(0));
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[1].kind, bs[1].hash, bs[1].stableId,
                                 bs[1].start),
            static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[2].kind, bs[2].hash, bs[2].stableId,
                                 bs[2].start),
            static_cast<size_t>(2));
        // 三元无命中（stableId 不存在）→ (kind,hash) 多命中取 |start-nearByte|
        // 最小：nearByte 贴近中间块 start → 命中下标 1（fixture 避开等距）
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[2].kind, bs[2].hash, 9999, bs[1].start),
            static_cast<size_t>(1));
        // nearByte 贴近首块 → 下标 0；贴近末块 → 下标 2
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[2].kind, bs[2].hash, 9999, bs[0].start),
            static_cast<size_t>(0));
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[2].kind, bs[2].hash, 9999, bs[2].start),
            static_cast<size_t>(2));
    }

    // 等距 tie-break 取下标小者。这里手工构造块向量是刻意的：mdFindBlockByContent 是纯函数，
    // 只读 kind/hash/start/stableId，不要求这些块与任何真实文档的切分结果一致
    {
        std::vector<MdBlock> blocks;
        const uint64_t h = 42;
        blocks.push_back({MdBlockKind::Paragraph, 0, 1, h, 10});
        blocks.push_back({MdBlockKind::Paragraph, 4, 5, h, 11});
        blocks.push_back({MdBlockKind::Paragraph, 8, 9, h, 12});
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(blocks, MdBlockKind::Paragraph, h, 9999, 2),
            static_cast<size_t>(0));   // |0-2| == |4-2| → 取 0
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(blocks, MdBlockKind::Paragraph, h, 9999, 6),
            static_cast<size_t>(1));   // |4-6| == |8-6| → 取 1
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(blocks, MdBlockKind::Paragraph, h, 9999, 5),
            static_cast<size_t>(1));   // 距 4 为 1 < 距 8 为 3 → 最近者胜
    }

    // 零命中（kind 或 hash 无匹配）→ npos
    {
        MdBlockModel m;
        m.update("alpha\n\nbravo\n");
        const auto& bs = m.blocks();
        DOCTEST_REQUIRE(bs.size() == 2);
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, MdBlockKind::Heading, bs[0].hash,
                                 bs[0].stableId, bs[0].start),
            std::string::npos);
        DOCTEST_CHECK_EQ(
            mdFindBlockByContent(bs, bs[0].kind, 7, bs[0].stableId, bs[0].start),
            std::string::npos);
    }
    return true;
})

REGISTER_TEST(md_block_model_perf, "md_block_model: 万行文档分块稳态（记录式，预算 8ms）", {
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
    // 不设墙钟红线：并发编译负载下墙钟不能当失败判据。改为记录式打印实测值 + 断言同一
    // 输入二次分段逐块等价（kind/start/end/stableId/hash 全等）——「快了但切错了」必红。
    std::printf("[perf] md_block_model: cold=%.1fms warm=%.1fms"
                "（预算 8ms；记录式，不设墙钟红线）\n",
                coldUs / 1000.0, warmUs / 1000.0);
    for (size_t i = 0; i < blocks.size(); ++i) {
        DOCTEST_CHECK(blocks[i].kind == blocks2[i].kind);
        DOCTEST_CHECK(blocks[i].start == blocks2[i].start);
        DOCTEST_CHECK(blocks[i].end == blocks2[i].end);
        DOCTEST_CHECK(blocks[i].hash == blocks2[i].hash);
        DOCTEST_CHECK(blocks[i].stableId == blocks2[i].stableId);
    }
    return true;
})
