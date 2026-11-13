// tests/test_platform/test_git_tree_model.cpp
// git 管理器批次二（need:7）：左栏目录树模型——
//   ① walkChildren：文件枚举 + .git 隐藏 + ignore 过滤 + 中文目录 + 子仓库降级；
//   ② flatten（rows()）：展开/折叠/深度/派生行（空目录/懒加载错误）+ 两跑幂等；
//   ③ 两模式：ChangesOnly = status 平铺（不 walk）；AllFiles 着色叠加；
//   ④ 懒加载三态：loading 占位 / 无权限降级（NoAccess）/ 错误行重试回待加载。
// 夹具：git_fixture.h libgit2 API 本地建仓（离线）。
// 清理纪律（Windows）：libgit2 打开 repo 期间 .git 句柄被占——断言体进 lambda，
// cleanupRepoDir 在 repo 析构之后跑（test_git_raii 同款壳）。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "module_git/git_tree_model.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using module::git::GitTreeModel;
using module::git::TreeRow;
using module::git::TreeRowKind;
using module::git::WalkResult;
using platform::git::Libgit2Scope;
using platform::git::Repo;
using platform::git::StatusKind;
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

int countKind(const std::vector<TreeRow>& rows, TreeRowKind kind) {
    int n = 0;
    for (const auto& r : rows) {
        if (r.kind == kind) ++n;
    }
    return n;
}

const TreeRow* findRow(const std::vector<TreeRow>& rows, const std::string& rel) {
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

        const WalkResult walked =
            GitTreeModel::walkChildren(&repo, repo.workdir(), "");
        DOCTEST_REQUIRE(walked.ok);
        if (!walked.ok) return false;
        // 目录在前：说明文档/ + .gitignore + a.txt；build/（ignored）与 .git 被滤
        DOCTEST_CHECK_EQ(walked.entries.size(), static_cast<size_t>(3));
        if (walked.entries.size() != 3) return false;
        DOCTEST_CHECK_EQ(walked.entries[0].kind, TreeRowKind::Directory);
        DOCTEST_CHECK_EQ(walked.entries[0].name, "说明文档");
        DOCTEST_CHECK_EQ(walked.entries[0].relPath, "说明文档");
        DOCTEST_CHECK_EQ(walked.entries[1].kind, TreeRowKind::File);
        DOCTEST_CHECK_EQ(walked.entries[1].name, ".gitignore");
        DOCTEST_CHECK_EQ(walked.entries[2].kind, TreeRowKind::File);
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

        const WalkResult walked =
            GitTreeModel::walkChildren(&repo, repo.workdir(), "");
        DOCTEST_REQUIRE(walked.ok);
        const TreeRow* subRow = findRow(walked.entries, "嵌入仓库");
        DOCTEST_REQUIRE(subRow != nullptr);
        if (subRow == nullptr) return false;
        DOCTEST_CHECK_EQ(subRow->kind, TreeRowKind::Subrepo);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitTreeFlattenExpandCollapse, "rows(): 展开/折叠/深度 + 两跑幂等（clue#22）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeWalkRepo("flat");
        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        Repo& repo = opened.value();

        GitTreeModel model;
        model.applyChildren(
            "", GitTreeModel::walkChildren(&repo, repo.workdir(), "").entries);

        // 折叠态：只有根层 3 行（说明文档/ + .gitignore + a.txt）
        const auto collapsed = model.rows();
        DOCTEST_CHECK_EQ(collapsed.size(), static_cast<size_t>(3));
        for (const auto& r : collapsed) DOCTEST_CHECK_EQ(r.depth, 0);

        // 展开中文目录 → 子层行 depth 1
        model.setExpanded("说明文档", true);
        DOCTEST_CHECK(model.isExpanded("说明文档"));
        model.applyChildren(
            "说明文档",
            GitTreeModel::walkChildren(&repo, repo.workdir(), "说明文档").entries);
        const auto expanded = model.rows();
        DOCTEST_CHECK_EQ(expanded.size(), static_cast<size_t>(4));  // 根 3 + 子 1
        const TreeRow* note = findRow(expanded, "说明文档/笔记.md");
        DOCTEST_REQUIRE(note != nullptr);
        if (note != nullptr) {
            DOCTEST_CHECK_EQ(note->depth, 1);
            DOCTEST_CHECK_EQ(note->kind, TreeRowKind::File);
        }

        // 两跑幂等：同状态再取一次必须逐行一致
        const auto again = model.rows();
        DOCTEST_REQUIRE_EQ(again.size(), expanded.size());
        for (size_t i = 0; i < again.size() && i < expanded.size(); ++i) {
            DOCTEST_CHECK_EQ(again[i].relPath, expanded[i].relPath);
            DOCTEST_CHECK_EQ(again[i].depth, expanded[i].depth);
            DOCTEST_CHECK(again[i].expanded == expanded[i].expanded);
        }

        // 折叠回 → 子层消失
        model.setExpanded("说明文档", false);
        DOCTEST_CHECK_EQ(model.rows().size(), static_cast<size_t>(3));
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitTreeEmptyDirHintRow, "rows(): 空目录展开 → EmptyDir 派生行", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeWalkRepo("emptydir");
        // 磁盘真空目录（walk 走 fs 不走 git，空目录可枚举；不落 keeper 文件）
        std::error_code mkEc;
        fs::create_directories(dir / "空目录", mkEc);
        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        Repo& repo = opened.value();

        GitTreeModel model;
        model.applyChildren(
            "", GitTreeModel::walkChildren(&repo, repo.workdir(), "").entries);
        model.setExpanded("空目录", true);
        model.applyChildren(
            "空目录",
            GitTreeModel::walkChildren(&repo, repo.workdir(), "空目录").entries);
        const auto rows = model.rows();
        DOCTEST_CHECK_EQ(countKind(rows, TreeRowKind::EmptyDir), 1);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitTreeChangesOnlyModeFromStatus, "仅修改模式：status 平铺不 walk + 全部文件模式回切不炸", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeWalkRepo("chgmode");
        writeTextFile(dir / "a.txt", "hello\nchanged\n");  // modified（unstaged）
        writeTextFile(dir / "新增文件.txt", "new\n");       // untracked

        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        auto snapshot = opened.value().status();
        DOCTEST_REQUIRE(snapshot.isOk());
        if (snapshot.isFail()) return false;

        GitTreeModel model;
        model.setMode(GitTreeModel::Mode::ChangesOnly);
        model.applyStatus(snapshot.value());
        const auto rows = model.rows();
        DOCTEST_CHECK_EQ(rows.size(), static_cast<size_t>(2));
        for (const auto& r : rows) {
            DOCTEST_CHECK_EQ(r.depth, 0);  // 平铺无目录语义
            DOCTEST_CHECK(r.status.has_value());
        }
        const TreeRow* mod = findRow(rows, "a.txt");
        DOCTEST_REQUIRE(mod != nullptr);
        if (mod != nullptr) DOCTEST_CHECK_EQ(*mod->status, StatusKind::Modified);
        const TreeRow* unt = findRow(rows, "新增文件.txt");
        DOCTEST_REQUIRE(unt != nullptr);
        if (unt != nullptr) DOCTEST_CHECK_EQ(*unt->status, StatusKind::Untracked);

        // 切回全部文件模式：根层未 walk → 空行集（视图出骨架态，不炸）
        model.setMode(GitTreeModel::Mode::AllFiles);
        DOCTEST_CHECK(model.rows().empty());
        DOCTEST_CHECK_FALSE(model.rootLoaded());
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitTreeLazyStatesAndRetry, "懒加载三态：loading 占位 / NoAccess 降级 / 错误行重试回待加载", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeWalkRepo("lazy");
        auto opened = Repo::open(dir);
        DOCTEST_REQUIRE(opened.isOk());
        if (opened.isFail()) return false;
        Repo& repo = opened.value();

        GitTreeModel model;
        model.applyChildren(
            "", GitTreeModel::walkChildren(&repo, repo.workdir(), "").entries);
        model.setExpanded("说明文档", true);

        // ① 根层在位、子层未加载 → pendingDirs 含目标；markLoading 后退出待投集
        bool pendingHasDir = false;
        for (const auto& d : model.pendingDirs()) {
            if (d == "说明文档") pendingHasDir = true;
        }
        DOCTEST_CHECK(pendingHasDir);
        model.markLoading("说明文档");
        pendingHasDir = false;
        for (const auto& d : model.pendingDirs()) {
            if (d == "说明文档") pendingHasDir = true;
        }
        DOCTEST_CHECK_FALSE(pendingHasDir);

        // ② 无权限失败 → NoAccess 降级（展开符收回）
        model.markNoAccess("说明文档");
        const auto degraded = model.rows();
        const TreeRow* noAccess = findRow(degraded, "说明文档");
        DOCTEST_REQUIRE(noAccess != nullptr);
        if (noAccess != nullptr) {
            DOCTEST_CHECK_EQ(noAccess->kind, TreeRowKind::NoAccess);
            DOCTEST_CHECK_FALSE(noAccess->expanded);
        }

        // ③ 其它失败 → 行内 Error 行；重试 → 清错误回待加载
        //（markNoAccess 已收展开符——requestRetry 后须重新展开才见行内错误）
        model.requestRetry("说明文档");
        model.setExpanded("说明文档", true);
        model.markLoadFailed("说明文档", "磁盘满");
        DOCTEST_CHECK_EQ(countKind(model.rows(), TreeRowKind::Error), 1);
        model.requestRetry("说明文档");
        pendingHasDir = false;
        for (const auto& d : model.pendingDirs()) {
            if (d == "说明文档") pendingHasDir = true;
        }
        DOCTEST_CHECK(pendingHasDir);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})
