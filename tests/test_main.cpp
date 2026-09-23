// tests/test_main.cpp
// 唯一定义 main() 的文件
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/theme.h"
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

// 环境变量通道：Windows 传空串即删除变量；POSIX 空串走 unsetenv
void setEnvVar(const char* key, const std::string& value) {
#if defined(_WIN32)
    ::_putenv_s(key, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(key);
    } else {
        ::setenv(key, value.c_str(), 1);
    }
#endif
}

// ==================== per-case 全局态重置（测试隔离） ====================
// 全量 ctest 偶发失败（editor_font:83 / theme_hierarchy:207/208）的根因：
// 用例体内 fail-fast（DOCTEST_CHECK/REQUIRE 直接 return false）会跳过尾部
// 恢复代码，core::theme 全局值与模块视图状态跨用例/跨进程泄漏——
// markdown/todo 模块的视图状态（三态模式/选中笔记）持久化在
// markdownViewStateFile()/todoViewStateFile() 指向的文件里，未设
// DEVDESK_*_VIEW_STATE_PATH 的用例直接读写真实 %APPDATA% 文件，三态模式
// 在多次运行间轮换，断言「onStart 后默认 Edit 态」的用例约 1/3 概率翻车。
// 治本：每个用例执行前把进程级全局态拉回出厂默认（clean-room），用例体
// 内的显式 setLight/setFontScale 等不受影响（它们在重置之后执行）。
void resetCaseGlobals() {
    // core::theme：默认深色 / 编辑器字号 15 / 缩放 1.0（与 theme.h 契约一致）。
    // 先摘 change handler 再切主题：泄漏的 handler 可能捕获已析构的用例栈
    // 变量（test_theme 用 [&] 捕获），后切会让它摸到野指针
    core::theme::setChangeHandler(nullptr);
    core::theme::setDark();
    core::theme::setEditorFontSize(15.0f);
    core::theme::setFontScale(1.0f);
    // 共享 DB 连接：用例提前返回时手工 register 的可能悬垂（DbEnv RAII 的
    // 已自注销；这里防非 RAII 写法），需要 DB 的用例都在体内自行注册
    ::core::registerAppDatabase(nullptr);
    // 路径环境变量：一律拉回「未设置」出厂态；两个视图状态例外——重置为
    // 指向 scratch 的新文件并删除，让未显式隔离的用例 onStart 也拿默认态
    setEnvVar("DEVDESK_NOTES_DIR", "");
    setEnvVar("DEVDESK_CONFIG_PATH", "");
    setEnvVar("DEVDESK_PETS_DIR", "");
    setEnvVar("DEVDESK_CLAUDE_CONFIG_DIR", "");
    std::error_code ec;
    const auto scratch = std::filesystem::temp_directory_path() /
                         "devdesk_test_env_scratch";
    const std::string mdVs = (scratch / "md_view_state.json").string();
    const std::string todoVs = (scratch / "todo_view_state.json").string();
    std::filesystem::remove(mdVs, ec);
    std::filesystem::remove(todoVs, ec);
    setEnvVar("DEVDESK_MARKDOWN_VIEW_STATE_PATH", mdVs);
    setEnvVar("DEVDESK_TODO_VIEW_STATE_PATH", todoVs);
}

} // namespace

// 用法：devdesk_tests.exe [子串] [--log]
//   无参数   → 全量跑
//   子串     → 只跑描述里含该子串的测试（如 "streaming"）
//   --log    → 打开控制台日志 + Debug 级别（排查流式/代理问题时用）
int main(int argc, char** argv) {
    using Clock = std::chrono::steady_clock;
    int passed = 0, failed = 0, total = 0;
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--log") {
            core::log::setLevel(core::log::Level::Debug);
            core::log::addConsoleOutput();
        } else if (!arg.empty()) {
            filter = arg;
        }
    }
    for (const auto& t : test_registry::registry()) {
        if (!filter.empty() && t.first.find(filter) == std::string::npos) {
            continue;
        }
        ++total;
        bool ok = false;
        std::string errMsg;
        const auto t0 = Clock::now();
        // 单个测试的异常（如 Windows 上 std::filesystem_error）只让本测试
        // 失败，不炸掉整个套件
        try {
            resetCaseGlobals();
            ok = t.second();
        } catch (const std::exception& e) {
            ok = false;
            errMsg = e.what();
        } catch (...) {
            ok = false;
            errMsg = "unknown exception";
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - t0)
                            .count();
        // std::endl 强制 flush，重定向到文件时结果实时可见
        if (ok) {
            std::cout << "[PASS] " << t.first << " [" << ms << "ms]"
                      << std::endl;
            ++passed;
        } else {
            std::cout << "[FAIL] " << t.first << " [" << ms << "ms]"
                      << std::endl;
            if (!errMsg.empty()) {
                std::cout << "       exception: " << errMsg << std::endl;
            }
            ++failed;
        }
    }
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Total:  " << total << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    return failed == 0 ? 0 : 1;
}
