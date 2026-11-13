// tests/test_platform/test_git_status_model.cpp
// git 管理器批次二（need:7）：中栏变更列表模型——
//   ① GitChangesModel::build：变更/未跟踪两组 + 路径去重合并双侧 + 字节序输出；
//   ② 重命名旧→新（F-06②）：RAII 层 StatusEntry.oldPath 通道（staged/unstaged 两侧）；
//   ③ 组头文案计数；徽标字母映射（A/M/D/R/U）；
//   ④ GitColors 四态着色浅/深双套（t.isDark 分档，md_highlight 同法映射）。
// 纯函数面用构造快照直打；rename/status 语义用 git_fixture 真仓断言。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "module_git/git_changes_model.h"
#include "module_git/git_colors.h"
#include "module_git/git_tree_model.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using module::git::ChangeGroup;
using module::git::ChangeRow;
using module::git::GitChangesModel;
using module::git::GitColors;
using module::git::gitColors;
using module::git::statusBadgeLetter;
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

size_t countGroup(const std::vector<ChangeRow>& rows, ChangeGroup group) {
    size_t n = 0;
    for (const auto& r : rows) {
        if (r.group == group) ++n;
    }
    return n;
}

// eui::Color = {float r,g,b,a} 聚合体，无 operator==——逐分量比较
bool colorSame(const eui::Color& a, const eui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

} // namespace

REGISTER_TEST(gitStatusModelGroupsAndDedup, "build: 变更/未跟踪两组 + 同路径双侧合并一条 + 字节序", {
    StatusSnapshot snap;
    snap.entries = {
        entryOf("z.txt", StatusKind::Modified, false),
        entryOf("a.txt", StatusKind::Modified, true),
        entryOf("a.txt", StatusKind::Modified, false),  // 同路径双侧 → 合并
        entryOf("m/b.bin", StatusKind::Untracked, false),
        entryOf("新文件.md", StatusKind::New, false),
        entryOf("删除.txt", StatusKind::Deleted, true),
    };
    const auto rows = GitChangesModel::build(snap);
    DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(5));  // a.txt 合并后 5 条
    DOCTEST_CHECK_EQ(countGroup(rows, ChangeGroup::Changed), 4);
    DOCTEST_CHECK_EQ(countGroup(rows, ChangeGroup::Untracked), 1);
    // map 输出 = 路径 UTF-8 字节序（'删'U+5220→E5…，'新'U+65B0→E6…，故 删除.txt < 新文件.md）
    DOCTEST_CHECK(rows[0].path == "a.txt");
    DOCTEST_CHECK(rows[1].path == "m/b.bin");
    DOCTEST_CHECK(rows[3].path == "删除.txt");
    DOCTEST_CHECK(rows[4].path == "新文件.md");
    // 未跟踪组独立：唯一 Untracked 按字节序落位（模型=扁平 map 序，分组呈现由视图层承担）
    DOCTEST_CHECK(rows[1].group == ChangeGroup::Untracked);
    return true;
})

REGISTER_TEST(gitStatusModelRenameOldToNew, "build: 重命名旧→新（F-06②）+ 严重度合并保 rename", {
    StatusSnapshot snap;
    // staged rename（head→index 侧）+ unstaged 同路径修改 → 合并保 Renamed
    snap.entries = {
        entryOf("docs/接口规范.md", StatusKind::Renamed, true, "docs/说明文档.md"),
        entryOf("docs/接口规范.md", StatusKind::Modified, false),
    };
    const auto rows = GitChangesModel::build(snap);
    DOCTEST_REQUIRE_EQ(rows.size(), static_cast<size_t>(1));
    DOCTEST_CHECK_EQ(rows[0].kind, StatusKind::Renamed);
    DOCTEST_CHECK_EQ(rows[0].path, "docs/接口规范.md");
    DOCTEST_CHECK_EQ(rows[0].oldPath, "docs/说明文档.md");  // 主行新名 + 副行旧名通道
    return true;
})

REGISTER_TEST(gitStatusModelHeaderAndBadge, "组头文案计数 + 徽标字母映射（A/M/D/R/U）", {
    DOCTEST_CHECK_EQ(GitChangesModel::groupHeader(ChangeGroup::Changed, 5),
                     std::string("变更 (5)"));
    DOCTEST_CHECK_EQ(GitChangesModel::groupHeader(ChangeGroup::Untracked, 2),
                     std::string("未跟踪 (2)"));
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::New), 'A');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Modified), 'M');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Deleted), 'D');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Renamed), 'R');
    DOCTEST_CHECK_EQ(statusBadgeLetter(StatusKind::Untracked), 'U');
    return true;
})

REGISTER_TEST(gitStatusModelRealRepoStatus, "真仓 status → build：修改/新增/删除/重命名四态全链", {
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

        const auto rows = GitChangesModel::build(snapshot.value());
        bool sawModified = false, sawDeleted = false, sawUntracked = false,
             sawRenamed = false;
        for (const auto& r : rows) {
            if (r.path == "keep.txt" && r.kind == StatusKind::Modified) sawModified = true;
            if (r.path == "删除我.txt" && r.kind == StatusKind::Deleted) sawDeleted = true;
            if (r.path == "新文件.txt" && r.kind == StatusKind::Untracked) sawUntracked = true;
            if (r.path == "新名.txt" && r.kind == StatusKind::Renamed) {
                sawRenamed = true;
                DOCTEST_CHECK_EQ(r.oldPath, "旧名.txt");
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
