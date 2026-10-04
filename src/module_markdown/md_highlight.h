// src/module_markdown/md_highlight.h
// Markdown 行级高亮器（styled-runs Phase C）：自研行级词法扫描器，围栏状态与大纲共用
//   一套状态机语义。
// 框架契约：回调仅对可视行、且行文本/styleRevision 变化时调用（行缓存由 EUI-NEO input
//   负责，本类不自建）；返回 runs 用【全文档坐标】(byteStart = lineStartByte + 行内偏移)；
//   颜色 run 不改 layout/caret，对光标/选区/IME 覆盖层零影响。
#pragma once

#include "md_line_scanner.h"

#include <eui/types.h>

#include <string>
#include <vector>

namespace core::theme {
struct Tokens;  // mdHighlightColors 参数（见 md_highlight.cpp）
}

namespace module::markdown {

// ux-design §6.2 色表 → core::theme 语义色 token（明暗两套由调用方注入；斜体无 italic
// 字形用 info 色替代；粗体在 Phase D weight run 落地前保持正文色，仅 ** 标记弱化）。
struct MdHighlightColors {
    eui::Color marker;      // 标记符号：# 前缀 / ** / ` / > / | / []()   ← textTertiary
    eui::Color code;        // 行内代码 + 围栏内容（含关键字着色的 default 段）← success
    eui::Color link;        // 链接文字 / 斜体内容（同 info，§6.2）      ← info
    eui::Color listMarker;  // 列表符号 - * + N.                         ← primary
    eui::Color quote;       // 引用行正文                                ← textSecondary
    // 预览代码块关键字多色（Phase 3 ⑤，md_keyword_highlight 消费；编辑器行级高亮器
    // 不读这四字段，仍整行 code 单色）。未覆盖语言回退逐行 code 单色，与编辑器 token 等价。
    eui::Color codeKeyword; // C++/Python 关键字 + C++ 预处理指令        ← info
    eui::Color codeString;  // 字符串 / 字符字面量 / 三引号串             ← warning
    eui::Color codeComment; // 行注释 / 块注释                            ← textTertiary
    eui::Color codeNumber;  // 整数字面量（含十六进制）/ 浮点             ← primaryText
};

// 色表映射（token 已齐）
MdHighlightColors mdHighlightColors(const core::theme::Tokens& tokens);

// 高亮器实例挂在模块成员（非线程安全，单 UI 线程使用）。围栏状态按 displayText 内部
// 重建：文本变化后首次回调做一次 O(n) 扫描，其后同文本的可视行回调走快路径。
class MdHighlighter {
public:
    void setColors(const MdHighlightColors& colors) { colors_ = colors; }

    // lineStartByte = 行首在 fullText 的字节偏移；fullText 含 IME 组合替换后的内容。
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
