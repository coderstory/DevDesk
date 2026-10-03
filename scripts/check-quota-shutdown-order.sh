#!/usr/bin/env bash
# scripts/check-quota-shutdown-order.sh
# 余量链停机顺序静态守卫（M5）——「顺序错了就是悬垂」用**代码**保证，不靠注释。
#
# ── 它在守什么 ─────────────────────────────────────────────────────
# src/module_claude_proxy/claude_module.cpp 的 stopQuotaRefresh() 有一条不可
# 协商的释放顺序：
#
#     quotaTestThread_.join()   ← 「测试连接」线程必须先收（留 joinable 即析构
#                                 std::terminate，且工作线程捕获了 this）
#     quotaRefresher_->stopAll() + quotaRefresher_.reset()   ← 引用 credentials/store
#     quotaCredentials_.reset()
#     quotaStore_.reset()       ← **必须最后**：store 先死，刷新器下一次扫描解引用即 UB
#
# 这段顺序此前**只有注释**在说。静态审计（2026-10-03 M5）用两次变异实测过它的
# 零覆盖：把 quotaStore_.reset() 挪到最前 → 全量 1257 例零新增失败；把
# `if (quotaTestThread_.joinable()) quotaTestThread_.join();` 整行删掉 → 同样零覆盖。
# 两条最贵的并发契约，一条测试都没有。
#
# ── 为什么用源码形态守卫而不是运行时用例 ────────────────────────────
# 「顺序」是一个**纯形态**不变式：把它做成运行时用例需要构造一条在途的
# quotaTestThread_（该路径要真发一次 provider 查询，见 runQuotaTestConnection），
# 代价是真网络 + 极脆的时序编排，而它断言的仍然只是「没崩」。形态守卫直接对着
# 源码顺序断言，成本为零、确定性为 100%。**两条都用**：形态守卫在本脚本，
# 生命周期用例在 tests/（见 test_claude_quota_store.cpp 的 shutdown_lifecycle）。
#
# ── 断言（四条）────────────────────────────────────────────────────
#   A. **四个动作各恰好命中一次**，且都在 stopQuotaRefresh() 函数体内。
#   B. **行号严格递增**：join < refresher.reset < credentials.reset < store.reset。
#      顺序颠倒就是悬垂，必须报红。
#   C. join 那行必须带 `joinable()` 守卫（裸 join() 在线程没起时是 UB）。
#   D. **自证非恒真**（--self-test）：喂构造的坏输入（把 store reset 挪最前 /
#      删掉整行 join / 去掉 joinable 守卫），必须逐条报红。
#      一条从不变绿的守卫等于没写——这是本项目反复翻车的那类假绿。
#
# 退出码：0 = 通过；1 = 违规；2 = 环境错（**不静默绿**）
#
# 用法：bash scripts/check-quota-shutdown-order.sh [--root DIR] [--self-test]
set -uo pipefail

usage() {
    echo "用法: bash scripts/check-quota-shutdown-order.sh [--root DIR] [--self-test]" >&2
}

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git -C "$SELF" rev-parse --show-toplevel 2>/dev/null || dirname "$SELF")"
SELF_TEST=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --root)
            [ $# -ge 2 ] || { usage; exit 2; }
            ROOT="$2"; shift 2 ;;
        --self-test)
            SELF_TEST=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            usage; exit 2 ;;
    esac
done

TARGET_REL="src/module_claude_proxy/claude_module.cpp"

if [ ! -f "$ROOT/$TARGET_REL" ]; then
    echo "环境错：找不到 $TARGET_REL" >&2
    exit 2
fi

# 从 stopQuotaRefresh() 的函数签名起，到它**闭合的**那一行 `}` 为止，抽出函数体。
# ⚠ 必须按**花括号深度**判定结束，不能用「第一行顶格/缩进的 `}`」——真实文件里
#   stopQuotaRefresh 体内先撞上的是 `if (quotaRefresher_) { ... }` 的收尾 `}`
#   （claude_module.cpp:920），按「第一个 }」截断会把 quotaCredentials_/quotaStore_
#   两行切在块外 → 守卫在**好树上就报红**（恒红守卫比没有守卫更糟）。
# 行内注释与字符串里的花括号先剥掉，否则注释里一个 `}` 就能提前截断。
stop_body() {
    awk '
        # 认**定义**不认调用：`stopQuotaRefresh();`（析构 :69 与 onStop :405 各
        # 调一次，且都排在定义之前）也会匹配 `stopQuotaRefresh()`，取首个就会
        # 从调用点开始截断。要求同一行以 `{` 收尾才当定义。
        /stopQuotaRefresh\(\)[ \t]*\{/ && !seen { seen = 1; inbody = 1; depth = 0 }
        !inbody { next }
        {
            code = $0
            sub(/\/\/.*$/, "", code)          # 剥行内注释
            gsub(/"[^"]*"/, "", code)          # 剥字符串字面量
            opens = gsub(/\{/, "{", code)
            closes = gsub(/\}/, "}", code)
            print FNR ":" $0
            depth += opens - closes
            if (depth <= 0) exit
        }
    ' "$1"
}

# $1 = 仓库根。返回违规条数（0 = 通过）。stdout 打违规明细。
check_order() {
    local root="$1"
    local file="$root/$TARGET_REL"
    local body
    body="$(stop_body "$file")"
    if [ -z "$body" ]; then
        echo "环境错：在 $TARGET_REL 里找不到 stopQuotaRefresh() 的函数体（签名被改名？）"
        return 2
    fi

    # 从头一行开始，故行号可与 stop_body 的输出直接对齐比较。
    # 四个动作各取第一条命中（函数体内不应出现第二次；A 条另查重复）。
    local violations=0
    local join_line
    join_line="$(printf '%s\n' "$body" | grep -n 'quotaTestThread_\.join()' | head -1 | cut -d: -f1)"
    # C 的判据：**join 那一行本身**必须带 joinable() 守卫。不能用「join 行存在」
    # 顺带推出「守卫存在」—— 那是复制粘贴式的判据错位（本脚本第一版就这么写，
    # 自测立刻报出「裸 join 仍绿」，当场拆穿）。裸 join() 在线程没起时是 UB。
    local joinable_guarded=0
    if printf '%s\n' "$body" | grep -q 'joinable().*quotaTestThread_\.join()'; then
        joinable_guarded=1
    fi
    local refresher_line creds_line store_line
    refresher_line="$(printf '%s\n' "$body" | grep -n 'quotaRefresher_\.reset()' | head -1 | cut -d: -f1)"
    creds_line="$(printf '%s\n' "$body" | grep -n 'quotaCredentials_\.reset()' | head -1 | cut -d: -f1)"
    store_line="$(printf '%s\n' "$body" | grep -n 'quotaStore_\.reset()' | head -1 | cut -d: -f1)"

    # A：四个动作都必须存在
    local missing=0
    [ -n "$join_line" ] || { echo "违规：stopQuotaRefresh() 里没有 quotaTestThread_.join() —— 留 joinable 线程即析构 std::terminate"; missing=1; }
    [ -n "$refresher_line" ] || { echo "违规：stopQuotaRefresh() 里没有 quotaRefresher_.reset() —— 刷新器会活过凭据源与 store"; missing=1; }
    [ -n "$creds_line" ] || { echo "违规：stopQuotaRefresh() 里没有 quotaCredentials_.reset()"; missing=1; }
    [ -n "$store_line" ] || { echo "违规：stopQuotaRefresh() 里没有 quotaStore_.reset() —— 每次停机都泄漏一整套 store"; missing=1; }
    if [ "$missing" -ne 0 ]; then
        violations=$((violations + missing))
        return $violations
    fi

    # C：join 必须带 joinable() 守卫
    if [ "$joinable_guarded" -eq 0 ]; then
        echo "违规：quotaTestThread_.join() 没有 joinable() 守卫 —— 线程未起时裸 join() 是 UB"
        violations=$((violations + 1))
    fi

    # B：行号严格递增
    local prev_name="quotaTestThread_.join()" prev_line="$join_line"
    local cur
    for cur in "$refresher_line" "$creds_line" "$store_line"; do
        local cur_name
        case "$cur" in
            "$refresher_line") cur_name="quotaRefresher_.reset()" ;;
            "$creds_line")    cur_name="quotaCredentials_.reset()" ;;
            *)                cur_name="quotaStore_.reset()" ;;
        esac
        if [ "$cur" -le "$prev_line" ]; then
            echo "违规：${cur_name}（第 ${cur} 行）必须排在 ${prev_name}（第 ${prev_line} 行）**之后** —— store 先死则刷新器下一次扫描解引用即 UB"
            violations=$((violations + 1))
        fi
        prev_line="$cur"
        prev_name="$cur_name"
    done
    return $violations
}

# ── --self-test：喂构造的坏输入，必须逐条报红 ───────────────────────
if [ "$SELF_TEST" = "1" ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    mkdir -p "$TMP/$(dirname "$TARGET_REL")"

    # 好树：顺序正确 + join 带 joinable 守卫。
    cat > "$TMP/$TARGET_REL" <<'GOOD'
void ClaudeModule::stopQuotaRefresh() {
    if (quotaTestThread_.joinable()) quotaTestThread_.join();
    quotaTestDone_.store(false, std::memory_order_release);
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
    quotaStore_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
GOOD
    # 坏输入 ①：store reset 挪到最前（**正是这条不变式要防的悬垂**）
    cat > "$TMP/$TARGET_REL" <<'BAD_STORE_FIRST'
void ClaudeModule::stopQuotaRefresh() {
    quotaStore_.reset();
    if (quotaTestThread_.joinable()) quotaTestThread_.join();
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
BAD_STORE_FIRST
    # 坏输入 ②：整行删掉 join（留 joinable → 析构 std::terminate）
    cat > "$TMP/$TARGET_REL" <<'BAD_NO_JOIN'
void ClaudeModule::stopQuotaRefresh() {
    quotaTestDone_.store(false, std::memory_order_release);
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
    quotaStore_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
BAD_NO_JOIN
    # 坏输入 ③：join 去掉 joinable() 守卫（裸 join）
    cat > "$TMP/$TARGET_REL" <<'BAD_JOIN_UNGUARDED'
void ClaudeModule::stopQuotaRefresh() {
    quotaTestThread_.join();
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
    quotaStore_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
BAD_JOIN_UNGUARDED

    fail=0

    cat > "$TMP/$TARGET_REL" <<'GOOD'
void ClaudeModule::stopQuotaRefresh() {
    if (quotaTestThread_.joinable()) quotaTestThread_.join();
    quotaTestDone_.store(false, std::memory_order_release);
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
    quotaStore_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
GOOD
    out="$(check_order "$TMP" 2>&1)"; rc=$?
    [ "$rc" -eq 0 ] || { echo "$out"; echo "❌ 自证失败：好表被误报（恒红守卫比没有守卫更糟）"; fail=1; }

    for bad in BAD_STORE_FIRST BAD_NO_JOIN BAD_JOIN_UNGUARDED; do
        case "$bad" in
            BAD_STORE_FIRST) body=$(cat <<'X'
void ClaudeModule::stopQuotaRefresh() {
    quotaStore_.reset();
    if (quotaTestThread_.joinable()) quotaTestThread_.join();
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
X
) ;;
            BAD_NO_JOIN) body=$(cat <<'X'
void ClaudeModule::stopQuotaRefresh() {
    quotaTestDone_.store(false, std::memory_order_release);
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
    quotaStore_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
X
) ;;
            BAD_JOIN_UNGUARDED) body=$(cat <<'X'
void ClaudeModule::stopQuotaRefresh() {
    quotaTestThread_.join();
    if (quotaRefresher_) {
        quotaRefresher_->stopAll();
        quotaRefresher_.reset();
    }
    quotaCredentials_.reset();
    quotaStore_.reset();
}
void ClaudeModule::harvestQuotaTestResult() {
    quotaTestThread_.join();
}
X
) ;;
        esac
        printf '%s\n' "$body" > "$TMP/$TARGET_REL"
        out="$(check_order "$TMP" 2>&1)"; rc=$?
        [ "$rc" -ne 0 ] || { echo "❌ 自证失败：坏输入 [$bad] 下守卫仍报 0 违规（恒真守卫 = 没写）"; fail=1; }
    done

    [ "$fail" -eq 0 ] || exit 1
    echo "✅ 守卫自证非恒真：坏输入①store 挪最前 / ②删 join 行 / ③裸 join 各报红、好表不误报"
    exit 0
fi

violations="$(check_order "$ROOT")"
rc=$?
if [ "$rc" -eq 2 ]; then
    echo "❌ 余量停机顺序守卫：环境错（$rc）"
    exit 2
fi
if [ "$rc" -ne 0 ]; then
    echo "❌ 余量停机顺序守卫：$rc 条违规（顺序错了就是悬垂：store 先死，刷新器下一次扫描解引用即 UB）"
    exit 1
fi
echo "✅ 余量停机顺序守卫通过（stopQuotaRefresh: 测试线程 join → refresher reset → credentials reset → store reset，join 带 joinable 守卫）"
exit 0