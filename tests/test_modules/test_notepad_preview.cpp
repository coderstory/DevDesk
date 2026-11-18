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
#include <components/markdown.h>  // need345 选区缝（displayTextOf/矩形/点反查）

#include <algorithm>  // std::min（need345 码点计数步进）
#include <string>

using module::notepad::NotepadSession;
using module::notepad::notepadPreviewSelText;  // need345 选区文本构建单点直驱
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
// ── need345（池 #345）：md 预览「选中复制+toast」——选区文本构建（整篇显示文本码点域，语义平移自退役查看器 eebed0b9 池 #264）
// 硬验收：显示域无 markdown 标记符；跨块 '\n' 含在扁平域内（= 旧「多块按阅读序 '\n' 连接」等价）；CJK 码点切片无半字；空选区 = 空串；逆序拖选归一化。
// session 字段直驱（NotepadSession 开放结构 = 注入缝），与拖选抬起自动复制 / Ctrl+C 同一条 notepadPreviewSelText 通路。
REGISTER_TEST(notepad_md_sel_text_build,
              "notepad_preview: need345 预览选区文本——标记剥离 + 跨块 '\n' + CJK 无半字 + 空/逆序/越界钳", {
    // 码点计数（到字节偏移为止；尾截断序列钳位串尾）
    auto cpsUpTo = [](const std::string& t, size_t bytes) {
        size_t n = 0, i = 0;
        while (i < bytes && i < t.size()) {
            const unsigned char c = static_cast<unsigned char>(t[i]);
            i += std::min<size_t>(c >= 0xF0U ? 4 : c >= 0xE0U ? 3 : c >= 0xC0U ? 2 : 1,
                                  t.size() - i);
            ++n;
        }
        return n;
    };
    NotepadSession s = makeSession("/d/a.md");
    s.editText = "# 标题\n\n中文段落 **加粗** 与 `code` 标记。\n\n- 甲项\n- 乙项\n";
    // display = 标题\n中文段落 加粗 与 code 标记。\n-甲项\n-乙项（列表 prefix 无空格分隔）
    const std::string display = components::MarkdownBuilder::displayTextOf(s.editText);
    DOCTEST_CHECK(display.find("**") == std::string::npos);  // 标记符剥离（显示域）
    DOCTEST_CHECK(display.find("`") == std::string::npos);
    DOCTEST_CHECK(display.find("-甲项") != std::string::npos);  // 列表 prefix 进显示域

    auto cpOf = [&](const char* sub) {
        return cpsUpTo(display, display.find(sub));
    };
    const size_t totalCps = cpsUpTo(display, display.size());

    // CJK 码点切片无半字：「文段」= 中文段落 的 [1,3)
    s.selAnchorCp = cpOf("中文段落") + 1;
    s.selFocusCp = cpOf("中文段落") + 3;
    DOCTEST_CHECK_EQ(notepadPreviewSelText(s), std::string("文段"));

    // 跨块尾段（cp 越界钳到显示文本末）：块尾词 + 块间 '\n' + 两个列表项
    s.selAnchorCp = cpOf("标记");
    s.selFocusCp = 9999;
    DOCTEST_CHECK_EQ(notepadPreviewSelText(s), display.substr(display.find("标记")));

    // 全选 [0, 总码点) = 整条显示文本（扁平域身份性 + 钳界）
    s.selAnchorCp = 0;
    s.selFocusCp = totalCps;
    DOCTEST_CHECK_EQ(notepadPreviewSelText(s), display);

    // 逆序拖选（焦点在锚前）归一化到同一阅读序文本
    s.selAnchorCp = totalCps;
    s.selFocusCp = 0;
    DOCTEST_CHECK_EQ(notepadPreviewSelText(s), display);

    // 空选区（锚=焦）= 空串（复制语义：不写剪贴板）
    s.selAnchorCp = s.selFocusCp = 5;
    DOCTEST_CHECK_EQ(notepadPreviewSelText(s), std::string());
    return true;
})

// ── need345：整篇文档系选区矩形缝（hitRectsForDisplayRangeDoc）零漂移 ─────
// 硬验收 = 与 displayCpForPoint 同源零漂移（渲染同源）：逐码点取单码点文档系矩形，左缘(+ε)反查必回同一码点；矩形 y 全体落在文档高度内（纵向累计与 estimateHeight/渲染排布一致）；标题/段落/列表/代码块域逐码点覆盖；表格 cell 级角点抽查（旧查看器 hitA 同口径——cell 包围盒近似下单码点往返不适用）。
REGISTER_TEST(notepad_md_sel_doc_rects_roundtrip,
              "notepad_preview: need345 文档系选区矩形——displayCpForPoint 往返零漂移 + 高度域内 + 多块型覆盖", {
    constexpr float kWidth = 420.0f;
    const components::MarkdownStyle style;
    // 逐码点往返域：标题/长段落（真实折行）/列表/代码块（无表格）
    const std::string body =
        "# CJK 标题\n\n"
        "正文段落：中文连续排布会真实折行，English words 混排验证最近边界反查，"
        "这一段刻意写到足够长确保 wrap 发生出现第二行带。\n\n"
        "- 列表项含词\n\n"
        "```cpp\nint x = 42; // 注释\n```\n";
    const std::string display = components::MarkdownBuilder::displayTextOf(body);
    size_t totalCps = 0, bi = 0;
    while (bi < display.size()) {
        const unsigned char c = static_cast<unsigned char>(display[bi]);
        bi += std::min<size_t>(c >= 0xF0U ? 4 : c >= 0xE0U ? 3 : c >= 0xC0U ? 2 : 1,
                              display.size() - bi);
        ++totalCps;
    }
    const float docH =
        components::MarkdownBuilder::estimateHeight(body, kWidth, style);
    DOCTEST_CHECK(docH > 0.0f);

    int roundtripped = 0;
    for (size_t cp = 0; cp < totalCps; ++cp) {
        const auto rects = components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
            body, kWidth, style, cp, cp + 1);
        if (rects.empty()) continue;
        const auto& r = rects.front();
        // 高度域内（文档系 y；尾块后 yAcc 多计一个 blockGap，放行一个 gap 余量）
        DOCTEST_CHECK(r.y >= 0.0f);
        DOCTEST_CHECK(r.y + r.h <= docH + style.blockGap + 0.5f);
        // 左缘(+ε)反查回同一码点：矩形与点反查同一条布局/纵向计账通路
        const auto hit = components::MarkdownBuilder::displayCpForPoint(
            body, kWidth, style, r.x + 0.5f, r.y + r.h * 0.5f);
        DOCTEST_CHECK_MESSAGE(
            hit.ok && hit.cp == cp,
            "doc 左缘往返 cp=" + std::to_string(cp) +
                " hit=" + std::to_string(hit.cp) +
                " y=" + std::to_string(r.y));
        ++roundtripped;
    }
    DOCTEST_CHECK(roundtripped > 40);  // 标题+长段落+列表+代码块逐码点覆盖量

    // 代码块子文档单独覆盖（虚拟行近似域非空 + 左缘往返首码点）
    const std::string codeDoc = "```cpp\nint x = 42;\n```";
    const std::string codeDisp = components::MarkdownBuilder::displayTextOf(codeDoc);
    const auto codeRects = components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
        codeDoc, kWidth, style, 0, codeDisp.size());
    DOCTEST_CHECK(!codeRects.empty());
    const auto codeHit = components::MarkdownBuilder::displayCpForPoint(
        codeDoc, kWidth, style, codeRects.front().x + 0.5f,
        codeRects.front().y + codeRects.front().h * 0.5f);
    DOCTEST_CHECK(codeHit.ok && codeHit.cp == 0);

    // 表格域（cell 级近似）：矩形非空 + 首行上缘反查落 cp0（旧查看器 hitA 同口径）
    const std::string tblDoc = "| 甲 | 乙 |\n| --- | --- |\n| 丙 | 丁 |";
    const std::string tblDisp = components::MarkdownBuilder::displayTextOf(tblDoc);
    const auto tblRects = components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
        tblDoc, kWidth, style, 0, tblDisp.size());
    DOCTEST_CHECK(!tblRects.empty());
    const auto tblHit = components::MarkdownBuilder::displayCpForPoint(
        tblDoc, kWidth, style, 1.0f, tblRects.front().y + 1.0f);
    DOCTEST_CHECK(tblHit.ok);
    DOCTEST_CHECK(tblHit.cp == 0);

    // 整篇全选矩形量 ≥ 单码点计数（铺满各块）
    const auto allRects = components::MarkdownBuilder::hitRectsForDisplayRangeDoc(
        body, kWidth, style, 0, totalCps);
    DOCTEST_CHECK(allRects.size() >= static_cast<size_t>(roundtripped));
    return true;
})
