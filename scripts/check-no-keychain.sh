#!/usr/bin/env bash
# scripts/check-no-keychain.sh
# keychain 编译硬禁令（need327 老板令 2026-11-18「彻底删除」终局守卫）：
#   src/ 与 tests/ 里出现任何 keychain 引用（代码符号/头文件/注释口径）即 exit 1，
#   build-test-run.sh 在构建前置门调用——**先于 ninja**，命中即整个构建不开始。
#
# 背景：keychain 已全链退役（need#317 删平台五文件+测试+脚手架；need#304/#305
# 三处切 app_secrets DB 专表）。本守卫防「删了又冒」（历史教训：memory 闭环批
# 曾把 LLM key 通道接回 keychain，2026-11-18 老板裁定全禁）。新凭据一律走
# core/secrets_store（app_secrets 表，决策#11 口径）。
#
# 判据：grep -ri 'keychain' 于 src/ tests/（大小写不敏感）。零白名单——历史口径
# 注释也必须改写为 DB 表述；真需要提这个词的场景（如本守卫注释自身）只在
# scripts/ 下，不在受检目录。
# node_modules 例外：tests/e2e/ 的 npm 依赖（Playwright）是第三方产物，不是本仓
# 源码，且其内部启动参数含该词；npm ci 后不应把构建门禁卡成红。
#
# 本脚本全程绝对路径、不读 ${PWD}、脚本内不 cd（对齐 check-module-isolation.sh）。
# 退出码：0 通过 / 1 违规（清单到 stdout）/ 2 环境错（仓库根找不到）。

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [ ! -d "${REPO_ROOT}/src" ] || [ ! -d "${REPO_ROOT}/tests" ]; then
  echo "❌ keychain 禁令守卫：仓库根定位失败（${REPO_ROOT}）"
  exit 2
fi

# 排除构建产物与第三方（tests/ 下无 build，稳妥起见仍显式排除）
HITS="$(grep -rin --exclude-dir=build --exclude-dir=build-release \
        --exclude-dir=.git --exclude-dir=node_modules 'keychain' \
        "${REPO_ROOT}/src" "${REPO_ROOT}/tests" 2>/dev/null || true)"

if [ -n "${HITS}" ]; then
  echo "❌ keychain 禁令守卫：keychain 已全链退役（need327 老板令 2026-11-18），以下引用禁止出现："
  echo "${HITS}" | head -40
  COUNT="$(echo "${HITS}" | wc -l | tr -d ' ')"
  echo "   共 ${COUNT} 处。新凭据一律走 core/secrets_store（app_secrets 表）；历史口径注释改写为 DB 表述。"
  exit 1
fi

echo "✅ keychain 禁令守卫通过（src/ tests/ 零引用）"
exit 0
