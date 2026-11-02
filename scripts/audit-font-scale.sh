#!/usr/bin/env bash
# scripts/audit-font-scale.sh
# 字号缩放审计（一次性，B6/F-69-2）：找出「字号走缩放、行盒/文本盒走裸字面量」的调用点。
#
# 语义（对齐方案文档 §10.7 两条 grep）：
#   判据 A：`.lineHeight(<数字>` 且不含 scaled / roundFs / scaledText / st.lineHeight
#   判据 B：`.size(<任意>, <数字>` 里第二参数是裸数字（行盒没随字号缩放）
#           且同一元素链含 `.text(`（纯图形/容器不算）
#
# 输出：路径:行号:命中行 的清单（stdout），末行给两个判据的总数。
# 退出码恒 0（这是审计工具不是守卫）；是否整改由人工过清单决定。
#
# 用法：bash scripts/audit-font-scale.sh [输出文件]
#   └ 给输出文件时同时落盘（供证据归档），stdout 仍打印。
#
# 注意：本脚本只报候选，不做语义判定——命中里可能含「刻意固定行盒」的图形元素
#（进度条、色块、空态占位），需人工过一遍再改。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-}"

emit() {
    if [[ -n "$OUT" ]]; then
        tee "$OUT"
    else
        cat
    fi
}

EXCLUDE='scaled|roundFs|scaledText|st\.lineHeight|lineHeight\(h\)|lineHeight\(nameH\)|lineHeight\(rowDescH\)'

{
    echo "# 判据 A：.lineHeight(裸数字)（行盒不随字号缩放 → 文字被裁）"
    grep -rn --include='*.cpp' --include='*.h' -E '\.lineHeight\([0-9]' "$ROOT/src" |
        grep -vE "$EXCLUDE" | sed "s|$ROOT/||" || true

    echo
    echo "# 判据 B：.size(<任意>, 裸数字) 且同一元素链含 .text(（文本盒高不随字号缩放）"
    # dsl 链跨多行，按「块」扫：块起点 = 含 ui.text( 的行（含 ui_kit 的文本组件），
    # 块终点 = 含 .build() 的行；块内出现裸数字第二参数 .size( 即报块首行号。
    awk -v excl="$EXCLUDE" '
        {
            line = $0
            if (line ~ /\.build\(\)/ && inblk) {
                if (hitsize && hastxt) { printf "%s:%d: %s\n", file, start, blkhead }
                inblk = 0; hitsize = 0; hastxt = 0
            }
            if (!inblk && line ~ /(ui|inner|panel)\.(text|button|input)\(/) {
                inblk = 1; start = FNR; blkhead = line
                hitsize = 0; hastxt = 0
            }
            if (inblk) {
                if (line ~ /\.text\(/) hastxt = 1
                if (line ~ /\.size\([^,)]*,[[:space:]]*[0-9]/) {
                    if (line !~ excl) hitsize = 1
                }
            }
        }
        FNR == 1 { if (NR > 1) {} }
        { file = FILENAME }
    ' $(find "$ROOT/src" -name '*.cpp') | sed "s|$ROOT/||" | sort || true

    echo
    a=$(grep -rn --include='*.cpp' --include='*.h' -E '\.lineHeight\([0-9]' "$ROOT/src" |
        grep -vE "$EXCLUDE" | wc -l | tr -d ' ')
    b=$(awk -v excl="$EXCLUDE" '
        {
            line = $0
            if (line ~ /\.build\(\)/ && inblk) {
                if (hitsize && hastxt) n++
                inblk = 0; hitsize = 0; hastxt = 0
            }
            if (!inblk && line ~ /(ui|inner|panel)\.(text|button|input)\(/) {
                inblk = 1; hitsize = 0; hastxt = 0
            }
            if (inblk) {
                if (line ~ /\.text\(/) hastxt = 1
                if (line ~ /\.size\([^,)]*,[[:space:]]*[0-9]/) {
                    if (line !~ excl) hitsize = 1
                }
            }
        }
        END { print n + 0 }
    ' $(find "$ROOT/src" -name '*.cpp'))
    echo "# 合计：判据 A = $a 处，判据 B = $b 处"
} | emit

exit 0
