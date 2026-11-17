// tests/test_platform/test_git_tree_model.cpp
// git 管理器重设计（need:291）：GitTreeModel 类随左栏砍除退役（F-03 / D2-A），
// 单层枚举链迁出为 git_fs_walk 纯函数——本文件保留 walkChildren 直测
//（语义零变，防迁移回归；迁移前先在新挂点复跑绿）：
//   ① walkChildren：文件枚举 + .git 隐藏 + ignore 过滤 + 中文目录 + 子仓库降级。
// 旧 GitTreeModel 类行为用例迁移去向（随类退役，不再在此维护）：
//   - 展开/折叠/深度/两跑幂等、空目录派生行、懒加载三态（loading/NoAccess/Error
//     重试）→ GitChangeListModel 展开位图 / lazy 子行注入断言：
//     test_git_change_list_model.cpp（新模型测试文件）
//   - ChangesOnly status 平铺（含 untracked 目录尾斜杠 U 行特殊化）→
//     GitChangeListModel rebuild 分桶 / U 特殊化断言：test_git_change_list_model.cpp
// 夹具：git_fixture.h libgit2 API 本地建仓（离线）。
// 清理纪律（Windows）：libgit2 打开 repo 期间 .git 句柄被占——断言体进 lambda，
// cleanupRepoDir 在 repo 析构之后跑（test_git_raii 同款壳）。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "module_git/git_fs_walk.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using module::git::fs_walk::EntryKind;
using module::git::fs_walk::FsEntry;
using module::git::fs_walk::WalkResult;
using module::git::fs_walk::walkChildren;
using platform::git::Libgit2Scope;
using platform::git::Repo;
using test_support::writeTextFile;

namespace {

// 夹具仓：a.txt（跟踪）/ build/out.o（ignored）/ 说明文档/笔记.md（中文目录）
fs::path makeWalkRepo(const std::string& tag) {
    const fs::path dir = test_support::makeTempDir("devdesk_git_tree_", tag);
    git_fixture::initRepo(dir);
    writeTextFile(dir / ".gitignore", "build/\n");
    writeTextFile(dir / "a.txt", "hello\n");
    writeTextFile(dir / "说明文档" / "笔记.md", "内容\n");
    git_fixture::commitAll(dir, {{".gitignore", "build/\n"},
                                 {"a.txt", "hello\n"},
                                 {"说明文档/笔记.md", "内容\n"}});
    // ignored 目录在 commit 之后造（不进 HEAD）
    writeTextFile(dir / "build" / "out.o", "obj\n");
    return dir;
}

const FsEntry* findRow(const std::vector<FsEntry>& rows, const std::string& rel) {
    for (const auto& r : rows) {
        if (r.relPath == rel) return &r;
    }
    return nullptr;
}

} // namespace

REGISTER_TEST(gitTreeWalkEnumeratesAndFilters, "walkChildren: 枚举+ignore 过滤+.git 隐藏+中文目录（F-10）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeWalkRepo("walk");
        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        Repo& repo = opened.value();

        const WalkResult walked = walkChildren(&repo, repo.workdir(), "");
        DOCTEST_REQUIRE(walked.ok);
        if (!walked.ok) return false;
        // 目录在前：说明文档/ + .gitignore + a.txt；build/（ignored）与 .git 被滤
        DOCTEST_CHECK_EQ(walked.entries.size(), static_cast<size_t>(3));
        if (walked.entries.size() != 3) return false;
        DOCTEST_CHECK_EQ(walked.entries[0].kind, EntryKind::Directory);
        DOCTEST_CHECK_EQ(walked.entries[0].name, "说明文档");
        DOCTEST_CHECK_EQ(walked.entries[0].relPath, "说明文档");
        DOCTEST_CHECK_EQ(walked.entries[1].kind, EntryKind::File);
        DOCTEST_CHECK_EQ(walked.entries[1].name, ".gitignore");
        DOCTEST_CHECK_EQ(walked.entries[2].kind, EntryKind::File);
        DOCTEST_CHECK_EQ(walked.entries[2].name, "a.txt");
        for (const auto& r : walked.entries) {
            DOCTEST_CHECK(r.name != ".git");
            DOCTEST_CHECK(r.name != "build");
        }
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitTreeWalkSubrepoDegraded, "walkChildren: 子目录含 .git → Subrepo 降级（F-12①）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeWalkRepo("sub");
        std::error_code ec;
        fs::create_directories(dir / "嵌入仓库" / ".git", ec);
        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        Repo& repo = opened.value();

        const WalkResult walked = walkChildren(&repo, repo.workdir(), "");
        DOCTEST_REQUIRE(walked.ok);
        const FsEntry* subRow = findRow(walked.entries, "嵌入仓库");
        DOCTEST_REQUIRE(subRow != nullptr);
        if (subRow == nullptr) return false;
        DOCTEST_CHECK_EQ(subRow->kind, EntryKind::Subrepo);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})
