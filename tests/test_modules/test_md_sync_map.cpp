// tests/test_modules/test_md_sync_map.cpp
// MdSyncMap（同步滚动块模型）纯单测：块首行锚点精度 / 双向映射互逆 /
// 块增删后锚点不漂 / memo 只对脏块重测。
// 跨文件前提：
//   - 高度由外部 heightFn 注入，本文件不碰 EUI-NEO、不碰真实折行，误差断言按「精确」级。
//   - buildReference 是测试侧独立实现的参考累计表（与 MdSyncMap 同序、算法不共用），
//     它才是对照组：拿 map 自己的输出当期望值就是自证式断言。
//   - 块身份靠 stableId 跨文首插入/删除继承，靠后缀 (kind, hash) 对齐。
#include "doctest.h"
#include "module_markdown/md_block_model.h"
#include "module_markdown/md_line_scanner.h"
#include "module_markdown/md_sync_map.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using module::markdown::MdBlock;
using module::markdown::MdBlockModel;
using module::markdown::MdSyncMap;
using module::markdown::countLines;

namespace {

// 注入高度：非空行 × 10px（块尾空行不占预览高度，模拟真实折行结构）——
// 全整数，让期望值可用 float 精确累加
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

// 块占编辑器行数（与 MdSyncMap 内部同定义：'\n' 数 + 无换行尾时 1）
int synthLineCount(const std::string& text, const MdBlock& b) {
    int nl = 0;
    for (size_t i = b.start; i < b.end && i < text.size(); ++i) {
        if (text[i] == '\n') ++nl;
    }
    const bool endsNl = b.end > b.start && text[b.end - 1] == '\n';
    return std::max(1, nl + (endsNl ? 0 : 1));
}

constexpr float kGap = 12.0f;
constexpr float kWidth = 600.0f;

struct Reference {
    std::vector<float> topY;      // 块顶 y
    std::vector<int> firstLine;   // 块首编辑器行
    std::vector<int> lineCount;   // 块编辑器行数
    std::vector<float> height;
};

Reference buildReference(const std::string& text, const std::vector<MdBlock>& blocks) {
    Reference r;
    float curY = 0.0f;
    int curLine = 0;
    for (const auto& b : blocks) {
        const int lc = synthLineCount(text, b);
        const float h = synthHeight(text, b);
        r.topY.push_back(curY);
        r.firstLine.push_back(curLine);
        r.lineCount.push_back(lc);
        r.height.push_back(h);
        curY += h + kGap;
        curLine += lc;
    }
    return r;
}

MdSyncMap buildMap(MdBlockModel& model, const std::string& text, uint64_t rev) {
    model.update(text);
    MdSyncMap map;
    const std::string snapshot = model.text();
    map.rebuild(snapshot, model.blocks(), rev, kWidth, kGap,
               [&snapshot](const MdBlock& b) { return synthHeight(snapshot, b); });
    return map;
}

// 多结构文档：标题 / 4 行段 / 单行段 / 列表 / 围栏 / 段落（块高互不相等，
// 且含非标题块——非标题块也必须有精确锚）
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

} // namespace

REGISTER_TEST(md_sync_map_block_anchor_exact,
              "md_sync_map: 块首行正向映射 == 累计块几何（误差 0，块内线性）", {
    MdBlockModel model;
    const std::string text = kDoc;
    MdSyncMap map = buildMap(model, text, 0);
    const Reference ref = buildReference(text, model.blocks());
    DOCTEST_REQUIRE(map.blockCount() == ref.topY.size());
    DOCTEST_REQUIRE(map.blockCount() >= 5);  // 标题/段/短段/列表/围栏/尾段

    // ① 每个块首行：previewYForEditorLine(firstLine_k) == Σ_{i<k}(H_i+gap)
    //    （整数高度 + 整数 gap 的 float 累加，期望值逐位相等 = 误差 0）
    for (size_t k = 0; k < ref.topY.size(); ++k) {
        const float y = map.previewYForEditorLine(ref.firstLine[k]);
        DOCTEST_CHECK(std::fabs(y - ref.topY[k]) < 1e-3f);
        DOCTEST_CHECK(std::fabs(map.blockTopY(k) - ref.topY[k]) < 1e-3f);
        DOCTEST_CHECK_EQ(map.blockFirstLine(k), ref.firstLine[k]);
    }
    // 恒真陷阱：下面这个循环只要文档里没有「非标题且 topY>0」的块就整个跳过断言，
    // 恒绿。它之所以还绿，靠的是 kDoc 头部那个非零标题块把后续块顶推到 y>0；
    // 换夹具时必须重看这条。
    bool sawNonHeadingAnchor = false;
    for (size_t k = 0; k < ref.topY.size(); ++k) {
        if (model.blocks()[k].kind != module::markdown::MdBlockKind::Heading &&
            ref.topY[k] > 0.0f) {
            sawNonHeadingAnchor = true;
        }
    }
    DOCTEST_CHECK(sawNonHeadingAnchor);

    // ② 块内：整数行 i → topY + (i-fl)/lc*H 精确线性
    for (size_t k = 0; k < ref.topY.size(); ++k) {
        for (int i = 1; i < ref.lineCount[k]; ++i) {
            const double line = ref.firstLine[k] + i;
            const float expected = ref.topY[k] +
                                   static_cast<float>(i) / ref.lineCount[k] * ref.height[k];
            DOCTEST_CHECK(std::fabs(map.previewYForEditorLine(line) - expected) < 0.01f);
        }
    }

    // ③ 单调：连续行 → y 不减
    float prev = -1.0f;
    for (int L = 0; L <= countLines(text); ++L) {
        const float y = map.previewYForEditorLine(L);
        DOCTEST_CHECK(y >= prev - 1e-6f);
        prev = y;
    }
    // 末行之后钳到末块底部
    DOCTEST_CHECK(std::fabs(map.previewYForEditorLine(1e9) - map.totalHeight()) < 1e-3f);
    return true;
})

REGISTER_TEST(md_sync_map_roundtrip_bidirectional,
              "md_sync_map: 反向映射是正向反函数（round-trip 恒等，双向）", {
    MdBlockModel model;
    const std::string text = kDoc;
    MdSyncMap map = buildMap(model, text, 0);
    const Reference ref = buildReference(text, model.blocks());

    // 逐整数行（只遍历有效行 0..countLines-1；超出文末的行正向映射钳到
    // 文末，round-trip 只对文内行有恒等语义）
    for (int L = 0; L < countLines(text); ++L) {
        const float y = map.previewYForEditorLine(L);
        const double back = map.editorLineForPreviewY(y);
        if (!(std::fabs(back - L) < 1e-4)) {
            std::cerr << "L=" << L << " y=" << y << " back=" << back << std::endl;
        }
        DOCTEST_CHECK(std::fabs(back - L) < 1e-4);
    }
    // 块首行 round-trip：y == blockTopY（反向命中同一锚点）
    for (size_t k = 0; k < ref.topY.size(); ++k) {
        const float y = map.previewYForEditorLine(ref.firstLine[k]);
        DOCTEST_CHECK_EQ(y, map.blockTopY(k));
        DOCTEST_CHECK(std::fabs(map.editorLineForPreviewY(y) - ref.firstLine[k]) < 1e-4);
    }
    // 间隙中点 → 钳到下一块首行（编辑器空行 ↔ 预览块间隙的合法对应）
    for (size_t k = 0; k + 1 < ref.topY.size(); ++k) {
        const float gapMid = ref.topY[k] + ref.height[k] + kGap * 0.5f;
        const double line = map.editorLineForPreviewY(gapMid);
        DOCTEST_CHECK(std::fabs(line - ref.firstLine[k + 1]) < 1e-4);
    }
    // 单调不减（反向）
    double prevL = -1.0;
    for (float y = 0.0f; y <= map.totalHeight(); y += 5.0f) {
        const double L = map.editorLineForPreviewY(y);
        DOCTEST_CHECK(L >= prevL - 1e-9);
        prevL = L;
    }
    // 越界钳制
    DOCTEST_CHECK_EQ(map.editorLineForPreviewY(-10.0f), 0.0);
    return true;
})

REGISTER_TEST(md_sync_map_insert_delete_no_drift,
              "md_sync_map: 块插入/删除后目标块锚点位移 == 新块 H+gap（不漂）", {
    const std::string base = kDoc;
    const std::string inserted = "ins line A\nins line B\n\n";
    const std::string text1 = inserted + base;

    MdBlockModel model;
    const std::string snap0 = (model.update(base), model.text());
    MdSyncMap map0;
    map0.rebuild(snap0, model.blocks(), 0, kWidth, kGap,
                 [&snap0](const MdBlock& b) { return synthHeight(snap0, b); });
    const Reference ref0 = buildReference(base, model.blocks());
    DOCTEST_REQUIRE(ref0.topY.size() >= 5);
    const size_t tgt = ref0.topY.size() - 1;  // 尾段块（非标题、深层）
    const float yBefore = map0.previewYForEditorLine(ref0.firstLine[tgt]);

    // 插入后：目标块内容未变 → 后缀对齐继承 stableId；锚点位移必须
    // 插入块的 H + gap（锚点跟随块几何，不随「节内摊分」漂）
    const std::string snap1 = (model.update(text1), model.text());
    MdSyncMap map1;
    map1.rebuild(snap1, model.blocks(), 1, kWidth, kGap,
                 [&snap1](const MdBlock& b) { return synthHeight(snap1, b); });
    const Reference ref1 = buildReference(text1, model.blocks());
    DOCTEST_REQUIRE(ref1.topY.size() == ref0.topY.size() + 1);
    // 插入块（新序列块 0）的高 + gap
    const float delta = ref1.height[0] + kGap;
    const float yAfter = map1.previewYForEditorLine(ref1.firstLine[tgt + 1]);
    DOCTEST_CHECK(std::fabs((yAfter - yBefore) - delta) < 1e-3f);
    // 目标块自身高度未变（memo 按 stableId 复用 → 逐位相等）
    DOCTEST_CHECK_EQ(map1.blockTopY(tgt + 1) - delta, map0.blockTopY(tgt));

    // 删除回去：映射恢复原值
    const std::string snap2 = (model.update(base), model.text());
    MdSyncMap map2;
    map2.rebuild(snap2, model.blocks(), 2, kWidth, kGap,
                 [&snap2](const MdBlock& b) { return synthHeight(snap2, b); });
    const Reference ref2 = buildReference(base, model.blocks());
    DOCTEST_CHECK(std::fabs(map2.previewYForEditorLine(ref2.firstLine[tgt]) - yBefore) < 1e-3f);
    return true;
})

REGISTER_TEST(md_sync_map_memo_dirty_only,
              "md_sync_map: (rev,width,gap,块数) 命中零重算；只有新块付 heightFn", {
    const std::string base = kDoc;
    MdBlockModel model;
    const std::string snap0 = (model.update(base), model.text());
    const std::vector<MdBlock> blocks0 = model.blocks();

    MdSyncMap map;
    int calls = 0;
    auto fn = [&](const MdBlock& b) { ++calls; return synthHeight(snap0, b); };
    map.rebuild(snap0, blocks0, 0, kWidth, kGap, fn);
    DOCTEST_CHECK_EQ(static_cast<size_t>(calls), blocks0.size());  // 首次全测
    DOCTEST_CHECK_EQ(map.rebuildCount(), 1u);

    // 同键重复 rebuild → 守卫直接返回（零重算、计数器不动）
    map.rebuild(snap0, blocks0, 0, kWidth, kGap, fn);
    DOCTEST_CHECK_EQ(map.rebuildCount(), 1u);
    DOCTEST_CHECK_EQ(static_cast<size_t>(calls), blocks0.size());

    // 文首插入一个新段落 → 后缀 (kind,hash) 对齐全部 stableId 继承 → 只有
    // 新块付测量（不吞后随块：ATX 标题必打断段落，"intro\n# Section" 安全切开）
    const std::string text1 = "intro new para\n" + base;
    const std::string snap1 = (model.update(text1), model.text());
    const std::vector<MdBlock> blocks1 = model.blocks();
    DOCTEST_REQUIRE(blocks1.size() == blocks0.size() + 1);
    auto fn1 = [&](const MdBlock& b) { ++calls; return synthHeight(snap1, b); };
    map.rebuild(snap1, blocks1, 1, kWidth, kGap, fn1);
    DOCTEST_CHECK_EQ(map.rebuildCount(), 2u);
    DOCTEST_CHECK_EQ(static_cast<size_t>(calls), blocks0.size() + 1);  // 恰好 +1

    // 宽度变化 → 全量重测（memo 键含宽度）
    map.rebuild(snap1, blocks1, 1, 900.0f, kGap, fn1);
    DOCTEST_CHECK_EQ(static_cast<size_t>(calls), 2 * blocks0.size() + 2);
    // memo 只保留存活 id，换代即淘汰死 id（不无界增长）
    return true;
})
