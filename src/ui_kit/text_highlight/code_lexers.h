// 代码词法扫描器（styled-runs 同模型，need:10 批次三）：java/js/ts/sql/json
//   五类，行级 span 输出（kind 不带色，视图按当前主题映射——主题切换零重算）。
// 架构与 module_markdown/md_keyword_highlight 的 LineLexer 同源：单一手写状态机
//   + per-language 关键字表，跨行 token（块注释/反引号模板串/Java 文本块）用 mode
//   在行间传递。容错同源：识别不确定（未闭合串/正则字面量）= 不产 span，漏标回
//   默认色，绝不半途吞掉后文错标。纯 UI 层，不依赖 git。
#pragma once

#include <core/theme.h>

#include <eui/types.h>

#include <string_view>
#include <vector>

namespace ui_kit::text_highlight {

enum class CodeLang { Java, JavaScript, TypeScript, Sql, Json };

// token 类别（UI-A §3.2 --hl-keyword/string/comment/number 四组）
enum class HlKind { Keyword, String, Comment, Number };

struct HlSpan {
    int start;   // 行内字节偏移（0 起，行内容不含行尾符）
    int end;
    HlKind kind;
};

// UI-A §3.2 --hl-* 主题映射（浅/深随 Tokens 双套；与 mdHighlightColors 同法从
// 语义槽位派生：keyword=info 蓝系、string=primaryText 绿系、comment=textTertiary
// 灰、number=warning 琥珀）
struct CodeTokenColors {
    eui::Color keyword;
    eui::Color string_;
    eui::Color comment;
    eui::Color number;
};
CodeTokenColors codeTokenColors(const core::theme::Tokens& t);
eui::Color colorOf(HlKind kind, const CodeTokenColors& c);

// 文件名扩展名 → 语言（.java/.js/.mjs/.cjs/.jsx/.ts/.tsx/.sql/.json；大小写不
// 敏感）。未识别返回 false。
bool codeLangFromExtension(std::string_view fileName, CodeLang& out);

// 逐行扫描器（跨行状态：块注释 / JS·TS 反引号模板串 / Java 文本块 """）。
// 用法：同一文件的行按序逐行调用（行内容不含行尾 \n\r）；换文件先 reset()。
// 返回行内偏移升序不重叠的 span；漏标段 = 无 span（默认色兜底）。非线程安全。
class CodeHighlighter {
public:
    // 跨行 token 状态（LineLexer 同模型）：Normal / 块注释 / 反引号模板串 / Java 文本块
    enum class Mode { Normal, BlockComment, Backtick, TripleQuote };

    void reset() { mode_ = Mode::Normal; }
    void setLang(CodeLang lang) { lang_ = lang; reset(); }

    std::vector<HlSpan> spansForLine(std::string_view line);

private:
    CodeLang lang_ = CodeLang::JavaScript;
    Mode mode_ = Mode::Normal;
};

} // namespace ui_kit::text_highlight
