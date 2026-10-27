"""数据模型（纯 dataclass，不依赖 sqlite3）。

这些结构是其他并行任务（压缩 / 检索 / dream）之间的冻结契约，
字段名和顺序不要随意改动。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class SessionFile:
    """一个 Claude Code 会话 jsonl 文件的快照。"""

    path: Path
    project: str
    session_id: str
    size: int
    mtime: float


@dataclass
class IngestResult:
    """单个会话文件的摄取结果。offset 为已消费到的字节位置。"""

    session_id: str
    events: int
    skipped: int
    offset: int


@dataclass
class IngestSummary:
    """一次批量摄取的汇总。"""

    sessions: int
    events: int
    skipped: int
    errors: int


@dataclass
class Observation:
    """一条记忆观察（压缩后的最小记忆单元）。"""

    session_id: str
    project: str
    title: str
    kind: str = "observation"
    subtitle: str = ""
    narrative: str = ""
    facts: list[str] = field(default_factory=list)
    concepts: list[str] = field(default_factory=list)
    files: list[str] = field(default_factory=list)
    importance: int = 3
    created_at: str = ""
    source_event_range: str = ""
    id: int | None = None

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "session_id": self.session_id,
            "project": self.project,
            "kind": self.kind,
            "title": self.title,
            "subtitle": self.subtitle,
            "narrative": self.narrative,
            "facts": list(self.facts),
            "concepts": list(self.concepts),
            "files": list(self.files),
            "importance": self.importance,
            "created_at": self.created_at,
            "source_event_range": self.source_event_range,
        }


@dataclass
class SearchHit:
    """检索结果行。"""

    obs_id: int
    score: float
    title: str
    subtitle: str
    snippet: str
    project: str
    created_at: str
    source: str


@dataclass
class LlmUsage:
    """一次（或累计）LLM 调用的 token 用量。"""

    input_tokens: int = 0
    output_tokens: int = 0
    calls: int = 0


@dataclass
class Segment:
    """压缩输入的一段事件窗口。"""

    session_id: str
    seq_start: int
    seq_end: int
    text: str
    event_count: int