// src/module_notepad/notepad_language.cpp（接口语义见头文件）。
#include "notepad_language.h"

#include <core/notepad_file_types.h>
#include <core/theme.h>
#include <ui_kit/text_highlight/md_highlight.h>
#include <ui_kit/text_highlight/md_keyword_highlight.h>

#include <utility>

namespace module::notepad {
namespace {

// tech 卡 §五 映射表：唯一一份，加语言只加行。条目互不为后缀，顺序无关。
struct LangMapping {
    std::string_view ext;
    std::string_view lang;
};
constexpr LangMapping kLangMappings[] = {
    {".cpp", "cpp"}, {".cc", "cpp"}, {".cxx", "cpp"}, {".h", "cpp"}, {".hpp", "cpp"},
    {".py", "python"},
    {".js", "js"},
    {".ts", "js"},  // 收敛：LangSpec 无 TypeScript 词法表，暂用 Js（待办 need263）
    {".go", "go"},
    {".rs", "rust"},
    {".sh", "shell"}, {".bash", "shell"},
    {".java", "java"},
    {".json", "json"},
    {".sql", "sql"},  // need375：SQL 高亮（词法在 ui_kit md_keyword_highlight）
    // .md/.markdown 不在表内 → None：LangSpec 无 Markdown 词法（待办，见头文件注记）
};

}  // namespace

std::string_view languageForExtension(std::string_view path) {
    for (const LangMapping& m : kLangMappings) {
        if (path.size() < m.ext.size()) continue;
        const size_t off = path.size() - m.ext.size();
        bool hit = true;
        for (size_t i = 0; i < m.ext.size(); ++i) {
            if (core::notepad::notepadAsciiLower(path[off + i]) !=
                core::notepad::notepadAsciiLower(m.ext[i])) {
                hit = false;
                break;
            }
        }
        if (hit) return m.lang;
    }
    return {};
}

std::vector<eui::TextRun> highlightLine(const std::string& displayText,
                                        size_t lineStartByte, int lineNo,
                                        std::string_view lang) {
    (void)lineNo;  // 契约位：跨行 token 的行间 mode 传递后续批次再接
    std::vector<eui::TextRun> runs;
    if (lang.empty()) return runs;  // None 纯文本：零 runs
    const auto kw = ui_kit::text_highlight::mdKeywordHighlight(
        displayText, lang,
        module::markdown::mdHighlightColors(core::theme::current()));
    if (!kw.supported) return runs;
    runs = std::move(kw.runs);
    const auto base = static_cast<int>(lineStartByte);
    for (eui::TextRun& r : runs) {
        r.byteStart += base;
        r.byteEnd += base;
    }
    return runs;
}

}  // namespace module::notepad
