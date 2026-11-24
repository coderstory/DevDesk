#!/usr/bin/env bash
# 网页终端 E2E 入口（need#490）：CI 与本机共用。
# 默认跑回环主链路（web_terminal_e2e.mjs）；--auth 跑非回环鉴权双路径（auth_e2e.mjs）。
# 其余参数（如 --bin）原样透传给 node。
# 依赖/浏览器已就位时可 DEVDESK_E2E_SKIP_INSTALL=1 跳过重复安装。
set -euo pipefail
cd "$(dirname "$0")"
if [ "${DEVDESK_E2E_SKIP_INSTALL:-0}" != "1" ]; then
  npm ci
  npx playwright install chromium webkit
fi
script="web_terminal_e2e.mjs"
args=()
for a in "$@"; do
  case "$a" in
    --auth) script="auth_e2e.mjs" ;;
    --loopback) script="web_terminal_e2e.mjs" ;;
    *) args+=("$a") ;;
  esac
done
exec node "$script" ${args[@]+"${args[@]}"}
