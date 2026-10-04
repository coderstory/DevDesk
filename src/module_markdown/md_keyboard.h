// src/module_markdown/md_keyboard.h
// 编辑器键盘流纯函数（spec §5.2 Phase 2）：输入/输出都是 EditBuffer（文本 + 光标/选区
//   字节偏移，语义同 InputState），不依赖 EUI-NEO；接线在 markdown_module.cpp 的
//   onKeyEvent（构造 buf → 纯函数变换 → 写回 InputState，含 pushUndoState 等副作用）。
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

// 行的列表前缀（含行首缩进）："- a" → "- "，"1. a" → "1. " 等；非列表行
//（另有 "-a" 无空格、"# 标题"、空行）返回 ""。
std::string listPrefixOf(const std::string& line);

// 前缀续行版：有序列表递增（"1. " → "2. "，缩进保留），无序列表原样返回。
std::string nextListPrefix(const std::string& prefix);

// Enter 列表续行（光标行是列表项时）：非空项 → 插入 "\n" + 续行前缀；空列表项 →
// 删除前缀退出列表（不换行）。true = 已处理；false = 交回 input 内置 Enter。
bool applyEnterList(EditBuffer& buf);

// Tab / Shift+Tab：无选区 Tab = 插入 4 空格；有选区（或 Shift）= 覆盖的所有行整体
// 缩进 4 空格 / 反缩进（每行去掉 1 个 Tab 或至多 4 个前导空格）。
void applyTab(EditBuffer& buf, bool shift);

// Ctrl+B / Ctrl+I：选区包裹 marker；再按 = 去包裹（选区两端恰为 marker 时
// 移除，unwrap toggle，spec §5.2）；无选区插入成对标记、光标放中间。
void applyWrapAround(EditBuffer& buf, const std::string& marker);

// Ctrl+K：有选区 → [选区](链接) 并选中「链接」占位；
// 无选区 → [文本](链接) 并选中「文本」占位（直接输入即替换）。
void applyLinkSkeleton(EditBuffer& buf);

// 表格 MVP：插入表格 + Tab 跳格（ux-design §5.5）

// 生成 rows×cols 的 Markdown 表格模板（含表头：空表头行 + |---| 分隔行 + 空数据行；
// rows==1 → 表头 + 分隔行，GFM 合法）。rows/cols 下限钳到 1，返回不含结尾换行。
std::string buildMarkdownTable(int rows, int cols);

// 光标处插入表格：光标行为空白 → 整行替换；非空 → 行尾插入（前置空行隔开段落）。
// 光标落进第一个表头单元格、无选区。MVP 边界：紧贴表尾时不为后续段落补空行。
void applyInsertTable(EditBuffer& buf, int rows, int cols);

// Tab/Shift+Tab 表格跳格：光标所在行是表格行（trim 后首尾皆 '|' 且 ≥2 竖线）时接管
// ——光标格 = 最后一根严格在其左侧的竖线的右邻格；Tab 进本行下一格，行尾/表头分隔行
// → 块内下一个非分隔行首格（越块回绕），Shift+Tab 镜像。true = 已处理，false = 交回 applyTab。
bool applyTabTable(EditBuffer& buf, bool shift);

} // namespace module::markdown
