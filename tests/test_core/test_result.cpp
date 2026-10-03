#include "doctest.h"
#include "core/result.h"

#include <filesystem>
#include <fstream>
#include <ios>
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

// M8（2026-09-27 专家审计）：默认构造态是「伪失败态」——isOk()==false 但
// errorCode()==Ok，而旧 errorMessage() 直接走 std::get<2> 会抛
// std::bad_variant_access（app 用 -fno-exceptions 编译，抛即 terminate）。
// 现返回占位串；本用例锁住默认态行为（若哪天真抛了，异常会让本用例失败）。
REGISTER_TEST(test_result_default_state, "Result 默认构造态不抛异常", {
    core::Result<int> defT;
    DOCTEST_CHECK(defT.isFail());
    DOCTEST_CHECK_EQ(defT.errorMessage(), std::string("（未初始化）"));

    // Result<void> 默认态行为一致：同为失败态、errorMessage() 同样不抛
    //（其默认 message_ 为空串，与 Result<int> 的占位串不同是既有事实）
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

// ==================== CP-08 取值口收敛护栏 ====================
// 审计 CP-08：Result 原有 4 个取值口（value / expect / unsafeValue / valueOr），
// 同一个 value() 的失败行为还随 TU 的编译选项漂移（带异常的 TU 抛 logic_error、
// -fno-exceptions 的 Release app TU 直接 terminate），而无守卫的那个口失败时是
// std::get<1> 错索引 = 不带任何上下文的 terminate。现收敛为两个口：
//   valueOr()  可能失败时用（返回默认值）
//   value()    按不变量不可能失败时用（失败即 LOG_FATAL + std::abort，唯一失败
//              路径、与编译选项无关）
//
// 护栏两条腿：
//   1) 编译期——被删的两个口在 Result 上探测不到；value / valueOr 必须在
//      （防「把口删光也算过」）。真被加回来 / 删过头，本文件直接编译失败，
//      比运行时断言更早；
//   2) 源码级——src/ + tests/ 全域不得再出现这两个名字（含注释），防止调用点
//      被写回来或另起旁路。
// 注：护栏靠 cwd 上溯定位仓库根，**不**用 __FILE__——ccache 会把它相对化
//（TIN-01 的教训：靠 __FILE__ 上溯的守卫从别的 cwd 跑必误报）；定位失败时大声
// 报错，不静默绿。
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

// 被删的两个口：一个都不许留（int 与 void 两套特化都查）
static_assert(!HasRemovedUnsafeValue<core::Result<int>>::value,
              "CP-08：取值口已收敛，不得再有这个无守卫的直接取值口");
static_assert(!HasRemovedUnsafeValue<core::Result<void>>::value,
              "CP-08：取值口已收敛，不得再有这个无守卫的直接取值口");
static_assert(!HasRemovedExpect<core::Result<int>>::value,
              "CP-08：取值口已收敛，不得再有这个带上下文的取值口");
static_assert(!HasRemovedExpect<core::Result<void>>::value,
              "CP-08：取值口已收敛，不得再有这个带上下文的取值口");

// 保留的两个口：必须在，且签名不许变（value 非 const 重载要给可写引用）
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

// 仓库根：cwd 或它的祖先里同时有 src/core/result.h 与 tests/CMakeLists.txt
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

// 护栏自身：探测代码与说明里必然带这两个名字，跳过（只有这一个例外）
constexpr const char* kGuardFileRelPath = "tests/test_core/test_result.cpp";

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// 扫 src/ + tests/ 下的源码文件，返回 "相对路径:行号: 行内容" 形式的命中
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

// 空串 = 通过；非空 = 失败原因（含逐条命中，便于直接定位）
std::string resultPortGuardReport() {
    const std::filesystem::path root = findRepoRoot();
    if (root.empty()) {
        std::error_code ec;
        return "护栏无法定位仓库根（cwd=" +
               std::filesystem::current_path(ec).string() +
               "）：请从仓库根或 build/ 下运行 devdesk_tests —— 护栏靠 cwd 上溯"
               "定位（不能用 __FILE__：ccache 会把绝对路径相对化，TIN-01 教训）";
    }

    std::string report;

    // 1) 两个被删的名字不得再出现在任何源码行（含注释）里
    //    第二个用 ".expect(" 精确模式：expect 是常见英文词，不能裸查
    const std::vector<std::string> hits =
        scanForTokens(root, {"unsafeValue", ".expect("});
    if (!hits.empty()) {
        report += "src/ + tests/ 又出现了已删的取值口（" +
                  std::to_string(hits.size()) + " 处）——可能失败的取值一律走 "
                  "valueOr()、不可能失败的走 value()：\n";
        for (const std::string& h : hits) report += "  " + h + "\n";
    }

    // 2) result.h 自身：value() 的失败路径必须落在「明确终止」这一条上
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
                  // 成功态：两个口同源（const / 非 const 重载都给同一个值）
                  auto ok = core::Result<std::string>::ok("v");
                  DOCTEST_CHECK_EQ(ok.value(), std::string("v"));
                  const core::Result<std::string>& cok = ok;
                  DOCTEST_CHECK_EQ(cok.value(), std::string("v"));
                  DOCTEST_CHECK_EQ(ok.valueOr("d"), std::string("v"));

                  // 非 const 重载必须返回可写引用（改 result 内对象的唯一路径）
                  ok.value() = "w";
                  DOCTEST_CHECK_EQ(ok.valueOr("d"), std::string("w"));
                  DOCTEST_CHECK_EQ(cok.value(), std::string("w"));

                  // 失败态：只有 valueOr 可走。value() 的失败路径是 LOG_FATAL +
                  // std::abort，进程内断言不到（真断言就 abort 整个套件）——它的
                  //「唯一且明确」由上面的编译期 / 源码护栏锁住
                  const auto bad = core::Result<std::string>::fail(
                      core::ErrorCode::ValidationError, "boom");
                  DOCTEST_CHECK(bad.isFail());
                  DOCTEST_CHECK_EQ(bad.valueOr("d"), std::string("d"));
                  DOCTEST_CHECK_EQ(bad.errorCode(),
                                   core::ErrorCode::ValidationError);
                  DOCTEST_CHECK_EQ(bad.errorMessage(), std::string("boom"));
                  return true;
              })