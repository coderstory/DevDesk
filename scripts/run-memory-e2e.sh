#!/usr/bin/env bash
# scripts/run-memory-e2e.sh —— 记忆模块 e2e 一键回归
# 配方权威记录：docs/research/2026-11-03-记忆模块实测报告.md §2（2026-11-03 全程实操跑通）
# 链路：临时数据根 + 伪造会话(3 jsonl) → 起 llama-server(:11435)+telemem(:8421) → 探活 →
#       memory_e2e_probe(39 断言) → hook 真二进制三问(改写×2 命中 + 负对照 0 注入) →
#       fail-open(杀 telemem 复跑 = stdout 空 + exit 0) → 红线 grep → 清场
# 用法：bash scripts/run-memory-e2e.sh [--keep]
#   --keep  保留临时数据根供事后检查（引擎仍会清场）
# 前置（缺任一直接报错退出，本脚本不自动构建）：
#   build/tests/memory_e2e_probe   缺失 → bash scripts/build-test-run.sh 增量构建
#   build/DevDesk                  （hook 真二进制）
#   /opt/homebrew/bin/llama-server （Homebrew llama.cpp，embedding 引擎）
#   ~/DevDesk/memory/venv          （telemem 引擎 venv，含 telemem==1.10.1）
#   ~/DevDesk/memory/models/bge-small-zh-v1.5-Q8_0.gguf
# 路径可用环境变量覆盖：LLAMA_SERVER_BIN / TELEMEM_PY / GGUF_MODEL / DEVDESK_BIN / PROBE_BIN
# TODO(Windows): 另一机验收时补 Windows 分支（各路径换 %USERPROFILE%\DevDesk 口径，探活同为 curl）。
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

LLAMA_SERVER_BIN="${LLAMA_SERVER_BIN:-/opt/homebrew/bin/llama-server}"
TELEMEM_PY="${TELEMEM_PY:-$HOME/DevDesk/memory/venv/bin/python}"
GGUF_MODEL="${GGUF_MODEL:-$HOME/DevDesk/memory/models/bge-small-zh-v1.5-Q8_0.gguf}"
DEVDESK_BIN="${DEVDESK_BIN:-$REPO_ROOT/build/DevDesk}"
PROBE_BIN="${PROBE_BIN:-$REPO_ROOT/build/tests/memory_e2e_probe}"

LLAMA_PORT=11435
TELEMEM_PORT=8421
DUMMY_KEY="sk-devdesk-e2e-dummy-key-not-real-000000"
HEALTH_TIMEOUT_S=120

EVIDENCE_DIR="$REPO_ROOT/evidence/$(date +%F)-memory-e2e"
mkdir -p "$EVIDENCE_DIR"
exec > >(tee "$EVIDENCE_DIR/run.log") 2>&1

TEMP_ROOT="$(mktemp -d /tmp/mem-e2e.XXXXXX)"
DATA_ROOT="$TEMP_ROOT/data"
PROJ_ROOT="$TEMP_ROOT/cc-projects"
mkdir -p "$DATA_ROOT/memory" "$PROJ_ROOT"   # memory/ 目录是探针前置假设

LLAMA_PID=""
TELEMEM_PID=""
FAILS=0

log()  { echo "[e2e-run] $*"; }
pass() { echo "[e2e-run] PASS: $*"; }
fail() { echo "[e2e-run] FAIL: $*"; FAILS=$((FAILS + 1)); }

cleanup() {
  [ -n "$LLAMA_PID" ] && kill "$LLAMA_PID" 2>/dev/null
  [ -n "$TELEMEM_PID" ] && kill "$TELEMEM_PID" 2>/dev/null
  if [ "$KEEP" = "1" ]; then
    log "保留现场：${TEMP_ROOT}（引擎已清场）"
  else
    rm -rf "$TEMP_ROOT"
  fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM

# ---------- 0. 前置 ----------
log "临时数据根：$TEMP_ROOT"
for f in "$PROBE_BIN" "$DEVDESK_BIN" "$LLAMA_SERVER_BIN" "$TELEMEM_PY" "$GGUF_MODEL"; do
  if [ ! -e "$f" ]; then
    fail "前置缺失：${f}（probe/DevDesk 缺失先 bash scripts/build-test-run.sh 增量构建；本脚本不自动全量构建）"
  fi
done
for p in "$LLAMA_PORT" "$TELEMEM_PORT"; do
  if lsof -nP -iTCP:"$p" -sTCP:LISTEN | grep -q LISTEN; then
    fail "端口 $p 已被占用（上次运行残留？先清场再跑）"
  fi
done

# ---------- 1. 伪造数据（内容 = 报告 §2 现场口径，探针硬断言依赖：session-a/b 前缀、恰 3 段、恰 1 段含敏感） ----------
cat > "$PROJ_ROOT/session-a-build.jsonl" <<'EOF'
{"type":"user","sessionId":"session-a-build","message":{"role":"user","content":"DevDesk 改了几个文件后怎么快速验证？全量跑太慢了。"}}
{"type":"assistant","sessionId":"session-a-build","message":{"role":"assistant","content":[{"type":"text","text":"开发内循环用增量构建：一律走仓库根的 scripts/build-test-run.sh --dev，配合 --filter 只跑改动相关的用例，不要手敲 cmake。增量构建比全量快一个量级；全量 doctest 只在阶段节点跑。并行度用 DEVDESK_JOBS 控制，本机封顶 DEVDESK_JOBS=3。"}]}}
{"type":"user","sessionId":"session-a-build","message":{"role":"user","content":"测试二进制想只跑某个文件的用例怎么办？"}}
{"type":"assistant","sessionId":"session-a-build","message":{"role":"assistant","content":[{"type":"text","text":"--filter 支持 file: 前缀按源文件圈定，也支持描述子串。注意直跑二进制只认裸词 unique_name，通配符是 0 例。"}]}}
EOF
cat > "$PROJ_ROOT/session-b-telemem.jsonl" <<'EOF'
{"type":"user","sessionId":"session-b-telemem","message":{"role":"user","content":"记忆模块的引擎组件和端口是什么？"}}
{"type":"assistant","sessionId":"session-b-telemem","message":{"role":"assistant","content":[{"type":"text","text":"telemem 1.10.1 提供 MCP 服务绑定回环 8421；向量化是本机 llama-server 跑 bge-small-zh-v1.5 的 Q8_0 量化模型，512 维，端口 11435。向量库 faiss 落在数据根 memory/faiss_db。"}]}}
{"type":"user","sessionId":"session-b-telemem","message":{"role":"user","content":"hook 注入有什么预算限制？"}}
{"type":"assistant","sessionId":"session-b-telemem","message":{"role":"assistant","content":[{"type":"text","text":"UserPromptSubmit 时最多注入 5 条、合计 500 字；引擎不可用时放行不阻断提问（fail-open）。"}]}}
EOF
cat > "$PROJ_ROOT/session-c-secret.jsonl" <<'EOF'
{"type":"user","sessionId":"session-c-secret","message":{"role":"user","content":"帮我记一下测试环境信息。"}}
{"type":"assistant","sessionId":"session-c-secret","message":{"role":"assistant","content":[{"type":"text","text":"测试环境：联系人手机号 13800138000，API 密钥 sk-e2eFakeKey000111222333444，内网服务 10.20.30.40:8080。"}]}}
EOF
pass "伪造 3 会话写入 $PROJ_ROOT"

# ---------- 2. 环境 yaml（faiss/history 指向本次临时数据根，禁复用旧库） ----------
cat > "$TEMP_ROOT/telemem-config.yaml" <<EOF
llm:
  provider: anthropic
  config:
    anthropic_base_url: https://open.bigmodel.cn/api/anthropic
    model: glm-5.3-flash
    api_key: ""
embedder:
  provider: openai
  config:
    openai_base_url: http://127.0.0.1:$LLAMA_PORT/v1
    model: bge-small-zh-v1.5
    api_key: "local"
    embedding_dims: 512
vector_store:
  provider: faiss
  config:
    path: $DATA_ROOT/memory/faiss_db
    embedding_model_dims: 512
history_db_path: $DATA_ROOT/memory/history.db
EOF
pass "环境 yaml 写入 $TEMP_ROOT/telemem-config.yaml"

# ---------- 3. 起引擎 + 探活 ----------
log "启动 llama-server (:${LLAMA_PORT})…"
nohup "$LLAMA_SERVER_BIN" --host 127.0.0.1 --port "$LLAMA_PORT" \
  -m "$GGUF_MODEL" --embeddings \
  > "$EVIDENCE_DIR/llama-server.log" 2>&1 &
LLAMA_PID=$!
log "启动 telemem (:${TELEMEM_PORT})…"
TELEMEM_CONFIG="$TEMP_ROOT/telemem-config.yaml" ANTHROPIC_API_KEY="$DUMMY_KEY" \
  nohup "$TELEMEM_PY" -m telemem.mcp --transport streamable-http \
  --host 127.0.0.1 --port "$TELEMEM_PORT" \
  > "$EVIDENCE_DIR/telemem.log" 2>&1 &
TELEMEM_PID=$!

ok=0
for _ in $(seq 1 "$HEALTH_TIMEOUT_S"); do
  code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$LLAMA_PORT/health" || true)
  [ "$code" = "200" ] && { ok=1; break; }
  sleep 1
done
[ "$ok" = 1 ] && pass "llama-server 探活 /health=200 (:${LLAMA_PORT})" \
  || fail "llama-server ${HEALTH_TIMEOUT_S}s 未探活（详见 $EVIDENCE_DIR/llama-server.log）"

MCP_INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"devdesk-e2e","version":"1.0"}}}'
ok=0
for _ in $(seq 1 "$HEALTH_TIMEOUT_S"); do
  code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$TELEMEM_PORT/mcp" \
    -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
    -d "$MCP_INIT" || true)
  [ "$code" = "200" ] && { ok=1; break; }
  sleep 1
done
[ "$ok" = 1 ] && pass "telemem 探活 initialize=200 (:${TELEMEM_PORT})" \
  || fail "telemem ${HEALTH_TIMEOUT_S}s 未探活（详见 $EVIDENCE_DIR/telemem.log）"

# ---------- 4. 探针（S3 采集脱敏 → S4 入库检索 → S5 hook 契约 → S6 冻结评测 → S7 播种） ----------
DEVDESK_DATA_ROOT="$DATA_ROOT" "$PROBE_BIN" --projects "$TEMP_ROOT" \
  > "$EVIDENCE_DIR/probe.log" 2>&1
probe_rc=$?
if [ "$probe_rc" = 0 ] && ! grep -q "\[e2e\] FAIL" "$EVIDENCE_DIR/probe.log"; then
  pass "探针全绿（$EVIDENCE_DIR/probe.log）"
else
  fail "探针 rc=$probe_rc 或有 FAIL（详见 $EVIDENCE_DIR/probe.log）"
fi

# ---------- 5. hook 真二进制三问 ----------
hook_ask() { # $1=stdin json $2=stdout 文件 $3=stderr 文件；回显退出码
  DEVDESK_DATA_ROOT="$DATA_ROOT" "$DEVDESK_BIN" memory hook < "$1" > "$2" 2> "$3"
  echo $?
}
make_prompt() { # $1=prompt $2=输出文件
  printf '{"session_id":"uat","hook_event_name":"UserPromptSubmit","prompt":"%s"}' "$1" > "$2"
}

make_prompt "改完代码怎么验证" "$TEMP_ROOT/hook-q-build.json"
rc=$(hook_ask "$TEMP_ROOT/hook-q-build.json" \
  "$EVIDENCE_DIR/hook-para-build.json" "$EVIDENCE_DIR/hook-para-build.err")
if [ "$rc" = 0 ] && grep -q "以下是长期记忆" "$EVIDENCE_DIR/hook-para-build.json"; then
  pass "hook 改写问①「改完代码怎么验证」命中注入"
else
  fail "hook 改写问①未命中注入（rc=${rc}，见 $EVIDENCE_DIR/hook-para-build.json）"
fi

make_prompt "记忆服务监听哪个端口" "$TEMP_ROOT/hook-q-port.json"
rc=$(hook_ask "$TEMP_ROOT/hook-q-port.json" \
  "$EVIDENCE_DIR/hook-para-port.json" "$EVIDENCE_DIR/hook-para-port.err")
if [ "$rc" = 0 ] && grep -q "以下是长期记忆" "$EVIDENCE_DIR/hook-para-port.json"; then
  pass "hook 改写问②「记忆服务监听哪个端口」命中注入"
else
  fail "hook 改写问②未命中注入（rc=${rc}，见 $EVIDENCE_DIR/hook-para-port.json）"
fi

make_prompt "今天天气怎么样" "$TEMP_ROOT/hook-q-neg.json"
rc=$(hook_ask "$TEMP_ROOT/hook-q-neg.json" \
  "$EVIDENCE_DIR/hook-neg-control.json" "$EVIDENCE_DIR/hook-neg-control.err")
if [ "$rc" = 0 ] && [ ! -s "$EVIDENCE_DIR/hook-neg-control.json" ]; then
  pass "hook 负对照「今天天气怎么样」0 注入（threshold 生效）"
else
  fail "hook 负对照非 0 注入 → 回归（rc=${rc}，见 $EVIDENCE_DIR/hook-neg-control.json）"
fi

# ---------- 6. fail-open（杀 telemem 复跑） ----------
kill "$TELEMEM_PID" 2>/dev/null; TELEMEM_PID=""
sleep 1
rc=$(hook_ask "$TEMP_ROOT/hook-q-build.json" \
  "$EVIDENCE_DIR/hook-failopen.json" "$EVIDENCE_DIR/hook-failopen.err")
if [ "$rc" = 0 ] && [ ! -s "$EVIDENCE_DIR/hook-failopen.json" ]; then
  pass "fail-open：引擎停后 stdout 空 + exit 0"
else
  fail "fail-open 违约（rc=${rc}，见 $EVIDENCE_DIR/hook-failopen.json）"
fi

# ---------- 7. 红线 grep（数据根全产物不得出现敏感原文） ----------
redline_ok=1
for v in 13800138000 sk-e2eFakeKey000111222333444 10.20.30.40; do
  if grep -raq "$v" "$DATA_ROOT"; then
    fail "红线命中：敏感原文 $v 出现在数据根产物"
    redline_ok=0
  fi
done
[ "$redline_ok" = 1 ] && pass "红线 grep 零命中（phone/secret/ipv4）"

# ---------- 8. 汇总 ----------
log "————————————————————"
if [ "$FAILS" -eq 0 ]; then
  log "结论：全部 PASS（evidence: ${EVIDENCE_DIR}）"
  exit 0
fi
log "结论：$FAILS 项 FAIL（evidence: ${EVIDENCE_DIR}）"
exit 1
