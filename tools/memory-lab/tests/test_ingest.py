"""摄取测试：扫描、事件抽取、幂等、增量、截断、半行、批量汇总。"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from memory_lab.config import Config  # noqa: E402
from memory_lab.db import connect, init_schema  # noqa: E402
from memory_lab.ingest import (  # noqa: E402
    ingest,
    ingest_file,
    scan_sessions,
)
from memory_lab.models import SessionFile  # noqa: E402

PROJECT = "P--demo"

RECORDS = [
    {
        "type": "user",
        "timestamp": "2026-10-26T10:00:00Z",
        "message": {"role": "user", "content": "请记住：项目代号是蓝鲸。"},
    },
    {
        "type": "assistant",
        "timestamp": "2026-10-26T10:00:05Z",
        "message": {
            "role": "assistant",
            "content": [
                {"type": "thinking", "thinking": "用户给出了项目代号"},
                {"type": "text", "text": "记住了。"},
                {
                    "type": "tool_use",
                    "id": "tool-1",
                    "name": "Bash",
                    "input": {"command": "ls"},
                },
            ],
        },
    },
    {
        "type": "user",
        "timestamp": "2026-10-26T10:00:06Z",
        "message": {
            "role": "user",
            "content": [
                {
                    "type": "tool_result",
                    "tool_use_id": "tool-1",
                    "content": [{"type": "text", "text": "a.txt"}],
                }
            ],
        },
    },
    {"type": "mode", "timestamp": "2026-10-26T10:00:07Z"},
    {"type": "attachment", "timestamp": "2026-10-26T10:00:08Z"},
]


class IngestTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cc_root = self.tmp / "projects"
        self.project_dir = self.cc_root / PROJECT
        self.project_dir.mkdir(parents=True)
        self.cfg = Config(
            home=self.tmp / "home",
            db_path=self.tmp / "home" / "memory.db",
            cc_projects_root=self.cc_root,
            llm_base_url="",
            llm_token="",
            llm_model="test-model",
        )
        self.conn = connect(self.cfg.db_path)
        init_schema(self.conn)

    def tearDown(self) -> None:
        self.conn.close()
        self._tmp.cleanup()

    # -- helpers ---------------------------------------------------------
    def _write(self, name: str, lines: list[str], newline: bool = True) -> Path:
        path = self.project_dir / name
        data = "\n".join(lines) + ("\n" if newline else "")
        path.write_text(data, encoding="utf-8")
        return path

    def _session_file(self, path: Path) -> SessionFile:
        st = path.stat()
        return SessionFile(
            path=path,
            project=PROJECT,
            session_id=path.stem,
            size=st.st_size,
            mtime=st.st_mtime,
        )

    def _count(self, table: str) -> int:
        return int(self.conn.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])

    def _lines(self) -> list[str]:
        return [json.dumps(r, ensure_ascii=False) for r in RECORDS]

    def _ingest_named(self, path: Path):
        return ingest_file(self.conn, self.cfg, self._session_file(path))

    # -- tests -----------------------------------------------------------
    def test_scan_sessions(self) -> None:
        path = self._write("sess-1.jsonl", self._lines())
        found = scan_sessions(self.cfg, project=PROJECT)
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].session_id, "sess-1")
        self.assertEqual(found[0].project, PROJECT)
        self.assertEqual(found[0].size, path.stat().st_size)

    def test_scan_sessions_limit_and_since(self) -> None:
        older = self._write("older.jsonl", self._lines())
        newer = self._write("newer.jsonl", self._lines())
        os.utime(older, (1_000_000_000, 1_000_000_000))
        os.utime(newer, (1_000_000_100, 1_000_000_100))

        latest = scan_sessions(self.cfg, project=PROJECT, limit=1)
        self.assertEqual([s.session_id for s in latest], ["newer"])

        recent = scan_sessions(self.cfg, project=PROJECT, since=1_000_000_050)
        self.assertEqual([s.session_id for s in recent], ["newer"])

        iso = scan_sessions(self.cfg, project=PROJECT, since="1970-01-01T00:00:00Z")
        self.assertEqual(len(iso), 2)

    def test_extract_events_and_skip_bad_line(self) -> None:
        lines = self._lines()
        lines.insert(3, '{"broken": ')
        path = self._write("sess-1.jsonl", lines)

        result = self._ingest_named(path)
        self.assertEqual(result.events, 5)
        self.assertEqual(result.skipped, 1)
        self.assertEqual(result.offset, path.stat().st_size)

        rows = self.conn.execute(
            "SELECT seq, role, kind, text, tool_name, meta_json FROM events "
            "WHERE session_id='sess-1' ORDER BY seq"
        ).fetchall()
        self.assertEqual([r["seq"] for r in rows], [0, 1, 2, 3, 4])
        self.assertEqual(
            [r["kind"] for r in rows],
            ["user", "thinking", "assistant", "tool_use", "tool_result"],
        )
        self.assertEqual(rows[0]["text"], "请记住：项目代号是蓝鲸。")
        self.assertEqual(rows[3]["tool_name"], "Bash")
        self.assertEqual(rows[3]["text"], '{"command": "ls"}')
        self.assertEqual(json.loads(rows[4]["meta_json"])["tool_use_id"], "tool-1")
        self.assertEqual(rows[4]["text"], "a.txt")

    def test_reingest_is_idempotent(self) -> None:
        path = self._write("sess-1.jsonl", self._lines())
        first = self._ingest_named(path)
        second = self._ingest_named(path)
        self.assertEqual(first.events, 5)
        self.assertEqual(second.events, 0)
        self.assertEqual(self._count("events"), 5)

    def test_incremental_append(self) -> None:
        path = self._write("sess-1.jsonl", self._lines())
        self._ingest_named(path)

        extra = {
            "type": "user",
            "timestamp": "2026-10-26T10:01:00Z",
            "message": {"role": "user", "content": "补充一条。"},
        }
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(extra, ensure_ascii=False) + "\n")

        result = self._ingest_named(path)
        self.assertEqual(result.events, 1)
        self.assertEqual(self._count("events"), 6)
        row = self.conn.execute(
            "SELECT role, kind, text FROM events WHERE session_id='sess-1' AND seq=5"
        ).fetchone()
        self.assertEqual((row["role"], row["kind"], row["text"]), ("user", "user", "补充一条。"))

    def test_truncated_file_reread_from_zero(self) -> None:
        path = self._write("sess-1.jsonl", self._lines())
        self._ingest_named(path)
        self.assertEqual(self._count("events"), 5)

        self._write("sess-1.jsonl", self._lines()[:1])
        result = self._ingest_named(path)
        self.assertEqual(result.events, 1)
        self.assertEqual(self._count("events"), 1)
        row = self.conn.execute("SELECT text FROM events").fetchone()
        self.assertEqual(row["text"], "请记住：项目代号是蓝鲸。")

    def test_rewrite_same_size_restarts(self) -> None:
        first_text = "AAAAAAAAAA"
        second_text = "BBBBBBBBBB"
        rec = lambda text: json.dumps(
            {
                "type": "user",
                "timestamp": "2026-10-26T10:00:00Z",
                "message": {"role": "user", "content": text},
            },
            ensure_ascii=False,
        )

        path = self.project_dir / "sess-1.jsonl"
        path.write_text(rec(first_text) + "\n", encoding="utf-8")
        os.utime(path, (1_000_000_000, 1_000_000_000))
        self._ingest_named(path)
        self.assertEqual(
            self.conn.execute("SELECT text FROM events").fetchone()["text"], first_text
        )

        path.write_text(rec(second_text) + "\n", encoding="utf-8")
        os.utime(path, (1_000_000_100, 1_000_000_100))
        result = self._ingest_named(path)
        self.assertEqual(result.events, 1)
        self.assertEqual(self._count("events"), 1)
        self.assertEqual(
            self.conn.execute("SELECT text FROM events").fetchone()["text"], second_text
        )

    def test_partial_line_not_consumed(self) -> None:
        line = json.dumps(RECORDS[0], ensure_ascii=False)
        path = self.project_dir / "sess-1.jsonl"
        path.write_text(line[:10], encoding="utf-8")  # 半行，没有换行

        first = self._ingest_named(path)
        self.assertEqual(first.events, 0)
        self.assertEqual(first.offset, 0)
        self.assertEqual(self._count("events"), 0)

        path.write_text(line + "\n", encoding="utf-8")
        second = self._ingest_named(path)
        self.assertEqual(second.events, 1)
        self.assertEqual(self._count("events"), 1)

    def test_ingest_summary(self) -> None:
        self._write("sess-1.jsonl", self._lines())
        summary = ingest(self.cfg, project=PROJECT)
        self.assertEqual(
            (summary.sessions, summary.events, summary.skipped, summary.errors),
            (1, 5, 0, 0),
        )

    def test_ingest_unknown_project_returns_empty(self) -> None:
        summary = ingest(self.cfg, project="NOPE")
        self.assertEqual((summary.sessions, summary.events), (0, 0))


if __name__ == "__main__":
    unittest.main()