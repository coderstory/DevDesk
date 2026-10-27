#!/usr/bin/env bash
# scripts/run-mcp-live.sh
# B4-03：真实 Claude Code → DevDesk MCP 实连脚本。
#
# 前置：cmake -B build -DDEVDESK_BUILD_LIVE_TOOLS=ON 后 build/tests/mcp_live(.exe)
# 已构建（mcp_live --serve 提供进程内 MCP 服务）。
# 用法：bash scripts/run-mcp-live.sh [目标URL]
#   目标 URL 默认 https://www.iana.org/domains/reserved（tester 实测：标记句在此页真实存在且现有 CA 策略可抓通；example.com 现行文案已不含该句），断言标记固定 illustrative examples。
# 口径：不设 CLAUDE_CONFIG_DIR、不碰用户 ~/.claude/**；临时配置与空 cwd 全部
# 走 mktemp -d 并由 trap 清理；服务进程只按本脚本拉起的 PID 收尾。
set -u

TARGET_URL="${1:-https://www.iana.org/domains/reserved}"
EXPECTED_MARKER="illustrative examples"
SERVE_WALL_CLOCK_SECS=240   # 外层墙钟兜底（claude 卡死时不无限等）

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/tests/mcp_live"
if [ ! -x "$BIN" ] && [ -x "$BIN.exe" ]; then
    BIN="$BIN.exe"
fi
if [ ! -x "$BIN" ]; then
    echo "[run-mcp-live] 找不到 $BIN —— 先构建：cmake -B build -DDEVDESK_BUILD_LIVE_TOOLS=ON && cmake --build build --target mcp_live" >&2
    exit 2
fi
if ! command -v claude >/dev/null 2>&1; then
    echo "[run-mcp-live] 找不到 claude CLI —— 实连需要真实 Claude Code" >&2
    exit 2
fi

CFGDIR="$(mktemp -d)"
WORKDIR="$(mktemp -d)"
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$CFGDIR" "$WORKDIR"
}
trap cleanup EXIT

# ── 起 MCP 服务（--serve 打印 PORT= 后阻塞）─────────────────────────
"$BIN" --serve >"$CFGDIR/serve.log" 2>&1 &
SERVER_PID=$!

PORT=""
for _ in $(seq 1 50); do
    PORT="$(sed -n 's/^PORT=//p' "$CFGDIR/serve.log" | head -n1)"
    [ -n "$PORT" ] && break
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[run-mcp-live] --serve 提前退出：" >&2
        cat "$CFGDIR/serve.log" >&2
        exit 1
    fi
    sleep 0.2
done
if [ -z "$PORT" ]; then
    echo "[run-mcp-live] 15s 内未解析到 PORT= ——" >&2
    cat "$CFGDIR/serve.log" >&2
    exit 1
fi
echo "[run-mcp-live] MCP 服务就绪：http://127.0.0.1:${PORT}/mcp (pid $SERVER_PID)"

# ── 临时 MCP 配置：server key 固定 devdesk（与 --allowedTools 对应）──
cat > "$CFGDIR/mcp.json" <<MCPJSON
{
  "mcpServers": {
    "devdesk": {
      "type": "http",
      "url": "http://127.0.0.1:${PORT}/mcp"
    }
  }
}
MCPJSON

# 显式导出超时（claude CLI 口径）；刻意不设 CLAUDE_CONFIG_DIR，不碰用户配置
export MCP_TIMEOUT=30000
export MCP_TOOL_TIMEOUT=120000

PROMPT="Use the mcp__devdesk__web_fetch_markdown tool to fetch ${TARGET_URL}, then print verbatim the sentence on the page that contains \"${EXPECTED_MARKER}\"."

# 空 cwd 运行（防 claude 读到仓库内配置）；墙钟兜底防卡死
(
    cd "$WORKDIR" || exit 1
    if command -v timeout >/dev/null 2>&1; then
        timeout "$SERVE_WALL_CLOCK_SECS" claude -p \
            --strict-mcp-config \
            --mcp-config "$CFGDIR/mcp.json" \
            --allowedTools mcp__devdesk__web_fetch_markdown \
            --output-format text \
            "$PROMPT"
    else
        claude -p \
            --strict-mcp-config \
            --mcp-config "$CFGDIR/mcp.json" \
            --allowedTools mcp__devdesk__web_fetch_markdown \
            --output-format text \
            "$PROMPT"
    fi
) >"$CFGDIR/out.txt" 2>&1
CLAUDE_RC=$?

echo "[run-mcp-live] claude 退出码 $CLAUDE_RC，输出："
echo "----------------------------------------"
cat "$CFGDIR/out.txt"
echo "----------------------------------------"

if [ "$CLAUDE_RC" -ne 0 ]; then
    echo "[run-mcp-live] FAIL：claude 非零退出（超时/连接失败）" >&2
    exit 1
fi
if grep -q "$EXPECTED_MARKER" "$CFGDIR/out.txt"; then
    echo "[run-mcp-live] PASS：输出含标记「${EXPECTED_MARKER}」（目标 ${TARGET_URL}）"
    exit 0
fi
echo "[run-mcp-live] FAIL：输出不含标记「${EXPECTED_MARKER}」" >&2
exit 1
