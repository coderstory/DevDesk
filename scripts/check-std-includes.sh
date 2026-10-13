#!/usr/bin/env bash
# scripts/check-std-includes.sh
# 守卫：本文件直接用了 std::X，却只靠**标准库内部**的传递包含才看得见 → 报红。
#
# 为什么这道门禁必须静态、「编一遍」不够：缺 include 在本机和交叉链上都不会炸 ——
# macOS libc++ 与 llvm-mingw 的 libc++ 都经 `core/sqlite_db.h` → <map> →
# <__node_handle> → <optional> 把 std::optional 传递带进来（clang -H 实测到的链条），
# 而 CI 的 MSYS2 libstdc++ 不带（quota_store.cpp:43 就是在 CI 上炸的）。只要标准库
# 自己还在传递包含，任何编译型门禁都抓不到这一类 bug。本门禁改用与标准库无关的判据：
# 本文件直接写了 std::X，而能声明它的头既不在本文件、也不在本文件直接 include 的
# 本项目头文件里 → 报红。
#
# 跨文件不变量：`quota_store.cpp` 是**共享文件**（Windows CI 编它），而
# check-windows-cross-compile.sh 的早期版本只编 `*_win.cpp` —— 共享文件的缺 include
# 只有 Windows CI 那台编译器会报，所以「本机绿」在这道门禁上没有任何证据力。
#
# 口径：区分「真实风险」（报）与「既有风格」（不报）
#   ① 既有风格，不报：本文件直接 include 的某个**本项目头文件**里显式写了 `<X>`。
#      那条依赖链是我们自己控制的（头文件作者显式声明），不是标准库实现细节的偶然
#      产物。为这类文件补 include 是「顺手大改所有文件的 include 块」。
#   ② 真实风险，报：`<X>` 只经由**标准库自己头与头之间的传递包含**才可见
#      （`__node_handle` → `<optional>` 这类）。这类链条随标准库版本/实现漂移，
#      换编译器就断。
#
# 验到了什么 / 没验什么
#   ✅ .cpp / .h 里直接使用的 std 符号，既没在本文件 include、也没在直接 include 的
#      项目头里显式声明，以及 `using namespace std::X` 的同类缺头。
#   ❌ 第三方库头（curl / sqlite / openssl / httplib / EUI-NEO）内部的 include ——
#      不在本门禁索引范围内，记成「无法判定」而非违规。
#   ❌ 整块被 `#if` 关掉的分支（本门禁不跑预处理器）、宏拼出来的符号、头文件本身能不能
#      编过（那是 check-windows-cross-compile.sh 与 CI 的活）。
#   ❌ 项目头 → 项目头 → `<X>` 的**两级以上**间接（只做一级；本仓几乎没有这种写法，
#      两级会归入「无法判定」）。
#
# --self-test：自证非恒真。好树必须 0 违规；每条坏树都先 `diff` 断言**真的改了文件**
#   （等价变异 = 零证据），再要求它变红。
#   坏树 A：删掉某文件的一行 `#include <optional>` → 必须红（CI 那次事故的复现）
#   坏树 B：往某文件塞一行裸 `std::optional<int> bad_probe_xyz;` → 必须红
#   坏树 C：把文件换成「只经项目头拿到 std 符号」的形态 → 必须**绿**（证明 ① 的豁免
#     真的在起作用，不是一律报红）
#   坏树 D：只给 `<map>` + 裸 `std::optional` → 必须红（② 的反证：libc++ 经
#     __node_handle 带进来、libstdc++ 不带的那一类）
#   坏树 E：只给 `<fstream>` + `std::ios::binary` → 必须**绿**（标准强制关系的反向证据：
#     豁免真在收窄，而不是要求每个文件重复 include）
#   坏树 F：只给 `<ios>` + `std::istream` → 必须红（豁免**不得**越界：`<ios>` 的
#     synopsis 只有 `#include <iosfwd>`，给的是不完整类型）
#   D/E/F 三条同时是 `STD_CLOSURE` 表的回归护栏：删任一行、或加回旧的错误行
#   `ios istream ostream streambuf`，对应自测必红。
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

# 符号 → 头 映射表。接受列表（空格分隔的若干头）：任一头被显式 include 即通过；
# 故意比「最小集合」宽（std::accumulate 同时接受 <numeric> 与 <algorithm>，C++17 起
# 两处都声明）：判据是「有没有显式包含一个能声明它的头」，不是「唯一正解」。
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
# 接受列表的宽度 = 「标准**明文**声明该符号的头」的集合，不是「唯一正解」。
#   size_t / ptrdiff_t：<cstddef> 是归属头；但 C 标准兼容头 <cstdio> <cstdlib>
#   <cstring> <ctime> 的 synopsis 里**明文**写了 `using size_t = ...`（实测草案正文），
#   所以这四个同样是**明文保证**，任一被显式 include 即通过 —— 与
#   `std::abs|<cstdlib> <cmath>` 同一判据，不是给 size_t 开的特例。
#   ⚠ 刻意**不含** <string>/<vector>/<map>：它们能带出 size_t 是**传递包含**（实现
#   细节，libc++ 与 libstdc++ 恰好都带），不属于明文保证 —— 与 <optional> 经 <map>
#   同属一类，正是本守卫要抓的。
#   byte / max_align_t / nullptr_t：**只有** <cstddef> 声明（实测 <cstdio>/<cstdlib>/
#   <cstring> 的 synopsis 里 nullptr_t 出现 0 次，且单独 `#include <cstdlib>` +
#   `std::nullptr_t x;` 编译失败），故接受列表只有一项。
std::size_t|<cstddef> <cstdio> <cstdlib> <cstring> <ctime>
std::ptrdiff_t|<cstddef> <cstdio> <cstdlib> <cstring> <ctime>
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

# C++ 标准**强制保证**的头与头包含关系（不是实现细节，所以不算「靠传递包含」）。
# 每行的第一个 token 是「本文件 include 的头」，其余是它**标准保证带到**的头。
# 依据全部出自标准正文：
#   [iostream.syn] synopsis 明写 4 行 #include：<ios> <streambuf> <istream> <ostream>。
#   [fstream.syn]/[sstream.syn]/[istream.syn]/[ostream.syn] 的 synopsis **没有**任何
#       #include 行 —— 所以它们带出 <ios> 不是 synopsis 明写的，而是**推导**出来的：
#         basic_filebuf  : public basic_streambuf     ([filebuf])
#         basic_ifstream : public basic_istream       ([ifstream])
#         basic_istream  : virtual public basic_ios   ([istream])
#       [class.derived.general]/2：基类 shall denote a class type that is **not an
#       incompletely defined class**。要定义 basic_ifstream 就必须有**完整**的
#       basic_istream，要定义它又必须有**完整**的 basic_ios —— 传递闭包是强制的。
#   [iomanip] synopsis 同样无 #include，但它声明的操纵符形参类型是 basic_ostream /
#       basic_istream，由上面的闭包推得。
# 反向一侧：`<ios>` **不**保证 istream/ostream —— `<ios>` 的 synopsis 只有一行
#   `#include <iosfwd>`，而 `<iosfwd>` 只给**不完整**的 basic_istream/basic_ostream
#   （`#include <ios>` + `std::istream x;` 编译失败）。所以表里**不能**有
#   "ios istream ostream streambuf" 那一行：留着就是让守卫对真缺失放行 = 假绿。
#   坏树 F 就是钉这一条的。
# 这几条与我们要抓的东西**不是一类**：`<optional>` 经 `<map>` → `__node_handle` 传递
#   进来是 libc++ 的实现细节、libstdc++ 根本没有（[map.overview] 的 synopsis 里 0 次
#   node_handle）；`<fstream>` 带出 `<ios>` 则是标准强制的，换编译器照样成立。
STD_CLOSURE="iostream ios istream ostream streambuf
fstream ios istream ostream streambuf
sstream ios istream ostream streambuf
iomanip ios istream ostream"

# 源文件清单 = src/ + tests/ 的 .cpp/.h；3rd 与 build 目录被 -prune 掉，不在范围内。
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

# 组装 awk 程序：表 + 逻辑写进临时文件，用 -f 读它（避免引号地狱）。
build_awk() {
    local out="$1"
    # 1) 映射表 —— **必须整体包在 BEGIN { } 里**。awk 把裸的 `HDR["k"] = "v"` 解析成
    #    「模式 HDR["k"] + 默认动作 {print}」，表一被填满（元素非空即真），之后**每一行
    #    输入都会被打印** —— 371 个文件能吐 5500 万行，管道背压直接把门禁挂死。
    #    下面 STD_CLOSURE 的产物是同一个坑，一并用 BEGIN 封死。
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

    # STD_CLOSURE → awk 的二维数组 STD_E[头, i] / STD_N[头]。
    # 这里**必须有 print**：只赋值不输出，awk 一行都不吐（生成 0 字节）→ STD_E/STD_N
    #   全空 → compute_effective 里的标准保证豁免**整条从未生效**，而门禁照样一路绿。
    #   所以判据是**代码自证**而不是靠注释提醒：生成完立刻断言产物非空且含赋值。
    # 产物必须包在 BEGIN{} 里：裸的 `STD_N["x"] = 2` 会被 awk 当成「模式 + 默认动作
    #   {print}」，表一填满元素就恒真，之后每一行输入都会被打印（同上）。
    _closure_awk="$out.closure"
    printf '%s\n' "$STD_CLOSURE" | awk '
    NF >= 2 { key = $1
               for (i = 2; i <= NF; i++) STD_E[key, i - 1] = $i
               STD_N[key] = NF - 1 }
    END {
        print "BEGIN {"
        for (k in STD_N) {
            line = "    STD_N[\"" k "\"] = " STD_N[k]
            for (i = 1; i <= STD_N[k]; i++)
                line = line "; STD_E[\"" k "\", " i "] = \"" STD_E[k, i] "\""
            print line
        }
        print "}"
    }' > "$_closure_awk"
    # 生成结果必须**结构正确**：键得是合法头名（非数字、含字母）。拿 NF 当键就是数字，
    # 产物看着有内容、实际一条都匹配不上 —— 「跑通了、绿了、结论是错的」，这里用代码
    # 挡住而不是靠注释。
    if ! grep -qE 'STD_N\["[a-z_]+"\]' "$_closure_awk"; then
        echo "❌ STD_CLOSURE 产物结构非法（键必须是头名，不是字段数）—— 豁免判据已失效，拒绝报绿" >&2
        return 2
    fi
    cat "$_closure_awk" >> "$out"
    rm -f "$_closure_awk"

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
# 必须接住返回码：build_awk 的自证失败是 `return 2`；本脚本没开 set -e，不接就会带着
#   空的 awk 程序继续跑完整个门禁 → 假绿。
build_awk "$AWKF" "$NFILES"
if [ $? -eq 2 ]; then
    echo "❌ 检查器自身构建失败（豁免表解析器没生效）—— 拒绝报绿" >&2
    exit 2
fi
if ! grep -q 'STD_N\["' "$AWKF" 2>/dev/null; then
    echo "❌ 生成的 awk 里没有 STD_N 赋值 —— 标准保证豁免未生效，拒绝报绿" >&2
    exit 2
fi

# 只索引项目头（.cpp 的 include 在检查时现场采集）
HEADERS="$(printf '%s\n' "$FILES" | grep '\.h$' || true)"

# run_gate <待检文件清单> [额外的项目头清单]
# 两遍：MODE=index 产出项目头的显式 <> 头清单；MODE=check 据此算有效集。
# 每遍都单独判「结果为空」：空跑一遍的守卫会以 0 退出，而「匹配 0 条」与「没红」是两件事。
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
        # 只给违规行加 ❌；SUMMARY / UNMAPPED / OPAQUE 是诊断行，混进计数会让文件数
        # 与违规数一起算错。
        printf '%s\n' "$OUT" | grep -E '^[A-Za-z0-9_./-]+:[0-9]+: ' | sed 's/^/  ❌ /'
        echo "❌ 有文件「直接用了 std 符号，却只靠标准库内部传递包含才看得见声明」"
        echo "   修法：给该文件补上列出的一行标准头 include。"
        exit 1
    fi
    [ "$VERBOSE" -eq 1 ] && printf '%s\n' "$OUT" | grep -E '^[A-Za-z0-9_./-]+:[0-9]+: ' | sed 's/^/  /'
    echo "✅ 全部 ${NFILES} 个文件：直接使用的 std 符号都有显式 include（本文件或它直接 include 的项目头里）"
    exit 0
fi

if [ "$SELF_TEST" -eq 1 ]; then
    ok=0
    echo "  探针文件：src/module_claude_proxy/quota/quota_store.cpp"
    PROBE_REL="src/module_claude_proxy/quota/quota_store.cpp"

    # 好树：真实源码必须 0 违规（证明"绿"是真绿）。
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

    # 坏树 A：删掉一行 #include <optional> → 必须红
    mkdir -p "$TMP/badA"
    grep -v '^#include <optional>$' "${ROOT}/${PROBE_REL}" > "$TMP/badA/probe.cpp"
    if diff -q "${ROOT}/${PROBE_REL}" "$TMP/badA/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 A：变异是空操作 —— 这条自测无效"; ok=1
    elif OUT="$(run_gate "$TMP/badA/probe.cpp" 2>/dev/null)"; then
        echo "  ❌ 坏树 A：删掉 #include <optional> 后仍绿 —— **假绿守卫**"; ok=1
    else
        echo "  ✅ 坏树 A：红 — $(printf '%s\n' "$OUT" | grep -v '^  ' | head -1 | cut -c1-110)"
    fi

    # 坏树 B：塞一行裸 std::optional 用法（且不给 <optional>）→ 必须红
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

    # 坏树 C：把 <optional> 换成「项目头里显式 include <optional>」→ 必须**绿**，
    # 证明「既有风格豁免」真在生效而不是一律报红（否则守卫退化成「所有文件都得重复
    # include」的无差别刷屏，迟早被人关掉）。
    mkdir -p "$TMP/projhdr"
    printf '#pragma once\n#include <optional>\n' > "$TMP/projhdr/optional_provider.h"
    # 坏树 D/E/F 的额外项目头只为满足 run_gate 的前置条件，它自身**不提供**任何 std
    # 声明 —— 否则豁免的功劳会被算到它头上，测不出真判据。
    printf '#pragma once\n' > "$TMP/projhdr/dummy_provider.h"
    mkdir -p "$TMP/badC"
    { echo '#include "optional_provider.h"'
      grep -v '^#include <optional>$' "${ROOT}/${PROBE_REL}"
    } > "$TMP/badC/probe.cpp"
    if diff -q "${ROOT}/${PROBE_REL}" "$TMP/badC/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 C：变异是空操作 —— 这条自测无效"; ok=1
    else
        # 豁免要成立，optional_provider.h 必须作为项目头进索引（run_gate 的第二个参数）
        if OUT="$(run_gate "$TMP/badC/probe.cpp" "$TMP/projhdr/optional_provider.h" 2>/dev/null)"; then
            echo "  ✅ 坏树 C：绿（声明由项目头显式提供 → 判为既有风格，不报）—— 豁免真在生效"
        else
            echo "  ❌ 坏树 C：经项目头显式提供 <optional> 仍红 —— 豁免判据写错了"
            printf '%s\n' "$OUT" | grep -v '^  ' | head -3 | sed 's/^/       /'
            ok=1
        fi
    fi

    # 坏树 D（判据②的反证）：符号只经**实现细节**传递才可见 → 必须红。只给 <map> 不给
    # <optional> —— libc++ 经 __node_handle 带进来、libstdc++ 不带的那一类，正是
    # quota_store.cpp 炸 CI 的成因；守卫若是恒绿的假绿守卫，这条会绿。
    mkdir -p "$TMP/badD"
    printf '#include "dummy_provider.h"\n#include <map>\nnamespace {\nstd::optional<int> bad_probe_map_chain = {};\n}\n' \
        > "$TMP/badD/probe.cpp"
    if diff -q /dev/null "$TMP/badD/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 D：变异是空操作 —— 这条自测无效"; ok=1
    elif OUT="$(run_gate "$TMP/badD/probe.cpp" "$TMP/projhdr/dummy_provider.h" 2>/dev/null)"; then
        echo "  ❌ 坏树 D：靠 <map>→实现细节传递才可见的 std::optional 竟绿 —— **判据第③条失效**"; ok=1
    else
        echo "  ✅ 坏树 D：红 — $(printf '%s\n' "$OUT" | grep -v '^  ' | head -1 | cut -c1-110)"
    fi

    # 坏树 E（标准强制关系的反向证据）：只留 <fstream>、用 std::ios::binary → 必须绿。
    # <fstream> 带出完整 basic_ios 是强制的（[ifstream] basic_ifstream : public
    # basic_istream，[class.derived.general]/2 基类不得是不完整类型），所以这不是缺
    # include。若这条会红 = 豁免没收窄，守卫退化成「每个文件都得重复 include」的刷屏，
    # 迟早被人关掉 —— 这条就是防那个的。
    mkdir -p "$TMP/badE"
    printf '#include "dummy_provider.h"\n#include <fstream>\nnamespace {\nstd::ofstream bad_probe_out;\nint bad_probe_mode() { return std::ios::binary | std::ios::trunc; }\n}\n' \
        > "$TMP/badE/probe.cpp"
    if diff -q /dev/null "$TMP/badE/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 E：变异是空操作 —— 这条自测无效"; ok=1
    elif OUT="$(run_gate "$TMP/badE/probe.cpp" "$TMP/projhdr/dummy_provider.h" 2>/dev/null)"; then
        echo "  ✅ 坏树 E：绿（std::ios 由 <fstream> 经标准强制关系带出 → 不算缺头）—— 豁免真在生效"
    else
        echo "  ❌ 坏树 E：<fstream> 已标准保证 std::ios 仍红 —— 豁免判据写错了"
        printf '%s\n' "$OUT" | grep -v '^  ' | head -3 | sed 's/^/       /'
        ok=1
    fi

    # 坏树 F（豁免**不得**越界的那一侧）：<ios> **不**保证 istream → 必须红。
    # STD_CLOSURE 若有 "ios istream ostream streambuf" 那一行就是假绿：<ios> 的
    # synopsis 只有 `#include <iosfwd>`，只给**不完整**的 basic_istream（`#include <ios>`
    # + `std::istream x;` 编译失败）。这条红就是钉死它的。
    mkdir -p "$TMP/badF"
    printf '#include "dummy_provider.h"\n#include <ios>\nnamespace {\nstd::istream bad_probe_by_value;\n}\n' \
        > "$TMP/badF/probe.cpp"
    if diff -q /dev/null "$TMP/badF/probe.cpp" >/dev/null 2>&1; then
        echo "  ❌ 坏树 F：变异是空操作 —— 这条自测无效"; ok=1
    elif OUT="$(run_gate "$TMP/badF/probe.cpp" "$TMP/projhdr/dummy_provider.h" 2>/dev/null)"; then
        echo "  ❌ 坏树 F：<ios> 并不保证 std::istream 却放行 —— 豁免**越界**了（假绿）"; ok=1
    else
        echo "  ✅ 坏树 F：红 — $(printf '%s\n' "$OUT" | grep -v '^  ' | head -1 | cut -c1-110)"
    fi

    if [ "$ok" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
    else
        echo "✅ --self-test 通过：好树绿 / 坏树 A·B·D·F 红 / 坏树 C·E 绿（豁免生效）—— 非恒真"
    fi
    exit "$ok"
fi