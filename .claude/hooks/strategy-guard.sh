#!/usr/bin/env bash
# strategy-guard.sh —— 战略基线守卫（PostToolUse Edit|Write 触发）
# 作用：守卫范围内的基线文件（docs/strategy、docs/adr、docs/superpowers、GLOSSARY.md、CLAUDE.md）
#       被改动后，立即全量回归 docs/strategy/cumulative-markers.txt；历史轮次标记有丢失 → 退出码 2，
#       缺失清单反馈给 Claude，强制先处理（恢复内容，或确认有意废弃后同步更新清单并在回复里说明依据）。
# 为什么命中时静默、只有丢东西才说话：提醒常响 = 提醒失效，守卫只在异常时发声。
# 依赖：只用 sed/grep/bash，不用 jq——hook 环境不保证用户 PATH 里有 jq，缺了会静默变哑守卫。
# fail-soft：清单或核对脚本不存在（如 worktree 未含未提交基线）→ stderr 警告 + 退出 0，不阻塞。
set -u
input="$(cat)"
f="$(printf '%s' "$input" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
[ -z "$f" ] && exit 0
f="${f//\\//}"
case "$f" in
  *"/docs/strategy/"*|*"/docs/adr/"*|*"/docs/superpowers/"*|*"/GLOSSARY.md"|*"/CLAUDE.md") ;;
  *) exit 0 ;;
esac
ROOT="${CLAUDE_PROJECT_DIR:-$(pwd)}"
check="$ROOT/scripts/check-cumulative-markers.sh"
list="$ROOT/docs/strategy/cumulative-markers.txt"
if [ ! -f "$check" ] || [ ! -f "$list" ]; then
  echo "[strategy-guard] 清单或核对脚本不存在，跳过（worktree 未含基线时属正常）: $ROOT" >&2
  exit 0
fi
out="$("$check" "$list" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ]; then
  echo "[strategy-guard] 累积核对失败——本次改动挤掉了历史轮次的标记。先恢复内容；确认有意废弃则同步更新清单（删对应行）并在回复里说明依据：" >&2
  printf '%s\n' "$out" | grep '^\[缺失\]' >&2
  exit 2
fi
exit 0
