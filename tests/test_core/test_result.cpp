#include "doctest.h"
#include "core/result.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

REGISTER_TEST(test_result_ok, "Result<int> ok", {
    auto r = core::Result<int>::ok(42);
    DOCTEST_CHECK(r.isOk());
    DOCTEST_CHECK_EQ(r.value(), 42);
    return true;
})

REGISTER_TEST(test_result_fail, "Result<int> fail", {
    auto r = core::Result<int>::fail(core::ErrorCode::FileNotFound, "missing");
    DOCTEST_CHECK(r.isFail());
    DOCTEST_CHECK_EQ(r.errorCode(), core::ErrorCode::FileNotFound);
    DOCTEST_CHECK_EQ(r.errorMessage(), std::string("missing"));
    return true;
})

REGISTER_TEST(test_result_valueOr, "Result<int> valueOr", {
    auto ok = core::Result<int>::ok(10);
    auto fail = core::Result<int>::fail(core::ErrorCode::Internal, "x");
    DOCTEST_CHECK_EQ(ok.valueOr(99), 10);
    DOCTEST_CHECK_EQ(fail.valueOr(99), 99);
    return true;
})

REGISTER_TEST(test_result_void, "Result<void>", {
    auto ok = core::Result<void>::ok();
    auto fail = core::Result<void>::fail(core::ErrorCode::Internal, "x");
    DOCTEST_CHECK(ok.isOk());
    DOCTEST_CHECK(fail.isFail());
    DOCTEST_CHECK_EQ(fail.errorMessage(), std::string("x"));
    return true;
})

// 默认构造态是「伪失败态」：isOk()==false 但 errorCode()==Ok。app 用 -fno-exceptions
// 编译，所以 Result 的任何失败路径都不得走 throw，抛即 terminate
REGISTER_TEST(test_result_default_state, "Result 默认构造态不抛异常", {
    core::Result<int> defT;
    DOCTEST_CHECK(defT.isFail());
    DOCTEST_CHECK_EQ(defT.errorMessage(), std::string("（未初始化）"));

    // Result<void> 默认态同为失败态、errorMessage() 同样不抛；其默认 message_ 是空串，
    // 与 Result<int> 的占位串不同是既有事实，不要当成漏写
    core::Result<void> defV;
    DOCTEST_CHECK(defV.isFail());
    DOCTEST_CHECK(defV.errorMessage().empty());
    return true;
})

REGISTER_TEST(test_error_code_values, "ErrorCode values", {
    DOCTEST_CHECK_EQ(static_cast<int>(core::ErrorCode::Ok), 0);
    DOCTEST_CHECK_NE(static_cast<int>(core::ErrorCode::FileNotFound),
                     static_cast<int>(core::ErrorCode::Ok));
    return true;
})

// Result 的取值口收敛为两个，失败的语义因口而异：
//   valueOr()  可能失败时用（返回默认值）
//   value()    按不变量不可能失败时用（失败即 LOG_FATAL + std::abort，与编译选项无关）
// 另外两个带上下文的旧取值口不得回来：源码里一旦出现（注释里也算）本护栏就红。
// 编译期 static_assert 负责另一半：被删的两个口在 Result 上探测不到，而 value /
// valueOr 必须在（防「把口删光也算过」）。
// 护栏靠 cwd 上溯定位仓库根，不用 __FILE__——ccache 会把 __FILE__ 相对化，靠它
// 上溯的守卫换个 cwd 跑就误报；定位失败时直接报错，不静默绿。
namespace {

template <typename T, typename = void>
struct HasRemovedUnsafeValue : std::false_type {};
template <typename T>
struct HasRemovedUnsafeValue<
    T, std::void_t<decltype(std::declval<const T&>().unsafeValue())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRemovedExpect : std::false_type {};
template <typename T>
struct HasRemovedExpect<T, std::void_t<decltype(std::declval<const T&>().expect(
                             std::declval<const std::string&>()))>>
    : std::true_type {};

static_assert(!HasRemovedUnsafeValue<core::Result<int>>::value,
              "CP-08：取值口已收敛，不得再有这个无守卫的直接取值口");
static_assert(!HasRemovedUnsafeValue<core::Result<void>>::value,
              "CP-08：取值口已收敛，不得再有这个无守卫的直接取值口");
static_assert(!HasRemovedExpect<core::Result<int>>::value,
              "CP-08：取值口已收敛，不得再有这个带上下文的取值口");
static_assert(!HasRemovedExpect<core::Result<void>>::value,
              "CP-08：取值口已收敛，不得再有这个带上下文的取值口");

// 保留的两个口：必须在，且签名不许变（value 的非 const 重载必须返回可写引用）
static_assert(
    std::is_same_v<decltype(std::declval<const core::Result<int>&>().value()),
                   const int&>,
    "CP-08：value() const 重载必须保留且返回 const T&");
static_assert(
    std::is_same_v<decltype(std::declval<core::Result<int>&>().value()), int&>,
    "CP-08：value() 非 const 重载必须保留且返回 T&");
static_assert(std::is_same_v<decltype(std::declval<const core::Result<int>&>()
                                          .valueOr(0)),
                             int>,
              "CP-08：valueOr() 必须保留");

std::filesystem::path findRepoRoot() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec);
    if (ec) return {};
    for (int up = 0; up < 8; ++up) {
        if (std::filesystem::exists(dir / "src" / "core" / "result.h", ec) &&
            std::filesystem::exists(dir / "tests" / "CMakeLists.txt", ec)) {
            return dir;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;  // 已到根
        dir = parent;
    }
    return {};
}

// 护栏自身：探测代码与说明里必然带这两个名字，只有这一个文件跳过
constexpr const char* kGuardFileRelPath = "tests/test_core/test_result.cpp";

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::vector<std::string> scanForTokens(const std::filesystem::path& root,
                                       const std::vector<std::string>& needles) {
    std::vector<std::string> hits;
    for (const char* sub : {"src", "tests"}) {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            root / sub, std::filesystem::directory_options::skip_permission_denied,
            ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".cc" && ext != ".cxx" && ext != ".h" &&
                ext != ".hpp" && ext != ".hxx") {
                continue;
            }
            const std::string rel =
                std::filesystem::relative(it->path(), root, fec).generic_string();
            if (fec || rel == kGuardFileRelPath) continue;
            std::ifstream in(it->path());
            if (!in) continue;
            std::string line;
            int lineNo = 0;
            while (std::getline(in, line)) {
                ++lineNo;
                for (const std::string& needle : needles) {
                    if (line.find(needle) != std::string::npos) {
                        hits.push_back(rel + ":" + std::to_string(lineNo) + ": " +
                                       line);
                        break;
                    }
                }
            }
        }
    }
    return hits;
}

// 空串 = 通过；非空 = 失败原因，含逐条命中便于直接定位
std::string resultPortGuardReport() {
    const std::filesystem::path root = findRepoRoot();
    if (root.empty()) {
        std::error_code ec;
        return "护栏无法定位仓库根（cwd=" +
               std::filesystem::current_path(ec).string() +
               "）：请从仓库根或 build/ 下运行 devdesk_tests —— 护栏靠 cwd 上溯"
               "定位（不能用 __FILE__：ccache 会把绝对路径相对化）";
    }

    std::string report;

    // 两个被删的名字不得再出现在任何源码行里，注释也算。第二个用 ".expect(" 精确模式：
    // expect 是常见英文词，裸查会误伤
    const std::vector<std::string> hits =
        scanForTokens(root, {"unsafeValue", ".expect("});
    if (!hits.empty()) {
        report += "src/ + tests/ 又出现了已删的取值口（" +
                  std::to_string(hits.size()) + " 处）——可能失败的取值一律走 "
                  "valueOr()、不可能失败的走 value()：\n";
        for (const std::string& h : hits) report += "  " + h + "\n";
    }

    const std::string resultHeader =
        readWholeFile(root / "src" / "core" / "result.h");
    if (resultHeader.empty()) {
        report += "读不到 src/core/result.h\n";
        return report;
    }
    if (resultHeader.find("std::abort()") == std::string::npos) {
        report += "src/core/result.h 的 value() 失败路径里没有 std::abort()"
                  "（应为 LOG_FATAL + std::abort 的明确终止）\n";
    }
    if (resultHeader.find("std::logic_error") != std::string::npos) {
        report += "src/core/result.h 又走回异常失败路径了（std::logic_error）——"
                  "失败行为会随 TU 的编译选项漂移\n";
    }
    return report;
}

} // namespace

REGISTER_TEST(test_result_ports_source_guard,
              "Result 取值口收敛护栏：全仓不得再出现 unsafeValue / .expect(", {
                  const std::string report = resultPortGuardReport();
                  DOCTEST_CHECK_MESSAGE(report.empty(), report);
                  return true;
              })

REGISTER_TEST(test_result_ports_consistency,
              "Result 取值口收敛：只剩 valueOr + value，行为一致", {
                  auto ok = core::Result<std::string>::ok("v");
                  DOCTEST_CHECK_EQ(ok.value(), std::string("v"));
                  const core::Result<std::string>& cok = ok;
                  DOCTEST_CHECK_EQ(cok.value(), std::string("v"));
                  DOCTEST_CHECK_EQ(ok.valueOr("d"), std::string("v"));

                  // 非 const 重载返回可写引用，这是改 result 内对象的唯一路径
                  ok.value() = "w";
                  DOCTEST_CHECK_EQ(ok.valueOr("d"), std::string("w"));
                  DOCTEST_CHECK_EQ(cok.value(), std::string("w"));

                  // 失败态只有 valueOr 可走。value() 在失败态是 LOG_FATAL + std::abort，进程内断言不到
                  //（真断言会 abort 整个套件），它的「唯一且明确」由上面的编译期 / 源码护栏锁住
                  const auto bad = core::Result<std::string>::fail(
                      core::ErrorCode::ValidationError, "boom");
                  DOCTEST_CHECK(bad.isFail());
                  DOCTEST_CHECK_EQ(bad.valueOr("d"), std::string("d"));
                  DOCTEST_CHECK_EQ(bad.errorCode(),
                                   core::ErrorCode::ValidationError);
                  DOCTEST_CHECK_EQ(bad.errorMessage(), std::string("boom"));
                  return true;
              })