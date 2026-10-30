// tests/test_platform/test_process_runner.cpp
// platform::process_runner 真实现覆盖（POSIX 族：macos posix_spawn / posix fork+exec）：
//   ① 基础契约：echo 回显 / stderr 捕获 / stdin 输入 / 超时 SIGKILL。
//   ② 大输出不互锁回归（3d59d076 病灶：>64KB 输出必须排空优先）。
//   ③ 孙进程持写端收尸后宽限（债-20 ①）：主进程退出后孙进程占住 stdout 写端时，
//      EOF 拖延必须被宽限上限兜住，不能随孙进程挂满全程。
//   ④ POLLERR/POLLNVAL 排水、ECHILD 口径：触发条件进程内不可构造（管道 POLLERR
//      罕见、ECHILD 需全局信号干预），不写假测试，靠与 macos 版同构对齐 + 代码评审，
//      见 docs/plans/2026-10-31-debt20-process-runner-tests.md。
// Windows 侧：本批用例依赖 sh 语义（sleep/重定向），win 版的对应待办不在本批，
// 用例整体空转（win 回归另行登记）。
#include "doctest.h"

#include "platform/process_runner.h"

#include <chrono>
#include <string>
#include <vector>

#if !defined(_WIN32)

namespace {

using platform::ProcessResult;
using platform::systemProcessRunner;

struct RunOut {
    core::Result<ProcessResult> r;
    long elapsedMs = 0;
};

RunOut runShell(const std::string& script, const std::string& stdinText = "",
                long timeoutMs = 0) {
    const auto t0 = std::chrono::steady_clock::now();
    RunOut out;
    out.r = systemProcessRunner()({platform::defaultShellPath(), "-c", script},
                                  stdinText, "", timeoutMs);
    out.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    return out;
}

} // namespace

REGISTER_TEST(pr_basic_contract,
              "process_runner 基础契约：回显/stderr/stdin/超时杀（POSIX 真实现）", {
    auto runner = systemProcessRunner();

    auto r1 = runner({platform::defaultShellPath(), "-c", "echo runner_ok"}, "", "", 5000);
    DOCTEST_REQUIRE(r1.isOk());
    DOCTEST_CHECK(r1.value().exitCode == 0);
    DOCTEST_CHECK(r1.value().stdoutText.find("runner_ok") != std::string::npos);

    auto r2 = runner({platform::defaultShellPath(), "-c", "echo err_line 1>&2"}, "", "", 5000);
    DOCTEST_REQUIRE(r2.isOk());
    DOCTEST_CHECK(r2.value().stderrText.find("err_line") != std::string::npos);

    auto r3 = runner({platform::defaultShellPath(), "-c", "cat"}, "stdin_payload", "", 5000);
    DOCTEST_REQUIRE(r3.isOk());
    DOCTEST_CHECK(r3.value().stdoutText == "stdin_payload");

    auto r4 = runner({platform::defaultShellPath(), "-c", "sleep 10"}, "", "", 400);
    DOCTEST_REQUIRE(r4.isOk());
    DOCTEST_CHECK(r4.value().timedOut);
    return true;
})

REGISTER_TEST(pr_big_output_no_interlock,
              "process_runner 大输出不互锁（>64KB 排空优先回归）", {
    // 200KB >> 管道缓冲；修坏排空优先会让本用例挂死或截断
    auto out = runShell("yes runner_pad | head -c 200000", "", 10000);
    DOCTEST_REQUIRE(out.r.isOk());
    DOCTEST_CHECK(out.r.value().stdoutText.size() == 200000);
    return true;
})

REGISTER_TEST(pr_grandchild_writer_grace,
              "process_runner 孙进程持写端：收尸后 EOF 宽限兜底（债-20 ①）", {
    // (sleep 5)& 持住 stdout 写端；主 shell 立即退出输出 done。
    // 修复前：收尸后无限拖 EOF → 随孙进程挂满 5s（红：elapsed >= 4500）。
    // 修复后：宽限内拿满 done 即返回（elapsed ~2s 量级）。
    auto out = runShell("(sleep 5) & echo done", "", 0);
    DOCTEST_REQUIRE(out.r.isOk());
    DOCTEST_CHECK(out.r.value().stdoutText.find("done") != std::string::npos);
    DOCTEST_CHECK(out.elapsedMs < 4500);
    return true;
})

#else

REGISTER_TEST(pr_win_placeholder, "process_runner win 侧用例另行登记", {
    DOCTEST_CHECK(true);
    return true;
})

#endif
