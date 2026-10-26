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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>  // std::error_code（tempPath 的 create_directories 用）
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h> // getpid
#endif

#define DOCTEST_CHECK(expr) \
    do { if (!(expr)) { \
        std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " #expr << std::endl; \
        return false; \
    }} while (0)

// _a/_b 为**值拷贝**而非 const auto&（本仓库大量 `Result::value()`
// 链式调用返回"指向临时量的引用"，绑引用即 GCC 16 -Wdangling-reference 真阳性，
// 50 处既有用例被 -Werror 拦停；拷贝才是这些表达式的安全形态）。此处只取审计
// 建议里的 const 部分：加 const 防止断言过程意外改写被比较值。
#define DOCTEST_CHECK_EQ(a, b) \
    do { const auto _a = (a); const auto _b = (b); \
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

// [SKIP] 标记：用例体调 DOCTEST_SKIP 时置位，运行器（test_main.cpp）逐用例
// 取用后复位（consumeSkipped），单独计 Skipped——Skipped 不算失败。
inline bool& skipFlag() {
    static bool flag = false;
    return flag;
}

inline void markSkipped() { skipFlag() = true; }

// 取出当前用例的跳过标记并复位；运行器每个用例调用一次
inline bool consumeSkipped() {
    const bool flag = skipFlag();
    skipFlag() = false;
    return flag;
}

} // namespace test_registry

// 跳过当前用例（环境门控未开 / 平台 stub 等）：打印 [SKIP] + 原因，标记为
// 跳过并返回 true——不计失败。只能用在返回 bool 的用例体 / helper 里。
#define DOCTEST_SKIP(message) \
    do { \
        std::cout << "[SKIP] " << (message) << std::endl; \
        ::test_registry::markSkipped(); \
        return true; \
    } while (0)

// 临时路径唯一化
// 仓库按「并发多 worktree」开发：多个会话可能同时跑同一个 devdesk_tests.exe，
// 固定名临时路径会被另一进程 remove_all / 覆写 → 随机红。
// 统一走 tempPath()：pid（进程间唯一）+ 进程内自增序号（进程内唯一），
// 扩展名原样保留（.db / .json 语义不变）。
namespace test_temp {

inline long processId() {
#if defined(_WIN32)
    return static_cast<long>(::_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

// 临时文件基目录：可用环境变量 DEVDESK_TEST_TMPDIR 注入；默认 <repo>/build/testtmp。
// 注入值**原样采用**，兜底侧自己补**绝对**（对 cwd 取 absolute）——基目录必须绝对：有断言
// 依赖它（test_claude_service 的 run-settings 路径 is_absolute()），相对值会让同一份用例
// 随调用方式红绿不定，而门禁的失败提示教的**正是**「直接跑二进制」（判据见 test_paths.cpp
// 的 test_infra 用例）。
// 刻意不依赖系统 %TEMP%/TMPDIR：受限执行环境（沙箱 ec=13）对用户临时目录写入被拒，
// 整套测试无法原地跑。进程内缓存一次；目录不存在则按需创建。
inline const std::filesystem::path& tempBaseDir() {
    static const std::filesystem::path base = [] {
        if (const char* env = std::getenv("DEVDESK_TEST_TMPDIR"); env && *env) {
            return std::filesystem::path(env);
        }
        // absolute() 失败（极罕见：cwd 不可读）退回相对值 = 原行为，不静默换目录，
        // 后续文件读写自会以可见错误暴露。
        std::error_code ec;
        const std::filesystem::path rel =
            std::filesystem::path("build") / "testtmp";
        const auto abs = std::filesystem::absolute(rel, ec);
        return ec ? rel : abs;
    }();
    static const bool created = [] {
        std::error_code ec;
        std::filesystem::create_directories(base, ec);
        return !ec;
    }();
    (void)created;  // 创建失败不在此兜底：后续文件读写自会以可见错误暴露，不静默换目录
    return base;
}

// tempPath("devdesk_test.db") → <基目录>/devdesk_test-<pid>-<n>.db
inline std::filesystem::path tempPath(const std::string& name) {
    static std::atomic<unsigned long long> seq{0};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path p(name);
    const std::string ext = p.extension().string();
    p.replace_extension();
    return tempBaseDir() /
           (p.string() + "-" + std::to_string(processId()) + "-" +
            std::to_string(n) + ext);
}

// 仓库根定位（源码契约测试 / 需要仓库自身的用例通用）。
// 为什么不能只靠 __FILE__ 上溯：generator 可能以**相对路径**传源文件（ninja 下常见
// `..\tests\test_modules\x.cpp`），weakly_canonical 按**运行时 cwd** 解析——
// 从 build/ 直跑能对上，ctest 的 cwd 是 build/tests 就错位成不存在的路径。
// 双锚点各向上找本仓库根独有标记 tests/doctest.h：
//   ① doctest.h 自身路径（含相对形态；cwd 错位时标记检查不中，自动落到 ②）
//   ② 运行时 cwd（无论 cwd=build/ 还是 build/tests，build 目录都在根的正下方）
// CI 与本地、ctest 与直跑因此拿到同一根。找不到返回空 path——调用方必须
// DOCTEST_REQUIRE(!root.empty()) 兜底报错，不许静默跳过断言。
inline std::filesystem::path repoRoot() {
    namespace fs = std::filesystem;
    static const fs::path root = [] {
        std::error_code ec;
        const fs::path fileAnchor =
            fs::weakly_canonical(fs::path(__FILE__), ec).parent_path();
        const fs::path anchors[2] = {fileAnchor, fs::current_path(ec)};
        for (const fs::path& start : anchors) {
            for (fs::path dir = start;
                 !dir.empty() && dir != dir.parent_path();
                 dir = dir.parent_path()) {
                if (fs::exists(dir / "tests" / "doctest.h", ec)) return dir;
            }
        }
        return fs::path{};
    }();
    return root;
}

} // namespace test_temp

// 直接注册测试：REGISTER_TEST(unique_name, "Description", { body })
// unique_name 必须是合法 C++ 标识符（驼峰命名）
// body 是返回 bool 的代码块；return true 表示通过
// 例：REGISTER_TEST(test_addition, "1+1=2", { return 1+1 == 2; })
//
// body 走可变参 `...` / `__VA_ARGS__` 承接：预处理器只在
// 「顶层」逗号处切分实参，body 从第 3 个实参起被整体收进 __VA_ARGS__，
// 因此用例体里的裸逗号（聚合初始化列表、`std::map<int, float>` 模板逗号、
// 多声明语句等）**不再**需要写成括号包裹 / 逐条 push_back / 搬出宏外等规避式。
// 签名对既有调用点完全兼容（原来第 3 个实参就是 body）。
#define REGISTER_TEST(unique_name, description, ...) \
    namespace { \
        static bool unique_name() __VA_ARGS__ \
        struct UniqueRegistrar_##unique_name { \
            UniqueRegistrar_##unique_name() { \
                test_registry::register_test(description, unique_name); \
            } \
        }; \
        static UniqueRegistrar_##unique_name registrar_instance_##unique_name; \
    }

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// main() 直接在 test_main.cpp 中定义，避免重复