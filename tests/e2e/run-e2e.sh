#!/usr/bin/env bash
# 网页终端 E2E 入口（need#490 W7）：CI 与本机共用。
# 依赖/浏览器已就位时可 DEVDESK_E2E_SKIP_INSTALL=1 跳过重复安装。
set -euo pipefail
cd "$(dirname "$0")"
if [ "${DEVDESK_E2E_SKIP_INSTALL:-0}" != "1" ]; then
  npm ci
  npx playwright install chromium webkit
fi
exec node web_terminal_e2e.mjs "$@"
