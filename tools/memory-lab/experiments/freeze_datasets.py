"""把评测所需的数据集冻结落盘（只做一次，之后评测只读这些文件）。

输入：
- raw/mem_obs_batch.json：2026-10-26/27 抽取实验的原始产出（31 条）
- raw/query_generation.json：generate_queries.py 保存的 LLM 响应（若不存在则跳过查询集）

输出：
- observations.json：按标题去重后的 27 条记忆（id 0..26，稳定顺序）
- queries.json：15 条冻结查询 + 等价答案组（gold_ids）

等价答案组说明：27 条记忆里有多组同义重复（例如 4/10/18 都是"MCP 端口在设置-通用"），
若只用单条 gold，重复条目会让 MRR 系统性偏低。gold_ids 是人工标注的语义等价集合，
命中任意一条都算正确；primary_id 保留出题目标本身。
"""

from __future__ import annotations

import json
from pathlib import Path

HERE = Path(__file__).resolve().parent

# 人工标注的语义等价组（按 observations.json 的 id）；组间可重叠。
GOLD_GROUPS = {
    "ask_matt": [0],
    "subagent_driven": [1],
    "main_session_orchestrates": [2],
    "worktree": [3, 9, 16, 26],
    "mcp_enable_port": [4, 10, 18, 22, 23],
    "cross_platform": [5, 13, 19, 25],
    "mcp_abstract_interface": [6, 11, 24],
    "plan_before_code": [7, 12, 21, 26],
    "hello_world_mcp": [8, 14, 20, 24],
    "mcp_skeleton": [5, 13, 15, 19],
    "devdesk_mcp_module": [3, 9, 17, 22, 23],
}
# 出题目标 id -> 等价组名（queries.json 的 gold_ids 由此展开）
TARGET_GROUP = {
    0: "ask_matt",
    1: "subagent_driven",
    2: "main_session_orchestrates",
    3: "worktree",
    4: "mcp_enable_port",
    5: "cross_platform",
    6: "mcp_abstract_interface",
    7: "plan_before_code",
    8: "hello_world_mcp",
    9: "worktree",
    10: "mcp_enable_port",
    11: "mcp_abstract_interface",
    12: "plan_before_code",
    13: "mcp_skeleton",
    14: "hello_world_mcp",
}


def build_observations() -> list[dict]:
    raw = json.loads((HERE / "raw" / "mem_obs_batch.json").read_text(encoding="utf-8"))
    seen: set[str] = set()
    observations: list[dict] = []
    for item in raw:
        title = (item.get("title") or "").strip()
        if not title or title in seen:
            continue
        seen.add(title)
        observations.append(
            {
                "id": len(observations),
                "kind": item.get("kind", "fact"),
                "title": title,
                "narrative": (item.get("narrative") or "").strip(),
                "facts": item.get("facts") or [],
                "concepts": item.get("concepts") or [],
                "importance": item.get("importance", 3),
                "evidence": item.get("evidence", ""),
                "source": {"session": item.get("_session", ""), "segment": item.get("_seg", -1)},
            }
        )
    return observations


def build_queries() -> list[dict] | None:
    raw_path = HERE / "raw" / "queries_llm_only.json"
    if not raw_path.exists():
        return None
    queries = json.loads(raw_path.read_text(encoding="utf-8"))
    for query in queries:
        group = TARGET_GROUP.get(int(query["target_id"]))
        if group is None:
            raise SystemExit(f"no gold group for target {query['target_id']}")
        query["gold_ids"] = sorted(GOLD_GROUPS[group])
        query["gold_group"] = group
        query["primary_id"] = int(query["target_id"])
    return queries


def main() -> None:
    observations = build_observations()
    (HERE / "observations.json").write_text(
        json.dumps(observations, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    print(f"observations: {len(observations)} entries")
    queries = build_queries()
    if queries is None:
        print("queries: skipped (raw/queries_llm_only.json missing)")
        return
    (HERE / "queries.json").write_text(
        json.dumps(queries, ensure_ascii=False, indent=1), encoding="utf-8"
    )
    print(f"queries: {len(queries)} frozen entries")


if __name__ == "__main__":
    main()