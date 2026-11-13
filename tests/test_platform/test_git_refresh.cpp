// tests/test_platform/test_git_refresh.cpp
// git 管理器批次四（need:11）：刷新联动 + 节流 + 过期条 + N10 会话恢复 + submodule
// 降级 status（F-09④ / F-12③ / F-13①②③④）。
//   ① watcher 过滤表逐行（技术方案 §2.2：工作树/索引元数据 → 脏；objects/gc → 忽略；
//     Unknown → 全量重扫兜底）——纯函数 classifyWatchEvent；
//   ② 1s 节流窗：脏标记 trailing 到窗发一次；窗内不重复投（F-09④）；
//   ③ 过期条：元数据/溢出/命中打开文件 → staleBar 置位；reloadStale 清除（F-13③）；
//   ④ N10：切换仓库后重选同仓恢复 树模式+右栏打开文件与视图类型（仓/模式/打开文件）；
//   ⑤ submodule gitlink 仓 status 不误报内部改动（F-12③，EXCLUDE_SUBMODULES）。
// 控制器驱动：picker 注入缝 + 真 GitWorker 线程 + composePump 排空回投（协议同生产）。
#include "doctest.h"
#include "test_support.h"

#include "module_git/git_changes_model.h"
#include "module_git/git_content_model.h"
#include "module_git/git_page_controller.h"
#include "module_git/git_worker.h"
#include "platform/folder_picker.h"
#include "platform/git/git_libgit2.h"

#include "git_fixture.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using module::git::GitPageController;
using module::git::GitPagePhase;
using module::git::GitWorker;
using module::git::RightPaneState;
using module::git::WatchVerdict;

namespace {

// PickFolderFn 是裸函数指针（非 std::function）：路径经静态槽中转
const std::string* g_pickerPath = nullptr;
core::Result<std::string> pickFixed(const std::string&, const std::string&) {
    return core::Result<std::string>::ok(*g_pickerPath);
}

// 排空 worker 回投直到条件满足（composePump 即生产帧泵协议）
template <typename Pred>
bool pumpUntil(GitPageController& c, Pred pred, int maxMs = 5000) {
    for (int i = 0; i < maxMs / 5; ++i) {
        c.composePump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    c.composePump();
    return pred();
}

// 打开 fixture 仓（picker 注入 → 校验 → Ready；DEVDESK_DATA_ROOT 隔离由用例负责）
bool openRepoViaPicker(GitPageController& c, const std::string& repoUtf8) {
    g_pickerPath = &repoUtf8;
    platform::folderpicker::setPickerForTest(&pickFixed);
    c.beginPickRepo();
    return pumpUntil(c, [&] { return c.phase() == GitPagePhase::Ready; });
}

} // namespace

// ---- ① watcher 过滤表（技术方案 §2.2 逐行）----

REGISTER_TEST(gitWatchFilterWorktreeAndMeta, "watcher 过滤表：工作树/索引元数据置脏，objects/gc 忽略，Unknown 全量重扫", {
    using A = platform::FileWatchAction;
    // 工作树路径 → status 脏（是否过期由控制器比对打开文件）
    const WatchVerdict src = module::git::classifyWatchEvent(A::Modified, "src/a.cpp");
    DOCTEST_CHECK(src.marksStatus);
    DOCTEST_CHECK(src.marksOpenStale == false);
    // .git 元数据三形态 → 脏 + 无条件置过期条
    for (const char* meta : {".git/index", ".git/HEAD"}) {
        const WatchVerdict v = module::git::classifyWatchEvent(A::Modified, meta);
        DOCTEST_CHECK(v.marksStatus);
        DOCTEST_CHECK(v.marksOpenStale);
    }
    const WatchVerdict refs = module::git::classifyWatchEvent(A::Created, ".git/refs/heads/main");
    DOCTEST_CHECK(refs.marksStatus);
    DOCTEST_CHECK(refs.marksOpenStale);
    // gc 洪泛忽略
    const WatchVerdict objs = module::git::classifyWatchEvent(A::Created, ".git/objects/ab/cdef1234");
    DOCTEST_CHECK_FALSE(objs.marksStatus);
    const WatchVerdict logs = module::git::classifyWatchEvent(A::Modified, ".git/logs/HEAD");
    DOCTEST_CHECK_FALSE(logs.marksStatus);
    const WatchVerdict pack = module::git::classifyWatchEvent(A::Created, ".git/objects/pack/pack-xyz.pack");
    DOCTEST_CHECK_FALSE(pack.marksStatus);
    // Unknown / 空路径 = 缓冲溢出 → 全量重扫兜底
    const WatchVerdict unk = module::git::classifyWatchEvent(A::Unknown, "");
    DOCTEST_CHECK(unk.marksStatus);
    DOCTEST_CHECK(unk.marksOpenStale);
    // .git 目录自身（无尾斜杠）建删 → 脏
    const WatchVerdict dotgit = module::git::classifyWatchEvent(A::Created, ".git");
    DOCTEST_CHECK(dotgit.marksStatus);
    return true;
})

// ---- ② 1s 节流窗 + ③ 过期条 + ④ N10（同一控制器会话内走完）----

REGISTER_TEST(gitWatchThrottleAndStaleBar, "刷新联动：watcher 脏→1s 窗至多一次 status；元数据置过期条；reloadStale 清除；命中打开文件置条", {
    const platform::git::Libgit2Scope libgit2;  // fixture 直调 git2 API，先全局 init
    const auto root = test_support::makeTempDir("devdesk_git_refresh_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto repoDir = root / "节流仓";
    std::filesystem::create_directories(repoDir);
    git_fixture::initRepo(repoDir);
    git_fixture::commitAll(repoDir, {{"a.txt", "第一版\n"}});
    const std::string repoUtf8 = platform::git::pathToUtf8(repoDir);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK(openRepoViaPicker(c, repoUtf8));
    DOCTEST_CHECK(pumpUntil(c, [&] { return c.tree().rootLoaded(); }));
    // 静默基线：走出开仓那次 status 的 1s 节流窗，后续 watcher 断言确定化
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.composePump();

    // ② watcher 脏 → 1s 节流：静默基线后窗已过 → 本帧立即投一次
    c.onWatchEvents({{platform::FileWatchAction::Modified, "a.txt"}});
    c.composePump();
    DOCTEST_CHECK(c.statusLoading());
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
    // 窗内连击：置脏但不投（1s 内至多一次重算，F-09④）
    c.onWatchEvents({{platform::FileWatchAction::Modified, "a.txt"}});
    c.onWatchEvents({{platform::FileWatchAction::Modified, "说明.md"}});
    c.composePump();
    DOCTEST_CHECK_FALSE(c.statusLoading());
    // 窗过 → trailing 补发一次
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.composePump();
    DOCTEST_CHECK(c.statusLoading());
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));

    // ③ 工作树事件命中打开文件 → 过期条；未命中 → 不置条
    // （先静默出窗，避免 pending 脏标记在本段中途触发 status 干扰时序）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.composePump();
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
    c.selectTreeFile("a.txt");
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.contentState() == RightPaneState::Ready;
    }));
    c.onWatchEvents({{platform::FileWatchAction::Modified, "其它文件.txt"}});
    c.composePump();
    DOCTEST_CHECK_FALSE(c.staleBarVisible());
    c.onWatchEvents({{platform::FileWatchAction::Modified, "a.txt"}});
    c.composePump();
    DOCTEST_CHECK(c.staleBarVisible());
    // 元数据跳变（外部 commit）→ 过期条无条件置位
    c.onWatchEvents({{platform::FileWatchAction::Modified, ".git/HEAD"}});
    c.composePump();
    DOCTEST_CHECK(c.staleBarVisible());
    c.reloadStale();
    DOCTEST_CHECK_FALSE(c.staleBarVisible());
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.contentState() == RightPaneState::Ready;
    }));

    // ② 手动刷新同闸：1s 窗内连点至多一次（F-09④）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.composePump();
    c.refresh();
    DOCTEST_CHECK(c.statusLoading());
    c.refresh();
    c.refresh();
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
    worker.stop();
    platform::folderpicker::setPickerForTest(nullptr);
    return git_fixture::cleanupRepoDir(repoDir, true);
})

REGISTER_TEST(gitN10SessionRestore, "N10：切换仓库后重选同仓恢复 树模式+右栏打开文件+视图类型；异仓全新会话", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_n10_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto repoA = root / "仓甲";
    const auto repoB = root / "仓乙";
    std::filesystem::create_directories(repoA);
    std::filesystem::create_directories(repoB);
    git_fixture::initRepo(repoA);
    git_fixture::commitAll(repoA, {{"甲.txt", "甲\n"}});
    git_fixture::initRepo(repoB);
    git_fixture::commitAll(repoB, {{"乙.txt", "乙\n"}});

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK(openRepoViaPicker(c, platform::git::pathToUtf8(repoA)));
    c.setTreeMode(module::git::GitTreeModel::Mode::ChangesOnly);
    // 制造变更条目并选中（打开文件 + diff 视图）
    std::ofstream(repoA / platform::git::utf8ToPath("甲.txt"), std::ios::binary) << "改\n";
    c.refresh();
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
    c.selectChange("甲.txt");
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.diffState() == RightPaneState::Ready ||
               c.diffState() == RightPaneState::Placeholder;
    }));
    DOCTEST_CHECK(c.selectedPath() == "甲.txt");
    DOCTEST_CHECK(c.rightShowsDiff());

    // 切走（N2 空态页）→ 重选同仓：恢复模式 + 打开文件 + 视图类型
    c.switchRepo();
    DOCTEST_CHECK(c.phase() == GitPagePhase::Empty);
    DOCTEST_CHECK(openRepoViaPicker(c, platform::git::pathToUtf8(repoA)));
    DOCTEST_CHECK(c.tree().mode() == module::git::GitTreeModel::Mode::ChangesOnly);
    DOCTEST_CHECK(c.selectedPath() == "甲.txt");
    DOCTEST_CHECK(c.rightShowsDiff());
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.diffState() == RightPaneState::Ready ||
               c.diffState() == RightPaneState::Placeholder;
    }));

    // 切走 → 重选异仓：全新会话（不携带旧选中）
    c.switchRepo();
    DOCTEST_CHECK(openRepoViaPicker(c, platform::git::pathToUtf8(repoB)));
    DOCTEST_CHECK(c.selectedPath().empty());
    DOCTEST_CHECK(c.rightShowsDiff() == false);
    worker.stop();
    platform::folderpicker::setPickerForTest(nullptr);
    return git_fixture::cleanupRepoDir(repoA, true) &&
           git_fixture::cleanupRepoDir(repoB, true);
})

// ---- ⑤ submodule 降级 status（F-12③）----

REGISTER_TEST(gitSubmoduleStatusNotReported, "F-12③：gitlink submodule 内部改动不误报外层 status 条目", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_sub_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto outer = root / "外层仓";
    std::filesystem::create_directories(outer);
    git_fixture::initRepo(outer);
    // 子目录伪装 submodule：.gitmodules + gitlink index 条目（mode 0160000）+
    // 子目录内乱放的文件（若误递归即报 untracked）
    const auto sub = outer / "子模块";
    std::filesystem::create_directories(sub);
    git_fixture::commitAll(outer, {{".gitmodules",
                                    "[submodule \"子模块\"]\n\tpath = 子模块\n\turl = ../fake.git\n"},
                                   {"readme.txt", "外层\n"}});
    { std::ofstream(sub / "内部.txt", std::ios::binary) << "内部改动\n"; }

    git_repository* repo = nullptr;
    DOCTEST_REQUIRE(git_repository_open_ext(&repo,
                                            platform::git::pathToUtf8(outer).c_str(),
                                            GIT_REPOSITORY_OPEN_NO_SEARCH,
                                            nullptr) >= 0);
    git_index* index = nullptr;
    DOCTEST_REQUIRE(git_repository_index(&index, repo) >= 0);
    git_oid fakeOid;
    DOCTEST_REQUIRE(git_oid_fromstr(&fakeOid,
                                    "0123456789abcdef0123456789abcdef01234567") >= 0);
    git_index_entry entry{};
    entry.path = "子模块";
    entry.mode = 0160000;  // GIT_FILEMODE_COMMIT（gitlink）
    entry.id = fakeOid;
    DOCTEST_REQUIRE(git_index_add(index, &entry) >= 0);
    DOCTEST_REQUIRE(git_index_write(index) >= 0);
    git_index_free(index);
    git_repository_free(repo);

    platform::git::Libgit2Scope nested;  // refcount 嵌套安全（用例头已 init）
    auto opened = platform::git::Repo::open(outer);
    DOCTEST_REQUIRE(opened.isOk());
    auto status = opened.value().status();
    DOCTEST_REQUIRE(status.isOk());
    for (const auto& e : status.value().entries) {
        DOCTEST_CHECK_MESSAGE(e.path.rfind("子模块", 0) != 0,
                              "submodule 内部不应误报外层条目: " + e.path);
    }
    bool hasReadme = false;
    for (const auto& e : status.value().entries) {
        if (e.path == "readme.txt") hasReadme = true;
    }
    DOCTEST_CHECK_FALSE(hasReadme);  // 外层干净：readme 已提交无改动
    return git_fixture::cleanupRepoDir(outer, true);
})

// ---- F-11③ 超限占位短路（模型面：零内容读入）----

REGISTER_TEST(gitOversizePlaceholderNoRead, "F-11③：超 1 MiB 占位短路——state=Oversize、不产出行、maxLineLen=0", {
    const auto root = test_support::makeTempDir("devdesk_git_oversize_", "root");
    const auto big = root / "大文件.txt";
    {
        std::ofstream f(big, std::ios::binary);
        const std::string chunk(1024, 'x');
        for (int i = 0; i < 1024 + 16; ++i) f << chunk;  // > 1 MiB
    }
    const auto fc = module::git::FileContent::readFromDisk(big);
    DOCTEST_CHECK(fc.state == module::git::ContentState::Oversize);
    DOCTEST_CHECK(fc.lines.empty());
    DOCTEST_CHECK(fc.maxLineLen == 0);
    return git_fixture::cleanupRepoDir(root, true);
})
