// src/module_markdown/md_preview_highlight.h
// 预览代码块语法高亮（Phase 3 ③）：CodeFence 块源码 → 预览显示串 + 着色 runs。
//
// 背景：上游 components::markdown 的代码块是单色渲染，且只吃 source 串、不暴露
// 预解析注入（KNOWN_LIMITATIONS #9）——预览的代码块高亮无法经上游通路达成，
// CodeFence 块的预览改为 DevDesk 侧自绘（markdown_module 的
// drawPreviewCodeBlock，几何参数与上游 renderCodeBlock 一致）。本文件提供其
// 纯函数底座：块源码解析 + 着色 runs 产出 + 脏块缓存，全部可单测、无 UI 依赖。
//
// 不造新轮（复用清单）：
//   - 围栏语义：scanFences（md_line_scanner——与编辑器高亮 / 分块器同一套状态机）
//   - 色表：MdHighlightColors（md_highlight——浅/深色由调用方从 core::theme 注入，
//     与编辑器完全同源）
//   - run 载体：eui::TextRun（上游 styled-runs：颜色只改顶点色，不动 layout/测量，
//     DSL TextBuilder .runs() 消费）
//
// 着色语义 = 与编辑器 MdHighlighter 的 token 序列等价（回归锁
// md_preview_highlight_editor_equivalence）：
//   - 编辑器围栏行（```lang 开栏 / 闭栏）整行 marker 色 → 预览不显示围栏线、
//     仅显示 lang 标签（显示串结构对齐上游 codeBlockDisplayText：
//     lang\n + 内容行以 \n 连接），lang 段 marker 色
//   - 编辑器围栏内容行整行 code 色（不做行内识别——v1 无语言词法器，真语言
//     tokenize 是 ux-design §8 #8 的「P3 或不做」项）→ 预览逐行 code 色 run，
//     空行无 run，未覆盖段兜底 style.codeText 基色（防御，正常不触发）
//   - 未来接语言词法器只换本文件的 run 产出器，自绘/缓存/接线零改动
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
    bool ok = false;                  // false = 块源码无法解析为恰好一个围栏块
                                      //（防御性：块模型保证 CodeFence 块单围栏，
                                      // 真触发说明上游分块异常，调用方回退
                                      // components::markdown 渲染该块）
    std::string displayText;          // 预览渲染文本（含 lang 首行，若有）
    std::vector<eui::TextRun> runs;   // displayText 字节坐标，升序不重叠
    std::string lang;                 // 围栏 info 首词（md4c MD_LANG 语义；日志/测试用）
};

// blockSource = MdBlock(kind==CodeFence) 的块源码（开栏行 + 内容 + 闭栏行 +
// 可能的块尾空行）。colors = mdHighlightColors(theme tokens)（与编辑器同表）。
MdPreviewCodeHighlight mdPreviewCodeHighlight(std::string_view blockSource,
                                              const MdHighlightColors& colors);

// 脏块高亮缓存：按 (stableId, 块内容 hash) 双键命中——块不变 → 复用零重算；
// 编辑改动块（hash 变）→ 重算一次；块删除（stableId 消失）→ pruneActive 回收。
// 主题切换（色表变）→ setColors 整表作废（罕见，重算便宜）。纯状态，单 UI 线程，
// 不抛异常。
class MdPreviewCodeCache {
public:
    // 每帧注入当前色表；色表变化时清空全部条目（下一次 get 全部 miss 重算）
    void setColors(const MdHighlightColors& colors);

    // 命中返回条目（引用归缓存所有，到下一次 put/setColors/prune 前有效）；
    // 未命中返回 nullptr 并计 miss
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
