#!/usr/bin/env bash
# scripts/dispatch-check.sh
# 派单前守卫：即将派给 agent 的文件是否已有在飞 worktree 在写。
#
# 团队纪律「同一文件同一时刻只有一个写者」的机器闸。对全部非主树 worktree 查两类写面：
#   1. 树脏面     git status --porcelain -z 的全部路径（rename 两侧都算写面）；
#   2. 提交领先面 git diff --name-only dev...<tip>（领先 dev 的 commit 触碰的文件）。
# 任一写面与参数路径重叠 → 逐条打印 `worktree路径:分支:撞文件`，exit 1。
# 无参数 = 纯清单模式：打印每个在飞 worktree 的分支/领先落后/脏文件数。
#
# 退出码：0 = 通过 / 1 = 撞车（禁派）/ 2 = 环境错（非 git 仓、缺 dev、用法错）。
# 绝不静默绿：查不到写面（如目录缺失）只降级为空集；环境级失败一律 2，不当作 0。
#
# 全程绝对路径、不读 $PWD、脚本内不 cd；相对参数一律锚到仓库根解析，不锚调用者
# cwd。教训：isolation 守卫曾因依赖 cwd 在不同入口下必误报。
set -u

PROG="$(basename "${BASH_SOURCE[0]}")"
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || exit 2
SELF_FILE="$SELF/$PROG"
EXIT_OK=0; EXIT_VIOLATION=1; EXIT_ENV=2
TAB="$(printf '\t')"

usage() {
    echo "用法: $PROG [--root <仓库根>] [文件路径...]"
    echo "  有参数   撞车检查：参数路径与任一在飞 worktree 写面重叠 → exit 1"
    echo "  无参数   在飞清单：打印每个在飞 worktree 概况（分支/领先落后/脏文件）"
    echo "  --self-test   /tmp 临时仓自测（好树绿/坏树红），绝不碰真实仓库"
}

# 关 core.quotepath：非 ASCII 路径默认被八进制转义，后续字符串比对会整批失配
git() { command git -c core.quotepath=false "$@"; }

# porcelain 条目：worktree/HEAD/branch|detached/[bare|locked|prunable]，空行分隔；
# 第一条恒为主树（git 保证顺序）。输出行：路径<TAB>sha<TAB>分支<TAB>flags
wt_rows() { # $1=仓库根
    git -C "$1" worktree list --porcelain 2>/dev/null | awk -v OFS='\t' '
        $1 == "worktree" { if (p != "") print p, h, b, f; p = substr($0, 10); h = "-"; b = "-"; f = "-"; next }
        $1 == "HEAD"     { h = substr($0, 6); next }
        $1 == "branch"   { b = substr($0, 8); sub(/^refs\/heads\//, "", b); next }
        $1 == "detached" { b = "(detached)"; next }
        $1 == "locked"   { f = (f == "-" ? "locked" : f " locked"); next }
        $1 == "prunable" { f = (f == "-" ? "prunable" : f " prunable"); next }
        END              { if (p != "") print p, h, b, f }'
}

# 树脏面：status --porcelain -z 记录 NUL 分隔；rename = 新路径 NUL 旧路径，两侧都算
dirty_files() { # $1=worktree 路径
    [ -d "$1" ] || return 0   # 目录缺失（prunable/半删）：树脏面取不到，交提交领先面兜底
    git -C "$1" status --porcelain -z 2>/dev/null | {
        prev_rename=0
        while IFS= read -r -d '' rec; do
            if [ "$prev_rename" -eq 1 ]; then printf '%s\n' "$rec"; prev_rename=0; continue; fi
            st="${rec:0:2}"; path="${rec:3}"
            [ -n "$path" ] && printf '%s\n' "$path"
            case "$st" in *R*|*C*) prev_rename=1 ;; esac
        done
    }
    return 0
}

# 单 worktree 全部写面 = 树脏面 ∪ 提交领先面，排序去重
wt_write_faces() { # $1=仓库根 $2=worktree路径 $3=tip(sha)
    { dirty_files "$2"; git -C "$1" diff --name-only "dev...$3" 2>/dev/null; } | sort -u
}

# 撞车判定：文件 f 等于参数 p 或落在目录 p 之下
collides() { # $1=p(仓库相对) $2=f(仓库相对)
    [ "$2" = "$1" ] && return 0
    case "$2" in "$1"/*) return 0 ;; esac
    return 1
}

# 参数路径 → 仓库根相对路径；仓库外/越级(..)路径按用法错拒
normalize_one() { # $1=仓库根 $2=用户参数
    p="$2"
    p="${p%/}"
    [ -n "$p" ] || return 1
    case "$p" in
        /*)
            case "$p" in
                "$1")   return 1 ;;
                "$1"/*) p="${p#"$1"/}" ;;
                *)      return 1 ;;
            esac
            ;;
        ./*) p="${p#./}" ;;
        ..|../*) return 1 ;;
    esac
    [ -n "$p" ] || return 1
    printf '%s\n' "$p"
}

self_test() {
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/dispatch-check-selftest.XXXXXX")" || exit $EXIT_ENV
    trap 'rm -rf "$TMP"' EXIT
    fail=0; out=""; rc=0
    say() { echo "  ✅ $*"; }
    bad() { echo "  ❌ $1"; shift; printf '%s\n' "$*" | head -6 | sed 's/^/      | /'; fail=1; }
    run_case() { out="$(bash "$SELF_FILE" --root "$FIX" "$@" 2>&1)"; rc=$?; }
    rc_is() { [ "$rc" -eq "$1" ] && say "$2" || bad "$2 —— rc=${rc}（期望 $1）" "$out"; }
    has()   { printf '%s\n' "$out" | grep -q "$1" && say "$2" || bad "$2 —— 输出缺 [$1]" "$out"; }

    FIX="$TMP/repo"
    git init -q "$FIX" && git -C "$FIX" symbolic-ref HEAD refs/heads/dev || exit $EXIT_ENV
    git -C "$FIX" -c user.email=g@test -c user.name=guard commit -q --allow-empty -m init
    printf 'a\n' > "$FIX/A.txt"; printf 'b\n' > "$FIX/B.txt"
    git -C "$FIX" add A.txt B.txt
    git -C "$FIX" -c user.email=g@test -c user.name=guard commit -q -m files
    git -C "$FIX" worktree add -q -b feat/dirty "$TMP/wt-dirty" dev
    git -C "$FIX" worktree add -q -b feat/ahead "$TMP/wt-ahead" dev
    git -C "$FIX" worktree add -q -b feat/clean "$TMP/wt-clean" dev
    printf 'a-dirty\n' >> "$TMP/wt-dirty/A.txt"    # 树脏面
    printf 'b-ahead\n' >> "$TMP/wt-ahead/B.txt"    # 提交领先面
    git -C "$TMP/wt-ahead" add B.txt
    git -C "$TMP/wt-ahead" -c user.email=g@test -c user.name=guard commit -q -m ahead

    echo "dispatch-check self-test @ $TMP"

    run_case "A.txt"
    rc_is 1 "脏面撞车 → exit 1（A.txt 被 wt-dirty 脏改）"
    has ":feat/dirty:A.txt" "按 路径:分支:文件 三段式报出撞车"

    run_case "$FIX/A.txt"
    rc_is 1 "绝对路径参数归一后同样撞车 → 1"

    run_case "B.txt"
    rc_is 1 "提交领先面撞车 → 1（B.txt 在 feat/ahead 的领先提交里）"
    has "feat/ahead" "报出提交领先面所属分支"

    run_case "C-不存在.txt"
    rc_is 0 "无撞车 → 0"

    run_case
    rc_is 0 "无参数清单模式 → 0"
    has "feat/ahead" "清单列出在飞 worktree"

    run_case --root "$TMP/not-a-repo" "A.txt"
    rc_is 2 "非 git 仓 --root → 2（环境错，不静默绿）"

    if [ "$fail" -eq 0 ]; then echo "self-test: 全绿"; else echo "self-test: 有断言失败"; exit 1; fi
}

SELF_TEST=0; ROOT=""
PARAMS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --root)      [ $# -ge 2 ] || { usage; exit $EXIT_ENV; }; ROOT="$2"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help)   usage; exit $EXIT_OK ;;
        --)          shift; while [ $# -gt 0 ]; do PARAMS+=("$1"); shift; done ;;
        -*)          usage; echo "未知参数：$1" >&2; exit $EXIT_ENV ;;
        *)           PARAMS+=("$1"); shift ;;
    esac
done

if [ "$SELF_TEST" -eq 1 ]; then self_test; exit $EXIT_OK; fi

[ -n "$ROOT" ] || ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null)" || true
[ -n "$ROOT" ] || { echo "❌ 环境错：$SELF 不在 git 仓内且未给 --root" >&2; exit $EXIT_ENV; }
[ -d "$ROOT" ] || { echo "❌ 环境错：--root 不存在: $ROOT" >&2; exit $EXIT_ENV; }
# dev = 集成基线；缺 dev 则领先/落后与提交领先面全部无从算起
git -C "$ROOT" rev-parse --verify --quiet refs/heads/dev >/dev/null \
    || { echo "❌ 环境错：$ROOT 缺集成分支 dev" >&2; exit $EXIT_ENV; }

rows="$(wt_rows "$ROOT")" || true
[ -n "$rows" ] || { echo "❌ 环境错：git worktree list 无输出" >&2; exit $EXIT_ENV; }

if [ "${#PARAMS[@]}" -gt 0 ]; then
    RELS=()
    for p in "${PARAMS[@]}"; do
        r="$(normalize_one "$ROOT" "$p")" || { echo "❌ 用法错：参数路径不在仓库内或为空: $p" >&2; exit $EXIT_ENV; }
        RELS+=("$r")
    done

    hits=0; first=1
    while IFS="$TAB" read -r wtp wtsha wtbranch wtflags; do
        if [ "$first" -eq 1 ]; then first=0; continue; fi   # 首条 = 主树：主树写者即发起派单的会话
        faces="$(wt_write_faces "$ROOT" "$wtp" "$wtsha")"
        [ -n "$faces" ] || continue
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            for rel in "${RELS[@]}"; do
                if collides "$rel" "$f"; then
                    [ "$hits" -eq 0 ] && echo "撞车 —— 在飞 worktree 正在写目标文件（先收尾再派单）："
                    printf '%s:%s:%s\n' "$wtp" "$wtbranch" "$f"
                    hits=$((hits + 1))
                    break
                fi
            done
        done <<< "$faces"
    done <<< "$rows"

    if [ "$hits" -gt 0 ]; then
        echo "共 $hits 处撞车 → exit 1"
        exit $EXIT_VIOLATION
    fi
    echo "✅ 无撞车：${#PARAMS[@]} 个目标路径与全部在飞 worktree 写面无重叠"
    exit $EXIT_OK
fi

echo "在飞 worktree 清单（基线 = dev；主树除外）："
first=1; any=0
while IFS="$TAB" read -r wtp wtsha wtbranch wtflags; do
    if [ "$first" -eq 1 ]; then first=0; continue; fi
    any=1
    counts="$(git -C "$ROOT" rev-list --left-right --count "dev...$wtsha" 2>/dev/null)"
    if [ -n "$counts" ]; then behind="${counts%%$'\t'*}"; ahead="${counts#*$'\t'}"; else behind="?"; ahead="?"; fi
    if [ -d "$wtp" ]; then dirty_n="$(dirty_files "$wtp" | grep -c .)"; else dirty_n="?(目录缺失)"; fi
    printf '%s\n' "$wtp"
    printf '  分支:%s  领先dev:%s 落后dev:%s  脏文件:%s\n' "$wtbranch" "$ahead" "$behind" "$dirty_n"
    [ "$wtflags" = "-" ] || printf '  标记:%s\n' "$wtflags"
done <<< "$rows"
[ "$any" -eq 1 ] || echo "(无非主树 worktree)"
exit $EXIT_OK
