#!/usr/bin/env bash
# embedding 随包资产解包 helper（need:202 交付链，三路共用）：
#   unpack-memory-assets.sh <目标根目录> [--required]
# 从仓内 assets/memory-embedding.7z 解出 assets/llama/bin + assets/models 到
# <目标根>/assets/——7z 档案内路径自带 assets\ 前缀（poc 实测 2026-11-04），
# 故 -o 直接指目标根一步落位。stamp 幂等：解包 marker 存在且不旧于 7z 即跳过。
#
# 7z 探测序：DEVDESK_SEVENZIP 环境变量 → C:/Program Files/7-Zip/7z.exe → PATH(7z/7zz)。
# --required：打包链用——7z 缺失 / 解包失败 / 解后 marker 缺失一律 exit 1（静默产出
# 无 embedding 的安装包比打不了包严重，与 mpv 同口径）；缺省（dev 内循环 / 装机）
# 只告警不阻断（运行时 HF 下载与数据根兜底仍在）。
set -u
MODE="warn"
[ "${2:-}" = "--required" ] && MODE="required"
ROOT="${1:?用法: unpack-memory-assets.sh <目标根目录> [--required]}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$REPO_ROOT/assets/memory-embedding.7z"
if [ ! -f "$SRC" ]; then
  echo "⚠ memory-embedding.7z 不在仓内（$SRC）——跳过 embedding 资产解包"
  [ "$MODE" = "required" ] && exit 1
  exit 0
fi

EXE="llama-server"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE="llama-server.exe" ;; esac
MARKER="$ROOT/assets/llama/bin/$EXE"
STAMP="$ROOT/assets/.memory-embedding.unpack-stamp"
# 7z 解包会还原档案内 mtime（早于 7z 文件本身的 mtime），不能用 marker -nt SRC 判
# 新旧——解包成功后 touch stamp（mtime=现在），stamp 存在且不旧于 7z 即幂等跳过
if [ -f "$STAMP" ] && [ "$STAMP" -nt "$SRC" ] && [ -f "$MARKER" ]; then
  exit 0
fi

SEVENZIP="${DEVDESK_SEVENZIP:-}"
if [ -z "$SEVENZIP" ]; then
  for cand in "/c/Program Files/7-Zip/7z.exe" "$(command -v 7z 2>/dev/null || true)" "$(command -v 7zz 2>/dev/null || true)"; do
    if [ -n "$cand" ] && [ -e "$cand" ]; then SEVENZIP="$cand"; break; fi
  done
fi
if [ -z "$SEVENZIP" ]; then
  echo "⚠ 未找到 7z（DEVDESK_SEVENZIP / C:/Program Files/7-Zip/7z.exe / PATH 7z|7zz 全落空）——跳过 embedding 资产解包（运行时走 HF 下载/数据根兜底；打包链须设 DEVDESK_SEVENZIP）"
  [ "$MODE" = "required" ] && exit 1
  exit 0
fi

mkdir -p "$ROOT"
if ! "$SEVENZIP" x -y -o"$ROOT" "$SRC" >/dev/null; then
  echo "⚠ embedding 资产解包失败（$SRC → $ROOT）"
  [ "$MODE" = "required" ] && exit 1
  exit 0
fi
if [ ! -f "$MARKER" ]; then
  echo "⚠ 解包后未见 $MARKER——档案内容异常（缺 llama-server 本体）"
  [ "$MODE" = "required" ] && exit 1
  exit 0
fi
touch "$STAMP"
echo "✅ embedding 随包资产已解包 → $ROOT/assets（llama/bin + models）"
exit 0
