// tests/test_main.cpp
// 唯一定义 main() 的文件
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <iostream>

int main() {
    int passed = 0, failed = 0;
    for (const auto& t : test_registry::registry()) {
        bool ok = t.second();
        if (ok) { std::cout << "[PASS] " << t.first << std::endl; ++passed; }
        else    { std::cout << "[FAIL] " << t.first << std::endl; ++failed; }
    }
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    return failed == 0 ? 0 : 1;
}