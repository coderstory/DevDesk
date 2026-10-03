#!/usr/bin/env bash
# scripts/check-quota-write-paths.sh
# 余量结果落库路径静态守卫（G8）——「失败韧性的决策只有一个入口」用**代码**保证。
#
# ── 为什么需要它（这不是重复劳动，是补一个真实的洞）────────────────────
# quota_store.h 不变量 3 声称：「`applyFetchResult` 是一次 fetch 落库的**唯一入口**
# ……谁要是绕过它直接 saveResult，韧性就废了」。这句话原本**只是一句注释**——
# 本会话的病根正是「注释断言」冒充「代码断言」（quota_provider.h 用三条注释禁止
# 「把失败画成有效值」、注册表守卫用一句注释声称看得见哨兵之后的行，两句都被现实
# 破了）。本守卫把那句注释变成会红的断言。
#
# ── 断言（四条）────────────────────────────────────────────────────
#   A. `src/` **与 `tools/`** 下任何文件都不得出现针对 `profile_quota_result`
#      的 DML，除了 quota_store.cpp 里的那两条白名单语句（见 B）。schema.h 的
#      CREATE TABLE 是 DDL、不在管辖内；注释行按「首非空白为 #」过滤。
#      ⚠ **为什么必须扫 tools/**：tools/quota_live_probe.cpp 就在 tools/ 下、
#      **链接同一个 devdesk_module_claude_proxy.a**（run-quota-live-probe.sh），
#      它完全有能力绕过 applyFetchResult 直写这张表，而守卫一个字都不会报。
#      静态审计（2026-10-03）亲手构造过这个反证：往临时树的 tools/rogue.cpp
#      放一条 `DELETE FROM profile_quota_result` → 旧版守卫报绿、EXIT=0。
#      「韧性决策只有一个入口」这个守卫，在自己最该管的目录外留了个洞。
#   B. 那两条白名单语句必须**词法落在**对应函数体内：
#        INSERT OR REPLACE INTO profile_quota_result  → QuotaStore::applyFetchResult
#        DELETE FROM profile_quota_result             → QuotaStore::deleteResult
#      插在别的函数里 = 韧性决策被绕开。
#   C. **条数钉死**：必须恰好 1 条 INSERT + 1 条 DELETE。少一条不是「更干净」
#      而是「入口被改名 / 被删 / 被挪到 grep 看不见的形态」——那种情况本守卫
#      必须报红而不是放行（这是本脚本第一版踩过的真坑：正则写成
#      `INSERT OR REPLACE` 直接跟表名，漏了 `INTO`，于是 INSERT 一条都扫不到，
#      脚本却因为「扫到了 DELETE」而报绿——**假绿守卫**，正是本项目反复翻车的那类）。
#   D. **自证非恒真**（--self-test）：同一段逻辑喂构造的坏输入（INSERT 挪出
#      applyFetchResult / 另一个文件直接 DELETE / 把 INSERT 删掉 / **在
#      tools/ 下直写 DELETE**），必须逐条报红。一条从不变绿的守卫等于没写。
#
# 退出码：0 = 通过；1 = 违规；2 = 环境错（**不静默绿**）
#
# 用法：bash scripts/check-quota-write-paths.sh [--root DIR] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-quota-write-paths.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
            usage
            exit 2
            ;;
    esac
done

STORE_REL="src/module_claude_proxy/quota/quota_store.cpp"
STORE="$ROOT/$STORE_REL"

if [ ! -f "$STORE" ]; then
    echo "环境错：找不到 $STORE_REL" >&2
    exit 2
fi
if [ ! -d "$ROOT/src" ] && [ ! -d "$ROOT/tools" ]; then
    echo "环境错：$ROOT 下既没有 src/ 也没有 tools/（--root 指错了？）" >&2
    exit 2
fi

# DML 形态三条各自成一条 alternation。**注意 INSERT 分支里的 INTO 是必需的**：
# 漏掉它会让 INSERT 一条都扫不到，而脚本因「扫到了 DELETE」照样报绿（假绿）。
DML_RE='(^|[^A-Za-z_])(INSERT[[:space:]]+(OR[[:space:]]+(REPLACE|IGNORE)[[:space:]]+)?INTO[[:space:]]+profile_quota_result|UPDATE[[:space:]]+profile_quota_result|DELETE[[:space:]]+FROM[[:space:]]+profile_quota_result)'

# 把「谁写了 profile_quota_result」问出来。抽成函数，好让 --self-test 拿构造的
# 坏输入跑**同一段**逻辑（否则自证走的是另一条路径，证明不了真守卫）。
# $1 = 仓库根。命中打到 stdout，返回违规条数（0 = 干净）。
scan_write_paths() {
    local root="$1"
    local store="$root/$STORE_REL"
    local violations=0
    local inserts=0
    local deletes=0

    # ⚠ **扫描面 = src/ + tools/**（静态审计 B3）。tools/ 下的
    # quota_live_probe.cpp 链接同一个静态库、能绕过 applyFetchResult 直写这张
    # 表 —— 旧版只扫 src/ 时这里是个洞（亲手构造反证：tools/rogue.cpp 里一条
    # DELETE → 守卫报绿 EXIT=0）。少扫一个目录 = 守卫在它最该管的地方失明。
    local scan_dirs=()
    [ -d "$root/src" ] && scan_dirs+=("$root/src")
    [ -d "$root/tools" ] && scan_dirs+=("$root/tools")
    if [ "${#scan_dirs[@]}" -eq 0 ]; then
        echo "环境错：$root 下既没有 src/ 也没有 tools/"
        return 2
    fi

    local hits
    hits="$(grep -rnE "$DML_RE" "${scan_dirs[@]}" --include='*.cpp' --include='*.h' 2>/dev/null \
        | grep -vE ':[[:space:]]*#' || true)"

    if [ -z "$hits" ]; then
        echo "违规：一条 DML 都没扫到——正则失效或表被改名，守卫不得静默绿"
        return 1
    fi

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        local file="${line%%:*}"
        local rest="${line#*:}"
        local lineno="${rest%%:*}"
        local text="${rest#*:}"

        if [ "$file" != "$store" ]; then
            echo "违规：${file#$root/}:$lineno —— 绕过 QuotaStore::applyFetchResult 直接写 profile_quota_result"
            echo "      $text"
            violations=$((violations + 1))
            continue
        fi

        # 归属函数：从文件开头数到本行，最后一个顶格函数签名就是它。本文件所有
        # 函数签名顶格、闭合 `}` 也顶格（仓库统一风格），故 `^[A-Za-z_].*\(` 即签名。
        # ⚠ **已知脆弱点（静态审计 m3 登记，此处刻意不修）**：签名**折行**时匹配到的
        #   是折行后的最后一行（quota_store.cpp:417-419 的 applyFetchResult 本身
        #   就折行，当前恰好还能用）。有人**再折一行**（参数列表某项单独成行且顶格）
        #   → sig 被更新成一个不以 `QuotaStore::` 结尾的行 → sed 抽不出函数名 →
        #   owner_name 为空 → **报红**。方向是保守的（假红，不是假绿），
        #   故本轮不修；但它不是「无害」——真出问题时它会挡住一次正确提交，
        #   所以登记在案：若某天守卫在 applyFetchResult 那行报「落在 <未知函数>」，
        #   先去数 applyFetchResult 的签名折了几行，而不是去改归属逻辑。
        local owner_name
        owner_name="$(awk -v target="$lineno" '
            /^[A-Za-z_].*\(/ { sig=$0 }
            NR == target { print sig; exit }
        ' "$store" | sed -E 's/.*(QuotaStore::[A-Za-z_]+).*/\1/')"

        case "$text" in
            *INSERT*)
                inserts=$((inserts + 1))
                if [ "$owner_name" != "QuotaStore::applyFetchResult" ]; then
                    echo "违规：$STORE_REL:$lineno —— INSERT 落在 ${owner_name:-<未知函数>} 里，应在 QuotaStore::applyFetchResult"
                    echo "      $text"
                    violations=$((violations + 1))
                fi
                ;;
            *DELETE*)
                deletes=$((deletes + 1))
                if [ "$owner_name" != "QuotaStore::deleteResult" ]; then
                    echo "违规：$STORE_REL:$lineno —— DELETE 落在 ${owner_name:-<未知函数>} 里，应在 QuotaStore::deleteResult"
                    echo "      $text"
                    violations=$((violations + 1))
                fi
                ;;
            *)
                echo "违规：$STORE_REL:$lineno —— 未预期的写语句：$text"
                violations=$((violations + 1))
                ;;
        esac
    done <<< "$hits"

    # C：条数钉死。少一条 = 入口消失 / 变形，不是「更干净」。
    if [ "$inserts" -ne 1 ]; then
        echo "违规：applyFetchResult 的 INSERT 语句应恰好 1 条，实际扫到 $inserts 条（入口被改名/删除/变形）"
        violations=$((violations + 1))
    fi
    if [ "$deletes" -ne 1 ]; then
        echo "违规：deleteResult 的 DELETE 语句应恰好 1 条，实际扫到 $deletes 条"
        violations=$((violations + 1))
    fi

    return $violations
}

# ── --self-test：同一段逻辑喂构造的坏输入，必须逐条报红 ──────────────
if [ "$SELF_TEST" = "1" ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    mkdir -p "$TMP/src/module_claude_proxy/quota"

    # 坏输入 1：INSERT 挪到 applyFetchResult 之外；另有一个文件直接 DELETE。
    cat > "$TMP/$STORE_REL" <<'BADSTORE'
namespace module::claude::quota {
void QuotaStore::deleteResult(int64_t) {}
void QuotaStore::applyFetchResult(int64_t, const QuotaResult&, int64_t) {}
void QuotaStore::sneakyDirectWrite(int64_t) {
    db_.prepare("INSERT OR REPLACE INTO profile_quota_result (profile_id) VALUES (?)");
}
}
BADSTORE
    cat > "$TMP/src/other_writer.cpp" <<'BADOTHER'
void elsewhere() {
    stmt("DELETE FROM profile_quota_result WHERE profile_id = ?");
}
BADOTHER
    out="$(scan_write_paths "$TMP" 2>&1)"; rc=$?
    echo "$out"
    fail=0
    [ "$rc" -ne 0 ] || { echo "❌ 自证失败：坏输入①下守卫仍报 0 违规（恒真守卫 = 没写）"; fail=1; }
    printf '%s' "$out" | grep -q "sneakyDirectWrite" \
        || { echo "❌ 自证失败：没认出 applyFetchResult 之外的 INSERT"; fail=1; }
    printf '%s' "$out" | grep -q "other_writer.cpp" \
        || { echo "❌ 自证失败：没认出 quota_store.cpp 之外的写入"; fail=1; }

    # 坏输入 ②：好表——INSERT / DELETE 各在对的函数里。这一条必须**报 0 违规**，
    # 否则说明守卫见什么都是红（那也是恒真，只是方向相反）。
    cat > "$TMP/$STORE_REL" <<'GOODSTORE'
namespace module::claude::quota {
void QuotaStore::deleteResult(int64_t) {
    db_.prepare("DELETE FROM profile_quota_result WHERE profile_id = ?");
}
void QuotaStore::applyFetchResult(int64_t, const QuotaResult&, int64_t) {
    db_.prepare("INSERT OR REPLACE INTO profile_quota_result (profile_id) VALUES (?)");
}
}
GOODSTORE
    rm -f "$TMP/src/other_writer.cpp"
    out2="$(scan_write_paths "$TMP" 2>&1)"; rc2=$?
    echo "$out2"
    [ "$rc2" -eq 0 ] || { echo "❌ 自证失败：好表被误报（$rc2 违规）"; fail=1; }

    # 坏输入 ③：INSERT 整条删掉（入口消失）——必须报红，不能因为「还有 DELETE」
    # 就放行。这正是本脚本第一版的假绿形态。
    cat > "$TMP/$STORE_REL" <<'NOINSERT'
namespace module::claude::quota {
void QuotaStore::deleteResult(int64_t) {
    db_.prepare("DELETE FROM profile_quota_result WHERE profile_id = ?");
}
void QuotaStore::applyFetchResult(int64_t, const QuotaResult&, int64_t) {}
}
NOINSERT
    out3="$(scan_write_paths "$TMP" 2>&1)"; rc3=$?
    echo "$out3"
    [ "$rc3" -ne 0 ] || { echo "❌ 自证失败：INSERT 消失仍报绿（第一版踩的假绿）"; fail=1; }
    printf '%s' "$out3" | grep -q "恰好 1 条" \
        || { echo "❌ 自证失败：没报出条数钉死那条"; fail=1; }

    # 坏输入 ④：直写挪到 **tools/** 下 —— 必须报红。
    # 这条专门钉住 2026-10-03 静态审计 B3 那个亲手构造出来的洞：旧版只扫
    # src/，在 tools/rogue.cpp 里放一条 DELETE 照样 EXIT=0 报绿，而
    # tools/quota_live_probe.cpp 就在那儿、链同一个静态库、完全能绕开入口。
    # 只补扫描面而不补这条自测 = 下次有人把扫描面改回去，没人知道。
    cat > "$TMP/$STORE_REL" <<'GOODSTORE2'
namespace module::claude::quota {
void QuotaStore::deleteResult(int64_t) {
    db_.prepare("DELETE FROM profile_quota_result WHERE profile_id = ?");
}
void QuotaStore::applyFetchResult(int64_t, const QuotaResult&, int64_t) {
    db_.prepare("INSERT OR REPLACE INTO profile_quota_result (profile_id) VALUES (?)");
}
}
GOODSTORE2
    mkdir -p "$TMP/tools"
    cat > "$TMP/tools/rogue.cpp" <<'ROGUE'
void rogueProbeMain() {
    db.prepare("DELETE FROM profile_quota_result WHERE profile_id=1");
}
ROGUE
    out4="$(scan_write_paths "$TMP" 2>&1)"; rc4=$?
    echo "$out4"
    [ "$rc4" -ne 0 ] || { echo "❌ 自证失败：tools/ 下直写 DELETE 仍报绿（审计 B3 的洞复发）"; fail=1; }
    printf '%s' "$out4" | grep -q "tools/rogue.cpp" \
        || { echo "❌ 自证失败：没认出 tools/rogue.cpp 的越界写入（扫描面可能又退回 src-only）"; fail=1; }
    rm -rf "$TMP/tools"

    [ "$fail" -eq 0 ] || exit 1
    echo "✅ 守卫自证非恒真：坏输入①②③④各报红、好表不误报"
    exit 0
fi

violations="$(scan_write_paths "$ROOT")"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "❌ 余量落库路径守卫：$rc 条违规（韧性决策必须只在 applyFetchResult 里）"
    exit 1
fi
echo "✅ 余量落库路径守卫通过（profile_quota_result 的 DML 只有 applyFetchResult 的 INSERT 与 deleteResult 的 DELETE，均在 quota_store.cpp）"
exit 0
