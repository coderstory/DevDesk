// tests/test_platform/test_git_raii.cpp
// platform::git RAII 层契约（need:8 批次一；方案 §5）：
//   ① Libgit2Scope 配对 init/shutdown（嵌套/refcount）；
//   ② open 错误三因映射（F-01①②）：非仓→ValidationError / 目录不存在→FileNotFound；
//   ③ status kind 矩阵 + staged 双侧拆条（§2.1 口径）；
//   ④ diff 三基准语义 + untracked/二进制/pathFilter + 空仓 unborn 基准；
//   ⑤ blame 留接口 → NotImplemented；移动语义安全。
// 夹具：git_fixture.h libgit2 API 本地建仓（离线）；夹具失败抛 runtime_error（test_support 同款）。
// 清理纪律（Windows）：libgit2 打开 repo 期间 .git/objects 文件句柄被占，目录删除必须
// 放在 repo 析构之后——断言体进 lambda（CHECK 的 return false 从 lambda 退出），lambda
// 返回后再 remove_all。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using platform::git::DiffBasis;
using platform::git::Libgit2Scope;
using platform::git::Repo;
using platform::git::StatusKind;
using test_support::writeTextFile;

namespace {

// 建夹具仓：a.txt / b.txt 单 commit
fs::path makeBasicRepo(const std::string& tag) {
    const fs::path dir = test_support::makeTempDir("devdesk_git_raii_", tag);
    git_fixture::initRepo(dir);
    git_fixture::commitAll(dir, {{"a.txt", "第一行\n第二行\n"}, {"b.txt", "bye\n"}});
    return dir;
}

// status 快照里是否存在 (path, kind, staged) 条目
bool hasEntry(const platform::git::StatusSnapshot& snap, const std::string& path,
              StatusKind kind, bool staged) {
    for (const auto& e : snap.entries) {
        if (e.path == path && e.kind == kind && e.staged == staged) return true;
    }
    return false;
}

// 断言体跑完后按 repo 已析构的前提清目录：统一走 git_fixture::cleanupRepoDir

} // namespace

REGISTER_TEST(gitRaiiScopeNested, "Libgit2Scope 嵌套配对 init/shutdown（refcount 语义）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeBasicRepo("scope");
        auto r = Repo::open(dir);
        DOCTEST_CHECK(r.isOk());  // 嵌套作用域析构后库仍可用（外层 refcount 在）
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiOpenNonGitDirValidationError, "open 非 git 目录 → ValidationError（F-01①）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = test_support::makeTempDir("devdesk_git_raii_", "norepo");
        auto r = Repo::open(dir);
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK(r.errorCode() == core::ErrorCode::ValidationError);
        DOCTEST_CHECK(!r.errorMessage().empty());
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiOpenMissingDirFileNotFound, "open 目录不存在 → FileNotFound", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = test_support::makeTempDir("devdesk_git_raii_", "seed");
        auto r = Repo::open(dir / "不存在目录");
        DOCTEST_CHECK(r.isFail());
        DOCTEST_CHECK(r.errorCode() == core::ErrorCode::FileNotFound);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiOpenWorkdirAndMove, "open 成功：workdir 与所选目录一致 + 移动语义安全", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeBasicRepo("open");
        auto r = Repo::open(dir);
        DOCTEST_CHECK(r.isOk());
        Repo repo = std::move(r.value());
        DOCTEST_CHECK_EQ(platform::git::pathToUtf8(repo.workdir()), platform::git::pathToUtf8(dir));

        Repo moved = std::move(repo);  // 移动后源对象析构安全（unique_ptr 置空）
        auto s = moved.status();
        DOCTEST_CHECK(s.isOk());
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiStatusKindMatrix, "status kind 矩阵：修改/删除/新文件 + staged 双侧拆条", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeBasicRepo("status");
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());

        DOCTEST_CHECK(writeTextFile(dir / "a.txt", "第一行\n第二行改\n"));  // WT_MODIFIED
        DOCTEST_CHECK(fs::remove(dir / "b.txt"));                            // WT_DELETED
        DOCTEST_CHECK(writeTextFile(dir / "新增.txt", "untracked\n"));       // WT_NEW
        auto s1 = repo.status();
        DOCTEST_CHECK(s1.isOk());
        DOCTEST_CHECK(hasEntry(s1.value(), "a.txt", StatusKind::Modified, false));
        DOCTEST_CHECK(hasEntry(s1.value(), "b.txt", StatusKind::Deleted, false));
        DOCTEST_CHECK(hasEntry(s1.value(), "新增.txt", StatusKind::Untracked, false));

        // stage 修改后的 a.txt：index≠HEAD → staged=true；再写 workdir → 双侧两条
        git_fixture::stageExistingFile(dir, "a.txt");
        auto s2 = repo.status();
        DOCTEST_CHECK(s2.isOk());
        DOCTEST_CHECK(hasEntry(s2.value(), "a.txt", StatusKind::Modified, true));
        DOCTEST_CHECK(!hasEntry(s2.value(), "a.txt", StatusKind::Modified, false));
        DOCTEST_CHECK(writeTextFile(dir / "a.txt", "第一行\n第二行改了两次\n"));
        auto s3 = repo.status();
        DOCTEST_CHECK(s3.isOk());
        DOCTEST_CHECK(hasEntry(s3.value(), "a.txt", StatusKind::Modified, true));
        DOCTEST_CHECK(hasEntry(s3.value(), "a.txt", StatusKind::Modified, false));
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiDiffWorkdirToHead, "diff WorkdirToHead：增删行 + 行号 + untracked + pathFilter", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeBasicRepo("diff");
        DOCTEST_CHECK(writeTextFile(dir / "a.txt", "第一行\n第二行改\n"));
        DOCTEST_CHECK(writeTextFile(dir / "c.txt", "全新内容\n"));
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());

        auto d = repo.diff(DiffBasis::WorkdirToHead);
        DOCTEST_CHECK(d.isOk());
        const auto& patches = d.value().patches;
        DOCTEST_CHECK_EQ(patches.size(), 2u);
        DOCTEST_CHECK_EQ(patches[0].newPath, "a.txt");
        DOCTEST_CHECK(!patches[0].binary);
        DOCTEST_CHECK_EQ(patches[0].hunks.size(), 1u);
        bool hasMinus = false;
        bool hasPlus = false;
        for (const auto& line : patches[0].hunks[0].lines) {
            if (line.origin == '-') {
                hasMinus = true;
                DOCTEST_CHECK_EQ(line.content, "第二行");
                DOCTEST_CHECK(line.oldNo.has_value());
                DOCTEST_CHECK_EQ(*line.oldNo, 2);
                DOCTEST_CHECK(!line.newNo.has_value());
            }
            if (line.origin == '+') {
                hasPlus = true;
                DOCTEST_CHECK_EQ(line.content, "第二行改");
                DOCTEST_CHECK(line.newNo.has_value());
                DOCTEST_CHECK_EQ(*line.newNo, 2);
            }
        }
        DOCTEST_CHECK(hasMinus);
        DOCTEST_CHECK(hasPlus);
        // untracked：c.txt 全增行单条目（pathsort a.txt < c.txt）
        DOCTEST_CHECK_EQ(patches[1].newPath, "c.txt");
        DOCTEST_CHECK(patches[1].isUntracked);

        // pathFilter：只出 a.txt
        auto d2 = repo.diff(DiffBasis::WorkdirToHead, "a.txt");
        DOCTEST_CHECK(d2.isOk());
        DOCTEST_CHECK_EQ(d2.value().patches.size(), 1u);
        DOCTEST_CHECK_EQ(d2.value().patches[0].newPath, "a.txt");
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiDiffBasisSplit, "diff 三基准语义：TreeToIndex 只看已暂存 / IndexToWorkdir 只看未暂存", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeBasicRepo("basis");
        DOCTEST_CHECK(writeTextFile(dir / "a.txt", "第一行\n第二行改\n"));
        git_fixture::stageExistingFile(dir, "a.txt");  // index 前进，workdir 干净
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());

        auto toIndex = repo.diff(DiffBasis::TreeToIndex);
        DOCTEST_CHECK(toIndex.isOk());
        DOCTEST_CHECK_EQ(toIndex.value().patches.size(), 1u);
        DOCTEST_CHECK_EQ(toIndex.value().patches[0].newPath, "a.txt");
        DOCTEST_CHECK(!toIndex.value().patches[0].isUntracked);

        auto toWorkdir = repo.diff(DiffBasis::IndexToWorkdir);
        DOCTEST_CHECK(toWorkdir.isOk());
        DOCTEST_CHECK(toWorkdir.value().patches.empty());

        DOCTEST_CHECK(writeTextFile(dir / "a.txt", "第一行\n第二行改了两次\n"));  // workdir 再动
        auto toWorkdir2 = repo.diff(DiffBasis::IndexToWorkdir);
        DOCTEST_CHECK(toWorkdir2.isOk());
        DOCTEST_CHECK_EQ(toWorkdir2.value().patches.size(), 1u);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiDiffUnbornHeadEmptyRepo, "空仓（unborn HEAD）diff WorkdirToHead：空树基准不炸", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = test_support::makeTempDir("devdesk_git_raii_", "unborn");
        git_fixture::initRepo(dir);
        DOCTEST_CHECK(writeTextFile(dir / "only.txt", "唯一文件\n"));
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());
        auto d = repo.diff(DiffBasis::WorkdirToHead);
        DOCTEST_CHECK(d.isOk());
        DOCTEST_CHECK_EQ(d.value().patches.size(), 1u);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiDiffBinaryFlag, "二进制文件 diff → patch.binary 标记（F-11 判据①）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = test_support::makeTempDir("devdesk_git_raii_", "bin");
        git_fixture::initRepo(dir);
        git_fixture::commitAll(dir, {{"bin.dat", std::string("BIN\0ARY\0", 8)}});
        DOCTEST_CHECK(writeTextFile(dir / "bin.dat", std::string("\0\1\2\3changed\0", 12)));
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());
        auto d = repo.diff(DiffBasis::WorkdirToHead);
        DOCTEST_CHECK(d.isOk());
        DOCTEST_CHECK_EQ(d.value().patches.size(), 1u);
        DOCTEST_CHECK(d.value().patches[0].binary);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitRaiiBlameNotImplemented, "blame 留接口 → NotImplemented（PRD 非目标 2）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeBasicRepo("blame");
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());
        auto b = repo.blame("a.txt");
        DOCTEST_CHECK(b.isFail());
        DOCTEST_CHECK(b.errorCode() == core::ErrorCode::NotImplemented);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})
