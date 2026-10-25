#!/usr/bin/env bash
# scripts/check-no-glass.sh
# 玻璃体系收尾守卫：src/**/*.{h,cpp} 不得出现 drawGlassCardLayers 或
# #include .*glass_card\.h（注释里的散文提及不算）。
#
# 退出码：0 = 通过；1 = 违规（逐条打印 file:line:命中行）；2 = 环境错（不得静默绿）
#
# 用法：bash scripts/check-no-glass.sh [--root DIR] [--self-test]
#   --root DIR  被检查的仓库根（默认 = 本脚本所属 git 仓库的根）
#   --self-test 造坏输入证明本守卫不是恒绿
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-no-glass.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF_FILE="$SELF/$(basename "${BASH_SOURCE[0]}")"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --root)
            [ $# -ge 2 ] || { usage; exit 2; }
            ROOT="$2"
            shift 2
            ;;
        --self-test)
            SELF_TEST=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "未知参数: $1" >&2
            usage
            exit 2
            ;;
    esac
done

# ── --self-test：自证非恒真 ──────────────────────────────────────────
if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT

    [ -d "$ROOT/src" ] || { echo "❌ 自检失败：$ROOT/src 不存在" >&2; exit 2; }

    mkdir -p "$TMP/good"
    cp -R "$ROOT/src" "$TMP/good/src"

    new_tree() { rm -rf "$TMP/$1"; cp -R "$TMP/good" "$TMP/$1"; }

    sed_i() {
        local f="$1" t; shift
        t="$(mktemp)"
        sed "$@" "$f" > "$t" && cat "$t" > "$f"
        rm -f "$t"
    }

    run_tree() { bash "$SELF_FILE" --root "$TMP/$1" >/dev/null 2>&1; }

    assert_red() {
        local rc
        run_tree "$1"; rc=$?
        if diff -r "$TMP/good" "$TMP/$1" >/dev/null 2>&1; then
            echo "  ❌ 坏树 [$1]：与好树逐字节相同 —— 变异没生效，这条自测是假证据（$2）"
            fail=1; return
        fi
        if [ "$rc" -eq 1 ]; then
            echo "  ✅ 坏树 [$1]：红 —— $2"
        else
            echo "  ❌ 坏树 [$1]：退出码 ${rc}（期望恰好 1 = 违规）—— $2"
            fail=1
        fi
    }
    assert_green() {
        local rc
        run_tree "$1"; rc=$?
        if diff -r "$TMP/good" "$TMP/$1" >/dev/null 2>&1; then
            echo "  ❌ 树 [$1]：与好树逐字节相同 —— 这条证据不成立（什么都没改）"
            fail=1; return
        fi
        if [ "$rc" -eq 0 ]; then
            echo "  ✅ 树 [$1]：绿 —— $2"
        else
            echo "  ❌ 树 [$1]：退出码 ${rc}（期望 0）—— $2"
            fail=1
        fi
    }

    fail=0

    # 0) 好树基线
    if run_tree good; then
        echo "  ✅ 好树（真实仓库树逐字节拷贝）：绿"
    else
        echo "  ❌ 好树（真实仓库树）就红了 —— 本守卫对当前代码不成立，先修守卫"
        bash "$SELF_FILE" --root "$TMP/good" 2>&1 | head -12
        fail=1
    fi

    # 1) 坏树：非注释行出现 drawGlassCardLayers
    new_tree bad_draw
    for f in "$TMP/bad_draw"/src/module_about/*.cpp; do
        [ -f "$f" ] || continue
        printf '%s\n' 'drawGlassCardLayers(ui,"x",0,0,100,100,6,tokens,GlassCardStyle::subtle());' >> "$f"
    done
    assert_red bad_draw "非注释行出现 drawGlassCardLayers"

    # 2) 坏树：非注释行出现 #include .*glass_card\.h
    new_tree bad_include
    for f in "$TMP/bad_include"/src/module_home/*.cpp; do
        [ -f "$f" ] || continue
        printf '%s\n' '#include "ui_kit/glass_card.h"' >> "$f"
    done
    assert_red bad_include "非注释行 #include glass_card.h"

    # 3) 反向证据：注释里提 drawGlassCardLayers 不算违规
    new_tree benign_comment
    for f in "$TMP/benign_comment"/src/module_about/*.cpp; do
        [ -f "$f" ] || continue
        printf '%s\n' '// 历史说明：旧版用 drawGlassCardLayers 绘制玻璃卡，S6.1 已移除' >> "$f"
    done
    assert_green benign_comment "注释里提 drawGlassCardLayers（散文提及不算违规）"

    if [ "$fail" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
        exit 1
    fi
    echo "✅ --self-test 通过：好树绿 / 2 棵坏树各红 / 反向树绿 —— 非恒真"
    exit 0
fi

# ── 实际检查 ──────────────────────────────────────────────────────────

SRC="$ROOT/src"
[ -d "$SRC" ] || { echo "环境错误：源目录不存在: $SRC" >&2; exit 2; }

VIOLATIONS=0

rel() {
    case "$1" in
        "$ROOT"/*) printf '%s' "${1#"$ROOT"/}" ;;
        *)         printf '%s' "$1" ;;
    esac
}

report() {
    printf '%s\n' "$1"
    VIOLATIONS=$((VIOLATIONS + 1))
}

# 在 .h/.cpp 文件中查找模式，排除纯注释行（以 // 开头，含前导空白）
# 返回命中的 "文件:行号:内容" 行
scan_pattern() {
    local pattern="$1" file line
    while IFS= read -r file; do
        [ -f "$file" ] || continue
        while IFS=: read -r lineno text; do
            # 跳过纯注释行（以可选空白 + // 开头）
            case "$text" in
                [[:space:]]*//*|//*) continue ;;
            esac
            report "$(rel "$file"):$lineno:$text"
        done < <(grep -n -- "$pattern" "$file" 2>/dev/null || true)
    done < <(find "$SRC" -type f \( -name '*.h' -o -name '*.cpp' \) 2>/dev/null)
}

# 判据 A：drawGlassCardLayers 不得出现在非注释行
scan_pattern 'drawGlassCardLayers'

# 判据 B：#include .*glass_card\.h 不得出现在非注释行
scan_pattern '#include.*glass_card\.h'

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "❌ 玻璃体系守卫失败：$VIOLATIONS 处违规（上方 file:line:命中行）" >&2
    exit 1
fi

echo "✅ 玻璃体系守卫通过（无 drawGlassCardLayers 调用 / 无 glass_card.h include）"
exit 0