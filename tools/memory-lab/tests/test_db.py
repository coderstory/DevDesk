"""存储层测试：schema 幂等、会话/事件写入、观察 roundtrip、FTS 触发器、统计。"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from memory_lab.config import load_config, strip_model_suffix  # noqa: E402
from memory_lab.db import (  # noqa: E402
    connect,
    get_observation,
    init_schema,
    insert_event,
    insert_observation,
    stats,
    upsert_session,
)

OBS = {
    "session_id": "s1",
    "project": "P--demo",
    "kind": "observation",
    "title": "项目代号确认",
    "subtitle": "蓝鲸项目",
    "narrative": "用户确认项目代号是蓝鲸，记忆库需要长期保存。",
    "facts": ["代号是蓝鲸", "项目目录在 D 盘"],
    "concepts": ["项目代号", "记忆库"],
    "files": ["docs/spec.md", "src/main.cpp"],
    "importance": 5,
    "created_at": "2026-10-26T10:00:00+00:00",
    "source_event_range": "s1:0-12",
}


class DbTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.db_path = self.tmp / "nested" / "memory.db"
        self.conn = connect(self.db_path)
        init_schema(self.conn)

    def tearDown(self) -> None:
        self.conn.close()
        self._tmp.cleanup()


class SchemaTests(DbTestCase):
    def test_connect_creates_parent_and_wal(self) -> None:
        self.assertTrue(self.db_path.exists())
        mode = self.conn.execute("PRAGMA journal_mode").fetchone()[0]
        self.assertEqual(mode.lower(), "wal")

    def test_init_schema_is_idempotent(self) -> None:
        init_schema(self.conn)
        init_schema(self.conn)
        tables = {
            row[0]
            for row in self.conn.execute(
                "SELECT name FROM sqlite_master WHERE type IN ('table','trigger')"
            )
        }
        for name in (
            "sessions",
            "events",
            "observations",
            "embeddings",
            "dream_proposals",
            "ingest_state",
            "meta",
            "fts_observations",
            "trg_obs_ai",
            "trg_obs_au",
            "trg_obs_ad",
        ):
            self.assertIn(name, tables)


class SessionEventTests(DbTestCase):
    def test_upsert_session_preserves_started_at(self) -> None:
        upsert_session(
            self.conn, "s1", "P", "a.jsonl",
            started_at="2026-10-26T10:00:00+00:00",
            ended_at="2026-10-26T10:01:00+00:00",
        )
        upsert_session(self.conn, "s1", "P", "a.jsonl", ended_at="2026-10-26T10:02:00+00:00")
        row = self.conn.execute("SELECT * FROM sessions WHERE id = 's1'").fetchone()
        self.assertEqual(row["started_at"], "2026-10-26T10:00:00+00:00")
        self.assertEqual(row["ended_at"], "2026-10-26T10:02:00+00:00")

    def test_insert_event_replaces_same_seq(self) -> None:
        insert_event(self.conn, "s1", 0, "t0", "user", "user", "old")
        insert_event(self.conn, "s1", 0, "t1", "user", "user", "new")
        self.conn.commit()
        rows = self.conn.execute("SELECT * FROM events WHERE session_id='s1'").fetchall()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["text"], "new")
        self.assertEqual(rows[0]["ts"], "t1")


class ObservationTests(DbTestCase):
    def test_roundtrip_lists(self) -> None:
        obs_id = insert_observation(self.conn, OBS)
        self.conn.commit()
        got = get_observation(self.conn, obs_id)
        self.assertIsNotNone(got)
        self.assertEqual(got["facts"], OBS["facts"])
        self.assertEqual(got["concepts"], OBS["concepts"])
        self.assertEqual(got["files"], OBS["files"])
        self.assertEqual(got["importance"], 5)
        raw = self.conn.execute(
            "SELECT facts, concepts, files FROM observations WHERE id=?", (obs_id,)
        ).fetchone()
        self.assertEqual(raw["facts"], "代号是蓝鲸\n项目目录在 D 盘")
        self.assertEqual(raw["concepts"], "项目代号, 记忆库")
        self.assertEqual(raw["files"], "docs/spec.md\nsrc/main.cpp")

    def test_get_missing_returns_none(self) -> None:
        self.assertIsNone(get_observation(self.conn, 9999))

    def test_insert_observation_defaults(self) -> None:
        obs_id = insert_observation(
            self.conn,
            {"session_id": "s1", "project": "P", "title": "最小观察"},
        )
        got = get_observation(self.conn, obs_id)
        self.assertEqual(got["kind"], "observation")
        self.assertEqual(got["importance"], 3)
        self.assertEqual(got["status"], "active")
        self.assertEqual(got["facts"], [])
        self.assertTrue(got["created_at"])


class FtsTests(DbTestCase):
    def _search(self, term: str) -> list[int]:
        rows = self.conn.execute(
            "SELECT o.id FROM fts_observations "
            "JOIN observations o ON o.id = fts_observations.rowid "
            "WHERE fts_observations MATCH ?",
            (term,),
        ).fetchall()
        return [int(r["id"]) for r in rows]

    def test_fts_insert_update_delete_triggers(self) -> None:
        obs_id = insert_observation(self.conn, OBS)
        self.conn.commit()
        self.assertIn(obs_id, self._search("记忆库"))
        self.assertIn(obs_id, self._search("长期保存"))

        self.conn.execute(
            "UPDATE observations SET narrative = ? WHERE id = ?",
            ("这一段改写成了完全不同的说法，提到检索质量。", obs_id),
        )
        self.conn.commit()
        self.assertEqual(self._search("长期保存"), [])
        self.assertIn(obs_id, self._search("检索质量"))

        self.conn.execute("DELETE FROM observations WHERE id = ?", (obs_id,))
        self.conn.commit()
        self.assertEqual(self._search("检索质量"), [])


class StatsTests(DbTestCase):
    def test_stats_counts_and_active_by_project(self) -> None:
        upsert_session(self.conn, "s1", "P--a", "a.jsonl")
        insert_event(self.conn, "s1", 0, None, "user", "user", "x")
        insert_observation(self.conn, OBS)
        insert_observation(self.conn, dict(OBS, title="另一条", project="P--b"))
        inactive_id = insert_observation(self.conn, dict(OBS, title="已合并"))
        self.conn.execute("UPDATE observations SET status='merged' WHERE id=?", (inactive_id,))
        self.conn.commit()

        data = stats(self.conn)
        self.assertEqual(data["sessions"], 1)
        self.assertEqual(data["events"], 1)
        self.assertEqual(data["observations"], 3)
        self.assertEqual(data["embeddings"], 0)
        self.assertEqual(data["proposals"], 0)
        self.assertEqual(data["active_by_project"], {"P--b": 1, "P--demo": 1})


class ConfigTests(unittest.TestCase):
    def test_strip_model_suffix(self) -> None:
        self.assertEqual(strip_model_suffix("qianfan/deepseek-v4.1-flash[1M]"), "qianfan/deepseek-v4.1-flash")
        self.assertEqual(strip_model_suffix("qianfan/deepseek-v4.1-flash[1m]"), "qianfan/deepseek-v4.1-flash")
        self.assertEqual(strip_model_suffix("qianfan/deepseek-v4.1-flash"), "qianfan/deepseek-v4.1-flash")

    def test_env_overrides(self) -> None:
        keys = {
            "MEMORY_LAB_HOME": str(Path(tempfile.gettempdir()) / "ml-home-test"),
            "MEMORY_LAB_CC_ROOT": str(Path(tempfile.gettempdir()) / "ml-cc-test"),
            "MEMORY_LAB_LLM_BASE_URL": "http://gateway.test",
            "MEMORY_LAB_LLM_TOKEN": "token-test",
            "MEMORY_LAB_LLM_MODEL": "qianfan/model[1M]",
        }
        old = {k: os.environ.get(k) for k in keys}
        try:
            os.environ.update(keys)
            cfg = load_config()
            self.assertEqual(str(cfg.home), keys["MEMORY_LAB_HOME"])
            self.assertEqual(cfg.db_path, cfg.home / "memory.db")
            self.assertEqual(str(cfg.cc_projects_root), keys["MEMORY_LAB_CC_ROOT"])
            self.assertEqual(cfg.llm_base_url, "http://gateway.test")
            self.assertEqual(cfg.llm_token, "token-test")
            self.assertEqual(cfg.llm_model, "qianfan/model")
        finally:
            for key, value in old.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value


if __name__ == "__main__":
    unittest.main()