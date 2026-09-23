// src/module_markdown/md_highlight.h
// Markdown 行级高亮器（styled-runs Phase C，设计 2026-09-23-eui-styled-runs-design.md §6
// 方案 A：自研行级词法扫描器，基于 md_line_scanner 扩展——围栏状态与大纲共用一套
// 状态机语义，一鱼三吃）。
//
// 语义约定（Phase B 框架契约）：
//   - 回调按需调用：仅对【可视行】、行文本或 styleRevision 变化时一次；
//     行级命中/失效由 EUI-NEO input 的 (lineNo, textRevision, styleRevision)
//     三元组行缓存负责，本类不自建正确性缓存
//   - 返回 runs 用【全文档坐标】（byteStart = lineStartByte + 行内偏移），
//     框架负责裁剪到行内、平移、归一化
//   - 颜色 run 不改 layout/caret，光标/选区/IME 覆盖层零影响
#pragma once

#include "md_line_scanner.h"

#include <eui/types.h>

#include <string>
#include <vector>

namespace core::theme {
struct Tokens;  // mdHighlightColors 参数（见 md_highlight.cpp）
}

namespace module::markdown {

// ux-design §6.2 色表 → core::theme 语义色 token（明暗两套由调用方注入；
// 斜体无 italic 字形用 info 色替代；粗体内容在 Phase D weight run 落地前
// 保持正文色——只有 ** 标记弱化）
struct MdHighlightColors {
    eui::Color marker;      // 标记符号：# 前缀 / ** / ` / > / | / []()   ← textTertiary
    eui::Color code;        // 行内代码 + 围栏内容                       ← success
    eui::Color link;        // 链接文字 / 斜体内容（同 info，§6.2）      ← info
    eui::Color listMarker;  // 列表符号 - * + N.                         ← primary
    eui::Color quote;       // 引用行正文                                ← textSecondary
};

// 色表映射（token 已齐，无需新增语义色）
MdHighlightColors mdHighlightColors(const core::theme::Tokens& tokens);

// 高亮器实例挂在模块成员（非线程安全，单 UI 线程使用）。围栏状态由内部按
// displayText 重建：文本变化后的首次回调做一次 O(n) 扫描，其后同文本的
// 可视行回调走快路径（等值比较）。
class MdHighlighter {
public:
    void setColors(const MdHighlightColors& colors) { colors_ = colors; }

    // fullText    = 当前 display 全文（含 IME 组合替换后的内容，与框架回调
    //               传给高亮器的 lineStartByte 同坐标系）
    // lineText    = 该可视行文本（软换行时是段文本，非整段）
    // lineStartByte = 行首在 fullText 的字节偏移
    // 返回全文档坐标 runs；空 vector = 该行默认色。
    std::vector<eui::TextRun> runsForLine(const std::string& fullText,
                                          const std::string& lineText,
                                          int lineStartByte);

private:
    void ensureFences(const std::string& fullText);

    MdHighlightColors colors_;
    std::string fenceText_;              // 围栏区间对应的文本快照
    std::vector<MdFenceBlock> fences_;   // scanFences 结果（升序、互不重叠）
};

} // namespace module::markdown
