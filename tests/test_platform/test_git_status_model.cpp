// tests/test_platform/test_git_status_model.cpp
// git 管理器重设计（need:291）：变更列表新模型 GitChangeListModel 消费面——
//   ① rebuild 分桶（Staged/Unstaged 两组头）+ 组内去重、跨组不去重（D6-A 行为
//      变更点：旧 GitChangesModel 双侧合并一条 → 现两组各一行）+ 组内字节序；
//   ② 重命名旧→新：R 行 oldPath 通道（组内保留、跨组不合并）；
//   ③ 组头文案「已暂存 (N)」/「未暂存 (N)」（设计 §4.3）；徽标字母映射（A/M/D/R/U）；
//   ④ GitColors 四态着色浅/深双套（不动）。
// 纯函数面用构造快照直打；真仓 platform 层 status 用例保留回归（模型无关）。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "module_git/git_change_list_model.h"
#include "module_git/git_colors.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using module::git::ChangeListRow;
using module::git::GitChangeListModel;
using module::git::GitColors;
using module::git::gitColors;
using module::git::statusBadgeLetter;
using module::git::ChangeViewMode;
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

// 组内文件行数（组头/目录行不计；组头计数=组内文件行数，设计 §4.3）
size_t countGroupFiles(const std::vector<ChangeListRow>& rows,
                       ChangeListRow::Group group) {
    size_t n = 0;
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::File && r.group == group) ++n;
    }
    return n;
}

// 组内首个匹配 relPath 的文件行（找不到返回 nullptr）
const ChangeListRow* findGroupFile(const std::vector<ChangeListRow>& rows,
                                   ChangeListRow::Group group,
                                   const std::string& relPath) {
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::File && r.group == group &&
            r.relPath == relPath) {
            return &r;
        }
    }
    return nullptr;
}

// eui::Color = {float r,g,b,a} 聚合体，无 operator==——逐分量比较
bool colorSame(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

} // namespace

REGISTER_TEST(gitStatusModelGroupsAndDedup, "rebuild: 已暂存/未暂存分桶 + 组内去重跨组不去重（D6-A）+ 组内字节序", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("z.txt", StatusKind::Modified, false),
        entryOf("a.txt", StatusKind::Modified, true),
        entryOf("a.txt", StatusKind::Modified, false),  // 同路径双侧 → 两组各一行（不去重）
        entryOf("m/b.bin", StatusKind::Untracked, false),
        entryOf("新文件.md", StatusKind::New, false),
        entryOf("删除.txt", StatusKind::Deleted, true),
        entryOf("z.txt", StatusKind::Modified, false),  // 组内重复 → 合并一条
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    const auto& rows = model.rows();
    // 分桶：staged 2 文件（a.txt/删除.txt）、unstaged 4 文件（z.txt/a.txt/m/b.bin/新文件.md）
    DOCTEST_CHECK_EQ(countGroupFiles(rows, ChangeListRow::Group::Staged),
                     static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(countGroupFiles(rows, ChangeListRow::Group::Unstaged),
                     static_cast<size_t>(4));
    // 跨组不去重（D6-A 行为变更点：旧模型双侧合并一条 → 两组各一行）
    DOCTEST_CHECK(findGroupFile(rows, ChangeListRow::Group::Staged, "a.txt") != nullptr);
    DOCTEST_CHECK(findGroupFile(rows, ChangeListRow::Group::Unstaged, "a.txt") != nullptr);
    // 组内去重：z.txt 双条目只出一行
    size_t zCount = 0;
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::File &&
            r.group == ChangeListRow::Group::Unstaged && r.relPath == "z.txt") {
            ++zCount;
        }
    }
    DOCTEST_CHECK_EQ(zCount, static_cast<size_t>(1));
    // 两组头各一（组头计数=组内文件行数，断言见 gitStatusModelHeaderAndBadge）
    size_t headers = 0;
    for (const auto& r : rows) {
        if (r.kind == ChangeListRow::Kind::GroupHeader) ++headers;
    }
    DOCTEST_CHECK_EQ(headers, static_cast<size_t>(2));
    // 组内输出序 = 路径 UTF-8 字节序（map 序）：根层文件 depth 0 过滤目录派生行，
    // '删'U+5220→E5… 与 '新'U+65B0→E6… 的字节序语义同旧模型
    std::vector<std::string> stagedOrder;
    std::vector<std::string> unstagedRootOrder;
    for (const auto& r : rows) {
        if (r.kind != ChangeListRow::Kind::File) continue;
        if (r.group == ChangeListRow::Group::Staged) {
            stagedOrder.push_back(r.relPath);
        } else if (r.depth == 0) {
            unstagedRootOrder.push_back(r.relPath);
        }
    }
    DOCTEST_CHECK_EQ(stagedOrder.size(), static_cast<size_t>(2));
    DOCTEST_CHECK(stagedOrder[0] == "a.txt");       // 'a' 0x61 < '删' E5
    DOCTEST_CHECK(stagedOrder[1] == "删除.txt");
    const std::vector<std::string> wantRoot = {"a.txt", "z.txt", "新文件.md"};
    DOCTEST_REQUIRE_EQ(unstagedRootOrder.size(), wantRoot.size());
    for (size_t i = 0; i < unstagedRootOrder.size() && i < wantRoot.size(); ++i) {
        DOCTEST_CHECK_EQ(unstagedRootOrder[i], wantRoot[i]);
    }
    // 徽标随行保留：staged Deleted / unstaged Untracked（嵌套文件，树形态 depth>0）
    const ChangeListRow* del =
        findGroupFile(rows, ChangeListRow::Group::Staged, "删除.txt");
    DOCTEST_REQUIRE(del != nullptr);
    if (del != nullptr) DOCTEST_CHECK_EQ(*del->status, StatusKind::Deleted);
    const ChangeListRow* bin =
        findGroupFile(rows, ChangeListRow::Group::Unstaged, "m/b.bin");
    DOCTEST_REQUIRE(bin != nullptr);
    if (bin != nullptr) DOCTEST_CHECK_EQ(*bin->status, StatusKind::Untracked);
    return true;
})

REGISTER_TEST(gitStatusModelRenameOldToNew, "rebuild: 重命名旧→新 R 行 oldPath 通道 + 跨组不合并（D6-A）", {
    StatusSnapshot snap;
    // staged rename（head→index 侧）+ unstaged 同路径修改 → 两组各一行（不合并）
    snap.entries = {
        entryOf("docs/接口规范.md", StatusKind::Renamed, true, "docs/说明文档.md"),
        entryOf("docs/接口规范.md", StatusKind::Modified, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    const auto& rows = model.rows();
    DOCTEST_REQUIRE_EQ(countGroupFiles(rows, ChangeListRow::Group::Staged),
                       static_cast<size_t>(1));
    DOCTEST_REQUIRE_EQ(countGroupFiles(rows, ChangeListRow::Group::Unstaged),
                       static_cast<size_t>(1));
    const ChangeListRow* stagedRow =
        findGroupFile(rows, ChangeListRow::Group::Staged, "docs/接口规范.md");
    DOCTEST_REQUIRE(stagedRow != nullptr);
    if (stagedRow != nullptr) {
        DOCTEST_CHECK_EQ(*stagedRow->status, StatusKind::Renamed);
        DOCTEST_CHECK_EQ(stagedRow->oldPath, "docs/说明文档.md");  // R 行旧路径通道（tooltip 用）
    }
    const ChangeListRow* unstagedRow =
        findGroupFile(rows, ChangeListRow::Group::Unstaged, "docs/接口规范.md");
    DOCTEST_REQUIRE(unstagedRow != nullptr);
    if (unstagedRow != nullptr) {
        DOCTEST_CHECK_EQ(*unstagedRow->status, StatusKind::Modified);
        DOCTEST_CHECK(unstagedRow->oldPath.empty());  // 副行不带旧名
    }
    return true;
})

REGISTER_TEST(gitStatusModelHeaderAndBadge, "组头行文案计数（已暂存/未暂存 N）+ 徽标字母映射（A/M/D/R/U）", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("s1.txt", StatusKind::Modified, true),
        entryOf("s2.txt", StatusKind::Deleted, true),
        entryOf("u1.txt", StatusKind::Untracked, false),
    };
    GitChangeListModel model;
    model.rebuild(snap, ChangeViewMode::Tree);
    const auto& rows = model.rows();
    std::string stagedHeader;
    std::string unstagedHeader;
    for (const auto& r : rows) {
        if (r.kind != ChangeListRow::Kind::GroupHeader) continue;
        if (r.group == ChangeListRow::Group::Staged) stagedHeader = r.name;
        if (r.group == ChangeListRow::Group::Unstaged) unstagedHeader = r.name;
    }
    // 设计 §4.3：计数=组内文件行数（untracked 并入未暂存组，无独立组头——D6-A）
    DOCTEST_CHECK_EQ(stagedHeader, std::string("已暂存 (2)"));
    DOCTEST_CHECK_EQ(unstagedHeader, std::string("未暂存 (1)"));
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::New), 'A');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Modified), 'M');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Deleted), 'D');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Renamed), 'R');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Untracked), 'U');
    return true;
})

REGISTER_TEST(gitStatusModelRealRepoStatus, "真仓 status（platform 层回归，模型无关）：修改/新增/删除/重命名四态全链", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = test_support::makeTempDir("devdesk_git_status_", "real");
        git_fixture::initRepo(dir);
        // 三文件内容互异：同内容会让 rename 检测跨文件错配（删除我.txt 曾与
        // 同内容的 untracked 残留 旧名.txt 配成 rename → sawDeleted 恒红，批次二遗留）
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
        // staged rename：git2 API——index 删旧名加新名（内容同 → RENAMES 检出 R）
        {
            git_repository* repo = nullptr;
            git_fixture::checkRc(git_repository_open_ext(
                &repo, platform::git::pathToUtf8(dir).c_str(),
                GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr), "open");
            git_index* index = nullptr;
            git_fixture::checkRc(git_repository_index(&index, repo), "index");
            git_fixture::checkRc(git_index_remove_bypath(index, "旧名.txt"),
                                 "remove");
            // 写一条新路径同内容条目（改名：旧名删 + 新名加 → RENAMES 检出 R）
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
            // workdir 同步去掉旧名（git mv 语义）：否则 旧名.txt 以 untracked 残留，
            // 与同内容 index 删除侧再配成第二组 rename，淹没 sawDeleted
            fs::remove(dir / "旧名.txt", ec);
        }

        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        auto snapshot = opened.value().status();
        DOCTEST_REQUIRE(snapshot.isOk());
        if (snapshot.isFail()) return false;

        // 模型重建面（need:291）归 GitChangeListModel 消费用例；本用例钉 platform
        // status() 四态全链（keep Modified unstaged / 删除我 Deleted / 新文件
        // Untracked / 新名 Renamed+oldPath），直接断言快照条目（模型无关）
        const auto& entries = snapshot.value().entries;
        bool sawModified = false, sawDeleted = false, sawUntracked = false,
             sawRenamed = false;
        for (const auto& e : entries) {
            if (e.path == "keep.txt" && e.kind == StatusKind::Modified && !e.staged)
                sawModified = true;
            if (e.path == "删除我.txt" && e.kind == StatusKind::Deleted) sawDeleted = true;
            if (e.path == "新文件.txt" && e.kind == StatusKind::Untracked) sawUntracked = true;
            if (e.path == "新名.txt" && e.kind == StatusKind::Renamed) {
                sawRenamed = true;
                DOCTEST_CHECK_EQ(e.oldPath, "旧名.txt");
            }
        }
        DOCTEST_CHECK(sawModified);
        DOCTEST_CHECK(sawDeleted);
        DOCTEST_CHECK(sawUntracked);
        DOCTEST_CHECK(sawRenamed);  // F-06②：rename 检测旧→新全链
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitColorsDualTheme, "GitColors: 四态着色浅/深双套（t.isDark 分档、色随主题）", {
    core::theme::setLight();
    const GitColors light = gitColors(core::theme::current());
    // 四态文字色互不相同（可区分性契约，PRD §6）
    DOCTEST_CHECK(!colorSame(light.added, light.modified));
    DOCTEST_CHECK(!colorSame(light.modified, light.deleted));
    DOCTEST_CHECK(!colorSame(light.deleted, light.renamed));
    DOCTEST_CHECK(!colorSame(light.added, light.deleted));

    core::theme::setDark();
    const GitColors dark = gitColors(core::theme::current());
    DOCTEST_CHECK(!colorSame(dark.added, dark.modified));
    DOCTEST_CHECK(!colorSame(dark.modified, dark.deleted));
    // 深浅两套 added/deleted 不同值（双套成立）
    DOCTEST_CHECK(!colorSame(dark.added, light.added));
    DOCTEST_CHECK(!colorSame(dark.deleted, light.deleted));
    core::theme::setLight();  // 还原全局主题态（不污染后续用例）
    return true;
})
