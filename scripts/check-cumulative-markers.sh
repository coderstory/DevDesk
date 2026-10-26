#!/usr/bin/env bash
# check-cumulative-markers.sh —— 累积核对：同一文件经多轮注入/修改后，验证全部历史轮次的交付标记仍存在。
# 背景与方法：multi-wave 覆盖事故（2026-10-21 战略基线融合）——后轮改写会挤掉前轮注入，
# 每轮只验「本轮交付项」会全绿漏丢；唯一可靠验收 = 全部历史轮次标记的累积清单逐项回归。
#
# 用法：bash check-cumulative-markers.sh <清单文件>
#   清单格式：每行一条「<仓库内相对路径>::<期望子串>」；# 开头为注释；空行/纯空白行忽略（兼容 CRLF 行尾）
#   子串按固定字符串匹配（grep -F），不解析正则——清单写什么查什么，防正则意外
# 退出码：0 = 全部命中（PASS）；1 = 有缺失（逐条列出，FAIL）；2 = 环境/格式错（不静默绿）
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
list="${1:-}"
if [ -z "$list" ] || [ ! -r "$list" ]; then
  echo "用法: bash $0 <清单文件>（每行: 相对路径::期望子串）" >&2
  exit 2
fi
total=0; miss=0; bad=0
while IFS= read -r line; do
  line="${line%$'\r'}" # 清单可能被 Windows 编辑器写成 CRLF：先剥行尾 CR，否则空行变 "\r" 误报格式错、标记带 CR 致 grep 全 miss
  case "${line//[[:space:]]/}" in ''|'#'*) continue ;; esac # 空行/纯空白/注释行忽略，仅对非空行校验格式
  path="${line%%::*}"
  marker="${line#*::}"
  if [ -z "$path" ] || [ -z "$marker" ] || [ "$path" = "$line" ]; then
    echo "[清单格式错误] 缺 :: 分隔: $line" >&2
    bad=1
    continue
  fi
  total=$((total+1))
  f="$ROOT/$path"
  if [ ! -f "$f" ]; then
    echo "[缺失] 文件不存在: $path"
    miss=$((miss+1))
    continue
  fi
  n=$(grep -cF -- "$marker" "$f" 2>/dev/null)
  n="${n:-0}"
  if [ "$n" -eq 0 ]; then
    echo "[缺失] $path :: $marker"
    miss=$((miss+1))
  else
    echo "[命中] $path :: $marker ($n)"
  fi
done < "$list"
if [ "$bad" -eq 1 ]; then
  echo "结果: 清单格式有错 —— 退出 2" >&2
  exit 2
fi
if [ "$miss" -gt 0 ]; then
  echo "结果: $miss/$total 缺失 —— FAIL" >&2
  exit 1
fi
echo "结果: $total/$total 全命中 —— PASS"
exit 0
