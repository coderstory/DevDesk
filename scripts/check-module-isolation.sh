#!/usr/bin/env bash
# scripts/check-module-isolation.sh
# 模块隔离静态守卫：module_* 之间不得互相依赖。
#
# 本脚本全程绝对路径、不读 ${PWD}、脚本内不 cd —— **不要**改成相对路径、也不要依赖
# 调用者的 cwd。历史原因：守卫原先编在 tests/test_modules/test_settings.cpp 里靠
# __FILE__ 上溯找仓库根，而 ccache 会把 __FILE__ 改写成相对路径，cwd = 仓库根时
# 该候选退化成裸相对路径并误命中 → 报「找不到仓库根」→ 全量必红。
#
# 断言（本守卫的全部契约）：
#   A. 全模块互斥：遍历 src/module_*（动态枚举目录）两两组合，任何模块的 .h/.cpp/.mm
#      都不得 include 兄弟模块头（`"module_x/` 与 `<module_x/` 两种形态）。
#      模块名全部由目录名推导，新增/删除模块自动纳入，无需改本脚本。
#   B. src/module_settings/CMakeLists.txt 的**非注释行**不得 link 兄弟模块库
#      devdesk_module_claude_proxy（解释性注释里允许提及）。
#   C. module_agent 消费 todo/markdown 工具走 core::ToolRegistry 按名调用
#      （todo_tools.h 头注的设计契约），所以 module_agent/* 不得 include 兄弟模块
#      头；反向 todo/markdown/claude_proxy/**settings** 的工具集与模块文件
#      （todo_tools / markdown_tools / claude_tools / app_tools / todo_module /
#      markdown_module / claude_module / settings_module）不得引用 module_agent/
#      ——注册方不回依赖消费方。两侧判据都按「include 路径形态」= 带斜杠后缀匹配，
#      注释里不带斜杠的模块名不算违规。
#   D. 平台宏守卫：src/module_*/、src/ui_kit/、src/app_*.{h,cpp} 零
#      `#ifdef _WIN32` / `#if defined(_WIN32)` / `#ifdef __APPLE__` /
#      `#if defined(__APPLE__)`。平台拆分文件（*_win.cpp / *_macos.* / *_posix.cpp /
#      *_stub.cpp / *.mm / 任意 platform/ 子目录）放行——平台差异一律拆文件 + CMake
#      按平台选源，业务代码不写条件编译。
#
# 退出码：0 = 通过；1 = 违规（逐条打印 文件:行:命中行）；2 = 环境错（不得静默绿）
#
# 用法：bash scripts/check-module-isolation.sh [--root DIR] [--self-test]
#   --root DIR  被检查的仓库根（默认 = 本脚本所属 git 仓库的根，worktree 里即该
#               worktree 根）；反证测试用它指向仓库外拷贝树。
#   --self-test 造坏输入证明本守卫不是恒绿，并**用真实仓库树做底**（见下）。
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-module-isolation.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# SELF 是**脚本所在目录**（git -C "$SELF" / dirname "$SELF" 都按目录用）；--self-test
# 要**执行**本脚本，所以另存一份脚本自身的绝对路径，别把目录当命令跑。
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
# 形态：**拿真实仓库树当底 + 定点变异**。不要改成手搓最小假树——假树会与真实代码
# 形状漂移（守卫将来加一条文件级断言，假树缺文件 → 好树假红），且「假树上的判据」
# 推不出「这份代码上的判据」。
# 每条坏树先 `diff -r` 断言树真的变了，再断言它红了：顺序反了就白忙（跑完是绿的，
# 但什么都没改）。
# 坏树的退出码要求是**恰好 1**（违规），不是「任何非零」——2 是环境错，把它当成红
# 会把「守卫自己跑不起来」伪装成「守卫抓到了违规」。
if [ "$SELF_TEST" -eq 1 ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT

    [ -d "$ROOT/src" ] || { echo "❌ 自检失败：$ROOT/src 不存在" >&2; exit 2; }
    [ -f "$ROOT/CMakeLists.txt" ] || { echo "❌ 自检失败：$ROOT/CMakeLists.txt 不存在" >&2; exit 2; }

    mkdir -p "$TMP/good"
    cp -R "$ROOT/src" "$TMP/good/src"
    cp "$ROOT/CMakeLists.txt" "$TMP/good/CMakeLists.txt"

    new_tree() { rm -rf "$TMP/$1"; cp -R "$TMP/good" "$TMP/$1"; }

    # 跨平台原地改文件。**不要用 `sed -i ''`**：BSD 专有写法，GNU sed（Windows CI
    # 的 MSYS2 就是它）会把 '' 当脚本、把真正的表达式当文件名，结果什么都没改还报
    # 一行错——自测据此得出「假绿守卫」的错误结论。本守卫要进 Windows CI 的门禁链，
    # 构造动作必须两平台同形。
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

    # 0) 好树基线：**好树必须先绿，之后每棵坏树才谈得上红**。好树不绿、或顺序反了，
    #    整套变异自测就没有意义。
    if run_tree good; then
        echo "  ✅ 好树（真实仓库树逐字节拷贝）：绿"
    else
        echo "  ❌ 好树（真实仓库树）就红了 —— 本守卫对当前代码不成立，先修守卫"
        bash "$SELF_FILE" --root "$TMP/good" 2>&1 | head -12
        fail=1
    fi

    new_tree agent_include_sibling
    for f in "$TMP/agent_include_sibling"/src/module_agent/*.cpp \
             "$TMP/agent_include_sibling"/src/module_agent/*.h; do
        [ -f "$f" ] || continue
        printf '%s\n' '#include "module_todo/todo_tools.h"' >> "$f"
    done
    assert_red agent_include_sibling "module_agent include 兄弟模块头（C5）"

    new_tree tool_ref_agent
    printf '%s\n' '#include "module_agent/agent_tools.h"' >> "$TMP/tool_ref_agent/src/module_todo/todo_tools.h"
    assert_red tool_ref_agent "todo_tools 回依赖 module_agent（C6）"

    # 11) 反向证据：一棵「写法不同但合规」的树必须绿。删掉它，守卫就可能退化成
    #     刷屏器。刻意做一件「像整理、但合规」的事：settings_module.cpp 里加注释，
    #     注释里**故意提到兄弟模块裸名**（判据按 include 路径形态 = 带斜杠后缀匹配，
    #     不是按子串，裸名不误报）。
    new_tree benign_style
    printf '%s\n' '// 说明：本模块不依赖 module_todo。' \
        >> "$TMP/benign_style/src/module_settings/settings_module.cpp"
    assert_green benign_style "写法不同但合规：注释含裸模块名"

    # 12) 全模块互斥（A 段）：任一模块 include 兄弟模块头都必须红。
    #     选 wallpaper→todo 这条边：两者毫无既有耦合，命中只可能来自 A 段。
    new_tree cross_module_include
    printf '%s\n' '#include "module_todo/todo_tools.h"' \
        >> "$TMP/cross_module_include/src/module_wallpaper/wallpaper_module.cpp"
    assert_red cross_module_include "module_wallpaper include 兄弟模块头（A 全模块互斥）"

    # 13) 平台宏守卫（D 段）：业务模块里出现平台条件编译必须红。
    new_tree platform_macro_in_module
    printf '%s\n' '#ifdef _WIN32' \
        >> "$TMP/platform_macro_in_module/src/module_todo/todo_service.cpp"
    assert_red platform_macro_in_module "module_todo 内出现 #ifdef _WIN32（D 平台宏守卫）"

    # 14) D 段白名单反向证据：平台拆分目录里的同名宏必须放行
    #     （真实仓库 src/module_wallpaper/platform/mpv_controller.cpp:603 即此形态）。
    new_tree platform_macro_whitelisted
    printf '%s\n' '#if defined(_WIN32)' \
        >> "$TMP/platform_macro_whitelisted/src/module_wallpaper/platform/mpv_controller.cpp"
    assert_green platform_macro_whitelisted "platform/ 子目录内平台宏放行（D 白名单）"

    if [ "$fail" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
        exit 1
    fi
    echo "✅ --self-test 通过：好树绿 / 3 棵坏树各红 / 2 棵合规树绿 —— 非恒真"
    exit 0
fi

SRC="$ROOT/src/module_settings/settings_module.cpp"
CMK="$ROOT/src/module_settings/CMakeLists.txt"

[ -d "$ROOT" ] || { echo "环境错误：仓库根不存在: $ROOT" >&2; exit 2; }
[ -f "$SRC" ] && [ -r "$SRC" ] || { echo "环境错误：不可读 $SRC" >&2; exit 2; }
[ -f "$CMK" ] && [ -r "$CMK" ] || { echo "环境错误：不可读 $CMK" >&2; exit 2; }

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

check_absent_fixed() {
    local file="$1" pat="$2" hits line
    hits="$(grep -nF -- "$pat" "$file" || true)"
    [ -n "$hits" ] || return 0
    while IFS= read -r line; do
        report "$(rel "$file"):$line"
    done <<< "$hits"
}

check_exactly_once_fixed() {
    local file="$1" pat="$2" n
    n="$(grep -oF -- "$pat" "$file" | wc -l | tr -d ' ')"
    if [ "$n" != "1" ]; then
        report "$(rel "$file"):0:缺少/重复期望内容 '$pat'（要求恰好 1 次，实测 ${n} 次）"
    fi
}

check_at_least_once_fixed() {
    local file="$1" pat="$2"
    if ! grep -qF -- "$pat" "$file"; then
        report "$(rel "$file"):0:缺少期望调用 '$pat'（要求 ≥ 1 次，实测 0 次）"
    fi
}

# A：全模块互斥——src/module_* 两两遍历，任何模块不得 include 兄弟模块头。
# 模块清单由目录动态枚举（新增模块自动纳入，无需改脚本）；判据是 include 路径形态
# （`"module_x/` 与 `<module_x/`），注释里不带斜杠的裸模块名不算违规。
MODULE_DIRS=()
for d in "$ROOT"/src/module_*/; do
    [ -d "$d" ] || { echo "环境错误：目录不存在: $d" >&2; exit 2; }
    MODULE_DIRS+=("$(basename "$d")")
done
[ "${#MODULE_DIRS[@]}" -gt 0 ] || { echo "环境错误：$ROOT/src 下无 module_* 目录" >&2; exit 2; }

for own in "${MODULE_DIRS[@]}"; do
    own_files=()
    while IFS= read -r f; do
        own_files+=("$f")
    done < <(find "$ROOT/src/$own" -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) 2>/dev/null)
    for other in "${MODULE_DIRS[@]}"; do
        [ "$own" = "$other" ] && continue
        for f in "${own_files[@]+"${own_files[@]}"}"; do
            hits="$(grep -nF -e "\"$other/" -e "<$other/" "$f" || true)"
            [ -n "$hits" ] || continue
            while IFS= read -r line; do
                report "$(rel "$f"):$line"
            done <<< "$hits"
        done
    done
done

# B：settings CMakeLists.txt 的**非注释行**不得 link 兄弟模块库（注释行允许提及）
CMK_HITS="$(awk -v f="$(rel "$CMK")" '
    /^[ \t]*#/ { next }                       # 注释行允许提及目标名
    /devdesk_module_claude_proxy/ {
        printf "%s:%d:%s\n", f, FNR, $0
    }
' "$CMK" || true)"
if [ -n "$CMK_HITS" ]; then
    while IFS= read -r line; do
        report "$line"
    done <<< "$CMK_HITS"
fi

# C：module_agent 隔离——兄弟模块头零 include + 工具方不回依赖注册方
AGENT_DIR="$ROOT/src/module_agent"
[ -d "$AGENT_DIR" ] || { echo "环境错误：目录不存在: $AGENT_DIR" >&2; exit 2; }
# agent 源文件不得 include 兄弟模块头（include 路径形态 = 带斜杠后缀）
for agent_file in "$AGENT_DIR"/*.h "$AGENT_DIR"/*.cpp; do
    [ -f "$agent_file" ] || { echo "环境错误：module_agent 源文件缺失: $agent_file" >&2; exit 2; }
    for sibling in module_home module_markdown module_todo \
                   module_claude_proxy module_settings; do
        check_absent_fixed "$agent_file" "${sibling}/"
    done
done
# 反向：工具集/模块注册文件不得引用 module_agent 头（同款带斜杠形态）
for tool_file in "$ROOT/src/module_todo/todo_tools.h" \
                 "$ROOT/src/module_todo/todo_tools.cpp" \
                 "$ROOT/src/module_todo/todo_module.cpp" \
                 "$ROOT/src/module_markdown/markdown_tools.h" \
                 "$ROOT/src/module_markdown/markdown_tools.cpp" \
                 "$ROOT/src/module_markdown/markdown_module.cpp" \
                 "$ROOT/src/module_claude_proxy/claude_tools.h" \
                 "$ROOT/src/module_claude_proxy/claude_tools.cpp" \
                 "$ROOT/src/module_claude_proxy/claude_module.cpp" \
                 "$ROOT/src/module_settings/app_tools.h" \
                 "$ROOT/src/module_settings/app_tools.cpp" \
                 "$ROOT/src/module_settings/settings_module.cpp"; do
    [ -f "$tool_file" ] && [ -r "$tool_file" ] || {
        echo "环境错误：不可读 $tool_file" >&2; exit 2; }
    check_absent_fixed "$tool_file" 'module_agent/'
done

# D：平台宏守卫——业务代码零平台条件编译（平台差异一律拆文件 + CMake 按平台选源）。
# 平台拆分文件放行：*_win.cpp / *_macos.* / *_posix.cpp / *_stub.cpp / *.mm /
# 任意 platform/ 子目录（如 src/module_wallpaper/platform/* 全部是平台拆分实现）。
PLATFORM_MACRO_RE='^[[:space:]]*#[[:space:]]*(ifdef[[:space:]]+(_WIN32|__APPLE__)|if[[:space:]]+defined[[:space:]]*\(?[[:space:]]*(_WIN32|__APPLE__))'
is_platform_file() {
    case "$1" in
        */platform/*) return 0 ;;
    esac
    case "${1##*/}" in
        *_win.cpp|*_macos.*|*_posix.cpp|*_stub.cpp|*.mm) return 0 ;;
    esac
    return 1
}
PLATFORM_SCAN_PATHS=()
while IFS= read -r d; do
    [ -d "$d" ] && PLATFORM_SCAN_PATHS+=("$d")
done < <(find "$ROOT/src" -maxdepth 1 -type d -name 'module_*')
[ -d "$ROOT/src/ui_kit" ] && PLATFORM_SCAN_PATHS+=("$ROOT/src/ui_kit")
for f in "$ROOT"/src/app_*.h "$ROOT"/src/app_*.cpp; do
    [ -f "$f" ] && PLATFORM_SCAN_PATHS+=("$f")
done

while IFS= read -r f; do
    is_platform_file "$f" && continue
    hits="$(grep -nE "$PLATFORM_MACRO_RE" "$f" || true)"
    [ -n "$hits" ] || continue
    while IFS= read -r line; do
        report "$(rel "$f"):$line"
    done <<< "$hits"
done < <(find "${PLATFORM_SCAN_PATHS[@]+"${PLATFORM_SCAN_PATHS[@]}"}" -type f \
             \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) 2>/dev/null)

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "❌ 模块隔离守卫失败：$VIOLATIONS 处违规（上方 文件:行:命中行）" >&2
    exit 1
fi

echo "✅ 模块隔离守卫通过（module_* 全模块互斥；settings CMakeLists 未 link 兄弟模块；module_agent 零兄弟 include 且工具方不回依赖；业务代码零平台宏）"
exit 0
