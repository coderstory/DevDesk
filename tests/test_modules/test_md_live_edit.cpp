// tests/test_modules/test_md_live_edit.cpp
// Markdown Phase 3 · M2 纯函数层单测（md_live_edit.h）
// 依据：docs/superpowers/specs/2026-09-27-markdown-phase3-block-render-design.md
//       §8.1 纯函数用例表（活动块推导 / 块局部偏移 / 内容贴合高度公式）
//
// 只测纯计算：不建 Ui、不碰 EUI-NEO、不起 app。行数测量按签名注入假函数 ——
// 真实场景注入的是 InputModel::measureLines（消费侧接线在 markdown_module.cpp，
// 其行为由 test_markdown.cpp 的无头 UI 用例覆盖）。

#include "doctest.h"

#include "module_markdown/md_block_model.h"
#include "module_markdown/md_live_edit.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using module::markdown::MdBlock;
using module::markdown::MdBlockModel;
using module::markdown::mdLiveActiveBlockIndex;
using module::markdown::mdLiveBlockLocalOffset;
using module::markdown::mdLiveInputHeight;

// input 内部行高系数（components/input.h：fontSize × 1.2）——公式期望值同源
constexpr float kLineH = 1.2f;

// 浮点近似比较（1.2f 不是二进制精确值：差值断言不能按位相等）
bool near(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

// 假测量函数：按 '\n' 切行（与 measureLines 的切法一致：末行无换行也算一行）
int fakeLines(const std::string& text, float /*textWidth*/) {
    return static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
}

// 建好块模型的便捷入口（纯函数用例都用它）
MdBlockModel modelOf(const std::string& text) {
    MdBlockModel m;
    m.update(text);
    return m;
}

} // namespace

// ---- 活动块推导：空文档 / 单块 / cursor==size 归末块 ----

REGISTER_TEST(md_live_active_block_empty_and_single,
              "md_live_edit: 活动块推导 空文档与单块（含 cursor==size）", {
    // 空文档：无块（切分器对空串返回空序列）→ 恒 0，任何偏移都不越界
    const MdBlockModel empty = modelOf("");
    DOCTEST_CHECK(empty.blocks().empty());
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(empty, 0), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(empty, 9999), static_cast<size_t>(0));

    // 单块文档："hello\n"（无空行 → 一个段落块）
    const std::string one = "hello\n";
    const MdBlockModel m = modelOf(one);
    DOCTEST_REQUIRE_EQ(m.blocks().size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, 0), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, 3), static_cast<size_t>(0));
    // cursor == size（光标停在文末）→ 归末块（否则文末打字会没有活动块）
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, one.size()),
                     static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, one.size() + 100),
                     static_cast<size_t>(0));
    return true;
})

// ---- 活动块推导：块首 / 块尾字节 / 空行归属 / 越界 ----

REGISTER_TEST(md_live_active_block_boundaries,
              "md_live_edit: 活动块推导 块首块尾字节与空行归属", {
    // 块序列连续覆盖 [0,size)：b[k].end == b[k+1].start，块间空行并入【后一块】
    // 头部（md_block_model 的切分口径）——故边界字节归后一块。
    const std::string doc = "# T\n\npara one\n\npara two\n";
    const MdBlockModel m = modelOf(doc);
    const auto& bs = m.blocks();
    DOCTEST_REQUIRE(bs.size() >= 2);

    size_t covered = 0;
    for (size_t k = 0; k < bs.size(); ++k) {
        DOCTEST_CHECK_EQ(bs[k].start, covered);   // 无缝无重叠
        covered = bs[k].end;
        // 块首字节 → 本块；块内任意字节 → 本块
        DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, bs[k].start), k);
        if (bs[k].end > bs[k].start) {
            DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, bs[k].end - 1), k);
        }
        // 块尾字节（== 后一块 start）→ 后一块；末块（end == size）→ 自己
        const size_t atEnd = (k + 1 < bs.size()) ? k + 1 : k;
        DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, bs[k].end), atEnd);
    }
    DOCTEST_CHECK_EQ(covered, doc.size());
    // cursor == size 归末块（末块 end == size 的同一件事，单列一条锁语义）
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, doc.size()),
                     static_cast<size_t>(bs.size() - 1));

    // 首块之后紧跟的空行属于后一块：块 k 的 end 正好是空行起点
    DOCTEST_REQUIRE(bs.size() > 1);
    DOCTEST_CHECK_EQ(doc[bs[1].start], '\n');   // 第二块以空行开头
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, bs[1].start),
                     static_cast<size_t>(1));
    return true;
})

// ---- 活动块推导：CRLF 行尾 ----

REGISTER_TEST(md_live_active_block_crlf,
              "md_live_edit: 活动块推导 CRLF 行尾", {
    const std::string doc = "# T\r\n\r\nnext\r\n";
    const MdBlockModel m = modelOf(doc);
    const auto& bs = m.blocks();
    DOCTEST_REQUIRE(bs.size() >= 2);
    // '\r' 属行体（lineBody 只去尾），块区间仍按字节精确切
    DOCTEST_CHECK_EQ(bs[0].start, static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(bs[0].end, static_cast<size_t>(5));   // "# T\r\n"
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, 4), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, 5), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, doc.size()),
                     static_cast<size_t>(bs.size() - 1));
    // 局部偏移把 '\r' 也算进块内（输入框看到的就是块源码字节）
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(bs[1], doc.size()), doc.size() - 5);
    return true;
})

// ---- 活动块推导：中文多字节边界（纯字节算术，不校验 UTF-8） ----

REGISTER_TEST(md_live_active_block_utf8,
              "md_live_edit: 活动块推导 中文多字节边界", {
    const std::string doc = "中文段落\n\n第二段\n";   // 每汉字 3 字节
    const MdBlockModel m = modelOf(doc);
    const auto& bs = m.blocks();
    DOCTEST_REQUIRE(bs.size() >= 2);
    // 逐字节扫全篇：每个字节偏移都能定位到唯一块（含字中间字节，不越界/不崩）
    for (size_t off = 0; off <= doc.size(); ++off) {
        const size_t k = mdLiveActiveBlockIndex(m, off);
        DOCTEST_CHECK(k < bs.size());
        if (off < doc.size()) {
            DOCTEST_CHECK(off >= bs[k].start && off < bs[k].end);
        }
    }
    // 汉字中间字节（"中" = E4 B8 AD，偏移 1/2 在字内）→ 仍归首块
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, 1), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveActiveBlockIndex(m, 2), static_cast<size_t>(0));
    // 块局部偏移：字节差（光标始终落在字符边界，字内偏移只是防御性输入）
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(bs[0], bs[0].start + 3),
                     static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(bs[0], bs[0].start),
                     static_cast<size_t>(0));
    return true;
})

// ---- 块局部偏移：钳制两端 ----

REGISTER_TEST(md_live_block_local_offset_clamp,
              "md_live_edit: 块局部偏移 两端钳制", {
    const MdBlock b{.kind = module::markdown::MdBlockKind::Paragraph, .start = 10,
                    .end = 16, .hash = 0, .stableId = 7};   // [10,16) → 块长 6
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 0), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 9), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 10), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 11), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 15), static_cast<size_t>(5));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 16), static_cast<size_t>(6));
    DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, 999), static_cast<size_t>(6));
    // 往返恒等：块内偏移 → 文档偏移 → 局部偏移
    for (size_t i = 0; i <= 6; ++i) {
        DOCTEST_CHECK_EQ(mdLiveBlockLocalOffset(b, b.start + i), i);
    }
    return true;
})

// ---- 内容贴合高度：注入假测量函数（N 行 → N×lineH + inset×2 + 1） ----

REGISTER_TEST(md_live_input_height_formula,
              "md_live_edit: 贴合高度公式（N 行/0 行/字号与 inset 变化）", {
    const auto lineCount = module::markdown::MdLiveLineCountFn(fakeLines);
    const std::string two = "a\nb\n";       // 2 个换行 → 3 行
    DOCTEST_CHECK_EQ(fakeLines(two, 0.0f), 3);

    // 3 行 × 15px × 1.2 + inset(10)×2 + 1
    const float h3 = mdLiveInputHeight(two, 400.0f, 15.0f, 10.0f, lineCount);
    DOCTEST_CHECK_EQ(h3, 3.0f * 15.0f * kLineH + 20.0f + 1.0f);

    // 1 行（无换行）
    const float h1 = mdLiveInputHeight("x", 400.0f, 15.0f, 10.0f, lineCount);
    DOCTEST_CHECK_EQ(h1, 1.0f * 15.0f * kLineH + 20.0f + 1.0f);
    DOCTEST_CHECK(h3 > h1);

    // 0 行（空文本 + 假函数返回 0）→ 钳成 1 行：input 至少有一行 caret 高
    const auto zeroLines = module::markdown::MdLiveLineCountFn(
        [](const std::string&, float) { return 0; });
    DOCTEST_CHECK_EQ(mdLiveInputHeight("", 400.0f, 15.0f, 10.0f, zeroLines),
                     h1);

    // 字号变化：行高部分随字号线性放大（inset 部分不变）
    const float h3Big = mdLiveInputHeight(two, 400.0f, 30.0f, 10.0f, lineCount);
    DOCTEST_CHECK_EQ(h3Big, 3.0f * 30.0f * kLineH + 20.0f + 1.0f);
    DOCTEST_CHECK(near(h3Big - h3, 3.0f * 15.0f * kLineH));

    // inset 变化：只动 inset 部分
    const float h3Wide = mdLiveInputHeight(two, 400.0f, 15.0f, 20.0f, lineCount);
    DOCTEST_CHECK(near(h3Wide - h3, 20.0f));

    // 空测量函数（未注入）→ 按 1 行兜底，不崩
    DOCTEST_CHECK_EQ(mdLiveInputHeight(two, 400.0f, 15.0f, 10.0f, {}), h1);
    return true;
})

// ---- 内容贴合高度：超宽折行由注入的测量函数决定（宽度透传） ----

REGISTER_TEST(md_live_input_height_wrap,
              "md_live_edit: 贴合高度 超宽折行计数 + 文本宽透传", {
    float seenWidth = -1.0f;
    const auto wrapLines = module::markdown::MdLiveLineCountFn(
        [&seenWidth](const std::string&, float textWidth) {
            seenWidth = textWidth;
            // 窄列折成 5 行、宽列 2 行（模拟 measureLines 的软折行）
            return textWidth < 100.0f ? 5 : 2;
        });
    const float narrow = mdLiveInputHeight("aaaa", 80.0f, 15.0f, 10.0f, wrapLines);
    DOCTEST_CHECK_EQ(seenWidth, 80.0f);      // 文本宽原样透传给测量函数
    DOCTEST_CHECK_EQ(narrow, 5.0f * 15.0f * kLineH + 20.0f + 1.0f);
    const float wide = mdLiveInputHeight("aaaa", 400.0f, 15.0f, 10.0f, wrapLines);
    DOCTEST_CHECK_EQ(seenWidth, 400.0f);
    DOCTEST_CHECK_EQ(wide, 2.0f * 15.0f * kLineH + 20.0f + 1.0f);
    DOCTEST_CHECK(narrow > wide);
    return true;
})
