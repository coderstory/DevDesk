#pragma once
// 极简测试框架
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define DOCTEST_CHECK(expr) \
    do { if (!(expr)) { \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " #expr << std::endl; \
        return false; \
    }} while (0)

#define DOCTEST_CHECK_EQ(a, b) \
    do { auto _a = (a); auto _b = (b); \
         if (!(_a == _b)) { \
            std::ostringstream oss; \
            oss << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " #a " != " #b; \
            std::cerr << oss.str() << std::endl; \
            return false; \
         }} while (0)

#define DOCTEST_CHECK_NE(a, b) \
    do { auto _a = (a); auto _b = (b); \
         if (!(_a != _b)) { \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " #a " == " #b << std::endl; \
            return false; \
         }} while (0)

#define DOCTEST_CHECK_TRUE(expr) DOCTEST_CHECK(expr)
#define DOCTEST_CHECK_FALSE(expr) DOCTEST_CHECK(!(expr))
#define DOCTEST_REQUIRE(expr) DOCTEST_CHECK(expr)
#define DOCTEST_REQUIRE_EQ(a, b) DOCTEST_CHECK_EQ(a, b)
#define DOCTEST_REQUIRE_NE(a, b) DOCTEST_CHECK_NE(a, b)

using TestFunc = bool (*)();

namespace test_registry {

inline std::vector<std::pair<std::string, TestFunc>>& registry() {
    static std::vector<std::pair<std::string, TestFunc>> r;
    return r;
}

inline void register_test(const std::string& name, TestFunc f) {
    registry().emplace_back(name, f);
}

} // namespace test_registry

// 直接注册测试：REGISTER_TEST(unique_name, "Description", { body })
// unique_name 必须是合法 C++ 标识符（驼峰命名）
// body 是返回 bool 的代码块；return true 表示通过
// 例：REGISTER_TEST(test_addition, "1+1=2", { return 1+1 == 2; })
#define REGISTER_TEST(unique_name, description, body) \
    namespace { \
        static bool unique_name() body \
        struct UniqueRegistrar_##unique_name { \
            UniqueRegistrar_##unique_name() { \
                test_registry::register_test(description, unique_name); \
            } \
        }; \
        static UniqueRegistrar_##unique_name registrar_instance_##unique_name; \
    }

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// main() 直接在 test_main.cpp 中定义，避免重复