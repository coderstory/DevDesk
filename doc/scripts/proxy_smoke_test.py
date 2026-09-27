#!/usr/bin/env python3
"""DevDesk Claude 代理直测脚本（不经过 Claude Code）。

用途：直接向本地代理（127.0.0.1:<port>）发 /v1/messages 请求，验证
「本地代理 → 上游网关」链路；并可选直连上游做对照，用于定位失败发生在哪一跳。
代理仅监听 127.0.0.1 且**无鉴权**（2026-09-27 起），故代理这一跳不带任何凭据。

用法：
    python doc/scripts/proxy_smoke_test.py              # 全部（代理非流式 + 代理流式 + 直连上游对照）
    python doc/scripts/proxy_smoke_test.py proxy        # 只测本地代理
    python doc/scripts/proxy_smoke_test.py upstream     # 只测直连上游

环境变量覆盖：DEVDESK_PROXY / DEVDESK_UPSTREAM / DEVDESK_MODEL / DEVDESK_TOKEN
（token 默认从 ~/.claude/settings.json 的 env.ANTHROPIC_AUTH_TOKEN 取，仅直连上游时用）

退出码：0 = 被测项全部非 5xx / 非网络错；1 = 有失败项。
"""
import json
import os
import sys
import time
import urllib.error
import urllib.request

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PROXY = os.environ.get("DEVDESK_PROXY", "http://127.0.0.1:8888")
UPSTREAM = os.environ.get(
    "DEVDESK_UPSTREAM", "https://codingxrui.geely-test.com/bailian/anthropic"
)
MODEL = os.environ.get("DEVDESK_MODEL", "bailian/glm-5.2[1M]")

_skipped = []


def _token():
    if os.environ.get("DEVDESK_TOKEN"):
        return os.environ["DEVDESK_TOKEN"]
    p = os.path.expanduser("~/.claude/settings.json")
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f).get("env", {}).get("ANTHROPIC_AUTH_TOKEN", "")
    except Exception:
        return ""


def _body(stream):
    return json.dumps(
        {
            "model": MODEL,
            "max_tokens": 32,
            "messages": [{"role": "user", "content": "只回复一个字：好"}],
            "stream": stream,
        }
    ).encode("utf-8")


def call(url, stream, key):
    headers = {
        "content-type": "application/json",
        "anthropic-version": "2023-06-01",
        "accept": "text/event-stream" if stream else "application/json",
    }
    if key:
        headers["x-api-key"] = key
        headers["authorization"] = "Bearer " + key
    req = urllib.request.Request(url, data=_body(stream), headers=headers, method="POST")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=90) as r:
            return "OK", r.status, (time.time() - t0) * 1000, r.read()
    except urllib.error.HTTPError as e:
        return "HTTP_ERR", e.code, (time.time() - t0) * 1000, e.read()
    except Exception as e:  # 连接被拒 / DNS / 超时 / TLS
        return "NET_ERR", None, (time.time() - t0) * 1000, repr(e).encode("utf-8", "replace")


def show(tag, kind, code, ms, raw, is_stream):
    print(f"--- {tag} ---")
    print(f"  {kind} status={code} {ms:.0f}ms bytes={len(raw)}")
    txt = raw.decode("utf-8", "replace")
    if is_stream:
        data_lines = [l for l in txt.splitlines() if l.startswith("data: ")]
        for line in data_lines[:3]:
            print("  " + line[:220])
        if data_lines:
            print(f"  (SSE data 行 {len(data_lines)} 条)")
        else:
            print("  " + txt[:400].replace("\n", "\n  "))
    else:
        print("  " + txt[:700].replace("\n", "\n  "))
    print()
    return kind == "OK" and (code is None or code < 500)


which = sys.argv[1] if len(sys.argv) > 1 else "all"
results = []

if which in ("all", "proxy"):
    print(f"[本地代理] url={PROXY}（无鉴权，不带凭据；真实上游 key 由代理注入）")
    results.append(show("本地代理 · 非流式", *call(PROXY + "/v1/messages", False, ""), is_stream=False))
    results.append(show("本地代理 · 流式", *call(PROXY + "/v1/messages", True, ""), is_stream=True))

if which in ("all", "upstream"):
    tk = _token()
    print(f"[直连上游对照] url={UPSTREAM} token={'有(' + tk[:12] + '...)' if tk else '无'}")
    results.append(show("直连上游 · 非流式", *call(UPSTREAM + "/v1/messages", False, tk), is_stream=False))

print("结论：" + ("全部通过" if all(results) else f"有 {results.count(False)} 项失败"))
sys.exit(0 if all(results) else 1)
