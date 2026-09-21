// tests/test_main.cpp
// 唯一定义 main() 的文件
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <chrono>
#include <exception>
#include <iostream>
#include <string>

int main() {
    using Clock = std::chrono::steady_clock;
    int passed = 0, failed = 0, total = 0;
    for (const auto& t : test_registry::registry()) {
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
