#!/bin/sh
# 项目适配层（薄委托）：证据物归档位置是本项目约定（DevDesk/evidence/，已 git-ignore），
# 采集逻辑由 devflow 插件供给（/Users/coderstory/dev/zcode-plugins/devflow/scripts/evidence-capture.sh）
export EVIDENCE_ROOT="${ZCODE_PROJECT_DIR}"
exec bash /Users/coderstory/dev/zcode-plugins/devflow/scripts/evidence-capture.sh "$@"
