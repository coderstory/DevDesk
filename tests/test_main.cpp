// tests/test_main.cpp
// 唯一定义 main() 的文件
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "core/logging.h"
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

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
