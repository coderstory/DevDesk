#!/usr/bin/env bash
# scripts/check-windows-cross-compile.sh
# Windows CI 源文件集合的**交叉编译**守卫
#
# ── 受编集合（本门禁最容易被"改进"坏的地方）──────────────────────────
# 受编集合 = **从 CMakeLists.txt 解析出来的、WIN32 分支下真正参与构建的源文件**。
# **不要退回只编 `*_win.cpp`，也不要改成手写清单**：Windows CI 编的是**整棵树**，
# `claude_service.cpp` / `home_service.cpp` 这些**共享文件**它全编。
# 真实事故：共享 cpp 缺 `#include <optional>`，只编 *_win.cpp 的门禁压根看不见，
# 红得毫无预警。手写清单也必然随
# 新文件漂移，漂了就是又一次静默盲区。
#
# ── 解析规则（解析器本身也要能被检查，见下面的交叉校验）──────────────
#   逐个 CMakeLists.txt 行扫：维护 if/elseif/else/endif 的分支栈，只要栈里没有
#   "处在非 WIN32 分支"，就把该行里的 .cpp token 收进来。
#   token 前缀解析：`${CMAKE_SOURCE_DIR}/` → 仓库根；`${…}`（其它变量）→ 跳过；
#   裸 `x.cpp` → 相对该 CMakeLists 所在目录；`file(GLOB …)` 的通配结果跳过
#   （守卫只保证「显式登记的源文件」，GLOB 的新文件要等 CMakeLists 真登记）。
#   **三条交叉校验，任何一条不成立直接 exit 2**（宁可拒绝跑，不给假绿）：
#     ① 解析结果非空且 ≥ 50 个文件；
#     ② 磁盘上每个 `*_win.cpp` 都必须在解析结果里（解析器漏读 WIN32 分支 = 红）；
#     ③ 解析结果里每个文件都必须真实存在（CMakeLists 写了不存在的文件 = 红）。
#
# ── 编译方式：-fsyntax-only ──────────────────────────────────────────
# 不链接，所以**不需要** sqlite3 / curl / openssl 的库，只需要头。头找不到就
# **跳过 + 明确登记**，绝不静默，也**绝不造假 stub**——stub 是我们自己编的声明，
# 编过了只能证明「对着我们编的声明编过了」，那是假绿，比不验更坏。机器上装了
# 第三方头就自动纳入，没装就在输出里逐条列出「未验证 + 原因」。
#
# ── 诚实边界：本机工具链 ≠ CI 的编译器 ──────────────────────────────
#   · 本机（macOS）= llvm-mingw 的 clang 前端 + libc++；CI（windows-2025 runner）
#     = MSYS2 MinGW64 的 GCC + libstdc++。
#   · 标准库不同 ⇒ 传递包含边界不同 ⇒「本机编得过」**推不出**「CI 编得过」。反例
#     就是上面那个 `std::optional`：libc++ 第 8 层传递带了 `<optional>`，libstdc++
#     不带。**「缺标准头」这一类由 scripts/check-std-includes.sh 用与标准库无关的
#     静态判据守**，本门禁不声称能覆盖它。
#   · 诊断集合不同：clang 与 GCC 的告警不完全重合，-Werror 下两边可能一边绿一边红。
#   · 结论：**本门禁通过 ≠ CI 一定绿**。它能可靠抓的是「Win32 API 拼错 / 签名不对 /
#     类型不匹配 / 本仓库 -Werror 全档告警 / 缺 include（含 libcurl 这类第三方头）」，
#     抓不到「只有 GCC 或只有 libstdc++ 才暴露的问题」。
#   · 反过来它也有独占价值：Windows CI 的 runner 里 `x86_64-w64-mingw32-g++` **就在
#     PATH 上**（MSYS2 MINGW64 的原生编译器），所以本门禁在 CI 上会**用真 GCC
#     原生跑一遍全量源文件**——那才是真正的 CI 同款检查，不是交叉近似。
#
# ── 工具链缺失时退出 0 而不是红（不要"修正"成红）────────────────────
# 故意为之：macOS runner 没装 MinGW 交叉链，别人的机器也未必装；一个「别人机器
# 上必红」的守卫会被立刻关掉。探测不到工具链 → 打印原因（含装法）→ 退出 0。
# **装了链还编不过才是红。**
#
# ── --self-test：自证非恒真 ────────────────────────────────────────
#   好树：真源码必须编过，且**必须产出非空 .o**（走 -c 单独编一个文件：
#         「没报错」与「编出了东西」是两件事，空跑一遍的守卫会以 0 退出）
#   坏树 A：注入未定义符号 → 必须红
#   坏树 B：注入未使用变量（-Werror 未用变量会失败）→ 必须红
#   坏树 C：拼错 Win32 API 名 → 必须红
#   坏树 D：解析器反向自检——若把 APPLE/POSIX 独占分支也算进受编集合 → 必须红
#   每条坏树都先 `diff` 断言**真的改了输入**（等价变异 = 零证据）。
#
# 退出码：0 = 通过（含"工具链缺失，显式跳过"）；1 = 有源码编不过；2 = 环境错
#
# 用法：bash scripts/check-windows-cross-compile.sh [--root DIR] [--self-test]
#                                       [--jobs N] [--verbose] [--eui-neo DIR]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-windows-cross-compile.sh [--root DIR] [--self-test] [--jobs N] [--verbose] [--eui-neo DIR]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0
VERBOSE=0
JOBS="${DEVDESK_XCOMPILE_JOBS:-4}"
EUI_NEO_DIR_ARG=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)     ROOT="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        --jobs)     JOBS="${2:-}"; shift 2 ;;
        --eui-neo)  EUI_NEO_DIR_ARG="${2:-}"; shift 2 ;;
        --verbose)  VERBOSE=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

find_toolchain() {
    if [ -n "${MINGW_CXX:-}" ] && command -v "$MINGW_CXX" >/dev/null 2>&1; then
        command -v "$MINGW_CXX"; return 0
    fi
    if command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        command -v x86_64-w64-mingw32-g++; return 0
    fi
    local d
    for d in "$HOME"/.local/llvm-mingw-*/bin; do
        if [ -x "$d/x86_64-w64-mingw32-g++" ]; then
            echo "$d/x86_64-w64-mingw32-g++"; return 0
        fi
    done
    return 1
}

CXX="$(find_toolchain || true)"

# 编译标志：与 cmake/CompilerWarnings.cmake 的 DevDesk 档一致（Clang/GNU 分支），
# 但只取**可移植**子集。CompilerWarnings.cmake 里那五条 -Wno-error=（array-bounds /
# restrict / stringop-overflow / alloc-size-larger-than / conversion-null）是
# **GCC 专属**名，clang 见到报 -Wunknown-warning-option、在 -Werror 下直接把构建打死；
# llvm-mingw 的 g++ 驱动是 clang，所以这里绝不能带上它们。
# -Wno-error=missing-field-initializers 两个前端都认，保留。
# 反向同理：clang 独有诊断也按前端条件追加（当前一条：unused-but-set-global，
# llvm-mingw clang 23 对 single_instance_win.cpp 刻意长持句柄的误报，2026-10-14）。
CXXFLAGS=(
    -std=c++20
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter
    -Wno-error=missing-field-initializers
    -fsyntax-only
)

# 前端探测放 CXXFLAGS 构造之后：clang 才追加它独有的 -Wno-error= 名，GCC 不发该
# 诊断、什么都不加（依赖 GCC 对未知 -Wno-* 的宽容不是干净做法）。
if [ -n "$CXX" ] && "$CXX" --version 2>/dev/null | grep -qi clang; then
    CXXFLAGS+=(-Wno-error=unused-but-set-global)
fi

# include 路径：与 CMake 里给 DevDesk / devdesk_tests 的那套一致（顶层
# target_include_directories(DevDesk PRIVATE …) + devdesk_add_eui_includes）。
# **绝不给 -I src/core**：src/core/time.h 会经 -I 优先级劫持系统 <time.h>/<ctime>
# （libc++ 内部链条实测编译爆炸）。core/CMakeLists.txt 里那条注释记的就是这件事。
build_incs() {
    INCS=(-I "${ROOT}/src" -I "${ROOT}/src/platform"
          -I "${ROOT}/src/module_home" -I "${ROOT}/src/module_markdown"
          -I "${ROOT}/src/module_todo" -I "${ROOT}/src/module_claude_proxy"
          -I "${ROOT}/src/module_settings" -I "${ROOT}/src/module_about"
          -I "${ROOT}/src/module_mcp"
          -I "${ROOT}/tests")
    if [ -n "$EUI_DIR" ]; then
        INCS+=(-I "${EUI_DIR}/include" -I "${EUI_DIR}"
               -I "${EUI_DIR}/3rd/yyjson-0.12.0/src" -I "${EUI_DIR}/3rd/md4c/src")
    fi
    [ -d "${ROOT}/3rd/cpp-httplib" ] && INCS+=(-I "${ROOT}/3rd/cpp-httplib")
    [ -n "$SQLITE_INC" ] && INCS+=(-I "$SQLITE_INC")
    [ -n "$OPENSSL_INC" ] && INCS+=(-I "$OPENSSL_INC")
    [ -n "$CURL_INC" ] && INCS+=(-I "$CURL_INC")
}

# EUI-NEO 定位（与 scripts/build-test-run.sh 同一套优先级，避免两边各认一份）：
#   显式参数 → $EUI_NEO_DIR → 同级 ../EUI-NEO → git 主工作树同级
# 第三条是 worktree 唯一能用的路径：worktree 里没有同级 ../EUI-NEO，要靠
# git-common-dir 反推主工作树（只写 `${ROOT}/../EUI-NEO` 的话，worktree 下
# 恒定找不到，EUI 相关文件会被整片记成「编不到」）。
# rev-parse --git-common-dir 的返回形态：主工作树 = 相对路径（`.git`，须锚定到
# ROOT 才能用）；链接 worktree = 绝对路径，Windows 上是 `D:/...` 盘符形式——
# 它不以 `/` 开头，判绝对路径时必须连盘符一起认，否则被误判成相对路径拼成
# `${ROOT}/D:/...`，一切无同级 EUI-NEO 的 worktree（盘符根 / .claude/worktrees/
# 下）恒定找不到 EUI-NEO，120 个 TU 被记成「编不过」的假红。同级其它脚本
# （build-test-run / coverage / package-windows）直接用原始值不判形态，没这坑。
find_eui() {
    local c common main_root
    for c in "$EUI_NEO_DIR_ARG" "${EUI_NEO_DIR:-}"; do
        if [ -n "$c" ] && [ -f "${c}/CMakeLists.txt" ]; then echo "$c"; return 0; fi
    done
    for c in "$(cd "$ROOT/.." 2>/dev/null && pwd)/EUI-NEO" "${ROOT}/../EUI-NEO"; do
        if [ -f "${c}/CMakeLists.txt" ]; then (cd "$c" && pwd); return 0; fi
    done
    common="$(git -C "$ROOT" rev-parse --git-common-dir 2>/dev/null || true)"
    if [ -n "$common" ]; then
        # /* = POSIX 绝对路径；[A-Za-z]:[/\\]* = Windows 盘符绝对路径。两者原样可用，
        # 只有相对形态（主工作树的 `.git`）才需要锚定到 ROOT。
        case "$common" in
            /*|[A-Za-z]:[/\\]*) ;;
            *) common="${ROOT}/${common}" ;;
        esac
        main_root="$(cd "$common/.." 2>/dev/null && pwd || true)"
        if [ -n "$main_root" ] && [ -f "${main_root}/../EUI-NEO/CMakeLists.txt" ]; then
            (cd "${main_root}/../EUI-NEO" && pwd); return 0
        fi
    fi
    return 1
}

# 第三方头探测：显式环境变量 → brew prefix → 常见前缀。找不到不失败，只把该库记进
# 「编不到 + 原因」清单（如实登记，不静默）。
first_existing_dir() { local d; for d in "$@"; do [ -n "$d" ] && [ -d "$d" ] && { echo "$d"; return 0; }; done; return 1; }

find_sqlite() {
    local p
    if [ -n "${DEVDESK_XCOMPILE_SQLITE_INCLUDE:-}" ] && [ -f "${DEVDESK_XCOMPILE_SQLITE_INCLUDE}/sqlite3.h" ]; then
        echo "${DEVDESK_XCOMPILE_SQLITE_INCLUDE}"; return 0
    fi
    p="$(command -v brew >/dev/null 2>&1 && brew --prefix sqlite 2>/dev/null)"
    first_existing_dir "${p:+$p/include}" /opt/homebrew/opt/sqlite/include /usr/local/opt/sqlite/include
}
find_openssl() {
    local p
    if [ -n "${DEVDESK_XCOMPILE_OPENSSL_INCLUDE:-}" ] && [ -f "${DEVDESK_XCOMPILE_OPENSSL_INCLUDE}/openssl/ssl.h" ]; then
        echo "${DEVDESK_XCOMPILE_OPENSSL_INCLUDE}"; return 0
    fi
    p="$(command -v brew >/dev/null 2>&1 && brew --prefix openssl@3 2>/dev/null)"
    first_existing_dir "${p:+$p/include}" /opt/homebrew/opt/openssl@3/include /usr/local/opt/openssl@3/include
}
find_curl() {
    local p
    if [ -n "${DEVDESK_XCOMPILE_CURL_INCLUDE:-}" ] && [ -f "${DEVDESK_XCOMPILE_CURL_INCLUDE}/curl/curl.h" ]; then
        echo "${DEVDESK_XCOMPILE_CURL_INCLUDE}"; return 0
    fi
    p="$(command -v brew >/dev/null 2>&1 && brew --prefix curl 2>/dev/null)"
    first_existing_dir "${p:+$p/include}" /opt/homebrew/opt/curl/include /usr/local/opt/curl/include
}

EUI_DIR="$(find_eui || true)"
if [ -z "$EUI_DIR" ]; then
    # EUI-NEO 是必选依赖，不适用「头找不到就跳过 + 登记」的第三方豁免：src/tests
    # 绝大多数 TU 都 include eui/*.h，缺它继续编只会把整片文件记成「编不过」，
    # 产出长得像代码回归的假红。按文件头退出码口径走 2 = 环境错，绝不装成代码错。
    echo "❌ 找不到 EUI-NEO —— 环境错（退出 2），不是源码编不过。" >&2
    echo "   探测顺序：--eui-neo 参数 → \$EUI_NEO_DIR → 同级 ../EUI-NEO → git 主工作树同级" >&2
    echo "   显式指定后重跑：bash scripts/check-windows-cross-compile.sh --eui-neo <EUI-NEO 路径>" >&2
    exit 2
fi
SQLITE_INC="$(find_sqlite || true)"
OPENSSL_INC="$(find_openssl || true)"
CURL_INC="$(find_curl || true)"

# DEVDESK_VERSION：tests/CMakeLists.txt 用 target_compile_definitions 注入它，
# tests/test_modules/test_settings.cpp 有 `#error` 断言它存在。不注入的话门禁会把
# 「门禁自己少传一个宏」误报成源码编译失败——那正是"环境错当代码错"。取值口径
# 与顶层 CMakeLists 一致：VERSION 文件优先，缺失则 0.1.0。
DEV_VERSION="0.1.0"
[ -f "${ROOT}/VERSION" ] && DEV_VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
build_incs

# MISSING_HDRS = 本机根本拿不到的 <hdr>，编译失败只因这些头时判跳过、不判失败。
# 只登记**可选**第三方（curl/sqlite/openssl，装了自动纳入）；EUI-NEO 走上面的
# exit 2，不在这里——必选依赖缺了要报环境错，不能当「跳过」混过门禁。
# 第三方再增新依赖头时要同步这里，否则对应文件会被误判成编不过。
MISSING_HDRS=""
[ -z "$CURL_INC" ] && MISSING_HDRS="${MISSING_HDRS} curl/curl.h"
[ -z "$SQLITE_INC" ] && MISSING_HDRS="${MISSING_HDRS} sqlite3.h"
[ -z "$OPENSSL_INC" ] && MISSING_HDRS="${MISSING_HDRS} openssl/opensslv.h"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 从 CMakeLists 解析 WIN32 分支下真正参与构建的源文件：awk 分支栈 + .cpp token
# 收集，栈顶为 1 表示"当前不在 WIN32 分支"。
CMAWK="$TMP/parse.awk"
cat > "$CMAWK" <<'PARSE_EOF'
# is_win <条件串> → 0 = 该分支是 Windows（0 表示"没有非 Windows 标记"）
function is_win(a) {
    if (a ~ /^[[:space:]]*NOT[[:space:]]/) return 1     # if(NOT WIN32) 是反的
    if (a ~ /APPLE|DARWIN|UNIX|LINUX|POSIX|MSVC/) return 1
    return 0
}
{ line = $0
  sub(/#.*$/, "", line)                      # 去掉行注释（本仓 CMakeLists 无 # 出现在路径里）
  nopen = 0; nclose = 0; switched = 0
  rest = line
  # ⚠ 必须连**条件一起**吃掉：`/if[ \t]*\(/` 只匹配到 "if("，于是 `if(WIN32)` 里
  #   看不到 WIN32 → 整个 WIN32 分支被当成非 Windows → 受编集合里一个 *_win.cpp
  #   都没有（靠"磁盘上每个 *_win.cpp 都必须在集合里"这条交叉校验抓到的）。
  #   备选顺序按"长优先"排：POSIX 左most-最长，elseif(APPLE) 不会被拆成 else+if(。
  while (match(rest, /elseif[ \t]*\([^)]*\)|if[ \t]*\([^)]*\)|endif[ \t]*\([^)]*\)|else/)) {
    tok = substr(rest, RSTART, RLENGTH)
    rest = substr(rest, RSTART + RLENGTH)
    if (tok ~ /^elseif/) {
      arg = tok; sub(/^elseif[ \t]*\(/, "", arg); sub(/\)[ \t]*$/, "", arg)
      if (sp > 0) stack[sp] = is_win(arg)
      switched++
    } else if (tok ~ /^if/) {
      arg = tok; sub(/^if[ \t]*\(/, "", arg); sub(/\)[ \t]*$/, "", arg)
      stack[++sp] = is_win(arg); nopen++
    } else if (tok ~ /^endif/) {
      if (sp > 0) sp--
      nclose++
    } else {                                   # else / else()
      if (sp > 0) { stack[sp] = 1; switched++ }
    }
  }
  if (nopen > 0 || nclose > 0) next           # 结构行不放 .cpp（保险）
  win = 1
  for (i = 1; i <= sp; i++) if (stack[i]) win = 0
  if (!win) next
  n = split(line, toks, /[ \t()"]+/)
  for (i = 1; i <= n; i++) {
    t = toks[i]
    if (t !~ /\.cpp$/) continue
    if (t ~ /^\$\{/) {
      if (t ~ /^\$\{CMAKE_SOURCE_DIR\}\//) t = substr(t, length("${CMAKE_SOURCE_DIR}") + 2)
      else next                              # 其它变量（EUI_NEO_DIR 等）无法静态解析
    }
    if (t == "") next
    dir = DIRNAME
    if (t ~ /^\//) dir = ""                  # 绝对路径按仓库根处理
    print (dir == "" ? t : dir "/" t)
  }
}
PARSE_EOF

derive_sources() {
    local cl d
    {
        for cl in "$ROOT"/CMakeLists.txt "$ROOT"/src/*/CMakeLists.txt "$ROOT"/tests/CMakeLists.txt; do
            [ -f "$cl" ] || continue
            # 目录必须转成**相对 ROOT** 的形式：解析结果随后按 ${ROOT}/${f} 拼路径，
            # 绝对路径会拼成 /root//abs/... 从而全被"文件不存在"过滤掉。
            d="${cl%/*}"
            if [ "$d" = "$ROOT" ]; then d=""
            else d="${d#"$ROOT"/}"; fi
            awk -v DIRNAME="$d" -f "$CMAWK" "$cl"
        done
    } | LC_ALL=C sort -u | grep -v '^$' > "$TMP/sources.txt"

    : > "$TMP/sources_ok.txt"
    local f
    while IFS= read -r f; do
        [ -f "${ROOT}/${f}" ] && echo "$f" >> "$TMP/sources_ok.txt"
    done < "$TMP/sources.txt"
}

verify_source_set() {
    local n f missing_win=""
    n="$(grep -c . "$TMP/sources_ok.txt" || true)"
    if [ "${n:-0}" -lt 50 ]; then
        echo "❌ 解析出的 Windows 源文件只有 ${n:-0} 个（期望 ≥50）—— 解析器坏了，拒绝跑" >&2
        echo "   解析产物（前 20 条）：" >&2
        head -20 "$TMP/sources_ok.txt" >&2
        return 1
    fi
    for f in $(cd "$ROOT" && find src tests -name '*_win.cpp' -type f | LC_ALL=C sort); do
        grep -qx "$f" "$TMP/sources_ok.txt" || missing_win="${missing_win} ${f}"
    done
    if [ -n "$missing_win" ]; then
        echo "❌ 这些 *_win.cpp 存在于磁盘却没进受编集合（解析器漏读 WIN32 分支）：${missing_win}" >&2
        return 1
    fi
    while IFS= read -r f; do
        [ -f "${ROOT}/${f}" ] || { echo "❌ 受编集合里的文件不存在：$f" >&2; return 1; }
    done < "$TMP/sources_ok.txt"
    TOTAL_SRC="$n"
    return 0
}

compile_one() {
    local file="$1" outdir="$2" base
    base="$(echo "$file" | tr '/' '_')"
    "$CXX" "${CXXFLAGS[@]}" "${INCS[@]}" "${ROOT}/${file}" \
        -o "${outdir}/${base}.o" >"${outdir}/${base}.log" 2>&1
}

# 该文件的失败是否**只**因为缺第三方头（是 → 跳过登记；否 → 真失败）
only_missing_thirdparty() {
    local log="$1"
    local errs
    errs="$(grep -c 'error:' "$log" 2>/dev/null || true)"
    [ "${errs:-0}" -gt 0 ] || return 1
    local e
    while IFS= read -r e; do
        local h
        h="$(printf '%s\n' "$e" | sed -n "s/.*fatal error: '\([^']*\)' file not found.*/\1/p")"
        [ -n "$h" ] || return 1
        case " $MISSING_HDRS " in
            *" $h "*) : ;;
            *) return 1 ;;
        esac
    done < <(grep 'error:' "$log")
    return 0
}

skip_reason_for() {
    local log="$1"
    grep -m1 -o "fatal error: '[^']*' file not found" "$log" 2>/dev/null \
        | sed "s/fatal error: '//; s/' file not found//"
}

if [ "$SELF_TEST" -eq 0 ]; then
    if [ -z "$CXX" ]; then
        echo "Windows 交叉编译守卫：**跳过**（本机没装 MinGW 工具链）"
        echo "  探测顺序：\$MINGW_CXX → PATH 的 x86_64-w64-mingw32-g++ → ~/.local/llvm-mingw-*/bin"
        echo "  装法（macOS）：解包 llvm-mingw 预编译包到 ~/.local/llvm-mingw-<版本>/，或 pacman -S mingw-w64-x86_64-gcc。"
        echo "  说明：macOS CI runner 同样没有这条链，所以**跳过是正确行为而非失败**。"
        exit 0
    fi

    derive_sources
    if ! verify_source_set; then exit 2; fi

    echo "Windows 源文件交叉编译守卫：${CXX}"
    "$CXX" --version | head -1 | sed 's/^/  /'
    echo "  受编集合：从 CMakeLists 的 WIN32 分支解析出 ${TOTAL_SRC} 个源文件（src + tests，含共享文件）"
    echo "  EUI-NEO：${EUI_DIR:-<未找到>}"
    echo "  ⚠ 边界：本机是 clang + libc++，CI 是 MSYS2 GCC + libstdc++。"
    echo "        标准库不同 ⇒ 传递包含边界不同 ⇒「本机编得过」推不出「CI 编得过」。"
    echo "        缺标准头那一类由 scripts/check-std-includes.sh 用与标准库无关的静态判据守。"

    mkdir -p "$TMP/obj"
    : > "$TMP/obj/status.txt"
    # 并行：每个 worker 编一个文件，状态行追加到 status.txt（O_APPEND 短写，安全）。
    # bash 数组不能 export，所以把 flags / incs 压成字符串传给 worker 再 split 回来。
    FLAGS_STR="$(
        printf '%s ' "${CXXFLAGS[@]}"
        printf -- '-DDEVDESK_VERSION="%s" ' "$DEV_VERSION"
    )"
    INCS_STR="${INCS[*]}"
    cat > "$TMP/worker.sh" <<'WORKER_EOF'
#!/usr/bin/env bash
set -u
f="$1"; outdir="$2"; root="$3"
read -r -a FLAGS <<< "${WCXXFLAGS_STR:-}"
read -r -a INCS  <<< "${WCXXINCS_STR:-}"
base="$(echo "$f" | tr '/' '_')"
if "$WCXX" "${FLAGS[@]}" "${INCS[@]}" "${root}/${f}" -o "${outdir}/${base}.o" \
       >"${outdir}/${base}.log" 2>&1; then
    printf 'OK %s\n' "$f" >> "${outdir}/status.txt"
else
    printf 'FAIL %s\n' "$f" >> "${outdir}/status.txt"
fi
WORKER_EOF
    chmod +x "$TMP/worker.sh"
    (
        export WCXX="$CXX" WCXXFLAGS_STR="$FLAGS_STR" WCXXINCS_STR="$INCS_STR" ROOT
        xargs -P "$JOBS" -I{} /bin/bash "$TMP/worker.sh" {} "$TMP/obj" "$ROOT" \
            < "$TMP/sources_ok.txt"
    )

    total=0; okc=0; failc=0; skipped=0
    : > "$TMP/failed.txt"; : > "$TMP/skipped.txt"
    while IFS= read -r line; do
        case "$line" in
            OK\ *)   total=$((total+1)); okc=$((okc+1)) ;;
            FAIL\ *)
                total=$((total+1))
                f="${line#FAIL }"
                if only_missing_thirdparty "$TMP/obj/$(echo "$f" | tr '/' '_').log"; then
                    skipped=$((skipped+1))
                    echo "${f}|缺 $(skip_reason_for "$TMP/obj/$(echo "$f" | tr '/' '_').log") 的头（本机没装该库，**未验证**）" >> "$TMP/skipped.txt"
                else
                    failc=$((failc+1))
                    echo "$f" >> "$TMP/failed.txt"
                fi
                ;;
        esac
    done < "$TMP/obj/status.txt"

    if [ "$total" -lt 1 ]; then
        echo "❌ 编译结果 0 条（total=0）—— 守卫没有实际检查任何东西，拒绝报绿" >&2
        exit 2
    fi

    echo "  ── 编过 ${okc} / 共 ${total}（含 ${skipped} 个因缺第三方头跳过）"
    if [ "$VERBOSE" -eq 1 ]; then
        grep '^OK ' "$TMP/obj/status.txt" | sed 's/^OK /     ✅ /' | head -20
    fi
    if [ "$skipped" -gt 0 ]; then
        echo "  ── 编不到（如实列出，**未验证**；装上对应的头即自动纳入）："
        while IFS= read -r s; do echo "     · ${s%%|*} —— ${s##*|}"; done < "$TMP/skipped.txt"
    fi

    if [ "$failc" -ne 0 ]; then
        echo "❌ ${failc} 个 Windows 源文件交叉编译失败："
        while IFS= read -r f; do
            echo "     · $f"
            grep -m3 'error:' "$TMP/obj/$(echo "$f" | tr '/' '_').log" 2>/dev/null | sed 's/^/         /'
        done < "$TMP/failed.txt"
        echo "   注意：这**只**说明编不过。DPAPI 密文正确性、DPAPI 解密能否还原等运行时"
        echo "   行为，交叉编译一概验不到，仍需 Windows 真机。"
        exit 1
    fi

    echo "✅ Windows CI 源文件集合（${total} 个）交叉编译全通过"
    echo "   ⚠ 但这不等于「CI 一定绿」：本机 clang+libc++ ≠ CI 的 GCC+libstdc++（见文件头）。"
    exit 0
fi

if [ "$SELF_TEST" -eq 1 ]; then
    if [ -z "$CXX" ]; then
        echo "❌ --self-test 需要真实的 MinGW 工具链（当前探测不到）" >&2
        echo "   本守卫的'红'必须由**真编译**产生；没有工具链时 --self-test 无法自证，" >&2
        echo "   退出非 0 而不是假装通过。" >&2
        exit 2
    fi
    ok=0
    echo "自测：工具链 = ${CXX}"
    "$CXX" --version | head -1 | sed 's/^/       /'

    derive_sources
    if verify_source_set; then
        echo "  ✅ 解析器：受编集合 ${TOTAL_SRC} 个文件，三条交叉校验通过（非空 / 含全部 *_win.cpp / 文件都存在）"
    else
        echo "  ❌ 解析器：交叉校验失败"
        ok=1
    fi

    SRC_REL="src/platform/credential_protection_win.cpp"

    mkdir -p "$TMP/obj"
    # 注意：这里刻意用 -c（不带 -fsyntax-only），好树要证明"真编出了东西"
    if "$CXX" -std=c++20 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
           -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter \
           -Wno-error=missing-field-initializers \
           -c "${ROOT}/${SRC_REL}" "${INCS[@]}" -o "$TMP/obj/good.o" \
           >"$TMP/obj/good.log" 2>&1; then
        if [ -s "$TMP/obj/good.o" ]; then
            echo "  ✅ 好树：绿（且产出非空 .o $(wc -c < "$TMP/obj/good.o") 字节 —— 真编了，不是没干活）"
        else
            echo "  ❌ 好树：退出码 0 但没产出 .o —— 假绿，守卫无效"; ok=1
        fi
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫本身坏了（恒红守卫比没有守卫更糟）"
        head -5 "$TMP/obj/good.log" | sed 's/^/       /'
        ok=1
    fi

    declare -a BAD_NAMES=(undef_symbol unused_var typo_winapi)
    declare -a BAD_SEDS=(
        's|    return core::Result<std::string>::ok(std::move(plain));|    return notARealSymbol_xyz;|'
        's|const unsigned char c = static_cast<unsigned char>(ch);|const unsigned char c = static_cast<unsigned char>(ch); int unused_probe_xyz = 0;|'
        's|CryptUnprotectData(|CryptUnprotectDataTypoXYZ(|'
    )
    for i in "${!BAD_NAMES[@]}"; do
        name="${BAD_NAMES[$i]}"
        sed "${BAD_SEDS[$i]}" "${ROOT}/${SRC_REL}" > "$TMP/$name.cpp"
        if diff -q "${ROOT}/${SRC_REL}" "$TMP/$name.cpp" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$name]：sed 没匹配上（变异是空操作）—— 这条自测无效"; ok=1; continue
        fi
        if "$CXX" "${CXXFLAGS[@]}" -I "${ROOT}/src" -I "${ROOT}/src/platform" \
               -c "$TMP/$name.cpp" -I "${ROOT}/src" -I "${ROOT}/src/platform" \
               -o "$TMP/$name.o" >"$TMP/$name.log" 2>&1; then
            echo "  ❌ 坏树 [$name]：本该红却绿了 —— **假绿守卫**，断言无效"; ok=1
        else
            echo "  ✅ 坏树 [$name]：红（已确认变异真的改了源码）"
        fi
    done

    # 坏树 D：解析器反向自检。正向（受编集合含全部 *_win.cpp）已在 verify_source_set
    # 里查过，这里查反向——集合里混进 *_macos/_posix/_stub/_linux 即说明解析器把
    # APPLE/POSIX 独占分支也算进来了。本条不改文件，所以不走上面的 diff 断言，
    # 断言点直接是**集合内容**（真能改变行为的切点），不是「跑一遍没报错」。
    n_before="$(grep -c . "$TMP/sources_ok.txt" || true)"
    n_apple=0
    while IFS= read -r f; do
        case "$f" in *_macos.cpp|*_posix.cpp|*_stub.cpp|*_linux.cpp) n_apple=$((n_apple+1)) ;; esac
    done < "$TMP/sources_ok.txt"
    if [ "${n_apple:-0}" -eq 0 ]; then
        echo "  ✅ 坏树 D：解析器没把 APPLE/POSIX 独占文件算进 Windows 集合（非 WIN32 分支读对了）"
    else
        echo "  ❌ 坏树 D：受编集合里混进了 ${n_apple} 个 macOS/POSIX 独占文件 —— 解析器把非 WIN32 分支也算进来了"
        ok=1
    fi
    if [ "${n_before:-0}" -ge 50 ]; then
        echo "       （受编集合 ${n_before} 个，规模下限断言同时成立）"
    else
        echo "  ❌ 受编集合只有 ${n_before:-0} 个，规模下限断言不成立"; ok=1
    fi

    if [ "$ok" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
    else
        echo "✅ --self-test 通过：解析器有效 + 好树绿且产出非空 .o + 三条坏树红 + 分支读对（非恒真）"
    fi
    exit "$ok"
fi