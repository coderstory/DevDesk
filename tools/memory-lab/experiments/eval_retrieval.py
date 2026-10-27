"""冻结集上的检索评测：FTS5(bigram+unicode61) / 向量 / 混合，以及与 trigram 的对照。

设计要点（对应审计 Blocker 1）：
- 全文路必须是**真实 SQLite FTS5**，不是 Python BM25 代理。历史 mem_eval.py 的"全文"
  是字符 bigram BM25，与报告推荐的 FTS5 trigram 不匹配，本脚本取代它。
- 中文用"入库前双字分词 + unicode61"：中文连续串切成重叠 2-gram，空格连接后交给
  FTS5 词级索引。查询走同一分词，词间 OR，bm25 排序。
- 同时建一张 trigram 表做对照，并单独测 2 字中文短查询（trigram 返回 0 行）。
- 向量：fastembed + BAAI/bge-small-zh-v1.5（512 维），精确余弦。
- 混合：RRF(k=60) 融合 FTS5 与向量排名。
- 查询集与观测集冻结在 observations.json / queries.json，本脚本不生成任何数据。

用法：
    D:/software/venvs/memory-lab/Scripts/python.exe -I eval_retrieval.py

输出：results/results.json、results/results.md、raw/eval_run.json（完整逐查询排名）
"""

from __future__ import annotations

import json
import math
import re
import sqlite3
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

CJK_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]+")
ASCII_RE = re.compile(r"[a-z0-9]+")
TOKEN_SPLIT_RE = re.compile(r"[^0-9a-zA-Z\u3400-\u4dbf\u4e00-\u9fff\u3040-\u30ff\uac00-\ud7af]+")
RRF_K = 60


def normalize(text: str) -> str:
    return (text or "").casefold()


def bigram_tokens(text: str) -> list[str]:
    """中文重叠双字 + ASCII 小写词；用于 FTS5 unicode61 索引/查询。"""
    tokens: list[str] = []
    for segment in TOKEN_SPLIT_RE.split(normalize(text)):
        if not segment:
            continue
        if CJK_RE.fullmatch(segment):
            if len(segment) == 1:
                tokens.append(segment)
            else:
                tokens.extend(segment[i : i + 2] for i in range(len(segment) - 1))
        else:
            # 混合串（如 get端口2）：拆 ASCII 词，再按中文/非中文切分
            for part in re.findall(r"[a-z0-9]+|[^\x00-\x7f]+", segment):
                if CJK_RE.fullmatch(part):
                    tokens.extend(part[i : i + 2] for i in range(max(1, len(part) - 1)))
                else:
                    tokens.append(part)
    return tokens


def trigram_tokens(text: str) -> list[str]:
    """中文 3-gram + ASCII 3-gram（trigram 表查询用；2 字中文无 token）。"""
    tokens: list[str] = []
    for segment in TOKEN_SPLIT_RE.split(normalize(text)):
        if not segment:
            continue
        for run in re.findall(r"[a-z0-9]+|[^\x00-\x7f]+", segment):
            size = 3
            if len(run) < size:
                continue
            tokens.extend(run[i : i + size] for i in range(len(run) - size + 1))
    return tokens


def fts_match_expr(tokens: list[str]) -> str:
    if not tokens:
        return ""
    unique = list(dict.fromkeys(tokens))
    return " OR ".join('"' + token.replace('"', '""') + '"' for token in unique)


def cosine(a: list[float], b: list[float]) -> float:
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    return dot / (na * nb + 1e-12)


def build_fts(connection: sqlite3.Connection, name: str, tokenizer: str, rows: list[tuple[int, str]], text_fn) -> None:
    connection.execute(
        f"CREATE VIRTUAL TABLE {name} USING fts5(idx_text, tokenize='{tokenizer}')"
    )
    connection.executemany(
        f"INSERT INTO {name}(rowid, idx_text) VALUES (?, ?)",
        [(memory_id, text_fn(text)) for memory_id, text in rows],
    )


def ranked_ids(connection: sqlite3.Connection, table: str, match: str) -> list[int]:
    if not match:
        return []
    rows = connection.execute(
        f"SELECT rowid FROM {table} WHERE {table} MATCH ? ORDER BY bm25({table})",
        (match,),
    ).fetchall()
    return [int(row[0]) for row in rows]


def first_hit_rank(ranking: list[int], gold_ids: list[int]) -> int | None:
    for position, memory_id in enumerate(ranking):
        if memory_id in gold_ids:
            return position
    return None


def metrics(rankings: dict[str, list[int]], queries: list[dict]) -> dict:
    output: dict[str, dict] = {}
    for mode, per_query in rankings.items():
        ranks = []
        for query, ranking in zip(queries, per_query):
            rank = first_hit_rank(ranking, query["gold_ids"])
            ranks.append(rank)
        hits5 = sum(1 for rank in ranks if rank is not None and rank < 5)
        reciprocal = [0.0 if rank is None else 1.0 / (rank + 1) for rank in ranks]
        output[mode] = {
            "recall_at_5": round(hits5 / len(queries), 4),
            "mrr": round(sum(reciprocal) / len(reciprocal), 4),
            "per_query_rank": ranks,
        }
    return output


def main() -> None:
    observations = json.loads((HERE / "observations.json").read_text(encoding="utf-8"))
    queries = json.loads((HERE / "queries.json").read_text(encoding="utf-8"))
    docs = [
        "。".join(
            part
            for part in [
                item["title"],
                item.get("narrative", ""),
                " ".join(item.get("facts") or []),
                " ".join(item.get("concepts") or []),
            ]
            if part
        )
        for item in observations
    ]
    ids = [item["id"] for item in observations]

    connection = sqlite3.connect(":memory:")
    connection.execute("CREATE TABLE memories_raw(id INTEGER PRIMARY KEY, text TEXT)")
    connection.executemany("INSERT INTO memories_raw(id, text) VALUES (?, ?)", list(zip(ids, docs)))
    t_index = time.perf_counter()
    build_fts(connection, "fts_bigram", "unicode61", list(zip(ids, docs)), lambda text: " ".join(bigram_tokens(text)))
    bigram_index_seconds = time.perf_counter() - t_index
    t_trigram = time.perf_counter()
    build_fts(connection, "fts_trigram", "trigram", list(zip(ids, docs)), lambda text: text)
    trigram_index_seconds = time.perf_counter() - t_trigram

    # 短查询探针：2 字中文在 trigram 下 0 行，bigram 命中，LIKE 兜底命中
    short_queries = ["端口", "配置", "模块", "端口配置", "worktree", "getUserById"]
    short_probe = []
    for text in short_queries:
        bigram_hits = ranked_ids(connection, "fts_bigram", fts_match_expr(bigram_tokens(text)))
        trigram_hits = ranked_ids(connection, "fts_trigram", fts_match_expr(trigram_tokens(text)))
        like_hits = [
            int(row[0])
            for row in connection.execute(
                "SELECT id FROM memories_raw WHERE text LIKE ?", (f"%{text}%",)
            ).fetchall()
        ]
        short_probe.append(
            {
                "query": text,
                "bigram_hits": len(bigram_hits),
                "trigram_hits": len(trigram_hits),
                "like_raw_text_hits": len(like_hits),
            }
        )

    # 向量路
    sys.path.insert(0, str(HERE.parent))
    from fastembed import TextEmbedding  # noqa: E402
    import numpy as np  # noqa: E402

    t_model = time.perf_counter()
    model = TextEmbedding("BAAI/bge-small-zh-v1.5")
    doc_vectors = np.asarray(list(model.embed(docs)), dtype=np.float32)
    query_vectors = np.asarray(list(model.embed([query["query"] for query in queries])), dtype=np.float32)
    embedding_seconds = time.perf_counter() - t_model
    doc_norms = np.linalg.norm(doc_vectors, axis=1) + 1e-12
    query_norms = np.linalg.norm(query_vectors, axis=1) + 1e-12
    similarity = (query_vectors / query_norms[:, None]) @ (doc_vectors / doc_norms[:, None]).T

    rankings: dict[str, list[list[int]]] = {"fts5_bigram": [], "fts5_trigram": [], "vector": [], "hybrid": []}
    query_timings: list[dict] = []
    for query_index, query in enumerate(queries):
        started = time.perf_counter()
        bigram_rank = ranked_ids(connection, "fts_bigram", fts_match_expr(bigram_tokens(query["query"])))
        trigram_rank = ranked_ids(connection, "fts_trigram", fts_match_expr(trigram_tokens(query["query"])))
        vector_rank = [int(ids[i]) for i in np.argsort(-similarity[query_index])]
        rrf_scores: dict[int, float] = {}
        for ranking in (bigram_rank, vector_rank):
            for position, memory_id in enumerate(ranking):
                rrf_scores[memory_id] = rrf_scores.get(memory_id, 0.0) + 1.0 / (RRF_K + position + 1)
        hybrid_rank = sorted(rrf_scores, key=lambda memory_id: -rrf_scores[memory_id])
        query_timings.append(time.perf_counter() - started)
        rankings["fts5_bigram"].append(bigram_rank)
        rankings["fts5_trigram"].append(trigram_rank)
        rankings["vector"].append(vector_rank)
        rankings["hybrid"].append(hybrid_rank)

    scores = metrics(rankings, queries)
    scores["fts5_bigram"]["notes"] = "中文重叠双字分词 + unicode61，查询词 OR + bm25"
    scores["fts5_trigram"]["notes"] = "trigram 分词，查询 3-gram OR；2 字中文查询没有 3-gram token → 0 命中"
    scores["vector"]["notes"] = "fastembed BAAI/bge-small-zh-v1.5，精确余弦"
    scores["hybrid"]["notes"] = f"RRF(k={RRF_K}) 融合 fts5_bigram 与 vector 排名"

    result = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "python": sys.version.split()[0],
        "sqlite": sqlite3.sqlite_version,
        "dataset": {"observations": len(observations), "queries": len(queries)},
        "modes": scores,
        "short_query_probe": short_probe,
        "timings": {
            "fts5_bigram_index_seconds": round(bigram_index_seconds, 4),
            "fts5_trigram_index_seconds": round(trigram_index_seconds, 4),
            "embedding_model_load_and_encode_seconds": round(embedding_seconds, 4),
            "query_total_percentiles_ms": {
                "p50": round(sorted(query_timings)[len(query_timings) // 2] * 1000, 3),
                "max": round(max(query_timings) * 1000, 3),
            },
        },
        "per_query": [
            {
                "id": query["id"],
                "query": query["query"],
                "gold_ids": query["gold_ids"],
                "ranks": {mode: scores[mode]["per_query_rank"][index] for mode in scores},
                "top5": {mode: rankings[mode][index][:5] for mode in ("fts5_bigram", "fts5_trigram", "vector", "hybrid")},
            }
            for index, query in enumerate(queries)
        ],
    }
    (HERE / "results").mkdir(exist_ok=True)
    (HERE / "raw" / "eval_run.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    (HERE / "results" / "results.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8"
    )

    lines = [
        "# 检索评测结果（冻结集，27 条记忆 / 15 条查询）",
        "",
        f"- 生成时间：{result['generated_at']}；Python {result['python']}；SQLite {result['sqlite']}",
        "- 数据集：`observations.json`（27 条，按标题去重）、`queries.json`（15 条，冻结）",
        "- 全文实现：SQLite FTS5 `unicode61` + 入库前中文重叠双字分词（不是 Python BM25 代理）",
        "- 向量：fastembed `BAAI/bge-small-zh-v1.5`（512 维），精确余弦；混合：RRF(k=60)",
        "",
        "## 指标",
        "",
        "| 路 | Recall@5 | MRR | 说明 |",
        "|---|---|---|---|",
    ]
    for mode in ("fts5_bigram", "vector", "hybrid", "fts5_trigram"):
        entry = scores[mode]
        lines.append(f"| {mode} | {entry['recall_at_5']:.2f} | {entry['mrr']:.4f} | {entry['notes']} |")
    lines += [
        "",
        "## 逐查询排名（0 = 第 1 名；None = 未命中）",
        "",
        "| id | 查询 | gold | fts5_bigram | vector | hybrid | fts5_trigram |",
        "|---|---|---|---|---|---|---|",
    ]
    for row in result["per_query"]:
        ranks = row["ranks"]
        def fmt(value):
            return "未命中" if value is None else str(value + 1)
        lines.append(
            f"| {row['id']} | {row['query']} | {row['gold_ids']} | {fmt(ranks['fts5_bigram'])} | "
            f"{fmt(ranks['vector'])} | {fmt(ranks['hybrid'])} | {fmt(ranks['fts5_trigram'])} |"
        )
    lines += [
        "",
        "## 短查询探针（2 字中文 / 代码符号）",
        "",
        "| 查询 | bigram 命中数 | trigram 命中数 | LIKE 原文命中数 |",
        "|---|---|---|---|",
    ]
    for row in short_probe:
        lines.append(
            f"| {row['query']} | {row['bigram_hits']} | {row['trigram_hits']} | {row['like_raw_text_hits']} |"
        )
    lines += [
        "",
        "## 耗时",
        "",
        f"- FTS5 bigram 建索引：{result['timings']['fts5_bigram_index_seconds']} s（27 条）",
        f"- FTS5 trigram 建索引：{result['timings']['fts5_trigram_index_seconds']} s（27 条）",
        f"- 向量模型加载 + 42 条编码：{result['timings']['embedding_model_load_and_encode_seconds']} s",
        f"- 单查询（FTS+trigram+向量+融合）p50 {result['timings']['query_total_percentiles_ms']['p50']} ms，"
        f"max {result['timings']['query_total_percentiles_ms']['max']} ms",
        "",
        "## 读法",
        "",
        "- 27 条里有大量同义重复，gold 用人工等价组（queries.json 的 `gold_ids`），命中任意等价条目都算对；"
        "若只用单条 gold，重复条目会把 MRR 系统性压低。",
        "- 查询由目标记忆反推生成，天然对检索有利；Recall@5 已到天花板，**不能作为质量结论**，"
        "只能证明机制在真实 FTS5 上跑通。",
        "- trigram 在长查询上可工作，但 2 字中文查询没有 3-gram token，命中 0；这是方案必须加双字分词的原因。",
        "",
        f"原始逐查询排名：`raw/eval_run.json`；本文件由 `eval_retrieval.py` 生成。",
    ]
    (HERE / "results" / "results.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines[:40]))
    print("\nwritten: results/results.json, results/results.md, raw/eval_run.json")


if __name__ == "__main__":
    main()