#!/usr/bin/env bash
# scripts/check-module-isolation.sh
# 模块隔离静态守卫（波 1 TIN-01）——settings 侧「不依赖兄弟模块」的契约检查。
#
# 背景：这条守卫原先编在 tests/test_modules/test_settings.cpp 里
#（REGISTER_TEST settings_module_isolation_guard），靠 __FILE__ 上溯找仓库根。
# ccache 把 __FILE__ 改写成相对路径后，cwd = 仓库根 时该候选退化成裸相对路径
# 并误命中 → 返回空 root 报「找不到仓库根」→ 全量必红（机制见波 1 spec §L10）。
# 脚本版不依赖 __FILE__、不依赖 $PWD（全程绝对路径、脚本内不 cd）。
#
# 断言（与删除前的 C++ 守卫逐条等价）：
#   A. src/module_settings/settings_module.cpp
#      1) 不得出现 module_pet/、PetConfigStore、petConfigFile、namespace module::pet
#      2) 必含 #include "module_pet_bridge.h" 恰好 1 次（桥声明唯一来源）
#      3) ::module::pet::{discoverSkins,applyPetSkin,resetPetPosition,currentSkinName,closePetWindow}
#         各 ≥ 1 次（解耦不是删功能：桥函数都要真被调用）
#   B. src/module_settings/CMakeLists.txt 的非注释行
#      4) 不得出现 devdesk_module_pet / devdesk_module_claude_proxy
#        （解释性注释里允许提及，故按行过滤首非空白字符为 '#' 的行）
#
# 退出码：0 = 通过；1 = 违规（逐条打印 文件:行:命中行）；2 = 环境错（不得静默绿）
#
# 用法：bash scripts/check-module-isolation.sh [--root DIR]
#   --root DIR  指定被检查的仓库根（默认自动定位：本脚本所属 git 仓库的根，
#               worktree 里即该 worktree 根），反证测试用它指向仓库外拷贝树。
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-module-isolation.sh [--root DIR]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"

while [ $# -gt 0 ]; do
    case "$1" in
        --root)
            [ $# -ge 2 ] || { usage; exit 2; }
            ROOT="$2"
            shift 2
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

SRC="$ROOT/src/module_settings/settings_module.cpp"
CMK="$ROOT/src/module_settings/CMakeLists.txt"

# ---- 环境检查：缺东西一律 exit 2，绝不静默绿 ----
[ -d "$ROOT" ] || { echo "环境错误：仓库根不存在: $ROOT" >&2; exit 2; }
[ -f "$SRC" ] && [ -r "$SRC" ] || { echo "环境错误：不可读 $SRC" >&2; exit 2; }
[ -f "$CMK" ] && [ -r "$CMK" ] || { echo "环境错误：不可读 $CMK" >&2; exit 2; }

VIOLATIONS=0

# 绝对路径 → 仓库相对路径（打印用）；不在 ROOT 下则原样输出
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

# 命中即违规：grep -nF 输出形如「行号:命中行」，拼成 文件:行:命中行
check_absent_fixed() {
    local file="$1" pat="$2" hits line
    hits="$(grep -nF -- "$pat" "$file" || true)"
    [ -n "$hits" ] || return 0
    while IFS= read -r line; do
        report "$(rel "$file"):$line"
    done <<< "$hits"
}

# 期望恰好一次
check_exactly_once_fixed() {
    local file="$1" pat="$2" n
    n="$(grep -oF -- "$pat" "$file" | wc -l | tr -d ' ')"
    if [ "$n" != "1" ]; then
        report "$(rel "$file"):0:缺少/重复期望内容 '$pat'（要求恰好 1 次，实测 ${n} 次）"
    fi
}

# 期望至少一次
check_at_least_once_fixed() {
    local file="$1" pat="$2"
    if ! grep -qF -- "$pat" "$file"; then
        report "$(rel "$file"):0:缺少期望调用 '$pat'（要求 ≥ 1 次，实测 0 次）"
    fi
}

# ---- A1. settings_module.cpp：pet 头零 include、盘 store 零触碰、无内联声明块 ----
check_absent_fixed "$SRC" 'module_pet/'
check_absent_fixed "$SRC" 'PetConfigStore'
check_absent_fixed "$SRC" 'petConfigFile'
check_absent_fixed "$SRC" 'namespace module::pet'

# ---- A2. 正向：桥声明唯一来源 = src 根中立头，恰好 include 一次 ----
check_exactly_once_fixed "$SRC" '#include "module_pet_bridge.h"'

# ---- A3. 正向：桥函数都真在调用点被用到（`::module::pet::X` 调用形）----
# closePetWindow（PET-CLOSE，模块开关取消勾选 → 立即关窗）同样要求真被调用：
# 解耦不是删功能
for fn in discoverSkins applyPetSkin resetPetPosition currentSkinName closePetWindow; do
    check_at_least_once_fixed "$SRC" "::module::pet::${fn}"
done

# ---- B. settings CMakeLists.txt：非注释行不得再 link 兄弟模块库 ----
CMK_HITS="$(awk -v f="$(rel "$CMK")" '
    /^[ \t]*#/ { next }                       # 注释行允许提及目标名
    /devdesk_module_pet|devdesk_module_claude_proxy/ {
        printf "%s:%d:%s\n", f, FNR, $0
    }
' "$CMK" || true)"
if [ -n "$CMK_HITS" ]; then
    while IFS= read -r line; do
        report "$line"
    done <<< "$CMK_HITS"
fi

# ---- 结论 ----
if [ "$VIOLATIONS" -gt 0 ]; then
    echo "❌ 模块隔离守卫失败：$VIOLATIONS 处违规（上方 文件:行:命中行）" >&2
    exit 1
fi

echo "✅ 模块隔离守卫通过（settings_module.cpp 无 pet 依赖 / 桥调用齐全；CMakeLists 未 link 兄弟模块）"
exit 0
