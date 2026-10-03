#!/usr/bin/env bash
# scripts/check-probe-skip-gate.sh
# probe 测试「77 = 跳过」登记守卫（2026-10-03，agent-hd 收尾批）
#
# ── 它在守什么（为什么会有这道门禁）──────────────────────────────────
# tests/probes/probe_harness.h 在**无显示环境**里以 77 退出（ctest 的跳过约定），
# 而 tests/CMakeLists.txt 必须为**每一个** probe 测试登记 SKIP_RETURN_CODE 77。
# 这两件事少一件，后果是：
#   · 少登记一条 → 无显示 CI 里那条 probe 的 77 被 ctest 当成 **Failed**，
#     「诚实跳过」退化成「环境红」——正是本批要消灭的那个 14/14 假红。
#   · 登记的值与 harness 的退出码不一致（比如 harness 改 78、CMake 还写 77）→
#     跳过**永远不生效**，且没有任何东西会报——这是典型的静默失效。
#
# ── 为什么读**生成的** CTestTestfile.cmake，而不是源码里的那几行 ──────
# 「tests/CMakeLists.txt 里写了 SKIP_RETURN_CODE」是**注释级**的保证：写错值、
# 写到别的 add_test 块里、被 foreach 边界吃掉，源码看上去都对。真正决定 ctest
# 行为的是 configure 产物里 `set_tests_properties(... SKIP_RETURN_CODE "77")`
# 那几行。本守卫断言的就是它——「生效层」而不是「源码层」。
# ⚠ 边界：读的是**已 configure** 的产物。ninja 在 CMakeLists.txt 变更时会自动
#   重跑 cmake，常规门禁路径（build-test-run.sh 先 build 再跑本守卫）拿到的是新
#   产物；单独手跑本守卫、且刚改过 CMakeLists 未重新 configure 时会看到旧产物。
#
# ── 断言 ───────────────────────────────────────────────────────────
#   A. 生成文件里存在 ≥1 个挂 `LABELS "probe"` 的测试（0 条 = 门禁空转，报红）。
#   B. 每个 probe 测试的登记行都带 `SKIP_RETURN_CODE "<kSkipExitCode>"`。
#   C. harness 里能解析出 `kSkipExitCode`（判据的「应该是什么」不能丢），且
#      它与 B 用的值同源——CMake 写别的值必然报红。
#
# ── --self-test：自证非恒真 ────────────────────────────────────────
#   好树（probe 全带 77）→ 绿
#   坏树 A：某 probe 测试缺 SKIP_RETURN_CODE        → 红（本批要防的形态）
#   坏树 B：某 probe 的 SKIP_RETURN_CODE 写成 76    → 红（值与 harness 漂移）
#   坏树 C：harness 的 kSkipExitCode 改成 78        → 红（源码改了、登记没跟）
#   坏树 D：没有任何 probe 标签的测试               → 红（门禁空转也报绿 = 假绿）
#   坏树 E：**非 probe** 测试缺 SKIP_RETURN_CODE    → 绿（判据只盯 probe 标签，
#           不误伤无关测试——否则有人会为「修绿」把本守卫关掉）
#
# 退出码：0 = 通过；1 = 违规；2 = 环境错（build 目录未 configure / 文件缺失，**不静默绿**）
#
# 用法：bash scripts/check-probe-skip-gate.sh [--root DIR] [--build-dir DIR] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-probe-skip-gate.sh [--root DIR] [--build-dir DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
BUILD_DIR=""
SELF_TEST=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)      ROOT="${2:-}"; shift 2 ;;
        --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

# build-dir 相对路径按仓库根解析（门禁里传的是 "build" / "build-release"）
[ -n "$BUILD_DIR" ] || BUILD_DIR="build"
case "$BUILD_DIR" in
    /*) CTEST_FILE="$BUILD_DIR/tests/CTestTestfile.cmake" ;;
    *)  CTEST_FILE="$ROOT/$BUILD_DIR/tests/CTestTestfile.cmake" ;;
esac
HARNESS="$ROOT/tests/probes/probe_harness.h"

# 判据本体。返回 0 = 通过；1 = 违规；2 = 环境错（调用方按非零处理，不静默绿）。
check() {
    local ctest_file="$1" harness="$2" fail=0
    if [ ! -f "$ctest_file" ]; then
        echo "ENV  找不到生成的测试登记：${ctest_file}"
        echo "     （build 目录未 configure？本守卫的判据来源是 configure 产物，不从源码猜）"
        return 2
    fi
    if [ ! -f "$harness" ]; then
        echo "ENV  找不到 probe 判据头：${harness}"
        return 2
    fi

    # 判据的「应该是什么」：harness 的跳过退出码。丢了它，B 就无从比对。
    local kskip
    kskip="$(grep -oE 'kSkipExitCode *= *[0-9]+' "$harness" \
        | grep -oE '[0-9]+' | head -1 || true)"
    if [ -z "$kskip" ]; then
        echo "FAIL 断言 C：从 ${harness} 解析不到 kSkipExitCode（harness 侧判据丢了）"
        return 1
    fi

    # 每个 probe 测试的登记行（生成文件里整条在一行）
    local probe_entries count
    probe_entries="$(grep -E '^set_tests_properties\(.*LABELS "probe"' "$ctest_file" || true)"
    count="$(printf '%s\n' "$probe_entries" | grep -c 'set_tests_properties(' || true)"
    if [ "$count" -lt 1 ]; then
        echo "FAIL 断言 A：${ctest_file} 里没有任何挂 probe 标签的测试 —— 门禁跑的是 -L probe，0 条 = 空转"
        return 1
    fi

    # 断言 B：逐条核对 SKIP_RETURN_CODE 与 kskip 一致
    local line tname
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        if ! printf '%s' "$line" | grep -qE "SKIP_RETURN_CODE *\"?${kskip}\"?"; then
            tname="$(printf '%s' "$line" | sed -n 's/^set_tests_properties(\([^ ]*\) .*/\1/p')"
            echo "FAIL 断言 B：probe 测试 ${tname:-?} 未登记 SKIP_RETURN_CODE ${kskip}"
            echo "     （无显示环境会把这 77 当 Failed，诚实跳过失效；或值被写成别的数）"
            fail=1
        fi
    done <<< "$probe_entries"
    [ "$fail" -eq 0 ]
}

if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    # 跨平台原地改文件（**不要用 `sed -i ''`**：BSD 专有，GNU sed 会把 '' 当脚本）
    sed_i() {
        local f="$1" t; shift
        t="$(mktemp)"
        sed "$@" "$f" > "$t" && cat "$t" > "$f"
        rm -f "$t"
    }
    # 造一棵树：$1=目录 $2=CTestTestfile 内容 $3=harness 里的 kSkipExitCode 值
    mk_tree() {
        local dir="$1" ctest_body="$2" kskip="$3"
        mkdir -p "$dir/tests/probes"
        printf '%s\n' "$ctest_body" > "$dir/tests/CTestTestfile.cmake"
        printf 'inline constexpr int kSkipExitCode = %s;\n' "$kskip" \
            > "$dir/tests/probes/probe_harness.h"
    }
    # 好树：两个 probe（都带 77）+ 一个非 probe
    GOOD_CTEST='set_tests_properties(home_probes PROPERTIES  LABELS "probe" SKIP_RETURN_CODE "77" _BACKTRACE_TRIPLES "x;1;add_test;")
set_tests_properties(home_probes PROPERTIES  LABELS "probe" SKIP_RETURN_CODE "77" _BACKTRACE_TRIPLES "x;2;add_test;")
set_tests_properties(devdesk_tests PROPERTIES  LABELS "unit" _BACKTRACE_TRIPLES "x;3;add_test;")'
    mk_tree "$TMP/good" "$GOOD_CTEST" 77

    # 坏树 A：一个 probe 缺 SKIP_RETURN_CODE
    mk_tree "$TMP/badA" 'set_tests_properties(home_probes PROPERTIES  LABELS "probe" SKIP_RETURN_CODE "77" _BACKTRACE_TRIPLES "x;1;")
set_tests_properties(home_probes PROPERTIES  LABELS "probe" _BACKTRACE_TRIPLES "x;2;")' 77
    # 坏树 B：probe 的跳过码写成 76（与 harness 的 77 漂移）
    mk_tree "$TMP/badB" 'set_tests_properties(home_probes PROPERTIES  LABELS "probe" SKIP_RETURN_CODE "76" _BACKTRACE_TRIPLES "x;1;")' 77
    # 坏树 C：harness 的 kSkipExitCode 改成 78（源码改了、登记没跟）
    mk_tree "$TMP/badC" "$GOOD_CTEST" 78
    # 坏树 D：没有任何 probe 标签的测试（门禁空转）
    mk_tree "$TMP/badD" 'set_tests_properties(devdesk_tests PROPERTIES  LABELS "unit" _BACKTRACE_TRIPLES "x;1;")' 77
    # 坏树 E：**非 probe** 测试缺 SKIP_RETURN_CODE → 必须**绿**（判据不误伤无关测试）
    mk_tree "$TMP/badE" 'set_tests_properties(home_probes PROPERTIES  LABELS "probe" SKIP_RETURN_CODE "77" _BACKTRACE_TRIPLES "x;1;")
set_tests_properties(devdesk_tests PROPERTIES  LABELS "unit" _BACKTRACE_TRIPLES "x;2;")' 77

    ok=0
    if check "$TMP/good/tests/CTestTestfile.cmake" "$TMP/good/tests/probes/probe_harness.h"; then
        echo "  ✅ 好树：绿"
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫本身坏了（恒红守卫比没有守卫更糟）"
        ok=1
    fi
    for bad in badA badB badC badD; do
        if check "$TMP/$bad/tests/CTestTestfile.cmake" "$TMP/$bad/tests/probes/probe_harness.h" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$bad]：本该红却绿了 —— **假绿守卫**，断言无效"
            ok=1
        else
            echo "  ✅ 坏树 [$bad]：红"
        fi
    done
    if check "$TMP/badE/tests/CTestTestfile.cmake" "$TMP/badE/tests/probes/probe_harness.h" >/dev/null 2>&1; then
        echo "  ✅ 坏树 E：绿（非 probe 测试缺登记不算违规 —— 判据只盯 probe 标签）"
    else
        echo "  ❌ 坏树 E：非 probe 测试被误伤成红 —— 判据太宽，迟早被人关掉"
        ok=1
    fi
    if [ "$ok" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
    else
        echo "✅ --self-test 通过：好树绿 / 坏树 A·B·C·D 红 / 坏树 E 绿 —— 非恒真"
    fi
    exit "$ok"
fi

echo "探针跳过登记：每个 probe 测试必须在 configure 产物里登记 SKIP_RETURN_CODE = harness 的 kSkipExitCode"
check "$CTEST_FILE" "$HARNESS"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  ✅ probe 跳过登记守卫通过（$(basename "$CTEST_FILE") 里每个 probe 测试都带 77）"
fi
exit "$rc"
