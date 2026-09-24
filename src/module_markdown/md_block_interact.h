// src/module_markdown/md_block_interact.h
// 块级交互（Phase 3 ⑤）：预览块点击 → 编辑器光标定位、hover 高亮状态——纯函数底座。
//
// 接线现实（markdown_module 预览逐块渲染）：上游 components::markdown 无逐块
// 命中/指针事件 API（纯展示：无 interactive、link 也不可点——KNOWN_LIMITATIONS
// #9），DevDesk 侧给【每个块】（CodeFence 自绘块与上游块一视同仁）包一层
// 「固定高 stack + 透明 MouseArea 覆盖层」：hit 几何 = MdSyncMap 的块高度表
// （与 ④ 同步滚动同一张表，块边界误差 0 的同一依据）。本文件是 hit 回调背后
// 的纯数学：区域局部 y → 预览 content y → 反查编辑器行 → 行首字节 + hover
// 状态机 + hover 混色，全部可单测、不依赖运行循环（仅值类型 eui::Color）。
//
// 反查语义精确性：previewYForEditorLine / editorLineForPreviewY 是同一分段
// 线性函数与它的反函数（④ 回归锁 round-trip 恒等）——点击定位直接消费反函数，
// 块边界行号精确；块内行号按「行数 × 等高」分摊，误差与 ④ 正向映射同级。
// 点击 x → 行内列【不映射】：预览折行/宽度（阅读态 clamp 760）与编辑器布局
// 不同源，行首是唯一无歧义的落点。
#pragma once

#include "md_block_model.h"
#include "md_sync_map.h"

#include <eui/types.h>

#include <cstddef>
#include <string>
#include <vector>

namespace module::markdown {

// 一次块点击的命中结果（坐标系 = 编辑器）
struct MdPreviewJump {
    bool ok = false;
    size_t blockIndex = 0;   // 块序列下标（与 map / blocks 同序）
    int stableId = -1;       // 命中块身份（blocks 越界防御时 -1）
    double editorLine = 0.0; // 反查得到的分数编辑器行（诊断/测试用）
    int targetLine = 0;      // 目标行（floor + 块内钳制，0 起）
    size_t cursorByte = 0;   // 目标行行首字节偏移（光标落点）
};

// 块 hit 回调 → 编辑器光标定位。blockIndex = 被点的块（每块自带 MouseArea，
// 不做全局 y 搜索）；localY = 点击在 hit 区域内的 y（理论 ∈ [0, H_k)，越界
// 一律钳进块内，防御取整毛刺）。cursorByte = lineStartOffset(text, targetLine)。
// map 空 / blockIndex 越界 → ok=false（调用方直接忽略本次点击）。
MdPreviewJump mdResolvePreviewJump(const std::string& text,
                                   const std::vector<MdBlock>& blocks,
                                   const MdSyncMap& map,
                                   size_t blockIndex,
                                   float localY);

// 逐块 hover 状态：同一时刻至多一个 hovered stableId。
// 乱序保护（快速划过相邻块时 enter/leave 到达顺序不保证）：leave 只在 id
// 与当前 hovered 一致时清除——迟到的旧块 leave 不得擦掉新块 hover。
// 切笔记/重开文档调 reset()（块身份换代，旧 hover 无意义）。
struct MdPreviewHoverState {
    int stableId = -1;

    void enter(int id) { stableId = id; }
    void leave(int id) {
        if (stableId == id) stableId = -1;
    }
    bool hovered(int id) const { return id >= 0 && stableId == id; }
    void reset() { stableId = -1; }
};

// hover 背景混色：base 按 amount∈[0,1] 向 tint 线性混合（rgb 通道，alpha
// 保留 base）。CodeFence 自绘 bg 不透明且盖在 hover 底衬位置 → 用它把
// codeBackground 混向 hover 色；普通（上游 markdown）块文字底透明，直接画
// tint 矩形垫在文字下即可，不走本函数。
eui::Color mdHoverMix(eui::Color base, eui::Color tint, float amount);

} // namespace module::markdown
