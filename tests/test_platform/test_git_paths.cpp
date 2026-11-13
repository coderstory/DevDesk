// tests/test_platform/test_git_paths.cpp
// 中文/非 ASCII + 空格路径全链 POC（need:8 批次一 POC-②；方案 §8 风险 2）：
// 临时目录本身带中文+空格 → git init → 中文/空格文件名 commit → open/status/diff/
// isIgnored 全链 UTF-8 语义实证（Windows 侧 libgit2 内部宽字符行为实测点）。
// 夹具：git_fixture.h libgit2 API 本地建仓（离线）。
// 清理纪律同 test_git_raii.cpp（repo 句柄析构后才删目录）。
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

// 目录名本身中文+空格：open 传参、.git 内部路径、workdir 回读全走一遍
fs::path makeChineseRepo(const std::string& tag) {
    const fs::path dir = test_support::makeTempDir("devdesk_git_paths_测试 ", tag);
    git_fixture::initRepo(dir);
    git_fixture::commitAll(dir, {
        {"说明 文档.txt", "第一行\n第二行\n"},
        {"nested/中文 笔记.md", "内容\n"},
    });
    return dir;
}

} // namespace

REGISTER_TEST(gitPathsOpenWorkdirRoundtrip, "中文+空格仓：init→commit→open→workdir 回读一致", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeChineseRepo("open");
        auto r = Repo::open(dir);
        DOCTEST_CHECK(r.isOk());
        Repo repo = std::move(r.value());
        DOCTEST_CHECK_EQ(platform::git::pathToUtf8(repo.workdir()), platform::git::pathToUtf8(dir));
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitPathsStatusUtf8Faithful, "中文+空格仓：status 路径 UTF-8 保真 + 中文新文件 Untracked", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeChineseRepo("status");
        DOCTEST_CHECK(writeTextFile(dir / "新 观察者.txt", "观察\n"));
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());
        auto s = repo.status();
        DOCTEST_CHECK(s.isOk());
        bool sawUntracked = false;
        for (const auto& e : s.value().entries) {
            if (e.path == "新 观察者.txt") {
                sawUntracked = true;
                DOCTEST_CHECK(e.kind == StatusKind::Untracked);
                DOCTEST_CHECK(!e.staged);
            }
        }
        DOCTEST_CHECK(sawUntracked);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitPathsDiffChineseLineContent, "中文+空格仓：diff 行内容与行号（修改中文文件）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeChineseRepo("diff");
        DOCTEST_CHECK(writeTextFile(dir / "说明 文档.txt", "第一行\n第二行改\n"));
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());
        auto d = repo.diff(DiffBasis::WorkdirToHead, "说明 文档.txt");
        DOCTEST_CHECK(d.isOk());
        DOCTEST_CHECK_EQ(d.value().patches.size(), 1u);
        DOCTEST_CHECK_EQ(d.value().patches[0].hunks.size(), 1u);
        bool hasPlus = false;
        for (const auto& line : d.value().patches[0].hunks[0].lines) {
            if (line.origin == '+') {
                hasPlus = true;
                DOCTEST_CHECK_EQ(line.content, "第二行改");
                DOCTEST_CHECK(line.newNo.has_value());
                DOCTEST_CHECK_EQ(*line.newNo, 2);
            }
        }
        DOCTEST_CHECK(hasPlus);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitPathsIgnoreJudgement, "中文+空格仓：isIgnored 判定 + ignored 不进 status", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeChineseRepo("ignore");
        git_fixture::commitAll(dir, {{".gitignore", "忽略*.log\n"}});
        DOCTEST_CHECK(writeTextFile(dir / "忽略我.log", "noise\n"));
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());
        auto ig = repo.isIgnored("忽略我.log");
        DOCTEST_CHECK(ig.isOk());
        DOCTEST_CHECK(ig.value());
        auto notIg = repo.isIgnored("说明 文档.txt");
        DOCTEST_CHECK(notIg.isOk());
        DOCTEST_CHECK(!notIg.value());
        auto s = repo.status();
        DOCTEST_CHECK(s.isOk());
        for (const auto& e : s.value().entries) {
            DOCTEST_CHECK(e.path != "忽略我.log");
        }
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})
