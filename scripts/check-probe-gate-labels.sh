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

# 门禁守卫登记表（断言 E 用）。**新增门禁守卫时必须同步登记到这里**，
# 否则本守卫的 E1 会立刻报红——那道红是设计意图，不是误报：
# 「加了一道门禁但没人为它的判据登记」等于没加。
# 每行注释写明这道守卫在守什么，便于下一个加门禁的人照抄格式。
REGISTERED_GUARDS=(
    "scripts/check-module-isolation.sh"        # 模块隔离：settings 不反向依赖 pet
    "scripts/check-quota-write-paths.sh"       # 余量落库唯一入口
    "scripts/check-probe-gate-labels.sh"       # 本守卫自己
    "scripts/check-windows-cross-compile.sh"   # Windows *_win.cpp 交叉编译（只验编译，不验运行时）
    "scripts/check-quota-shutdown-order.sh"    # 余量链停机顺序（join → refresher → credentials → store）
)

if [ ! -f "$TESTS_CMAKE" ]; then
    echo "❌ 找不到 ${TESTS_CMAKE}（--root 指错？）" >&2
    exit 2
fi
if [ ! -f "$GATE_SCRIPT" ]; then
    echo "❌ 找不到 ${GATE_SCRIPT}（--root 指错？）" >&2
    exit 2
fi

# 条目块 = 从 `add_test(NAME quota_live_probe` 起、到**下一个 `add_test(` 或文件尾**。
#
# ⚠ **为什么不能用固定行数窗口**（静态审计 M3 亲手构造的反证）：旧版取
# 「往后 12 行」。把 `set_tests_properties(... LABELS "live")` 挪到第 20 行
# （完全合法的 CMake 写法——中间插几行注释而已）→ 旧守卫**报绿，EXIT=0**，
# 断言 B/C 双双失效而它毫无察觉。那不是「窗口不够宽」，是「窗口的宽度本身
# 就是一条要人同步的隐式不变式」——正是本项目反复翻车的那类：守卫声称覆盖了
# 某个失败模式，实现只覆盖了其中一个子形态。
# 现在用 awk 状态机：块在下一个 `add_test(` 之前结束（或到文件尾）。
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
            # E1 坏树：门禁里塞了一道**没登记**的守卫 → 必须报红
            unregistered_guard)
                echo 'bash scripts/check-some-new-guard.sh' >> "$TMP/$bad/scripts/build-test-run.sh"
                ;;
            # E2 坏树：登记表里那道 windows 交叉编译守卫被门禁摘掉 → 必须报红
            guard_missing_from_gate)
                sed -i '' '/check-windows-cross-compile\.sh/d' "$TMP/$bad/scripts/build-test-run.sh"
                ;;
        esac
    done
    cp -R "$TMP/good" "$TMP/probe_probe_tag_mixed"
    sed -i '' 's/LABELS "live"/LABELS "probe;live"/' "$TMP/probe_probe_tag_mixed/tests/CMakeLists.txt"

    # 坏树 ⑤（静态审计 M3）：标签写到 add_test 之后第 18 行 —— 必须报红。
    # 这条钉住的是**守卫自身的健壮性**：旧版用「往后 12 行」的固定窗口，
    # 一条 probe 标签挪出窗口，断言 B/C 双双看不见而守卫**报绿**——那正是
    # 「真机探针被挂进默认门禁 = 一次 CI 真打四家供应商」的形态。形态完全
    # 合法（add_test 与 set_tests_properties 之间插十几行注释没有任何问题），
    # 恰恰是最容易被「整理一下」制造出来的那种。
    # ⚠ 标签值必须是 **probe** 而不是 live：标签挪远了但值仍是 live，正确答案
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