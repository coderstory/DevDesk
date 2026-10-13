#!/usr/bin/env bash
# scripts/check-probe-gate-labels.sh
#
# tools/quota_live_probe.cpp 是真机出网探针：真网络 + 本机 ~/.cc-switch/cc-switch.db，
# 每次实打四家供应商接口。它必须挂 ctest 标签 live，绝不能进 probe —— 默认门禁跑的是
# `ctest -L probe`，标签被改成 probe 或 probe;live 就等于一次 CI 真打四家供应商，
# 而没有任何别的机制会发现。
#
# 断言（校验对象 = tests/CMakeLists.txt 的 quota_live_probe 条目 + scripts/build-test-run.sh）：
#   A. add_test(NAME quota_live_probe 恰好 1 条：0 条 = 入口被删；≥2 条 = 有人复制了一份。
#   B. 该条目有 set_tests_properties(quota_live_probe ... LABELS ...)，且标签集合恰好 {live}。
#   C. 标签集合不含 probe。
#   D. build-test-run.sh 里不得出现 run-quota-live-probe.sh（它是常规门禁入口）。
#   E. --self-test 对构造的坏输入必须逐条报红；一条从不变绿的守卫等于没写。
#
# 退出码：0 = 通过；1 = 违规；2 = 环境错（不静默绿）
#
# 用法：bash scripts/check-probe-gate-labels.sh [--root DIR] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-probe-gate-labels.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root) ROOT="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

TESTS_CMAKE="${ROOT}/tests/CMakeLists.txt"
GATE_SCRIPT="${ROOT}/scripts/build-test-run.sh"

# 门禁守卫登记表（断言 E 用）。新增门禁守卫必须同步登记到这里，否则 E1 立刻报红——
# 那道红是设计意图不是误报：绕过登记，等于加了一道没人知道它存在、没人对它的判据负责的脚本。
REGISTERED_GUARDS=(
    "scripts/check-module-isolation.sh"        # 模块隔离：settings 不反向依赖 pet
    "scripts/check-quota-write-paths.sh"       # 余量落库唯一入口
    "scripts/check-probe-gate-labels.sh"       # 本守卫自己
    "scripts/check-probe-skip-gate.sh"         # probe 测试的 77=跳过 登记（读 configure 产物生效层）
    "scripts/check-windows-cross-compile.sh"   # Windows CI 源文件集合交叉编译（只验编译，不验运行时）
    "scripts/check-std-includes.sh"            # std 符号缺显式 include（跨标准库无关的静态判据）
    "scripts/check-quota-shutdown-order.sh"    # 余量链停机顺序（join → refresher → credentials → store）
    "scripts/check-eui-neo-version.sh"         # EUI-NEO 版本前置（跨仓库版本不匹配伪装成指错文件的编译错误）
)

if [ ! -f "$TESTS_CMAKE" ]; then
    echo "❌ 找不到 ${TESTS_CMAKE}（--root 指错？）" >&2
    exit 2
fi
if [ ! -f "$GATE_SCRIPT" ]; then
    echo "❌ 找不到 ${GATE_SCRIPT}（--root 指错？）" >&2
    exit 2
fi

# 条目块 = 从 `add_test(NAME quota_live_probe` 起、到下一个 `add_test(` 或文件尾。
# 下面用 awk 状态机划块，不要改回「往后 12 行」的固定窗口：把 set_tests_properties 挪到
# 窗口外（中间插几行注释，完全合法的 CMake 写法）会让断言 B/C 双双看不见而守卫报绿
# EXIT=0 —— 窗口宽度本身成了要人同步的隐式不变式，正是这道门禁存在的理由。
block_for() {
    awk -v name="quota_live_probe" '
        index($0, "add_test(") {
            if (inblock) { inblock = 0 }          # 上一个块到此为止
            if (index($0, "add_test(NAME " name)) { inblock = 1 }
        }
        inblock { print }
    ' "$1"
}

check() {
    local file="$1" src="$2" fail=0
    local blocks labels
    blocks="$(block_for "$file")"
    local count
    count="$(printf '%s\n' "$blocks" | grep -c 'add_test(NAME quota_live_probe' || true)"
    if [ "$count" -ne 1 ]; then
        echo "FAIL 断言 A：期望恰好 1 条 add_test(NAME quota_live_probe ...)，实到 ${count} 条"
        [ "$count" -eq 0 ] && echo "      → 入口被删 = 回到「没人知道怎么跑」的状态"
        fail=1
    fi
    labels="$(printf '%s\n' "$blocks" \
        | grep 'set_tests_properties(quota_live_probe' \
        | sed -n 's/.*LABELS *"\{0,1\}\([^")]*\)"\{0,1\}).*/\1/p' | head -1)"
    if [ -z "$labels" ]; then
        echo "FAIL 断言 B：quota_live_probe 条目没有 set_tests_properties(... LABELS ...)"
        fail=1
    else
        local norm
        norm="$(printf '%s' "$labels" | tr ';' '\n' | tr -d ' ' | grep -v '^$' | sort | tr '\n' ',')"
        if [ "$norm" != "live," ]; then
            echo "FAIL 断言 B：标签集合期望恰好 {live}，实到 {${norm%,}}"
            fail=1
        fi
        case ",${norm}" in
            *,probe,*)
                echo "FAIL 断言 C：标签含 probe —— 门禁跑的是 ctest -L probe，含它即进默认门禁 = 真打四家供应商 = CI 必红"
                fail=1
                ;;
        esac
    fi
    if grep -q 'run-quota-live-probe\.sh' "$src"; then
        echo "FAIL 断言 D：门禁脚本里出现了 run-quota-live-probe.sh —— build-test-run.sh 是常规门禁入口，不得调用真网络探针"
        fail=1
    fi
    # 断言 E：门禁里跑的每一道 check-*.sh 都必须在本守卫的登记表里。
    # 为什么要这条：每加一道门禁都要登记，是因为「加了守卫但没人知道它存在 /
    # 没人为它的判据负责」等于没加——check-probe-gate-labels.sh 的职责就是把
    # 「默认门禁里到底有哪些检查」钉成可核对的清单。少登记 → 有人绕过登记直接
    # 往门禁里塞脚本；多登记（本守卫列了但门禁没跑）→ 清单撒谎。
    #   E1 反向：门禁里出现的每个 check-*.sh 都必须在登记表里（防绕过登记）
    #   E2 正向：登记表里的每个都必须真被门禁调用（防清单撒谎 / 守卫被摘掉）
    local guard_in_gate guard_in_registry
    guard_in_gate="$(grep -oE 'scripts/check-[a-z0-9-]+\.sh' "$src" | sort -u)"
    guard_in_registry="$(printf '%s\n' "${REGISTERED_GUARDS[@]}")"
    local g
    for g in $guard_in_gate; do
        case "
${guard_in_registry}
" in
            *"
${g}
"*) : ;;
            *)
                echo "FAIL 断言 E1：门禁调用了未登记的守卫 ${g} —— 新增门禁必须同时登记到 check-probe-gate-labels.sh"
                fail=1
                ;;
        esac
    done
    for g in $guard_in_registry; do
        case "
${guard_in_gate}
" in
            *"
${g}
"*) : ;;
            *)
                echo "FAIL 断言 E2：登记表里的 ${g} 没有被门禁调用 —— 清单与实际不一致（守卫被摘掉，或登记写错）"
                fail=1
                ;;
        esac
    done
    if [ "$fail" -ne 0 ]; then
        echo "  文件: ${src}"
        return 1
    fi
    return 0
}

if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    # 跨平台原地改文件。**不要用 `sed -i ''`**（BSD 专有）：GNU sed（Windows CI
    # 的 MSYS2 就是它）会把 '' 当脚本、把真正的表达式当文件名，结果是**什么都没改**
    # 还报一行错 —— 自测据此会误判成「假绿守卫」。
    sed_i() {
        local f="$1" t; shift
        t="$(mktemp)"
        sed "$@" "$f" > "$t" && cat "$t" > "$f"
        rm -f "$t"
    }
    # 构造一棵最小假树：好树 + 三个坏树
    mkdir -p "$TMP/good/tests" "$TMP/good/scripts"
    printf '%s\n' \
        'if(UNIX)' \
        '    add_test(NAME quota_live_probe' \
        '        COMMAND bash "x/run-quota-live-probe.sh" "${CMAKE_BINARY_DIR}")' \
        '    set_tests_properties(quota_live_probe PROPERTIES LABELS "live")' \
        'endif()' > "$TMP/good/tests/CMakeLists.txt"
    # 好树的门禁脚本必须**恰好**列出登记表里那四道守卫，否则断言 E2 会报红
    # （好树自己不过 = 恒红守卫）。用 printf 从 REGISTERED_GUARDS 生成，
    # 免得以后加第五道守卫时这里忘了同步。
    # 注意：此处是**顶层**（--self-test 分支不在函数里），不能写 local ——
    # `local: can only be used in a function` 会把这一行变成报错中断。
    : > "$TMP/good/scripts/build-test-run.sh"
    for g in "${REGISTERED_GUARDS[@]}"; do
        printf 'bash %s\n' "$g" >> "$TMP/good/scripts/build-test-run.sh"
    done

    for bad in probe_probe_tag entry_missing gate_calls_probe \
               unregistered_guard guard_missing_from_gate; do
        cp -R "$TMP/good" "$TMP/$bad"
        case "$bad" in
            probe_probe_tag)
                sed_i "$TMP/$bad/tests/CMakeLists.txt" 's/LABELS "live"/LABELS "probe"/'
                ;;
            probe_probe_tag_mixed)
                :
                ;;
            entry_missing)
                sed_i "$TMP/$bad/tests/CMakeLists.txt" '/quota_live_probe/d'
                ;;
            gate_calls_probe)
                echo 'bash scripts/run-quota-live-probe.sh' >> "$TMP/$bad/scripts/build-test-run.sh"
                ;;
            # E1 坏树：门禁里塞了一道**没登记**的守卫 → 必须报红
            unregistered_guard)
                echo 'bash scripts/check-some-new-guard.sh' >> "$TMP/$bad/scripts/build-test-run.sh"
                ;;
            # E2 坏树：登记表里那道 windows 交叉编译守卫被门禁摘掉 → 必须报红
            guard_missing_from_gate)
                sed_i "$TMP/$bad/scripts/build-test-run.sh" '/check-windows-cross-compile\.sh/d'
                ;;
        esac
    done
    cp -R "$TMP/good" "$TMP/probe_probe_tag_mixed"
    sed_i "$TMP/probe_probe_tag_mixed/tests/CMakeLists.txt" 's/LABELS "live"/LABELS "probe;live"/'

    # 坏树 ⑤：标签写到 add_test 之后第 18 行 —— 必须报红。
    # 这条钉住的是**守卫自身的健壮性**：块若改回「往后 12 行」的固定窗口，
    # 一条 probe 标签挪出窗口，断言 B/C 双双看不见而守卫**报绿**——那正是
    # 「真机探针被挂进默认门禁 = 一次 CI 真打四家供应商」的形态。形态完全
    # 合法（add_test 与 set_tests_properties 之间插十几行注释没有任何问题），
    # 恰恰是最容易被「整理一下」制造出来的那种。
    # 标签值必须是 **probe** 而不是 live：标签挪远了但值仍是 live，正确答案
    # 本来就是绿 —— 用 live 造这条自测会得到一条「永远红」的自测，等于把守卫
    # 变成恒红（比没有守卫更糟）。本条的判据是「窗口外的违规必须被看见」。
    cp -R "$TMP/good" "$TMP/label_far_below"
    {
        printf '%s\n' 'if(UNIX)'
        printf '%s\n' '    add_test(NAME quota_live_probe'
        printf '%s\n' '        COMMAND bash "x/run-quota-live-probe.sh" "${CMAKE_BINARY_DIR}")'
        # 填充到 add_test 之后第 18 行才写标签（旧守卫的 12 行窗口看不见它）
        for i in $(seq 1 17); do printf '%s\n' "    # fill ${i}"; done
        printf '%s\n' '    set_tests_properties(quota_live_probe PROPERTIES LABELS "probe")'
        printf '%s\n' 'endif()'
    } > "$TMP/label_far_below/tests/CMakeLists.txt"

    ok=0
    if check "$TMP/good/tests/CMakeLists.txt" "$TMP/good/scripts/build-test-run.sh"; then
        echo "  ✅ 好树：绿"
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫本身坏了（恒红守卫比没有守卫更糟）"
        ok=1
    fi
    for bad in probe_probe_tag probe_probe_tag_mixed entry_missing gate_calls_probe \
               unregistered_guard guard_missing_from_gate label_far_below; do
        if check "$TMP/$bad/tests/CMakeLists.txt" "$TMP/$bad/scripts/build-test-run.sh" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$bad]：本该红却绿了 —— **假绿守卫**，断言无效"
            ok=1
        else
            echo "  ✅ 坏树 [$bad]：红"
        fi
    done
    exit "$ok"
fi

echo "探针门禁标签：quota_live_probe 必须挂 live 标签且不得挂 probe"
if check "$TESTS_CMAKE" "$GATE_SCRIPT"; then
    echo "  ✅ 真机探针门禁标签守卫通过（入口在 live 标签下，默认门禁看不到它）"
    exit 0
fi
exit 1