// src/module_markdown/md_block_interact.h
// 块级交互（Phase 3 ⑤）纯函数底座：预览块点击 → 编辑器光标定位、hover 状态机。
// 上游 components::markdown 无逐块命中/指针事件 API，故每个块外包「固定高 stack +
//   透明 MouseArea」，hit 几何 = MdSyncMap 块高度表（与同步滚动同表，边界误差 0）。
// 点击 x → 行内列不映射：预览折行宽度与编辑器布局不同源，行首是唯一无歧义落点。
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
    int stableId = -1;       // 命中块身份（越界防御时 -1）
    double editorLine = 0.0; // 反查得到的分数编辑器行（诊断/测试用）
    int targetLine = 0;      // 目标行（floor + 块内钳制，0 起）
    size_t cursorByte = 0;   // 目标行行首字节偏移（光标落点）
};

// 块 hit 回调 → 编辑器光标定位。blockIndex = 被点的块，localY = 点击在 hit 区域内
// 的 y（越界钳进块内，防取整毛刺）。map 空 / blockIndex 越界 → ok=false。
MdPreviewJump mdResolvePreviewJump(const std::string& text,
                                   const std::vector<MdBlock>& blocks,
                                   const MdSyncMap& map,
                                   size_t blockIndex,
                                   float localY);

// 逐块 hover 状态：同一时刻至多一个 hovered stableId。leave 只在 id 与当前一致时
// 清除（快速划过时 enter/leave 到达乱序，迟到旧块 leave 不得擦掉新块 hover）。
struct MdPreviewHoverState {
    int stableId = -1;

    void enter(int id) { stableId = id; }
    void leave(int id) {
        if (stableId == id) stableId = -1;
    }
    bool hovered(int id) const { return id >= 0 && stableId == id; }
    void reset() { stableId = -1; }
};

// hover 背景混色：base 按 amount∈[0,1] 向 tint 线性混合（保留 base 的 alpha）。
// CodeFence 自绘 bg 不透明需走本函数；上游块文字底透明，直接画 tint 矩形即可。
eui::Color mdHoverMix(eui::Color base, eui::Color tint, float amount);

} // namespace module::markdown
