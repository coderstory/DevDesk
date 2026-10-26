"""观测压缩：把会话事件窗口交给 LLM，压成结构化记忆观察并写库。

流程：
1. :func:`build_segments` 按条数/字符预算把 events 切成段（``Segment``）；
2. 段内只有 thinking / tool_result 且没有用户文本时直接跳过，不花 LLM 调用；
3. :func:`compress_segment` 调 LLM 产出 1-3 条观察（严格 JSON）；
4. :func:`compress_session` 落库（``db.insert_observation``）并可顺带补嵌入。

依赖 ``memory_lab.db`` / ``memory_lab.models`` / ``memory_lab.embedding``，
这些模块由 foundation 分支提供，本模块只按冻结契约调用。
"""

from __future__ import annotations

from memory_lab.db import insert_observation, utc_now
from memory_lab.embedding import observation_text, upsert_embedding
from memory_lab.llm import extract_json
from memory_lab.models import Segment

DEFAULT_MAX_CHARS = 6000
DEFAULT_MAX_EVENTS = 40
MAX_OBSERVATIONS_PER_SEGMENT = 3
_LLM_MAX_TOKENS = 1500
_SKIP_KINDS = frozenset({"thinking", "tool_result"})

_SYSTEM_PROMPT = """你是个人记忆压缩器。把下面这段 Claude Code 会话事件压缩成 1-3 条结构化记忆观察。
要求：
1. 只记录事实：做了什么、为什么这么做、结论是什么、踩了什么坑、涉及哪些文件。
2. 不要写寒暄、不要复述大段代码、不要编造事件里没有的信息。
3. 每条观察字段：
   - title: ≤20 字（必填）
   - subtitle: ≤40 字
   - narrative: ≤120 字
   - facts: 关键事实短句数组（可为空数组）
   - concepts: 主题/技术概念数组（可为空数组）
   - files: 事件中出现的文件路径数组（可为空数组）
   - importance: 1-5 整数（踩坑/决策=4-5，普通操作=3，例行=2）
4. 输出严格 JSON，不要任何解释或 Markdown 围栏：
{"observations":[{"title":"...","subtitle":"...","narrative":"...","facts":["..."],"concepts":["..."],"files":["..."],"importance":3}]}
5. 若这段内容没有可保留的信息（纯闲聊/空），返回 {"observations":[]}。"""


# ---------------------------------------------------------------------------
# 分段
# ---------------------------------------------------------------------------

def _row_get(row, name: str, index: int):
    if hasattr(row, "keys"):
        return row[name]
    return row[index]


def _format_line(row, text: str, max_chars: int) -> str:
    seq = _row_get(row, "seq", 0)
    kind = str(_row_get(row, "kind", 2) or "event")
    tool_name = _row_get(row, "tool_name", 4)
    label = f"{kind}:{tool_name}" if tool_name else kind
    header = f"[{seq} {label}] "
    room = max(0, max_chars - len(header))
    return header + text[:room]


def build_segments(
    conn, session_id: str, max_chars: int = DEFAULT_MAX_CHARS, max_events: int = DEFAULT_MAX_EVENTS
) -> list[Segment]:
    """把会话事件按顺序切成若干段，每段 ≤ max_events 条、文本 ≤ max_chars 字符。"""
    max_chars = max(1, int(max_chars))
    max_events = max(1, int(max_events))
    rows = conn.execute(
        "SELECT seq, role, kind, text, tool_name FROM events "
        "WHERE session_id = ? ORDER BY seq",
        (session_id,),
    ).fetchall()

    segments: list[Segment] = []
    buffer: list[str] = []
    buffer_chars = 0
    seq_start: int | None = None
    seq_end: int | None = None

    def flush() -> None:
        nonlocal buffer, buffer_chars, seq_start, seq_end
        if not buffer or seq_start is None or seq_end is None:
            return
        segments.append(
            Segment(
                session_id=session_id,
                seq_start=seq_start,
                seq_end=seq_end,
                text="\n".join(buffer),
                event_count=len(buffer),
            )
        )
        buffer = []
        buffer_chars = 0
        seq_start = None
        seq_end = None

    for row in rows:
        text = (_row_get(row, "text", 3) or "").strip()
        if not text:
            continue
        line = _format_line(row, text, max_chars)
        extra = len(line) + (1 if buffer else 0)
        if buffer and (
            len(buffer) >= max_events or buffer_chars + extra > max_chars
        ):
            flush()
        seq = int(_row_get(row, "seq", 0))
        if seq_start is None:
            seq_start = seq
        seq_end = seq
        buffer.append(line)
        buffer_chars += len(line) + (1 if len(buffer) > 1 else 0)
    flush()
    return segments


def _segment_is_skippable(conn, seg: Segment) -> bool:
    """段内只有 thinking / tool_result 且没有用户文本时，不值得发 LLM。"""
    rows = conn.execute(
        "SELECT kind FROM events WHERE session_id = ? AND seq BETWEEN ? AND ?",
        (seg.session_id, seg.seq_start, seg.seq_end),
    ).fetchall()
    if not rows:
        return True
    kinds = [str(_row_get(row, "kind", 0) or "") for row in rows]
    if any(kind == "user" for kind in kinds):
        return False
    return all(kind in _SKIP_KINDS for kind in kinds)


# ---------------------------------------------------------------------------
# 单段压缩
# ---------------------------------------------------------------------------

def _build_messages(seg: Segment, project: str | None) -> list[dict]:
    user_prompt = (
        f"项目：{project or '未知'}\n"
        f"会话：{seg.session_id}\n"
        f"事件范围：{seg.seq_start}-{seg.seq_end}（{seg.event_count} 条）\n"
        f"事件：\n{seg.text}"
    )
    return [
        {"role": "system", "content": _SYSTEM_PROMPT},
        {"role": "user", "content": user_prompt},
    ]


def _clip(value, limit: int) -> str:
    text = str(value or "").strip()
    return text if len(text) <= limit else text[:limit]


def _string_list(value) -> list[str]:
    if isinstance(value, str):
        items = [value]
    elif isinstance(value, (list, tuple)):
        items = list(value)
    else:
        items = []
    result: list[str] = []
    for item in items:
        text = str(item).strip() if item is not None else ""
        if text:
            result.append(text)
    return result


def _normalize_observation(item) -> dict | None:
    if not isinstance(item, dict):
        return None
    title = _clip(item.get("title"), 20)
    if not title:
        return None
    try:
        importance = int(item.get("importance", 3))
    except (TypeError, ValueError):
        importance = 3
    importance = max(1, min(5, importance))
    return {
        "title": title,
        "subtitle": _clip(item.get("subtitle"), 40),
        "narrative": _clip(item.get("narrative"), 120),
        "facts": _string_list(item.get("facts")),
        "concepts": _string_list(item.get("concepts")),
        "files": _string_list(item.get("files")),
        "importance": importance,
    }


def _compress_segment_with_usage(
    llm, seg: Segment, project: str | None = None
) -> tuple[list[dict], dict]:
    text, usage = llm.complete(
        _build_messages(seg, project),
        max_tokens=_LLM_MAX_TOKENS,
        temperature=0.0,
    )
    data = extract_json(text)
    if not isinstance(data, dict):
        return [], usage
    items = data.get("observations")
    if not isinstance(items, list):
        return [], usage
    observations: list[dict] = []
    for item in items:
        normalized = _normalize_observation(item)
        if normalized is not None:
            observations.append(normalized)
        if len(observations) >= MAX_OBSERVATIONS_PER_SEGMENT:
            break
    return observations, usage


def compress_segment(llm, seg: Segment, project: str | None = None) -> list[dict]:
    """压缩一段事件，返回 0-3 条规范化后的观察 dict。"""
    observations, _usage = _compress_segment_with_usage(llm, seg, project)
    return observations


# ---------------------------------------------------------------------------
# 会话级压缩
# ---------------------------------------------------------------------------

def _session_project(conn, session_id: str) -> str:
    try:
        row = conn.execute(
            "SELECT project FROM sessions WHERE id = ?", (session_id,)
        ).fetchone()
    except Exception:  # noqa: BLE001 - sessions 表缺失时按未知项目处理
        return ""
    if row is None:
        return ""
    return str(_row_get(row, "project", 0) or "")


def compress_session(
    cfg, conn, session_id: str, llm, embedder=None, budget: int | None = None
) -> dict:
    """压缩整个会话并写库。

    ``budget`` 为本次允许的 LLM 调用次数上限（None 表示不限）。
    返回 ``{"observations": n, "usage": {...}, "calls": n}``。
    """
    del cfg  # 目前压缩参数全部来自入参；保留签名供 CLI/编排层统一传参
    project = _session_project(conn, session_id)
    segments = build_segments(conn, session_id)
    max_calls = None if budget is None else max(0, int(budget))

    total_observations = 0
    input_tokens = 0
    output_tokens = 0
    calls = 0

    for seg in segments:
        if max_calls is not None and calls >= max_calls:
            break
        if _segment_is_skippable(conn, seg):
            continue
        observations, usage = _compress_segment_with_usage(llm, seg, project)
        calls += 1
        input_tokens += int(usage.get("input_tokens") or 0)
        output_tokens += int(usage.get("output_tokens") or 0)

        created_at = utc_now()
        inserted: list[tuple[int, dict]] = []
        for observation in observations:
            record = dict(observation)
            record.update(
                {
                    "session_id": seg.session_id,
                    "project": project,
                    "kind": "observation",
                    "created_at": created_at,
                    "source_event_range": (
                        f"{seg.session_id}:{seg.seq_start}-{seg.seq_end}"
                    ),
                }
            )
            obs_id = insert_observation(conn, record)
            inserted.append((obs_id, record))
            total_observations += 1

        if embedder is not None and inserted:
            try:
                texts = [observation_text(record) for _id, record in inserted]
                vectors = embedder.embed(texts)
                if len(vectors) == len(inserted):
                    for (obs_id, _record), vector in zip(inserted, vectors):
                        upsert_embedding(
                            conn, "observation", obs_id, embedder.name, vector
                        )
            except Exception:  # noqa: BLE001 - 嵌入失败不丢已压缩的观察
                pass

    conn.commit()
    return {
        "observations": total_observations,
        "usage": {
            "input_tokens": input_tokens,
            "output_tokens": output_tokens,
        },
        "calls": calls,
    }