// src/core/str_util.h
// 跨模块共用的纯字符串工具（无平台依赖、无 EUI-NEO 依赖、不抛异常）。
//
// ── 为什么在 core ──────────────────────────────────────────────────────
// 架构铁律「module_* 之间不可互相依赖」的副作用是：每个模块遇到同一件小事
// （转义 SQL LIKE 模式、去首尾空白）都各抄一份。散成 N 份的代价不是行数，而是
// **drift**——一处改了另一处不改，行为悄悄分叉，且没人会发现。这类纯函数与业务
// 无关，下沉 core 是唯一正解（core 是各模块共同依赖的最底层，不反向依赖谁）。
//
// 边界：只放**逐字可共用**的纯函数。实现有语义差异的不合并——比如
// `toLowerAscii` 有 ASCII-only 版（`c >= 'A' && c <= 'Z'`）与 `std::tolower`
// locale 版两种写法，在非 ASCII 字节上行为不同，那是两个函数不是一个，合并会
// 造成静默行为变更。本文件只收已核实逐字等价的两个。
#pragma once

#include <cstddef>
#include <string>

namespace core::str {

// SQL LIKE 模式转义：把用户输入里的 \ / % / _ 转成字面量。
// 调用方口径：拼 `"%" + escapeLikePattern(q) + "%"`，并在 SQL 里配 `ESCAPE '\'`。
// 2026-10-03 合并自 module_markdown/markdown_service.cpp:82、
// module_claude_proxy/call_log_store.cpp:21、module_todo/todo_service.cpp:32
// ——三份逐字相同（只差参数名），合并前已逐行比对。
inline std::string escapeLikePattern(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\\' || ch == '%' || ch == '_') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

// 去首尾 ASCII 空白（空格 / 制表 / CR / LF）。
// 2026-10-03 合并自 core/paths.cpp:73 与 module_markdown/markdown_service.cpp:1092
// ——两份写法略异（一用谓词 lambda、一用内联条件）但语义相同，已核实。
inline std::string trimAscii(const std::string& s) {
    const auto isBlank = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isBlank(s[b])) ++b;
    while (e > b && isBlank(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace core::str
