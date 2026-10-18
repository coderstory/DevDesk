#!/usr/bin/env bash
# scripts/worktree-audit.sh
# worktree 审计 + 安全清理闸。默认只读审计；清理必须显式 --clean 且一次只清一个。
#
# 每 worktree 输出：路径 / 分支 / tip sha / 创建基线 base / 自 base 提交数 /
# 脏文件数 / 是否已并入 dev / 判活判定。
# base 从分支 reflog 的 `branch: Created from` 读，读不到标 unknown。
#
# 判活判定式：在飞 = (分支 tip 未并入 dev) || (树脏)；两者皆无 = 可清。
# 宁可把可清误报成在飞，绝不把在飞判成可清 —— 2026-10 真实事故：三条在飞
# worktree 被误清只能重做。故：主树永不清理；locked（在飞 agent 持有）不清理；
# 有未并入提交或树脏一律拒绝。
#
# 半删恢复：git worktree remove 失败（目录半删/元数据悬挂）→ 先
#   `git worktree repair <路径>` 修复后重试；确认目录已废弃则 `git worktree prune`。
#
# 退出码：0 = 审计完成 / 清理成功；1 = 违规（在飞拒清、主树、locked、remove 失败）；
#         2 = 环境错（非 git 仓、缺 dev、非登记 worktree、用法错）。
set -u

PROG="$(basename "${BASH_SOURCE[0]}")"
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)" || exit 2
SELF_FILE="$SELF/$PROG"
EXIT_OK=0; EXIT_VIOLATION=1; EXIT_ENV=2
TAB="$(printf '\t')"

usage() {
    echo "用法: $PROG [--root <仓库根>] [--clean <worktree路径>]"
    echo "  默认     只读审计全部 worktree（路径/分支/tip/base/脏文件/并入dev/判活）"
    echo "  --clean  清理一个 worktree；判活（未并入提交或树脏）→ 拒绝。一次只许一个"
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
    [ -d "$1" ] || return 0   # 目录缺失：取不到，判活交由「目录缺失」分支
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

# base = 分支 reflog 里最早的 `branch: Created from`；读不到（reflog 过期/被抹）输出空
branch_base() { # $1=仓库根 $2=分支短名
    [ "$2" = "(detached)" ] && return 0
    git -C "$1" reflog show "refs/heads/$2" --format='%H %gs' 2>/dev/null \
        | awk '/branch: Created from/ { print $1; exit }'
    return 0
}

audit_mode() {
    rows="$(wt_rows "$ROOT")" || true
    [ -n "$rows" ] || { echo "❌ 环境错：git worktree list 无输出" >&2; exit $EXIT_ENV; }
    echo "worktree 审计（判活式：在飞 = tip 未并入 dev || 树脏；基线 dev @ $(git -C "$ROOT" rev-parse --short dev)）"
    first=1; cleanable=0
    while IFS="$TAB" read -r wtp wtsha wtbranch wtflags; do
        if [ "$first" -eq 1 ]; then
            first=0
            printf '主树 %s（分支:%s @ %s）—— 主会话工作区，永不清理\n' \
                "$wtp" "$wtbranch" "$(git -C "$ROOT" rev-parse --short "$wtsha" 2>/dev/null)"
            continue
        fi
        short="$(git -C "$ROOT" rev-parse --short "$wtsha" 2>/dev/null)"
        merged=是
        git -C "$ROOT" merge-base --is-ancestor "$wtsha" dev 2>/dev/null || merged=否
        if [ "$wtbranch" = "(detached)" ]; then
            base_disp="unknown(detached 无分支 reflog)"; n_base="-"
        else
            base="$(branch_base "$ROOT" "$wtbranch")"
            if [ -n "$base" ]; then
                n_base="$(git -C "$ROOT" rev-list --count "$base..$wtbranch" 2>/dev/null)"
                base_disp="$base"
            else
                n_base="-"; base_disp="unknown(reflog 无 Created from)"
            fi
        fi
        if [ -d "$wtp" ]; then dirty_n="$(dirty_files "$wtp" | grep -c .)"; else dirty_n=""; fi
        # 判活判定式（与 --clean 闸同源）：
        if [ "$merged" = "否" ]; then
            alive="在飞(tip 未并入 dev)"
        elif [ -z "$dirty_n" ]; then
            alive="目录缺失(半删? 恢复: git worktree repair <路径>；确认废弃: git worktree prune)"
        elif [ "$dirty_n" -gt 0 ]; then
            alive="在飞(树脏 $dirty_n 文件)"
        else
            alive="可清"; cleanable=$((cleanable + 1))
        fi
        printf '%s\n' "$wtp"
        printf '  分支:%s  tip:%s  base:%s  自base:%s个提交  脏文件:%s  已并入dev:%s\n' \
            "$wtbranch" "$short" "$base_disp" "$n_base" "${dirty_n:-?}" "$merged"
        [ "$wtflags" = "-" ] || printf '  标记:%s\n' "$wtflags"
        printf '  判定:%s\n' "$alive"
    done <<< "$rows"
    echo "—— 可清 $cleanable 个。清理必须显式 --clean <路径>，一次只清一个。"
}

clean_mode() { # $1 = 目标路径
    t="${1%/}"
    t_canon="$(readlink -f "$t" 2>/dev/null)"; [ -n "$t_canon" ] && t="$t_canon"
    [ -n "$t" ] || { usage; exit $EXIT_ENV; }
    rows="$(wt_rows "$ROOT")" || true
    [ -n "$rows" ] || { echo "❌ 环境错：git worktree list 无输出" >&2; exit $EXIT_ENV; }
    main_path=""; m_branch=""; m_sha=""; m_flags="-"; found=0; first=1
    while IFS="$TAB" read -r wtp wtsha wtbranch wtflags; do
        if [ "$first" -eq 1 ]; then main_path="$wtp"; first=0; fi
        if [ "$wtp" = "$t" ]; then m_branch="$wtbranch"; m_sha="$wtsha"; m_flags="$wtflags"; found=1; fi
    done <<< "$rows"
    [ "$found" -eq 1 ] || { echo "❌ 环境错：$t 不是已登记的 worktree（git worktree list 无此条）" >&2; exit $EXIT_ENV; }
    if [ "$t" = "$main_path" ]; then
        echo "❌ 拒绝：$t 是主树（主会话工作区），永不清理"; exit $EXIT_VIOLATION
    fi
    case "$m_flags" in
        *locked*) echo "❌ 拒绝：worktree 处于 locked（在飞 agent 持有）。先确认归属并解锁，本脚本绝不代解锁"; exit $EXIT_VIOLATION ;;
    esac
    # 判活判定式：有未并入 dev 的自有提交 → 拒
    if ! git -C "$ROOT" merge-base --is-ancestor "$m_sha" dev 2>/dev/null; then
        n_ahead="$(git -C "$ROOT" rev-list --count "dev..$m_sha" 2>/dev/null)"
        echo "❌ 拒绝：分支 $m_branch 有未并入 dev 的自有提交（${n_ahead:-?} 个，tip $(git -C "$ROOT" rev-parse --short "$m_sha" 2>/dev/null)）——先合并或显式废弃"
        exit $EXIT_VIOLATION
    fi
    # 树脏 → 拒，提示先处理
    if [ -d "$t" ]; then
        dirty_n="$(dirty_files "$t" | grep -c .)"
        if [ "$dirty_n" -gt 0 ]; then
            echo "❌ 拒绝：树脏（$dirty_n 个文件）——先 commit 或丢弃后再清理"; exit $EXIT_VIOLATION
        fi
    fi
    if ! git -C "$ROOT" worktree remove "$t"; then
        echo "❌ git worktree remove 失败（疑似半删）。恢复路径：git worktree repair $t 修复后重试；确认目录已废弃则 git worktree prune" >&2
        exit $EXIT_VIOLATION
    fi
    echo "✅ 已清理 $t"
    case "$m_branch" in
        "(detached)") ;;
        *) echo "（分支 $m_branch 未删；确认不要：git branch -d ${m_branch}）" ;;
    esac
    exit $EXIT_OK
}

self_test() {
    TMP="$(mktemp -d "${TMPDIR:-/tmp}/worktree-audit-selftest.XXXXXX")" || exit $EXIT_ENV
    trap 'rm -rf "$TMP"' EXIT
    fail=0; out=""; rc=0
    say() { echo "  ✅ $*"; }
    bad() { echo "  ❌ $1"; printf '%s\n' "$out" | head -8 | sed 's/^/      | /'; fail=1; }
    run_case() { out="$(bash "$SELF_FILE" --root "$FIX" "$@" 2>&1)"; rc=$?; }
    rc_is() { [ "$rc" -eq "$1" ] && say "$2" || bad "$2 —— rc=${rc}（期望 $1）"; }
    has()   { printf '%s\n' "$out" | grep -q "$1" && say "$2" || bad "$2 —— 输出缺 [$1]"; }

    FIX="$TMP/repo"
    git init -q "$FIX" && git -C "$FIX" symbolic-ref HEAD refs/heads/dev || exit $EXIT_ENV
    git -C "$FIX" -c user.email=g@test -c user.name=guard commit -q --allow-empty -m init
    printf 'a\n' > "$FIX/A.txt"
    git -C "$FIX" add A.txt
    git -C "$FIX" -c user.email=g@test -c user.name=guard commit -q -m files
    git -C "$FIX" worktree add -q -b feat/dirty  "$TMP/wt-dirty"  dev
    git -C "$FIX" worktree add -q -b feat/ahead  "$TMP/wt-ahead"  dev
    git -C "$FIX" worktree add -q -b feat/clean  "$TMP/wt-clean"  dev
    git -C "$FIX" worktree add -q -b feat/merged "$TMP/wt-merged" dev
    git -C "$FIX" worktree add -q -b feat/norf   "$TMP/wt-norf"   dev
    printf 'd\n' >> "$TMP/wt-dirty/A.txt"                                             # 树脏
    printf 'a\n' >> "$TMP/wt-ahead/A.txt"
    git -C "$TMP/wt-ahead" add A.txt
    git -C "$TMP/wt-ahead" -c user.email=g@test -c user.name=guard commit -q -m ahead    # 未并入提交
    printf 'm\n' >> "$TMP/wt-merged/A.txt"
    git -C "$TMP/wt-merged" add A.txt
    git -C "$TMP/wt-merged" -c user.email=g@test -c user.name=guard commit -q -m m
    git -C "$FIX" merge -q --no-edit feat/merged         # 并回 dev：worktree 本体转为可清
    rm -f "$FIX/.git/logs/refs/heads/feat/norf"            # 抹 reflog → base 应标 unknown

    echo "worktree-audit self-test @ $TMP"

    run_case
    rc_is 0 "审计模式（只读）→ 0"
    has "feat/dirty"  "审计列出 wt-dirty"
    has "在飞(树脏"   "wt-dirty 判在飞（树脏）"
    has "在飞(tip 未并入 dev)" "wt-ahead 判在飞（未并入提交）"
    has "可清"        "干净已并入的树判可清"
    has "base:unknown" "reflog 被抹的分支 base 标 unknown"
    has "主树"        "主树单独标注且永不清理"

    run_case --clean "$TMP/wt-ahead"
    rc_is 1 "清理有未并入提交的 worktree → 拒(1)"

    run_case --clean "$TMP/wt-dirty"
    rc_is 1 "清理树脏的 worktree → 拒(1)"

    run_case --clean "$FIX"
    rc_is 1 "清理主树 → 拒(1)"

    run_case --clean "$TMP/wt-clean" extra
    rc_is 2 "--clean 带多余位置参数（一次只清一个）→ 2"

    run_case --clean "$TMP/not-registered"
    rc_is 2 "非登记 worktree → 2（环境错）"

    run_case --clean "$TMP/wt-clean"
    rc_is 0 "清理干净且已并入的 worktree → 0"
    if [ -d "$TMP/wt-clean" ]; then bad "wt-clean 目录应已删除"; else say "wt-clean 目录确已删除"; fi

    run_case --clean "$TMP/wt-merged"
    rc_is 0 "清理已并入 dev 的 worktree → 0"

    if [ "$fail" -eq 0 ]; then echo "self-test: 全绿"; else echo "self-test: 有断言失败"; exit 1; fi
}

SELF_TEST=0; ROOT=""; CLEAN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --root)      [ $# -ge 2 ] || { usage; exit $EXIT_ENV; }; ROOT="$2"; shift 2 ;;
        --clean)     [ $# -ge 2 ] || { usage; exit $EXIT_ENV; }; CLEAN="$2"; shift 2 ;;
        --self-test) SELF_TEST=1; shift ;;
        -h|--help)   usage; exit $EXIT_OK ;;
        *)           usage; echo "未知参数：$1（审计模式无位置参数；清理用 --clean <路径>，一次一个）" >&2; exit $EXIT_ENV ;;
    esac
done

if [ "$SELF_TEST" -eq 1 ]; then self_test; exit $EXIT_OK; fi

[ -n "$ROOT" ] || ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null)" || true
[ -n "$ROOT" ] || { echo "❌ 环境错：$SELF 不在 git 仓内且未给 --root" >&2; exit $EXIT_ENV; }
[ -d "$ROOT" ] || { echo "❌ 环境错：--root 不存在: $ROOT" >&2; exit $EXIT_ENV; }
# dev = 集成基线；缺 dev 则「已并入 dev / 判活」全部无从算起
git -C "$ROOT" rev-parse --verify --quiet refs/heads/dev >/dev/null \
    || { echo "❌ 环境错：$ROOT 缺集成分支 dev" >&2; exit $EXIT_ENV; }

if [ -n "$CLEAN" ]; then clean_mode "$CLEAN"; fi
audit_mode
exit $EXIT_OK
