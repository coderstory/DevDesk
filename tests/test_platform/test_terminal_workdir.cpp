// tests/test_platform/test_terminal_workdir.cpp
// need328：mac 终端 spawn 目录传参链——resolveSpawnWorkDir 空值→$HOME、非空→原值。
// 端到端（设置目录→会话实际落盘 cwd）归打包态真机复测，此处锁纯函数解析语义。
// swift_bridge 为 mac 专用 TU（无 posix 兜底族），非 Apple 平台编空 TU（守卫先例
// 同 test_file_watcher.cpp）。
#if defined(__APPLE__)

#include "doctest.h"
#include "test_support.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <filesystem>
#include <unistd.h>

#include "platform/terminal/swift_bridge.h"

#include "platform/terminal/terminal_backend.h"

REGISTER_TEST(terminal_workdir_resolve,
              "terminal_workdir: spawn 目录解析——非空原值、空→$HOME（need328）", {
    // 非空 → 原值直传
    DOCTEST_REQUIRE_EQ(platform::resolveSpawnWorkDir("/tmp/devdesk-wd-check"),
                       std::string("/tmp/devdesk-wd-check"));
    // 空 → $HOME（读当前进程 env，不主动改写避免污染其它用例）
    const char* home = std::getenv("HOME");
    DOCTEST_REQUIRE(home != nullptr);
    DOCTEST_REQUIRE_EQ(platform::resolveSpawnWorkDir(""),
                       std::string(home));
    return true;
})

// 端到端（need328 验收：目录真实落盘）：真 spawn → 会话内 pwd 写文件 → 回读比对。
// 两分支各一次：非空目录落之、空 → $HOME。纯进程判据，无 UI 点击。
namespace {
// sanitizer 独立档整机慢化 2-10×（家族先例 ci-asan-wallclock-flakes：窗口型用例
// 专爆）→ 轮询窗口 ×5 加宽；常规档窗口不变防拖慢。CI ASan 档 flake（need348）
// 实证：usleep 被提前打断时迭代计数预算会坍缩（174ms 即挂），故预算按墙钟
// deadline 计——打断只烧掉一次睡眠，窗宽不受影响。
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define TERMINAL_WD_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define TERMINAL_WD_TEST_SANITIZED 1
#endif

// 状态驱动轮询：读到期望值即过（断言条件满足，不空等）；轮询上限 = 墙钟
// deadline（耐心非阈值，禁缩短等待）。超时返回最后读到的非空内容供断言
// 输出诊断；从未写出则返回空。
std::string pollPwdOut(const std::string& path, const std::string& expected) {
#if defined(TERMINAL_WD_TEST_SANITIZED)
    constexpr int kPollBudgetMs = 25000; // sanitize 慢化档：5s × 5
#else
    constexpr int kPollBudgetMs = 5000;
#endif
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kPollBudgetMs);
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
        usleep(100000);
        std::ifstream f(path);
        std::string line;
        if (f && std::getline(f, line) && !line.empty()) {
            if (line == expected) return line;
            last = line; // 非期望内容：继续等（禁提前死），留给断言做诊断
        }
    }
    return last;
}
} // namespace

REGISTER_TEST(terminal_workdir_e2e_spawn_chdir,
              "terminal_workdir: 端到端——spawn 带目录会话 pwd 落之、空→$HOME（need328）", {
    std::filesystem::create_directories("/tmp/devdesk-wd-e2e");
    // macOS /tmp → /private/tmp 符号链接：chdir 后 shell pwd 打物理路径，断言用 canonical
    const std::string wd = std::filesystem::canonical("/tmp/devdesk-wd-e2e").string();
    const char* cmd = "pwd > /tmp/devdesk-wd-e2e/pwd.out; echo __WD_DONE__\n";

    // ① 非空目录 → 会话 cwd 落该目录
    {
        auto s = platform::SwiftBridgeSession::spawn("/bin/zsh", 24, 80, wd);
        DOCTEST_REQUIRE(s != nullptr);
        DOCTEST_REQUIRE(s->write(reinterpret_cast<const uint8_t*>(cmd),
                                 static_cast<int32_t>(std::strlen(cmd)), false));
        std::string got = pollPwdOut(wd + "/pwd.out", wd);
        DOCTEST_REQUIRE_EQ(got, wd);
        s->stop();
    }
    // ② 空目录 → 解析成 $HOME
    {
        std::filesystem::remove(wd + "/pwd.out"); // 清 ① 残留，防读到旧值误判
        const char* home = std::getenv("HOME");
        DOCTEST_REQUIRE(home != nullptr);
        const std::string homeCanon =
            std::filesystem::canonical(home).string();
        auto s = platform::SwiftBridgeSession::spawn("/bin/zsh", 24, 80, "");
        DOCTEST_REQUIRE(s != nullptr);
        DOCTEST_REQUIRE(s->write(reinterpret_cast<const uint8_t*>(cmd),
                                 static_cast<int32_t>(std::strlen(cmd)), false));
        std::string got = pollPwdOut(wd + "/pwd.out", homeCanon);
        DOCTEST_REQUIRE_EQ(got, homeCanon);
        s->stop();
    }
    return true;
})

// need330：设置面板 shell 裸名（"bash"）走 UI 真实入口 TerminalBackend::spawn——
// 上游 Spawn 要求绝对路径（executablePathNotAbsolute），裸名未解析 = 装机态 bash 会话
// 全灭。锁四象限的 bash 一对：裸名 spawn 可用 + {非空目录落之、空→$HOME}。
REGISTER_TEST(terminal_workdir_e2e_bash_bare_name,
              "terminal_workdir: bash 裸名经 backend spawn——目录落之/空→$HOME（need330）", {
    std::filesystem::create_directories("/tmp/devdesk-wd-e2e");
    const std::string wd = std::filesystem::canonical("/tmp/devdesk-wd-e2e").string();
    const char* cmd = "pwd > /tmp/devdesk-wd-e2e/pwd-bash.out; echo __WD_DONE__\n";
    const char* home = std::getenv("HOME");
    DOCTEST_REQUIRE(home != nullptr);
    const std::string homeCanon = std::filesystem::canonical(home).string();
    platform::TerminalSize size;
    size.rows = 24;
    size.cols = 80;

    // ① 裸名 "bash" + 非空目录 → 会话 cwd 落该目录
    {
        auto backend = platform::terminalBackendMac();
        auto spawned = backend->spawn("bash", size, wd);
        DOCTEST_REQUIRE(spawned.isOk());
        auto& h = spawned.value();
        DOCTEST_REQUIRE(h->write(reinterpret_cast<const uint8_t*>(cmd),
                                 static_cast<int32_t>(std::strlen(cmd)), false)
                            .isOk());
        std::string got = pollPwdOut(wd + "/pwd-bash.out", wd);
        DOCTEST_REQUIRE_EQ(got, wd);
        h->terminate();
    }
    // ② 裸名 "bash" + 空目录 → 解析成 $HOME
    {
        std::filesystem::remove(wd + "/pwd-bash.out");
        auto backend = platform::terminalBackendMac();
        auto spawned = backend->spawn("bash", size, "");
        DOCTEST_REQUIRE(spawned.isOk());
        auto& h = spawned.value();
        DOCTEST_REQUIRE(h->write(reinterpret_cast<const uint8_t*>(cmd),
                                 static_cast<int32_t>(std::strlen(cmd)), false)
                            .isOk());
        std::string got = pollPwdOut(wd + "/pwd-bash.out", homeCanon);
        DOCTEST_REQUIRE_EQ(got, homeCanon);
        h->terminate();
    }
    return true;
})


REGISTER_TEST(terminal_bridge_clamp,
              "terminal_bridge: 网格尺寸钳制到快照镜像上限（bug508）", {
    auto normal = platform::clampGridSize(24, 80);
    DOCTEST_CHECK_EQ(normal.rows, 24);
    DOCTEST_CHECK_EQ(normal.cols, 80);
    DOCTEST_CHECK_FALSE(normal.clamped);

    // 4K 外接屏 + 12px 字号：约 135 行 × 550 列——旧上限 200×60 会整帧丢弃
    auto wide = platform::clampGridSize(135, 550);
    DOCTEST_CHECK_EQ(wide.rows, 135);
    DOCTEST_CHECK_EQ(wide.cols, 550);
    DOCTEST_CHECK_FALSE(wide.clamped);

    // 超上限（6K/更小字号）→ 钳到 240×640 并标记
    auto huge = platform::clampGridSize(400, 900);
    DOCTEST_CHECK_EQ(huge.rows, 240);
    DOCTEST_CHECK_EQ(huge.cols, 640);
    DOCTEST_CHECK_TRUE(huge.clamped);

    // 下限兜底：非法 0/负值不产生空网格
    auto zero = platform::clampGridSize(0, 0);
    DOCTEST_CHECK_EQ(zero.rows, 1);
    DOCTEST_CHECK_EQ(zero.cols, 1);
    return true;
})


#endif // __APPLE__