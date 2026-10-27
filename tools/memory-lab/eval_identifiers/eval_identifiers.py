"""标识符/路径/错误码/缩写/2 字中文查询在双字分词方案下的召回评测（未验证项 14）。

三臂对照（全部跑在**真实 SQLite :memory: FTS5** 上，不是模拟）：
- bigram_only：FTS5 unicode61 + 冻结 bigram_tokens（中文重叠双字 + ASCII 词），词 OR + bm25——无兜底。
- like_only：原文 lower(text) 逐 token AND 的 LIKE——无索引。
- bigram_like：生产候选方案——FTS5 bigram 为主，命中不足 5 行时用 LIKE（全 token AND）
  按文档顺序补位，FTS 已命中的不重复。兜底只补位，不改变 FTS 已有排名。

判据：标识符类（camel/snake/path/error_code/abbrev）Recall@5 >= 0.8 → 双字方案对 P0 可用；
低于则逐查询输出失败模式，不据此改存储方案。

只读依赖：experiments/eval_retrieval.py 的 bigram_tokens / ranked_ids / fts_match_expr /
first_hit_rank（冻结分词与排序契约）。本脚本不复制分词逻辑，保证测的就是冻结方案本身。

用法：
    python -I eval_identifiers.py

输出：results/results.json、results/results.md、raw/eval_run.json（逐查询排名与失败明细）
"""

from __future__ import annotations

import importlib.util
import json
import sqlite3
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

IDENTIFIER_CATEGORIES = ("camel", "snake", "path", "error_code", "abbrev")
ALL_CATEGORIES = IDENTIFIER_CATEGORIES + ("zh_short",)
P0_RECALL_THRESHOLD = 0.8
TOP_K = 5


def load_frozen_contract():
    """以模块方式加载冻结的 eval_retrieval，借用其分词/排序/计分契约。"""
    path = HERE.parent / "experiments" / "eval_retrieval.py"
    spec = importlib.util.spec_from_file_location("frozen_eval_retrieval", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


frozen = load_frozen_contract()
bigram_tokens = frozen.bigram_tokens
ranked_ids = frozen.ranked_ids
fts_match_expr = frozen.fts_match_expr
first_hit_rank = frozen.first_hit_rank


def doc_text(item: dict) -> str:
    """入库文本：title。narrative。facts…。concepts…（与冻结评测同构）。"""
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


def escape_like(token: str) -> str:
    return token.replace("!", "!!").replace("%", "!%").replace("_", "!_")


def like_ranking(connection: sqlite3.Connection, tokens: list[str]) -> list[int]:
    """原文 LIKE 路：全部 token AND 才命中，按文档顺序（id 升序）。"""
    if not tokens:
        return []
    unique = list(dict.fromkeys(tokens))
    where = " AND ".join("text_lower LIKE ? ESCAPE '!'" for _ in unique)
    params = [f"%{escape_like(token)}%" for token in unique]
    rows = connection.execute(
        f"SELECT id FROM memories_raw WHERE {where} ORDER BY id", params
    ).fetchall()
    return [int(row[0]) for row in rows]


def bigram_with_like_fallback(connection: sqlite3.Connection, query: str) -> list[int]:
    """生产候选：FTS5 bigram 排名在前，不足 TOP_K 行时 LIKE 补位，不重复。"""
    tokens = bigram_tokens(query)
    ranking = ranked_ids(connection, "fts_bigram", fts_match_expr(tokens))
    if len(ranking) < TOP_K:
        seen = set(ranking)
        for memory_id in like_ranking(connection, tokens):
            if memory_id not in seen:
                ranking.append(memory_id)
                seen.add(memory_id)
    return ranking


def build_db(memories: list[dict]) -> sqlite3.Connection:
    connection = sqlite3.connect(":memory:")
    connection.execute(
        "CREATE TABLE memories_raw(id INTEGER PRIMARY KEY, text TEXT, text_lower TEXT)"
    )
    connection.executemany(
        "INSERT INTO memories_raw(id, text, text_lower) VALUES (?, ?, ?)",
        [(item["id"], doc_text(item), doc_text(item).casefold()) for item in memories],
    )
    connection.execute(
        "CREATE VIRTUAL TABLE fts_bigram USING fts5(idx_text, tokenize='unicode61')"
    )
    connection.executemany(
        "INSERT INTO fts_bigram(rowid, idx_text) VALUES (?, ?)",
        [(item["id"], " ".join(bigram_tokens(doc_text(item)))) for item in memories],
    )
    return connection


def recall_at_5(rankings: list[list[int]], queries: list[dict]) -> float:
    hits = 0
    for ranking, query in zip(rankings, queries):
        rank = first_hit_rank(ranking, query["gold_ids"])
        if rank is not None and rank < TOP_K:
            hits += 1
    return round(hits / len(queries), 4)


def evaluate(connection: sqlite3.Connection, queries: list[dict]) -> dict:
    arms = {
        "bigram_only": lambda query: ranked_ids(
            connection, "fts_bigram", fts_match_expr(bigram_tokens(query))
        ),
        "like_only": lambda query: like_ranking(connection, bigram_tokens(query)),
        "bigram_like": lambda query: bigram_with_like_fallback(connection, query),
    }
    rankings: dict[str, list[list[int]]] = {mode: [] for mode in arms}
    for query in queries:
        for mode, rank_fn in arms.items():
            rankings[mode].append(rank_fn(query["query"]))

    result: dict = {"overall": {}, "by_category": {}, "per_query": []}
    for mode, per_query in rankings.items():
        ranks = [first_hit_rank(r, q["gold_ids"]) for r, q in zip(per_query, queries)]
        reciprocal = [0.0 if rank is None else 1.0 / (rank + 1) for rank in ranks]
        result["overall"][mode] = {
            "recall_at_5": recall_at_5(per_query, queries),
            "mrr": round(sum(reciprocal) / len(reciprocal), 4),
        }
        for category in ALL_CATEGORIES:
            subset = [(r, q) for r, q in zip(ranks, queries) if q["category"] == category]
            sub_ranks = [r for r, _ in subset]
            result["by_category"].setdefault(category, {})[mode] = {
                "n": len(subset),
                "recall_at_5": round(
                    sum(1 for rank in sub_ranks if rank is not None and rank < TOP_K) / len(subset),
                    4,
                ),
            }
        for query, rank in zip(queries, ranks):
            result["per_query"].append(
                {
                    "id": query["id"],
                    "query": query["query"],
                    "category": query["category"],
                    "gold_ids": query["gold_ids"],
                    "mode": mode,
                    "rank": rank,
                    "hit_at_5": rank is not None and rank < TOP_K,
                }
            )
    return result


def collect_failures(connection: sqlite3.Connection, queries: list[dict], per_query: list[dict]) -> list[dict]:
    """组合臂（bigram_like）的逐条失败模式：漏在哪、FTS/LIKE 各自能否看到 gold。"""
    failures = []
    by_key = {(item["mode"], item["id"]): item for item in per_query}
    for query in queries:
        combined = by_key[("bigram_like", query["id"])]
        if combined["hit_at_5"]:
            continue
        tokens = bigram_tokens(query["query"])
        fts_rank = by_key[("bigram_only", query["id"])]["rank"]
        like_rank = by_key[("like_only", query["id"])]["rank"]
        fts_hits = len(ranked_ids(connection, "fts_bigram", fts_match_expr(tokens)))
        like_hits = len(like_ranking(connection, tokens))
        if fts_rank is None and like_rank is None:
            reason = "gold 两路都不可见：token 化后与原文无交集（分词/形态缺口）"
        elif fts_rank is None:
            reason = "FTS 词形不匹配（token 级不等），LIKE 可见但兜底后仍排不进 5"
        elif like_rank is None:
            reason = "LIKE 全 token AND 过严，FTS 可见但 OR 排序被竞争文档压出 5"
        else:
            reason = "两路都可见，但 OR/AND 排序被竞争文档压出 5"
        failures.append(
            {
                "id": query["id"],
                "query": query["query"],
                "category": query["category"],
                "gold_ids": query["gold_ids"],
                "tokens": tokens,
                "fts_rank": fts_rank,
                "like_rank": like_rank,
                "fts_total_hits": fts_hits,
                "like_total_hits": like_hits,
                "reason": reason,
            }
        )
    return failures


def render_report(result: dict, failures: list[dict], dataset_sizes: dict, payload: dict) -> str:
    label = ["bigram_only", "like_only", "bigram_like"]
    lines = [
        "# 标识符/路径/错误码查询召回评测（双字分词方案，未验证项 14）",
        "",
        f"- 生成时间：{payload['generated_at']}；SQLite {payload['sqlite']}；"
        f"记忆 {dataset_sizes['memories']} 条 / 查询 {dataset_sizes['queries']} 条（真实 SQLite :memory: FTS5）",
        "",
        "| 查询类别 | n | 纯 bigram(无兜底) | 纯 LIKE | bigram+LIKE兜底 |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for category in ALL_CATEGORIES:
        row = result["by_category"][category]
        lines.append(
            f"| {category} | {row['bigram_only']['n']}"
            + "".join(f" | {row[mode]['recall_at_5']:.4f}" for mode in label)
            + " |"
        )
    ident_totals = {}
    for mode in label:
        hits = sum(
            1
            for item in result["per_query"]
            if item["mode"] == mode and item["category"] in IDENTIFIER_CATEGORIES and item["hit_at_5"]
        )
        total = sum(
            1
            for item in result["per_query"]
            if item["mode"] == mode and item["category"] in IDENTIFIER_CATEGORIES
        )
        ident_totals[mode] = (hits, total)
    lines.append(
        f"| 标识符类合计 | {ident_totals['bigram_only'][1]}"
        + "".join(
            f" | {hits / total:.4f}" for hits, total in (ident_totals[mode] for mode in label)
        )
        + " |"
    )
    lines += [
        "",
        f"判据：标识符类 Recall@5 >= {P0_RECALL_THRESHOLD} → 双字方案对 P0 可用。",
        "",
        "## bigram+LIKE兜底 失败明细",
        "",
    ]
    if failures:
        for item in failures:
            lines.append(
                f"- [{item['category']}] {item['query']}：gold={item['gold_ids']}，"
                f"fts_rank={item['fts_rank']}，like_rank={item['like_rank']}，{item['reason']}"
            )
    else:
        lines.append("无失败查询。")
    return "\n".join(lines)


def main() -> int:
    dataset = json.loads((HERE / "datasets" / "identifiers.json").read_text(encoding="utf-8"))
    memories = dataset["memories"]
    queries = dataset["queries"]
    connection = build_db(memories)
    result = evaluate(connection, queries)
    failures = collect_failures(connection, queries, result["per_query"])
    (HERE / "results").mkdir(exist_ok=True)
    (HERE / "raw").mkdir(exist_ok=True)
    payload = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "sqlite": sqlite3.sqlite_version,
        "dataset": {"memories": len(memories), "queries": len(queries)},
        "p0_threshold": P0_RECALL_THRESHOLD,
        "identifier_categories": list(IDENTIFIER_CATEGORIES),
        "overall": result["overall"],
        "by_category": result["by_category"],
        "failures_bigram_like": failures,
        "per_query": result["per_query"],
    }
    report = render_report(result, failures, payload["dataset"], payload)
    (HERE / "results" / "results.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    (HERE / "raw" / "eval_run.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    (HERE / "results" / "results.md").write_text(report + "\n", encoding="utf-8")
    print(report)
    print("\nwritten: results/results.json, results/results.md, raw/eval_run.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
