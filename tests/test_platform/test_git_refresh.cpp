// tests/test_platform/test_git_refresh.cpp
// git 管理器批次四（need:11）：刷新联动 + 节流 + 过期条 + N10 会话恢复 + submodule
// 降级 status（F-09④ / F-12③ / F-13①②③④）。
//   ① watcher 过滤表逐行（技术方案 §2.2：工作树/索引元数据 → 脏；objects/gc → 忽略；
//     Unknown → 全量重扫兜底）——纯函数 classifyWatchEvent；
//   ② 1s 节流窗：脏标记 trailing 到窗发一次；窗内不重复投（F-09④）；
//   ③ 过期条：元数据/溢出/命中打开文件 → staleBar 置位；reloadStale 清除（F-13③）；
//   ④ N10：切换仓库后重选同仓恢复 视图形态（树/平铺）+右栏打开文件与视图类型（仓/模式/打开文件）；
//   ⑤ submodule gitlink 仓 status 不误报内部改动（F-12③，EXCLUDE_SUBMODULES）；
//   ⑥ F-09（need:291）：刷新重建行序列保 选中/目录展开位图/组折叠位图。
// 控制器驱动：picker 注入缝 + 真 GitWorker 线程 + composePump 排空回投（协议同生产）。
#include "doctest.h"
#include "test_support.h"

#include "module_git/git_change_list_model.h"
#include "module_git/git_content_model.h"
#include "module_git/git_page_controller.h"
#include "module_git/git_repo_store.h"
#include "module_git/git_worker.h"
#include "platform/folder_picker.h"
#include "platform/git/git_libgit2.h"

#include "git_fixture.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <ios>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
    // a.txt 置为真实变更条目（选中的打开文件须是变更成员——N8：刷新后选中
    // 非变更文件会被清选中+右栏回空态；watcher Modified a.txt 事件语义=文件真改）
    test_support::writeTextFile(repoDir / "a.txt", "第二版\n");
    const std::string repoUtf8 = platform::git::pathToUtf8(repoDir);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK(openRepoViaPicker(c, repoUtf8));
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
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
    // （先静默出窗，避免 pending 脏标记在本段中途触发 status 干扰时序；
    //   need:291 后「打开文件」动线=selectChange——中栏平铺/树行点击同链路）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.composePump();
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
    c.selectChange("a.txt");
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.diffState() == RightPaneState::Ready;
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
        return c.diffState() == RightPaneState::Ready;
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

REGISTER_TEST(gitN10SessionRestore, "N10：切换仓库后重选同仓恢复 视图形态+右栏打开文件+视图类型；异仓全新会话", {
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
    c.setViewMode(module::git::ChangeViewMode::Flat);
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
    DOCTEST_CHECK(c.rightTab() == GitPageController::RightTab::Diff);  // diff 动线（D5-A 判据）

    // 切走（N2 空态页）→ 重选同仓：恢复视图形态 + 打开文件 + 视图类型
    //（树/平铺切换记忆至会话不落盘，设计 §6.3；N10 跨仓恢复沿用同一位）
    c.switchRepo();
    DOCTEST_CHECK(c.phase() == GitPagePhase::Empty);
    DOCTEST_CHECK(openRepoViaPicker(c, platform::git::pathToUtf8(repoA)));
    DOCTEST_CHECK(c.viewMode() == module::git::ChangeViewMode::Flat);
    DOCTEST_CHECK(c.selectedPath() == "甲.txt");
    DOCTEST_CHECK(c.rightTab() == GitPageController::RightTab::Diff);  // diff 动线（D5-A 判据）
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.diffState() == RightPaneState::Ready ||
               c.diffState() == RightPaneState::Placeholder;
    }));

    // 切走 → 重选异仓：全新会话（不携带旧选中）
    c.switchRepo();
    DOCTEST_CHECK(openRepoViaPicker(c, platform::git::pathToUtf8(repoB)));
    DOCTEST_CHECK(c.selectedPath().empty());
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

// ---- ⑥ F-09（need:291）：刷新重建行序列，列表 UI 状态位保持 ----

REGISTER_TEST(gitRefreshKeepsListUiState, "F-09：刷新重建行序列保 选中/目录展开位图/组折叠位图（need:291）", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_listui_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto repoDir = root / "保位仓";
    std::filesystem::create_directories(repoDir);
    git_fixture::initRepo(repoDir);
    git_fixture::commitAll(repoDir, {{"根.txt", "v1\n"}, {"docs/指南.md", "v1\n"}});
    // 工作区改动（均 unstaged，tracked 派生目录——纯派生不走 worker lazy）：
    // 根内文件 + docs 子目录文件 → unstaged 组内派生 docs 目录节点
    test_support::writeTextFile(repoDir / "根.txt", "v2\n");
    test_support::writeTextFile(repoDir / "docs" / "指南.md", "v2\n");
    const std::string repoUtf8 = platform::git::pathToUtf8(repoDir);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK(openRepoViaPicker(c, repoUtf8));
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));

    module::git::GitChangeListModel& list = c.listModel();
    // tracked 派生目录默认展开（设计 §7 N2）→ 显式折叠验证记忆位图（纯重建不走 lazy）
    c.toggleDir("docs");
    DOCTEST_CHECK_FALSE(list.isExpanded("docs"));
    // 选中根内文件 + 右栏 diff（主动线）
    c.selectChange("根.txt");
    DOCTEST_CHECK(pumpUntil(c, [&] { return c.diffState() == RightPaneState::Ready; }));
    // 折叠未暂存组（选中行在组内——设计 §6.4 折叠组含选中行=选中保持前提）
    list.setGroupCollapsed(module::git::ChangeListRow::Group::Unstaged, true);
    DOCTEST_CHECK(list.isGroupCollapsed(module::git::ChangeListRow::Group::Unstaged));

    // 手动刷新（出开仓 1s 节流窗后投一次，F-09④ 同闸）
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    c.composePump();
    c.refresh();
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));

    // 三位保持：选中 + 右栏动线、目录展开位图、组折叠位图
    DOCTEST_CHECK(c.selectedPath() == "根.txt");
    DOCTEST_CHECK(c.rightTab() == GitPageController::RightTab::Diff);  // diff 动线（D5-A 判据）
    DOCTEST_CHECK_FALSE(list.isExpanded("docs"));  // 折叠记忆跨刷新保留
    DOCTEST_CHECK(list.isGroupCollapsed(module::git::ChangeListRow::Group::Unstaged));
    // 折叠组子树不出行（组头仍在，行序列确已重建）
    bool sawFileRow = false;
    bool sawUnstagedHeader = false;
    for (const auto& r : list.rows()) {
        if (r.kind == module::git::ChangeListRow::Kind::File) sawFileRow = true;
        if (r.kind == module::git::ChangeListRow::Kind::GroupHeader &&
            r.group == module::git::ChangeListRow::Group::Unstaged) {
            sawUnstagedHeader = true;
        }
    }
    DOCTEST_CHECK_FALSE(sawFileRow);
    DOCTEST_CHECK(sawUnstagedHeader);
    // 重新展开组 + docs 目录 → 子行凭折叠/展开记忆位图回归
    list.setGroupCollapsed(module::git::ChangeListRow::Group::Unstaged, false);
    c.toggleDir("docs");  // 撤销 :334 的显式折叠（记忆位图往返）
    bool sawGuide = false;
    for (const auto& r : list.rows()) {
        if (r.kind == module::git::ChangeListRow::Kind::File &&
            r.relPath == "docs/指南.md") {
            sawGuide = true;
        }
    }
    DOCTEST_CHECK(sawGuide);
    worker.stop();
    platform::folderpicker::setPickerForTest(nullptr);
    return git_fixture::cleanupRepoDir(repoDir, true);
})

// ---- ⑦ bug#296：U 行点开 = 全增 diff（真仓实测；顶层未跟踪 + lazy 子文件）----
// 老板 2026-11-17 装机版打回：「.project 标记U 点开显示文件已无变更。。。又没人修改
// 到底变了还是没变」。根因：未跟踪行在 workdir↔HEAD diff 里无 delta 可匹配（F-09
// 不递归：lazy 子文件必然无；顶层靠 pathspec 命中）→ 零匹配落「文件已无变更，列表
// 将刷新」误报。契约：U 行点击一律合成全增 diff，禁误报文案。
REGISTER_TEST(gitUntrackedClickFullAddDiff, "bug296：点 U 行出全增 diff（顶层+lazy 子文件），禁「文件已无变更」误报", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_refresh_", "bug296");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto repoDir = root / "未跟踪仓";
    std::filesystem::create_directories(repoDir);
    git_fixture::initRepo(repoDir);
    git_fixture::commitAll(repoDir, {{"a.txt", "第一版\n"}});
    // 对照档前置：a.txt 置真实变更（否则它不在变更列表里，点开零匹配文案=正确行为）
    test_support::writeTextFile(repoDir / "a.txt", "第二版\n");
    // 顶层未跟踪文件（老板场景本体：.project）
    test_support::writeTextFile(repoDir / ".project",
                                "<?xml version=\"1.0\"?>\n<nature>eclipse</nature>\n");
    // 未跟踪目录 + 子文件（D3-A lazy 展开层——无 delta 可匹配重灾区）
    test_support::writeTextFile(repoDir / "新模块" / "note.md", "新内容\n");
    const std::string repoUtf8 = platform::git::pathToUtf8(repoDir);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK(openRepoViaPicker(c, repoUtf8));
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));

    // 档①：顶层未跟踪 .project 行 → 全增 diff（2 行、origin '+'、newNo 1 起）
    c.selectChange(".project");
    DOCTEST_CHECK(pumpUntil(c, [&] { return c.diffState() == RightPaneState::Ready; }));
    DOCTEST_CHECK(c.diffModel().placeholder().empty());  // bug296：禁误报文案
    DOCTEST_CHECK_EQ(c.diffModel().patch().newPath, ".project");
    DOCTEST_CHECK(c.diffModel().patch().isUntracked);
    if (c.diffModel().patch().hunks.size() == 1) {
        DOCTEST_CHECK_EQ(c.diffModel().patch().hunks[0].lines.size(), 2);
        if (c.diffModel().patch().hunks[0].lines.size() == 2) {
            DOCTEST_CHECK_EQ(c.diffModel().patch().hunks[0].lines[0].origin, '+');
            DOCTEST_CHECK_EQ(c.diffModel().patch().hunks[0].lines[1].newNo, 2);
        }
    } else {
        DOCTEST_CHECK_EQ(c.diffModel().patch().hunks.size(), 1);
    }

    // 档②：顶层 untracked 目录自动枚举（裁决④）→ 子文件行直接在列 → 同契约
    DOCTEST_CHECK(pumpUntil(c, [&] {
        for (const auto& r : c.listRows()) {
            if (r.kind == module::git::ChangeListRow::Kind::File &&
                r.relPath == "新模块/note.md") {
                return true;
            }
        }
        return false;
    }));
    // 裁决④+原则令：「新模块」目录行=纯结构装置（无徽标非 lazy，计数=文件行数），
    // 不再是带 U 徽标的 untracked 目录条目行
    for (const auto& r : c.listRows()) {
        if (r.kind == module::git::ChangeListRow::Kind::Dir &&
            r.relPath == "新模块") {
            DOCTEST_CHECK_MESSAGE(!r.status.has_value(),
                                  "目录行不应带状态徽标（文件夹原则令）");
            DOCTEST_CHECK_MESSAGE(!r.lazy,
                                  "顶层条目枚举后的目录行=tracked 式派生结构目录");
        }
    }
    c.selectChange("新模块/note.md");
    DOCTEST_CHECK(pumpUntil(c, [&] { return c.diffState() == RightPaneState::Ready; }));
    DOCTEST_CHECK(c.diffModel().placeholder().empty());  // bug296 核心
    DOCTEST_CHECK_EQ(c.diffModel().patch().newPath, "新模块/note.md");
    DOCTEST_CHECK(c.diffModel().patch().isUntracked);
    if (c.diffModel().patch().hunks.size() == 1) {
        DOCTEST_CHECK_EQ(c.diffModel().patch().hunks[0].lines.size(), 1);
        if (!c.diffModel().patch().hunks[0].lines.empty()) {
            DOCTEST_CHECK_EQ(c.diffModel().patch().hunks[0].lines[0].origin, '+');
        }
    } else {
        DOCTEST_CHECK_EQ(c.diffModel().patch().hunks.size(), 1);
    }

    // 对照档：真实修改文件 → 正常 diff
    c.selectChange("a.txt");
    DOCTEST_CHECK(pumpUntil(c, [&] { return c.diffState() == RightPaneState::Ready; }));
    DOCTEST_CHECK(c.diffModel().placeholder().empty());
    DOCTEST_CHECK_EQ(c.diffModel().patch().hunks.size(), 1);

    worker.stop();
    platform::folderpicker::setPickerForTest(nullptr);
    return git_fixture::cleanupRepoDir(root, true);
})

// ---- ⑧ need:299：最近仓历史 LRU + 切仓清过滤 + 当前仓下拉点击幂等 ----

REGISTER_TEST(gitRecentRepoLruAndFilterReset, "need:299：开仓记录最近仓 LRU（≤8 去重新→旧，git_manager.json 持久化）；切仓清过滤词；当前仓下拉点击=收起不重载", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_lru_", "root");
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
    const std::string aUtf8 = platform::git::pathToUtf8(repoA);
    const std::string bUtf8 = platform::git::pathToUtf8(repoB);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK(openRepoViaPicker(c, aUtf8));
    DOCTEST_CHECK(pumpUntil(c, [&] { return !c.statusLoading(); }));
    // 开 A 即过滤（词挂在列表上）→ 切 B：词清空（§4.9 切仓=过滤词清空）
    c.setFilterDraft("甲");
    DOCTEST_CHECK(c.filterActive());
    DOCTEST_CHECK(openRepoViaPicker(c, bUtf8));
    // 校验→openRepo 异步链路完成（Ready 期 pumpUntil(Ready) 会秒过——等到切仓落地）
    DOCTEST_CHECK(pumpUntil(c, [&] {
        return c.repoPathUtf8() == bUtf8 && !c.statusLoading();
    }));
    DOCTEST_CHECK(c.filterDraft().empty());
    DOCTEST_CHECK_FALSE(c.filterActive());
    // LRU：B 置顶新→旧，A 随后；持久化 git_manager.json（≤8 条）
    const auto mem = module::git::GitRepoStore::load();
    DOCTEST_REQUIRE_EQ(mem.recentRepos.size(), static_cast<size_t>(2));
    DOCTEST_CHECK_EQ(mem.recentRepos[0], bUtf8);
    DOCTEST_CHECK_EQ(mem.recentRepos[1], aUtf8);
    // 当前仓下拉点击=收起幂等不重载（§6.10④）；Ready 保持
    c.setRepoDropdownVisible(true);
    c.openRecentRepo(bUtf8);
    DOCTEST_CHECK_FALSE(c.repoDropdownVisible());
    DOCTEST_CHECK(c.phase() == GitPagePhase::Ready);
    // 下拉行集：可见时 composePump 刷新（当前仓 exists+current 标记）
    c.setRepoDropdownVisible(true);
    c.composePump();
    DOCTEST_REQUIRE_EQ(c.recentRepoEntries().size(), static_cast<size_t>(2));
    DOCTEST_CHECK(c.recentRepoEntries()[0].current);   // B 当前
    DOCTEST_CHECK(c.recentRepoEntries()[0].exists);
    DOCTEST_CHECK_FALSE(c.recentRepoEntries()[1].current);
    c.setRepoDropdownVisible(false);
    worker.stop();
    platform::folderpicker::setPickerForTest(nullptr);
    return git_fixture::cleanupRepoDir(repoA, true) &&
           git_fixture::cleanupRepoDir(repoB, true);
})

// ---- ⑨ bug#296 真仓门控实证（DEVDESK_GIT_REAL_REPO=<真实仓路径>）----
// 用真实工作树验 U 行（真未跟踪文件）点开 = 全增 diff、禁「文件已无变更」误报；
// M 行（真修改文件）仍走真 delta；已跟踪未改文件为对照档（零匹配语义）。
// 只读契约：只有 status/diff/读盘，用例内不做任何 git 写（禁 add/commit/checkout/
// stash）。默认未设环境变量 → SKIP，CI 不红（真仓路径含本机未提交现状，不可作基线）。
REGISTER_TEST(gitUntrackedClickRealRepo,
              "bug296真仓(env门控)：工作树 U 文件行点开=全增 diff 禁「文件已无变更」；M 行真 delta；已跟踪未改对照档零匹配", {
    const char* envRepo = std::getenv("DEVDESK_GIT_REAL_REPO");
    if (envRepo == nullptr || *envRepo == '\0') {
        DOCTEST_SKIP("DEVDESK_GIT_REAL_REPO 未设置——真仓用例需显式指定仓路径"
                     "（CI 默认不跑）");
    }
    const std::filesystem::path realRepo = platform::git::utf8ToPath(envRepo);
    const bool isRepo = std::filesystem::exists(realRepo / ".git");
    DOCTEST_CHECK_MESSAGE(isRepo, std::string("不是 git 仓: ") + envRepo);
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_realrepo_", "bug296");
    // 数据根隔离：开仓记录/git_manager.json 写临时目录，不碰真实 %USERPROFILE%\DevDesk
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const std::string repoUtf8 = platform::git::pathToUtf8(realRepo);
    std::cout << "[bug296] real repo = " << repoUtf8 << std::endl;

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    DOCTEST_CHECK_MESSAGE(openRepoViaPicker(c, repoUtf8), "真仓打开失败");
    DOCTEST_CHECK_MESSAGE(pumpUntil(c, [&] { return !c.statusLoading(); }, 20000),
                          "status 扫描超时（20s）");

    auto pumpFor = [&](auto pred, int maxMs = 10000) {
        return pumpUntil(c, pred, maxMs);
    };
    auto rowsOfStatus = [&](platform::git::StatusKind kind) {
        std::vector<std::string> out;
        for (const auto& r : c.listRows()) {
            if (r.kind == module::git::ChangeListRow::Kind::File && r.status.has_value() &&
                *r.status == kind) {
                out.push_back(r.relPath);
            }
        }
        return out;
    };
    auto listedAsFile = [&](const std::string& rel) {
        for (const auto& r : c.listRows()) {
            if (r.kind == module::git::ChangeListRow::Kind::File && r.relPath == rel) return true;
        }
        return false;
    };

    // ① U 行（真未跟踪文件，含 untracked 目录自动枚举出的子文件行）：
    // 未跟踪目录首层枚举（裁决④）是 status 后的异步链——等 U 行集合稳定（两轮
    // 100ms 间隔数量不变）再收集，否则真仓的目录派生 U 文件行会漏采样
    for (int i = 0; i < 20; ++i) {
        const size_t before = rowsOfStatus(platform::git::StatusKind::Untracked).size();
        pumpUntil(c, [] { return false; }, 100);
        if (rowsOfStatus(platform::git::StatusKind::Untracked).size() == before) break;
    }
    // 点开 → 合成全增 diff；断言禁误报文案、全 '+'、newNo 1 起连续、oldNo 无
    const auto untracked = rowsOfStatus(platform::git::StatusKind::Untracked);
    DOCTEST_CHECK_MESSAGE(!untracked.empty(),
                          "真仓无未跟踪文件行——无法覆盖 U 行主症状");
    int uChecked = 0;
    for (const auto& rel : untracked) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(realRepo / platform::git::utf8ToPath(rel), ec);
        if (ec || sz > 1024u * 1024u) {  // >1MiB 走「文件过大」合法占位，另口径
            std::cout << "[bug296] skip U (oversize/err): " << rel << std::endl;
            continue;
        }
        ++uChecked;
        std::cout << "[bug296] U row: " << rel << " (" << sz << " bytes)" << std::endl;
        c.selectChange(rel);
        DOCTEST_CHECK_MESSAGE(
            pumpFor([&] {
                return c.diffState() == RightPaneState::Ready &&
                       c.diffModel().patch().newPath == rel;
            }),
            "U 行 diff 未就绪: " + rel);
        const std::string ph = c.diffModel().placeholder();
        DOCTEST_CHECK_MESSAGE(ph.empty(), "U 行误报文案 [" + ph + "]: " + rel);
        DOCTEST_CHECK_EQ(c.diffModel().patch().newPath, rel);
        DOCTEST_CHECK(c.diffModel().patch().isUntracked);
        DOCTEST_CHECK(c.diffModel().patch().binary || !c.diffModel().patch().hunks.empty());
        if (c.diffModel().patch().binary) {
            // 二进制 U（如 png/pool.db-shm）：走「二进制文件」占位，不是全增行——
            // 但同样禁「文件已无变更」误报（上面 ph.empty 已断言）
            std::cout << "[bug296] U binary placeholder OK: " << rel << std::endl;
            continue;
        }
        DOCTEST_CHECK_MESSAGE(c.diffModel().patch().hunks.size() == 1,
                              "U 行应按整文件合成为单 hunk: " + rel);
        const auto& lines = c.diffModel().patch().hunks[0].lines;
        DOCTEST_CHECK_MESSAGE(!lines.empty(), "U 行合成 diff 空: " + rel);
        for (size_t i = 0; i < lines.size(); ++i) {
            DOCTEST_CHECK_MESSAGE(lines[i].origin == '+', "非增行: " + rel);
            DOCTEST_CHECK_MESSAGE(lines[i].newNo.has_value() &&
                                      *lines[i].newNo == static_cast<int>(i) + 1,
                                  "newNo 非 1 起连续: " + rel);
            DOCTEST_CHECK_MESSAGE(!lines[i].oldNo.has_value(), "全增行不应有 oldNo: " + rel);
        }
        std::cout << "[bug296] U add-lines=" << lines.size() << std::endl;
    }
    DOCTEST_CHECK_MESSAGE(uChecked > 0, "真仓 U 行样本为空");

    // 红面成因证据（工作树只读替代——本单禁止还原产品源码）：直接走修复前的
    // workdir↔HEAD diff 管线，未跟踪文件零匹配 → 正是「文件已无变更」误报来源；
    // 上方 U 分支断言证明控制器已绕开该管线。二者同仓同文件，构成红/绿对照。
    {
        auto repo = platform::git::Repo::open(realRepo);
        DOCTEST_CHECK_MESSAGE(repo.isOk(), "直开真仓失败（红面证据无法取证）");
        if (repo.isOk()) {
            for (const auto& rel : untracked) {
                auto raw = repo.value().diff(platform::git::DiffBasis::WorkdirToHead, rel);
                DOCTEST_CHECK_MESSAGE(raw.isOk(), "原管线 diff 失败: " + rel);
                bool rawMatched = false;
                for (const auto& p : raw.value().patches) {
                    if (p.newPath == rel || p.oldPath == rel) rawMatched = true;
                }
                std::cout << "[bug296] pre-fix pipeline match(" << rel
                          << ") = " << (rawMatched ? "yes" : "NO(误报成因)") << std::endl;
                DOCTEST_CHECK_MESSAGE(!rawMatched,
                                      "未跟踪文件在原 diff 管线竟有 delta（红面成因不成立）: " + rel);
            }
        }
    }

    // ② M 行（真修改文件）：真 delta 不受 bug#296 修复影响（placeholder 空、hunks 非空）
    const auto modified = rowsOfStatus(platform::git::StatusKind::Modified);
    DOCTEST_CHECK_MESSAGE(!modified.empty(), "真仓无已修改文件行——M 行对照缺失");
    int mChecked = 0;
    for (const auto& rel : modified) {
        ++mChecked;
        std::cout << "[bug296] M row: " << rel << std::endl;
        c.selectChange(rel);
        DOCTEST_CHECK_MESSAGE(
            pumpFor([&] {
                return c.diffState() == RightPaneState::Ready &&
                       c.diffModel().patch().newPath == rel;
            }),
            "M 行 diff 未就绪: " + rel);
        DOCTEST_CHECK_MESSAGE(c.diffModel().placeholder().empty(), "M 行误报文案: " + rel);
        DOCTEST_CHECK_FALSE(c.diffModel().patch().isUntracked);
        DOCTEST_CHECK_MESSAGE(!c.diffModel().patch().hunks.empty(), "M 行无真 delta: " + rel);
    }
    DOCTEST_CHECK(mChecked > 0);

    // ③ 对照档：已跟踪且真未改文件 → 零匹配/无变更语义仍正确（不合成假全增）
    const std::string control = "CMakeLists.txt";
    DOCTEST_CHECK_MESSAGE(!listedAsFile(control), "对照文件意外出现在变更列表: " + control);
    c.selectChange(control);
    DOCTEST_CHECK_MESSAGE(pumpFor([&] { return !c.diffModel().placeholder().empty(); }),
                          "对照档零匹配结果未到达");
    const std::string cph = c.diffModel().placeholder();
    std::cout << "[bug296] control placeholder = [" << cph << "]" << std::endl;
    DOCTEST_CHECK_MESSAGE(cph.find("已无变更") != std::string::npos,
                          "对照档零匹配文案语义丢失: [" + cph + "]");
    DOCTEST_CHECK(c.diffModel().patch().hunks.empty());
    DOCTEST_CHECK_FALSE(c.diffModel().patch().isUntracked);

    worker.stop();
    platform::folderpicker::setPickerForTest(nullptr);
    // 只清临时数据根；真实仓一律不动（只读实证）
    return git_fixture::cleanupRepoDir(root, true);
})
