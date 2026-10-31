#!/usr/bin/env python3
"""会话资产库(需-10 段1)——Claude Code 会话转写归档 + FTS5 检索。

spec: docs/specs/2026-10-31-需-10-会话资产库-spec草案.md
采集入口: Claude Code SessionEnd hook(POC: ZCode 内核不支持该事件,见 spec)。

三模式:
  ingest   SessionEnd hook handler:stdin 读 payload,解析 transcript_path 入库
  sync     扫 ~/.claude/projects/**/*.jsonl 增量对账(强杀漏采兜底)
  search   FTS5 检索(--project/--since/--limit)

存储: ~/DevDesk/session-assets/session-assets.db(SESSION_ASSETS_DB 可覆盖,测试用)。
只用 stdlib;hook 里用系统 python3,不依赖 venv。
"""

from __future__ import annotations

import json
import os
import re
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

BY = "session_archive"
DEFAULT_DB = Path.home() / "DevDesk" / "session-assets" / "session-assets.db"
CLAUDE_PROJECTS = Path.home() / ".claude" / "projects"

# FTS5 unicode61 不切 CJK(整句成单 token,中文检索必失效)——入库/查询两侧统一
# 做 CJK bigram 预切分:连续 CJK 段按 2 字滑窗,非 CJK 段原样交 unicode61。
_CJK_RE = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]+")

def _cjk_bigram(text: str) -> str:
    parts: list[str] = []
    pos = 0
    for m in _CJK_RE.finditer(text):
        parts.append(text[pos:m.start()])
        seg = m.group()
        if len(seg) == 1:
            parts.append(seg)
        else:
            parts.extend(seg[i:i + 2] for i in range(len(seg) - 1))
        pos = m.end()
    parts.append(text[pos:])
    return " ".join(p for p in parts if p)


def _db_path() -> Path:
    env = os.environ.get("SESSION_ASSETS_DB")
    return Path(env) if env else DEFAULT_DB


def _now() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S %z")


def _connect() -> sqlite3.Connection:
    db = _db_path()
    db.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    _ensure_schema(conn)
    return conn


def _ensure_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS assets (
          session_id TEXT PRIMARY KEY,
          project TEXT NOT NULL,
          title TEXT,
          started TEXT, ended TEXT,
          turns INTEGER DEFAULT 0,
          size_bytes INTEGER DEFAULT 0,
          mtime REAL DEFAULT 0,
          reason TEXT,
          path TEXT,
          deleted INTEGER DEFAULT 0,
          created_by TEXT, updated_by TEXT,
          created TEXT, updated TEXT
        );
        CREATE VIRTUAL TABLE IF NOT EXISTS assets_fts USING fts5(
          session_id UNINDEXED,
          title, body
        );
        CREATE INDEX IF NOT EXISTS idx_assets_project ON assets(project);
        """
    )
    conn.commit()


# ---------- transcript 解析 ----------

def _texts_from_content(content: object) -> list[str]:
    """message.content 兼容 字符串 / 块数组 两种形态;只取 text 块。"""
    if isinstance(content, str):
        return [content] if content.strip() else []
    out: list[str] = []
    if isinstance(content, list):
        for block in content:
            if isinstance(block, dict) and block.get("type") == "text":
                t = block.get("text", "")
                if t.strip():
                    out.append(t)
    return out


def parse_transcript(path: Path) -> dict | None:
    """解析一个 transcript jsonl → {project,title,started,ended,turns,body}。

    非法行跳过;user/assistant 的文本块入 body,tool_use/tool_result 等噪音剔除。
    """
    texts: list[str] = []
    stamps: list[str] = []
    project = ""
    title = ""
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw.strip():
            continue
        try:
            line = json.loads(raw)
        except json.JSONDecodeError:
            continue
        # 合法 JSON 但非对象(数组/字符串/数字)同样跳过——否则下游 .get 炸 AttributeError,
        # 炸穿 ingest 的 exit-0 契约并中断 sync 全量对账(独立审计 P1)
        if not isinstance(line, dict):
            continue
        if not project and line.get("cwd"):
            project = str(line["cwd"])
        if line.get("timestamp"):
            stamps.append(str(line["timestamp"]))
        mtype = line.get("type")
        if mtype == "summary" and not title:
            title = str(line.get("summary", ""))[:120]
            continue
        msg = line.get("message")
        if not isinstance(msg, dict):
            continue
        role = msg.get("role")
        if role not in ("user", "assistant"):
            continue
        for t in _texts_from_content(msg.get("content")):
            # 命令注入类噪音(<system-reminder>/<local-command>)不入索引
            if t.lstrip().startswith("<system-reminder>") or t.lstrip().startswith("<local-command"):
                continue
            if not title and role == "user":
                title = t.strip()[:120]
            texts.append(t)
    if not texts and not stamps:
        return None
    body = "\n".join(texts)
    turns = len(texts)
    return {
        "project": project or "(unknown)",
        "title": title or "(无标题会话)",
        "started": min(stamps) if stamps else "",
        "ended": max(stamps) if stamps else "",
        "turns": turns,
        "body": body,
    }


def _upsert(conn: sqlite3.Connection, sid: str, parsed: dict, path: Path, reason: str) -> bool:
    """增量入库:size+mtime 未变则跳过;变化则重索引。返回是否写入。"""
    size = path.stat().st_size
    mtime = path.stat().st_mtime
    row = conn.execute("SELECT size_bytes, mtime FROM assets WHERE session_id=?", (sid,)).fetchone()
    if row and row["size_bytes"] == size and row["mtime"] == mtime:
        return False
    now = _now()
    if row:
        conn.execute(
            "UPDATE assets SET project=?,title=?,started=?,ended=?,turns=?,size_bytes=?,mtime=?,"
            "reason=?,path=?,updated_by=?,updated=?,deleted=0 WHERE session_id=?",
            (parsed["project"], parsed["title"], parsed["started"], parsed["ended"],
             parsed["turns"], size, mtime, reason, str(path), BY, now, sid),
        )
        conn.execute("DELETE FROM assets_fts WHERE session_id=?", (sid,))
    else:
        conn.execute(
            "INSERT INTO assets(session_id,project,title,started,ended,turns,size_bytes,mtime,"
            "reason,path,deleted,created_by,updated_by,created,updated) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,0,?,?,?,?)",
            (sid, parsed["project"], parsed["title"], parsed["started"], parsed["ended"],
             parsed["turns"], size, mtime, reason, str(path), BY, BY, now, now),
        )
    conn.execute(
        "INSERT INTO assets_fts(session_id,title,body) VALUES(?,?,?)",
        (sid, _cjk_bigram(parsed["title"]), _cjk_bigram(parsed["body"])),
    )
    conn.commit()
    return True


# ---------- 三模式 ----------

def cmd_ingest() -> int:
    """SessionEnd hook handler。任何失败都 exit 0(无决策权,不骚扰会话退出)——
    整体兜底,不让 DB 不可写/解析异常等任何错误以 traceback 破约(独立审计 P2)。"""
    try:
        try:
            payload = json.loads(sys.stdin.read() or "{}")
        except json.JSONDecodeError:
            return 0
        tp = payload.get("transcript_path")
        if not tp or not Path(tp).exists():
            return 0
        sid = str(payload.get("session_id") or Path(tp).stem)
        parsed = parse_transcript(Path(tp))
        if not parsed:
            return 0
        conn = _connect()
        try:
            _upsert(conn, sid, parsed, Path(tp), str(payload.get("reason") or ""))
        finally:
            conn.close()
    except Exception as e:  # noqa: BLE001 — hook 契约:一切失败静默 exit 0
        print(f"session_archive ingest: {e}", file=sys.stderr)
    return 0


def cmd_sync() -> int:
    """对账补漏:扫全部项目目录,增量收录。"""
    if not CLAUDE_PROJECTS.exists():
        print(f"no {CLAUDE_PROJECTS}", file=sys.stderr)
        return 2
    files = sorted(CLAUDE_PROJECTS.glob("*/*.jsonl"))
    conn = _connect()
    added = skipped = failed = 0
    try:
        for i, f in enumerate(files, 1):
            sid = f.stem
            try:
                parsed = parse_transcript(f)
                if parsed and _upsert(conn, sid, parsed, f, "sync"):
                    added += 1
                else:
                    skipped += 1
            except (OSError, sqlite3.Error) as e:
                # 半途 _upsert 可能已写 assets 未写 fts:先 rollback 防脏状态
                # 被下一个文件的 commit 顺带提交,造成 FTS 与 assets 静默失配(独立审计 P2)
                conn.rollback()
                failed += 1
                print(f"fail {f.name}: {e}", file=sys.stderr)
            if i % 200 == 0:
                print(f"[{i}/{len(files)}] added={added} skipped={skipped}", file=sys.stderr)
    finally:
        conn.close()
    print(json.dumps({"total": len(files), "added": added, "skipped": skipped, "failed": failed},
                     ensure_ascii=False))
    return 0


def cmd_search(keyword: str, project: str | None, since: str | None, limit: int) -> int:
    conn = _connect()
    try:
        # FTS5 查询词安全化:剔无实义词 + 内嵌引号双写转义;CJK bigram 与入库侧同法
        words = [w for w in _cjk_bigram(keyword).split() if re.search(r"[\w\u3400-\u9fff]", w)]
        q = " ".join('"%s"' % w.replace('"', '""') for w in words)
        if not q:
            print("empty keyword", file=sys.stderr)
            return 2
        sql = [
            "SELECT a.session_id, a.project, a.title, a.started, a.ended, a.turns, a.path",
            "FROM assets a JOIN (",
            "  SELECT session_id, bm25(assets_fts) AS rank",
            "  FROM assets_fts WHERE assets_fts MATCH ?",
            ") fts ON a.session_id=fts.session_id",
            "WHERE a.deleted=0",
        ]
        args: list[object] = [q]
        if project:
            sql.append("AND a.project LIKE ?")
            args.append(f"%{project}%")
        if since:
            sql.append("AND a.started >= ?")
            args.append(since)
        sql.append("ORDER BY fts.rank LIMIT ?")
        args.append(limit)
        rows = conn.execute("\n".join(sql), args).fetchall()
    finally:
        conn.close()
    print(json.dumps({"hits": len(rows), "rows": [dict(r) for r in rows]},
                     ensure_ascii=False, indent=1))
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    mode = sys.argv[1]
    if mode == "ingest":
        return cmd_ingest()
    if mode == "sync":
        return cmd_sync()
    if mode == "search":
        kw = sys.argv[2] if len(sys.argv) > 2 else ""
        project = since = None
        limit = 20
        extra = sys.argv[3:]

        def _flag_value(i: int) -> str:
            if i + 1 >= len(extra):
                print(f"flag {extra[i]} 缺值", file=sys.stderr)
                sys.exit(2)
            return extra[i + 1]

        for i, a in enumerate(extra):
            if a == "--project":
                project = _flag_value(i)
            elif a == "--since":
                since = _flag_value(i)
            elif a == "--limit":
                try:
                    limit = int(_flag_value(i))
                except ValueError:
                    print("invalid --limit(需整数)", file=sys.stderr)
                    sys.exit(2)
        return cmd_search(kw, project, since, limit)
    print(f"unknown mode: {mode}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
