"""向量嵌入层：零依赖 HashEmbedder + 可选 FastEmbed 后端 + 存储助手。

HashEmbedder 是完全确定性的特征哈希（blake2b，每字符 1-3 gram + 词哈希，
n-gram 权重随长度递减，符号哈希 + L2 归一化），512 维，跨进程/跨机器一致，
用于没有第三方依赖时的近似语义检索。

FastEmbedEmbedder 依赖 ``fastembed`` 与 HuggingFace 模型
``BAAI/bge-small-zh-v1.5``（512 维）；不可用时由 :func:`get_embedder`
抛 :class:`EmbedderUnavailable`，调用方决定是否降级。
"""

from __future__ import annotations

import hashlib
import math
import re
import struct
from typing import Protocol, runtime_checkable

HASH_DIM = 512
_GRAM_WEIGHTS = {1: 1.0, 2: 0.6, 3: 0.3}
_WORD_WEIGHT = 1.2
_CJK_RANGES = "㐀-䶿一-鿿"
_WORD_RE = re.compile(r"[a-z0-9_]+")
_CJK_RUN_RE = re.compile(f"[{_CJK_RANGES}]+")
_OWNER_OBSERVATION = "observation"


class EmbedderUnavailable(RuntimeError):
    """请求的嵌入后端不可用（未安装/模型下载失败等）。"""


@runtime_checkable
class Embedder(Protocol):
    name: str
    dim: int

    def embed(self, texts: list[str]) -> list[list[float]]:
        ...


def _bucket(token: str, dim: int) -> tuple[int, float]:
    digest = hashlib.blake2b(token.encode("utf-8"), digest_size=8).digest()
    value = int.from_bytes(digest, "little")
    index = value % dim
    sign = 1.0 if (value >> 63) & 1 else -1.0
    return index, sign


class HashEmbedder:
    """确定性特征哈希嵌入（512 维，L2 归一化）。"""

    name = "hash"
    dim = HASH_DIM

    def _tokens(self, text: str):
        normalized = (text or "").casefold()
        # 词哈希：拉丁词 + 单个汉字
        for word in _WORD_RE.findall(normalized):
            yield word, _WORD_WEIGHT
        # 字符 1-3 gram：拉丁词内部与连续汉字串
        segments = _WORD_RE.findall(normalized)
        segments.extend(_CJK_RUN_RE.findall(normalized))
        for segment in segments:
            length = len(segment)
            for n in (1, 2, 3):
                if length < n:
                    continue
                weight = _GRAM_WEIGHTS[n]
                for start in range(length - n + 1):
                    yield segment[start : start + n], weight

    def embed(self, texts: list[str]) -> list[list[float]]:
        vectors: list[list[float]] = []
        for text in texts or []:
            vector = [0.0] * self.dim
            for token, weight in self._tokens(str(text)):
                index, sign = _bucket(token, self.dim)
                vector[index] += sign * weight
            norm = math.sqrt(sum(value * value for value in vector))
            if norm > 0.0:
                vector = [value / norm for value in vector]
            vectors.append(vector)
        return vectors


class FastEmbedEmbedder:
    """fastembed 后端（BAAI/bge-small-zh-v1.5，512 维，首次使用会下载模型）。"""

    name = "fastembed:BAAI/bge-small-zh-v1.5"
    dim = 512

    def __init__(self, model_name: str = "BAAI/bge-small-zh-v1.5"):
        try:
            from fastembed import TextEmbedding
        except Exception as exc:  # noqa: BLE001 - 缺包/依赖损坏统一视为不可用
            raise EmbedderUnavailable(f"fastembed 不可用：{exc}") from exc
        self.model_name = model_name
        try:
            self._model = TextEmbedding(model_name=model_name)
        except Exception as exc:  # noqa: BLE001 - 模型下载/加载失败
            raise EmbedderUnavailable(
                f"fastembed 模型 {model_name} 加载失败：{exc}"
            ) from exc

    def embed(self, texts: list[str]) -> list[list[float]]:
        if not texts:
            return []
        vectors: list[list[float]] = []
        for raw in self._model.embed([str(text) for text in texts]):
            vectors.append([float(value) for value in raw])
        return vectors


def get_embedder(name: str | None = None) -> Embedder:
    """按名字取嵌入后端：``None``/``"hash"`` 用 HashEmbedder，``"fastembed"`` 尝试 FastEmbed。"""
    normalized = (name or "hash").strip().lower()
    if normalized in ("", "hash"):
        return HashEmbedder()
    if normalized == "fastembed":
        return FastEmbedEmbedder()
    raise ValueError(f"未知的嵌入后端：{name!r}（可选 hash / fastembed）")


# ---------------------------------------------------------------------------
# 向量编解码
# ---------------------------------------------------------------------------

def pack_vector(vec: list[float]) -> bytes:
    """float32 小端打包。"""
    values = [float(value) for value in (vec or [])]
    return struct.pack(f"<{len(values)}f", *values)


def unpack_vector(blob: bytes) -> list[float]:
    """float32 小端解包。"""
    if blob is None:
        raise ValueError("向量 blob 为空")
    data = bytes(blob)
    if len(data) % 4 != 0:
        raise ValueError(f"向量 blob 长度 {len(data)} 不是 4 的倍数")
    count = len(data) // 4
    return list(struct.unpack(f"<{count}f", data))


# ---------------------------------------------------------------------------
# 存储
# ---------------------------------------------------------------------------

def upsert_embedding(
    conn, owner_type: str, owner_id: int, model: str, vec: list[float]
) -> None:
    """写入/更新一条向量（embeddings 表有 UNIQUE(owner_type, owner_id, model)）。"""
    blob = pack_vector(vec)
    conn.execute(
        """
        INSERT INTO embeddings(owner_type, owner_id, model, dim, vector)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(owner_type, owner_id, model) DO UPDATE SET
          dim = excluded.dim,
          vector = excluded.vector
        """,
        (owner_type, int(owner_id), model, len(vec or []), blob),
    )


def load_embedding(
    conn, owner_type: str, owner_id: int, model: str
) -> list[float] | None:
    """读取向量；不存在返回 None。"""
    row = conn.execute(
        "SELECT vector FROM embeddings "
        "WHERE owner_type = ? AND owner_id = ? AND model = ?",
        (owner_type, int(owner_id), model),
    ).fetchone()
    if row is None:
        return None
    blob = row["vector"] if hasattr(row, "keys") else row[0]
    return unpack_vector(blob)


def observation_text(obs: dict) -> str:
    """观测的嵌入文本：title + narrative + facts（与契约一致）。"""
    facts = obs.get("facts")
    if isinstance(facts, (list, tuple)):
        facts_text = "\n".join(str(item) for item in facts if str(item).strip())
    else:
        facts_text = str(facts or "")
    parts = [
        str(obs.get("title") or "").strip(),
        str(obs.get("narrative") or "").strip(),
        facts_text.strip(),
    ]
    return "\n".join(part for part in parts if part)


def _row_get(row, name: str, index: int):
    if hasattr(row, "keys"):
        return row[name]
    return row[index]


def embed_pending_observations(conn, embedder: Embedder, limit: int | None = None) -> int:
    """给缺该模型向量的 active 观测补嵌入，返回处理条数。"""
    sql = """
        SELECT o.id, o.title, o.narrative, o.facts
        FROM observations o
        WHERE o.status = 'active'
          AND NOT EXISTS (
            SELECT 1 FROM embeddings e
            WHERE e.owner_type = ? AND e.owner_id = o.id AND e.model = ?
          )
        ORDER BY o.id
    """
    params: list = [_OWNER_OBSERVATION, embedder.name]
    if limit is not None:
        sql += " LIMIT ?"
        params.append(int(limit))
    rows = conn.execute(sql, params).fetchall()
    if not rows:
        return 0

    processed = 0
    batch_size = 32
    for start in range(0, len(rows), batch_size):
        batch = rows[start : start + batch_size]
        observations = [
            {
                "id": int(_row_get(row, "id", 0)),
                "title": _row_get(row, "title", 1),
                "narrative": _row_get(row, "narrative", 2),
                "facts": _row_get(row, "facts", 3),
            }
            for row in batch
        ]
        texts = [observation_text(obs) for obs in observations]
        vectors = embedder.embed(texts)
        if len(vectors) != len(observations):
            raise RuntimeError(
                f"嵌入后端返回数量不符：期望 {len(observations)}，实际 {len(vectors)}"
            )
        for obs, vector in zip(observations, vectors):
            upsert_embedding(conn, _OWNER_OBSERVATION, obs["id"], embedder.name, vector)
            processed += 1
    conn.commit()
    return processed