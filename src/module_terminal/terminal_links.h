// src/module_terminal/terminal_links.h
// M2 Task 7：终端行内链接识别（URL / 绝对路径）。纯函数，可 doctest 直测。
// 列口径与 terminal_text_util 一致（ASCII 1 列、CJK/emoji 2 列）；目标为原文字节串。
#pragma once

#include <string>
#include <vector>

namespace terminal {

enum class LinkKind { Url, Path };

struct LinkSpan {
    int colBegin = 0;      // 起始列（含）
    int colEnd = 0;        // 结束列（不含）
    std::string target;    // 打开目标（URL 原样 / 路径原样，不做解码/剥引号）
    LinkKind kind = LinkKind::Url;
};

// 扫描一行文本中所有链接（升序、互不重叠）。识别规则：
//  - URL：http:// 或 https:// 开头（大小写不敏感）；遇空白/引号/< > | ` 截止；
//    尾部连续剥掉 .,;:!?)]} 与中文标点 ，。；：！？）】
//  - 路径：'/' 开头（'//' 与 '/*' 除外）、'~/' 开头、盘符 'X:\' 或 'X:/' 开头；
//    同上截止与剥尾；实体长度 > 1 且含路径分隔符（'/' 或 '\'）
//  - 路径候选紧贴在 ASCII 词字符后不识别（'a/b' 这类相对路径不算链接）
std::vector<LinkSpan> findLinks(const std::string& lineText);

// 该列是否落在某个链接内（无则 nullptr）；区间为 [colBegin, colEnd)
const LinkSpan* linkAtColumn(const std::vector<LinkSpan>& links, int col);

} // namespace terminal
