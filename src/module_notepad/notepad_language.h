// src/module_notepad/notepad_language.h
// 记事本语言层：扩展名→高亮语言映射（单一真源）+ per-line 高亮 provider。
// need263 批 2（池 #285）。词法一律走 ui_kit::text_highlight LangSpec 单入口
//（md_keyword_highlight，加语言=加映射表一行，不新造词法器）；code_lexers（HlSpan
// 模型）不接、不做模型转换（tech 卡 §五「避免双源」）。
#pragma once

#include <eui/types.h>

#include <string>
#include <string_view>
#include <vector>

namespace module::notepad {

// 扩展名→LangSpec 语言名（"cpp"/"python"/"js"/"go"/"rust"/"shell"/"java"/"json"），
// 直接喂 mdKeywordHighlight 的 lang 参数；大小写不敏感（core::notepad notepadAsciiLower
// 同源折叠）。tech 卡 §五 清单外 / 无扩展名 → 空串 = None 纯文本（F-14/N-9）。
// 收敛注记（LangSpec 现表 8 语言，以实查为准）：.ts 暂用 Js 词法（无 TS 专属表）；
// .md/.markdown → None（LangSpec 无 Markdown 词法；md_highlight 的 MdHighlighter 是
// 需 fullText 的有状态围栏模型，per-line 契约接不了）——两处待办记 need263 终报。
std::string_view languageForExtension(std::string_view path);

// per-line 高亮 provider 核心：tech 卡 §四 lineStyles 契约形状 + lang 显式入参
//（无状态三参形态表达不了「多窗各持不同语言」，window 层接线以 lambda 绑
// session.language 构成三参 provider，本函数保持纯函数可直测）。颜色源 =
// core::theme::current() 派生（mdHighlightColors 同源，随主题明暗切换）。
// 返回 runs 已平移为全文档字节坐标（byteStart = lineStartByte + 行内偏移）；
// None / LangSpec 未覆盖语言 → 空 vector。大文件（session.largeFile）由调用方关
// provider，本函数不判大小。
std::vector<eui::TextRun> highlightLine(const std::string& displayText,
                                        size_t lineStartByte,
                                        int lineNo,
                                        std::string_view lang);

}  // namespace module::notepad
