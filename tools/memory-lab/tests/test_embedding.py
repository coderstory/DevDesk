"""嵌入层离线测试：HashEmbedder 确定性/归一化/余弦、编解码、存储助手。"""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import support  # noqa: E402

support.install_foundation_stubs()

from memory_lab.embedding import (  # noqa: E402
    EmbedderUnavailable,
    FastEmbedEmbedder,
    HashEmbedder,
    embed_pending_observations,
    get_embedder,
    load_embedding,
    observation_text,
    pack_vector,
    unpack_vector,
    upsert_embedding,
)


def cosine(a, b):
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    if na == 0 or nb == 0:
        return 0.0
    return dot / (na * nb)


class HashEmbedderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.embedder = HashEmbedder()

    def test_dim_and_determinism(self):
        self.assertEqual(self.embedder.dim, 512)
        first = self.embedder.embed(["今天天气很好，我们出门散步"])[0]
        second = HashEmbedder().embed(["今天天气很好，我们出门散步"])[0]
        self.assertEqual(len(first), 512)
        self.assertEqual(first, second)

    def test_l2_normalized_and_empty(self):
        vector = self.embedder.embed(["hello world 你好"])[0]
        norm = math.sqrt(sum(value * value for value in vector))
        self.assertAlmostEqual(norm, 1.0, places=5)
        zero = self.embedder.embed([""])[0]
        self.assertEqual(zero, [0.0] * 512)
        self.assertEqual(self.embedder.embed([]), [])

    def test_similar_texts_score_higher_than_unrelated(self):
        vectors = self.embedder.embed(
            [
                "今天天气很好，我们出门散步",
                "今天天气不错，我们一起去散步吧",
                "数据库索引优化与查询执行计划分析",
            ]
        )
        similar = cosine(vectors[0], vectors[1])
        unrelated = cosine(vectors[0], vectors[2])
        self.assertGreater(similar, 0.4)
        self.assertGreater(similar, unrelated + 0.3)

    def test_batch_preserves_order_and_length(self):
        texts = ["alpha", "beta", "gamma"]
        vectors = self.embedder.embed(texts)
        self.assertEqual(len(vectors), 3)
        self.assertNotEqual(vectors[0], vectors[1])
        self.assertEqual(vectors[0], self.embedder.embed(["alpha"])[0])


class VectorCodecTest(unittest.TestCase):
    def test_round_trip(self):
        original = [0.0, 1.5, -2.25, 3.125]
        self.assertEqual(unpack_vector(pack_vector(original)), original)

    def test_float32_precision_and_empty(self):
        packed = pack_vector([0.1])
        self.assertEqual(len(packed), 4)
        self.assertAlmostEqual(unpack_vector(packed)[0], 0.1, places=6)
        self.assertEqual(pack_vector([]), b"")
        self.assertEqual(unpack_vector(b""), [])

    def test_bad_blob_rejected(self):
        with self.assertRaises(ValueError):
            unpack_vector(b"\x00\x01\x02")


class EmbeddingStoreTest(unittest.TestCase):
    def setUp(self):
        self.conn = support.new_conn()

    def tearDown(self):
        self.conn.close()

    def test_upsert_and_load(self):
        upsert_embedding(self.conn, "observation", 7, "hash", [1.0, 0.0, 0.5])
        self.assertEqual(
            load_embedding(self.conn, "observation", 7, "hash"), [1.0, 0.0, 0.5]
        )
        upsert_embedding(self.conn, "observation", 7, "hash", [0.25, 1.0])
        self.assertEqual(
            load_embedding(self.conn, "observation", 7, "hash"), [0.25, 1.0]
        )
        count = self.conn.execute("SELECT COUNT(*) FROM embeddings").fetchone()[0]
        self.assertEqual(count, 1)
        self.assertIsNone(load_embedding(self.conn, "observation", 7, "other"))
        self.assertIsNone(load_embedding(self.conn, "observation", 99, "hash"))

    def test_embed_pending_observations(self):
        support.insert_observation_row(
            self.conn, "s1", "标题一", status="active", narrative="叙事一", facts="事实1\n事实2"
        )
        support.insert_observation_row(
            self.conn, "s1", "标题二", status="active", narrative="叙事二"
        )
        support.insert_observation_row(
            self.conn, "s1", "已合并", status="merged", narrative="不该嵌入"
        )
        embedder = HashEmbedder()
        processed = embed_pending_observations(self.conn, embedder)
        self.assertEqual(processed, 2)
        rows = self.conn.execute(
            "SELECT owner_type, owner_id, model, dim FROM embeddings ORDER BY owner_id"
        ).fetchall()
        self.assertEqual(len(rows), 2)
        self.assertTrue(all(row["owner_type"] == "observation" for row in rows))
        self.assertTrue(all(row["model"] == "hash" for row in rows))
        self.assertTrue(all(row["dim"] == 512 for row in rows))
        # 幂等：第二次没有待嵌入项
        self.assertEqual(embed_pending_observations(self.conn, embedder), 0)
        vector = load_embedding(self.conn, "observation", rows[0]["owner_id"], "hash")
        self.assertEqual(len(vector), 512)

    def test_embed_pending_limit(self):
        for index in range(3):
            support.insert_observation_row(self.conn, "s1", f"标题{index}")
        self.assertEqual(
            embed_pending_observations(self.conn, HashEmbedder(), limit=1), 1
        )
        self.assertEqual(
            self.conn.execute("SELECT COUNT(*) FROM embeddings").fetchone()[0], 1
        )

    def test_observation_text(self):
        self.assertEqual(
            observation_text({"title": "T", "narrative": "N", "facts": ["a", "b"]}),
            "T\nN\na\nb",
        )
        self.assertEqual(
            observation_text({"title": "T", "narrative": "", "facts": "x\ny"}),
            "T\nx\ny",
        )


class GetEmbedderTest(unittest.TestCase):
    def test_hash_variants(self):
        self.assertIsInstance(get_embedder(), HashEmbedder)
        self.assertIsInstance(get_embedder(None), HashEmbedder)
        self.assertIsInstance(get_embedder("hash"), HashEmbedder)

    def test_unknown_raises(self):
        with self.assertRaises(ValueError):
            get_embedder("no-such-backend")

    def test_fastembed_unavailable_is_explicit(self):
        try:
            import fastembed  # noqa: F401
        except ImportError:
            with self.assertRaises(EmbedderUnavailable):
                get_embedder("fastembed")
        else:  # 已安装时不联网下载模型，只确认类存在
            self.assertEqual(FastEmbedEmbedder.dim, 512)


if __name__ == "__main__":
    unittest.main()