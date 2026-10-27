"""标识符评测自身测试：数据集体例、门槛判定、兜底单调性。"""

from __future__ import annotations

import importlib.util
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import support  # noqa: E402

support.ensure_import_path()

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "eval_identifiers" / "eval_identifiers.py"
DATASET_PATH = ROOT / "eval_identifiers" / "datasets" / "identifiers.json"

_spec = importlib.util.spec_from_file_location("eval_identifiers_mod", MODULE_PATH)
mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mod)


class DatasetIntegrityTest(unittest.TestCase):
    """identifiers.json 冻结体例：规模、类别覆盖、gold 指向有效。"""

    @classmethod
    def setUpClass(cls):
        cls.dataset = json.loads(DATASET_PATH.read_text(encoding="utf-8"))
        cls.memories = cls.dataset["memories"]
        cls.queries = cls.dataset["queries"]

    def test_at_least_60_queries(self):
        self.assertGreaterEqual(len(self.queries), 60)

    def test_unique_ids(self):
        self.assertEqual(len({q["id"] for q in self.queries}), len(self.queries))
        self.assertEqual(len({m["id"] for m in self.memories}), len(self.memories))

    def test_all_categories_covered(self):
        categories = {q["category"] for q in self.queries}
        self.assertEqual(categories, set(mod.ALL_CATEGORIES))

    def test_gold_ids_valid(self):
        memory_ids = {m["id"] for m in self.memories}
        for query in self.queries:
            self.assertTrue(query["gold_ids"], query["id"])
            self.assertTrue(set(query["gold_ids"]) <= memory_ids, query["id"])
            self.assertIn(query["primary_id"], query["gold_ids"], query["id"])

    def test_memories_have_content(self):
        for memory in self.memories:
            self.assertTrue(memory["title"].strip(), memory["id"])
            self.assertTrue(memory["narrative"].strip(), memory["id"])


class IdentifierRecallGateTest(unittest.TestCase):
    """P0 门槛：标识符类 Recall@5 >= 0.8；兜底臂不得劣于纯 bigram 臂。"""

    @classmethod
    def setUpClass(cls):
        cls.exit_code = mod.main()
        cls.results = json.loads(
            (ROOT / "eval_identifiers" / "results" / "results.json").read_text(encoding="utf-8")
        )

    def _ident_recall(self, mode: str) -> float:
        items = [
            item
            for item in self.results["per_query"]
            if item["mode"] == mode and item["category"] in mod.IDENTIFIER_CATEGORIES
        ]
        return sum(1 for item in items if item["hit_at_5"]) / len(items)

    def test_main_returns_zero(self):
        self.assertEqual(self.exit_code, 0)

    def test_identifier_recall_at_5_meets_p0_threshold(self):
        recall = self._ident_recall("bigram_like")
        self.assertGreaterEqual(
            recall,
            mod.P0_RECALL_THRESHOLD,
            f"标识符类 Recall@5={recall} 低于 P0 门槛 {mod.P0_RECALL_THRESHOLD}，"
            "需给出失败模式与修补建议",
        )

    def test_fallback_not_worse_than_bigram_only(self):
        """兜底只补位不改排名：组合臂任一类别不得低于纯 bigram 臂。"""
        for category, arms in self.results["by_category"].items():
            self.assertGreaterEqual(
                arms["bigram_like"]["recall_at_5"],
                arms["bigram_only"]["recall_at_5"],
                category,
            )

    def test_category_counts_partition_queries(self):
        total = sum(
            arms["bigram_only"]["n"] for arms in self.results["by_category"].values()
        )
        self.assertEqual(total, len(self.results["per_query"]) // 3)

    def test_result_files_written(self):
        for rel in ("results/results.json", "results/results.md", "raw/eval_run.json"):
            self.assertTrue((ROOT / "eval_identifiers" / rel).exists(), rel)

    def test_sqlite_reported(self):
        self.assertIn("sqlite", self.results)
        self.assertTrue(self.results["sqlite"])


if __name__ == "__main__":
    unittest.main()
