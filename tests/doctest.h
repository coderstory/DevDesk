#pragma once
// Minimal doctest stub for offline compilation.
// If you have internet access, replace with the official doctest.h from https://github.com/doctest/doctest
// For now, this provides enough functionality for the tests in this project.

#ifndef DOCTEST_H
#define DOCTEST_H

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

namespace doctest {

struct TestCase {
    std::string name;
    std::string suite;
    void (*func)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Context {
    bool quiet = false;
};

inline Context& context() {
    static Context c;
    return c;
}

struct TestRegistrar {
    TestRegistrar(const char* name, const char* suite, void (*f)()) {
        registry().push_back({name, suite ? suite : "", f});
    }
};

inline int runAll() {
    int passed = 0;
    int failed = 0;
    for (auto& t : registry()) {
        std::cout << "[ TEST ] " << t.name << " ... " << std::flush;
        try {
            t.func();
            std::cout << "PASSED" << std::endl;
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << std::endl;
            ++failed;
        }
    }
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    return failed == 0 ? 0 : 1;
}

} // namespace doctest

#define DOCTEST_TEST_CASE(name) \
    static void test_##__LINE__(); \
    static doctest::TestRegistrar reg_##__LINE__(name, "", test_##__LINE__); \
    static void test_##__LINE__()

#define DOCTEST_TEST_SUITE(name)
#define DOCTEST_TEST_SUITE_END()

#define DOCTEST_SUBCASE(name)

#define DOCTEST_CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream oss; \
            oss << "CHECK failed: " #expr " at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define DOCTEST_CHECK_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (!(_a == _b)) { \
            std::ostringstream oss; \
            oss << "CHECK_EQ failed: " #a " (" << _a << ") != " #b " (" << _b << ") at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define DOCTEST_CHECK_NE(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (!(_a != _b)) { \
            std::ostringstream oss; \
            oss << "CHECK_NE failed at " __FILE__ ":" << __LINE__; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (0)

#define DOCTEST_CHECK_TRUE(expr) DOCTEST_CHECK(expr)
#define DOCTEST_CHECK_FALSE(expr) DOCTEST_CHECK(!(expr))

#define DOCTEST_REQUIRE(expr) DOCTEST_CHECK(expr)
#define DOCTEST_REQUIRE_EQ(a, b) DOCTEST_CHECK_EQ(a, b)

#define DOCTEST_INFO(msg) // ignored

// Test runner implementation macro
#ifdef DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    return doctest::runAll();
}
#endif

#endif // DOCTEST_H