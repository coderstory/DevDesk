// Markdown 行分类扫描器 —— 纯函数、无副作用、可单测：标题行扫描、行号 ↔ 字节
// 偏移换算、UTF-8 码点计数、围栏块扫描。
// 标题识别用行扫描 ^#{1,6}\s，不依赖 md4c（md4c Block 无源码 offset）。
#pragma once

#include <ui_kit/text_highlight/md_fence_scan.h>

#include <cstddef>
#include <string>
#include <vector>

namespace module::markdown {

// MdFenceBlock / scanFences 已随 md_highlight 平移至 ui_kit/text_highlight
//（need:10 批次三，平移不复制）；经本头透出维持 module_markdown 消费面不变。

// 一条 ATX 标题。byteOffset 为该行行首字节偏移（可作光标跳转目标 / 前缀切点）。
struct MdHeading {
    int level = 1;        // 1-6
    int lineNo = 0;       // 0 起
    size_t byteOffset = 0;
    std::string title;    // 去掉 # 前缀和首尾空白后的标题文本
};

// 扫描全文标题。围栏块内的 # 行不算标题（与 scanFences 同源）。
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
