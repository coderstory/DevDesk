// src/module_markdown/md_sync_map.h
// 同步滚动块模型精确锚（Phase 3 ④）：编辑器行 ↔ 预览 y 双向映射，几何公式与预览逐块
//   渲染同源（预览块 k 顶部 y = Σ(H_i + blockGap)，H_i = 注入的 heightFn，与上游
//   renderBlock 共用测量 ⇒ 块边界误差 0）。块内按「行数 × 等高」线性插值，反函数块
//   边界精确可逆（round-trip 恒等），供块级交互点预览定位使用。
// 增量缓存：rebuild() 仅 (rev, width, blockGap, 块数) 变化时重算，块高按 stableId 备忘，
//   未变块零重测；任意行 O(log n) 查询，锚 = 每个块。纯数学 + 注入测量，不依赖 EUI-NEO。
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
    // 块 → 预览高度（像素），由消费侧注入（本类不感知排版实现，单测可用纯算术替代）；
    // 返回值要求 > 0。
    using HeightFn = std::function<float(const MdBlock&)>;

    // 重建几何表：(rev, width, blockGap, blocks.size()) 全等则直接返回（同 rev ⇒ 同 blocks）；
    // 否则逐块取高（memo 命中免测）、重排 firstLine/累计 topY。
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
    // 块 k 首行行号（越界返回 0）
    int blockFirstLine(size_t k) const;
    // 块 k 的预览高度 H_k（块级 hit/hover 几何来源；越界返回 0）
    float blockHeight(size_t k) const;
    // 块 k 占编辑器行数（≥1；P3-⑤ 命中行钳制边界；越界返回 0）
    int blockLineCount(size_t k) const;

    // 正向：编辑器行（可小数——滚动像素/行高）→ 预览 y。行 < 首块首行 → 0；
    // 行 > 末块末行 → 末块底部（clamp，滚动到底不越界）。
    float previewYForEditorLine(double editorLine) const;

    // 反向：预览 y → 编辑器行（上函数反函数，块边界精确）；y 落块间隙时钳到该
    // 间隙前块末行 = 后块首行。
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
