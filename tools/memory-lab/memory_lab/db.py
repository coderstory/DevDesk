"""SQLite 存储层：schema、会话/事件写入、观察读写、统计。

约定：
- facts / files 在库里是换行分隔文本，concepts 是逗号分隔文本；
  Python 侧的 list <-> text 转换全部在本模块内完成，调用方只见 list。
- FTS5 使用 external content + trigram 分词；INSERT/UPDATE/DELETE 由触发器同步。
"""

from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path

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

CREATE TABLE IF NOT EXISTS dream_proposals (
  id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending', payload_json TEXT NOT NULL,
  rationale TEXT, created_at TEXT NOT NULL, applied_at TEXT);

CREATE TABLE IF NOT EXISTS ingest_state (
  file_path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime REAL NOT NULL, offset INTEGER NOT NULL);

CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);

CREATE VIRTUAL TABLE IF NOT EXISTS fts_observations USING fts5(
  title, subtitle, narrative, facts, concepts, files,
  content='observations', content_rowid='id', tokenize='trigram');

CREATE TRIGGER IF NOT EXISTS trg_obs_ai AFTER INSERT ON observations BEGIN
  INSERT INTO fts_observations(rowid, title, subtitle, narrative, facts, concepts, files)
  VALUES (new.id, new.title, coalesce(new.subtitle, ''), coalesce(new.narrative, ''),
          coalesce(new.facts, ''), coalesce(new.concepts, ''), coalesce(new.files, ''));
END;

CREATE TRIGGER IF NOT EXISTS trg_obs_ad AFTER DELETE ON observations BEGIN
  INSERT INTO fts_observations(fts_observations, rowid, title, subtitle, narrative, facts, concepts, files)
  VALUES ('delete', old.id, old.title, coalesce(old.subtitle, ''), coalesce(old.narrative, ''),
          coalesce(old.facts, ''), coalesce(old.concepts, ''), coalesce(old.files, ''));
END;

CREATE TRIGGER IF NOT EXISTS trg_obs_au AFTER UPDATE ON observations BEGIN
  INSERT INTO fts_observations(fts_observations, rowid, title, subtitle, narrative, facts, concepts, files)
  VALUES ('delete', old.id, old.title, coalesce(old.subtitle, ''), coalesce(old.narrative, ''),
          coalesce(old.facts, ''), coalesce(old.concepts, ''), coalesce(old.files, ''));
  INSERT INTO fts_observations(rowid, title, subtitle, narrative, facts, concepts, files)
  VALUES (new.id, new.title, coalesce(new.subtitle, ''), coalesce(new.narrative, ''),
          coalesce(new.facts, ''), coalesce(new.concepts, ''), coalesce(new.files, ''));
END;
"""

_OBS_COLUMNS = (
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

_LIST_FIELDS = ("facts", "concepts", "files")


def utc_now() -> str:
    """UTC ISO-8601 时间字符串（秒精度）。"""
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def connect(db_path: Path | str) -> sqlite3.Connection:
    """打开数据库：自动建父目录，Row 工厂，WAL，外键约束。"""
    path = Path(db_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path), timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_schema(conn: sqlite3.Connection) -> None:
    """幂等建表：重复调用不报错、不清数据。"""
    conn.executescript(SCHEMA_SQL)
    conn.commit()


# ---------------------------------------------------------------------------
# list <-> 文本编解码
# ---------------------------------------------------------------------------

def encode_list(field_name: str, values) -> str:
    if values is None:
        return ""
    items = [str(v) for v in values if str(v) != ""]
    if field_name == "concepts":
        return ", ".join(items)
    return "\n".join(items)


def decode_list(field_name: str, text) -> list[str]:
    if not text:
        return []
    if field_name == "concepts":
        return [part.strip() for part in str(text).split(",") if part.strip()]
    return [line.strip() for line in str(text).splitlines() if line.strip()]


def _row_to_observation(row: sqlite3.Row) -> dict:
    data = dict(row)
    for name in _LIST_FIELDS:
        data[name] = decode_list(name, data.get(name))
    return data


# ---------------------------------------------------------------------------
# 写入
# ---------------------------------------------------------------------------

def upsert_session(
    conn: sqlite3.Connection,
    session_id: str,
    project: str,
    file_path: str,
    started_at: str | None = None,
    ended_at: str | None = None,
    meta_json: str | dict | None = None,
) -> None:
    if isinstance(meta_json, dict):
        meta_json = json.dumps(meta_json, ensure_ascii=False)
    conn.execute(
        """
        INSERT INTO sessions(id, project, file_path, started_at, ended_at, meta_json)
        VALUES (?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
          project = excluded.project,
          file_path = excluded.file_path,
          started_at = coalesce(excluded.started_at, sessions.started_at),
          ended_at = coalesce(excluded.ended_at, sessions.ended_at),
          meta_json = coalesce(excluded.meta_json, sessions.meta_json)
        """,
        (session_id, project, file_path, started_at, ended_at, meta_json),
    )


def insert_event(
    conn: sqlite3.Connection,
    session_id: str,
    seq: int,
    ts: str | None,
    role: str | None,
    kind: str,
    text: str | None,
    tool_name: str | None = None,
    meta_json: str | dict | None = None,
) -> int:
    if isinstance(meta_json, dict):
        meta_json = json.dumps(meta_json, ensure_ascii=False)
    cur = conn.execute(
        """
        INSERT OR REPLACE INTO events
          (session_id, seq, ts, role, kind, text, tool_name, meta_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """,
        (session_id, seq, ts, role, kind, text, tool_name, meta_json),
    )
    return int(cur.lastrowid)


def insert_observation(conn: sqlite3.Connection, obs: dict) -> int:
    """插入观察，返回新 id。obs 可缺省可选字段。"""
    values = {
        "session_id": obs["session_id"],
        "project": obs["project"],
        "kind": obs.get("kind") or "observation",
        "title": obs["title"],
        "subtitle": obs.get("subtitle") or "",
        "narrative": obs.get("narrative") or "",
        "facts": encode_list("facts", obs.get("facts")),
        "concepts": encode_list("concepts", obs.get("concepts")),
        "files": encode_list("files", obs.get("files")),
        "importance": int(obs.get("importance", 3)),
        "created_at": obs.get("created_at") or utc_now(),
        "source_event_range": obs.get("source_event_range"),
    }
    cur = conn.execute(
        f"INSERT INTO observations({', '.join(_OBS_COLUMNS)}) "
        f"VALUES ({', '.join('?' for _ in _OBS_COLUMNS)})",
        tuple(values[name] for name in _OBS_COLUMNS),
    )
    return int(cur.lastrowid)


def get_observation(conn: sqlite3.Connection, obs_id: int) -> dict | None:
    row = conn.execute("SELECT * FROM observations WHERE id = ?", (obs_id,)).fetchone()
    return _row_to_observation(row) if row is not None else None


# ---------------------------------------------------------------------------
# 统计
# ---------------------------------------------------------------------------

def stats(conn: sqlite3.Connection) -> dict:
    def count(table: str) -> int:
        row = conn.execute(f"SELECT COUNT(*) AS c FROM {table}").fetchone()
        return int(row["c"])

    active = conn.execute(
        "SELECT project, COUNT(*) AS c FROM observations "
        "WHERE status = 'active' GROUP BY project ORDER BY c DESC, project ASC"
    ).fetchall()
    return {
        "sessions": count("sessions"),
        "events": count("events"),
        "observations": count("observations"),
        "embeddings": count("embeddings"),
        "proposals": count("dream_proposals"),
        "active_by_project": {row["project"]: int(row["c"]) for row in active},
    }