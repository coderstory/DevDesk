// notepad_encoding.h — 编码 / 行尾纯函数探测层（need:273 批 1，无 UI 依赖可无头直测）。
// 口径 = PRD F-15（D-8 选项 1：v1 仅 UTF-8）：
//   BOM 三型（UTF-8 / UTF-16LE / UTF-16BE）→ 严格 UTF-8 校验（照抄
//   module_git git_content_model 算法口径，模块隔离重写）→ 失败则推测编码标签
//   （GB18030 双字节启发 / Latin-1 兜底，界面标「推测」）→ 只读降级。
//   二进制探测 = 首 8KiB 含 NUL（git 同口径，F-14 拒编分支）。
//   行尾四态 = CRLF / LF / CR / 混合；缓冲统一 LF 存储，保存按档回写。
#pragma once

#include <cstddef>
#include <string>

namespace module::notepad {

enum class NotepadBom { None, Utf8, Utf16Le, Utf16Be };
enum class NotepadEol { Lf, Crlf, Cr, Mixed };

struct NotepadEncodingProbe {
    NotepadBom bom = NotepadBom::None;
    bool validUtf8 = true;             // BOM 剥离后严格 UTF-8 校验结果
    bool binary = false;               // 首 8KiB 含 NUL（BOM 剥离前判定）
    const char* guessLabel = nullptr;  // 非 UTF-8 时非空："GB18030（推测）" / "Latin-1（推测）"
};

// 全流程探测：BOM → 二进制 → 严格 UTF-8 → 推测标签。UTF-16 BOM 置 validUtf8=false
//（无编码转换层，只读降级 + 精确标注 LE/BE）。
NotepadEncodingProbe notepadProbeEncoding(const std::string& bytes);

// 严格 UTF-8 校验：拒绝代理区 / 超 U+10FFFF / 过长编码 / 悬挂 continuation。
bool notepadIsValidUtf8(const std::string& s);

// 行尾四态探测（LF 域归一前调用；空文本 = Lf）。
NotepadEol notepadDetectEol(const std::string& text);

// CRLF / 孤 CR → LF 归一；mixedOut 非空时回填「归一前是否混合行尾」。
std::string notepadNormalizeToLf(const std::string& text, bool* mixedOut = nullptr);

// LF 文本按档回写行尾（脏缓冲保存重编码用；Mixed 传入时按 CRLF 处理——调用方
// 应先给出明确档位，混合档不落盘直通）。
std::string notepadApplyEol(const std::string& textLf, NotepadEol eol);

// UTF-8 BOM 剥离（None / Utf16 原样返回；只处理 kUtf8Bom 三字节）。
std::string notepadStripBom(const std::string& bytes);

// UTF-8 BOM 三个头部字节（EF BB BF）。
extern const char kUtf8Bom[3];

// ── 文本度量（UTF-8 码点域；module_markdown md_line_scanner 同算法重写——
//   模块隔离不能 include，三小函数按算法照抄）─────────────────────────

// 字节偏移 → 0 起行号（offset 允许 == text.size()）。
int notepadByteOffsetToLine(const std::string& text, size_t offset);
// 0 起行号 → 该行行首字节偏移（lineNo 越界时钳到末行行首=末尾）。
size_t notepadLineStartOffset(const std::string& text, int lineNo);
// UTF-8 码点数（跳过 continuation byte 0b10xxxxxx）。
size_t notepadUtf8Length(const std::string& text);

}  // namespace module::notepad
