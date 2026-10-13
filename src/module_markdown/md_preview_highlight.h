// src/module_markdown/md_preview_highlight.h
// 预览代码块语法高亮：CodeFence 块源码 → 预览显示串 + 着色 runs。
// 上游 components::markdown 代码块单色且不吃预解析注入，故 CodeFence 预览改为 DevDesk
//   自绘（drawPreviewCodeBlock，几何同上游 renderCodeBlock）；本文件是其纯函数底座。
// 复用：围栏语义 scanFences、色表 MdHighlightColors、run 载体 eui::TextRun、
//   关键字多色 md_keyword_highlight（C++/Python 手写词法器）。
// 着色：预览不显示围栏线与 lang 标签，显示串 = 内容行以 \n 连接；受支持语言出五色 runs，
//   未覆盖/空 lang 回退逐行 code 色单 run。
#pragma once

#include "md_highlight.h"
#include "md_line_scanner.h"

#include <eui/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace module::markdown {

// 一个 CodeFence 块的高亮产物（预览显示串 + displayText 坐标系的着色 runs）
struct MdPreviewCodeHighlight {
    bool ok = false;                  // false = 块源码无法解析为恰好一个围栏块（防御性；真触发说明上游分块异常，调用方回退上游渲染）
    std::string displayText;          // 预览渲染文本（含 lang 首行，若有）
    std::vector<eui::TextRun> runs;   // displayText 字节坐标，升序不重叠
    std::string lang;                 // 围栏 info 首词（md4c MD_LANG 语义；日志/测试用）
};

// blockSource = MdBlock(kind==CodeFence) 的块源码；colors = mdHighlightColors(theme tokens)，与编辑器同表。
MdPreviewCodeHighlight mdPreviewCodeHighlight(std::string_view blockSource,
                                              const MdHighlightColors& colors);

// 脏块高亮缓存：按 (stableId, 块内容 hash) 双键命中——块不变复用零重算，改动重算一次，
// 删除则 pruneActive 回收；setColors 整表作废（主题切换罕见，重算便宜）。纯状态、单 UI 线程。
class MdPreviewCodeCache {
public:
    // 每帧注入当前色表；色表变化时清空全部条目（下一次 get 全部 miss 重算）
    void setColors(const MdHighlightColors& colors);

    // 命中返回条目（引用到下一次 put/setColors/prune 前有效）；未命中返回 nullptr 并计 miss。
    const MdPreviewCodeHighlight* get(int stableId, uint64_t hash) const;
    void put(int stableId, uint64_t hash, MdPreviewCodeHighlight highlight);

    // 只保留 activeStableIds 中的条目（预览块循环末尾调用）
    void pruneActive(const std::vector<int>& activeStableIds);

    size_t size() const { return entries_.size(); }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    struct Entry {
        uint64_t hash = 0;
        MdPreviewCodeHighlight highlight;
    };
    std::unordered_map<int, Entry> entries_;
    MdHighlightColors colors_{};
    bool colorsSet_ = false;
    mutable uint64_t hits_ = 0;
    mutable uint64_t misses_ = 0;
};

} // namespace module::markdown
