#!/usr/bin/env bash
# 智能体（module_agent）**真机全场景验收**的唯一显式入口：把「tests/live/agent_live.cpp
# 到底怎么跑」变成一条命令。
#
#     bash scripts/run-agent-live.sh                              # 跑全部 15 个场景
#     bash scripts/run-agent-live.sh A1_todo_6tools C_cron_trigger # 只跑指定场景
#     bash scripts/run-agent-live.sh <buildDir> A1_todo_6tools     # 指定构建目录
#     (cd build && ctest -L live)                                  # ctest 入口
#
# **绝不进默认门禁**：真网络 + 真激活 profile 的真 key —— CI 与离线环境必红。
#   · 本脚本不被 scripts/build-test-run.sh 调用（那里只跑 devdesk_tests 与
#     `ctest -L probe`）。
#   · ctest 条目的 LABELS 是 **live**、不含 **probe** → `ctest -L probe` 看不到它。
#
# 前置：驱动二进制要先编出来（DEVDESK_BUILD_LIVE_TOOLS 默认 OFF，显式开）：
#     cmake -B build ... -DDEVDESK_BUILD_LIVE_TOOLS=ON
#     cmake --build build --target agent_live
#
# 凭据红线：驱动进程自己从用户数据根的**只读副本**解析激活 profile 与 key（副本用完
# 即删）；本脚本不读、不传、不回显任何 key，也不 `set -x`。
# 隔离红线：驱动进程内自建临时 DEVDESK_DATA_ROOT（用完删），不碰用户真实库，不启
# GUI，不绑端口。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${DEVDESK_BUILD_ROOT:-${REPO_ROOT}/build}"
if [ "$#" -gt 0 ] && [ -d "$1" ]; then
    BUILD_DIR="$(cd "$1" && pwd)"
    shift
fi

DRIVER="${BUILD_DIR}/tests/agent_live"

if [ ! -x "${DRIVER}" ]; then
    echo "❌ 找不到驱动二进制：${DRIVER}" >&2
    echo "   先开开关重配再构建（目标挂 DEVDESK_BUILD_LIVE_TOOLS，默认 OFF）：" >&2
    echo "     cmake -B ${BUILD_DIR} -DDEVDESK_BUILD_LIVE_TOOLS=ON" >&2
    echo "     cmake --build ${BUILD_DIR} --target agent_live" >&2
    exit 2
fi

# 环境门：驱动侧要求 DEVDESK_AGENT_LIVE=1，缺了直接拒跑（防生产误用）
export DEVDESK_AGENT_LIVE=1

echo "== 智能体真机全场景验收 =="
echo "  build  = ${BUILD_DIR}"
echo "  driver = ${DRIVER}"
echo "  场景   = $*"

set +e
"${DRIVER}" "$@"
rc=$?
set -e

exit "${rc}"
