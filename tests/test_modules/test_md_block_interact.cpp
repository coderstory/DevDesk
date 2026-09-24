// tests/test_modules/test_md_block_interact.cpp
// Phase 3 ⑤ 块级交互纯底座单测（md_block_interact + MdSyncMap hit 访问器）。
// 几何用与 test_md_sync_map 同款的算术注入高度（非空行 × 10px），块高/行数
// 全整数 → 期望值可精确推导：
//   1. jump_codefence_exact：CodeFence 块点击——块顶 localY=0 精确落围栏首行
//      （cursorByte == 块起始字节），块底 localY=H 钳到块末行（不越块），
//      块中命中区间内；所有 targetLine 满足 lineStartOffset 往返一致。
//   2. jump_per_line_roundtrip（⑤ 的核心承诺）：对每个编辑器行 L，取 ④ 正向
//      映射 y=previewYForEditorLine(L) 反推点击（blockIndexAt(行首字节) 定位
//      块 + 区域局部 y）→ 解析出的 targetLine == L 且 cursorByte == 行首字节
//      ——预览逐行点击回编辑器逐行恒等（行首列，块边界/块内均精确）。
//   3. jump_defensive：空表/越界块 → ok=false；localY 越界一律钳进块内。
//   4. hover_state：enter 覆盖 + 迟到 leave 不误擦新 hover + 匹配 leave 清除
//      + hovered(-1) 恒 false + reset。
//   5. mix：rgb 线性混合、amount 钳 [0,1]、alpha 保留 base。
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

    // 块顶点击：落围栏首行（``` 行），cursorByte == 块起始字节
    const MdPreviewJump top = mdResolvePreviewJump(text, blocks, map, k, 0.0f);
    DOCTEST_REQUIRE(top.ok);
    DOCTEST_CHECK_EQ(top.blockIndex, k);
    DOCTEST_CHECK_EQ(top.stableId, fence.stableId);
    DOCTEST_CHECK_EQ(top.targetLine, map.blockFirstLine(k));
    DOCTEST_CHECK_EQ(top.cursorByte, fence.start);
    DOCTEST_CHECK_EQ(lineStartOffset(text, top.targetLine), top.cursorByte);
    DOCTEST_CHECK_EQ(byteOffsetToLine(text, top.cursorByte), top.targetLine);

    // 块底点击（localY=H，反函数 frac=1 会落到「下一块首行」）：钳回本块末行
    const MdPreviewJump bottom = mdResolvePreviewJump(text, blocks, map, k, h);
    DOCTEST_REQUIRE(bottom.ok);
    DOCTEST_CHECK_EQ(bottom.targetLine,
                     map.blockFirstLine(k) + map.blockLineCount(k) - 1);
    DOCTEST_CHECK(bottom.cursorByte < fence.end);

    // 块中点击：落在本块行区间内且行首字节自洽
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

    // 对每行 L：y = ④ 正向映射；块归属 = 行首字节 blockIndexAt；区域局部 y =
    // y - topY_k → 反查必须回到 L 行行首（⑤ 点击定位的核心正确性承诺）
    for (int L = 0; L < countLines(text); ++L) {
        const size_t lineByte = lineStartOffset(text, L);
        // 文尾换行后的「幻影末空行」：编辑器光标可停留，但预览不渲染该行
        // （末块可视高度止于最后一个非空行）→ 点击不可达，恒等语义不适用
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

    // 空表（未 rebuild）→ 一律 ok=false
    MdSyncMap emptyMap;
    DOCTEST_CHECK(!mdResolvePreviewJump(text, blocks, emptyMap, 0, 0.0f).ok);

    MdSyncMap map = buildMap(model, text, 0);
    // 块下标越界 → ok=false
    DOCTEST_CHECK(!mdResolvePreviewJump(text, blocks, map, map.blockCount(), 1.0f).ok);

    // localY 越界：负值钳到块首行；超大钳到块末行（均仍 ok 且行在块区间内）
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

    // 匹配的 leave 才清除
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

    // amount=0 → base 原色；越界钳到 [0,1] 端点
    mid = mdHoverMix(white, black, 0.0f);
    DOCTEST_CHECK(std::fabs(mid.r - 1.0f) < 1e-6f);
    mid = mdHoverMix(white, black, -1.0f);
    DOCTEST_CHECK(std::fabs(mid.r - 1.0f) < 1e-6f);
    mid = mdHoverMix(white, black, 2.0f);
    DOCTEST_CHECK(std::fabs(mid.r - 0.0f) < 1e-6f);
    return true;
})
