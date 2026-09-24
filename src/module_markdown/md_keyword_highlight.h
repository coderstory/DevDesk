// src/module_markdown/md_keyword_highlight.h
// 代码关键字多色词法器（Phase 3 ⑤，ux-design §5.1「真语言 tokenize」兑现）：
// 围栏内容源码 → 按语言的着色 runs（关键字/字符串/注释/数字/默认 五色）。
//
// 定位：纯函数、零第三方依赖、可单测。消费方 = md_preview_highlight 的
// CodeFence 产 runs 路径（预览侧多色）；编辑器侧仍整行单 code 色
// （MdHighlighter 不接本文件——编辑器多色是另批对称工程）。
//
// 架构（可扩展 = 加一个语言只加一张词法配置表）：
//   - 每语言一份 LangSpec：关键字表 + 注释符 + 字符串引号集 + 字符串前缀
//     （Python r/b/f/u）+ 块注释/三引号跨行支持开关 + 预处理行标记
//   - 单一手写状态机按 spec 扫描，无 per-language 代码分支（预处理 '#'、
//     前缀字符串这类小差异走 spec 字段）
//   - 新语言（JS/Go/Rust/Shell…）= 填一张 LangSpec + 别名表加词
//
// 语义约定（与 md_highlight「漏标回默认色，优于错标」同哲学）：
//   - 识别不确定（未闭合单行字符串、孤立前缀等边角）→ 该段按默认 code 色
//     输出，绝不半途吞掉后文错标
//   - 未覆盖语言 / 空 lang → supported=false、runs 空，调用方保持现状
//    （逐行整行 code 色，与编辑器 token 等价）
//   - runs 坐标系 = 传入 code 的字节偏移；升序、互不重叠
//   - 空白（空格/Tab/换行）恒为 run 间隙（default run 两端裁剪空白），
//     与逐行 fallback 的「行 run 不含换行、空行无 run」结构一致；跨行
//     token（块注释 / Python 三引号串）按行切成同色相邻 run，换行仍为间隙
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

// code = 围栏内容（行以 \n 连接、已剔 \r 的显示串段）；colors = 与编辑器
// 同源的 MdHighlightColors（本函数只用 codeKeyword/codeString/codeComment/
// codeNumber/code 五个字段）。纯函数：无状态、不抛异常。
MdKeywordHighlight mdKeywordHighlight(std::string_view code,
                                      std::string_view lang,
                                      const MdHighlightColors& colors);

} // namespace module::markdown
