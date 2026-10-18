#!/usr/bin/env bash
# scripts/check-toolguard-braces.sh
#
# 拦截「Guard 具名对象 + 双层花括号」形态：`ToolGuard g{{"a", "b"}}`。
#
# 为什么这条形态必须红：外层初始化列表只有一个元素，该元素（{"a","b"}）向 std::string
# 列表初始化会命中 basic_string(const char*, const char*) 迭代器对构造 —— 两指针来自
# 两个**不同字面量**，相减为负 → size_t 下溢 → length_error。是否抛出取决于两条字面量
# 在 .rodata 的相对位置（链接布局翻转即红绿翻转，2750e51 修的 53 用例批量红正是它）。
# 正确写法是单层：`ToolGuard g{"a", "b"}`（两个独立元素）。
#
# 判据（文本级，只拦 Guard 具名形态）：`[A-Za-z_][A-Za-z0-9_]*Guard{{`。
# `{{"a","b"}}` 在 pair / 聚合初始化处**合法**且不带 Guard 名，不拦 —— 误伤合法形态会
# 让人直接把这道守卫关掉。只扫 tests/（守卫与被守对象同域）；大小写敏感（仓内形如
# ToolGuard / statsGuard，std::lock_guard 小写不在此列）。
#
# 退出码：0 = 通过；1 = 违规；2 = 环境错（tests/ 缺失，不静默绿）
#
# 用法：bash scripts/check-toolguard-braces.sh [--root DIR] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-toolguard-braces.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)      ROOT="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

# 可选前缀组同时覆盖 `ToolGuard{{` / `statsGuard{{` / 裸 `Guard{{`
PATTERN='(^|[^A-Za-z0-9_])([A-Za-z_][A-Za-z0-9_]*)?Guard\{\{'

# 判据本体。返回 0 = 通过；1 = 违规；2 = 环境错。$1 = 仓库/树根。
check() {
    # root 与 tests_dir 必须分两条赋值：同一条 local 的多个参数先整体展开再执行，
    # $root 在展开 tests_dir 时还没赋值（set -u 直接炸 unbound variable）
    local root="$1"
    local tests_dir="$root/tests"
    local hits
    if [ ! -d "$tests_dir" ]; then
        echo "ENV  找不到 tests 目录：${tests_dir}"
        return 2
    fi
    hits="$(LC_ALL=C grep -rnE --include='*.cpp' --include='*.h' --include='*.hpp' \
        "$PATTERN" "$tests_dir" 2>/dev/null || true)"
    if [ -z "$hits" ]; then
        return 0
    fi
    echo "FAIL 下述 Guard 具名对象用了双层花括号（外层单元素 → std::string 迭代器对构造 UB，"
    echo "     红绿随链接布局翻转；正确写法单层 {\"a\", \"b\"}）："
    printf '%s\n' "$hits"
    return 1
}

if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    # 好树：单层花括号（正确写法）+ 不带 Guard 名的双层（pair/聚合合法形态，不许误伤）
    mkdir -p "$TMP/good/tests"
    cat > "$TMP/good/tests/t.cpp" <<'EOF'
ToolGuard statsGuard{"app.stats", "app.notify"};
auto pairForm = {{"a", "b"}};
std::pair<const char*, const char*> q{{"x", "y"}};
EOF
    # 坏树 1：真实事故形态（2750e51 修的 53 处同款）
    mkdir -p "$TMP/bad1/tests"
    printf 'ToolGuard statsGuard{{"app.stats", "app.notify"}};\n' > "$TMP/bad1/tests/t.cpp"
    # 坏树 2：裸 Guard 类型名 + {{（同一 UB 形态的另一种写法）
    mkdir -p "$TMP/bad2/tests"
    printf 'Guard{{"x", "y"}};\n' > "$TMP/bad2/tests/t.cpp"

    ok=0
    if check "$TMP/good" >/dev/null 2>&1; then
        echo "  ✅ 好树：绿（单层花括号 + pair 双层均不拦）"
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫太宽（误伤 pair/聚合合法形态）"
        ok=1
    fi
    for bad in bad1 bad2; do
        if check "$TMP/$bad" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$bad]：本该红却绿了 —— **假绿守卫**，断言无效"
            ok=1
        else
            echo "  ✅ 坏树 [$bad]：红"
        fi
    done
    if [ "$ok" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
    else
        echo "✅ --self-test 通过：好树绿 / 坏树红 —— 非恒真"
    fi
    exit "$ok"
fi

echo "ToolGuard 双层花括号守卫：tests/ 下 Guard 具名对象 + {{ 即违规（string 迭代器对构造 UB）"
check "$ROOT"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  ✅ 无 Guard{{ 形态（tests/ 全扫）"
fi
exit "$rc"
