#!/usr/bin/env bash
# GitHub Sync **真机验收**的唯一显式入口：把「tests/live/github_sync_live.cpp
# 到底怎么跑」变成一条命令。
#
#     bash scripts/run-ghsync-live.sh                       # 跑全部 15 个场景
#     bash scripts/run-ghsync-live.sh init upload download  # 只跑指定场景
#     bash scripts/run-ghsync-live.sh <buildDir> init       # 指定构建目录（默认 <repo>/build）
#     (cd build && ctest -L live)                           # ctest 入口
#
# **绝不进默认门禁**：真网络 + 真 PAT + 真远端仓 —— CI 与离线环境必红。
#   · 本脚本不被 scripts/build-test-run.sh 调用（那里只跑 devdesk_tests 与
#     `ctest -L probe`）。
#   · ctest 条目的 LABELS 是 **live**、不含 **probe** → `ctest -L probe` 看不到它。
#
# 前置：驱动二进制要先编出来（默认 OFF，显式开）：
#     cmake -B build ... -DDEVDESK_BUILD_LIVE_TOOLS=ON && cmake --build build
#
# 凭据红线：token 只从 0600 的 /tmp/devdesk-gh-test.pat（或已设的
# DEVDESK_GITHUB_TOKEN）读进 shell 变量，再经环境变量传给子进程 —— 绝不 echo /
# 不进 argv / 不写任何仓库文件。本脚本不 `set -x`。
#
# 隔离红线：每个场景一个**全新临时数据根**（DEVDESK_DATA_ROOT），跑完即删；
# 绝不碰用户真实数据根，绝不启 GUI、绝不绑端口。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${DEVDESK_BUILD_ROOT:-${REPO_ROOT}/build}"
if [ "$#" -gt 0 ] && [ -d "$1" ]; then
    BUILD_DIR="$(cd "$1" && pwd)"
    shift
fi

DRIVER="${BUILD_DIR}/tests/github_sync_live"
FIXTURE="${SCRIPT_DIR}/ghsync-live-fixture.sh"
PAT_FILE="/tmp/devdesk-gh-test.pat"

if [ ! -x "${DRIVER}" ]; then
    echo "❌ 找不到驱动二进制：${DRIVER}" >&2
    echo "   先开开关重配再构建：" >&2
    echo "     cmake -B ${BUILD_DIR} -DDEVDESK_BUILD_LIVE_TOOLS=ON && cmake --build ${BUILD_DIR} --target github_sync_live" >&2
    exit 2
fi

# token 读进变量，绝不回显
if [ -z "${DEVDESK_GITHUB_TOKEN:-}" ]; then
    if [ -r "${PAT_FILE}" ]; then
        DEVDESK_GITHUB_TOKEN="$(cat "${PAT_FILE}")"
    else
        echo "❌ 无 token：设 DEVDESK_GITHUB_TOKEN 或提供 0600 的 ${PAT_FILE}" >&2
        exit 2
    fi
fi
export DEVDESK_GITHUB_TOKEN

export DEVDESK_GITHUB_TEST_MODE=1        # 测试门：不开驱动直接拒跑（防生产误用）
export DEVDESK_GH_FIXTURE="${FIXTURE}"

ALL_SCENARIOS=(init upload download push-conflict pull-conflict
               branch-missing branch-exists branch-deleted
               repo-missing repo-deleted no-permission corrupt-db
               git-available git-unavailable git-history)

if [ "$#" -gt 0 ]; then
    SCENARIOS=("$@")
else
    SCENARIOS=("${ALL_SCENARIOS[@]}")
fi

TMPBASE="$(mktemp -d -t ghsync-live.XXXXXX)"
cleanup() { rm -rf "${TMPBASE}"; }
trap cleanup EXIT

echo "== GitHub Sync 真机验收 =="
echo "  build   = ${BUILD_DIR}"
echo "  driver  = ${DRIVER}"
echo "  fixture = ${FIXTURE}"
echo "  tmpbase = ${TMPBASE}"
echo "  场景数  = ${#SCENARIOS[@]}"

failed=0
pass=0
for sc in "${SCENARIOS[@]}"; do
    root="${TMPBASE}/${sc}"
    mkdir -p "${root}"
    set +e
    DEVDESK_DATA_ROOT="${root}" "${DRIVER}" "${sc}"
    rc=$?
    set -e
    if [ "${rc}" -eq 0 ]; then
        pass=$((pass + 1))
    else
        failed=$((failed + 1))
    fi
done

echo
echo "==== 驱动汇总：通过 ${pass} / 失败 ${failed}（共 ${#SCENARIOS[@]}）===="
[ "${failed}" -eq 0 ]
