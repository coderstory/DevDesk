"""红队评测脚本自身测试：数据集体例、门槛判定、raise 行为。"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import support  # noqa: E402

support.ensure_import_path()

from sanitize import eval_redteam  # noqa: E402


class DatasetIntegrityTest(unittest.TestCase):
    """红队数据集体例：冻结资产的质量底线。"""

    @classmethod
    def setUpClass(cls):
        cls.dataset = eval_redteam.load_dataset()

    def test_at_least_100_entries(self):
        self.assertGreaterEqual(len(self.dataset), eval_redteam.MIN_SAMPLES)

    def test_unique_ids(self):
        ids = [e["id"] for e in self.dataset]
        self.assertEqual(len(ids), len(set(ids)))

    def test_positive_has_verbatim_secrets_and_categories(self):
        for entry in self.dataset:
            if entry["kind"] != "positive":
                continue
            self.assertTrue(entry["secrets"], entry["id"])
            for secret in entry["secrets"]:
                self.assertIn(secret, entry["text"], entry["id"])
            self.assertTrue(entry["expect_categories"], entry["id"])

    def test_has_both_kinds_and_variants(self):
        kinds = {e["kind"] for e in self.dataset}
        self.assertEqual(kinds, {"positive", "negative"})
        negatives = [e for e in self.dataset if e["kind"] == "negative"]
        self.assertGreaterEqual(len(negatives), 10)
        # 变体覆盖：全角、分隔符、前缀至少各出现一次
        text = "".join(e["text"] for e in self.dataset)
        for marker in ("１３８", "１１０１", "138-1234-5678", "+8613812345678"):
            self.assertIn(marker, text)


class GateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stats = eval_redteam.run_redteam()

    def test_leak_zero(self):
        self.assertEqual(self.stats["leaks"], [], "正例泄漏必须为 0")

    def test_category_miss_zero(self):
        self.assertEqual(self.stats["category_misses"], [], "期望类别必须至少命中一个")

    def test_misfire_rate_reported_and_under_threshold(self):
        self.assertIn("misfire_rate", self.stats)
        self.assertLessEqual(self.stats["misfire_rate"], self.stats["threshold"])

    def test_idempotent(self):
        self.assertEqual(self.stats["non_idempotent"], [])

    def test_main_passes_on_current_dataset(self):
        self.assertEqual(eval_redteam.main(), 0)


def _pad_to_min(entries: list[dict]) -> list[dict]:
    """凑满样本量门槛，让注入用例打到泄漏/误杀分支而不是样本量分支。"""
    base = len(entries)
    padded = list(entries)
    for i in range(eval_redteam.MIN_SAMPLES - base + 1):
        padded.append({"id": f"pad{i}", "kind": "negative", "category": "benign", "text": "普通句子", "note": ""})
    return padded


class GateRaisesTest(unittest.TestCase):
    """门槛必须 raise，不许 print 完就过。"""

    def test_leak_triggers_raise(self):
        # 脱敏层拦不住的形态（非任何已知模式），泄漏必须触发 AssertionError
        poisoned = [
            {
                "id": "x01",
                "kind": "positive",
                "category": "secret",
                "text": "somewhere leakme1234567 stays",
                "secrets": ["leakme1234567"],
                "expect_categories": ["secret"],
                "note": "注入泄漏样本",
            }
        ]
        stats = eval_redteam.run_redteam(_pad_to_min(poisoned))
        self.assertEqual(len(stats["leaks"]), 1)
        with self.assertRaises(AssertionError):
            eval_redteam.ensure_gate(stats)

    def test_misfire_triggers_raise(self):
        # 伪造统计：全部误杀，超过阈值必须 raise
        stats = {
            "total": 101,
            "positives": 0,
            "negatives": 101,
            "leaks": [],
            "category_misses": [],
            "misfires": [{"id": f"k{i}"} for i in range(101)],
            "misfire_count": 101,
            "misfire_rate": 1.0,
            "threshold": eval_redteam.THRESHOLD_MISFIRE_RATE,
            "non_idempotent": [],
        }
        with self.assertRaises(AssertionError):
            eval_redteam.ensure_gate(stats)
        # 而真实跑这组负例不应误杀
        real = eval_redteam.run_redteam(
            [
                {"id": "k1", "kind": "negative", "category": "benign", "text": "13800000000", "note": ""},
                {"id": "k2", "kind": "negative", "category": "benign", "text": "127.0.0.1", "note": ""},
            ]
        )
        self.assertEqual(real["misfire_count"], 0)

    def test_sample_count_triggers_raise(self):
        stats = dict(eval_redteam.run_redteam())
        stats["total"] = 1
        with self.assertRaises(AssertionError):
            eval_redteam.ensure_gate(stats)


if __name__ == "__main__":
    unittest.main()
