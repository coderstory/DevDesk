// tests/test_platform/test_terminal_workdir.cpp
// need328：mac 终端 spawn 目录传参链——resolveSpawnWorkDir 空值→$HOME、非空→原值。
// 端到端（设置目录→会话实际落盘 cwd）归打包态真机复测，此处锁纯函数解析语义。
// swift_bridge 为 mac 专用 TU（无 posix 兜底族），非 Apple 平台编空 TU（守卫先例
// 同 test_file_watcher.cpp）。
#if defined(__APPLE__)

#include "doctest.h"
#include "test_support.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <filesystem>
#include <unistd.h>

#include "platform/terminal/swift_bridge.h"

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
// 轮询回读 pwd 输出（≤5s；shell 执行异步，读到非空即止）
std::string pollPwdOut(const std::string& path) {
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        std::ifstream f(path);
        std::string line;
        if (f && std::getline(f, line) && !line.empty()) return line;
    }
    return "";
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
        std::string got = pollPwdOut(wd + "/pwd.out");
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
        std::string got = pollPwdOut(wd + "/pwd.out");
        DOCTEST_REQUIRE_EQ(got, homeCanon);
        s->stop();
    }
    return true;
})

#endif // __APPLE__
