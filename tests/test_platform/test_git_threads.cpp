// tests/test_platform/test_git_threads.cpp
// 线程安全边界 POC（need:8 批次一；方案 §3 三项验证点）：
//   ③ per-thread handle 并发 open+status+diff 1000 轮 ×2 线程无崩全绿；
//   ② git_repository_open 单次成本实测（per-task open vs 缓存的判定输入，>1ms 倾向缓存）；
//   ⑤ 连续 status 后 .git/index 不再写盘（watcher 循环事件收敛的等价断言——本批次
//      无 watcher 依赖项，方案 §2.2 自触发收敛机制）。
// 夹具：git_fixture.h libgit2 API 本地建仓（离线）。清理纪律同 test_git_raii.cpp
//（repo 句柄析构后才删目录）。
#include "doctest.h"
#include "test_support.h"

#include "git_fixture.h"
#include "platform/git/git_libgit2.h"
#include "platform/git/git_repo.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using platform::git::DiffBasis;
using platform::git::Libgit2Scope;
using platform::git::Repo;

namespace {

fs::path makeThreadRepo(const std::string& tag) {
    const fs::path dir = test_support::makeTempDir("devdesk_git_threads_", tag);
    git_fixture::initRepo(dir);
    git_fixture::commitAll(dir, {
        {"a.txt", "第一行\n第二行\n"},
        {"b.txt", "内容\n"},
        {"c.txt", "更多\n"},
    });
    return dir;
}

} // namespace

REGISTER_TEST(gitThreadsConcurrent2000Rounds, "双线程各自 handle 并发 open+status+diff 1000 轮无崩", {
    Libgit2Scope scope;  // init 在主线程先于 worker（refcount ≥1 贯穿全程）
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeThreadRepo("concurrent");

        std::atomic<int> okRounds{0};
        auto worker = [&](int rounds) {
            for (int i = 0; i < rounds; ++i) {
                auto r = Repo::open(dir);
                if (r.isFail()) return;
                Repo repo = std::move(r.value());
                auto s = repo.status();
                auto d = repo.diff(DiffBasis::WorkdirToHead);
                if (s.isOk() && d.isOk()) {
                    okRounds.fetch_add(1);
                } else {
                    return;
                }
            }
        };

        const auto start = std::chrono::steady_clock::now();
        std::thread t1(worker, 1000);
        std::thread t2(worker, 1000);
        t1.join();
        t2.join();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
        std::cout << "[git_threads] 2 threads x 1000 rounds open+status+diff took "
                  << elapsedMs << " ms\n";
        DOCTEST_CHECK_EQ(okRounds.load(), 2000);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitThreadsOpenCostMeasured, "git_repository_open 单次成本实测（per-task vs 缓存判定输入）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeThreadRepo("opencost");

        // 预热 20 次（首次触碰 stat cache / OS 文件缓存）
        for (int i = 0; i < 20; ++i) {
            auto r = Repo::open(dir);
            DOCTEST_CHECK(r.isOk());
        }
        constexpr int kRounds = 200;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kRounds; ++i) {
            auto r = Repo::open(dir);
            DOCTEST_CHECK(r.isOk());
        }
        const auto avgUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count() / kRounds;
        // 判定阈值（方案 §3）：open < 1ms → per-task open 成立；> 1ms → 批次二换 worker
        // 内缓存 handle。此处只记录实测值供决策，不设硬门。
        std::cout << "[git_threads] open avg cost " << avgUs
                  << " us/call (threshold 1000 us)\n";
        DOCTEST_CHECK(avgUs > 0);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})

REGISTER_TEST(gitThreadsStatusIndexConverges, "连续 100 次 status 后 .git/index 不再写盘（watcher 收敛等价断言）", {
    Libgit2Scope scope;
    fs::path dir;
    const bool ok = [&]() -> bool {
        dir = makeThreadRepo("converge");
        auto openR = Repo::open(dir);
        DOCTEST_CHECK(openR.isOk());
        Repo repo = std::move(openR.value());

        // 最坏情况先造一次显式落盘：UPDATE_INDEX 强制 stat cache 回写（§2.2 唯一写事件源）
        {
            git_repository* raw = nullptr;
            DOCTEST_CHECK(git_repository_open_ext(&raw, platform::git::pathToUtf8(dir).c_str(),
                                                  GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr) == 0);
            git_status_options opts = GIT_STATUS_OPTIONS_INIT;
            opts.flags = GIT_STATUS_OPT_UPDATE_INDEX;
            git_status_list* list = nullptr;
            DOCTEST_CHECK(git_status_list_new(&list, raw, &opts) == 0);
            git_status_list_free(list);
            git_repository_free(raw);
        }

        const fs::path indexPath = dir / ".git" / "index";
        std::error_code ec;
        const auto before = fs::last_write_time(indexPath, ec);
        DOCTEST_CHECK(!ec);

        for (int i = 0; i < 100; ++i) {
            auto s = repo.status();
            DOCTEST_CHECK(s.isOk());
        }
        const auto after = fs::last_write_time(indexPath, ec);
        DOCTEST_CHECK(!ec);
        // 100 次只读 status 期间 index mtime 恒定 = 无循环写盘 = watcher 不会自触发
        DOCTEST_CHECK(before == after);
        return true;
    }();
    return git_fixture::cleanupRepoDir(dir, ok);
})
