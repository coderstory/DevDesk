// tests/test_modules/test_notepad_encoding.cpp
// need:273 批 1 —— 编码/行尾纯函数无头直测（断言面 = 技术方案卡 §十一
// test_notepad_encoding.cpp 行）：BOM 三型与剥离 / 严格 UTF-8 边界 / 推测标签 /
// 二进制扫描窗 / EOL 四态 / LF 归一与回写 / 干净直通 vs 脏重编码 / 文本度量。
#include "doctest.h"

#include "module_notepad/notepad_encoding.h"
#include "module_notepad/notepad_file_io.h"  // notepadEncodeBuffer（脏缓冲重编码）

#include <string>

namespace np = module::notepad;

// BOM 三型识别 + UTF-8 BOM 剥离（UTF-16 BOM 头直接短路返回，不再扫二进制——
// 即使正文含 NUL 也不判 binary）。
REGISTER_TEST(notepad_encoding_bom_three, "notepad_encoding: BOM 三型与剥离", {
    const np::NotepadEncodingProbe u8 =
        np::notepadProbeEncoding(std::string("\xEF\xBB\xBF") + "hello");
    DOCTEST_CHECK_EQ(u8.bom, np::NotepadBom::Utf8);
    DOCTEST_CHECK_TRUE(u8.validUtf8);
    DOCTEST_CHECK_FALSE(u8.binary);
    DOCTEST_CHECK_TRUE(u8.guessLabel == nullptr);

    const std::string be = std::string("\xFE\xFF\x00h\x00i");
    const np::NotepadEncodingProbe u16be = np::notepadProbeEncoding(be);
    DOCTEST_CHECK_EQ(u16be.bom, np::NotepadBom::Utf16Be);
    DOCTEST_CHECK_FALSE(u16be.validUtf8);
    DOCTEST_CHECK_FALSE(u16be.binary);  // UTF-16 优先：含 NUL 也不判二进制
    DOCTEST_CHECK_EQ(np::notepadStripBom(be), be);  // 只剥 UTF-8 BOM

    const std::string le = std::string("\xFF\xFEh\x00i\x00");
    const np::NotepadEncodingProbe u16le = np::notepadProbeEncoding(le);
    DOCTEST_CHECK_EQ(u16le.bom, np::NotepadBom::Utf16Le);
    DOCTEST_CHECK_FALSE(u16le.validUtf8);

    const std::string bare = "no bom";
    DOCTEST_CHECK_EQ(np::notepadProbeEncoding(bare).bom, np::NotepadBom::None);
    DOCTEST_CHECK_EQ(np::notepadStripBom(bare), bare);
    DOCTEST_CHECK_EQ(np::notepadStripBom(std::string("\xEF\xBB\xBF") + "body"),
                     std::string("body"));
    return true;
})

// 严格 UTF-8：合法 ASCII / CJK / 4 字节 emoji 全过。
REGISTER_TEST(notepad_encoding_utf8_valid, "notepad_encoding: 严格 UTF-8 合法面", {
    DOCTEST_CHECK_TRUE(np::notepadIsValidUtf8(""));
    DOCTEST_CHECK_TRUE(np::notepadIsValidUtf8("plain ascii"));
    DOCTEST_CHECK_TRUE(np::notepadIsValidUtf8("中文内容"));
    DOCTEST_CHECK_TRUE(np::notepadIsValidUtf8("\xF0\x9F\x98\x80"));  // U+1F600 emoji
    DOCTEST_CHECK_TRUE(np::notepadIsValidUtf8("mix中文\xF0\x9F\x9A\x80tail"));
    return true;
})

// 严格 UTF-8：截断尾序列 / 悬挂 continuation / 过长编码 / 代理区 / 超
// U+10FFFF / 5 字节首字节全拒。
REGISTER_TEST(notepad_encoding_utf8_reject, "notepad_encoding: 严格 UTF-8 拒绝面", {
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xE4\xBD"));          // 截断尾序列
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xE4\xBD\x2D"));      // 续字节缺位
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\x80"));              // 悬挂 continuation
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("abc\xBF"));           // 尾部悬挂
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xC0\x80"));          // 过长（1 字节档）
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xE0\x80\x80"));      // 过长（2 字节档）
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xED\xA0\x80"));      // 代理区 U+D800
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xF4\x90\x80\x80"));  // 超 U+10FFFF
    DOCTEST_CHECK_FALSE(np::notepadIsValidUtf8("\xF8\x80\x80\x80\x80"));  // 非法首字节
    return true;
})

// 推测标签：GB18030 双字节对占过半 → GB18030（推测）；零双字节非 ASCII →
// Latin-1（推测）；合法 UTF-8 无标签。
REGISTER_TEST(notepad_encoding_guess_labels, "notepad_encoding: 非 UTF-8 推测标签", {
    const np::NotepadEncodingProbe gb =
        np::notepadProbeEncoding(std::string("\xC4\xE3\xBA\xC3"));  // GBK「你好」
    DOCTEST_CHECK_FALSE(gb.validUtf8);
    DOCTEST_CHECK_TRUE(gb.guessLabel != nullptr);
    DOCTEST_CHECK_EQ(std::string(gb.guessLabel), std::string("GB18030（推测）"));

    const np::NotepadEncodingProbe latin =
        np::notepadProbeEncoding(std::string("abc\x80"));
    DOCTEST_CHECK_FALSE(latin.validUtf8);
    DOCTEST_CHECK_TRUE(latin.guessLabel != nullptr);
    DOCTEST_CHECK_EQ(std::string(latin.guessLabel), std::string("Latin-1（推测）"));

    DOCTEST_CHECK_TRUE(np::notepadProbeEncoding("utf8 ok").guessLabel == nullptr);
    return true;
})

// 二进制扫描窗：首 8KiB 含 NUL = 二进制；NUL 恰在 8KiB 之外 = 否。
REGISTER_TEST(notepad_encoding_binary_window, "notepad_encoding: 二进制扫描窗边界", {
    DOCTEST_CHECK_TRUE(np::notepadProbeEncoding(std::string("a\0b", 3)).binary);

    std::string outside(9000, 'x');
    outside[8192] = '\0';  // 首个 NUL 在扫描窗（0..8191）之外
    DOCTEST_CHECK_FALSE(np::notepadProbeEncoding(outside).binary);

    std::string edge(8192, 'x');
    edge[8191] = '\0';  // 扫描窗最后一个字节
    DOCTEST_CHECK_TRUE(np::notepadProbeEncoding(edge).binary);
    return true;
})

// EOL 四态探测（空文本 = Lf）。
REGISTER_TEST(notepad_encoding_eol_detect, "notepad_encoding: EOL 四态", {
    DOCTEST_CHECK_EQ(np::notepadDetectEol(""), np::NotepadEol::Lf);
    DOCTEST_CHECK_EQ(np::notepadDetectEol("no newline"), np::NotepadEol::Lf);
    DOCTEST_CHECK_EQ(np::notepadDetectEol("a\nb"), np::NotepadEol::Lf);
    DOCTEST_CHECK_EQ(np::notepadDetectEol("a\r\nb"), np::NotepadEol::Crlf);
    DOCTEST_CHECK_EQ(np::notepadDetectEol("a\rb"), np::NotepadEol::Cr);
    DOCTEST_CHECK_EQ(np::notepadDetectEol("a\r\nb\nc\rd"), np::NotepadEol::Mixed);
    return true;
})

// LF 归一：CRLF / 孤 CR → LF；混合回填 mixedOut=true；纯 LF 原样。
REGISTER_TEST(notepad_encoding_normalize_lf, "notepad_encoding: LF 归一", {
    bool mixed = true;
    DOCTEST_CHECK_EQ(np::notepadNormalizeToLf("a\r\nb", &mixed), std::string("a\nb"));
    DOCTEST_CHECK_FALSE(mixed);
    DOCTEST_CHECK_EQ(np::notepadNormalizeToLf("a\rb", &mixed), std::string("a\nb"));
    DOCTEST_CHECK_FALSE(mixed);
    DOCTEST_CHECK_EQ(np::notepadNormalizeToLf("a\r\nb\nc", &mixed), std::string("a\nb\nc"));
    DOCTEST_CHECK_TRUE(mixed);
    DOCTEST_CHECK_EQ(np::notepadNormalizeToLf("a\nb", &mixed), std::string("a\nb"));
    DOCTEST_CHECK_FALSE(mixed);
    return true;
})

// EOL 回写：Lf 原样 / Cr 全替 / Crlf 全扩 / Mixed 按 CRLF。
REGISTER_TEST(notepad_encoding_apply_eol, "notepad_encoding: EOL 回写四档", {
    DOCTEST_CHECK_EQ(np::notepadApplyEol("a\nb", np::NotepadEol::Lf), std::string("a\nb"));
    DOCTEST_CHECK_EQ(np::notepadApplyEol("a\nb", np::NotepadEol::Cr), std::string("a\rb"));
    DOCTEST_CHECK_EQ(np::notepadApplyEol("a\nb\nc", np::NotepadEol::Crlf),
                     std::string("a\r\nb\r\nc"));
    DOCTEST_CHECK_EQ(np::notepadApplyEol("a\nb", np::NotepadEol::Mixed),
                     std::string("a\r\nb"));
    return true;
})

// 干净直通 vs 脏重编码：notepadEncodeBuffer —— 无 BOM+Lf = 原样（干净缓冲
// 字节级直通的编码侧）；hasBom = BOM 三字节 + 体；CRLF 档行尾全扩。
//（BOM 字面量与后续 'a' 用字符串拼接断开，防 \xBFa 贪婪十六进制转义。）
REGISTER_TEST(notepad_encoding_encode_buffer, "notepad_encoding: 编码回写矩阵", {
    DOCTEST_CHECK_EQ(np::notepadEncodeBuffer("a\nb", false, np::NotepadEol::Lf),
                     std::string("a\nb"));
    DOCTEST_CHECK_EQ(np::notepadEncodeBuffer("a\nb", true, np::NotepadEol::Lf),
                     std::string("\xEF\xBB\xBF") + "a\nb");
    DOCTEST_CHECK_EQ(np::notepadEncodeBuffer("a\nb", false, np::NotepadEol::Crlf),
                     std::string("a\r\nb"));
    DOCTEST_CHECK_EQ(np::notepadEncodeBuffer("a\nb", true, np::NotepadEol::Crlf),
                     std::string("\xEF\xBB\xBF") + "a\r\nb");
    return true;
})

// 文本度量三函数（CJK / emoji 码点边界）。
REGISTER_TEST(notepad_encoding_text_metrics, "notepad_encoding: 文本度量三函数", {
    DOCTEST_CHECK_EQ(np::notepadUtf8Length("abc"), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(np::notepadUtf8Length("你好"), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(np::notepadUtf8Length("a\xF0\x9F\x98\x80" "b"), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(np::notepadUtf8Length(""), static_cast<size_t>(0));

    const std::string text = "ab\ncd\n中";  // a0 b1 \n2 c3 d4 \n5 中6..8
    DOCTEST_CHECK_EQ(np::notepadByteOffsetToLine(text, 0), 0);
    DOCTEST_CHECK_EQ(np::notepadByteOffsetToLine(text, 2), 0);
    DOCTEST_CHECK_EQ(np::notepadByteOffsetToLine(text, 3), 1);
    DOCTEST_CHECK_EQ(np::notepadByteOffsetToLine(text, 6), 2);
    DOCTEST_CHECK_EQ(np::notepadByteOffsetToLine(text, 9), 2);
    DOCTEST_CHECK_EQ(np::notepadByteOffsetToLine(text, 99), 2);  // 越界钳制

    DOCTEST_CHECK_EQ(np::notepadLineStartOffset(text, 0), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(np::notepadLineStartOffset(text, 1), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(np::notepadLineStartOffset(text, 2), static_cast<size_t>(6));
    DOCTEST_CHECK_EQ(np::notepadLineStartOffset(text, 3), text.size());  // 越界钳末尾
    DOCTEST_CHECK_EQ(np::notepadLineStartOffset(text, -1), static_cast<size_t>(0));

    // 行列换算闭环：第 7 字节（「中」行内）→ 第 2 行 → 行首=6
    DOCTEST_CHECK_EQ(np::notepadLineStartOffset(text, np::notepadByteOffsetToLine(text, 7)),
                     static_cast<size_t>(6));
    return true;
})
