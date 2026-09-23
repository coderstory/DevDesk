// src/module_markdown/md_line_scanner.h
// Markdown 行分类扫描器（Phase 1 底座）——纯函数、无副作用、可单测：
//   - 标题行扫描（大纲面板 + 同步滚动的锚点）
//   - 行号 ↔ 字节偏移换算（状态栏 行:列、大纲跳转写 InputState.cursor）
//   - UTF-8 码点计数（状态栏「N 字」按字符数而非字节数）
//
// 设计依据 docs/superpowers/specs/2026-09-22-markdown-editor-ux-design.md §5.4：
// 标题识别用行扫描 ^#{1,6}\s，不依赖 md4c（md4c Block 无源码 offset）。
// Phase 2 的语法高亮 / Phase 3 的分块器可在本文件上扩展（一鱼三吃）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace module::markdown {

// 一条 ATX 标题（# 开头）。lineNo 为 0 起行号，byteOffset 为该行行首在全文的
// 字节偏移（可直接用作 InputState.cursor 跳转目标 / estimateHeight 前缀切点）。
struct MdHeading {
    int level = 1;        // 1-6
    int lineNo = 0;       // 0 起
    size_t byteOffset = 0;
    std::string title;    // 去掉 # 前缀和首尾空白后的标题文本
};

// 一个围栏代码块（``` / ~~~）。行首字节偏移集合 {openLineStart, contentEnd}
// 即围栏行（开/闭栏行）行首——高亮器据此区分围栏行与内容行（一鱼三吃）。
struct MdFenceBlock {
    size_t openLineStart = 0;   // 开栏行（如 ```cpp）行首
    size_t contentStart = 0;    // 首个内容行行首（开栏行换行之后）
    size_t contentEnd = 0;      // 闭栏行行首；未闭合围栏 = text.size()
};

// 扫描围栏代码块区间。近似（ponytail）同 scanHeadings：围栏不校验缩进与
// 信息串语法，``` 与 ~~~ 各自按字符配对开关。区间按出现顺序升序、互不重叠。
std::vector<MdFenceBlock> scanFences(const std::string& text);

// 扫描全文标题。围栏代码块（``` / ~~~）内的 # 行不算标题（围栏语义与
// scanFences 同源，两处共用一套状态机语义）。
// 近似（ponytail）：同 scanFences。
std::vector<MdHeading> scanHeadings(const std::string& text);

// 总行数（0 行文本算 1 行；行尾无换行的最后一行也计一行）
int countLines(const std::string& text);

// 字节偏移 → 0 起行号（offset 允许 == text.size()）
int byteOffsetToLine(const std::string& text, size_t offset);

// 0 起行号 → 该行行首字节偏移（lineNo 越界时钳到首/末行）
size_t lineStartOffset(const std::string& text, int lineNo);

// UTF-8 码点数（跳过 continuation byte 0b10xxxxxx）
size_t utf8Length(const std::string& text);

} // namespace module::markdown
