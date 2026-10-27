"""memcpp S1 spike：telemem 五项实测（可复跑）。

对应 docs/superpowers/specs/2026-10-27-memcpp-integration-spec.md §3 S1 表：
  1. Windows pip 安装（安装动作由脚本外执行，`install-check` 验证产物）
  2. 常驻内存 RSS（`rss`：回环 8421 拉起 + 懒加载 + 工具级报错契约探针）
  3. 注入延迟 p50/p95（`latency`：MCP HTTP tools/call 全链路，含一次 embedding）
  4. 冻结集转测 Recall@5（`eval`：27 条 infer=False 直存 + 15 条查询，只读 memory-lab 资产）
  5. 端点只读发现（`discover`：环境变量名扫描 + 本机端口探测，绝不输出凭据值）

端点凭据一律经环境变量注入（本脚本不落盘任何密钥；生成的配置写在 .runtime/，已 gitignore）：
  TELEMEM_SPIKE_LLM_BASE_URL   如 https://gw.example.com/v1
  TELEMEM_SPIKE_LLM_API_KEY
  TELEMEM_SPIKE_LLM_MODEL      提炼模型名
  TELEMEM_SPIKE_EMBED_BASE_URL / TELEMEM_SPIKE_EMBED_API_KEY / TELEMEM_SPIKE_EMBED_MODEL
  TELEMEM_SPIKE_PORT           可选，默认 8421

用法（在仓库 worktree 内）：
  spike/.venv/Scripts/python.exe run_spike.py install-check
  spike/.venv/Scripts/python.exe run_spike.py discover
  spike/.venv/Scripts/python.exe run_spike.py rss
  spike/.venv/Scripts/python.exe run_spike.py latency
  spike/.venv/Scripts/python.exe run_spike.py eval

契约/不变量：
  - memory-lab 冻结资产只读：本脚本绝不写 tools/memory-lab/ 下任何文件。
  - 凭据只进 .runtime/（gitignored）与子进程环境，stdout/结果 JSON 只输出名称与掩码。
  - 拉起的 telemem-mcp 子进程由本脚本用 Popen 句柄自行终止，不做全机进程扫描。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import re
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # tools/memcpp-integration/spike -> repo root
MEMORY_LAB = REPO / "tools" / "memory-lab" / "experiments"
RUNTIME = HERE / ".runtime"
RESULTS = HERE / "results"

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = int(os.environ.get("TELEMEM_SPIKE_PORT", "8421"))
BASE_URL = f"http://127.0.0.1:{PORT}/mcp"
LATENCY_SAMPLES = 12  # spec 要求 >=10
USER_SCOPE = "spike"

# 环境变量名按“端点配置存在性”扫描，只记名称不记值（实测 5 只读发现）
ENDPOINT_ENV_RE = re.compile(
    r"OPENAI|ANTHROPIC|MINIMAX|DASHSCOPE|MOONSHOT|DEEPSEEK|SILICONFLOW|ZHIPU|QWEN"
    r"|GEMINI|GOOGLE_API|EMBED|LLM_|_LLM|API_KEY|BASE_URL"
)
LOCAL_ENDPOINT_PORTS = [11434, 1234, 8080]  # Ollama / LM Studio / llama.cpp 常见端口


def require_endpoints() -> dict:
    """读齐六个端点环境变量；缺哪个就报哪个，绝不猜默认值。"""
    needed = [
        "TELEMEM_SPIKE_LLM_BASE_URL",
        "TELEMEM_SPIKE_LLM_API_KEY",
        "TELEMEM_SPIKE_LLM_MODEL",
        "TELEMEM_SPIKE_EMBED_BASE_URL",
        "TELEMEM_SPIKE_EMBED_API_KEY",
        "TELEMEM_SPIKE_EMBED_MODEL",
    ]
    missing = [name for name in needed if not os.environ.get(name)]
    if missing:
        print(json.dumps({"blocked": "missing_env", "missing": missing}, ensure_ascii=False))
        sys.exit(2)
    return {name: os.environ[name] for name in needed}


def write_runtime_config(endpoints: dict | None) -> Path:
    """生成 TELEMEM_CONFIG 指向的 yaml；endpoints=None 时用不可达占位端点。"""
    RUNTIME.mkdir(parents=True, exist_ok=True)
    db_dir = RUNTIME / "faiss_db"
    if endpoints is None:
        llm = {"base": "http://127.0.0.1:9/v1", "key": "sk-spike-placeholder", "model": "placeholder"}
        embed = llm
    else:
        llm = {
            "base": endpoints["TELEMEM_SPIKE_LLM_BASE_URL"],
            "key": endpoints["TELEMEM_SPIKE_LLM_API_KEY"],
            "model": endpoints["TELEMEM_SPIKE_LLM_MODEL"],
        }
        embed = {
            "base": endpoints["TELEMEM_SPIKE_EMBED_BASE_URL"],
            "key": endpoints["TELEMEM_SPIKE_EMBED_API_KEY"],
            "model": endpoints["TELEMEM_SPIKE_EMBED_MODEL"],
        }
    config = f"""llm:
  provider: openai
  config:
    model: {llm["model"]}
    openai_base_url: "{llm["base"]}"
    api_key: "{llm["key"]}"

embedder:
  provider: openai
  config:
    model: {embed["model"]}
    openai_base_url: "{embed["base"]}"
    api_key: "{embed["key"]}"

vector_store:
  provider: faiss
  config:
    collection_name: telemem_spike
    path: {RUNTIME / "faiss_db"}

history_db_path: {RUNTIME / "history.db"}
"""
    path = RUNTIME / "config.yaml"
    path.write_text(config, encoding="utf-8")
    return path


def reset_runtime_db() -> None:
    """清掉上次运行的 faiss/history 残留，保证 ingest 可重跑；Windows 文件句柄释放有延迟，重试兜底。"""
    import shutil

    for target in [RUNTIME / "faiss_db", RUNTIME / "history.db"]:
        last_error: OSError | None = None
        for _ in range(10):
            try:
                if target.is_dir():
                    shutil.rmtree(target)
                elif target.exists():
                    target.unlink()
                last_error = None
                break
            except OSError as exc:
                last_error = exc
                time.sleep(0.5)
        if last_error is not None:
            raise last_error


def spawn_server(config_path: Path) -> subprocess.Popen:
    """用本 venv 解释器拉起 telemem-mcp（streamable-http，回环 8421）。"""
    env = dict(os.environ)
    env["TELEMEM_CONFIG"] = str(config_path)
    env.setdefault("MEM0_TELEMETRY", "False")
    proc = subprocess.Popen(
        [sys.executable, "-m", "telemem.mcp", "--transport", "streamable-http",
         "--host", "127.0.0.1", "--port", str(PORT)],
        env=env, cwd=str(HERE),
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
    )
    if not wait_port(PORT, timeout=30):
        kill_tree(proc)
        raise RuntimeError(f"telemem-mcp 未在 30s 内监听 {PORT}")
    return proc


def wait_port(port: int, timeout: float) -> bool:
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        if proc_is_gone():
            return False
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.3)
    return False


_PROC: subprocess.Popen | None = None


def kill_tree(proc: subprocess.Popen) -> None:
    """venv python.exe 是启动桩，terminate 只杀桩不杀真实解释器，必须按树杀。"""
    subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                   capture_output=True, text=True, timeout=30,
                   encoding="utf-8", errors="replace")  # taskkill 中文输出，按替换解码防空线程崩
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


def proc_is_gone() -> bool:
    return _PROC is not None and _PROC.poll() is not None


def rss_snapshot(pid: int) -> dict:
    """读工作集；venv python.exe 是启动桩，真实解释器是子进程，必须按进程树合计。

    返回 {entries:[{id,ws_mb,peak_ws_mb,priv_mb,name}], total_ws_mb}；MB 保留 1 位小数。
    """
    ps = (
        "$ids = @(%d);"
        "$ids += (Get-CimInstance Win32_Process -Filter 'ParentProcessId=%d').ProcessId;"
        "$out = foreach ($i in $ids) { $p = Get-Process -Id $i -ErrorAction SilentlyContinue; if ($p) { "
        "[pscustomobject]@{ id=$p.Id; name=$p.ProcessName; "
        "ws_mb=[math]::Round($p.WorkingSet64/1MB,1); "
        "peak_ws_mb=[math]::Round($p.PeakWorkingSet64/1MB,1); "
        "priv_mb=[math]::Round($p.PrivateMemorySize64/1MB,1) } } };"
        "$out | ConvertTo-Json -Compress" % (pid, pid)
    )
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command", ps],
        capture_output=True, text=True, timeout=30,
    )
    stdout = out.stdout.strip()
    entries = json.loads(stdout) if stdout else []
    if isinstance(entries, dict):
        entries = [entries]
    return {
        "entries": entries,
        "total_ws_mb": round(sum(entry["ws_mb"] for entry in entries), 1),
        "total_peak_ws_mb": round(max(entry["peak_ws_mb"] for entry in entries), 1),
    }


def save_result(name: str, payload: dict) -> Path:
    RESULTS.mkdir(parents=True, exist_ok=True)
    path = RESULTS / f"{name}.json"
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"written: {path}")
    return path


# ---------------------------------------------------------------- 实测 1
def cmd_install_check(_args: argparse.Namespace) -> None:
    code = (
        "import sys, telemem, telemem.mcp;"
        "print('telemem', telemem.__version__, 'python', sys.version.split()[0])"
    )
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    payload = {"exit": out.returncode, "stdout": out.stdout.strip(), "stderr": out.stderr.strip()[-500:]}
    print(json.dumps(payload, ensure_ascii=False))
    save_result("install-check", payload)


# ---------------------------------------------------------------- 实测 5
def cmd_discover(_args: argparse.Namespace) -> None:
    matched = sorted({k for k in os.environ if ENDPOINT_ENV_RE.search(k)})
    payload = {
        "env_var_names_matched": matched,  # 只记名称，值永不输出
        "local_openai_compatible_ports": {},
    }
    for port in LOCAL_ENDPOINT_PORTS:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                payload["local_openai_compatible_ports"][str(port)] = "listening"
        except OSError:
            payload["local_openai_compatible_ports"][str(port)] = "closed"
    print(json.dumps(payload, ensure_ascii=False, indent=1))
    save_result("discover", payload)


# ---------------------------------------------------------------- MCP 客户端
async def mcp_call(tool: str, arguments: dict) -> dict:
    """经 streamable HTTP 调一次 MCP 工具，返回 {content, structured, is_error, elapsed_ms}。"""
    from mcp.client.session import ClientSession
    from mcp.client.streamable_http import streamable_http_client

    started = time.perf_counter()
    async with streamable_http_client(BASE_URL) as streams:
        async with ClientSession(*streams[:2]) as session:
            await session.initialize()
            result = await session.call_tool(tool, arguments)
            elapsed = time.perf_counter() - started
    structured = getattr(result, "structured_content", None)  # SDK v2 为 snake_case
    text = "\n".join(getattr(item, "text", "") for item in (result.content or []))
    tool_payload: dict = {}
    try:
        tool_payload = json.loads(text)
    except (ValueError, TypeError):
        tool_payload = {}
    has_error_payload = '"error"' in json.dumps(structured or tool_payload)
    return {
        # telemem 工具级错误 = 正常返回里嵌套 {"error":...} 载荷（进程不崩），不依赖 is_error 位
        "is_error": bool(getattr(result, "is_error", False)) or has_error_payload,
        "structured": structured,
        "text": text[:800],
        "elapsed_ms": round(elapsed * 1000, 1),
    }


# ---------------------------------------------------------------- 实测 2
def cmd_rss(_args: argparse.Namespace) -> None:
    global _PROC
    reset_runtime_db()
    config_path = write_runtime_config(None)  # 无端点：懒加载后首个工具调用应报结构化错误
    _PROC = spawn_server(config_path)
    pid = _PROC.pid
    try:
        boot = rss_snapshot(pid)
        probe = asyncio.run(mcp_call(
            "add_memory",
            {"text": "内存探针", "user_id": USER_SCOPE},
        ))
        loaded = rss_snapshot(pid)
        payload = {
            "pid": pid,
            "rss_after_boot": boot,
            "probe_add_is_error": probe["is_error"],
            "probe_payload": probe["structured"] or probe["text"],
            "rss_after_lazy_load_and_failed_call": loaded,
        }
        print(json.dumps(payload, ensure_ascii=False, indent=1))
        save_result("rss", payload)
    finally:
        kill_tree(_PROC)


# ---------------------------------------------------------------- 冻结集（实测 3/4 共用）
def load_frozen() -> tuple[list[dict], list[dict], list[str]]:
    observations = json.loads((MEMORY_LAB / "observations.json").read_text(encoding="utf-8"))
    queries = json.loads((MEMORY_LAB / "queries.json").read_text(encoding="utf-8"))
    # 与 eval_retrieval.py 同一 doc 合成口径：标题。叙述。事实。概念
    docs = [
        "。".join(part for part in [
            item["title"],
            item.get("narrative", ""),
            " ".join(item.get("facts") or []),
            " ".join(item.get("concepts") or []),
        ] if part)
        for item in observations
    ]
    return observations, queries, docs


def ingest_direct(memory, docs: list[dict]) -> float:
    """infer=False 原文直存 27 条，返回 ingest 总耗时秒。"""
    started = time.perf_counter()
    for item in docs:
        result = memory.add(
            [{"role": "user", "content": item["text"]}],
            user_id=USER_SCOPE,
            metadata={"obs_id": item["id"], "kind": item["kind"]},
            infer=False,
        )
        if not result.get("results"):
            raise RuntimeError(f"obs {item['id']} 未落库: {result}")
    return time.perf_counter() - started


def resolve_obs_id(entry: dict, doc_by_text: dict[str, int]) -> int | None:
    meta = entry.get("metadata") or {}
    if "obs_id" in meta:
        return int(meta["obs_id"])
    return doc_by_text.get((entry.get("memory") or "").strip())


def cmd_eval(_args: argparse.Namespace) -> None:
    endpoints = require_endpoints()
    reset_runtime_db()
    config_path = write_runtime_config(endpoints)
    from telemem import Memory
    from telemem.utils import load_config

    observations, queries, docs = load_frozen()
    doc_items = [{"id": obs["id"], "kind": obs["kind"], "text": doc}
                 for obs, doc in zip(observations, docs)]
    doc_by_text = {item["text"].strip(): item["id"] for item in doc_items}

    memory = Memory(load_config(str(config_path)))
    ingest_seconds = ingest_direct(memory, doc_items)

    per_query = []
    hits5 = 0
    reciprocal_sum = 0.0
    search_seconds = []
    for query in queries:
        started = time.perf_counter()
        result = memory.search(query["query"], user_id=USER_SCOPE, limit=5)
        search_seconds.append(time.perf_counter() - started)
        entries = result.get("results") or []
        top5_ids = [resolve_obs_id(entry, doc_by_text) for entry in entries[:5]]
        rank = next((pos for pos, obs_id in enumerate(top5_ids) if obs_id in query["gold_ids"]), None)
        if rank is not None:
            hits5 += 1
            reciprocal_sum += 1.0 / (rank + 1)
        per_query.append({
            "id": query["id"], "query": query["query"], "gold_ids": query["gold_ids"],
            "rank": rank, "top5_obs_ids": top5_ids,
        })

    # 2 字中文短查询探针：向量档口径 = top-20 结果中文本包含该词的条数（基线 5/3/11 为全文索引命中数）
    short_probe = []
    for term in ["端口", "配置", "模块"]:
        result = memory.search(term, user_id=USER_SCOPE, limit=20)
        entries = result.get("results") or []
        top_ids = [resolve_obs_id(entry, doc_by_text) for entry in entries]
        containment = sum(1 for entry in entries if term in (entry.get("memory") or ""))
        short_probe.append({
            "term": term,
            "top20_text_containment": containment,
            "top1_contains_term": bool(entries) and term in (entries[0].get("memory") or ""),
            "top20_obs_ids": top_ids,
        })

    payload = {
        "embedding": os.environ["TELEMEM_SPIKE_EMBED_MODEL"],
        "llm": os.environ["TELEMEM_SPIKE_LLM_MODEL"],
        "corpus": len(doc_items), "queries": len(queries),
        "recall_at_5": round(hits5 / len(queries), 4),
        "mrr": round(reciprocal_sum / len(queries), 4),
        "baseline_fts5_bigram": {"recall_at_5": 1.0, "short_probe_hits": {"端口": 5, "配置": 3, "模块": 11}},
        "trigger_line": {"recall_at_5_min": 0.85, "short_probe_min": {"端口": 5, "配置": 3, "模块": 11}},
        "ingest_seconds": round(ingest_seconds, 2),
        "search_seconds_p50": round(statistics.median(search_seconds) * 1000, 1),
        "per_query": per_query,
        "short_probe": short_probe,
    }
    print(json.dumps({k: v for k, v in payload.items() if k != "per_query"}, ensure_ascii=False, indent=1))
    save_result("eval", payload)


# ---------------------------------------------------------------- 实测 3
def cmd_latency(_args: argparse.Namespace) -> None:
    endpoints = require_endpoints()
    reset_runtime_db()
    config_path = write_runtime_config(endpoints)
    global _PROC
    _PROC = spawn_server(config_path)
    try:
        observations, _queries, docs = load_frozen()
        from telemem import Memory
        from telemem.utils import load_config

        memory = Memory(load_config(str(config_path)))
        ingest_direct(memory, [{"id": obs["id"], "kind": obs["kind"], "text": doc}
                               for obs, doc in zip(observations, docs)])
        # 懒加载 + 首查预热各吸收一次初始化/连接开销，之后才计入样本
        asyncio.run(mcp_call("search_memories", {"query": "预热", "user_id": USER_SCOPE, "limit": 5}))
        timings, errors = [], []
        for i in range(LATENCY_SAMPLES):
            probe = asyncio.run(mcp_call(
                "search_memories",
                {"query": f"worktree 开发 {i}", "user_id": USER_SCOPE, "limit": 5},
            ))
            timings.append(probe["elapsed_ms"])
            if probe["is_error"]:
                errors.append(probe["structured"])
        timings.sort()

        def pct(values: list[float], q: float) -> float:
            pos = (len(values) - 1) * q
            low, high = int(pos), min(int(pos) + 1, len(values) - 1)
            return round(values[low] + (values[high] - values[low]) * (pos - low), 1)

        payload = {
            "transport": "mcp streamable-http tools/call search_memories limit=5",
            "samples": timings,
            "p50_ms": pct(timings, 0.5),
            "p95_ms": pct(timings, 0.95),
            "min_ms": timings[0], "max_ms": timings[-1],
            "threshold_ms": 500,
            "errors": errors,
        }
        print(json.dumps(payload, ensure_ascii=False, indent=1))
        save_result("latency", payload)
    finally:
        kill_tree(_PROC)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ["install-check", "discover", "rss", "latency", "eval"]:
        child = sub.add_parser(name)
        child.set_defaults(func={
            "install-check": cmd_install_check,
            "discover": cmd_discover,
            "rss": cmd_rss,
            "latency": cmd_latency,
            "eval": cmd_eval,
        }[name])
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
