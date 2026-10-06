// tests/test_modules/test_md_block_interact.cpp
// 预览点击→编辑器定位纯底座单测：块内命中、逐行往返恒等、越界防御、hover 状态机、混色。
// 跨文件前提：
//   - 块高不来自渲染层，而由 MdSyncMap 的高度回调注入：非空行 × 10px，块边界零误差，
//     块高/行数全整数 → 期望值可精确推导。改 kDoc 结构时行号注释要跟着改。
//   - 块归属只能按 blockIndexAt(行首字节) 判，不能按 y 反推——y 反推会跨块错位；
//     正向 previewYForEditorLine 与反向 mdResolvePreviewJump 必须成对使用。
#include "doctest.h"
#include "module_markdown/md_block_interact.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_line_scanner.h"
#include "module_markdown/md_sync_map.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using module::markdown::MdBlock;
using module::markdown::MdBlockKind;
using module::markdown::MdBlockModel;
using module::markdown::MdPreviewHoverState;
using module::markdown::MdPreviewJump;
using module::markdown::MdSyncMap;
using module::markdown::byteOffsetToLine;
using module::markdown::countLines;
using module::markdown::lineStartOffset;
using module::markdown::mdHoverMix;
using module::markdown::mdResolvePreviewJump;

namespace {

// 注入高度：非空行 × 10px（与 test_md_sync_map 同款，块边界误差 0 前提同构）
float synthHeight(const std::string& text, const MdBlock& b) {
    int nonEmpty = 0;
    std::string nl;
    for (size_t i = b.start; i < b.end && i < text.size(); ++i) {
        if (text[i] == '\n') {
            if (!nl.empty()) ++nonEmpty;
            nl.clear();
        } else if (text[i] != '\r') {
            nl.push_back(text[i]);
        }
    }
    if (!nl.empty()) ++nonEmpty;
    return 10.0f * (nonEmpty > 0 ? nonEmpty : 1);
}

constexpr float kGap = 12.0f;
constexpr float kWidth = 600.0f;

MdSyncMap buildMap(MdBlockModel& model, const std::string& text, uint64_t rev) {
    model.update(text);
    MdSyncMap map;
    const std::string snapshot = model.text();
    map.rebuild(snapshot, model.blocks(), rev, kWidth, kGap,
                [&snapshot](const MdBlock& b) { return synthHeight(snapshot, b); });
    return map;
}

// 含 CodeFence 的多结构文档（行号 0 起：12-16 围栏块，17 空行，18-19 尾段）
const char* kDoc =
    "# Section\n"
    "\n"
    "para line one\n"
    "para line two\n"
    "para line three\n"
    "para line four\n"
    "\n"
    "short para alone\n"
    "\n"
    "- list item one\n"
    "- list item two\n"
    "\n"
    "```\n"
    "code1\n"
    "code2\n"
    "code3\n"
    "```\n"
    "\n"
    "tail line one\n"
    "tail line two\n";

size_t findBlock(const std::vector<MdBlock>& blocks, MdBlockKind kind) {
    for (size_t k = 0; k < blocks.size(); ++k) {
        if (blocks[k].kind == kind) return k;
    }
    return static_cast<size_t>(-1);
}

} // namespace

REGISTER_TEST(md_block_interact_jump_codefence_exact,
              "md_block_interact: CodeFence 块点击 → 块顶精确首行字节 / 块底钳末行", {
    MdBlockModel model;
    const std::string text = kDoc;
    MdSyncMap map = buildMap(model, text, 0);
    const auto& blocks = model.blocks();
    const size_t k = findBlock(blocks, MdBlockKind::CodeFence);
    DOCTEST_REQUIRE(k != static_cast<size_t>(-1));

    const MdBlock& fence = blocks[k];
    const float h = map.blockHeight(k);
    DOCTEST_REQUIRE(h > 0.0f);
    DOCTEST_REQUIRE(map.blockLineCount(k) >= 5);  // 开栏 + 3 内容 + 闭栏

    const MdPreviewJump top = mdResolvePreviewJump(text, blocks, map, k, 0.0f);
    DOCTEST_REQUIRE(top.ok);
    DOCTEST_CHECK_EQ(top.blockIndex, k);
    DOCTEST_CHECK_EQ(top.stableId, fence.stableId);
    DOCTEST_CHECK_EQ(top.targetLine, map.blockFirstLine(k));
    DOCTEST_CHECK_EQ(top.cursorByte, fence.start);
    DOCTEST_CHECK_EQ(lineStartOffset(text, top.targetLine), top.cursorByte);
    DOCTEST_CHECK_EQ(byteOffsetToLine(text, top.cursorByte), top.targetLine);

    // 块顶 localY=H：反函数的 frac=1 会落到「下一块首行」，必须钳回本块末行
    const MdPreviewJump bottom = mdResolvePreviewJump(text, blocks, map, k, h);
    DOCTEST_REQUIRE(bottom.ok);
    DOCTEST_CHECK_EQ(bottom.targetLine,
                     map.blockFirstLine(k) + map.blockLineCount(k) - 1);
    DOCTEST_CHECK(bottom.cursorByte < fence.end);

    const MdPreviewJump mid = mdResolvePreviewJump(text, blocks, map, k, h * 0.5f);
    DOCTEST_REQUIRE(mid.ok);
    DOCTEST_CHECK(mid.targetLine >= map.blockFirstLine(k));
    DOCTEST_CHECK(mid.targetLine <= map.blockFirstLine(k) + map.blockLineCount(k) - 1);
    DOCTEST_CHECK_EQ(lineStartOffset(text, mid.targetLine), mid.cursorByte);
    return true;
})

REGISTER_TEST(md_block_interact_jump_per_line_roundtrip,
              "md_block_interact: 逐编辑器行正向→点击→反查恒等（行首字节）", {
    MdBlockModel model;
    const std::string text = kDoc;
    MdSyncMap map = buildMap(model, text, 0);
    const auto& blocks = model.blocks();

    // 点击定位的核心承诺：每个编辑器行 L 正映射出 y，再按「行首字节定位块 + 区域局部 y」
// 反查，必须精确回到 L 的行首（块边界与块内均如此）
    for (int L = 0; L < countLines(text); ++L) {
        const size_t lineByte = lineStartOffset(text, L);
        // 文尾换行后的「幻影末空行」：编辑器光标可停留，但预览不渲染该行（末块可视高度
        // 止于最后一个非空行）→ 点击不可达，恒等语义不适用。这行 continue 不可去掉，
        // 去掉后 L 会拿一个渲染不出的 y 去反查，断言必红。
        if (lineByte >= text.size() && !text.empty() && text.back() == '\n') continue;
        const size_t k = model.blockIndexAt(lineByte);
        DOCTEST_REQUIRE(k < blocks.size());
        const float y = map.previewYForEditorLine(L);
        const float localY = y - map.blockTopY(k);
        const MdPreviewJump jump =
            mdResolvePreviewJump(text, blocks, map, k, localY);
        DOCTEST_REQUIRE(jump.ok);
        if (!(jump.targetLine == L && jump.cursorByte == lineByte)) {
            std::cerr << "L=" << L << " k=" << k << " y=" << y
                      << " localY=" << localY << " got line=" << jump.targetLine
                      << " byte=" << jump.cursorByte << std::endl;
        }
        DOCTEST_CHECK_EQ(jump.targetLine, L);
        DOCTEST_CHECK_EQ(jump.cursorByte, lineByte);
        DOCTEST_CHECK_EQ(jump.stableId, blocks[k].stableId);
    }
    return true;
})

REGISTER_TEST(md_block_interact_jump_defensive,
              "md_block_interact: 空表/越界/越界 localY 防御语义", {
    MdBlockModel model;
    const std::string text = kDoc;
    const auto& blocks = model.blocks();

    MdSyncMap emptyMap;
    DOCTEST_CHECK(!mdResolvePreviewJump(text, blocks, emptyMap, 0, 0.0f).ok);

    MdSyncMap map = buildMap(model, text, 0);
    DOCTEST_CHECK(!mdResolvePreviewJump(text, blocks, map, map.blockCount(), 1.0f).ok);

    // localY 越界一律钳进块内而不报错：负值 → 块首行，超大 → 块末行
    const MdPreviewJump neg = mdResolvePreviewJump(text, blocks, map, 0, -50.0f);
    DOCTEST_REQUIRE(neg.ok);
    DOCTEST_CHECK_EQ(neg.targetLine, map.blockFirstLine(0));
    const MdPreviewJump huge = mdResolvePreviewJump(text, blocks, map, 0, 1e6f);
    DOCTEST_REQUIRE(huge.ok);
    DOCTEST_CHECK_EQ(huge.targetLine,
                     map.blockFirstLine(0) + map.blockLineCount(0) - 1);
    return true;
})

REGISTER_TEST(md_block_interact_hover_state,
              "md_block_interact: hover 状态机（enter 覆盖 / 迟到 leave / reset）", {
    MdPreviewHoverState hs;
    DOCTEST_CHECK_EQ(hs.stableId, -1);
    DOCTEST_CHECK(!hs.hovered(3));
    DOCTEST_CHECK(!hs.hovered(-1));  // -1 永不命中（哨兵防御）

    hs.enter(3);
    DOCTEST_CHECK(hs.hovered(3));
    DOCTEST_CHECK(!hs.hovered(4));

    // 快速划过：4 的 enter 先于 3 的 leave 到达 → 迟到的 leave(3) 不得清除 4
    hs.enter(4);
    DOCTEST_CHECK(hs.hovered(4));
    hs.leave(3);
    DOCTEST_CHECK(hs.hovered(4));
    DOCTEST_CHECK(!hs.hovered(3));

    // leave 按 id 匹配：只有与当前 stableId 相同的那次才清除
    hs.leave(4);
    DOCTEST_CHECK(!hs.hovered(4));
    DOCTEST_CHECK_EQ(hs.stableId, -1);

    hs.enter(7);
    hs.reset();
    DOCTEST_CHECK(!hs.hovered(7));
    return true;
})

REGISTER_TEST(md_block_interact_mix,
              "md_block_interact: hover 混色（rgb 线性 / amount 钳制 / alpha 保留）", {
    const eui::Color white(1.0f, 1.0f, 1.0f, 0.8f);
    const eui::Color black(0.0f, 0.0f, 0.0f, 1.0f);

    eui::Color mid = mdHoverMix(white, black, 0.5f);
    DOCTEST_CHECK(std::fabs(mid.r - 0.5f) < 1e-6f);
    DOCTEST_CHECK(std::fabs(mid.g - 0.5f) < 1e-6f);
    DOCTEST_CHECK(std::fabs(mid.b - 0.5f) < 1e-6f);
    DOCTEST_CHECK(std::fabs(mid.a - 0.8f) < 1e-6f);  // alpha 保留 base

    // amount 越界一律钳到 [0,1] 端点，0 → 原封不动取 base
    mid = mdHoverMix(white, black, 0.0f);
    DOCTEST_CHECK(std::fabs(mid.r - 1.0f) < 1e-6f);
    mid = mdHoverMix(white, black, -1.0f);
    DOCTEST_CHECK(std::fabs(mid.r - 1.0f) < 1e-6f);
    mid = mdHoverMix(white, black, 2.0f);
    DOCTEST_CHECK(std::fabs(mid.r - 0.0f) < 1e-6f);
    return true;
})
