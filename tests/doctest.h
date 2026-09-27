#pragma once
// 极简测试框架
//
// 断言语义三条（写用例前必读）：
// 1) 所有 DOCTEST_REQUIRE* 宏都只是对应 DOCTEST_CHECK* 宏的别名（见下方 #define），
//    没有"致命 / 非致命"之分——与真 doctest 语义不同，不要按名字推断。
// 2) 断言失败即 `return false`（fail-fast）：同一用例里后续断言不再执行、一次失败只报 1 条；
//    想在一个场景里看完多处失败，得拆成多个用例。
// 3) 断言只能写在返回 bool 的函数/用例体里（宏展开含 `return false`）；
//    返回非 bool 的 helper 中不能直接用，helper 需把结果回传、由调用方断言。
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
// 名实不符提示：REQUIRE == CHECK（同样是"失败即中断"，不是真 doctest 的"失败可续"语义）
#define DOCTEST_REQUIRE(expr) DOCTEST_CHECK(expr)
#define DOCTEST_REQUIRE_EQ(a, b) DOCTEST_CHECK_EQ(a, b)
#define DOCTEST_REQUIRE_NE(a, b) DOCTEST_CHECK_NE(a, b)

// 带自定义消息的断言（失败时输出 message 便于定位）
#define DOCTEST_CHECK_MESSAGE(expr, message) \
    do { if (!(expr)) { \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " #expr \
                  << " — " << (message) << std::endl; \
        return false; \
    }} while (0)

#define DOCTEST_REQUIRE_MESSAGE(expr, message) DOCTEST_CHECK_MESSAGE(expr, message)

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