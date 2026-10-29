#!/usr/bin/env bash
# scripts/check-eui-neo-version.sh
# EUI-NEO 版本前置校验
#
# ── 它在守什么 ─────────────────────────────────────────────────────
# DevDesk 与 EUI-NEO 是**两个仓库**（EUI-NEO 以仓内 submodule 承载，2026-10-29 起；
# CI 经 checkout 的 submodules 参数拉取 fork HEAD，不锁死内容——gitlink 记录的是
# 推进时的基线，见 ADR 0006 与 scripts/check-eui-neo-version.sh 同名守卫的消费政策）。
# 于是框架版本不匹配以编译错误的形式出现，而**报错文件是错的**（那一行代码本身
# 没问题），排查方向被带偏。本守卫把它从「编到一半才发现、且报错指错文件」提前成
# 「构建之前就红，并明确说：EUI-NEO 版本不匹配」。
#
# ── 判据：随 DevDesk 实际需求自动推导，不存在需要人同步的清单 ─────────
# 三份输入全部来自仓库自己：
#   ① **需求侧（DevDesk 用到哪些 EUI-NEO API）** = 自动扫 src/** 下所有
#      `#include <eui/…>` / `<components/…>` / `<modules/…>` 的编译单元，
#      每新增一处消费点名单就自动变大。
#   ② **供给侧（EUI-NEO 的 include 路径）** = 从顶层 CMakeLists.txt 的
#      `devdesk_add_eui_includes()` 函数体里**解析**（不是抄一份）；EUI-NEO 加
#      新 3rd 依赖时 CMakeLists 先改、本守卫自动跟上。
#   ③ **判据本体** = 把上面两者喂给编译器，只跑 `-fsyntax-only`：DevDesk 现在写
#      的每一行代码，在克隆到的这个 EUI-NEO 上都编得过——这正是版本匹配的定义。
# ①②任一失配都 exit 2「守卫失准」，绝不静默按空名单 / 空 include 跑。
#
# 为什么不用 git rev 比对（最容易被质疑的取舍，明确记在这里）：消费政策已定案
# （ADR 0006：源码同级引入、**不锁版本、每次用最新源码**），rev 比对在本项目
# 根本不成立——
#   ① CI 上 submodule 是浅检出（主仓 fetch-depth 受限，submodule 跟随），够不到
#      EUI-NEO 完整历史，祖先关系查不了；
#   ② EUI-NEO 的历史曾被重写，指向具体 rev 的校验没有稳定锚点（旧「最低依赖
#      rev」软校验的基准 rev 在现历史中已蒸发，该检查已于 2026-10-26 删除）；
#      且 origin 即 coderstory fork、已与本地 main 同步，不存在「本地与远端不在
#      一条线上」的特殊形态，也不存在一个应该被钉住的 rev；
#   ③ 判据是「API 够不够用」不是「rev 新不新」——同一个 rev 在不同机器上可能
#      因未提交改动而不同。所以判据必须是**内容级**的。
#
# ── 以后改代码时这道门禁会怎么指路 ──────────────────────────────────
# ① DevDesk 新增一个 include 了 EUI-NEO 头的源文件 → 名单自动变大，无需改动；
# ② EUI-NEO 增删一条 include 路径 → 从 CMakeLists 自动解析跟着变，无需改动；
#    万一解析规则失配 → exit 2「守卫失准」，并打出失配的是哪一条；
# ③ 某天 DevDesk 的头依赖了**构建期生成**的头（不在 src/ 下）→ 探测报
#    `file not found: <那个头>`，点名是哪个文件缺哪个 include——**修法是给探针
#    补一条 -iquote，不是删判据**。
#
# ── 退出码 ─────────────────────────────────────────────────────────
#   0 = 克隆到的 EUI-NEO 满足 DevDesk 当前需求
#   1 = **EUI-NEO 版本不匹配**（或 include 路径缺失）——构建前就红，且指名道姓
#   2 = 环境错（编译器缺失 / include 列表解析失配 / 找不到 EUI-NEO）——**不静默绿**
#
# 用法：bash scripts/check-eui-neo-version.sh [--root DIR] [--eui-dir DIR]
#                                           [--jobs N] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-eui-neo-version.sh [--root DIR] [--eui-dir DIR] [--jobs N] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
EUI_DIR=""
SELF_TEST=0
# 探测并发默认 8（-fsyntax-only 不产目标文件，IO/内存都很轻）。不要跟着整链编译
# 封顶走 DEVDESK_JOBS=3，否则这道门禁白等十几秒。
JOBS="${DEVDESK_EUI_JOBS:-8}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)     ROOT="${2:-}"; shift 2 ;;
        --eui-dir)  EUI_DIR="${2:-}"; shift 2 ;;
        --jobs)     JOBS="${2:-}"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) usage; echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

DEVDESK_OS="linux"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) DEVDESK_OS="windows" ;;
  Darwin)               DEVDESK_OS="darwin" ;;
esac

# 编译器：缺失即 exit 2。绝不「找不到编译器就当绿」——那正是本守卫要消灭的
# 「没人发现它没跑」形态。
pick_cxx() {
    local c
    for c in "${CXX:-}" c++ g++ clang++; do
        [ -n "$c" ] || continue
        if command -v "$c" >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
    done
    return 1
}

# EUI-NEO include 路径：从顶层 CMakeLists 的 devdesk_add_eui_includes() 解析，
# **唯一来源就是 CMakeLists 本身**——不抄第二份，避免两边漂移。解析不出来 =
# 守卫失准 = exit 2（宁红勿静默绿）。
parse_eui_includes() {
    local root="$1" eui="$2"
    local raw n
    raw="$(awk '
        index($0, "devdesk_add_eui_includes") { inblk = 1; next }
        inblk && /^[ \t]*endfunction/ { inblk = 0; next }
        inblk && /\$\{EUI_NEO_DIR\}|\$\{EUI_NEO_INCLUDE_DIR\}/ {
            line = $0
            gsub(/[)]/, "", line)              # 砍掉行尾可能跟的右括号
            sub(/^[ \t]+/, "", line)
            sub(/[ \t]+$/, "", line)
            if (line != "") print line
        }
    ' "$root/CMakeLists.txt")"
    if [ -z "$raw" ]; then
        echo "环境错误：无法从 $root/CMakeLists.txt 的 devdesk_add_eui_includes() 解析 EUI-NEO include 路径" >&2
        echo "          —— 解析规则已与 CMakeLists 失配，守卫失准，不放行（请同步本脚本）" >&2
        return 2
    fi
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        case "$p" in
            '${EUI_NEO_INCLUDE_DIR}') printf '%s\n' "$eui/include" ;;
            '${EUI_NEO_DIR}'*)        printf '%s\n' "$eui${p#'${EUI_NEO_DIR}'}" ;;
            *)
                echo "环境错误：CMakeLists 里的 include 条目无法映射：$p" >&2
                echo "          —— 守卫失准，不放行（请同步本脚本的映射表）" >&2
                return 2
                ;;
        esac
    done <<< "$raw"
    n="$(awk 'END{print NR}' <<< "$raw")"
    if [ "${n:-0}" -lt 2 ]; then
        echo "环境错误：只解析出 ${n:-0} 条 EUI-NEO include 路径（至少要 include 与框架根两条）——守卫失准，不放行" >&2
        return 2
    fi
}

# 编译单元名单 = src/ 下直接 include EUI-NEO 头的文件（需求侧唯一来源）。
discover_tus() {
    local root="$1"
    grep -rlE --include='*.cpp' --include='*.h' \
        '#include[[:space:]]*[<"](eui|components|modules)/' "$root/src" 2>/dev/null \
        | LC_ALL=C sort
}

# 数非空行。用 awk 而不是 `grep -c`：grep -c 没有匹配时打印 0 但退出码是 1，于是
# `$(grep -c . f || echo 0)` 得到 "0\n0"，进 `[ "$n" -gt 0 ]` 就是
# 「integer expression expected」。
count_nonempty() { awk 'NF{c++} END{print c+0}' "$1"; }

# 平台过滤：`*_win.cpp` 只在 Windows 编、`*_macos.cpp` 只在 macOS 编。必须用
# **精确后缀**匹配，不做子串匹配——`src/module_markdown/md_sync_map.cpp` 含 "_sync" 但
# 不是同步专属文件，子串匹配会把它误杀。
filter_platform() {
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        case "$f" in
            *_win.cpp|*_win.h)
                [ "$DEVDESK_OS" = "windows" ] && printf '%s\n' "$f"
                ;;
            *_macos.cpp|*_macos.h)
                [ "$DEVDESK_OS" = "darwin" ] && printf '%s\n' "$f"
                ;;
            *) printf '%s\n' "$f" ;;
        esac
    done
}

# 探测：把名单逐个丢给编译器，只做 -fsyntax-only。每个 TU 的失败诊断原样收集——
# **报错文件是准的**（就是 DevDesk 里那一行），但结论由本守卫给：这不是你的代码
# 写错了，是框架版本不匹配。
probe_tus() {
    local eui="$1" tus_file="$2" cxx="$3"
    local flags helper total red_list
    flags="-std=c++20 -fsyntax-only -I$ROOT/src -iquote $ROOT/src"
    local d
    # DevDesk 自己的头用 -iquote 而不是 -I：-iquote 只对 "" 形式生效，不参与 <>
    # 解析。用 -I 把 src 下**每个子目录**都加进去时，src/core/time.h 会遮蔽标准库的
    # <time.h>，全部 TU 假红（"reference to unresolved using declaration"）。
    # 与 EUI-NEO 的 include 分开用 -I/-iquote，遮蔽面收缩到 src/ 根目录那几个文件。
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        flags="$flags -iquote $d"
    done <<< "$(find "$ROOT/src" -mindepth 1 -type d 2>/dev/null | LC_ALL=C sort)"
    local inc
    while IFS= read -r inc; do
        [ -n "$inc" ] || continue
        if [ ! -d "$inc" ]; then
            echo "MISSING_INCLUDE	$inc"
            continue
        fi
        flags="$flags -I$inc"
    done <<< "$EUI_INCLUDES"

    total="$(count_nonempty "$tus_file")"
    [ "$total" -gt 0 ] || { echo "EMPTY_TU_LIST"; return 2; }

    # 单文件探测写成独立脚本而不是 sh -c：避免把 flags 展开几十次撑爆命令行，也把
    # 「怎么调编译器」收敛到一个文件里，不在 xargs 的引号里玩花活。
    helper="$(mktemp -t eui-probe.XXXXXX)"
    cat > "$helper" <<'PROBE_EOF'
#!/usr/bin/env bash
out=$("$CXX" $FLAGS "$1" 2>&1)
rc=$?
if [ "$rc" -ne 0 ]; then
    printf 'RED\t%s\n' "$1"
    printf '%s\n' "$out" | grep -E 'error:|fatal error:' | head -3
fi
PROBE_EOF
    # **必须 export**，不能写成 `CXX=... FLAGS=... PROBE_OUT="$(...)"`：那种写法
    # 里没有命令词，bash 把它当三条普通赋值，CXX/FLAGS 根本没进命令替换的子进程
    # 环境 → 探测脚本里 "$CXX" 为空 → 每个 TU 秒退非零 → 全红且一条 error 都
    # 打不出来。
    export CXX="$cxx" FLAGS="$flags"
    PROBE_OUT="$(xargs -P "$JOBS" -I{} bash "$helper" {} < "$tus_file" 2>&1)"
    rm -f "$helper"

    if [ "$PROBE_OUT" = "EMPTY_TU_LIST" ]; then
        echo "环境错误：编译单元名单为空" >&2
        return 2
    fi
    local missing
    missing="$(printf '%s\n' "$PROBE_OUT" | awk '/^MISSING_INCLUDE/{c++} END{print c+0}')"
    if [ "${missing:-0}" -gt 0 ]; then
        printf '%s\n' "$PROBE_OUT" | grep '^MISSING_INCLUDE' | sed 's/^MISSING_INCLUDE\t/EUI-NEO include 路径不存在\t/'
        return 1
    fi
    red_list="$(printf '%s\n' "$PROBE_OUT" | awk '/^RED/{c++} END{print c+0}')"
    if [ "${red_list:-0}" -gt 0 ]; then
        printf '%s\n' "$PROBE_OUT"
        return 1
    fi
    return 0
}

report_mismatch_head() {
    echo "❌ EUI-NEO 版本不匹配：DevDesk 现在写的代码在克隆到的 EUI-NEO 上编不过。"
    echo "   这**不是**上方那些 DevDesk 文件写错了 —— 错的是框架版本。"
    echo "   典型形态：某个新 API（如 TextBuilder::overflow）还没落到 fork 上。"
    echo "   处置：把 EUI-NEO 推到 fork（$EUI_DIR 所在仓库）并升 submodule 指针后重跑；"
    echo "        或先等 CI 检出到含该 API 的 HEAD。判据细节见本脚本文件头。"
}

# --self-test：造一棵「旧框架」证明探针真的会红
if [ "$SELF_TEST" -eq 1 ]; then
    CXX_BIN="$(pick_cxx)" || { echo "❌ 自检失败：找不到 C++ 编译器" >&2; exit 2; }
    [ -n "$EUI_DIR" ] || EUI_DIR="${EUI_NEO_DIR:-}"
    if [ -z "$EUI_DIR" ] && [ -f "$ROOT/EUI-NEO/CMakeLists.txt" ]; then
        EUI_DIR="$ROOT/EUI-NEO"
    fi
    [ -n "$EUI_DIR" ] && [ -f "$EUI_DIR/core/dsl.h" ] || {
        echo "❌ 自检失败：定位不到真 EUI-NEO（--eui-dir / EUI_NEO_DIR / 仓库根 ./EUI-NEO）" >&2; exit 2; }

    EUI_INCLUDES="$(parse_eui_includes "$ROOT" "$EUI_DIR")" || exit 2
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT

    # 造一棵「旧框架」：把 include 路径真正用到的那几个子树整份拷出来，再按**文本
    # 锚点**删掉一个 DevDesk 正在用的 API。必须拷整棵子树、不能用符号链接——
    # 符号链接让编译器仍读到真文件，变异就成了「等价变异」：跑完是绿的，结论是错的。
    copy_headers() {
        local dst="$1" sub
        mkdir -p "$dst"
        for sub in include components core modules; do
            [ -d "$EUI_DIR/$sub" ] && cp -R "$EUI_DIR/$sub" "$dst/$sub"
        done
        mkdir -p "$dst/3rd"
        [ -d "$EUI_DIR/3rd/yyjson-0.12.0" ] && cp -R "$EUI_DIR/3rd/yyjson-0.12.0" "$dst/3rd/"
        [ -d "$EUI_DIR/3rd/md4c" ] && cp -R "$EUI_DIR/3rd/md4c" "$dst/3rd/"
    }

    discover_tus "$ROOT" | filter_platform > "$TMP/tus.txt"
    TU_COUNT="$(count_nonempty "$TMP/tus.txt")"
    if [ "$TU_COUNT" -eq 0 ]; then
        echo "❌ 自检失败：自动推导出的编译单元名单为空 —— 需求侧发现逻辑失配" >&2
        exit 1
    fi

    fail=0

    echo "  [1/3] 好树（真 EUI-NEO，$TU_COUNT 个自动推导的编译单元）……"
    if probe_tus "$EUI_DIR" "$TMP/tus.txt" "$CXX_BIN" >/dev/null 2>&1; then
        echo "  ✅ 好树：绿"
    else
        echo "  ❌ 好树：本该绿却红了 —— 守卫对当前 EUI-NEO 有误报（恒红守卫比没有守卫更糟）"
        fail=1
    fi

    # ② 旧框架：按文本锚点删掉 TextBuilder::overflow（DevDesk 正在用的 API）→ 必须红
    echo "  [2/3] 坏树（旧框架缺 TextBuilder::overflow）……"
    copy_headers "$TMP/old"
    awk '
        BEGIN { skip = 0 }
        /TextBuilder& overflow\(TextOverflow value\)/ { skip = 1; next }
        skip == 1 && $0 == "    }" { skip = 0; next }
        skip == 0 { print }
    ' "$EUI_DIR/core/dsl.h" > "$TMP/old/core/dsl.h"
    # 先断言文件真的被改了，再断言它红了——顺序反了就白忙（跑通了、绿了，但其实
    # 什么都没变）。
    if diff -q "$EUI_DIR/core/dsl.h" "$TMP/old/core/dsl.h" >/dev/null 2>&1; then
        echo "  ❌ 坏树构造失败：dsl.h 与真文件逐字节相同 —— 变异根本没生效，这条自测是假证据"
        fail=1
    else
        EUI_INCLUDES="$(parse_eui_includes "$ROOT" "$TMP/old")" || exit 2
        if probe_tus "$TMP/old" "$TMP/tus.txt" "$CXX_BIN" >/dev/null 2>&1; then
            echo "  ❌ 坏树：本该红却绿了 —— **假绿守卫**，版本不匹配发现不了"
            fail=1
        else
            echo "  ✅ 坏树：红"
        fi
    fi

    # ③ 反向证据：同样改过文件，但**只增不减**（加一个 DevDesk 没用到的成员）必须
    #    仍然绿。否则守卫会退化成「只要文件动过就报红」的刷屏器。
    echo "  [3/3] 反向证据（同样改了文件，但只新增无关 API）……"
    cp -R "$TMP/old" "$TMP/benign"
    {
        cat "$EUI_DIR/core/dsl.h"
        printf '\nnamespace eui_neo_self_test_probe { inline int guard_self_test_marker() { return 0; } }\n'
    } > "$TMP/benign/core/dsl.h"
    if diff -q "$EUI_DIR/core/dsl.h" "$TMP/benign/core/dsl.h" >/dev/null 2>&1; then
        echo "  ❌ 反向树构造失败：文件没变 —— 这条证据不成立"
        fail=1
    else
        EUI_INCLUDES="$(parse_eui_includes "$ROOT" "$TMP/benign")" || exit 2
        if probe_tus "$TMP/benign" "$TMP/tus.txt" "$CXX_BIN" >/dev/null 2>&1; then
            echo "  ✅ 反向树：绿（判据看的是「API 够不够用」，不是「文件动没动」）"
        else
            echo "  ❌ 反向树：本该绿却红了 —— 守卫是刷屏器，迟早被关掉"
            fail=1
        fi
    fi

    if [ "$fail" -ne 0 ]; then
        echo "❌ --self-test 失败：守卫不能自证有效" >&2
        exit 1
    fi
    echo "✅ --self-test 通过：好树绿 / 缺 API 的旧框架红 / 等价改动仍绿 —— 非恒真"
    exit 0
fi

echo "EUI-NEO 版本前置校验：克隆到的框架是否满足 DevDesk 当前需求"

[ -d "$ROOT" ] || { echo "❌ 环境错误：仓库根不存在：$ROOT" >&2; exit 2; }
[ -f "$ROOT/CMakeLists.txt" ] || { echo "❌ 环境错误：不可读 $ROOT/CMakeLists.txt" >&2; exit 2; }
[ -d "$ROOT/src" ] || { echo "❌ 环境错误：目录不存在：$ROOT/src" >&2; exit 2; }

# EUI-NEO 定位：--eui-dir > $EUI_NEO_DIR > 仓库根 ./EUI-NEO（gitlink submodule）
if [ -z "$EUI_DIR" ]; then EUI_DIR="${EUI_NEO_DIR:-}"; fi
if [ -z "$EUI_DIR" ] && [ -f "$ROOT/EUI-NEO/CMakeLists.txt" ]; then
    EUI_DIR="$ROOT/EUI-NEO"
fi
if [ -z "$EUI_DIR" ] || [ ! -f "$EUI_DIR/CMakeLists.txt" ]; then
    echo "❌ 环境错误：找不到 EUI-NEO（试过 --eui-dir / \$EUI_NEO_DIR / 仓库根 ./EUI-NEO submodule）" >&2
    echo "   submodule 未检出先跑：git submodule update --init EUI-NEO" >&2
    echo "   版本校验必须对真实检出到的框架跑，跳过它等于本门禁形同虚设" >&2
    exit 2
fi

CXX_BIN="$(pick_cxx)" || { echo "❌ 环境错误：找不到 C++ 编译器（试过 \$CXX / c++ / g++ / clang++）" >&2; exit 2; }

EUI_INCLUDES="$(parse_eui_includes "$ROOT" "$EUI_DIR")" || exit 2

EUI_REV="$(git -C "$EUI_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "   EUI-NEO: $EUI_DIR (rev ${EUI_REV})"
echo "   include: $(printf '%s' "$EUI_INCLUDES" | tr '\n' ' ')"

TUS="$(mktemp -t eui-tus.XXXXXX)"
trap 'rm -f "$TUS"' EXIT
discover_tus "$ROOT" | filter_platform > "$TUS"
TU_COUNT="$(count_nonempty "$TUS")"
if [ "$TU_COUNT" -eq 0 ]; then
    echo "❌ 环境错误：src/ 下没有找到任何 include EUI-NEO 头的编译单元" >&2
    echo "   —— 需求侧发现逻辑失配，守卫失准，不放行" >&2
    exit 2
fi
echo "   需求侧：src/ 下自动推导出的 ${TU_COUNT} 个 EUI-NEO 消费编译单元"

probe_out="$(probe_tus "$EUI_DIR" "$TUS" "$CXX_BIN")"
rc=$?
if [ "$rc" -eq 2 ]; then
    printf '%s\n' "$probe_out" >&2
    exit 2
fi
if [ "$rc" -ne 0 ]; then
    echo "   —— 失败明细（DevDesk 文件:编译器诊断）——"
    printf '%s\n' "$probe_out" | head -30
    report_mismatch_head
    exit 1
fi

echo "✅ EUI-NEO 版本前置校验通过（${TU_COUNT} 个 EUI-NEO 消费编译单元在 rev ${EUI_REV} 上全部编得过）"
exit 0