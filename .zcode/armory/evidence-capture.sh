#!/bin/sh
# 项目适配层（薄委托）：证据物归档位置是本项目约定（DevDesk/evidence/，已 git-ignore），
# 采集逻辑由 devflow 插件供给（各机 clone 的 ~/dev/zcode-plugins/devflow/scripts/evidence-capture.sh）
export EVIDENCE_ROOT="${ZCODE_PROJECT_DIR}"
src="$HOME/dev/zcode-plugins/devflow/scripts/evidence-capture.sh"
if [ ! -f "$src" ]; then
  echo "[wrapper 断链] 找不到 $src —— 插件源码被移动 / 缓存被清理，重跑 devflow-setup 重新生成本 wrapper" >&2
  exit 2
fi
exec bash "$src" "$@"
