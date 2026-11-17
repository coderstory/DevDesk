// tests/test_modules/test_notepad_preview.cpp
// need263 批 3 棒 B（池 #286）：F-11 JSON 格式化 + F-13 md 预览纯函数面无头直测。
// ①jsonLineColForPos 薄封装边界（首行/末行/行首/CJK 前码点计数/越界钳界/空文本，
//   期望值经 yyjson 0.12.0 实测锚定）；②parse→stringify(pretty) 格式化幂等性
//   （二次格式化字节稳定）+ 键序字典序重排（json.cpp std::map 容器既定行为，
//   clue #252/#253）+ 两空格缩进；③工具条联动矩阵纯函数（按扩展名按钮矩阵 +
//   预览态/只读/大文件禁用，设计稿 §4.6）。
#include "doctest.h"
#include "module_notepad/notepad_session.h"
#include "platform/json.h"

#include <string>

using module::notepad::NotepadSession;
using module::notepad::NotepadToolbarMatrix;
using module::notepad::notepadToolbarMatrixFor;
using platform::json::JsonLineCol;
using platform::json::ParseDiag;
using platform::json::jsonLineColForPos;
using platform::json::parse;
using platform::json::stringify;

namespace {

// 构造测试 session（normKey 手写即判据；其余字段走默认值）。
NotepadSession makeSession(const std::string& normKey) {
    NotepadSession s;
    s.path = normKey;
    s.normKey = normKey;
    return s;
}

// parse→stringify(pretty) 完整格式化链（notepad_window notepadFormatJson 同序）。
bool formatJson(const std::string& in, std::string& out) {
    ParseDiag diag;
    const core::Result<platform::json::JsonValue> parsed = parse(in, &diag);
    if (!parsed.isOk()) return false;
    const core::Result<std::string> pretty =
        stringify(parsed.value(), /*pretty=*/true);
    if (!pretty.isOk()) return false;
    out = pretty.value();
    return true;
}

}  // namespace

// ── jsonLineColForPos（1 基行列；列按 Unicode 码点计，yyjson_locate_pos 语义）──

REGISTER_TEST(notepad_preview_linecol_basic,
              "notepad_preview: 行列换算 首行/跨行/行首/文末", {
    const JsonLineCol a = jsonLineColForPos("a\nb", 0);
    DOCTEST_CHECK_EQ(a.line, size_t{1});
    DOCTEST_CHECK_EQ(a.col, size_t{1});
    const JsonLineCol b = jsonLineColForPos("a\nb", 1);  // '\n' 本位 = 行 1 末列
    DOCTEST_CHECK_EQ(b.line, size_t{1});
    DOCTEST_CHECK_EQ(b.col, size_t{2});
    const JsonLineCol c = jsonLineColForPos("a\nb", 2);  // 第二行行首
    DOCTEST_CHECK_EQ(c.line, size_t{2});
    DOCTEST_CHECK_EQ(c.col, size_t{1});
    const JsonLineCol d = jsonLineColForPos("a\nb", 3);  // 文末（pos=len）
    DOCTEST_CHECK_EQ(d.line, size_t{2});
    DOCTEST_CHECK_EQ(d.col, size_t{2});
    return true;
})

REGISTER_TEST(notepad_preview_linecol_cjk_and_clamp,
              "notepad_preview: 行列换算 CJK 前码点计数/越界钳界/空文本", {
    // "ab\n中文x"：pos=9 = 'x'（中=3B 文=3B）；列按码点：中=1 文=2 x=3
    const JsonLineCol cjk =
        jsonLineColForPos("ab\n\xe4\xb8\xad\xe6\x96\x87x", 9);
    DOCTEST_CHECK_EQ(cjk.line, size_t{2});
    DOCTEST_CHECK_EQ(cjk.col, size_t{3});
    // 越界钳到文末（yyjson 对 pos>len 返回全 0，封装钳界 + 1 基兜底）
    const JsonLineCol oob = jsonLineColForPos("a\nb", 99);
    DOCTEST_CHECK_EQ(oob.line, size_t{2});
    DOCTEST_CHECK_EQ(oob.col, size_t{2});
    const JsonLineCol empty = jsonLineColForPos("", 0);
    DOCTEST_CHECK_EQ(empty.line, size_t{1});
    DOCTEST_CHECK_EQ(empty.col, size_t{1});
    return true;
})

REGISTER_TEST(notepad_preview_parse_diag_line,
              "notepad_preview: parse 错误 bytePos → 行列换算（第 3 行）", {
    const std::string text = "{\n  \"a\": 1,\n  \"b\": ?\n}";
    ParseDiag diag;
    const core::Result<platform::json::JsonValue> parsed = parse(text, &diag);
    DOCTEST_CHECK_FALSE(parsed.isOk());
    // 错误落在第 3 行 '?' 一带（字节域 [12,21]；不锁 yyjson 精确 pos）
    DOCTEST_CHECK(diag.bytePos >= 12);
    DOCTEST_CHECK(diag.bytePos <= 21);
    const JsonLineCol lc = jsonLineColForPos(text, diag.bytePos);
    DOCTEST_CHECK_EQ(lc.line, size_t{3});
    DOCTEST_CHECK(lc.col >= 1);
    return true;
})

// ── F-11 格式化链：幂等 / 键序 / 缩进 ──

REGISTER_TEST(notepad_preview_format_idempotent_keyorder,
              "notepad_preview: 格式化幂等 + 键序字典序重排 + 两空格缩进", {
    const std::string messy =
        "{\"b\":1,\"a\":[1,2,{\"z\":true,\"y\":null}],\"c\":\"文本\"}";
    std::string out1;
    DOCTEST_REQUIRE(formatJson(messy, out1));
    std::string out2;
    DOCTEST_REQUIRE(formatJson(out1, out2));
    DOCTEST_CHECK(out1 == out2);  // 二次格式化字节稳定（幂等）
    // std::map 容器 → 键按字典序输出（clue #252/#253 既定行为）
    DOCTEST_CHECK(out1.find("\"a\"") < out1.find("\"b\""));
    DOCTEST_CHECK(out1.find("\"b\"") < out1.find("\"c\""));
    const size_t zPos = out1.find("\"z\"");
    const size_t yPos = out1.find("\"y\"");
    DOCTEST_CHECK(zPos != std::string::npos);
    DOCTEST_CHECK(yPos != std::string::npos);
    DOCTEST_CHECK(yPos < zPos);  // 字典序 y < z（嵌套对象同样重排）
    // pretty = 两空格缩进（json.h YYJSON_WRITE_PRETTY_TWO_SPACES）
    DOCTEST_CHECK(out1.find("\n  \"a\"") != std::string::npos);
    DOCTEST_CHECK(out1.find("\n    1") != std::string::npos);
    return true;
})

// ── F-13/F-11 工具条联动矩阵（设计稿 §4.2/§4.6）──

REGISTER_TEST(notepad_preview_matrix_extension,
              "notepad_preview: 按扩展名按钮矩阵（仅 .json 格式化/仅 .md 预览）", {
    const NotepadToolbarMatrix json =
        notepadToolbarMatrixFor(makeSession("/d/cfg.json"));
    DOCTEST_CHECK(json.showFormat);
    DOCTEST_CHECK_FALSE(json.showPreview);
    DOCTEST_CHECK(json.formatEnabled);

    const NotepadToolbarMatrix md =
        notepadToolbarMatrixFor(makeSession("/d/note.md"));
    DOCTEST_CHECK_FALSE(md.showFormat);
    DOCTEST_CHECK(md.showPreview);
    DOCTEST_CHECK(md.previewEnabled);

    const NotepadToolbarMatrix mdown =
        notepadToolbarMatrixFor(makeSession("/d/README.markdown"));
    DOCTEST_CHECK(mdown.showPreview);

    const NotepadToolbarMatrix txt =
        notepadToolbarMatrixFor(makeSession("/d/a.txt"));
    DOCTEST_CHECK_FALSE(txt.showFormat);
    DOCTEST_CHECK_FALSE(txt.showPreview);

    // 扩展名大小写折叠（normKey 保留原 casing）
    const NotepadToolbarMatrix upper =
        notepadToolbarMatrixFor(makeSession("/d/NOTE.MD"));
    DOCTEST_CHECK(upper.showPreview);
    const NotepadToolbarMatrix upperJson =
        notepadToolbarMatrixFor(makeSession("/d/X.JSON"));
    DOCTEST_CHECK(upperJson.showFormat);
    return true;
})

REGISTER_TEST(notepad_preview_matrix_disable,
              "notepad_preview: 只读/二进制禁格式化/大文件禁预览/错误态全隐", {
    NotepadSession ro = makeSession("/d/a.json");
    ro.readonly = true;
    const NotepadToolbarMatrix mro = notepadToolbarMatrixFor(ro);
    DOCTEST_CHECK(mro.showFormat);  // 钮在场但禁用
    DOCTEST_CHECK_FALSE(mro.formatEnabled);

    NotepadSession bin = makeSession("/d/a.json");
    bin.binarySuspect = true;
    DOCTEST_CHECK_FALSE(notepadToolbarMatrixFor(bin).formatEnabled);

    NotepadSession large = makeSession("/d/a.md");
    large.largeFile = true;
    const NotepadToolbarMatrix mlarge = notepadToolbarMatrixFor(large);
    DOCTEST_CHECK(mlarge.showPreview);  // 钮在场但禁用（1MB 级渲染成本未证，PRD F-16 降级）
    DOCTEST_CHECK_FALSE(mlarge.previewEnabled);

    NotepadSession err = makeSession("/d/a.json");
    err.loadError = "文件不存在";
    const NotepadToolbarMatrix merr = notepadToolbarMatrixFor(err);
    DOCTEST_CHECK_FALSE(merr.showFormat);
    DOCTEST_CHECK_FALSE(merr.showPreview);
    DOCTEST_CHECK_FALSE(merr.fontControlsEnabled);  // P2-1：错误窗字体触发器同守（A± 原已守）
    return true;
})

REGISTER_TEST(notepad_preview_matrix_preview_linkage,
              "notepad_preview: 预览态工具条联动（格式化/查找/字体禁用）", {
    NotepadSession s = makeSession("/d/a.json");
    s.previewOn = true;
    const NotepadToolbarMatrix m = notepadToolbarMatrixFor(s);
    DOCTEST_CHECK(m.showFormat);  // 钮仍在场
    DOCTEST_CHECK_FALSE(m.formatEnabled);  // 预览态禁用（§4.6）
    DOCTEST_CHECK_FALSE(m.findToggleEnabled);
    DOCTEST_CHECK_FALSE(m.fontControlsEnabled);

    NotepadSession md = makeSession("/d/a.md");
    md.previewOn = true;
    const NotepadToolbarMatrix mm = notepadToolbarMatrixFor(md);
    DOCTEST_CHECK(mm.showPreview);
    DOCTEST_CHECK(mm.previewEnabled);  // 预览钮在预览态仍可点（切回编辑）
    DOCTEST_CHECK_FALSE(mm.findToggleEnabled);
    DOCTEST_CHECK_FALSE(mm.fontControlsEnabled);
    return true;
})
