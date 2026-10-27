"""测试公共设施。

本分支（LLM/嵌入/压缩）与 foundation 分支并行开发，foundation 的
``config.py / models.py / db.py / ingest.py`` 在合并前可能不存在。
这里在**真实模块缺失时**注入最小桩模块（字段/编码规则与冻结契约一致），
真实模块存在时一律使用真实实现，不做任何替换。

DDL 与 foundation 的 ``db.SCHEMA_SQL`` 保持一致（冻结契约的唯一副本），
测试用它在内存库里建表。
"""

from __future__ import annotations

import sqlite3
import sys
import types
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


def ensure_import_path() -> None:
    root = Path(__file__).resolve().parents[1]
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))


SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS sessions (
  id TEXT PRIMARY KEY, project TEXT NOT NULL, file_path TEXT NOT NULL,
  started_at TEXT, ended_at TEXT, meta_json TEXT);

CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, seq INTEGER NOT NULL,
  ts TEXT, role TEXT, kind TEXT NOT NULL, text TEXT, tool_name TEXT, meta_json TEXT,
  UNIQUE(session_id, seq));
CREATE INDEX IF NOT EXISTS idx_events_session ON events(session_id);

CREATE TABLE IF NOT EXISTS observations (
  id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL, project TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT 'observation', title TEXT NOT NULL, subtitle TEXT,
  narrative TEXT, facts TEXT, concepts TEXT, files TEXT,
  importance INTEGER DEFAULT 3, created_at TEXT NOT NULL, source_event_range TEXT,
  status TEXT NOT NULL DEFAULT 'active', merged_into INTEGER, dream_batch TEXT,
  access_count INTEGER DEFAULT 0, last_accessed TEXT);
CREATE INDEX IF NOT EXISTS idx_obs_project ON observations(project, status);

CREATE TABLE IF NOT EXISTS embeddings (
  id INTEGER PRIMARY KEY AUTOINCREMENT, owner_type TEXT NOT NULL, owner_id INTEGER NOT NULL,
  model TEXT NOT NULL, dim INTEGER NOT NULL, vector BLOB NOT NULL,
  UNIQUE(owner_type, owner_id, model));
"""


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _encode_list(field_name: str, values) -> str:
    if values is None:
        return ""
    items = [str(v) for v in values if str(v) != ""]
    if field_name == "concepts":
        return ", ".join(items)
    return "\n".join(items)


def _insert_observation(conn, obs: dict) -> int:
    columns = (
        "session_id",
        "project",
        "kind",
        "title",
        "subtitle",
        "narrative",
        "facts",
        "concepts",
        "files",
        "importance",
        "created_at",
        "source_event_range",
    )
    values = {
        "session_id": obs["session_id"],
        "project": obs["project"],
        "kind": obs.get("kind") or "observation",
        "title": obs["title"],
        "subtitle": obs.get("subtitle") or "",
        "narrative": obs.get("narrative") or "",
        "facts": _encode_list("facts", obs.get("facts")),
        "concepts": _encode_list("concepts", obs.get("concepts")),
        "files": _encode_list("files", obs.get("files")),
        "importance": int(obs.get("importance", 3)),
        "created_at": obs.get("created_at") or _utc_now(),
        "source_event_range": obs.get("source_event_range"),
    }
    cur = conn.execute(
        f"INSERT INTO observations({', '.join(columns)}) "
        f"VALUES ({', '.join('?' for _ in columns)})",
        tuple(values[name] for name in columns),
    )
    return int(cur.lastrowid)


def install_foundation_stubs() -> bool:
    """真实 foundation 模块存在则返回 False；否则注入桩并返回 True。"""
    ensure_import_path()
    try:
        import memory_lab.db  # noqa: F401
        import memory_lab.models  # noqa: F401
        return False
    except ModuleNotFoundError:
        pass

    import memory_lab as package

    models = types.ModuleType("memory_lab.models")

    @dataclass
    class Segment:
        session_id: str
        seq_start: int
        seq_end: int
        text: str
        event_count: int

    Segment.__module__ = "memory_lab.models"
    models.Segment = Segment
    sys.modules["memory_lab.models"] = models
    package.models = models

    db = types.ModuleType("memory_lab.db")
    db.utc_now = _utc_now
    db.encode_list = _encode_list
    db.insert_observation = _insert_observation
    sys.modules["memory_lab.db"] = db
    package.db = db
    return True


def new_conn() -> sqlite3.Connection:
    """内存库，schema 与冻结 DDL 一致。"""
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row
    conn.executescript(SCHEMA_SQL)
    return conn


def insert_event(
    conn,
    session_id: str,
    seq: int,
    kind: str,
    text: str,
    role: str = "user",
    tool_name: str | None = None,
) -> None:
    conn.execute(
        "INSERT INTO events(session_id, seq, ts, role, kind, text, tool_name) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        (session_id, seq, "2026-10-26T00:00:00+00:00", role, kind, text, tool_name),
    )


def insert_session(conn, session_id: str, project: str = "D--project-gui") -> None:
    conn.execute(
        "INSERT INTO sessions(id, project, file_path) VALUES (?, ?, ?)",
        (session_id, project, f"/tmp/{session_id}.jsonl"),
    )


def insert_observation_row(
    conn,
    session_id: str,
    title: str,
    status: str = "active",
    project: str = "p",
    narrative: str = "",
    facts: str = "",
) -> int:
    cur = conn.execute(
        "INSERT INTO observations(session_id, project, title, narrative, facts, "
        "importance, created_at, status) VALUES (?, ?, ?, ?, ?, 3, ?, ?)",
        (session_id, project, title, narrative, facts, _utc_now(), status),
    )
    return int(cur.lastrowid)