// tests/test_platform/test_git_diff_model.cpp
// git 管理器批次三（need:10）：diff unified 行模型——
//   ① patch → 「hunk 头/行」扁平行序列（顺序与下标锚定）；
//   ② hunk 折叠/展开（I-06）：折叠 = 头行 + 折叠条，行隐藏；再翻转恢复；
//   ③ 巨 diff >5000 行：行模型一次物化、折叠翻转 O(n) 重建不炸（分块喂虚拟列表）；
//   ④ 文件头文案（重命名 old → new）、二进制/空 diff 判据、clear 复位。
// 纯函数面直打构造 patch，零 git 依赖。
#include "doctest.h"
#include "test_support.h"

#include "module_git/git_diff_model.h"

#include <string>

using module::git::DiffRow;
using module::git::GitDiffModel;
using platform::git::DiffHunk;
using platform::git::DiffLine;
using platform::git::DiffPatch;

namespace {

DiffLine line(char origin, int oldNo, int newNo, std::string content) {
    DiffLine l;
    l.origin = origin;
    if (oldNo > 0) l.oldNo = oldNo;
    if (newNo > 0) l.newNo = newNo;
    l.content = std::move(content);
    return l;
}

DiffPatch makePatch(size_t hunkLines) {
    DiffPatch p;
    p.oldPath = "a.txt";
    p.newPath = "a.txt";
    DiffHunk h;
    h.header = "@@ -1,3 +1,4 @@";
    for (size_t i = 0; i < hunkLines; ++i) {
        if (i == 1) {
            h.lines.push_back(line('-', 2, 0, "old line"));
            h.lines.push_back(line('+', 0, 2, "new line"));
        } else {
            h.lines.push_back(line(' ', static_cast<int>(i + 1),
                                   static_cast<int>(i + 1), "ctx"));
        }
    }
    p.hunks.push_back(std::move(h));
    return p;
}

} // namespace

REGISTER_TEST(git_diff_rows_structure, "git_diff: 行序列 = hunk 头 + 行交错展开", {
    GitDiffModel m;
    m.setPatch(makePatch(5));
    const auto& rows = m.rows();
    DOCTEST_CHECK(rows.size() == 7);  // 1 头 + 6 行（5 迭代：4 ctx + 1 处 -/+ 对）
    DOCTEST_CHECK(rows[0].kind == DiffRow::Kind::HunkHeader);
    DOCTEST_CHECK(rows[0].hunkIndex == 0);
    DOCTEST_CHECK(rows[1].kind == DiffRow::Kind::Line);
    DOCTEST_CHECK(rows[1].lineIndex == 0);
    DOCTEST_CHECK(rows[2].lineIndex == 1);
    DOCTEST_CHECK(rows.back().kind == DiffRow::Kind::Line);
    DOCTEST_CHECK(!m.isEmpty());
    DOCTEST_CHECK(!m.binary());
    return true;
})

REGISTER_TEST(git_diff_fold_toggle, "git_diff: hunk 折叠=头+折叠条，展开恢复", {
    GitDiffModel m;
    m.setPatch(makePatch(5));
    DOCTEST_CHECK(!m.folded(0));
    m.toggleHunk(0);
    DOCTEST_CHECK(m.folded(0));
    const auto& rows = m.rows();
    DOCTEST_CHECK(rows.size() == 2);  // 头 + 折叠条
    DOCTEST_CHECK(rows[0].kind == DiffRow::Kind::HunkHeader);
    DOCTEST_CHECK(rows[1].kind == DiffRow::Kind::FoldBar);
    DOCTEST_CHECK(rows[1].hunkIndex == 0);  // 内容锚（clue#14：折叠按 hunk id 恢复）
    m.toggleHunk(0);
    DOCTEST_CHECK(!m.folded(0));
    DOCTEST_CHECK(m.rows().size() == 7);
    return true;
})

REGISTER_TEST(git_diff_giant_chunked, "git_diff: 巨 diff >5000 行模型物化与折叠", {
    GitDiffModel m;
    m.setPatch(makePatch(6000));
    DOCTEST_CHECK(m.rows().size() == 6002);  // 6000 迭代：5999 ctx + 1 对 → 头 + 6001 行
    m.toggleHunk(0);
    DOCTEST_CHECK(m.rows().size() == 2);
    m.toggleHunk(0);
    DOCTEST_CHECK(m.rows().size() == 6002);
    return true;
})

REGISTER_TEST(git_diff_header_and_flags, "git_diff: 重命名文件头/二进制/空 diff/clear", {
    {
        GitDiffModel m;
        DiffPatch p = makePatch(3);
        p.oldPath = "docs/旧名.md";
        p.newPath = "docs/新名.md";
        m.setPatch(std::move(p));
        DOCTEST_CHECK(m.headerText() == "docs/旧名.md → docs/新名.md");
    }
    {
        GitDiffModel m;
        DiffPatch p = makePatch(3);
        p.oldPath = "a.txt";
        p.newPath = "a.txt";
        m.setPatch(std::move(p));
        DOCTEST_CHECK(m.headerText() == "a.txt");
    }
    {
        GitDiffModel m;
        DiffPatch p;
        p.oldPath = "asset.png";
        p.newPath = "asset.png";
        p.binary = true;
        m.setPatch(std::move(p));
        DOCTEST_CHECK(m.binary());
        DOCTEST_CHECK(m.rows().empty());  // 二进制不出行（视图占位，F-11）
    }
    {
        GitDiffModel m;
        DiffPatch p;
        p.oldPath = "same.txt";
        p.newPath = "same.txt";
        m.setPatch(std::move(p));
        DOCTEST_CHECK(m.isEmpty());  // 无 hunk = 「无内容变更」占位
    }
    {
        GitDiffModel m;
        m.setPatch(makePatch(3));
        m.toggleHunk(0);
        m.clear();
        DOCTEST_CHECK(m.rows().empty());
        DOCTEST_CHECK(!m.folded(0));
        DOCTEST_CHECK(m.isEmpty());
    }
    return true;
})

// ---- bug#18：untracked 条目 diff 模型层合成（≤1MiB 全增行 / >1MiB·二进制占位 /
// 空文件空态；禁 GIT_DIFF_INCLUDE_UNTRACKED_CONTENT 的 F-09 内存约束下不读全文）----

REGISTER_TEST(git_diff_untracked_synth_all_add, "git_diff: untracked ≤1MiB 文本合成单 hunk 全增行（行号 1 起、origin='+'）", {
    const test_support::TempDir dir("untracked_add");
    const std::string content = "alpha\nbeta\n\ngamma\n";  // 含空行；尾换行不产生末尾空行
    dir.write(dir.file("u.txt"), content);

    DiffPatch p;
    p.oldPath.clear();
    p.newPath = "u.txt";
    p.isUntracked = true;
    const std::string ph = module::git::synthesizeUntrackedDiff(p, dir.file("u.txt"));
    DOCTEST_CHECK(ph.empty());
    DOCTEST_CHECK(p.hunks.size() == 1);
    DOCTEST_CHECK(p.hunks[0].header == "@@ -0,0 +1,4 @@");
    DOCTEST_CHECK(p.hunks[0].lines.size() == 4);
    for (size_t i = 0; i < p.hunks[0].lines.size(); ++i) {
        const DiffLine& l = p.hunks[0].lines[i];
        DOCTEST_CHECK(l.origin == '+');
        DOCTEST_CHECK(!l.oldNo.has_value());
        DOCTEST_CHECK(l.newNo.value_or(0) == static_cast<int>(i) + 1);  // 行号 1 起
    }
    DOCTEST_CHECK(p.hunks[0].lines[0].content == "alpha");
    DOCTEST_CHECK(p.hunks[0].lines[2].content.empty());  // 空行保留

    // 合成 hunk 走既有模型/行盒（clue#22）：行序列、二进制/空判据、横滚宽全复用
    GitDiffModel m;
    m.setPatch(std::move(p));
    DOCTEST_CHECK(!m.isEmpty());
    DOCTEST_CHECK(!m.binary());
    DOCTEST_CHECK(m.rows().size() == 5);  // 1 hunk 头 + 4 行
    DOCTEST_CHECK(m.maxLineLen() == 5);   // "alpha"/"gamma" = 5 字符
    return true;
})

REGISTER_TEST(git_diff_untracked_synth_oversize, "git_diff: untracked >1MiB → 占位文案（不读内容）、保持无 hunk", {
    const test_support::TempDir dir("untracked_big");
    dir.write(dir.file("big.txt"), std::string(1024 * 1024 + 1, 'x'));  // 1 MiB+1

    DiffPatch p;
    p.newPath = "big.txt";
    p.isUntracked = true;
    const std::string ph = module::git::synthesizeUntrackedDiff(p, dir.file("big.txt"));
    DOCTEST_CHECK(ph == "文件过大，未加载内容（>1 MiB）");
    DOCTEST_CHECK(p.hunks.empty());
    DOCTEST_CHECK(!p.binary);

    GitDiffModel m;
    m.setPatch(std::move(p), ph);
    DOCTEST_CHECK(m.placeholder() == "文件过大，未加载内容（>1 MiB）");
    DOCTEST_CHECK(m.isEmpty());
    DOCTEST_CHECK(m.rows().empty());  // 视图占位短路，不出行序列
    return true;
})

REGISTER_TEST(git_diff_untracked_synth_binary_empty, "git_diff: untracked 二进制→binary 占位、空文件→保持空态", {
    const test_support::TempDir dir("untracked_bin_empty");
    dir.write(dir.file("bin.dat"), std::string("abc\0def", 7));
    {
        DiffPatch p;
        p.newPath = "bin.dat";
        p.isUntracked = true;
        const std::string ph = module::git::synthesizeUntrackedDiff(p, dir.file("bin.dat"));
        DOCTEST_CHECK(ph.empty());
        DOCTEST_CHECK(p.binary);
        DOCTEST_CHECK(p.hunks.empty());
    }
    {
        dir.write(dir.file("empty.txt"), std::string());
        DiffPatch p;
        p.newPath = "empty.txt";
        p.isUntracked = true;
        const std::string ph = module::git::synthesizeUntrackedDiff(p, dir.file("empty.txt"));
        DOCTEST_CHECK(ph.empty());
        DOCTEST_CHECK(p.hunks.empty());
        DOCTEST_CHECK(!p.binary);  // 空文件 ≠ 二进制：走「无内容变更」空态
    }
    return true;
})

REGISTER_TEST(git_diff_untracked_synth_guards, "git_diff: 合成守卫——非 untracked / 目录条目(尾'/') / 已有 hunk 一律不动", {
    const test_support::TempDir dir("untracked_guard");
    dir.write(dir.file("t.txt"), "line\n");
    {
        DiffPatch p;  // 非 untracked（普通 delta 缺 hunk = 真无变更）：不动
        p.newPath = "t.txt";
        DOCTEST_CHECK(module::git::synthesizeUntrackedDiff(p, dir.file("t.txt")).empty());
        DOCTEST_CHECK(p.hunks.empty());
        DOCTEST_CHECK(!p.binary);
    }
    {
        DiffPatch p;  // untracked 目录条目：§2.1 不递归决策不破，不触碰磁盘
        p.newPath = "somedir/";
        p.isUntracked = true;
        DOCTEST_CHECK(module::git::synthesizeUntrackedDiff(p, dir.file("somedir/")).empty());
        DOCTEST_CHECK(p.hunks.empty());
    }
    {
        DiffPatch p;  // 已有 hunk 的 untracked patch（理论不可达）：原样保留
        p.newPath = "t.txt";
        p.isUntracked = true;
        p.hunks.push_back(DiffHunk{});
        p.hunks[0].lines.push_back(line(' ', 1, 1, "keep"));
        DOCTEST_CHECK(module::git::synthesizeUntrackedDiff(p, dir.file("t.txt")).empty());
        DOCTEST_CHECK(p.hunks.size() == 1);
        DOCTEST_CHECK(p.hunks[0].lines.size() == 1);
    }
    return true;
})

// bug#21（diff 侧生产点）：maxLineLen_ 曾按字节计（line.content.size()）——
// CJK 行 3 字节/字虚胖；need:14 P1-1：码点口径对纯 CJK 行仍欠宽（mono 字体
// CJK 实测 ~1.0em/字 vs 模型 0.62em/列）——修后按显示列计（非 ASCII 起始序列
// 2 列），与内容模型同口径，恒偏大不变式成立。
REGISTER_TEST(git_diff_maxlinelen_display_cells, "git_diff: maxLineLen 显示列数（非 ASCII 按 2 列，恒偏大不变式，bug#21+P1-1）", {
    GitDiffModel m;
    DiffPatch p;
    p.oldPath = "a.txt";
    p.newPath = "a.txt";
    DiffHunk h;
    const std::string cjk10 = "\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80"
                              "\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80\xE4\xB8\x80"
                              "\xE4\xB8\x80\xE4\xB8\x80";
    h.lines.push_back(line('+', 0, 1, cjk10));
    h.lines.push_back(line('+', 0, 2, "0123456789"));
    p.hunks.push_back(std::move(h));
    m.setPatch(std::move(p));
    DOCTEST_CHECK(m.maxLineLen() == 20);   // 10 字 × 2 列（恒偏大：不欠于实宽）
    return true;
})
