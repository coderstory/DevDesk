// Markdown 块级元素模型：把源码按安全块边界切成连续覆盖的块序列，供预览按块
// 拼装，实现「编辑只重算受影响块」。
// 切分原则（保证「逐块渲染 == 整篇渲染」）：只在空行后 / 围栏块 / GFM 表格段 /
// ATX 标题 / 分隔线处切；列表块与引用块绝不从中间切开；围栏外出现链接引用定义
// 行 → 整篇退化为单块。
// update：文本相等零重算，否则重扫后按 (kind, hash) 对齐旧序列 —— 未变块继承
// stableId。解析缓存 LRU 仅 8 条，故增量收益对 ≤8 块文档完整成立。
// 纯函数 + 单 UI 线程使用，不抛异常、无 EUI-NEO 依赖（可单测）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace module::markdown {

// 块类型：CodeFence/Table 独立成块直渲；Heading/Divider 为安全切分点；
// Paragraph/List/Quote 覆盖其余行。
enum class MdBlockKind {
    Paragraph,
    Heading,
    CodeFence,
    Table,
    List,
    Quote,
    Divider,
};

// 一个块 = 源码连续字节区间 [start, end)（含块尾换行/空行）。块序列连续覆盖 [0, size)。
struct MdBlock {
    MdBlockKind kind = MdBlockKind::Paragraph;
    size_t start = 0;
    size_t end = 0;
    uint64_t hash = 0;      // FNV-1a(块源码字节)：增量 diff 的内容身份
    int stableId = -1;      // update() 维护：内容不变跨编辑保持的 id
};

// FNV-1a 64 位（与内部同源，供测试/日志复用）
uint64_t mdBlockHash(std::string_view text);

// 全文切块（纯函数；stableId 恒为 -1）
std::vector<MdBlock> segmentMarkdownBlocks(const std::string& text);

// 重载后活动块内容映射：在【新块序列】里定位旧活动块。
//   ① (kind, hash, stableId) 全等命中 → 该下标（stableId 不重复，可区分实例）
//   ② 无命中 → 退回 (kind, hash) 等价类，取 |start - nearByte| 最小者（等距取小）
//   ③ 全无 → npos（内容被删改消失，调用侧钳位）。纯函数、O(块数)。
size_t mdFindBlockByContent(const std::vector<MdBlock>& blocks, MdBlockKind kind,
                            uint64_t hash, int stableId, size_t nearByte);

// kind 中文名（日志用）
const char* mdBlockKindName(MdBlockKind kind);

// update 结果摘要（脏区间按新块序列下标，闭区间；无脏块 dirtyFirst=1 > dirtyLast=0）
struct MdBlockUpdate {
    bool changed = false;
    size_t dirtyFirst = 0;
    size_t dirtyLast = 0;
    size_t reused = 0;       // (kind,hash) 与上一版对齐复用的块数
    bool incremental = false; // true = 走了区段重扫；false = 全文回退重扫
};

// 块模型：持有文本快照 + 切块结果。update(text) 是唯一入口。
class MdBlockModel {
public:
    MdBlockUpdate update(const std::string& text);

    const std::vector<MdBlock>& blocks() const { return blocks_; }
    const std::string& text() const { return text_; }

// 块序号（二分）；offset == size() 归最后一块
    size_t blockIndexAt(size_t offset) const;

private:
    std::string text_;
    std::vector<MdBlock> blocks_;
    int nextStableId_ = 0;
};

} // namespace module::markdown
