// src/module_markdown/md_sync_map.h
// 同步滚动块模型精确锚（Phase 3 ④，ux-design §5.3 升级）。
//
// 把编辑器 ↔ 预览的滚动映射从「标题锚点 + 估算高度 + 节内按行线性插值」
// 升级为「块字节区间精确锚」：编辑器视口顶行 → 块模型定位所在块 → 预览
// 侧该块的几何位置。几何公式与预览逐块渲染路径完全同源——
//   预览块 k 顶部 y = Σ_{i<k} (H_i + blockGap)
// 其中 H_i = 注入的 heightFn(块 i)（消费侧用 MarkdownBuilder::estimateHeight，
// 与上游 renderBlock 共用 cachedInlineLayout/estimateMarkdownTextHeight 测量，
// 即块边界的估计 == 实际布局，误差 0）。块内线性插值：段内各行按
// 「行数 × 等高」分摊——同一段落各行换行数相同时（不折行的短行）块内
// 映射同样精确；异高极端情形误差有界（≤ 块高，且远小于旧的整节摊分）。
//
// 反向（预览 y → 编辑器行）是同一分段线性函数的反函数，块边界精确可逆
// （round-trip 恒等）；供 P3-⑤ 块级交互（点预览块定位编辑器）接线。
//
// 增量几何缓存：rebuild() 只在 (修订, 宽度, blockGap, 块数) 变化时重算；
// 块高度按 stableId 备忘（MdBlockModel 保证 stableId ↔ 内容一一对应），
// 未变块零重测，只有新/脏块付 heightFn（estimateHeight 的解析/布局成本
// 又命中上游按 source 键控的缓存）。取代旧 [14] 的 yA/yB 单锚缓存——
// 旧缓存换锚即重算，且锚只在标题：非标题块深位置的映射被整节摊分误差
// 污染；新结构一次建全表，任意行 O(log n) 查询，锚 = 每个块。
//
// 纯数学 + 注入测量函数，不依赖 EUI-NEO（estimateHeight 由消费侧传入）、
// 不抛异常、可单测（同 md_block_model 的解耦约定）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "md_block_model.h"

namespace module::markdown {

class MdSyncMap {
public:
    // 块 → 预览高度（像素）。由消费侧注入（markdown_module 用
    // MarkdownBuilder::estimateHeight(块源码, width, style)）；本类不感知
    // 排版实现，单测可用纯算术函数替代。要求返回值 > 0。
    using HeightFn = std::function<float(const MdBlock&)>;

    // 重建几何表：(rev, width, blockGap, blocks.size()) 全等 → 直接返回
    // （rev 单调且与文本内容一一对应，同 rev ⇒ 同 blocks）。否则逐块取高
    // （memo 命中免测）、重排 firstLine/累计 topY。
    void rebuild(const std::string& text,
                 const std::vector<MdBlock>& blocks,
                 uint64_t rev,
                 float width,
                 float blockGap,
                 const HeightFn& heightFn);

    bool empty() const { return n_ == 0; }
    size_t blockCount() const { return n_; }
    float totalHeight() const;

    // 块 k 顶部在预览的 y（精确锚点位置；k 越界返回 0）
    float blockTopY(size_t k) const;
    // 编辑器中块 k 的首行行号（越界返回 0）
    int blockFirstLine(size_t k) const;
    // 块 k 的预览高度 H_k（P3-⑤：逐块 hit 覆盖层 / hover 包裹的几何来源；
    // 越界返回 0）
    float blockHeight(size_t k) const;
    // 块 k 占编辑器行数（≥1；P3-⑤ 命中行钳制边界；越界返回 0）
    int blockLineCount(size_t k) const;

    // 正向：编辑器行（可为小数——滚动像素 / 行高）→ 预览 y。
    // 行 < 首块首行 → 0；行 > 末块末行 → 末块底部（clamp，滚动到底不越界）。
    float previewYForEditorLine(double editorLine) const;

    // 反向：预览 y → 编辑器行（同一函数的反函数；块边界精确，
    // y 落在块间隙时钳到该间隙前一块的末行行号 = 后一块首行）。
    double editorLineForPreviewY(float previewY) const;

    // 诊断（测试缓存/增量行为）：rebuild 实际重算次数 / heightFn 调用次数
    uint64_t rebuildCount() const { return rebuildCount_; }
    uint64_t heightComputes() const { return heightComputes_; }

private:
    struct MemoEntry {
        float width = -1.0f;  // 该高度是在哪个宽度下测的（宽度变则作废）
        float height = 0.0f;
    };

    bool built_ = false;
    uint64_t rev_ = 0;
    float width_ = -1.0f;
    float gap_ = -1.0f;
    size_t n_ = 0;
    std::vector<int> firstLine_;   // 块首编辑器行（0 起）
    std::vector<int> lineCount_;   // 块占编辑器行数（≥1，含块尾空行）
    std::vector<float> height_;    // 块预览高度（>0）
    std::vector<float> topY_;      // 块预览顶 y（含 gap 累计）
    std::unordered_map<int, MemoEntry> memo_;  // stableId → (宽度, 高度)

    uint64_t rebuildCount_ = 0;
    uint64_t heightComputes_ = 0;
};

} // namespace module::markdown
