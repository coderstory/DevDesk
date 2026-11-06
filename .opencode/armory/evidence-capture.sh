#!/bin/sh
# devflow 项目适配 wrapper（0.38.14 起自包含）：采集逻辑内嵌，零插件源码/缓存路径依赖
# setup 时填充：EVIDENCE_ROOT = 归档根目录；重跑 setup 幂等刷新本副本（逻辑以插件 scripts/evidence-capture.sh 为准）
export EVIDENCE_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
slug="${1:?用法: evidence-capture.sh <slug> [--shot] [-- <命令>]}"
case "$slug" in *[!A-Za-z0-9_-]*) echo "slug 仅允许 [A-Za-z0-9_-]：$slug" >&2; exit 1;; esac
shift
shot=0
has_cmd=0
if [ "${1:-}" = "--shot" ]; then shot=1; shift; fi
if [ "${1:-}" = "--" ]; then shift; has_cmd=1; fi
root="${EVIDENCE_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
dir="$root/evidence/$(date +%Y%m%d)-$slug"
mkdir -p "$dir"
{
  echo "slug: $slug"
  echo "date: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "cwd:  $(pwd)"
  if [ "$has_cmd" = 1 ]; then printf 'cmd:  '; printf '%s ' "$@"; echo; else echo "cmd:  <无>"; fi
} > "$dir/meta.txt"
if [ "$has_cmd" = 1 ] && [ $# -gt 0 ]; then
  if "$@" > "$dir/output.log" 2>&1; then
    :
  else
    echo "[warn] 命令退出码非 0，完整输出见本文件" >> "$dir/output.log"
  fi
fi
if [ "$shot" = "1" ] && command -v screencapture >/dev/null 2>&1; then
  screencapture -x "$dir/shot-$(date +%H%M%S).png"
fi
echo "$dir"
