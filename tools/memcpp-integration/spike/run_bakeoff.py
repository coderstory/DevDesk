"""本地向量模型同场跑分：bge-small-zh-v1.5（基线）/ bge-m3 / Qwen3-Embedding-0.6B。

统一服务形态 = llama-server（OpenAI 兼容 /v1/embeddings），即未来 telemem 指向的
生产形态；跑分环境即生产环境，不用第二套实现。冻结集与口径复用
tools/memory-lab/experiments/（27 记忆 / 15 查询，只读），另加同目录
hard_queries.json（10 条难查询，构造原则见该文件 meta）。

每模型用法以各自模型卡为准（跑分公平性的关键）：
- bge-small-zh-v1.5: CLS 池化；查询侧加官方 s2p 指令
  "为这个句子生成表示以用于检索相关文章："
- bge-m3:            CLS 池化（GGUF 元数据 pooling_type=2）；不加快捷指令
- Qwen3-Embedding-0.6B: last 池化 + -ub 8192；查询侧加英文 instruction
  "Instruct: ...\\nQuery: ..."（官方格式，指令缺失可掉 1~5%）

用法（worktree 内，venv 为 spike 既有环境）：
    spike/.venv/Scripts/python.exe run_bakeoff.py <bge-small|bge-m3|qwen3> [--port N]
    spike/.venv/Scripts/python.exe run_bakeoff.py report   # 汇总已生成的结果 JSON

输出：.bakeoff/results/bakeoff_<key>.json（含 Recall@5/MRR/逐查询排名/RSS/延迟）。
模型与 llama-server 均不入库（.gitignore .bakeoff/）；本脚本只管理自己拉起的进程。
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import shlex
import sys
import time
import urllib.request
from pathlib import Path

import requests

HERE = Path(__file__).resolve().parent
FROZEN = HERE.parent.parent / "memory-lab" / "experiments"
BAKEOFF = HERE / ".bakeoff"
RESULTS = BAKEOFF / "results"
LOGS = BAKEOFF / "logs"
LLAMA_SERVER = BAKEOFF / "bin" / "llama-server.exe"
HOST = "127.0.0.1"

QWEN_INSTRUCTION = "Given a user query, retrieve relevant passages that answer the query"
BGE_ZH_INSTRUCTION = "为这个句子生成表示以用于检索相关文章："

MODELS = {
    "bge-small": {
        "gguf": "bge-small-zh-v1.5-q8_0.gguf",
        "pooling": "cls",
        "extra_flags": [],
        "query_prefix": BGE_ZH_INSTRUCTION,
        "label": "bge-small-zh-v1.5 Q8_0（基线）",
    },
    "bge-m3": {
        "gguf": "bge-m3-Q8_0.gguf",
        "pooling": "cls",
        "extra_flags": [],
        "query_prefix": "",
        "label": "bge-m3 Q8_0",
    },
    "qwen3": {
        "gguf": "Qwen3-Embedding-0.6B-Q8_0.gguf",
        "pooling": "last",
        "extra_flags": ["-ub", "8192"],
        "query_prefix": f"Instruct: {QWEN_INSTRUCTION}\nQuery: ",
        "label": "Qwen3-Embedding-0.6B Q8_0",
    },
}


def compose_doc(item: dict) -> str:
    # 与 eval_retrieval.py 的 doc 合成式逐字一致：标题。叙述。事实。概念
    return "。".join(
        part
        for part in [
            item["title"],
            item.get("narrative", ""),
            " ".join(item.get("facts") or []),
            " ".join(item.get("concepts") or []),
        ]
        if part
    )


def get_working_set(pid: int) -> int:
    """Windows 口径：WorkingSet 作为 RSS 代理（与 S1 实测口径一致）。"""
    import ctypes.wintypes as wt

    class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("cb", wt.DWORD),
            ("PageFaultCount", wt.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    psapi = ctypes.WinDLL("psapi")
    kernel32 = ctypes.WinDLL("kernel32")
    counters = PROCESS_MEMORY_COUNTERS()
    counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
    handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
    if not handle:
        return 0
    try:
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return 0
        return int(counters.WorkingSetSize)
    finally:
        kernel32.CloseHandle(handle)


def wait_health(port: int, timeout_s: int = 120) -> None:
    deadline = time.monotonic() + timeout_s
    last_err = ""
    while time.monotonic() < deadline:
        try:
            r = requests.get(f"http://{HOST}:{port}/health", timeout=2)
            if r.status_code == 200:
                return
            last_err = f"HTTP {r.status_code}"
        except Exception as exc:  # noqa: BLE001 - 轮询期连接失败属预期
            last_err = str(exc)[:120]
        time.sleep(1)
    raise RuntimeError(f"llama-server /health 未就绪（{timeout_s}s）：{last_err}")


def embed(client: requests.Session, port: int, texts: list[str], batch: int = 8) -> list[list[float]]:
    vectors: list[list[float]] = []
    for start in range(0, len(texts), batch):
        chunk = texts[start : start + batch]
        r = client.post(
            f"http://{HOST}:{port}/v1/embeddings",
            json={"input": chunk},
            timeout=120,
        )
        r.raise_for_status()
        data = sorted(r.json()["data"], key=lambda item: item["index"])
        vectors.extend(item["embedding"] for item in data)
    return vectors


def recall_metrics(rankings: list[list[int]], queries: list[dict]) -> dict:
    ranks = []
    for ranking, query in zip(rankings, queries):
        rank = next((pos for pos, memory_id in enumerate(ranking) if memory_id in query["gold_ids"]), None)
        ranks.append(rank)
    hits5 = sum(1 for rank in ranks if rank is not None and rank < 5)
    mrr = sum(0.0 if rank is None else 1.0 / (rank + 1) for rank in ranks) / len(ranks)
    return {"recall_at_5": round(hits5 / len(ranks), 4), "mrr": round(mrr, 4), "per_query_rank": ranks}


def percentile(samples: list[float], ratio: float) -> float:
    ordered = sorted(samples)
    index = min(len(ordered) - 1, math.ceil(ratio * len(ordered)) - 1)
    return ordered[max(0, index)]


def run_model(key: str, port: int, latency_samples: int) -> dict:
    config = MODELS[key]
    gguf = BAKEOFF / "models" / config["gguf"]
    if not gguf.is_file():
        raise SystemExit(f"缺模型文件：{gguf}")
    if not LLAMA_SERVER.is_file():
        raise SystemExit(f"缺 llama-server：{LLAMA_SERVER}")

    # 诊断开关：覆盖模型卡附加旗标（如测 -ub 敏感性）；正式跑分必须用模型卡默认值
    override = os.environ.get("BAKEOFF_EXTRA_FLAGS", "").strip()
    extra_flags = shlex.split(override) if override else config["extra_flags"]

    observations = json.loads((FROZEN / "observations.json").read_text(encoding="utf-8"))
    frozen_queries = json.loads((FROZEN / "queries.json").read_text(encoding="utf-8"))
    hard = json.loads((HERE / "hard_queries.json").read_text(encoding="utf-8"))["queries"]

    docs = [compose_doc(item) for item in observations]
    ids = [item["id"] for item in observations]
    id_pos = {memory_id: pos for pos, memory_id in enumerate(ids)}
    query_texts = [q["query"] for q in frozen_queries]
    hard_texts = [q["query"] for q in hard]

    LOGS.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    server_log = open(LOGS / f"server_{key}.log", "wb")
    cmd = [
        str(LLAMA_SERVER), "-m", str(gguf), "--embedding",
        "--pooling", config["pooling"], *extra_flags,
        "--host", HOST, "--port", str(port),
    ]
    proc = __import__("subprocess").Popen(cmd, stdout=server_log, stderr=server_log)
    t_start = time.perf_counter()
    try:
        wait_health(port)
        load_seconds = time.perf_counter() - t_start
        rss_after_load = get_working_set(proc.pid)

        with requests.Session() as client:
            embed(client, port, docs)  # 预热（含首次真实推理）
            t_all = time.perf_counter()
            doc_vectors = embed(client, port, docs)
            doc_encode_seconds = time.perf_counter() - t_all
            frozen_vecs = embed(client, port, [config["query_prefix"] + text for text in query_texts])
            hard_vecs = embed(client, port, [config["query_prefix"] + text for text in hard_texts])

            dims = {len(vector) for vector in doc_vectors}
            if len(dims) != 1:
                raise RuntimeError(f"维度不一致：{dims}")
            dimension = dims.pop()

            def rank_by_cosine(query_vectors: list[list[float]]) -> list[list[int]]:
                rankings = []
                for qv in query_vectors:
                    scored = sorted(
                        range(len(ids)),
                        key=lambda pos: -sum(a * b for a, b in zip(qv, doc_vectors[pos])),
                    )
                    rankings.append([ids[pos] for pos in scored])
                return rankings

            frozen_metrics = recall_metrics(rank_by_cosine(frozen_vecs), frozen_queries)
            hard_metrics = recall_metrics(rank_by_cosine(hard_vecs), hard)

            # 单条 embedding 延迟：预热 3 次后采 ≥latency_samples 个单请求样本
            probe = [config["query_prefix"] + query_texts[0]] * 3
            embed(client, port, probe)
            timings = []
            round_robin = (query_texts + hard_texts)
            for i in range(latency_samples):
                text = config["query_prefix"] + round_robin[i % len(round_robin)]
                t0 = time.perf_counter()
                embed(client, port, [text], batch=1)
                timings.append((time.perf_counter() - t0) * 1000.0)

        rss_steady = get_working_set(proc.pid)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001 - 兜底杀自己拉起的进程
            proc.kill()
        server_log.close()

    result = {
        "key": key,
        "model": config["label"],
        "gguf_file": config["gguf"],
        "gguf_bytes": gguf.stat().st_size,
        "llama_server_args": cmd[1:],
        "pooling": config["pooling"],
        "query_prefix": config["query_prefix"],
        "dims": dimension,
        "load_seconds": round(load_seconds, 2),
        "rss_after_load_bytes": rss_after_load,
        "rss_steady_bytes": rss_steady,
        "doc_encode_27_seconds": round(doc_encode_seconds, 3),
        "latency_single_embed_ms": {
            "samples": len(timings),
            "p50": round(percentile(timings, 0.5), 1),
            "p95": round(percentile(timings, 0.95), 1),
        },
        "frozen": {
            "n_queries": len(frozen_queries),
            "recall_at_5": frozen_metrics["recall_at_5"],
            "mrr": frozen_metrics["mrr"],
            "per_query_rank": frozen_metrics["per_query_rank"],
        },
        "hard": {
            "n_queries": len(hard),
            "recall_at_5": hard_metrics["recall_at_5"],
            "mrr": hard_metrics["mrr"],
            "per_query_rank": hard_metrics["per_query_rank"],
        },
        "per_query_detail": [
            {"id": q["id"], "query": q["query"], "gold_ids": q["gold_ids"], "rank": rank}
            for q, rank in zip(frozen_queries + hard, frozen_metrics["per_query_rank"] + hard_metrics["per_query_rank"])
        ],
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    out = RESULTS / f"bakeoff_{key}.json"
    out.write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    return result


def report() -> str:
    rows = []
    for key in ("bge-small", "bge-m3", "qwen3"):
        path = RESULTS / f"bakeoff_{key}.json"
        if not path.is_file():
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        rows.append(
            "| {model} | {dims} | {fz} | {hd} | {rss_load:.0f} | {rss_steady:.0f} | {p50} | {p95} | {size:.0f} |".format(
                model=data["model"],
                dims=data["dims"],
                fz=data["frozen"]["recall_at_5"],
                hd=data["hard"]["recall_at_5"],
                rss_load=data["rss_after_load_bytes"] / 1024 / 1024,
                rss_steady=data["rss_steady_bytes"] / 1024 / 1024,
                p50=data["latency_single_embed_ms"]["p50"],
                p95=data["latency_single_embed_ms"]["p95"],
                size=data["gguf_bytes"] / 1024 / 1024,
            )
        )
    header = [
        "| 模型 | 维度 | 原集 R@5 | 难集 R@5 | RSS加载后 MB | RSS稳态 MB | 延迟p50 ms | p95 ms | 体积 MB |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    return "\n".join(header + rows)


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("target", choices=["bge-small", "bge-m3", "qwen3", "report"])
    parser.add_argument("--port", type=int, default=18432)
    parser.add_argument("--samples", type=int, default=24)
    args = parser.parse_args()
    if args.target == "report":
        print(report())
        return
    result = run_model(args.target, args.port, args.samples)
    print(json.dumps({
        "model": result["model"], "dims": result["dims"],
        "frozen_r@5": result["frozen"]["recall_at_5"], "hard_r@5": result["hard"]["recall_at_5"],
        "rss_steady_mb": round(result["rss_steady_bytes"] / 1048576, 1),
        "p50_ms": result["latency_single_embed_ms"]["p50"], "p95_ms": result["latency_single_embed_ms"]["p95"],
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
