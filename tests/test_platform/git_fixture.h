// tests/test_platform/git_fixture.h
// git 管理器只读 v1 POC 夹具（need:8 批次一）：libgit2 API 本地建仓（零网络、零 CLI
// 依赖，方案 §4.2 POC 纪律）。仅测试用——生产代码一律走 platform::git 包装层。
// 所有夹具构造失败即抛 std::runtime_error（test_support.h 同款 fail-fast）。
#pragma once

#include <git2.h>

#include "doctest.h"
#include "platform/git/git_libgit2.h" // pathToUtf8 / utf8ToPath（中文路径 char8_t 通道）

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace git_fixture {

// libgit2 返回码断言：失败抛错并附 git_error_last 原始 message
inline void checkRc(int rc, const char* what) {
    if (rc < 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string(what) + ": " +
                                 (e != nullptr && e->message != nullptr ? e->message : "?"));
    }
}

// git init 空仓（dir 必须已存在）
inline void initRepo(const std::filesystem::path& dir) {
    git_repository* repo = nullptr;
    checkRc(git_repository_init(&repo, platform::git::pathToUtf8(dir).c_str(), 0),
            "git_repository_init");
    git_repository_free(repo);
}

namespace detail {
inline git_repository* openNoSearch(const std::filesystem::path& dir) {
    git_repository* repo = nullptr;
    checkRc(git_repository_open_ext(&repo, platform::git::pathToUtf8(dir).c_str(),
                                    GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr),
            "git_repository_open_ext");
    return repo;
}
} // namespace detail

// 写文件并把当前 workdir 全部内容提交为一个 commit（author 固定测试签名；history=1）。
// files: {相对路径（UTF-8，'/' 分隔）, 内容}
inline void commitAll(
    const std::filesystem::path& dir,
    const std::vector<std::pair<std::string, std::string>>& files) {
    git_repository* repo = detail::openNoSearch(dir);
    git_index* index = nullptr;
    checkRc(git_repository_index(&index, repo), "git_repository_index");
    for (const auto& [rel, content] : files) {
        const auto fullPath = dir / platform::git::utf8ToPath(rel);
        std::error_code mkEc;
        std::filesystem::create_directories(fullPath.parent_path(), mkEc);
        if (mkEc) {
            git_index_free(index);
            git_repository_free(repo);
            throw std::runtime_error("create fixture dir failed: " + rel);
        }
        std::ofstream f(fullPath, std::ios::binary);
        f << content;
        f.close();
        if (!f.good()) {
            git_index_free(index);
            git_repository_free(repo);
            throw std::runtime_error("write fixture file failed: " + rel);
        }
        checkRc(git_index_add_bypath(index, rel.c_str()), "git_index_add_bypath");
    }
    git_oid treeOid{};
    checkRc(git_index_write_tree(&treeOid, index), "git_index_write_tree");
    checkRc(git_index_write(index), "git_index_write");
    git_index_free(index);

    git_signature* sig = nullptr;
    checkRc(git_signature_now(&sig, "devdesk-test", "devdesk-test@local"), "git_signature_now");
    git_tree* tree = nullptr;
    checkRc(git_tree_lookup(&tree, repo, &treeOid), "git_tree_lookup");
    // 已有 HEAD → 以当前 tip 为第一父（二次 commit 场景，libgit2 强校验）；空仓 unborn → 无父
    git_commit* parentCommit = nullptr;
    size_t parentCount = 0;
    git_reference* headRef = nullptr;
    if (git_repository_head(&headRef, repo) == 0) {
        if (git_commit_lookup(&parentCommit, repo, git_reference_target(headRef)) == 0) {
            parentCount = 1;
        }
        git_reference_free(headRef);
    }
    git_oid commitOid{};
    const git_commit* parents[1] = {parentCommit};
    checkRc(git_commit_create(&commitOid, repo, "HEAD", sig, sig, "utf-8", "fixture commit", tree,
                              parentCount, parentCount > 0 ? parents : nullptr),
            "git_commit_create");
    if (parentCommit != nullptr) git_commit_free(parentCommit);
    git_tree_free(tree);
    git_signature_free(sig);
    git_repository_free(repo);
}

// 把 workdir 现有文件内容加入 index（index_write 落盘）——造「已暂存未提交」改动。
// 不改 workdir：index 与 workdir 同内容 → 只有 head→index 侧（staged=true）。
inline void stageExistingFile(const std::filesystem::path& dir, const std::string& rel) {
    git_repository* repo = detail::openNoSearch(dir);
    git_index* index = nullptr;
    checkRc(git_repository_index(&index, repo), "git_repository_index");
    checkRc(git_index_add_bypath(index, rel.c_str()), "git_index_add_bypath");
    checkRc(git_index_write(index), "git_index_write");
    git_index_free(index);
    git_repository_free(repo);
}

// 测试收尾：断言体通过后删夹具仓目录（须在 repo 句柄析构之后调用——Windows 下
// libgit2 打开期间 .git 内文件句柄被占）。
// 两道 Windows 特有障碍（need:8 实测）：
//   ① libgit2 按 git 惯例把 loose objects 落成只读（Windows = FILE_ATTRIBUTE_READONLY，
//      fs::remove 对只读文件恒 ACCESS_DENIED——确定性失败，非瞬态）→ 先递归清只读位；
//   ② 新写文件的杀软实时扫描短暂锁 → remove_all 再带 10×200ms 退避重试兜底。
// bodyOk=false（用例已红）时跳过清理，不掩盖原失败 note。
inline bool cleanupRepoDir(const std::filesystem::path& dir, bool bodyOk) {
    DOCTEST_CHECK(bodyOk);
    if (bodyOk) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            std::filesystem::permissions(it->path(), std::filesystem::perms::owner_write, ec);
        }
        std::error_code rmEc;
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::error_code attemptEc;
            std::filesystem::remove_all(dir, attemptEc);
            rmEc = attemptEc;
            if (!rmEc) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (rmEc) {
            std::cout << "[git_fixture] remove_all failed: " << rmEc.message() << " | "
                      << platform::git::pathToUtf8(dir) << "\n";
        }
        DOCTEST_CHECK(!rmEc);
    }
    return true;
}

} // namespace git_fixture
