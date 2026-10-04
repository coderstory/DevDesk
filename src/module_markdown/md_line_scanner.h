// Markdown 行分类扫描器 —— 纯函数、无副作用、可单测：标题行扫描、行号 ↔ 字节
// 偏移换算、UTF-8 码点计数、围栏块扫描。
// 标题识别用行扫描 ^#{1,6}\s，不依赖 md4c（md4c Block 无源码 offset）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace module::markdown {

// 一条 ATX 标题。byteOffset 为该行行首字节偏移（可作光标跳转目标 / 前缀切点）。
struct MdHeading {
    int level = 1;        // 1-6
    int lineNo = 0;       // 0 起
    size_t byteOffset = 0;
    std::string title;    // 去掉 # 前缀和首尾空白后的标题文本
};

// 一个围栏代码块。{openLineStart, contentEnd} 即围栏行行首，高亮器据此区分围栏/内容行。
struct MdFenceBlock {
    size_t openLineStart = 0;   // 开栏行（如 ```cpp）行首
    size_t contentStart = 0;    // 首个内容行行首（开栏行换行之后）
    size_t contentEnd = 0;      // 闭栏行行首；未闭合围栏 = text.size()
};

// 扫描围栏代码块区间，语义对齐 md4c/CommonMark（缩进 ≤3 空格、≥3 同字符、闭栏
// 长度 ≥ 开栏且行尾仅空白）。与预览分块 / 大纲 / 高亮共用这套状态机，等价性回归
// 锚 = md_block_model_render_equivalence。
std::vector<MdFenceBlock> scanFences(const std::string& text);

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
