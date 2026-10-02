#!/usr/bin/env bash
# scripts/check-probe-gate-labels.sh
# 探针门禁标签守卫（G8 遗留 4 的另一半，2026-10-03 final 批）
#
# ── 它在守什么 ─────────────────────────────────────────────────────
# tools/quota_live_probe.cpp 是**真机出网**探针：真网络 + 需要用户本机
# ~/.cc-switch/cc-switch.db + 每次实打实打四家供应商接口。它进不了默认门禁
# （CI / 离线环境必红，别的机器上没有凭据）——这一点 tests/CMakeLists.txt 的注释
# 写着，但那只是**注释断言**。本会话的病根恰恰是「注释断言」冒充「代码断言」。
#
# 2026-10-03 给它补了显式入口：ctest 条目 quota_live_probe，LABELS = "live"。
# 于是「绝不进默认门禁」这条约定第一次**可以被违反而不被任何机制发现**——
# 只要有人顺手把 "live" 改成 "probe"（或写成 "probe;live"），一次 CI 跑就会
# 真打四家供应商。本守卫把那条约定钉成会红的断言。
#
# ── 断言（四条）────────────────────────────────────────────────────
#   A. tests/CMakeLists.txt 里存在一个 `add_test(NAME quota_live_probe`。
#      **条数必须恰好 1**：0 条 = 入口被删（回到「没人知道怎么跑」）；≥2 条 =
#      有人复制了一份。
#   B. 该条目 `set_tests_properties(quota_live_probe PROPERTIES LABELS ...)`
#      存在，且标签集合**恰好**是 {live}。
#   C. 标签集合**不含 probe** —— 门禁跑的是 `ctest -L probe`，含 probe 即进
#      默认门禁 = 真打网络 = CI 必红。
#   D. **门禁脚本不调用本探针**：scripts/build-test-run.sh 里不得出现
#      `run-quota-live-probe.sh`（build-test-run.sh 是常规门禁入口；它只应
#      串跑两个静态守卫 + `ctest -L probe`）。
#   E. **自证非恒真**（--self-test）：对构造的坏输入（标签改成 probe / 改成
#      "probe;live" / 条目删掉 / 门禁脚本里加上调用），必须逐条报红。
#      一条从不变绿的守卫等于没写——这是本项目反复翻车的那类假绿。
#
# 退出码：0 = 通过；1 = 违规；2 = 环境错（**不静默绿**）
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

if [ ! -f "$TESTS_CMAKE" ]; then
    echo "❌ 找不到 ${TESTS_CMAKE}（--root 指错？）" >&2
    exit 2
fi
if [ ! -f "$GATE_SCRIPT" ]; then
    echo "❌ 找不到 ${GATE_SCRIPT}（--root 指错？）" >&2
    exit 2
fi

# 条目块 = 从 `add_test(NAME quota_live_probe` 起、到该 if/endif 块结束。
# 取「往后 12 行」是够用的窄窗口：本文件里 add_test 与 set_tests_properties
# 紧邻（中间只隔一个 COMMAND 续行），12 行足够覆盖且不会串到下一个条目。
block_for() {
    awk -v name="quota_live_probe" '
        index($0, "add_test(NAME " name) { inblock = 1; n = 0 }
        inblock { print; n++ }
        inblock && n > 12 { inblock = 0 }
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
    if [ "$fail" -ne 0 ]; then
        echo "  文件: ${src}"
        return 1
    fi
    return 0
}

if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    # 构造一棵最小假树：好树 + 三个坏树
    mkdir -p "$TMP/good/tests" "$TMP/good/scripts"
    printf '%s\n' \
        'if(UNIX)' \
        '    add_test(NAME quota_live_probe' \
        '        COMMAND bash "x/run-quota-live-probe.sh" "${CMAKE_BINARY_DIR}")' \
        '    set_tests_properties(quota_live_probe PROPERTIES LABELS "live")' \
        'endif()' > "$TMP/good/tests/CMakeLists.txt"
    printf '%s\n' 'echo gate' > "$TMP/good/scripts/build-test-run.sh"

    for bad in probe_probe_tag entry_missing gate_calls_probe; do
        cp -R "$TMP/good" "$TMP/$bad"
        case "$bad" in
            probe_probe_tag)
                sed -i '' 's/LABELS "live"/LABELS "probe"/' "$TMP/$bad/tests/CMakeLists.txt"
                ;;
            probe_probe_tag_mixed)
                :
                ;;
            entry_missing)
                sed -i '' '/quota_live_probe/d' "$TMP/$bad/tests/CMakeLists.txt"
                ;;
            gate_calls_probe)
                echo 'bash scripts/run-quota-live-probe.sh' >> "$TMP/$bad/scripts/build-test-run.sh"
                ;;
        esac
    done
    cp -R "$TMP/good" "$TMP/probe_probe_tag_mixed"
    sed -i '' 's/LABELS "live"/LABELS "probe;live"/' "$TMP/probe_probe_tag_mixed/tests/CMakeLists.txt"

    ok=0
    if check "$TMP/good/tests/CMakeLists.txt" "$TMP/good/scripts/build-test-run.sh"; then
        echo "  ✅ 好树：绿"
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫本身坏了（恒红守卫比没有守卫更糟）"
        ok=1
    fi
    for bad in probe_probe_tag probe_probe_tag_mixed entry_missing gate_calls_probe; do
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