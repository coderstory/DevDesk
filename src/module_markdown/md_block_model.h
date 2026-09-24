// src/module_markdown/md_block_model.h
// Markdown 块级元素模型（Phase 3 首块，ux-design §4 方案 2 / §7 Phase 3）。
//
// 职责：把整篇源码按【安全块边界】切成连续覆盖的块序列（段落/标题/代码块/
// 表格/列表/引用/分隔线），供预览按块拼装 components::markdown，实现
// 「编辑只重算受影响块」——不变块的内容哈希不变 → 上游按 source 键控的
// md4c 解析缓存（cachedMarkdownDocument，LRU 8 条）天然命中，只重析脏块。
//
// 安全切分原则（保证「逐块渲染 == 整篇渲染」，与 md4c 分块语义对齐）：
//   - 只在这些位置切：空行之后、围栏块（```/~~~，语义与 scanFences 同源）、
//     GFM 表格行段（| 开头 + 分隔行校验）、ATX 标题行（列 0，必打断段落）、
//     分隔线（仅空行后的 ---/***/___；紧跟文本行的是 setext 标题下划线，
//     留在段落块内交给 md4c）
//   - 列表块 = 整个最大列表（含跨空行的松散项），引用块 = 到空行为止的
//     最大 > 段——二者绝不从中间切开（有序列表编号/松散度是跨项状态）
//   - 段落/标题/表格/代码/分隔线各自独立终结（下一块要么空行隔开要么必打断）
//
// 增量语义（MdBlockModel::update）：文本相等 → 零重算；否则重跑分块扫描
// （O(n) 行扫描，与既有 scanFences 同量级），完成后按 (kind, hash) 双端
// 对齐旧序列——内容未变的块继承 stableId（块身份跨编辑稳定），变化窗口
// 以新序列脏区间下标报告。「编辑只重算受影响块」的收益在【解析/布局层】：
// 预览逐块渲染时，未变块按内容串命中上游 md4c 解析缓存与行内布局缓存，
// 只有脏块重新解析（整篇渲染模式则任何一字符变化都全文重析）。
// （连分块扫描本身也增量化——变化窗口区段重扫——留给实测超预算再做。）
//
// 纯函数 + 单 UI 线程使用的轻量状态机，不抛异常、无 EUI-NEO 依赖（可单测）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace module::markdown {

// 块类型（Phase 3 首批：CodeFence/Table 独立成块直渲；Heading/Divider
// 为安全切分点；Paragraph/List/Quote 覆盖其余全部行）
enum class MdBlockKind {
    Paragraph,
    Heading,
    CodeFence,
    Table,
    List,
    Quote,
    Divider,
};

// 一个块 = 源码的一个连续字节区间 [start, end)（含块尾换行/空行）。
// 全文档的块序列连续覆盖 [0, size)，无缝无重叠。
struct MdBlock {
    MdBlockKind kind = MdBlockKind::Paragraph;
    size_t start = 0;
    size_t end = 0;
    uint64_t hash = 0;      // FNV-1a(块源码字节)：增量 diff 的内容身份
    int stableId = -1;      // update() 维护：内容不变跨编辑保持的 id
};

// FNV-1a 64 位（与 md_block_model 内部同源，供测试/日志复用）
uint64_t mdBlockHash(std::string_view text);

// 全文切块（纯函数，MdBlockModel 内部与测试共用；stableId 恒为 -1）
std::vector<MdBlock> segmentMarkdownBlocks(const std::string& text);

// kind 中文名（日志用）
const char* mdBlockKindName(MdBlockKind kind);

// 一次 update 的结果摘要（脏区间按新块序列下标，闭区间；无脏块统一用
// 哨兵 dirtyFirst=1 > dirtyLast=0）
struct MdBlockUpdate {
    bool changed = false;
    size_t dirtyFirst = 0;
    size_t dirtyLast = 0;
    size_t reused = 0;       // (kind,hash) 与上一版对齐复用的块数
    bool incremental = false; // true = 走了区段重扫；false = 全文回退重扫
};

// 块模型：持有一份文本快照 + 切块结果。update(text) 是唯一入口。
class MdBlockModel {
public:
    MdBlockUpdate update(const std::string& text);

    const std::vector<MdBlock>& blocks() const { return blocks_; }
    const std::string& text() const { return text_; }

    // 块序号（二分），字节偏移落在哪个块；offset == size() 归最后一块
    size_t blockIndexAt(size_t offset) const;

private:
    std::string text_;
    std::vector<MdBlock> blocks_;
    int nextStableId_ = 0;
};

} // namespace module::markdown
