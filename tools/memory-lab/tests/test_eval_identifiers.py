"""标识符评测自身测试：数据集体例、门槛判定、兜底护栏（变异 A/B 实证对应项）。

护栏设计（审计 F4/F5）：
- 构造性用例钉死兜底路径：bigram 臂 0 命中时必须由规格书 LIKE OR 兜底命中，
  触发标记与计数随 results.json 导出并被断言——注释掉兜底必红，不许恒真。
- zh_short 类门槛与标识符类同标准（审计变异 B：CJK 分词断裂不得静默绿灯）。
"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
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
    """P0 门槛：标识符类与 zh_short Recall@5 >= 0.8；组合臂强单调于两臂。

    产物写 tempfile（审计 F6）：测试跑一次不得弄脏仓库 results/、raw/。
    """

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.out_dir = Path(cls._tmp.name)
        cls.exit_code = mod.main(cls.out_dir)
        cls.results = json.loads(
            (cls.out_dir / "results" / "results.json").read_text(encoding="utf-8")
        )
        dataset = json.loads(DATASET_PATH.read_text(encoding="utf-8"))
        cls.memories = dataset["memories"]
        cls.queries = dataset["queries"]

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

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

    def test_zh_short_recall_at_5_meets_p0_threshold(self):
        """审计 F5/变异 B 护栏：zh_short 门槛与标识符类同标准，不许只锁 ASCII 类。"""
        recall = self.results["by_category"]["zh_short"]["bigram_like"]["recall_at_5"]
        self.assertGreaterEqual(
            recall,
            mod.P0_RECALL_THRESHOLD,
            f"zh_short Recall@5={recall} 低于 P0 门槛 {mod.P0_RECALL_THRESHOLD}，"
            "CJK 短查询召回断裂，需给失败模式",
        )

    def test_combined_dominates_both_arms(self):
        """强单调（审计 F4）：组合臂任一类别 >= max(bigram 臂, like 臂)。

        旧断言 combined>=bigram 在两臂同分时恒真；这里把 like 臂也压进下界。
        """
        for category, arms in self.results["by_category"].items():
            floor = max(arms["bigram_only"]["recall_at_5"], arms["like_only"]["recall_at_5"])
            self.assertGreaterEqual(
                arms["bigram_like"]["recall_at_5"],
                floor,
                f"{category}: 组合臂 {arms['bigram_like']['recall_at_5']} 低于两臂最大值 {floor}",
            )

    def test_fallback_trigger_count_consistent_with_fts_emptiness(self):
        """导出计数自身受检：fallback_used/总数 必须等于独立重算的「FTS 臂 0 行」集合。

        不许靠口径复述糊弄——这里绕开 combined_ranking，直接用冻结契约函数重算。
        """
        connection = mod.build_db(self.memories)
        expected_ids = {
            query["id"]
            for query in self.queries
            if not mod.ranked_ids(
                connection, "fts_bigram", mod.fts_match_expr(mod.bigram_tokens(query["query"]))
            )
        }
        used_ids = {
            entry["id"]
            for entry in self.results["per_query"]
            if entry["mode"] == "bigram_like" and entry["fallback_used"]
        }
        self.assertEqual(used_ids, expected_ids)
        self.assertEqual(self.results["fallback_triggers"], len(expected_ids))

    def test_category_counts_partition_queries(self):
        total = sum(
            arms["bigram_only"]["n"] for arms in self.results["by_category"].values()
        )
        self.assertEqual(total, len(self.results["per_query"]) // 3)

    def test_result_files_written(self):
        for rel in ("results/results.json", "results/results.md", "raw/eval_run.json"):
            self.assertTrue((self.out_dir / rel).exists(), rel)

    def test_sqlite_reported(self):
        self.assertIn("sqlite", self.results)
        self.assertTrue(self.results["sqlite"])


class FallbackGuardTest(unittest.TestCase):
    """审计 F4 构造性护栏：兜底死亡（变异 A）必须红。

    构造「bigram 臂 0 命中」的微型库：查询 token 是 gold 文档某 token 的子串但非等值 token
    （memstore ⊂ memstorex），FTS 词级匹配 0 行，LIKE 子串匹配可见——兜底是唯一活路。
    第二个 token zzzz 不在任何文档里：旧实现的「全 token AND」兜底在此 0 命中，
    规格书的「token OR」兜底命中——OR 语义一并被钉死。
    """

    MEMORIES = [
        {
            "id": 1,
            "title": "记忆存储层入口",
            "narrative": "memstorex 是记忆存储层唯一入口",
            "facts": [],
            "concepts": [],
        },
        {
            "id": 2,
            "title": "无关夹具",
            "narrative": "completely unrelated decoy content",
            "facts": [],
            "concepts": [],
        },
        {
            "id": 3,
            "title": "双命中夹具",
            "narrative": "memstorex storehouse 聚合入口",
            "facts": [],
            "concepts": [],
        },
    ]
    QUERY = "memstore zzzz"
    # 6 类各 1 条，让 mod.evaluate 的分类别分母非空
    QUERIES = [
        {"id": f"g{i}", "query": "memstore zzzz", "gold_ids": [1], "primary_id": 1, "category": cat}
        for i, cat in enumerate(mod.ALL_CATEGORIES, start=1)
    ]

    @classmethod
    def setUpClass(cls):
        cls.connection = mod.build_db(cls.MEMORIES)

    def test_bigram_arm_sees_nothing(self):
        """前提成立：冻结 FTS 契约对这些查询必须 0 行，否则用例测不到兜底。"""
        tokens = mod.bigram_tokens(self.QUERY)
        self.assertEqual(tokens, ["memstore", "zzzz"])
        self.assertEqual(
            mod.ranked_ids(self.connection, "fts_bigram", mod.fts_match_expr(tokens)),
            [],
            "前置失真：bigram 臂非空则本护栏没测到兜底路径",
        )

    def test_and_semantics_sees_nothing(self):
        """全 token AND（旧兜底实现）在此查询 0 命中：兜底若误用 AND 本用例必红。"""
        self.assertEqual(mod.like_ranking(self.connection, ["memstore", "zzzz"]), [])

    def test_fallback_triggers_and_counts_hit(self):
        """兜底被真实触发、命中被计入指标、计数随结果导出——三处都断言。"""
        ranking, triggered = mod.combined_ranking(self.connection, self.QUERY)
        self.assertTrue(triggered, "兜底未触发：gold 命中若存在只能来自 FTS，护栏失效")
        self.assertIn(1, ranking)

        result = mod.evaluate(self.connection, self.QUERIES)
        self.assertEqual(result["overall"]["bigram_only"]["recall_at_5"], 0.0)
        self.assertEqual(result["overall"]["bigram_like"]["recall_at_5"], 1.0)
        self.assertEqual(result["fallback_triggers"], len(self.QUERIES))
        used = [
            entry
            for entry in result["per_query"]
            if entry["mode"] == "bigram_like" and entry["fallback_used"]
        ]
        self.assertEqual(len(used), len(self.QUERIES))

    def test_fallback_orders_by_token_hit_count(self):
        """规格书 :195 排序口径：命中 token 数多者在前（doc3 双命中 > doc1 单命中）。"""
        tokens = mod.bigram_tokens("memstore house")
        self.assertEqual(
            mod.ranked_ids(self.connection, "fts_bigram", mod.fts_match_expr(tokens)), []
        )
        self.assertEqual(mod.spec_like_fallback(self.connection, tokens), [3, 1])


class FrozenContractPinTest(unittest.TestCase):
    """审计变异 B 直接护栏：冻结 bigram_tokens 的中文重叠双字契约不许被改单字切分。"""

    def test_cjk_runs_split_into_overlapping_bigrams(self):
        self.assertEqual(mod.bigram_tokens("建表"), ["建表"])
        tokens = mod.bigram_tokens("端口配置")
        for expected in ("端口", "口配", "配置"):
            self.assertIn(expected, tokens)

    def test_single_cjk_char_stays_single(self):
        self.assertEqual(mod.bigram_tokens("锁"), ["锁"])


if __name__ == "__main__":
    unittest.main()
