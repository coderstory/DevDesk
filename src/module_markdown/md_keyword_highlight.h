// src/module_markdown/md_keyword_highlight.h
// 代码关键字多色词法器：围栏源码 → 按语言着色的 runs（关键字/字符串/注释/
//   数字/默认 五色）。纯函数、零依赖、可单测；供 md_preview_highlight 的 CodeFence 路径，
//   编辑器侧仍整行单 code 色。
// 架构：每语言一份 LangSpec（关键字/注释符/引号集/字符串前缀/跨行开关/预处理标记），单一
//   手写状态机按该 LangSpec 扫描，无 per-language 分支 —— 加语言只加一张表。
// 语义：识别不确定（未闭合字符串、孤立前缀）→ 该段按默认 code 色，绝不半途吞掉后文错标；
//   未覆盖语言/空 lang → supported=false、runs 空；runs 用 code 偏移、升序不重叠，空白恒为 run 间隙。
#pragma once

#include "md_highlight.h"

#include <eui/types.h>

#include <string_view>
#include <vector>

namespace module::markdown {

struct MdKeywordHighlight {
    bool supported = false;           // lang 无词法器 → false（调用方回退单色）
    std::vector<eui::TextRun> runs;   // code 字节坐标，升序不重叠
};

// lang 是否有词法器（大小写不敏感 + 别名：cpp/c++/cc/cxx/hpp/hh → C++；
// python/py/python3 → Python）
bool mdKeywordLangSupported(std::string_view lang);

// code = 围栏内容（行以 \n 连接、已剔 \r 的显示串段）；colors = 与编辑器同源的
// MdHighlightColors（只用 codeKeyword/codeString/codeComment/codeNumber/code）。纯函数、不抛异常。
MdKeywordHighlight mdKeywordHighlight(std::string_view code,
                                      std::string_view lang,
                                      const MdHighlightColors& colors);

} // namespace module::markdown
