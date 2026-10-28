"""红队评测：feasibility §5 风险 8（云端外发泄漏）的可执行门槛。

判定契约：
- 正例泄漏必须为 0（敏感子串残留在脱敏输出中，或期望类别一个都没命中，都算泄漏）；
- 负例误杀率给出数字，超过阈值即 raise；
- 断言失败必须 raise AssertionError，本脚本不以退出码 0 掩盖泄漏。

运行::

    python sanitize/eval_redteam.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/memory-lab

from sanitize import sanitize  # noqa: E402

MIN_SAMPLES = 100
THRESHOLD_MISFIRE_RATE = 0.05
_VALID_KINDS = {"positive", "negative"}


def dataset_path() -> Path:
    return Path(__file__).resolve().parent / "datasets" / "redteam.json"


def load_dataset(path: Path | None = None) -> list[dict]:
    with open(path or dataset_path(), encoding="utf-8") as f:
        return json.load(f)


def _check_schema(dataset: list[dict]) -> None:
    ids = [entry["id"] for entry in dataset]
    if len(ids) != len(set(ids)):
        raise AssertionError("红队数据集 id 重复")
    for entry in dataset:
        if entry["kind"] not in _VALID_KINDS:
            raise AssertionError(f"{entry['id']}: 非法 kind={entry['kind']}")
        if entry["kind"] == "positive":
            if not entry.get("secrets"):
                raise AssertionError(f"{entry['id']}: 正例缺少 secrets")
            for secret in entry["secrets"]:
                if secret not in entry["text"]:
                    raise AssertionError(f"{entry['id']}: secrets 不在 text 中（自洽性破坏）")
            if not entry.get("expect_categories"):
                raise AssertionError(f"{entry['id']}: 正例缺少 expect_categories")


def run_redteam(dataset: list[dict] | None = None) -> dict:
    """跑全样本，返回统计与失败明细；不做判定（判定在 ensure_gate）。"""
    if dataset is None:
        dataset = load_dataset()
    _check_schema(dataset)
    positives = [e for e in dataset if e["kind"] == "positive"]
    negatives = [e for e in dataset if e["kind"] == "negative"]

    leaks: list[dict] = []
    category_misses: list[dict] = []
    misfires: list[dict] = []
    non_idempotent: list[dict] = []

    for entry in positives:
        result = sanitize(entry["text"])
        for secret in entry["secrets"]:
            if secret in result.text:
                leaks.append({"id": entry["id"], "secret_preview": secret[:8] + "…"})
        if not set(entry["expect_categories"]) & set(result.categories):
            category_misses.append(
                {"id": entry["id"], "expect": entry["expect_categories"], "got": result.categories}
            )
        once = sanitize(entry["text"]).text
        if sanitize(once).text != once:
            non_idempotent.append({"id": entry["id"]})

    for entry in negatives:
        result = sanitize(entry["text"])
        if result.hits or result.text != entry["text"]:
            misfires.append(
                {"id": entry["id"], "hits": [h.category for h in result.hits]}
            )

    misfire_rate = len(misfires) / len(negatives) if negatives else 0.0
    return {
        "total": len(dataset),
        "positives": len(positives),
        "negatives": len(negatives),
        "leaks": leaks,
        "category_misses": category_misses,
        "misfires": misfires,
        "misfire_count": len(misfires),
        "misfire_rate": misfire_rate,
        "threshold": THRESHOLD_MISFIRE_RATE,
        "non_idempotent": non_idempotent,
    }


def ensure_gate(stats: dict) -> None:
    """feasibility 风险 8 出口条件：泄漏=0。任何一条不过都 raise。"""
    if stats["total"] < MIN_SAMPLES:
        raise AssertionError(f"红队样本 {stats['total']} < {MIN_SAMPLES}")
    if stats["leaks"]:
        raise AssertionError(
            f"正例泄漏 {len(stats['leaks'])} > 0，禁止启用云端抽取：{stats['leaks']}"
        )
    if stats["category_misses"]:
        raise AssertionError(f"正例期望类别未命中 {len(stats['category_misses'])} 条：{stats['category_misses']}")
    if stats["misfire_rate"] > stats["threshold"]:
        raise AssertionError(
            f"负例误杀率 {stats['misfire_rate']:.3f} > 阈值 {stats['threshold']}：{stats['misfires']}"
        )
    if stats["non_idempotent"]:
        raise AssertionError(f"脱敏不幂等：{stats['non_idempotent']}")


def main() -> int:
    stats = run_redteam()
    ensure_gate(stats)
    print(
        f"redteam: total={stats['total']} positives={stats['positives']} "
        f"negatives={stats['negatives']} leaks={len(stats['leaks'])} "
        f"category_misses={len(stats['category_misses'])} "
        f"misfires={stats['misfire_count']}/{stats['negatives']} "
        f"misfire_rate={stats['misfire_rate']:.3f} (阈值 {stats['threshold']}) -> PASS"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
