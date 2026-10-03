#!/usr/bin/env bash
# scripts/check-std-includes.sh
# 「本文件直接用了 std::X，却靠**标准库内部**的传递包含才看得见」守卫（2026-10-03）
#
# ── 它在守什么（为什么会有这道门禁）──────────────────────────────────
# 2026-10-03 Windows CI（MSYS2 MinGW64 GCC）第一次编到几个从未被编译过的文件，
# 报 `quota_store.cpp:43: error: 'optional' in namespace 'std' does not name a
# template type`。真因：`quota_store.cpp` 第 43 行用了 `std::optional`，而本文件
# 的 include 块里没有 `<optional>`。它是被 `core/sqlite_db.h` → `<map>` →
# `<__node_handle>` → `<optional>` **传递带进来的**（用 clang `-H` 实测到的链条）。
# macOS libc++ 也传递 → 本机永远绿；MSYS2 的 libstdc++ 不传递 → 只有 Windows
# 编译器会报。
# **共享文件的缺 include 只有一个编译器会报，而那台编译器不是本机的**——这就是
# 「Windows 只编平台文件」那个旧门禁（scripts/check-windows-cross-compile.sh
# 早期版本只编 `*_win.cpp`）漏掉它的原因：`quota_store.cpp` 是**共享文件**，
# Windows CI 编它，旧门禁不编。
#
# ── 为什么这道门禁必须是**静态**的，「编一遍」不够 ──────────────────
# 交叉链 llvm-mingw 用的是 **libc++**（`c++/v1`），CI 用的是 **libstdc++**。
# 实测：同一份 quota_store.cpp 用 llvm-mingw 交叉编**通过**——因为 libc++ 恰好也
# 在第 8 层传递带了 `<optional>`。也就是说：**只要标准库自己还在传递包含，任何
# 编译型门禁都抓不到这一类 bug**，只有等 CI 换一台编译器才炸。本门禁改用与标准库
# 无关的判据：
#     本文件直接写了 std::X，而能声明它的头**既不在本文件，也不在本文件直接
#     include 的本项目头文件里** → 报红。
#
# ── 口径：区分「真实风险」与「既有风格」（这就是任务给的两条判据）────
#   ① **既有风格，不报**：本文件直接 include 的某个**本项目头文件**里显式写了
#      `<X>`。那条依赖链是我们自己控制的（头文件作者显式声明），不是标准库实现
#      细节的偶然产物。为这类文件补 include 是「顺手大改所有文件的 include 块」。
#   ② **真实风险，报**：`<X>` 只经由**标准库自己头与头之间的传递包含**才可见
#      （`__node_handle` → `<optional>` 这类）。这类链条随标准库版本/实现漂移，
#      换编译器就断——quota_store.cpp:43 就是这么炸的。
# 实测口径差：naive 判据（完全不看项目头）报 492 处；本判据报其中一部分，其余
# 归为 ①。以 ② 为准，见报告。
#
# ── 验到了什么 / 没验什么 ───────────────────────────────────────────
#   ✅ 验到：.cpp / .h 里**直接**使用的 std 符号，既没在本文件 include、也没在
#      直接 include 的项目头里显式声明（本次 CI 那类），以及 `using namespace
#      std::X` 的同类缺头。
#   ❌ 验不到：第三方库头（curl / sqlite / openssl / httplib / EUI-NEO）内部的
#      include —— 那些不在本门禁的索引范围内，会被记成「无法判定」而非违规。
#   ❌ 验不到：整块被 `#if` 关掉的分支（本门禁不跑预处理器）、宏拼出来的符号、
#      头文件本身能不能编过（那是 check-windows-cross-compile.sh 与 CI 的活）。
#   ❌ 验不到：项目头 → 项目头 → `<X>` 的**两级以上**间接（只做一级；实测本仓
#      几乎没有这种写法，两级会归入「无法判定」）。
#
# ── --self-test：自证非恒真 ────────────────────────────────────────
#   好树：真实源码必须 0 违规（否则是索引/映射表把正常代码判坏了）
#   坏树 A：删掉某文件的一行 `#include <optional>` → 必须红
#   坏树 B：往某文件塞一行裸 `std::optional<int> bad_probe_xyz;` → 必须红
#   坏树 C：把文件换成「只经项目头拿到 std 符号」的形态 → 必须**绿**（证明 ①
#     的豁免是真的在起作用，不是一律报红）
#   三条坏树都先 `diff` 断言**真的改了文件**（等价变异 = 零证据），再要求红。
#
# 退出码：0 = 通过；1 = 有违规；2 = 环境错
#
# 用法：bash scripts/check-std-includes.sh [--root DIR] [--self-test] [--verbose]
set -uo pipefail

usage() { echo "用法: bash scripts/check-std-includes.sh [--root DIR] [--self-test] [--verbose]" >&2; }

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0
VERBOSE=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)      ROOT="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        --verbose)   VERBOSE=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

if ! command -v awk >/dev/null 2>&1; then
    echo "❌ std include 守卫：找不到 awk —— 检查器跑不了，拒绝报绿" >&2
    exit 2
fi

# ── 符号 → 头 映射表 ───────────────────────────────────────────────
# 接受列表（空格分隔的若干头）：任一头被显式 include 即通过。
# 故意比「最小集合」宽（例：std::accumulate 同时接受 <numeric> 与 <algorithm>，
# C++17 起两处都声明）：判据是「有没有显式包含一个能声明它的头」，不是「唯一正解」。
SYMTAB=$(cat <<'TBL_EOF'
std::optional|<optional>
std::nullopt|<optional>
std::nullopt_t|<optional>
std::bad_optional_access|<optional>
std::variant|<variant>
std::monostate|<variant>
std::bad_variant_access|<variant>
std::holds_alternative|<variant>
std::visit|<variant>
std::get|<variant> <tuple> <utility> <array>
std::any|<any>
std::any_cast|<any>
std::string|<string>
std::wstring|<string>
std::u16string|<string>
std::u32string|<string>
std::to_string|<string>
std::stoi|<string>
std::stol|<string>
std::stoll|<string>
std::stoul|<string>
std::stoull|<string>
std::stof|<string>
std::stod|<string>
std::stold|<string>
std::getline|<string>
std::string_view|<string_view>
std::vector|<vector>
std::map|<map>
std::multimap|<map>
std::unordered_map|<unordered_map>
std::unordered_multimap|<unordered_map>
std::set|<set>
std::multiset|<set>
std::unordered_set|<unordered_set>
std::unordered_multiset|<unordered_set>
std::pair|<utility>
std::make_pair|<utility>
std::move|<utility>
std::move_if_noexcept|<utility>
std::forward|<utility>
std::swap|<utility> <algorithm>
std::exchange|<utility>
std::move_only_function|<utility>
std::in_place|<utility>
std::in_place_type|<utility>
std::tuple|<tuple>
std::make_tuple|<tuple>
std::tie|<tuple>
std::tuple_size|<tuple>
std::tuple_element|<tuple>
std::function|<functional>
std::bind|<functional>
std::ref|<functional>
std::cref|<functional>
std::invoke|<functional>
std::invoke_result|<type_traits>
std::hash|<functional>
std::less|<functional>
std::greater|<functional>
std::shared_ptr|<memory>
std::unique_ptr|<memory>
std::make_shared|<memory>
std::make_unique|<memory>
std::weak_ptr|<memory>
std::enable_shared_from_this|<memory>
std::allocator|<memory>
std::addressof|<memory>
std::default_delete|<memory>
std::mutex|<mutex>
std::recursive_mutex|<mutex>
std::timed_mutex|<mutex>
std::lock_guard|<mutex>
std::unique_lock|<mutex>
std::scoped_lock|<mutex>
std::once_flag|<mutex>
std::call_once|<mutex>
std::adopt_lock|<mutex>
std::defer_lock|<mutex>
std::try_to_lock|<mutex>
std::condition_variable|<condition_variable>
std::condition_variable_any|<condition_variable>
std::cv_status|<condition_variable>
std::notify_one_at_thread_exit|<condition_variable>
std::shared_mutex|<shared_mutex>
std::shared_lock|<shared_mutex>
std::thread|<thread>
std::jthread|<thread>
std::this_thread|<thread>
std::async|<future>
std::future|<future>
std::shared_future|<future>
std::promise|<future>
std::packaged_task|<future>
std::launch|<future>
std::atomic|<atomic>
std::atomic_flag|<atomic>
std::atomic_bool|<atomic>
std::atomic_int|<atomic>
std::atomic_size_t|<atomic>
std::memory_order|<atomic>
std::memory_order_relaxed|<atomic>
std::memory_order_acquire|<atomic>
std::memory_order_release|<atomic>
std::memory_order_acq_rel|<atomic>
std::memory_order_seq_cst|<atomic>
std::memory_order_acquire_release|<atomic>
std::memory_order_consume|<atomic>
std::barrier|<barrier>
std::latch|<latch>
std::counting_semaphore|<semaphore>
std::size_t|<cstddef>
std::ptrdiff_t|<cstddef>
std::byte|<cstddef>
std::max_align_t|<cstddef>
std::nullptr_t|<cstddef>
std::int8_t|<cstdint>
std::int16_t|<cstdint>
std::int32_t|<cstdint>
std::int64_t|<cstdint>
std::uint8_t|<cstdint>
std::uint16_t|<cstdint>
std::uint32_t|<cstdint>
std::uint64_t|<cstdint>
std::intmax_t|<cstdint>
std::uintmax_t|<cstdint>
std::intptr_t|<cstdint>
std::uintptr_t|<cstdint>
std::size|<iterator>
std::distance|<iterator>
std::begin|<iterator>
std::end|<iterator>
std::advance|<iterator>
std::next|<iterator>
std::prev|<iterator>
std::back_inserter|<iterator>
std::front_inserter|<iterator>
std::inserter|<iterator>
std::numeric_limits|<limits>
std::enable_if|<type_traits>
std::enable_if_t|<type_traits>
std::is_same|<type_traits>
std::is_same_v|<type_traits>
std::is_base_of|<type_traits>
std::is_integral|<type_traits>
std::is_floating_point|<type_traits>
std::is_convertible|<type_traits>
std::is_constructible|<type_traits>
std::is_trivially_copyable|<type_traits>
std::is_constant_evaluated|<type_traits>
std::decay|<type_traits>
std::decay_t|<type_traits>
std::remove_reference|<type_traits>
std::remove_cv|<type_traits>
std::remove_cvref|<type_traits>
std::remove_const|<type_traits>
std::remove_volatile|<type_traits>
std::remove_pointer|<type_traits>
std::add_pointer|<type_traits>
std::add_const|<type_traits>
std::add_lvalue_reference|<type_traits>
std::add_rvalue_reference|<type_traits>
std::conditional|<type_traits>
std::void_t|<type_traits>
std::true_type|<type_traits>
std::false_type|<type_traits>
std::underlying_type|<type_traits>
std::common_type|<type_traits>
std::aligned_storage|<type_traits>
std::rank|<type_traits>
std::is_arithmetic|<type_traits>
std::is_enum|<type_traits>
std::is_class|<type_traits>
std::is_pointer|<type_traits>
std::is_array|<type_traits>
std::is_function|<type_traits>
std::is_reference|<type_traits>
std::is_signed|<type_traits>
std::is_unsigned|<type_traits>
std::filesystem|<filesystem>
std::chrono|<chrono>
std::runtime_error|<stdexcept>
std::logic_error|<stdexcept>
std::invalid_argument|<stdexcept>
std::out_of_range|<stdexcept>
std::length_error|<stdexcept>
std::domain_error|<stdexcept>
std::overflow_error|<stdexcept>
std::underflow_error|<stdexcept>
std::range_error|<stdexcept>
std::exception|<exception>
std::terminate|<exception>
std::set_terminate|<exception>
std::bad_alloc|<new>
std::bad_array_new_length|<new>
std::nothrow|<new>
std::launder|<new>
std::hardware_destructive_interference_size|<new>
std::hardware_constructive_interference_size|<new>
std::error_code|<system_error>
std::error_category|<system_error>
std::error_condition|<system_error>
std::system_error|<system_error>
std::generic_category|<system_error>
std::make_error_code|<system_error>
std::max|<algorithm>
std::min|<algorithm>
std::minmax|<algorithm>
std::clamp|<algorithm>
std::sort|<algorithm>
std::stable_sort|<algorithm>
std::partial_sort|<algorithm>
std::is_sorted|<algorithm>
std::find|<algorithm>
std::find_if|<algorithm>
std::find_if_not|<algorithm>
std::find_first_of|<algorithm>
std::adjacent_find|<algorithm>
std::count|<algorithm>
std::count_if|<algorithm>
std::remove|<algorithm> <cstdio>
std::remove_if|<algorithm>
std::replace|<algorithm>
std::replace_if|<algorithm>
std::transform|<algorithm>
std::for_each|<algorithm>
std::for_each_n|<algorithm>
std::copy|<algorithm>
std::copy_if|<algorithm>
std::copy_n|<algorithm>
std::copy_backward|<algorithm>
std::fill|<algorithm>
std::fill_n|<algorithm>
std::generate|<algorithm>
std::generate_n|<algorithm>
std::reverse|<algorithm>
std::rotate|<algorithm>
std::unique|<algorithm>
std::erase|<algorithm> <vector> <map> <set> <string>
std::erase_if|<algorithm> <vector> <map> <set> <string>
std::lower_bound|<algorithm>
std::upper_bound|<algorithm>
std::equal_range|<algorithm>
std::binary_search|<algorithm>
std::search|<algorithm>
std::min_element|<algorithm>
std::max_element|<algorithm>
std::minmax_element|<algorithm>
std::all_of|<algorithm>
std::any_of|<algorithm>
std::none_of|<algorithm>
std::includes|<algorithm>
std::merge|<algorithm>
std::inplace_merge|<algorithm>
std::partition|<algorithm>
std::stable_partition|<algorithm>
std::lexicographical_compare|<algorithm>
std::accumulate|<numeric> <algorithm>
std::reduce|<numeric>
std::inner_product|<numeric>
std::partial_sum|<numeric>
std::adjacent_difference|<numeric>
std::iota|<numeric>
std::gcd|<numeric>
std::lcm|<numeric>
std::midpoint|<numeric>
std::memcpy|<cstring>
std::memmove|<cstring>
std::memset|<cstring>
std::memcmp|<cstring>
std::strlen|<cstring>
std::strcmp|<cstring>
std::strncmp|<cstring>
std::strcpy|<cstring>
std::strncpy|<cstring>
std::strcat|<cstring>
std::strchr|<cstring>
std::strstr|<cstring>
std::strdup|<cstring>
std::strerror|<cstring>
std::printf|<cstdio>
std::fprintf|<cstdio>
std::sprintf|<cstdio>
std::snprintf|<cstdio>
std::vsnprintf|<cstdio>
std::sscanf|<cstdio>
std::fopen|<cstdio>
std::fclose|<cstdio>
std::fread|<cstdio>
std::fwrite|<cstdio>
std::fflush|<cstdio>
std::fputs|<cstdio>
std::fgets|<cstdio>
std::fseek|<cstdio>
std::ftell|<cstdio>
std::rename|<cstdio>
std::tmpfile|<cstdio>
std::perror|<cstdio>
std::FILE|<cstdio>
std::stdout|<cstdio>
std::stderr|<cstdio>
std::stdin|<cstdio>
std::tolower|<cctype>
std::toupper|<cctype>
std::isalpha|<cctype>
std::isdigit|<cctype>
std::isalnum|<cctype>
std::isspace|<cctype>
std::isupper|<cctype>
std::islower|<cctype>
std::ispunct|<cctype>
std::isxdigit|<cctype>
std::isblank|<cctype>
std::abs|<cstdlib> <cmath>
std::labs|<cstdlib>
std::exit|<cstdlib>
std::abort|<cstdlib>
std::atoi|<cstdlib>
std::atol|<cstdlib>
std::atoll|<cstdlib>
std::atof|<cstdlib>
std::getenv|<cstdlib>
std::system|<cstdlib>
std::malloc|<cstdlib>
std::calloc|<cstdlib>
std::realloc|<cstdlib>
std::free|<cstdlib>
std::rand|<cstdlib>
std::srand|<cstdlib>
std::EXIT_SUCCESS|<cstdlib>
std::EXIT_FAILURE|<cstdlib>
std::floor|<cmath>
std::ceil|<cmath>
std::round|<cmath>
std::trunc|<cmath>
std::sqrt|<cmath>
std::pow|<cmath>
std::fabs|<cmath>
std::fmod|<cmath>
std::sin|<cmath>
std::cos|<cmath>
std::tan|<cmath>
std::atan2|<cmath>
std::hypot|<cmath>
std::exp|<cmath>
std::log|<cmath>
std::log2|<cmath>
std::log10|<cmath>
std::isnan|<cmath>
std::isinf|<cmath>
std::time|<ctime>
std::time_t|<ctime>
std::tm|<ctime>
std::clock|<ctime>
std::difftime|<ctime>
std::mktime|<ctime>
std::localtime|<ctime>
std::localtime_s|<ctime>
std::gmtime|<ctime>
std::gmtime_s|<ctime>
std::strftime|<ctime>
std::put_time|<iomanip>
std::get_time|<iomanip>
std::ostringstream|<sstream>
std::istringstream|<sstream>
std::stringstream|<sstream>
std::ifstream|<fstream>
std::ofstream|<fstream>
std::fstream|<fstream>
std::ios|<ios>
std::ios_base|<ios> <iostream> <fstream> <sstream>
std::ostream|<ostream> <iostream>
std::istream|<istream> <iostream>
std::cout|<iostream>
std::cerr|<iostream>
std::clog|<iostream>
std::cin|<iostream>
std::endl|<ostream> <iostream>
std::setw|<iomanip>
std::setfill|<iomanip>
std::setprecision|<iomanip>
std::put_time|<iomanip>
std::fixed|<ios> <iomanip>
std::boolalpha|<ios>
std::scientific|<ios> <iomanip>
std::hex|<ios> <iomanip> <cstdio>
std::oct|<ios> <iomanip> <cstdio>
std::dec|<ios> <iomanip> <cstdio>
std::left|<ios> <iomanip>
std::right|<ios> <iomanip>
std::array|<array>
std::deque|<deque>
std::list|<list>
std::forward_list|<forward_list>
std::initializer_list|<initializer_list>
std::bitset|<bitset>
std::span|<span>
std::regex|<regex>
std::smatch|<regex>
std::mt19937|<random>
std::seed_seq|<random>
std::uniform_int_distribution|<random>
std::normal_distribution|<random>
std::default_random_engine|<random>
std::complex|<complex>
std::ratio|<ratio>
std::stop_token|<stop_token>
std::stop_source|<stop_token>
std::atexit|<cstdlib>
std::current_exception|<exception>
std::rethrow_exception|<exception>
std::declval|<utility>
std::errc|<system_error>
std::make_error_code|<system_error>
std::flush|<ostream> <iostream>
std::from_chars|<charconv>
std::to_chars|<charconv>
std::get_if|<variant> <tuple> <utility>
std::is_default_constructible|<type_traits>
std::is_default_constructible_v|<type_traits>
std::isfinite|<cmath>
std::isinf|<cmath>
std::isnan|<cmath>
std::istreambuf_iterator|<iterator> <istream> <streambuf> <iosfwd>
std::ostreambuf_iterator|<iterator> <ostream> <streambuf> <iosfwd>
std::streamsize|<ios> <iosfwd> <iostream> <istream> <ostream>
std::streamoff|<ios> <iosfwd>
std::lround|<cmath>
std::milli|<ratio>
std::kilo|<ratio>
std::random_device|<random>
std::uniform_real_distribution|<random>
std::strtod|<cstdlib>
std::strtof|<cstdlib>
std::strtold|<cstdlib>
std::u8string|<string>
std::u8string_view|<string_view>
std::future_status|<future>
std::chrono|<chrono>
TBL_EOF
)

# C++ 标准**明文保证**的头与头包含关系（不是实现细节，所以不算「靠传递包含」）：
#   [iostream.syn]  <iostream> 定义 <istream>/<ostream>/<ios>/<streambuf> 的实体
#   [fstream.syn]   <fstream>   定义 <istream>/<ostream>
#   [sstream.syn]   <sstream>   定义 <istream>/<ostream>
#   [iomanip]       <iomanip>   定义 <istream>/<ostream>/<ios>
# 这几条与我们要抓的东西**不是一类**：`<optional>` 经 `<map>` → `__node_handle`
# 传递进来是 libc++ 的实现细节、libstdc++ 根本没有；而 `<iostream>` 带出
# `<ostream>` 是标准写死的，换编译器照样成立。把它们混在一起报全是误报。
STD_CLOSURE="iostream ios istream ostream streambuf
fstream ios istream ostream streambuf
sstream ios istream ostream streambuf
iomanip ios istream ostream
ios istream ostream streambuf"

# ── 源文件清单：src/ + tests/ 的 .cpp/.h（3rd 与 build 不在范围内）──
list_sources() {
    local root="$1"
    local d
    for d in src tests; do
        [ -d "${root}/${d}" ] || continue
        find "${root}/${d}" \
            \( -name build -o -name .claude -o -name 3rd \) -prune -o \
            \( -name '*.cpp' -o -name '*.h' \) -type f -print
    done | LC_ALL=C sort
}

# ── 组装 awk 程序：表 + 逻辑，写进临时文件（-f 读文件避免引号地狱）──
build_awk() {
    local out="$1"
    # 1) 映射表 —— **必须整体包在 BEGIN { } 里**。
    #    ⚠ awk 把裸的 `HDR["k"] = "v"` 解析成「模式 HDR["k"] + 默认动作 {print}」，
    #    表一被填满（元素非空即真），之后**每一行原文都会被打印出来**——实测
    #    371 个文件吐了 5500 万行，管道背压把门禁挂死。踩过一次，记在这里。
    {
        echo "BEGIN {"
        printf '    NFILES = %s\n' "${2:-0}"
        printf '    NV = 0; NU2 = 0; NOPQ = 0; OPAQUE = 0\n'
        printf '    ROOTDIR = "%s"\n' "$ROOT"
        printf '%s\n' "$SYMTAB" | awk -F'|' '
            NF >= 2 {
                k = $1; v = $2;
                gsub(/^[ \t]+|[ \t]+$/, "", k);
                gsub(/^[ \t]+|[ \t]+$/, "", v);
                # ⚠ 必须**去掉尖括号**再存：SELF_SYS 的键是 include 出来的裸头名
                #   （"string"），带 <> 存就永远匹配不上 → 判据恒红、2300 条全误报。
                gsub(/[<>]/, "", v);
                if (k == "" || v == "") next;
                if (k in seen) next;
                seen[k] = 1;
                printf "    HDR[\"%s\"] = \"%s\"\n", k, v;
            }'
        echo "}"
    } > "$out"

    printf '%s\n' "$STD_CLOSURE" | awk '
    { for (i = 1; i < NF; i++) STD_E[NF, i] = $i
      STD_N[NF] = NF - 1 }' >> "$out"

    cat >> "$out" <<'LOGIC_EOF'

# 项目头索引（检查模式读入）：相对路径 \t 该头显式 include 的 <> 头（空格分隔）
BEGIN {
    if (IDX != "") {
        while ((getline ln < IDX) > 0) {
            pp = index(ln, "\t")
            if (pp == 0) continue
            key = substr(ln, 1, pp - 1)
            HSYS[key] = substr(ln, pp + 1)
            bn = key
            sub(/^.*\//, "", bn)
            if (!(bn in BASEIDX)) BASEIDX[bn] = key
        }
        close(IDX)
    }
}
function emitindex(   k, a, n, i, j, t, out) {
    n = 0
    for (k in SELF_SYS) { n++; a[n] = k }
    for (i = 2; i <= n; i++) {                 # 插入排序：输出可复现，diff 稳定
        t = a[i]; j = i - 1
        while (j >= 1 && a[j] > t) { a[j + 1] = a[j]; j-- }
        a[j + 1] = t
    }
    out = ""
    for (i = 1; i <= n; i++) out = (i == 1 ? a[i] : out " " a[i])
    print CUR "\t" out
}
# 把绝对路径转成相对 ROOT 的路径；不在 ROOT 下则原样返回
function relpath(p,   n) {
    n = length(ROOTDIR)
    if (substr(p, 1, n + 1) == ROOTDIR "/") return substr(p, n + 2)
    return p
}
function dirpart(p,   k, arr, n, i, out) {
    n = split(p, arr, "/")
    out = ""
    for (i = 1; i < n; i++) out = (out == "" ? "" : out "/") arr[i]
    return out
}
# 解析 `#include "x.h"`：先按 includer 所在目录，再按 src/、tests/ 根，最后才
# 退化到同名匹配。⚠ 同名替换必须放**最后**——放前面会把 "quota_store.h" 换成
#   仓库相对全路径，再拼目录就变成 "…/quota/src/…/quota/quota_store.h"，永远解析不到。
function resolve_quoted(from, g,   orig, d, c) {
    orig = g
    d = dirpart(from)
    c = (d == "" ? orig : d "/" orig)
    if (c in HSYS) return c
    c = "src/" orig
    if (c in HSYS) return c
    c = "tests/" orig
    if (c in HSYS) return c
    if (orig in BASEIDX) {
        c = BASEIDX[orig]
        if (c in HSYS) return c
    }
    return ""
}

# 剥掉一行里的注释与字符串字面量（内容丢弃，换行保留 → 行号不变）。
# 按**片段**拷贝而不是逐字符拼串：逐字符在 awk 里是 O(n^2)，全仓一万多行会跑成
# 分钟级。一次正则 match 找下一个「起始构造」，中间整段 memcpy。
function cleanse(line,   out, i, n, tail, c, q, sp, ep) {
    out = ""; i = 1; n = length(line)
    while (i <= n) {
        if (INBLOCK) {
            if (substr(line, i, 2) == "*/") { INBLOCK = 0; i += 2; continue }
            ep = index(substr(line, i + 1), "*/")
            if (ep == 0) break
            i = i + ep + 2
            continue
        }
        tail = substr(line, i)
        # 后面再没有注释/字符串构造了 → 剩下的整段都是真代码，必须收下。
        # ⚠ 这里漏掉 `out = out tail` 会让**整行**被丢掉，`#include <x>` 一个都
        #   扫不到（判据整个退化成 naive）。踩过一次：492 条误报的根因。
        if (!match(tail, /\/\/|\/\*|R"|"|'/)) { out = out tail; break }
        sp = i + RSTART - 1
        out = out substr(line, i, sp - i)
        c = substr(line, sp, 2)
        if (c == "//") break
        if (c == "/*") {
            INBLOCK = 1; i = sp + 2
            ep = index(substr(line, i), "*/")
            if (ep == 0) break
            i = i + ep + 2
            continue
        }
        if (c == "R\"") {
            i = sp + 2
            ep = index(substr(line, i), ")")
            if (ep == 0) break
            i = i + ep + 1
            continue
        }
        q = substr(line, sp, 1)
        i = sp + 1
        while (i <= n) {
            c = substr(line, i, 1)
            if (c == "\\") { i += 2; continue }
            i++
            if (c == q) break
        }
    }
    CLEAN = out
}

# 0 = 已显式 include；1 = 缺（真实风险）；2 = 符号未登记
function resolve(sym,   cand, m, h, i) {
    cand = sym
    while (cand != "" && cand != "std") {
        if (cand in HDR) {
            m = split(HDR[cand], h, " ")
            for (i = 1; i <= m; i++)
                if (h[i] != "" && (h[i] in EFFECTIVE)) return 0
            return 1
        }
        # ⚠ 不能用 split(s, a, "::")：awk 的分隔符是正则，"::" 等价于 ":"，
        # 重建前缀会让串每轮变长 → 死循环（真踩过）。改成砍掉最后一段。
        if (!match(cand, /::[A-Za-z_][A-Za-z0-9_]*$/)) return 2
        cand = substr(cand, 1, RSTART - 1)
    }
    return 2
}
function HDRCANON(sym,   cand, i, m, h, out) {
    cand = sym
    while (cand != "" && cand != "std") {
        if (cand in HDR) {
            m = split(HDR[cand], h, " ")
            out = ""
            for (i = 1; i <= m; i++)
                if (h[i] != "") out = (out == "" ? "<" h[i] ">" : out " 或 <" h[i] ">")
            return (out == "" ? "?" : out)
        }
        if (!match(cand, /::[A-Za-z_][A-Za-z0-9_]*$/)) return "?"
        cand = substr(cand, 1, RSTART - 1)
    }
    return "?"
}
FNR == 1 {
    if (CUR != "") flush()
    CUR = relpath(FILENAME)
    INBLOCK = 0
    delete SELF_SYS
    delete SELF_QUO
    delete USED          # ⚠ 漏了这一行 → USED 跨文件累积，前面文件已见过的符号
                         #   在本文件里被当成"见过了"跳过 → NS 越小、漏报越多。
                         #   实测：quota_store.cpp 单跑 9 条，排在第 130 位后变 0 条。
                         #   「同一个门禁跑全树和跑单文件结论不同"就是它。
    NU = 0; NS = 0
    delete SYM; delete USING; delete USINGLINE
}
{
    scanline()
}
# 每行的采集单独成函数：awk 里裸 action 的临时变量都是全局的，会和 flush()/
# resolve() 的循环下标互相踩（踩一次就是死循环或漏报 —— 两个都真发生过）。
function scanline(   t, h, u, s, raw) {
    # include 必须从**原始行**取，不能用 cleanse 之后的行：cleanse 会把字符串
    # 字面量挖空，`#include "foo.h"` 的两个引号被抹掉 → 引号正则永远匹配不上 →
    # 「项目头显式提供了声明」的豁免从来没生效过（实测误报 612 条里的绝大部分）。
    # 用 `^[ \t]*#` 锚定行首 → 注释掉的 include（"// #include ..."）不会被误收。
    raw = $0
    while (match(raw, /^[ \t]*#[ \t]*include[ \t]*<[A-Za-z0-9_.+\/-]+>/)) {
        h = substr(raw, RSTART, RLENGTH)
        sub(/^[^<]*</, "", h); sub(/>$/, "", h)
        SELF_SYS[h] = 1
        raw = substr(raw, RSTART + RLENGTH)
    }
    raw = $0
    while (match(raw, /^[ \t]*#[ \t]*include[ \t]*"[^"]+"/)) {
        h = substr(raw, RSTART, RLENGTH)
        sub(/^[^"]*"/, "", h); sub(/"$/, "", h)
        SELF_QUO[h] = 1
        raw = substr(raw, RSTART + RLENGTH)
    }
    cleanse($0)
    t = CLEAN
    while (match(t, /using[ \t]+namespace[ \t]+std::[A-Za-z_][A-Za-z0-9_]*/)) {
        u = substr(t, RSTART, RLENGTH)
        sub(/^.*std::/, "", u)
        NU++; USING[NU] = u; USINGLINE[NU] = FNR
        t = substr(t, RSTART + RLENGTH)
    }
    t = CLEAN
    while (match(t, /std::[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*/)) {
        s = substr(t, RSTART, RLENGTH)
        if (!(s in USED)) { USED[s] = FNR; NS++; SYM[NS] = s }
        t = substr(t, RSTART + RLENGTH)
    }
}
# 有效 include 集 = 本文件的 <> 头 ∪（本文件 "..." 到的项目头里显式写的 <> 头）
# 后者就是「既有风格」的豁免口径（本项目自己的头显式声明，不是标准库偶然传递）。
function compute_effective(   k, tgt, n, arr2, j, cl) {
    delete EFFECTIVE
    for (k in SELF_SYS) EFFECTIVE[k] = 1
    # 标准保证的头与头包含关系（见 STD_CLOSURE）——不算「靠传递包含」
    for (k in SELF_SYS)
        if (k in STD_N)
            for (j = 1; j <= STD_N[k]; j++) EFFECTIVE[STD_E[k, j]] = 1
    for (k in SELF_QUO) {
        tgt = resolve_quoted(CUR, k)
        if (tgt == "") {
            # 引用的头不在本门禁索引范围内（第三方 / EUI-NEO / 仓库外）：
            # 记成「无法判定」，**不**当违规——否则全是误报，守卫会被关掉。
            OPAQUE++
            if (!(CUR in OPQ)) { OPQ[CUR] = 1; NOPQ++; OPQLIST[NOPQ] = CUR }
            continue
        }
        n = split(HSYS[tgt], arr2, " ")
        for (j = 1; j <= n; j++)
            if (arr2[j] != "") EFFECTIVE[arr2[j]] = 1
    }
    # 项目头也常用尖括号引（`#include <ui_kit/card.h>`，见 ui_kit/ 与 eui/ 的用法）。
    # 这些会落进 SELF_SYS；解析得到项目头就照样采信它的显式 include，解析不到就
    # 当作普通系统/第三方头忽略（**不**记 OPAQUE——尖括号几乎总是系统头）。
    for (k in SELF_SYS) {
        tgt = resolve_quoted(CUR, k)
        if (tgt == "") continue
        n = split(HSYS[tgt], arr2, " ")
        for (j = 1; j <= n; j++)
            if (arr2[j] != "" && !(arr2[j] in EFFECTIVE)) EFFECTIVE[arr2[j]] = 1
    }
}
function flush(   i, s, r, u) {
    if (MODE == "index") {
        emitindex()
        NS = 0; NU = 0
        delete SYM; delete USING; delete USINGLINE
        return
    }
    compute_effective()
    for (i = 1; i <= NS; i++) {
        s = SYM[i]
        r = resolve(s)
        if (r == 1) { NV++; VIO[NV] = CUR ":" USED[s] ": 直接使用 " s "，但本文件与它直接 include 的项目头都没显式 include " HDRCANON(s) "（靠标准库内部传递包含才可见）" }
        else if (r == 2) { NU2++; UNM[NU2] = CUR ":" USED[s] ": 未登记符号 " s }
    }
    for (i = 1; i <= NU; i++) {
        u = USING[i]
        if (!(("std::" u) in HDR)) continue
        if (resolve("std::" u) == 1) { NV++; VIO[NV] = CUR ":" USINGLINE[i] ": using namespace std::" u "，但没显式 include " HDRCANON("std::" u) }
    }
    NS = 0; NU = 0
    delete SYM; delete USING; delete USINGLINE
}
END {
    if (CUR != "") flush()
    if (MODE == "index") exit 0
    for (e = 1; e <= NV; e++) print VIO[e]
    for (e = 1; e <= NU2; e++) print "UNMAPPED " UNM[e] > "/dev/stderr"
    for (e = 1; e <= NOPQ; e++) print "OPAQUE " OPQLIST[e] > "/dev/stderr"
    print "SUMMARY files=" NFILES " violations=" NV " unmapped=" NU2 " opaque_files=" NOPQ > "/dev/stderr"
    if (NV > 0) exit 1
    exit 0
}
LOGIC_EOF
}

FILES="$(list_sources "$ROOT")"
NFILES="$(printf '%s\n' "$FILES" | grep -c . || true)"
if [ "$NFILES" -lt 1 ]; then
    echo "❌ 源文件清单为空（0 个）—— 守卫没有实际检查任何东西，拒绝报绿" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
AWKF="$TMP/check.awk"
build_awk "$AWKF" "$NFILES"

# 只索引项目头（.cpp 的 include 在检查时现场采集）
HEADERS="$(printf '%s\n' "$FILES" | grep '\.h$' || true)"

# run_gate <待检文件清单> [额外的项目头清单]
# 两遍：MODE=index 产出项目头的显式 <> 头清单；MODE=check 据此算有效集。
# ⚠ 每遍都单独判「结果为空」：「匹配 0 条」和「没红」是两件事，空跑一遍的守卫
#   会以 0 退出——那正是仓库反复翻车的那类假绿。
run_gate() {
    local files="$1" extra="${2:-}"
    local idx="$TMP/hdr.idx" out rc nidx hdrs
    hdrs="$HEADERS"
    [ -n "$extra" ] && hdrs="${hdrs}
${extra}"
    nidx_h="$(printf '%s\n' "$hdrs" | grep -c . || true)"
    if [ "${nidx_h:-0}" -lt 1 ]; then
        echo "❌ 项目头清单为空（0 条）—— 判据会退化成 naive 模式，拒绝报绿" >&2
        return 2
    fi
    idx="$TMP/hdr.idx"
    : > "$idx"
    printf '%s\n' "$hdrs" | tr '\n' ' ' | \
        xargs awk -v MODE=index -f "$AWKF" > "$idx" 2>/dev/null
    nidx="$(grep -c . "$idx" || true)"
    if [ "${nidx:-0}" -lt 1 ]; then
        echo "❌ 项目头索引产出 0 条（清单 ${nidx_h} 个）—— 索引器坏了，拒绝报绿" >&2
        return 2
    fi
    nf="$(printf '%s\n' "$files" | grep -c . || true)"
    if [ "${nf:-0}" -lt 1 ]; then
        echo "❌ 待检清单为空（0 条）—— 拒绝报绿" >&2
        return 2
    fi
    out="$(printf '%s\n' "$files" | tr '\n' ' ' | \
        xargs awk -v MODE=check -v IDX="$idx" -f "$AWKF" 2>"$TMP/err")"
    rc=$?
    echo "$out"
    cat "$TMP/err"
    return "$rc"
}

if [ "$SELF_TEST" -eq 0 ]; then
    echo "std include 守卫：扫描 ${NFILES} 个文件（${ROOT}/src + ${ROOT}/tests）"
    OUT="$(run_gate "$FILES")"
    rc=$?
    if [ "$rc" -eq 2 ]; then exit 2; fi
    if [ "$rc" -ne 0 ]; then
        # 只给违规行加 ❌；SUMMARY / UNMAPPED / OPAQUE 是诊断行，不许混进计数里
        # （混过一次，OPAQUE 被当成违规，文件数与违规数全错）。
        printf '%s\n' "$OUT" | grep -E '^[A-Za-z0-9_./-]+:[0-9]+: ' | sed 's/^/  ❌ /'
        echo "❌ 有文件「直接用了 std 符号，却只靠标准库内部传递包含才看得见声明」"
        echo "   修法：给该文件补上列出的一行标准头 include。"
        exit 1
    fi
    [ "$VERBOSE" -eq 1 ] && printf '%s\n' "$OUT" | grep -E '^[A-Za-z0-9_./-]+:[0-9]+: ' | sed 's/^/  /'
    echo "✅ 全部 ${NFILES} 个文件：直接使用的 std 符号都有显式 include（本文件或它直接 include 的项目头里）"
    exit 0
fi

# =====================================================================
# --self-test
# =====================================================================
if [ "$SELF_TEST" -eq 1 ]; then
    ok=0
    echo "  探针文件：src/module_claude_proxy/quota/quota_store.cpp"
    PROBE_REL="src/module_claude_proxy/quota/quota_store.cpp"

    # ── 好树：真实源码必须 0 违规（证明"绿"是真绿）──
    if OUT="$(run_gate "$FILES" 2>/dev/null)"; then
        echo "  ✅ 好树：真实源码 0 违规"
    else
        echo "  ❌ 好树：真实源码竟有违规 —— 先修好树，否则下面的红没有意义"
        printf '%s\n' "$OUT" | head -20 | sed 's/^/       /'
        ok=1
    fi

    if ! grep -q '^#include <optional>$' "${ROOT}/${PROBE_REL}"; then
        echo "  ❌ 坏树 A/B/C 的前置条件不成立：探针文件里没有 '#include <optional>' 行"
        exit 1
    fi

    mk_probe() { :; }   # 探针文件位置固定在 PROBE_REL，无需生成器

    # ── 坏树 A：删掉一行 #include <optional> → 必须红 ──
    mkdir -p "$TMP/badA"
    grep -v '^#include <optional>$' "${ROOT}/${PROBE_REL}" > "$TMP/badA/probe.cpp"
    if diff -q "${ROOT}/${PROBE_REL}" "$TMP/badA/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 A：变异是空操作 —— 这条自测无效"; ok=1
    elif OUT="$(run_gate "$TMP/badA/probe.cpp" 2>/dev/null)"; then
        echo "  ❌ 坏树 A：删掉 #include <optional> 后仍绿 —— **假绿守卫**"; ok=1
    else
        echo "  ✅ 坏树 A：红 — $(printf '%s\n' "$OUT" | grep -v '^  ' | head -1 | cut -c1-110)"
    fi

    # ── 坏树 B：塞一行裸 std::optional 用法（且不给 <optional>）→ 必须红 ──
    mkdir -p "$TMP/badB"
    { grep -v '^#include <optional>$' "${ROOT}/${PROBE_REL}"
      echo 'namespace { std::optional<int> bad_probe_xyz = {}; }'
    } > "$TMP/badB/probe.cpp"
    if diff -q "${ROOT}/${PROBE_REL}" "$TMP/badB/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 B：变异是空操作 —— 这条自测无效"; ok=1
    elif OUT="$(run_gate "$TMP/badB/probe.cpp" 2>/dev/null)"; then
        echo "  ❌ 坏树 B：塞了裸 std::optional 用法仍绿 —— **假绿守卫**"; ok=1
    else
        echo "  ✅ 坏树 B：红 — $(printf '%s\n' "$OUT" | grep -v '^  ' | head -1 | cut -c1-110)"
    fi

    # ── 坏树 C：把 <optional> 换成「项目头里显式 include <optional>」→ 必须**绿** ──
    #     证明「既有风格豁免」这条真在生效，而不是一律报红（否则这守卫会变成
    #     「所有文件都得重复 include」的无差别刷屏，迟早被人关掉）。
    mkdir -p "$TMP/projhdr"
    printf '#pragma once\n#include <optional>\n' > "$TMP/projhdr/optional_provider.h"
    mkdir -p "$TMP/badC"
    { echo '#include "optional_provider.h"'
      grep -v '^#include <optional>$' "${ROOT}/${PROBE_REL}"
    } > "$TMP/badC/probe.cpp"
    if diff -q "${ROOT}/${PROBE_REL}" "$TMP/badC/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 C：变异是空操作 —— 这条自测无效"; ok=1
    else
        # 索引里得有 projhdr/optional_provider.h：把它当项目头一起扫
        if OUT="$(run_gate "$TMP/badC/probe.cpp" "$TMP/projhdr/optional_provider.h" 2>/dev/null)"; then
            echo "  ✅ 坏树 C：绿（声明由项目头显式提供 → 判为既有风格，不报）—— 豁免真在生效"
        else
            echo "  ❌ 坏树 C：经项目头显式提供 <optional> 仍红 —— 豁免判据写错了"
            printf '%s\n' "$OUT" | grep -v '^  ' | head -3 | sed 's/^/       /'
            ok=1
        fi
    fi

    if [ "$ok" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
    else
        echo "✅ --self-test 通过：好树绿 / 坏树 A·B 红 / 坏树 C 绿（豁免生效）—— 非恒真"
    fi
    exit "$ok"
fi