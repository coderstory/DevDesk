// src/module_markdown/md_keyboard.h
// 编辑器键盘流纯函数（spec §5.2 Phase 2，通道 A：input.onKeyEvent passthrough）。
// 输入/输出都是 EditBuffer（文本 + 光标/选区字节偏移，语义同 InputState），
// 不依赖 EUI-NEO —— markdown_module.cpp 的 onKeyEvent 只做接线：
//   EditBuffer buf{st.text, st.cursor, st.selectionStart, st.selectionEnd};
//   ... 纯函数变换 ...
//   写回 InputState（含 pushUndoState / textRevision / onChange 副作用）
#pragma once

#include <string>

namespace module::markdown {

// 编辑缓冲快照（字节偏移，UTF-8 边界）
struct EditBuffer {
    std::string text;
    int cursor = 0;
    int selStart = 0;
    int selEnd = 0;
};

// 行的列表前缀（含行首缩进）：
//   "- a" → "- "   "* a" → "* "   "1. a" → "1. "   "  2. a" → "  2. "
// 非列表行（含 "-a" 无空格、"# 标题"、空行）返回 ""。
std::string listPrefixOf(const std::string& line);

// 前缀续行版：有序列表递增（"1. " → "2. "，缩进保留），无序列表原样返回。
std::string nextListPrefix(const std::string& prefix);

// Enter 列表续行（光标所在行是列表项时）：
//   非空项 → 插入 "\n" + 续行前缀；空列表项 → 删除前缀退出列表（不换行）。
// 返回 true = 已处理；false = 普通行/有选区，交回 input 内置 Enter。
bool applyEnterList(EditBuffer& buf);

// Tab / Shift+Tab：无选区 Tab = 插入两空格（同打字）；
// 有选区（或 Shift）= 光标/选区覆盖的所有行整体缩进 / 反缩进两空格。
void applyTab(EditBuffer& buf, bool shift);

// Ctrl+B / Ctrl+I：选区包裹 marker；无选区插入成对标记、光标放中间。
void applyWrapAround(EditBuffer& buf, const std::string& marker);

// Ctrl+K：有选区 → [选区](链接) 并选中「链接」占位；
// 无选区 → [文本](链接) 并选中「文本」占位（直接输入即替换）。
void applyLinkSkeleton(EditBuffer& buf);

} // namespace module::markdown
