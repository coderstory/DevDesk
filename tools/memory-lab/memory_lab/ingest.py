"""会话摄取：扫描 ~/.claude/projects 下的 jsonl，抽取事件写入 SQLite。

增量策略（ingest_state 记录 offset/size/mtime）：
- 文件只追加（size 变大 / mtime 变化但 size 更大）→ 从上次 offset 续读；
- 文件变小（被截断）或 mtime 变但 size 不变（被重写）→ 从头重读，
  并清掉该会话旧事件，避免残留尾部；
- 只消费以换行结尾的完整行，正在写入的半行留到下次。
"""

from __future__ import annotations

import json
import sys
from datetime import datetime
from pathlib import Path

from memory_lab.config import Config
from memory_lab.db import connect, init_schema, insert_event, upsert_session
from memory_lab.models import IngestResult, IngestSummary, SessionFile

TEXT_LIMIT = 4000
TOOL_RESULT_LIMIT = 2000
THINKING_LIMIT = 2000
TOOL_USE_LIMIT = 1500


# ---------------------------------------------------------------------------
# 扫描
# ---------------------------------------------------------------------------

def _to_epoch(value) -> float | None:
    """把 float/int/datetime/ISO 字符串统一成 epoch 秒。无法识别返回 None。"""
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, datetime):
        return value.timestamp()
    text = str(value).strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        pass
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def scan_sessions(
    cfg: Config,
    project: str | None = None,
    since=None,
    limit: int | None = None,
) -> list[SessionFile]:
    """列出会话文件，按 mtime 从新到旧排序（limit 取最新的 N 个）。"""
    root = Path(cfg.cc_projects_root)
    since_epoch = _to_epoch(since)

    if project is not None:
        project_dirs = [root / project]
    else:
        try:
            project_dirs = sorted(p for p in root.iterdir() if p.is_dir())
        except OSError:
            return []

    sessions: list[SessionFile] = []
    for pdir in project_dirs:
        if not pdir.is_dir():
            continue
        for path in pdir.glob("*.jsonl"):
            try:
                st = path.stat()
            except OSError:
                continue
            if since_epoch is not None and st.st_mtime < since_epoch:
                continue
            sessions.append(
                SessionFile(
                    path=path,
                    project=pdir.name,
                    session_id=path.stem,
                    size=st.st_size,
                    mtime=st.st_mtime,
                )
            )

    sessions.sort(key=lambda s: s.mtime, reverse=True)
    if limit is not None and limit >= 0:
        sessions = sessions[:limit]
    return sessions


# ---------------------------------------------------------------------------
# 单文件摄取
# ---------------------------------------------------------------------------

def _clip(text: str, limit: int) -> str:
    return text if len(text) <= limit else text[:limit]


def _summarize_content(content) -> str:
    """把 tool_result 的 content（str 或 block list）压成文本摘要。"""
    if content is None:
        return ""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts: list[str] = []
        for block in content:
            if isinstance(block, str):
                parts.append(block)
            elif isinstance(block, dict) and isinstance(block.get("text"), str):
                parts.append(block["text"])
        if parts:
            return "\n".join(parts)
    try:
        return json.dumps(content, ensure_ascii=False)
    except (TypeError, ValueError):
        return str(content)


def _event(kind: str, role: str, text: str, tool_name=None, meta=None) -> dict:
    return {
        "kind": kind,
        "role": role,
        "text": _clip(text, TEXT_LIMIT),
        "tool_name": tool_name,
        "meta": meta,
    }


def extract_events(record: dict) -> list[dict]:
    """按冻结规则把一条 jsonl 记录转成 0..N 个事件。"""
    rtype = record.get("type")
    message = record.get("message")
    if not isinstance(message, dict):
        return []

    events: list[dict] = []
    content = message.get("content")

    if rtype == "user":
        if isinstance(content, str):
            if content:
                events.append(_event("user", "user", content))
            return events
        if not isinstance(content, list):
            return events
        for block in content:
            if not isinstance(block, dict):
                continue
            btype = block.get("type")
            if btype == "text":
                text = block.get("text")
                if isinstance(text, str) and text:
                    events.append(_event("user", "user", text))
            elif btype == "tool_result":
                text = _clip(_summarize_content(block.get("content")), TOOL_RESULT_LIMIT)
                if text:
                    meta = {"tool_use_id": block.get("tool_use_id")}
                    events.append(_event("tool_result", "user", text, meta=meta))
            # 图片/文档等块不进入记忆
        return events

    if rtype == "assistant":
        if not isinstance(content, list):
            return events
        for block in content:
            if not isinstance(block, dict):
                continue
            btype = block.get("type")
            if btype == "text":
                text = block.get("text")
                if isinstance(text, str) and text:
                    events.append(_event("assistant", "assistant", text))
            elif btype == "thinking":
                text = block.get("thinking") or ""
                if isinstance(text, str) and text:
                    events.append(
                        _event("thinking", "assistant", _clip(text, THINKING_LIMIT))
                    )
            elif btype == "tool_use":
                payload = block.get("input", {})
                try:
                    text = json.dumps(payload, ensure_ascii=False, sort_keys=True)
                except (TypeError, ValueError):
                    text = str(payload)
                events.append(
                    _event(
                        "tool_use",
                        "assistant",
                        _clip(text, TOOL_USE_LIMIT),
                        tool_name=block.get("name"),
                        meta={"tool_use_id": block.get("id")},
                    )
                )
        return events

    # mode / permission-mode / attachment / system / queue-operation 等一律忽略
    return events


def ingest_file(conn, cfg: Config, sf: SessionFile) -> IngestResult:
    """增量摄取一个文件。返回本次新增事件数与最终 offset。"""
    path = Path(sf.path)
    state = conn.execute(
        "SELECT size, mtime, offset FROM ingest_state WHERE file_path = ?",
        (str(path),),
    ).fetchone()

    restart = state is None
    if state is not None:
        truncated = sf.size < state["size"]
        rewritten_same_size = sf.mtime != state["mtime"] and sf.size == state["size"]
        if truncated or rewritten_same_size:
            restart = True
            conn.execute("DELETE FROM events WHERE session_id = ?", (sf.session_id,))

    start_offset = 0 if restart else int(state["offset"])
    if restart:
        seq = 0
    else:
        row = conn.execute(
            "SELECT MAX(seq) AS m FROM events WHERE session_id = ?",
            (sf.session_id,),
        ).fetchone()
        seq = int(row["m"]) + 1 if row["m"] is not None else 0

    events_written = 0
    skipped = 0
    offset = start_offset

    if Path(path).exists():
        with open(path, "rb") as f:
            f.seek(start_offset)
            data = f.read()
        chunks = data.split(b"\n")
        tail = chunks.pop()  # 没有换行结尾的半行：本次不消费
        for raw in chunks:
            offset += len(raw) + 1
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except ValueError:
                skipped += 1
                continue
            if not isinstance(record, dict):
                skipped += 1
                continue
            for event in extract_events(record):
                insert_event(
                    conn,
                    sf.session_id,
                    seq,
                    record.get("timestamp"),
                    event["role"],
                    event["kind"],
                    event["text"],
                    tool_name=event["tool_name"],
                    meta_json=event["meta"],
                )
                seq += 1
                events_written += 1
        # tail（无换行结尾的半行）不消费，留给下次补全后继续
        _ = tail

    bounds = conn.execute(
        "SELECT MIN(ts) AS first_ts, MAX(ts) AS last_ts FROM events "
        "WHERE session_id = ? AND ts IS NOT NULL",
        (sf.session_id,),
    ).fetchone()
    upsert_session(
        conn,
        sf.session_id,
        sf.project,
        str(path),
        started_at=bounds["first_ts"],
        ended_at=bounds["last_ts"],
    )
    conn.execute(
        """
        INSERT INTO ingest_state(file_path, size, mtime, offset) VALUES (?, ?, ?, ?)
        ON CONFLICT(file_path) DO UPDATE SET
          size = excluded.size, mtime = excluded.mtime, offset = excluded.offset
        """,
        (str(path), offset, sf.mtime, offset),
    )
    conn.commit()
    return IngestResult(
        session_id=sf.session_id, events=events_written, skipped=skipped, offset=offset
    )


# ---------------------------------------------------------------------------
# 批量摄取
# ---------------------------------------------------------------------------

def ingest(
    cfg: Config,
    project: str | None = None,
    since=None,
    limit: int | None = None,
) -> IngestSummary:
    """扫描 + 逐文件增量摄取。单个文件失败不阻塞其余文件。"""
    conn = connect(cfg.db_path)
    try:
        init_schema(conn)
        files = scan_sessions(cfg, project=project, since=since, limit=limit)
        total_events = 0
        total_skipped = 0
        errors = 0
        for sf in files:
            try:
                result = ingest_file(conn, cfg, sf)
            except Exception as exc:  # noqa: BLE001 - 单文件失败要记数并继续
                errors += 1
                print(f"[memory-lab] ingest failed: {sf.path}: {exc}", file=sys.stderr)
                continue
            total_events += result.events
            total_skipped += result.skipped
        return IngestSummary(
            sessions=len(files),
            events=total_events,
            skipped=total_skipped,
            errors=errors,
        )
    finally:
        conn.close()