// tests/test_platform/test_git_change_list_model.cpp
// need291 Git 管理器重设计·T1 模型层分片（技术方案卡 doc25 §5 新用例族全量）：
//   ① 树派生（嵌套祖先目录集/目录前排序/深度）；② 单链压缩（a/b/c/ 合并节点）；
//   ③ 双组分桶——staged 双侧同文件两组各一行（D6-A 行为变更：跨组不去重，
//   原 test_git_status_model.cpp GitChangesModel::build 用例等价改挂）；
//   ④ untracked 目录行 U 特殊化（剥尾斜杠/childCount=0/R4）；⑤ 树/平铺展开位图
//   互不干扰（设计 §6.3）；⑥ >200 组头默认折叠 200|201 边界（POC②）；
//   ⑦ lazy 子行注入 + >500 截断 + pending 状态机（F-04/POC①）；
//   ⑧ 组头计数（展开子文件计入未暂存计数，设计 §4.3）；⑨ 刷新重建保位图/缓存
//   （F-09）；⑩ 平铺目录段聚合；⑪ 真仓 status 全链（原 real-repo 用例改挂）。
// 汇总编译模式：本波只编码不构建，红绿验证归波后合并验证子任务（老板 2026-11-10 令）。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "module_git/git_change_list_model.h"
#include "module_git/git_fs_walk.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using module::git::ChangeListRow;
using module::git::ChangeViewMode;
using module::git::GitChangeListModel;
using module::git::fs_walk::EntryKind;
using module::git::fs_walk::FsEntry;
using module::git::fs_walk::WalkResult;
using platform::git::Libgit2Scope;
using platform::git::Repo;
using platform::git::StatusEntry;
using platform::git::StatusKind;
using platform::git::StatusSnapshot;
using test_support::writeTextFile;

namespace {

StatusEntry entryOf(const std::string& path, StatusKind kind, bool staged,
                    const std::string& oldPath = {}) {
    StatusEntry e;
    e.path = path;
    e.kind = kind;
    e.staged = staged;
    e.oldPath = oldPath;
    return e;
}

const ChangeListRow* findRow(const std::vector<ChangeListRow>& rows,
                             ChangeListRow::Kind kind,
                             const std::string& relPath) {
    for (const auto& r : rows) {
        if (r.kind == kind && r.relPath == relPath) return &r;
    }
    return nullptr;
}

size_t countKind(const std::vector<ChangeListRow>& rows, ChangeListRow::Kind kind) {
    size_t n = 0;
    for (const auto& r : rows) {
        if (r.kind == kind) ++n;
    }
    return n;
}

const ChangeListRow* headerOf(const std::vector<ChangeListRow>& rows,
                              ChangeListRow::Group group) {
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::GroupHeader && r.group == group) {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

REGISTER_TEST(gitChangeListModelNestedTreeDerivation, "树派生：嵌套祖先目录集+目录前排序+深度（md_tree 同构）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("src/deep/x.cpp", StatusKind::Modified, false),
        entryOf("src/a.cpp", StatusKind::Modified, false),
        entryOf("zroot.txt", StatusKind::Modified, false),
        entryOf("文档/说明.md", StatusKind::New, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    const auto& rows = model.rows();
    // 默认全展开（设计 §7 N2：≤50 条全展开）：组头 + src/ + deep/ + x.cpp +
    // a.cpp + 文档/ + 说明.md + zroot.txt = 8 行
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(8));
    DOCTEST_CHECK_EQ(rows[0].kind, ChangeListRow::Kind::GroupHeader);
    DOCTEST_CHECK_EQ(rows[0].group, ChangeListRow::Group::Unstaged);
    DOCTEST_CHECK_EQ(rows[0].childCount, static_cast<size_t>(4));
    // 目录前排序：src/ < 文档/（字节序 's'(0x73) < '文'(0xE6)）
    DOCTEST_CHECK_EQ(rows[1].kind, ChangeListRow::Kind::Dir);
    DOCTEST_CHECK_EQ(rows[1].name, std::string("src/"));
    DOCTEST_CHECK_EQ(rows[1].depth, 0);
    DOCTEST_CHECK_EQ(rows[1].childCount, static_cast<size_t>(2));  // 子树 2 文件
    DOCTEST_CHECK(!rows[1].status.has_value());  // tracked 派生目录行无徽标
    // 子目录 deep/ 在直接文件 a.cpp 前（目录前排序）；x.cpp depth 2
    const ChangeListRow* deep = findRow(rows, ChangeListRow::Kind::Dir, "src/deep");
    DOCTEST_REQUIRE(deep != nullptr);
    DOCTEST_CHECK_EQ(deep->name, std::string("deep/"));
    DOCTEST_CHECK_EQ(deep->depth, 1);
    DOCTEST_CHECK_EQ(deep->childCount, static_cast<size_t>(1));
    const ChangeListRow* x = findRow(rows, ChangeListRow::Kind::File, "src/deep/x.cpp");
    DOCTEST_REQUIRE(x != nullptr);
    DOCTEST_CHECK_EQ(x->depth, 2);
    DOCTEST_CHECK_EQ(x->status, StatusKind::Modified);
    const ChangeListRow* a = findRow(rows, ChangeListRow::Kind::File, "src/a.cpp");
    DOCTEST_REQUIRE(a != nullptr);
    DOCTEST_CHECK_EQ(a->depth, 1);
    DOCTEST_CHECK_EQ(rows[5].name, std::string("文档/"));
    DOCTEST_CHECK_EQ(rows[7].kind, ChangeListRow::Kind::File);
    DOCTEST_CHECK_EQ(rows[7].relPath, std::string("zroot.txt"));
    // 显式折叠 src → 子树收起（目录行自身仍在，隐藏 deep/+x.cpp+a.cpp 共 3 行）
    model.setExpanded("src", false);
    DOCTEST_CHECK_EQ(model.rows().size(), static_cast<size_t>(5));  // 组头+src/+文档/+说明.md+zroot
    model.setExpanded("src", true);
    DOCTEST_CHECK_EQ(model.rows().size(), static_cast<size_t>(8));
    return true;
})

REGISTER_TEST(gitChangeListModelChainCompression, "单链压缩：唯一子目录链合并一个节点（x/y/z/），有分叉或直接文件即停（clue#199）", {
    // 纯链：x/y/z/f.txt → 一个压缩目录节点 + 文件
    StatusSnapshot snap;
    snap.entries = {entryOf("x/y/z/f.txt", StatusKind::Modified, false)};
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    const auto& rows = model.rows();
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(3));  // 组头 + 压缩目录 + 文件
    DOCTEST_CHECK_EQ(rows[1].kind, ChangeListRow::Kind::Dir);
    DOCTEST_CHECK_EQ(rows[1].name, std::string("x/y/z/"));
    DOCTEST_CHECK_EQ(rows[1].relPath, std::string("x/y/z"));  // 链最深段路径
    DOCTEST_CHECK_EQ(rows[1].depth, 0);
    DOCTEST_CHECK_EQ(rows[1].childCount, static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(rows[2].relPath, std::string("x/y/z/f.txt"));  // 默认展开文件可见
    // 显式折叠 → 子树收起；再展开恢复
    model.setExpanded("x/y/z", false);
    DOCTEST_CHECK_EQ(model.rows().size(), static_cast<size_t>(2));  // 组头 + 压缩目录
    model.setExpanded("x/y/z", true);
    DOCTEST_CHECK_EQ(model.rows().size(), static_cast<size_t>(3));
    // 分叉即停：a 有直接文件 → a 不与 b 合并；b 有直接文件 → b 不再下压
    StatusSnapshot snap2;
    snap2.entries = {
        entryOf("a/b/1.txt", StatusKind::Modified, false),
        entryOf("a/2.txt", StatusKind::Modified, false),
    };
    GitChangeListModel m2;
    m2.rebuild(snap2, ChangeViewMode::Tree);
    const auto& r2 = m2.rows();
    DOCTEST_CHECK_EQ(r2.size(), static_cast<size_t>(5));  // 组头 + a/ + b/ + 1.txt + 2.txt
    DOCTEST_CHECK_EQ(r2[1].name, std::string("a/"));
    DOCTEST_CHECK_EQ(r2[1].relPath, std::string("a"));  // 未压缩（有直接文件）
    const ChangeListRow* b = findRow(r2, ChangeListRow::Kind::Dir, "a/b");
    DOCTEST_REQUIRE(b != nullptr);
    DOCTEST_CHECK_EQ(b->name, std::string("b/"));  // 单段显示（链尾仅一层）
    DOCTEST_CHECK_EQ(b->depth, 1);
    const ChangeListRow* f1 = findRow(r2, ChangeListRow::Kind::File, "a/b/1.txt");
    DOCTEST_REQUIRE(f1 != nullptr);
    DOCTEST_CHECK_EQ(f1->depth, 2);
    return true;
})

REGISTER_TEST(gitChangeListModelBucketBothSidesTwoRows, "双组分桶：staged 双侧同文件两组各一行（D6-A 跨组不去重）+ 组内去重 + 组头序", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("a.txt", StatusKind::Modified, true),
        entryOf("a.txt", StatusKind::Modified, false),  // 双侧 → 两组各一行
        entryOf("a.txt", StatusKind::Modified, false),  // 组内重复（防御）→ 合并
        entryOf("b.txt", StatusKind::Modified, true),
        entryOf("m/u.bin", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Flat);
    const auto& rows = model.rows();
    // 已暂存组头 + 2 文件 + 未暂存组头 + 2 文件 = 6
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(6));
    const ChangeListRow* hs = headerOf(rows, ChangeListRow::Group::Staged);
    const ChangeListRow* hu = headerOf(rows, ChangeListRow::Group::Unstaged);
    DOCTEST_REQUIRE(hs != nullptr);
    DOCTEST_REQUIRE(hu != nullptr);
    DOCTEST_CHECK_EQ(rows[0].group, ChangeListRow::Group::Staged);  // 已暂存组在前
    DOCTEST_CHECK_EQ(hs->childCount, static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(hu->childCount, static_cast<size_t>(2));
    // 跨组不去重（D6-A 行为变更点，PRD F-12 验收原文）
    size_t stagedA = 0, unstagedA = 0;
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::File && r.relPath == "a.txt") {
            if (r.group == ChangeListRow::Group::Staged) {
                ++stagedA;
            } else {
                ++unstagedA;
            }
        }
    }
    DOCTEST_CHECK_EQ(stagedA, static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(unstagedA, static_cast<size_t>(1));
    // 组内去重：m/u.bin（untracked 文件）只一行且在未暂存组（无独立组——D6-A）
    size_t u = 0;
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::File && r.relPath == "m/u.bin") ++u;
    }
    DOCTEST_CHECK_EQ(u, static_cast<size_t>(1));
    return true;
})

REGISTER_TEST(gitChangeListModelUntrackedPureFileRows, "裁决④：untracked 目录条目不再产 Dir 行——自动枚举后文件行并入派生带 U 徽标；嵌套子目录=派生目录行（纯结构装置无徽标，原则令）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("docs/", StatusKind::Untracked, false),
        entryOf("普通.txt", StatusKind::Modified, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    // 枚举落地前：docs/ 条目不产任何行（文件夹原则令+裁决④）——组头 + 普通.txt
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Dir),
                     static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(findRow(model.rows(), ChangeListRow::Kind::File, "普通.txt") != nullptr,
                     true);
    // 自动枚举回包：文件行并入同源派生（U 徽标）、嵌套子目录=派生 lazy 目录行
    WalkResult r;
    r.ok = true;
    r.entries = {
        {"指南.md", "docs/指南.md", EntryKind::File},
        {"sub", "docs/sub", EntryKind::Directory},
    };
    model.applyUntrackedChildren("docs", r);
    const auto& rows = model.rows();
    // 组头 + docs/(派生结构目录) + 指南.md(U) + sub/(lazy) + 普通.txt = 5
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(5));
    const ChangeListRow* d = findRow(rows, ChangeListRow::Kind::Dir, "docs");
    DOCTEST_REQUIRE(d != nullptr);
    DOCTEST_CHECK_EQ(d->name, std::string("docs/"));
    DOCTEST_CHECK_FALSE(d->status.has_value());  // 原则令：目录行不带状态徽标
    DOCTEST_CHECK_FALSE(d->lazy);                // tracked 式派生目录（默认展开）
    DOCTEST_CHECK_EQ(d->childCount, static_cast<size_t>(1));  // 子树文件行=结构信息
    DOCTEST_CHECK_EQ(d->group, ChangeListRow::Group::Unstaged);  // 归未暂存组（D6-A）
    // 枚举文件行 = U 徽标（只落文件行）
    const ChangeListRow* f = findRow(rows, ChangeListRow::Kind::File, "docs/指南.md");
    DOCTEST_REQUIRE(f != nullptr);
    DOCTEST_REQUIRE(f->status.has_value());
    DOCTEST_CHECK_EQ(*f->status, StatusKind::Untracked);
    // 嵌套子目录：lazy 派生目录行（默认收起，展开走懒枚举；无徽标无计数——R4）
    const ChangeListRow* sub = findRow(rows, ChangeListRow::Kind::Dir, "docs/sub");
    DOCTEST_REQUIRE(sub != nullptr);
    DOCTEST_CHECK_EQ(sub->name, std::string("sub/"));
    DOCTEST_CHECK(sub->lazy);
    DOCTEST_CHECK_FALSE(sub->status.has_value());
    DOCTEST_CHECK_EQ(sub->childCount, static_cast<size_t>(0));
    // 平铺形态同源：文件行混排（无 untracked 目录行）
    model.setViewMode(ChangeViewMode::Flat);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Dir),
                     static_cast<size_t>(1));  // 仅 sub/ lazy 锚
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::File, "docs/指南.md") != nullptr);
    return true;
})

REGISTER_TEST(gitChangeListModelBitmapModeIndependence, "树/平铺展开位图互不干扰（设计 §6.3）：各自记忆、切换互不覆盖（need:299 嵌套 lazy 目录口径）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("docs/", StatusKind::Untracked, false),
        entryOf("src/a.cpp", StatusKind::Modified, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    // 枚举 docs：嵌套 lazy 子目录 sub（裁决④后 lazy 边界只在嵌套层）
    WalkResult r;
    r.ok = true;
    r.entries = {
        {"a.md", "docs/a.md", EntryKind::File},
        {"sub", "docs/sub", EntryKind::Directory},
    };
    model.applyUntrackedChildren("docs", r);
    // 树态展开 sub（嵌套 lazy）→ Loading 占位行出现
    model.setExpanded("docs/sub", true);
    model.markUntrackedLoading("docs/sub");
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Loading),
                     static_cast<size_t>(1));
    // 切平铺：平铺位图为空 → sub 行在但无 Loading 行
    model.setViewMode(ChangeViewMode::Flat);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Loading),
                     static_cast<size_t>(0));
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::Dir, "docs/sub") != nullptr);
    // 平铺态展开 → 平铺位图记录，平铺出 Loading
    model.setExpanded("docs/sub", true);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Loading),
                     static_cast<size_t>(1));
    // 切回树：树位图仍在（Loading 回现）——两位图互不覆盖
    model.setViewMode(ChangeViewMode::Tree);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Loading),
                     static_cast<size_t>(1));
    // 树态折叠 sub：平铺态不受影响
    model.setExpanded("docs/sub", false);
    model.setViewMode(ChangeViewMode::Flat);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Loading),
                     static_cast<size_t>(1));
    return true;
})

REGISTER_TEST(gitChangeListModelCollapseThresholdBoundary, "组头默认折叠阈值 200|201 边界（POC②）：200 展开 / 201 两组头收起；手动展开不被刷新重折叠", {
    StatusSnapshot snap200;
    for (int i = 0; i < 100; ++i) {
        snap200.entries.push_back(entryOf("s" + std::to_string(i) + ".txt",
                                          StatusKind::Modified, true));
        snap200.entries.push_back(entryOf("u" + std::to_string(i) + ".txt",
                                          StatusKind::Modified, false));
    }
    GitChangeListModel m200;
    m200.rebuild(snap200, ChangeViewMode::Flat);
    DOCTEST_CHECK_EQ(countKind(m200.rows(), ChangeListRow::Kind::File),
                     static_cast<size_t>(200));  // 组头默认展开：文件行全出
    DOCTEST_CHECK(!m200.isGroupCollapsed(ChangeListRow::Group::Staged));
    DOCTEST_CHECK(!m200.isGroupCollapsed(ChangeListRow::Group::Unstaged));
    // +1 条（201）→ 两组头默认收起（PRD N2 >200 折叠）
    StatusSnapshot snap201 = snap200;
    snap201.entries.push_back(entryOf("extra.txt", StatusKind::Modified, false));
    GitChangeListModel m201;
    m201.rebuild(snap201, ChangeViewMode::Flat);
    DOCTEST_CHECK_EQ(m201.rows().size(), static_cast<size_t>(2));  // 仅两组头
    DOCTEST_CHECK(m201.isGroupCollapsed(ChangeListRow::Group::Staged));
    DOCTEST_CHECK(m201.isGroupCollapsed(ChangeListRow::Group::Unstaged));
    // 用户手动展开未暂存组后刷新（仍 >200）：触碰过的组不重折叠（折叠态记忆）
    m201.setGroupCollapsed(ChangeListRow::Group::Unstaged, false);
    DOCTEST_CHECK_EQ(countKind(m201.rows(), ChangeListRow::Kind::File),
                     static_cast<size_t>(101));
    m201.rebuild(snap201, ChangeViewMode::Flat);
    DOCTEST_CHECK(m201.isGroupCollapsed(ChangeListRow::Group::Staged));
    DOCTEST_CHECK(!m201.isGroupCollapsed(ChangeListRow::Group::Unstaged));
    return true;
})

REGISTER_TEST(gitChangeListModelLazyInjectAndTruncate, "自动枚举注入 + >500 截断尾行 + pending/失败/重试状态机（F-04/POC①；裁决④顶层免展开）", {
    StatusSnapshot snap;
    snap.entries = {entryOf("big/", StatusKind::Untracked, false)};
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    // 裁决④：顶层条目自动枚举——pending 无视展开（不产 Dir 行，无 Loading 行）
    auto pending = model.pendingUntrackedDirs();
    DOCTEST_REQUIRE_EQ(pending.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(pending[0], std::string("big"));
    model.markUntrackedLoading("big");
    DOCTEST_CHECK_EQ(model.pendingUntrackedDirs().size(), static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Loading),
                     static_cast<size_t>(0));  // 顶层无目录行，Loading 无处挂
    // walk 回包：503 文件 + 2 子目录 → 前 500 文件并入派生 + 截断尾行 remain 3
    // + 2 嵌套 lazy 目录行
    WalkResult result;
    result.ok = true;
    for (int i = 0; i < 503; ++i) {
        const std::string name = "f" + std::to_string(i) + ".txt";
        result.entries.push_back({name, "big/" + name, EntryKind::File});
    }
    result.entries.push_back({"s1", "big/s1", EntryKind::Directory});
    result.entries.push_back({"s2", "big/s2", EntryKind::Directory});
    model.applyUntrackedChildren("big", result);
    const auto& rows = model.rows();
    // 组头 + big/(派生) + s1/ + s2/ + 500 文件 + 截断尾行 = 505
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(505));
    const ChangeListRow* tr = nullptr;
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::Truncate) tr = &r;
    }
    DOCTEST_REQUIRE(tr != nullptr);
    DOCTEST_CHECK_EQ(tr->remainCount, static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(tr->depth, 0);  // 裁决④：顶层无目录行可挂——组尾 depth 0
    DOCTEST_CHECK_EQ(tr->dirRel, std::string("big"));
    // 注入文件行 = U 徽标（枚举结果全量取用——非 ignored 即 untracked，POC①）
    const ChangeListRow* f0 = findRow(rows, ChangeListRow::Kind::File, "big/f0.txt");
    DOCTEST_REQUIRE(f0 != nullptr);
    DOCTEST_REQUIRE(f0->status.has_value());
    DOCTEST_CHECK_EQ(*f0->status, StatusKind::Untracked);
    // 组头计数（原则令：只算文件行）：500 可见文件（截断余量不计）
    DOCTEST_CHECK_EQ(headerOf(rows, ChangeListRow::Group::Unstaged)->childCount,
                     static_cast<size_t>(500));
    // 嵌套子目录续 lazy：展开 s1 → pending 指向 big/s1（展开才枚举）
    model.setExpanded("big/s1", true);
    pending = model.pendingUntrackedDirs();
    DOCTEST_REQUIRE_EQ(pending.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(pending[0], std::string("big/s1"));
    // 失败链路：行内错误 + 目录保持展开可重试；重试后回 pending
    model.markUntrackedLoading("big/s1");
    model.markUntrackedFailed("big/s1", "disk error");
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Error),
                     static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(model.pendingUntrackedDirs().size(), static_cast<size_t>(0));
    model.requestRetry("big/s1");
    DOCTEST_CHECK_EQ(model.pendingUntrackedDirs().size(), static_cast<size_t>(1));
    // 空目录竞态（walk ok 0 条）→ 缓存空、无错误行（行消失归下次刷新）
    WalkResult empty;
    empty.ok = true;
    model.applyUntrackedChildren("big/s1", empty);
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::Dir, "big/s1") != nullptr);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Error),
                     static_cast<size_t>(0));
    DOCTEST_CHECK_EQ(model.pendingUntrackedDirs().size(), static_cast<size_t>(0));
    // 顶层枚举失败：组尾错误行（无目录行可挂——裁决④）；重试清错误后因缓存
    // 已在（先前成功枚举）不重投——pending 排除已缓存目录
    model.markUntrackedLoading("big");
    model.markUntrackedFailed("big", "boom");
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::Error),
                     static_cast<size_t>(1));
    DOCTEST_CHECK(model.pendingUntrackedDirs().empty());
    model.requestRetry("big");
    DOCTEST_CHECK_EQ(model.pendingUntrackedDirs().size(), static_cast<size_t>(0));
    return true;
})

REGISTER_TEST(gitChangeListModelHeaderCountWithExpandedChildren, "组头计数（原则令：只算文件行）：枚举并入文件计入、未枚举目录不计；折叠组计数保留", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("u1.txt", StatusKind::Untracked, false),
        entryOf("docs/", StatusKind::Untracked, false),
        entryOf("staged1.txt", StatusKind::Modified, true),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Unstaged)
                         ->childCount,
                     static_cast<size_t>(1));  // 只算文件行：目录条目未枚举不计（原则令）
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Staged)
                         ->childCount,
                     static_cast<size_t>(1));
    // docs 枚举 2 子文件并入 → 未暂存计数 = 1 + 2 = 3；已暂存组不受影响
    WalkResult r;
    r.ok = true;
    r.entries = {
        {"a.md", "docs/a.md", EntryKind::File},
        {"b.md", "docs/b.md", EntryKind::File},
    };
    model.applyUntrackedChildren("docs", r);
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Unstaged)
                         ->childCount,
                     static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Staged)
                         ->childCount,
                     static_cast<size_t>(1));
    // 折叠组头：子行不出、计数保留（设计 §6.4）；已暂存组未折叠，组头+文件行在
    model.setGroupCollapsed(ChangeListRow::Group::Unstaged, true);
    DOCTEST_CHECK_EQ(model.rows().size(), static_cast<size_t>(3));
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Unstaged)
                         ->childCount,
                     static_cast<size_t>(3));
    return true;
})

REGISTER_TEST(gitChangeListModelLazyChildCountTruth, "bug#292 D4：lazy 嵌套目录枚举前=0（不显）；枚举后父目录子树计数如实（1→2）；收起不回退计数（组头/目录计数不随展开位跳变）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("top.txt", StatusKind::Modified, false),
        entryOf("0_untracked/", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    WalkResult r;
    r.ok = true;
    r.entries = {
        {"inner1.txt", "0_untracked/inner1.txt", EntryKind::File},
        {"nested", "0_untracked/nested", EntryKind::Directory},
    };
    model.applyUntrackedChildren("0_untracked", r);
    // 枚举前：组头=top.txt+inner1.txt=2；0_untracked 子树=inner1 1 + 嵌套未枚举 0
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Unstaged)->childCount,
                     static_cast<size_t>(2));
    const ChangeListRow* dir = findRow(model.rows(), ChangeListRow::Kind::Dir, "0_untracked");
    DOCTEST_CHECK(dir != nullptr);
    if (dir != nullptr) DOCTEST_CHECK_EQ(dir->childCount, static_cast<size_t>(1));
    const ChangeListRow* lazy =
        findRow(model.rows(), ChangeListRow::Kind::Dir, "0_untracked/nested");
    DOCTEST_CHECK(lazy != nullptr);
    if (lazy != nullptr) {
        DOCTEST_CHECK(lazy->lazy);
        DOCTEST_CHECK_EQ(lazy->childCount, static_cast<size_t>(0));  // 不显
    }
    // 展开并枚举嵌套 → 组头计入新枚举文件（设计 §4.3）；父目录子树计数如实=2
    model.setExpanded("0_untracked/nested", true);
    WalkResult rr;
    rr.ok = true;
    rr.entries = {{"nested_file.txt", "0_untracked/nested/nested_file.txt", EntryKind::File}};
    model.applyUntrackedChildren("0_untracked/nested", rr);
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Unstaged)->childCount,
                     static_cast<size_t>(3));
    dir = findRow(model.rows(), ChangeListRow::Kind::Dir, "0_untracked");
    DOCTEST_CHECK(dir != nullptr);
    if (dir != nullptr) DOCTEST_CHECK_EQ(dir->childCount, static_cast<size_t>(2));
    // 收起：已枚举事实不变 → 组头/父目录计数不回退（bug#292 D4 无理由跳变）
    model.setExpanded("0_untracked/nested", false);
    DOCTEST_CHECK_EQ(headerOf(model.rows(), ChangeListRow::Group::Unstaged)->childCount,
                     static_cast<size_t>(3));
    dir = findRow(model.rows(), ChangeListRow::Kind::Dir, "0_untracked");
    DOCTEST_CHECK(dir != nullptr);
    if (dir != nullptr) DOCTEST_CHECK_EQ(dir->childCount, static_cast<size_t>(2));
    return true;
})

REGISTER_TEST(gitChangeListModelRefreshKeepsBitmaps, "刷新重建保位图/缓存（F-09）：展开/组折叠/lazy 缓存跨 rebuild 保留；条目消失=行消失", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("src/a.cpp", StatusKind::Modified, false),
        entryOf("docs/", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    model.setExpanded("src", true);
    model.setExpanded("docs", true);
    model.setGroupCollapsed(ChangeListRow::Group::Staged, false);
    WalkResult r;
    r.ok = true;
    r.entries = {{"g.md", "docs/g.md", EntryKind::File}};
    model.applyUntrackedChildren("docs", r);
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::File),
                     static_cast<size_t>(2));  // a.cpp + g.md
    // 刷新：快照更新（新文件 b.cpp），位图/缓存应保留
    StatusSnapshot snap2;
    snap2.entries = {
        entryOf("src/a.cpp", StatusKind::Modified, false),
        entryOf("src/b.cpp", StatusKind::Modified, false),
        entryOf("docs/", StatusKind::Untracked, false),
    };
    model.rebuild(snap2, ChangeViewMode::Tree);
    const auto& rows = model.rows();
    DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "src/a.cpp") != nullptr);
    DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "src/b.cpp") != nullptr);
    DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "docs/g.md") != nullptr);
    DOCTEST_CHECK(model.isExpanded("src"));
    DOCTEST_CHECK(model.isExpanded("docs"));
    DOCTEST_CHECK_EQ(model.pendingUntrackedDirs().size(),
                     static_cast<size_t>(0));  // 缓存生效不需重投
    // 目录条目消失（status 不再含 docs/）→ 行消失（位图残留无害）
    StatusSnapshot snap3;
    snap3.entries = {entryOf("src/a.cpp", StatusKind::Modified, false)};
    model.rebuild(snap3, ChangeViewMode::Tree);
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::Dir, "docs") == nullptr);
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::File, "docs/g.md") == nullptr);
    DOCTEST_CHECK(model.isExpanded("src"));  // 位图仍保留
    return true;
})

REGISTER_TEST(gitChangeListModelFlatDirAggregation, "平铺形态：目录段聚合排序（同目录相邻）+ 枚举并入 untracked 文件行混排（裁决④：无目录条目行）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("zdir/late.txt", StatusKind::Modified, false),
        entryOf("adir/early.txt", StatusKind::Modified, false),
        entryOf("adir/mid.txt", StatusKind::New, false),
        entryOf("root.txt", StatusKind::Modified, false),
        entryOf("adir/", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Flat);
    {
        const auto& rows = model.rows();
        // 枚举前：adir/ 条目不产行——组头 + 4 文件
        DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(5));
        DOCTEST_CHECK_EQ(rows[1].relPath, std::string("adir/early.txt"));
        DOCTEST_CHECK_EQ(rows[2].relPath, std::string("adir/mid.txt"));  // 同目录相邻
        DOCTEST_CHECK_EQ(rows[3].relPath, std::string("root.txt"));
        DOCTEST_CHECK_EQ(rows[4].relPath, std::string("zdir/late.txt"));
    }
    // adir 枚举回包 → 文件行按路径字节序混排（无独立节、无目录条目行）
    WalkResult r;
    r.ok = true;
    r.entries = {{"aa.txt", "adir/aa.txt", EntryKind::File}};
    model.applyUntrackedChildren("adir", r);
    const auto& rows = model.rows();
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(6));
    DOCTEST_CHECK_EQ(rows[1].relPath, std::string("adir/aa.txt"));  // aa < early
    DOCTEST_REQUIRE(rows[1].status.has_value());
    DOCTEST_CHECK_EQ(*rows[1].status, StatusKind::Untracked);  // U 徽标（文件行）
    DOCTEST_CHECK_EQ(rows[2].relPath, std::string("adir/early.txt"));
    DOCTEST_CHECK_EQ(rows[3].relPath, std::string("adir/mid.txt"));
    DOCTEST_CHECK_EQ(rows[4].relPath, std::string("root.txt"));
    DOCTEST_CHECK_EQ(rows[5].relPath, std::string("zdir/late.txt"));
    return true;
})

REGISTER_TEST(gitChangeListModelFilterTree, "列表过滤·树形态（need:299 §4.9）：命中文件+祖先链自动临时展开；清空=恢复原展开位图（不写记忆）；组头计数=命中文件行数；lazy 目录自身路径命中保留", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("src/a.cpp", StatusKind::Modified, false),
        entryOf("src/deep/x.cpp", StatusKind::Modified, false),
        entryOf("docs/", StatusKind::Untracked, false),
        entryOf("root.txt", StatusKind::Modified, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    WalkResult r;
    r.ok = true;
    r.entries = {
        {"指南.md", "docs/指南.md", EntryKind::File},
        {"sub", "docs/sub", EntryKind::Directory},
    };
    model.applyUntrackedChildren("docs", r);
    // 预置折叠记忆：折叠 src（过滤展开不得写入此位图）
    model.setExpanded("src", false);
    DOCTEST_CHECK_FALSE(model.isExpanded("src"));
    // 过滤「deep」：命中 src/deep/x.cpp → 祖先链 src/deep 临时展开；折叠记忆不动
    model.setFilter("deep");
    {
        const auto& rows = model.rows();
        // 组头(1) + src/deep/(单链压缩：src 唯一子链 deep 无直接文件，clue#199)
        // + x.cpp = 3 行；docs/指南.md 不命中「deep」、root.txt 不命中 → 不显示
        DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(3));
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::Dir, "src") == nullptr);
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::Dir, "src/deep") != nullptr);
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "src/deep/x.cpp") != nullptr);
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "src/a.cpp") == nullptr);
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "root.txt") == nullptr);
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "docs/指南.md") == nullptr);
        DOCTEST_CHECK_EQ(headerOf(rows, ChangeListRow::Group::Unstaged)->childCount,
                         static_cast<size_t>(1));  // 命中文件行数
    }
    // 折叠记忆未被过滤写入：清空恢复原位图（src 收起、docs/指南.md 回归）
    model.setFilter({});
    {
        const auto& rows = model.rows();
        DOCTEST_CHECK_FALSE(model.isExpanded("src"));
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "src/deep/x.cpp") == nullptr);
        DOCTEST_CHECK(findRow(rows, ChangeListRow::Kind::File, "docs/指南.md") != nullptr);
        DOCTEST_CHECK_EQ(headerOf(rows, ChangeListRow::Group::Unstaged)->childCount,
                         static_cast<size_t>(4));  // 3 tracked + 1 枚举并入
    }
    // 过滤「sub」：lazy 目录自身路径命中 → 目录行保留（可点展开；过滤不触发枚举）
    model.setFilter("sub");
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::Dir, "docs/sub") != nullptr);
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::File, "docs/指南.md") == nullptr);
    model.setFilter({});
    // 过滤大小写不敏感（ASCII 折叠）：「DOCS」命中 docs/ 指南.md 路径
    model.setFilter("DOCS");
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::File, "docs/指南.md") != nullptr);
    model.setFilter({});
    return true;
})

REGISTER_TEST(gitChangeListModelFilterFlat, "列表过滤·平铺形态（need:299 §4.9）：行级匹配；0 命中=组头+行集临时隐藏；命中文件并入档（untracked 同口径）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("staged/a.txt", StatusKind::Modified, true),
        entryOf("unst/b.txt", StatusKind::Modified, false),
        entryOf("docs/", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Flat);
    WalkResult r;
    r.ok = true;
    r.entries = {{"笔记.md", "docs/笔记.md", EntryKind::File}};
    model.applyUntrackedChildren("docs", r);
    // 过滤「unst」：未暂存 1 命中；已暂存 0 命中=组头临时隐藏（非折叠语义）
    model.setFilter("unst");
    {
        const auto& rows = model.rows();
        DOCTEST_CHECK(headerOf(rows, ChangeListRow::Group::Staged) == nullptr);
        DOCTEST_CHECK(headerOf(rows, ChangeListRow::Group::Unstaged) != nullptr);
        DOCTEST_CHECK_EQ(countKind(rows, ChangeListRow::Kind::File),
                         static_cast<size_t>(1));
        DOCTEST_CHECK_EQ(headerOf(rows, ChangeListRow::Group::Unstaged)->childCount,
                         static_cast<size_t>(1));
    }
    // 过滤「docs」：枚举并入的 untracked 文件行命中（同源派生同口径）
    model.setFilter("docs");
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::File, "docs/笔记.md") != nullptr);
    // 过滤「zzz」：全组 0 命中=行序列空（视图据此显过滤空态，与干净空态区分）
    model.setFilter("zzz");
    DOCTEST_CHECK(model.rows().empty());
    // 清空恢复全量
    model.setFilter({});
    DOCTEST_CHECK_EQ(countKind(model.rows(), ChangeListRow::Kind::File),
                     static_cast<size_t>(3));
    return true;
})

REGISTER_TEST(gitChangeListModelFileStatusApi, "fileStatus：不依赖可见行集——折叠目录/未展开 lazy 缓存下的文件仍可取状态（选中行徽标/U 判据）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("src/a.cpp", StatusKind::Modified, false),
        entryOf("docs/", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    WalkResult r;
    r.ok = true;
    r.entries = {{"g.md", "docs/g.md", EntryKind::File}};
    model.applyUntrackedChildren("docs", r);
    // 折叠 docs 派生目录 → g.md 行不可见，但 fileStatus 仍可取（bug296 判据链）
    model.setExpanded("docs", false);
    DOCTEST_CHECK(findRow(model.rows(), ChangeListRow::Kind::File, "docs/g.md") == nullptr);
    const auto st = model.fileStatus("docs/g.md");
    DOCTEST_REQUIRE(st.has_value());
    DOCTEST_CHECK_EQ(*st, StatusKind::Untracked);
    const auto st2 = model.fileStatus("src/a.cpp");
    DOCTEST_REQUIRE(st2.has_value());
    DOCTEST_CHECK_EQ(*st2, StatusKind::Modified);
    DOCTEST_CHECK_FALSE(model.fileStatus("不存在.txt").has_value());
    return true;
})

REGISTER_TEST(gitChangeListModelRealRepoStatus, "真仓 status 全链（原 real-repo 用例改挂）：修改/新增/删除/重命名四态 + 双侧两组各一行", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = test_support::makeTempDir("devdesk_git_changelist_", "real");
        git_fixture::initRepo(dir);
        // 三文件内容互异：同内容会让 rename 检测跨文件错配（批次二遗留口径）
        writeTextFile(dir / "keep.txt", "v1\n");
        writeTextFile(dir / "删除我.txt", "del-1\n");
        writeTextFile(dir / "旧名.txt", "ren-1\n");
        git_fixture::commitAll(dir, {{"keep.txt", "v1\n"},
                                     {"删除我.txt", "del-1\n"},
                                     {"旧名.txt", "ren-1\n"}});
        // 工作区改动：modified / deleted / untracked
        writeTextFile(dir / "keep.txt", "v2\n");
        std::error_code ec;
        fs::remove(dir / "删除我.txt", ec);
        writeTextFile(dir / "新文件.txt", "new\n");
        // staged rename：index 删旧名加新名（内容同 → RENAMES 检出 R）
        {
            git_repository* repo = nullptr;
            git_fixture::checkRc(git_repository_open_ext(
                &repo, platform::git::pathToUtf8(dir).c_str(),
                GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr), "open");
            git_index* index = nullptr;
            git_fixture::checkRc(git_repository_index(&index, repo), "index");
            git_fixture::checkRc(git_index_remove_bypath(index, "旧名.txt"),
                                 "remove");
            git_index_entry e{};
            e.path = "新名.txt";
            e.mode = GIT_FILEMODE_BLOB;
            git_oid oid{};
            git_fixture::checkRc(git_blob_create_from_buffer(
                &oid, repo, "ren-1\n", 6), "blob");
            e.id = oid;
            git_fixture::checkRc(git_index_add(index, &e), "add");
            git_fixture::checkRc(git_index_write(index), "write");
            git_index_free(index);
            git_repository_free(repo);
            // workdir 同步去掉旧名（git mv 语义），避免残留配成第二组 rename
            fs::remove(dir / "旧名.txt", ec);
        }

        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        auto snapshot = opened.value().status();
        DOCTEST_REQUIRE(snapshot.isOk());
        if (snapshot.isFail()) return false;

        GitChangeListModel model;
        model.rebuild(snapshot.value(), ChangeViewMode::Flat);
        const auto& rows = model.rows();
        bool sawModified = false, sawDeleted = false, sawUntracked = false,
             sawRenamed = false;
        for (const auto& r : rows) {
            if (r.kind != ChangeListRow::Kind::File || !r.status.has_value()) {
                continue;
            }
            if (r.relPath == "keep.txt" && *r.status == StatusKind::Modified) {
                sawModified = true;
            }
            if (r.relPath == "删除我.txt" && *r.status == StatusKind::Deleted) {
                sawDeleted = true;
            }
            if (r.relPath == "新文件.txt" && *r.status == StatusKind::Untracked) {
                sawUntracked = true;  // untracked 文件并入未暂存组（D6-A）
                DOCTEST_CHECK_EQ(r.group, ChangeListRow::Group::Unstaged);
            }
            if (r.relPath == "新名.txt" && *r.status == StatusKind::Renamed) {
                sawRenamed = true;
                DOCTEST_CHECK_EQ(r.oldPath, "旧名.txt");
            }
        }
        DOCTEST_CHECK(sawModified);
        DOCTEST_CHECK(sawDeleted);
        DOCTEST_CHECK(sawUntracked);
        DOCTEST_CHECK(sawRenamed);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})
