// tests/test_platform/test_git_search.cpp
// git 管理器 v2 全局搜索（need:14）：匹配模型纯函数——asciiFold 折叠 /
// matchLines 子串命中与 5 条上限 / truncateAtCodepoint 码点边界截断。
// 收口修复批（P1-2/P2-5）增补控制器级两条：jumpToHit 重复点击高亮保持 /
// onStop 扫描面复位。控制器驱动协议同 test_git_refresh（picker 注入+真 worker）。
#include "doctest.h"
#include "test_support.h"

#include "module_git/git_page_controller.h"
#include "module_git/git_search_model.h"
#include "module_git/git_worker.h"
#include "platform/folder_picker.h"
#include "platform/git/git_libgit2.h"

#include "git_fixture.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using module::git::GitPageController;
using module::git::GitPagePhase;
using module::git::GitWorker;
using module::git::SearchHit;
using module::git::asciiFold;
using module::git::matchLines;
using module::git::truncateAtCodepoint;

namespace {
constexpr const char* kEllipsis = "\xE2\x80\xA6";  // … U+2026
constexpr const char* kCjk = "\xE4\xB8\x80";       // 一 U+4E00（3 字节）
} // namespace

REGISTER_TEST(git_search_asciifold, "git_search: asciiFold ASCII 折叠与非 ASCII 原样", {
    DOCTEST_CHECK(asciiFold("HeLLo-World_123") == "hello-world_123");
    DOCTEST_CHECK(asciiFold("") == "");
    // 非 ASCII（拉丁扩展 / CJK）不折叠
    DOCTEST_CHECK(asciiFold("\xC3\x84pfel") == "\xC3\x84pfel");  // Äpfel
    DOCTEST_CHECK(asciiFold(std::string("中文ABC")) == "中文abc");
    return true;
})

REGISTER_TEST(git_search_match_basic, "git_search: matchLines 子串命中与 1 起行号", {
    const std::vector<std::string> lines = {"Hello World", "foo hello bar", "nope",
                                            "HELLO"};
    const auto hits = matchLines(lines, "hello");
    DOCTEST_REQUIRE(hits.size() == 3);
    DOCTEST_CHECK(hits[0].line == 1);
    DOCTEST_CHECK(hits[1].line == 2);
    DOCTEST_CHECK(hits[2].line == 4);
    DOCTEST_CHECK_FALSE(hits[0].moreInFile);
    return true;
})

REGISTER_TEST(git_search_nonascii_nofold, "git_search: 非 ASCII 不折叠（Ä/ä 不互匹配）", {
    const std::vector<std::string> lines = {"\xC3\x84pfel", "\xC3\xA4pfel"};  // Äpfel / äpfel
    const auto hits = matchLines(lines, "\xC3\xA4pfel");  // 搜 äpfel
    DOCTEST_REQUIRE(hits.size() == 1);
    DOCTEST_CHECK(hits[0].line == 2);
    return true;
})

REGISTER_TEST(git_search_per_file_cap, "git_search: 单文件 5 条上限 + moreInFile 收口", {
    const std::vector<std::string> seven(7, "x");
    auto hits = matchLines(seven, "x");
    DOCTEST_REQUIRE(hits.size() == 5);
    DOCTEST_CHECK(hits.back().moreInFile);
    const std::vector<std::string> four(4, "x");
    hits = matchLines(four, "x");
    DOCTEST_REQUIRE(hits.size() == 4);
    DOCTEST_CHECK_FALSE(hits.back().moreInFile);
    return true;
})

REGISTER_TEST(git_search_empty_needle, "git_search: 空 needle 返回空结果", {
    const std::vector<std::string> lines = {"a", "b"};
    DOCTEST_CHECK(matchLines(lines, "").empty());
    return true;
})

REGISTER_TEST(git_search_preview_cjk_boundary, "git_search: preview 码点边界截断（不切半个多字节字符）", {
    // 10 CJK(30B) + NEEDLE(6B) + 10 CJK(30B)：窗内放得下 → 全行 preview，无省略号
    std::string line;
    for (int i = 0; i < 10; ++i) line += kCjk;
    line += "NEEDLE";
    for (int i = 0; i < 10; ++i) line += kCjk;
    size_t start = 0, len = 0;
    const std::string preview = truncateAtCodepoint(line, 30, 6, 40, &start, &len);
    DOCTEST_CHECK(preview == line);
    DOCTEST_CHECK(start == 30);
    DOCTEST_CHECK(len == 6);
    return true;
})

REGISTER_TEST(git_search_preview_ellipsis, "git_search: preview 前后省略号与 matchStart 平移", {
    // 60 CJK(180B) + NEEDLE(6B) + 60 CJK(180B)：前后各留 40 码点，两侧省略号
    std::string line;
    for (int i = 0; i < 60; ++i) line += kCjk;
    line += "NEEDLE";
    for (int i = 0; i < 60; ++i) line += kCjk;
    size_t start = 0, len = 0;
    const std::string preview = truncateAtCodepoint(line, 180, 6, 40, &start, &len);
    // preview = … + 40 CJK + NEEDLE + 40 CJK + …
    DOCTEST_CHECK(preview.rfind(kEllipsis, 0) == 0);
    DOCTEST_CHECK(preview.size() == 3 + 120 + 6 + 120 + 3);
    DOCTEST_CHECK(start == 3 + 120);
    DOCTEST_CHECK(len == 6);
    // 窗左界落在 CJK 序列起点（不切半个字符）：省略号后首字节是码点起始
    const unsigned char c = static_cast<unsigned char>(preview[3]);
    DOCTEST_CHECK((c & 0xC0) != 0x80);
    return true;
})

REGISTER_TEST(git_search_preview_at_start, "git_search: 命中在行首无前省略号（matchStart 不平移）", {
    std::string line = "NEEDLE";
    for (int i = 0; i < 60; ++i) line += kCjk;
    size_t start = 1, len = 0;
    const std::string preview = truncateAtCodepoint(line, 0, 6, 40, &start, &len);
    DOCTEST_CHECK(preview.rfind("NEEDLE", 0) == 0);
    DOCTEST_CHECK(start == 0);
    DOCTEST_CHECK(len == 6);
    return true;
})

// ---- 收口修复批（need:14 审计 P1-2/P2-5）：控制器级 ----

namespace {

const std::string* g_searchPickerPath = nullptr;
core::Result<std::string> pickSearchFixed(const std::string&, const std::string&) {
    return core::Result<std::string>::ok(*g_searchPickerPath);
}

// 排空 worker 回投直到条件满足（composePump 即生产帧泵协议，同 test_git_refresh）
template <typename Pred>
bool pumpUntilSearch(GitPageController& c, Pred pred, int maxMs = 5000) {
    for (int i = 0; i < maxMs / 5; ++i) {
        c.composePump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    c.composePump();
    return pred();
}

} // namespace

REGISTER_TEST(gitSearchRepeatJumpHighlightKept, "git_search: 重复 jumpToHit 同文件已 Ready 高亮不灭（P1-2）", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_search_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto repoDir = root / "跳转仓";
    std::filesystem::create_directories(repoDir);
    git_fixture::initRepo(repoDir);
    git_fixture::commitAll(repoDir, {{"数据.txt", "第一版\n第二版\n第三版\n"}});
    const std::string repoUtf8 = platform::git::pathToUtf8(repoDir);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    g_searchPickerPath = &repoUtf8;
    platform::folderpicker::setPickerForTest(&pickSearchFixed);
    c.beginPickRepo();
    DOCTEST_CHECK(pumpUntilSearch(c, [&] { return c.phase() == GitPagePhase::Ready; }));

    c.setSearchDraft("第二版");
    c.submitSearchNow();
    DOCTEST_CHECK(pumpUntilSearch(c, [&] {
        return c.searchState() == GitPageController::GitSearchState::Done &&
               !c.searchResults().empty();
    }));
    DOCTEST_REQUIRE(c.searchResults().size() >= 1);
    DOCTEST_CHECK(c.searchResults()[0].line == 2);

    // 首跳：文件未加载 → Ready 后高亮落值（F-08② 基线，修前应绿）
    c.jumpToHit(c.searchResults()[0]);
    DOCTEST_CHECK(pumpUntilSearch(c, [&] {
        return c.contentState() == module::git::RightPaneState::Ready &&
               c.contentHighlightLine() > 0;
    }));
    DOCTEST_CHECK(c.contentHighlightLine() == 2);

    // 重复点击同一命中：同文件已 Ready，requestContent 幂等早退——高亮必须仍在
    //（修前：selectTreeFile 先清 0、早退分支不经过 Ready 落值点 → 红椭圆）
    c.jumpToHit(c.searchResults()[0]);
    DOCTEST_CHECK(c.contentHighlightLine() == 2);
    return true;
})

REGISTER_TEST(gitSearchOnStopResetScanningFace, "git_search: 切模块 onStop 复位扫描面且同词可重扫（P2-5）", {
    const platform::git::Libgit2Scope libgit2;
    const auto root = test_support::makeTempDir("devdesk_git_search_", "root");
    const test_support::ScopedEnv dataRoot("DEVDESK_DATA_ROOT",
                                           platform::git::pathToUtf8(root).c_str());
    const auto repoDir = root / "停机仓";
    std::filesystem::create_directories(repoDir);
    git_fixture::initRepo(repoDir);
    git_fixture::commitAll(repoDir, {{"a.txt", "目标词出现\n"}});
    const std::string repoUtf8 = platform::git::pathToUtf8(repoDir);

    GitWorker worker;
    worker.start();
    GitPageController c(&worker);
    g_searchPickerPath = &repoUtf8;
    platform::folderpicker::setPickerForTest(&pickSearchFixed);
    c.beginPickRepo();
    DOCTEST_CHECK(pumpUntilSearch(c, [&] { return c.phase() == GitPagePhase::Ready; }));

    // 提交后不泵：状态同步置 Scanning（回投尚未排空）
    c.setSearchDraft("目标词");
    c.submitSearchNow();
    DOCTEST_CHECK(c.searchState() == GitPageController::GitSearchState::Scanning);

    // 取消③切模块：onStop 必须复位扫描面（修前：仅 ++seq+清回投箱 → 永挂 Scanning）
    c.onStop();
    DOCTEST_CHECK(c.searchState() == GitPageController::GitSearchState::None);
    DOCTEST_CHECK_FALSE(c.searchPopupVisible());
    DOCTEST_CHECK(c.searchDraft() == "目标词");  // 草稿保留：回模块可直接重查

    // 同词重扫不被幂等堵死（修前：query 未清 → submitSearch 早退 → 永不 Done）
    c.submitSearchNow();
    DOCTEST_CHECK(pumpUntilSearch(c, [&] {
        return c.searchState() == GitPageController::GitSearchState::Done;
    }));
    DOCTEST_CHECK(c.searchResults().size() == 1);
    return true;
})
