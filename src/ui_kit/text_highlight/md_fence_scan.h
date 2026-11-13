// Markdown 围栏块扫描（CommonMark/md4c 口径）——原 module_markdown/md_line_scanner
// 的围栏状态机（need:10 批次三随 md_highlight 平移至 ui_kit，平移不复制）。
// 消费方：text_highlight/md_highlight（高亮）+ module_markdown（大纲/预览分块，
// 经 md_line_scanner.h 透出，等价性回归锚 = md_block_model_render_equivalence）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace module::markdown {

// 一个围栏代码块。{openLineStart, contentEnd} 即围栏行行首，高亮器据此区分围栏/内容行。
struct MdFenceBlock {
    size_t openLineStart = 0;   // 开栏行（如 ```cpp）行首
    size_t contentStart = 0;    // 首个内容行行首（开栏行换行之后）
    size_t contentEnd = 0;      // 闭栏行行首；未闭合围栏 = text.size()
};

// 扫描围栏代码块区间，语义对齐 md4c/CommonMark（缩进 ≤3 空格、≥3 同字符、闭栏
// 长度 ≥ 开栏且行尾仅空白）。与预览分块 / 大纲 / 高亮共用这套状态机。
std::vector<MdFenceBlock> scanFences(const std::string& text);

} // namespace module::markdown
