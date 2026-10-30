#!/usr/bin/env python3
"""三池 MCP server（决策文档 docs/specs/2026-10-29-三池mcp决策文档.md 的实现单）。

单文件三合一：MCP stdio server + SQLite 访问 + HTML 报告生成。
独立进程常驻，与 DevDesk app 生命周期无关（app 是开发对象频繁重启，池状态不能随它丢）。

存储：docs/pool/pool.db（独立 SQLite + FTS5）。DB 路径解析：
  环境变量 POOL_DB > 脚本位置推仓库根(docs/pool/pool.db)。测试用 POOL_DB 指临时文件。
HTML 报告：docs/pool/pool-report.html，每次写操作尾部同步重生成，浏览器直开零构建。

四工具面（全员可读，写走单写者纪律——server 侧不加锁，SQLite 单写原子性兜底）：
  pool_add    入池
  pool_fetch  按 P 级/状态/池捞取
  pool_update 状态机校验 + 溯源 + 子表记录 + 报告重生成
  pool_search FTS5 关键词检索（查重铁律与开工前检索走它）
"""

from __future__ import annotations

import html
import os
import json
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

# mcp 2.x 起 FastMCP 更名 MCPServer（装饰器 tool / run(stdio) 用法同形）
from mcp.server.mcpserver import MCPServer

# ---------- 路径与连接 ----------

def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _db_path() -> Path:
    env = os.environ.get("POOL_DB")
    if env:
        return Path(env)
    return _repo_root() / "docs" / "pool" / "pool.db"


def _report_path() -> Path:
    env = os.environ.get("POOL_REPORT")
    if env:
        return Path(env)
    return _db_path().with_name("pool-report.html")


def _now() -> str:
    return datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S %z")


def _connect() -> sqlite3.Connection:
    db = _db_path()
    db.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    _ensure_schema(conn)
    return conn


def _ensure_schema(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS needs (
          id INTEGER PRIMARY KEY,
          pool TEXT CHECK(pool IN ('need','bug','debt')),
          title TEXT NOT NULL,
          priority TEXT CHECK(priority IN ('P0','P1','P2','P3')),
          status TEXT CHECK(status IN ('在池','排产','进行中','待验收','关闭','挂起','拒绝')),
          blocker TEXT,
          owner TEXT,
          detail TEXT,
          created TEXT, updated TEXT
        );
        CREATE TABLE IF NOT EXISTS dev_progress (
          id INTEGER PRIMARY KEY, need_id REFERENCES needs(id), time TEXT, note TEXT);
        CREATE TABLE IF NOT EXISTS bugs (
          id INTEGER PRIMARY KEY, need_id REFERENCES needs(id), time TEXT, note TEXT);
        CREATE TABLE IF NOT EXISTS acceptance (
          id INTEGER PRIMARY KEY, need_id REFERENCES needs(id), time TEXT, evidence TEXT);
        CREATE TABLE IF NOT EXISTS status_log (
          id INTEGER PRIMARY KEY, need_id REFERENCES needs(id), time TEXT,
          from_status TEXT, to_status TEXT, reason TEXT);
        CREATE VIRTUAL TABLE IF NOT EXISTS needs_fts USING fts5(title, body, content='');
        CREATE TABLE IF NOT EXISTS personal_todo (
          owner TEXT PRIMARY KEY,
          content TEXT NOT NULL,
          status TEXT NOT NULL DEFAULT '进行中',
          updated TEXT
        );
        """
    )
    # body 摘要列（决策文档修订 ④-1）：检索面=body，detail 保持指针/长文原样；
    # 老 db 无此列则 ALTER 迁移并全量重建 FTS（contentless 重建只能整表重来）。
    cols = {r[1] for r in conn.execute("PRAGMA table_info(needs)")}
    if "body" not in cols:
        conn.execute("ALTER TABLE needs ADD COLUMN body TEXT NOT NULL DEFAULT ''")
        # 存量行 body 从 detail 回填摘要（body 空才回填，不覆盖已有摘要）
        conn.execute("UPDATE needs SET body = substr(COALESCE(detail,''),1,400) WHERE body = ''")
        conn.execute("INSERT INTO needs_fts(needs_fts) VALUES ('delete-all')")
        for r in conn.execute("SELECT id, title, body FROM needs"):
            _fts_index(conn, r["id"], r["title"], r["body"] or "")
    cols = {r[1] for r in conn.execute("PRAGMA table_info(bugs)")}
    if "resolved" not in cols:
        # 存量行 DEFAULT 1 = 祖父化（迁移前的历史 BUG 视为已处理），否则老需求永远关不上；
        # 新行由 pool_update(bug=...) 显式写 0（未处理），关闭闸按 0 计数
        conn.execute("ALTER TABLE bugs ADD COLUMN resolved INTEGER NOT NULL DEFAULT 1")
    conn.commit()

    # ---- 审计四件套迁移（表结构规范 §6.5）：created_by/updated_by/deleted ----
    # needs 已有 created/updated 时间戳列；人员与软删标记缺失即补，存量行回填
    # created_by/updated_by=owner（迁移前无从考证，owner 即台账负责人）、deleted=0。
    for table in ("needs", "dev_progress", "bugs", "acceptance", "status_log"):
        tcols = {r[1] for r in conn.execute(f"PRAGMA table_info({table})")}
        backfill = ""
        if table == "needs":
            backfill = "UPDATE needs SET created_by=owner, updated_by=owner"
        for col in ("created_by", "updated_by"):
            if col not in tcols:
                conn.execute(f"ALTER TABLE {table} ADD COLUMN {col} TEXT")
        if "deleted" not in tcols:
            conn.execute(f"ALTER TABLE {table} ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0")
        if backfill:
            conn.execute(backfill)
    # stage 阶段字段（池 36 看板）：status 管生死、stage 管流程段（已分析/已调研/
    # 开发中/测试中/已验收），自由文本工具层维护；statusLog 等子表不需要
    ncols = {r[1] for r in conn.execute("PRAGMA table_info(needs)")}
    if "stage" not in ncols:
        conn.execute("ALTER TABLE needs ADD COLUMN stage TEXT NOT NULL DEFAULT ''")
    conn.commit()

    # personal_todo 按规范重建：id 自增 + 四件套 + 每人一条（owner 部分唯一索引，
    # 软删后可重新写入，未删行之间仍互斥）
    tcols = {r[1] for r in conn.execute("PRAGMA table_info(personal_todo)")}
    if "id" not in tcols:
        old_rows = [dict(r) for r in conn.execute("SELECT * FROM personal_todo")]
        conn.execute("DROP TABLE personal_todo")
        conn.execute(
            "CREATE TABLE personal_todo ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " owner TEXT NOT NULL, content TEXT NOT NULL,"
            " status TEXT NOT NULL DEFAULT '进行中', updated TEXT,"
            " created_by TEXT, updated_by TEXT, deleted INTEGER NOT NULL DEFAULT 0)"
        )
        conn.execute(
            "CREATE UNIQUE INDEX idx_personal_todo_owner_active"
            " ON personal_todo(owner) WHERE deleted = 0")
        for r in old_rows:
            conn.execute(
                "INSERT INTO personal_todo(owner, content, status, updated,"
                " created_by, updated_by, deleted) VALUES (?,?,?,?,?,?,0)",
                (r["owner"], r["content"], r["status"], r["updated"],
                 r["owner"], r["owner"]),
            )
    conn.commit()


# ---------- 状态机（决策文档 §4，非法迁移拒返回错误） ----------

TRANSITIONS: dict[str, set[str]] = {
    "在池": {"排产", "挂起", "拒绝"},
    "排产": {"进行中", "挂起"},
    "进行中": {"待验收", "挂起"},
    "待验收": {"关闭", "进行中"},
    "挂起": {"在池"},
    "关闭": set(),
    "拒绝": set(),
}

POOL_NAMES = {"need": "需求池", "bug": "bug 池", "debt": "待办池"}
PRIORITY_ORDER = {"P0": 0, "P1": 1, "P2": 2, "P3": 3}


def _cjk_space(s: str) -> str:
    # 默认 unicode61 分词器把连续汉字当**一个**长词，「冲鸭」查不中正文里的
    # 「…关键词冲鸭」。索引与查询两侧都在相邻 CJK 字符间插空格，把汉字拆成
    # 单字 token、查询侧用短语匹配还原相邻性——ASCII 词不受影响。
    out: list[str] = []
    for ch in s:
        if out and _is_cjk(out[-1]) and _is_cjk(ch):
            out.append(" ")
        out.append(ch)
    return "".join(out)


def _is_cjk(ch: str) -> bool:
    code = ord(ch)
    return 0x4E00 <= code <= 0x9FFF or 0x3400 <= code <= 0x4DBF


def _fts_query(keyword: str) -> str:
    return '"%s"' % _cjk_space(keyword).replace('"', '""')


def _summarize(detail: str) -> str:
    # body 摘要：入库时截取 detail 前 400 字符供 FTS 检索；detail 本身不截。
    return (detail or "")[:400]


def _fts_delete(conn: sqlite3.Connection, need_id: int) -> None:
    # content='' 的 contentless FTS5 删改必须走特殊 'delete' 命令，不能直接
    # DELETE/UPDATE——少这步会留下幽灵索引行，检索出已不存在的条目。
    conn.execute(
        "INSERT INTO needs_fts(needs_fts, rowid, title, body) "
        "SELECT 'delete', ?, title, COALESCE(body,'') FROM needs WHERE id = ?",
        (need_id, need_id),
    )


def _fts_index(conn: sqlite3.Connection, need_id: int, title: str, detail: str) -> None:
    conn.execute(
        "INSERT INTO needs_fts(rowid, title, body) VALUES (?, ?, ?)",
        (need_id, _cjk_space(title), _cjk_space(detail or "")),
    )


# ---------- MCP 工具 ----------

mcp = MCPServer("pool")


@mcp.tool()
def pool_add(pool: str, title: str, priority: str, owner: str, detail: str = "") -> dict:
    """入池。pool ∈ need|bug|debt；priority ∈ P0-P3。返回新条目 id。"""
    if pool not in POOL_NAMES:
        return {"ok": False, "error": f"pool 必须是 need|bug|debt，收到 {pool!r}"}
    if priority not in PRIORITY_ORDER:
        return {"ok": False, "error": f"priority 必须是 P0-P3，收到 {priority!r}"}
    now = _now()
    with _connect() as conn:
        cur = conn.execute(
            "INSERT INTO needs(pool, title, priority, status, owner, detail, body,"
            " created, updated, created_by, updated_by, deleted)"
            " VALUES (?, ?, ?, '在池', ?, ?, ?, ?, ?, ?, ?, 0)",
            (pool, title, priority, owner, detail, _summarize(detail), now, now,
             owner, owner),
        )
        need_id = cur.lastrowid
        conn.execute(
            "INSERT INTO status_log(need_id, time, from_status, to_status, reason,"
            " created_by, updated_by, deleted) VALUES (?,?,?,?,?,?,?,0)",
            (need_id, now, None, "在池", "入池", owner, owner),
        )
        _fts_index(conn, need_id, title, detail)
    _write_report()
    _write_snapshot()
    return {"ok": True, "id": need_id}


@mcp.tool()
def pool_fetch(
    priority: str | None = None,
    status: str | None = None,
    pool: str | None = None,
    owner: str | None = None,
    need_id: int | None = None,
) -> dict:
    """捞取。常规：按 P 级/状态/池/负责人过滤 needs（owner=自己=身份绑定查询）。
    need_id 给定时切换为**按需求查子表**（外键关联查询，与身份绑定独立）：
    返回该需求挂的 bugs 子表行（time/note），status 过滤作用于**父需求状态**——
    status=在池 → 只看父需求未关闭的（=未处理完的 BUG）；不带 status → 全部。"""
    if need_id is not None:
        sql = ("SELECT b.id, b.need_id, b.time, b.note, n.status AS parent_status"
               " FROM bugs b JOIN needs n ON n.id = b.need_id"
               " WHERE b.need_id = ? AND b.deleted = 0")
        args: list = [need_id]
        if status:
            sql += " AND n.status = ?"
            args.append(status)
        sql += " ORDER BY b.time, b.id"
        with _connect() as conn:
            rows = [dict(r) for r in conn.execute(sql, args)]
        return {"ok": True, "kind": "bugs_by_need", "rows": rows}
    sql = "SELECT * FROM needs WHERE deleted = 0"
    args: list = []
    if priority:
        sql += " AND priority = ?"
        args.append(priority)
    if status:
        sql += " AND status = ?"
        args.append(status)
    if pool:
        sql += " AND pool = ?"
        args.append(pool)
    if owner:
        sql += " AND owner = ?"
        args.append(owner)
    sql += " ORDER BY CASE priority "
    sql += " ".join(f"WHEN '{p}' THEN {i}" for p, i in PRIORITY_ORDER.items())
    sql += " END, id"
    with _connect() as conn:
        rows = [dict(r) for r in conn.execute(sql, args)]
    return {"ok": True, "rows": rows}


@mcp.tool()
def pool_update(
    id: int,
    status: str | None = None,
    priority: str | None = None,
    owner: str | None = None,
    blocker: str | None = None,
    note: str | None = None,
    bug: str | None = None,
    evidence: str | None = None,
    reason: str | None = None,
    detail: str | None = None,
    bug_resolved: int | None = None,
    by: str | None = None,
    stage: str | None = None,
) -> dict:
    """更新条目。状态迁移按状态机校验；note→dev_progress、bug→bugs（未处理行）、
    bug_resolved=行id 标记该 BUG 已处理；关闭必须带 evidence→acceptance 且子 BUG 全部
    已处理（有未处理行直接拒并报数，调研对齐：比 rollup 更硬的程序闸）；挂起必填
    blocker；拒绝必填 reason；detail 更新走 body 摘要同步（FTS 重建见下方统一刷新）。"""
    now = _now()
    with _connect() as conn:
        row = conn.execute("SELECT * FROM needs WHERE id = ?", (id,)).fetchone()
        if row is None:
            return {"ok": False, "error": f"条目 {id} 不存在"}
        if priority and priority not in PRIORITY_ORDER:
            return {"ok": False, "error": f"priority 必须是 P0-P3，收到 {priority!r}"}

        new_status = status or row["status"]
        if status and status != row["status"]:
            if status not in TRANSITIONS:
                return {"ok": False, "error": f"未知状态 {status!r}"}
            if status not in TRANSITIONS[row["status"]]:
                return {
                    "ok": False,
                    "error": f"非法迁移 {row['status']} → {status}（合法：{sorted(TRANSITIONS[row['status']])}）",
                }
            if status == "挂起" and not (blocker or row["blocker"]):
                return {"ok": False, "error": "挂起必填 blocker（等授权/等环境/条件未触发/登记不派工）"}
            if status == "关闭":
                if not evidence:
                    return {"ok": False, "error": "关闭前置：必须带 evidence（acceptance 记录）"}
            if status == "关闭":
                unresolved = conn.execute(
                    "SELECT COUNT(*) FROM bugs WHERE need_id = ? AND resolved = 0"
                    " AND deleted = 0", (id,)
                ).fetchone()[0]
                if unresolved > 0:
                    return {
                        "ok": False,
                        "error": f"关闭前置：该需求还有 {unresolved} 条未处理 BUG（先逐条 bug_resolved 标记）",
                    }
                conn.execute(
                    "INSERT INTO acceptance(need_id, time, evidence) VALUES (?, ?, ?)",
                    (id, now, evidence),
                )
            if status == "拒绝" and not reason:
                return {"ok": False, "error": "拒绝必填 reason（入 status_log）"}

        fields, args = [], []
        for col, val in (
            ("status", status),
            ("priority", priority),
            ("owner", owner),
            ("blocker", blocker),
            ("detail", detail),
        ):
            if val is not None:
                fields.append(f"{col} = ?")
                args.append(val)
        if detail is not None:
            # body 摘要随 detail 同步（body 是 FTS 的检索面，滞留旧摘要=检索命中旧内容）
            fields.append("body = ?")
            args.append(_summarize(detail))
        fields.append("updated = ?")
        args.append(now)
        if by or owner:
            fields.append("updated_by = ?")
            args.append(by or owner)
        if stage is not None:
            fields.append("stage = ?")
            args.append(stage)
        args.append(id)
        conn.execute(f"UPDATE needs SET {', '.join(fields)} WHERE id = ?", args)

        if status and status != row["status"]:
            AUTO_STAGE = {"排产": "已分析", "进行中": "开发中", "待验收": "测试中",
                          "关闭": "已验收", "拒绝": "已否决"}
            if stage is None and status in AUTO_STAGE:
                fields.append("stage = ?")
                args.append(AUTO_STAGE[status])
            conn.execute(
                "INSERT INTO status_log(need_id, time, from_status, to_status, reason,"
                " created_by, updated_by, deleted) VALUES (?,?,?,?,?,?,?,0)",
                (id, now, row["status"], status, reason or blocker or note or "",
                 by or owner or "", by or owner or ""))
        if note:
            conn.execute(
                "INSERT INTO dev_progress(need_id, time, note) VALUES (?, ?, ?)",
                (id, now, note),
            )
        if bug:
            conn.execute(
                "INSERT INTO bugs(need_id, time, note, resolved, created_by, updated_by, deleted)"
                " VALUES (?, ?, ?, 0, ?, ?, 0)",
                (id, now, bug, by or owner or "", by or owner or ""),
            )
        if bug_resolved is not None:
            conn.execute(
                "UPDATE bugs SET resolved = 1 WHERE id = ? AND need_id = ?",
                (bug_resolved, id),
            )
        # 标题/明细若变更需重建 FTS 行；本工具不改标题，此处按 detail 变更兜底
        fresh = conn.execute("SELECT title, detail FROM needs WHERE id = ?", (id,)).fetchone()
        conn.execute("UPDATE needs SET body = ? WHERE id = ?",
                     (_summarize(fresh["detail"]), id))
        _fts_delete(conn, id)
        _fts_index(conn, id, fresh["title"], _summarize(fresh["detail"]))
    _write_report()
    _write_snapshot()
    return {"ok": True}


@mcp.tool()
def pool_search(keyword: str) -> dict:
    """FTS5 检索标题/正文（查重铁律与开工前检索走它）。"""
    with _connect() as conn:
        rows = [
            dict(r)
            for r in conn.execute(
                "SELECT n.* FROM needs_fts f JOIN needs n ON n.id = f.rowid"
                " WHERE needs_fts MATCH ? AND n.deleted = 0 ORDER BY n.id",
                (_fts_query(keyword),),
            )
        ]
    return {"ok": True, "rows": rows}


@mcp.tool()
def pool_stats(owner: str | None = None, pool: str | None = None) -> dict:
    """统计：按状态计数（在池/排产/进行中/待验收/挂起/关闭/拒绝）。
    owner 传自己的 Bot 名 = 只看自己的账（「我还剩几个 bug」一条龙）；pool 可再限单池。"""
    sql = "SELECT status, COUNT(*) AS n FROM needs WHERE deleted = 0"
    args: list = []
    if owner:
        sql += " AND owner = ?"
        args.append(owner)
    if pool:
        sql += " AND pool = ?"
        args.append(pool)
    sql += " GROUP BY status"
    with _connect() as conn:
        by_status = {r["status"]: r["n"] for r in conn.execute(sql, args)}
    total = sum(by_status.values())
    open_items = sum(n for st, n in by_status.items()
                     if st not in ("关闭", "拒绝"))
    return {"ok": True, "total": total, "open": open_items, "by_status": by_status}


@mcp.tool()
def todo_set(owner: str, content: str, status: str = "进行中") -> dict:
    """写自己的个人 TODO（upsert：每人仅一条，写新的覆盖旧的）。
    owner=自己的 Bot 名；status 自由文本（如 进行中/已完成/blocked）。"""
    with _connect() as conn:
        conn.execute(
            "INSERT INTO personal_todo(owner, content, status, updated,"
            " created_by, updated_by, deleted) VALUES (?,?,?,?,?,?,0)"
            " ON CONFLICT(owner) DO UPDATE SET content=excluded.content,"
            " status=excluded.status, updated=excluded.updated,"
            " updated_by=excluded.updated_by, deleted=0"
            " WHERE personal_todo.deleted = 0",
            (owner, content, status, _now(), owner, owner),
        )
    _write_snapshot()
    return {"ok": True, "owner": owner, "content": content, "status": status}


@mcp.tool()
def todo_get(owner: str | None = None) -> dict:
    """读个人 TODO：owner 给定=看那个人的一条；不给=看全池（各 Bot 一人一条）。"""
    with _connect() as conn:
        if owner:
            row = conn.execute(
                "SELECT * FROM personal_todo WHERE owner = ? AND deleted = 0", (owner,)
            ).fetchone()
            return {"ok": True, "rows": [dict(row)] if row else []}
        rows = [dict(r) for r in conn.execute(
            "SELECT * FROM personal_todo WHERE deleted = 0 ORDER BY updated DESC")]
        return {"ok": True, "rows": rows}


@mcp.tool()
def pool_soft_delete(id: int, by: str) -> dict:
    """软删条目（deleted=1，默认所有读路径不再返回；数据保留可审计回溯）。
    by=操作人 Bot 名，必填（审计链不留无主删除）。"""
    with _connect() as conn:
        cur = conn.execute(
            "UPDATE needs SET deleted = 1, updated = ?, updated_by = ? WHERE id = ?",
            (_now(), by, id),
        )
    if cur.rowcount == 0:
        return {"ok": False, "error": f"条目 {id} 不存在"}
    _write_report()
    _write_snapshot()
    return {"ok": True}


@mcp.tool()
def pool_trace(id: int) -> dict:
    """溯源链：一条需求摸到全部状态变更/进度/BUG/验收（决策文档 §2 溯源查询）。"""
    with _connect() as conn:
        need = conn.execute("SELECT * FROM needs WHERE id = ?", (id,)).fetchone()
        if need is None:
            return {"ok": False, "error": f"条目 {id} 不存在"}
        def q(table: str) -> list[dict]:
            return [
                dict(r)
                for r in conn.execute(
                    f"SELECT * FROM {table} WHERE need_id = ? ORDER BY time, id", (id,)
                )
            ]
        return {
            "ok": True,
            "need": dict(need),
            "status_log": q("status_log"),
            "dev_progress": q("dev_progress"),
            "bugs": q("bugs"),
            "acceptance": q("acceptance"),
        }


# ---------- HTML 报告（决策文档 §5：纯字符串拼 HTML，无模板引擎） ----------

def _esc(s: object) -> str:
    return html.escape(str(s if s is not None else ""))


def _write_report() -> None:
    conn = _connect()
    try:
        needs = [
            dict(r)
            for r in conn.execute(
                "SELECT * FROM needs ORDER BY CASE priority "
                + " ".join(f"WHEN '{p}' THEN {i}" for p, i in PRIORITY_ORDER.items())
                + " END, id"
            )
        ]
        parts = [
            "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>",
            "<title>三池报告</title><style>",
            "body{font-family:-apple-system,sans-serif;margin:24px;color:#241C15}",
            "table{border-collapse:collapse;width:100%;margin:8px 0 24px}",
            "th,td{border:1px solid #d8cfc4;padding:6px 10px;text-align:left;font-size:14px}",
            "th{background:#f3ece2}.P0{color:#c0392b;font-weight:700}",
            "details{margin:8px 0}summary{cursor:pointer;font-weight:600}",
            "h2{border-bottom:2px solid #D97757;padding-bottom:4px}",
            "</style></head><body><h1>三池报告</h1>",
            f"<p>生成时间：{_esc(_now())} ｜ 条目数：{len(needs)}</p>",
        ]
        for pool_key, pool_name in POOL_NAMES.items():
            items = [n for n in needs if n["pool"] == pool_key]
            parts.append(f"<h2>{_esc(pool_name)}（{len(items)}）</h2>")
            if not items:
                parts.append("<p>空</p>")
                continue
            parts.append(
                "<table><tr><th>id</th><th>P</th><th>标题</th><th>状态</th>"
                "<th>blocker</th><th>owner</th><th>更新</th></tr>"
            )
            for n in items:
                parts.append(
                    f"<tr><td>{n['id']}</td><td class='{_esc(n['priority'])}'>"
                    f"{_esc(n['priority'])}</td><td>{_esc(n['title'])}</td>"
                    f"<td>{_esc(n['status'])}</td><td>{_esc(n['blocker'] or '')}</td>"
                    f"<td>{_esc(n['owner'] or '')}</td><td>{_esc(n['updated'])}</td></tr>"
                )
            parts.append("</table>")
            # 在产条目排产视图 + 溯源链（折叠）
            for n in items:
                nid = n["id"]
                parts.append(f"<details><summary>#{nid} {_esc(n['title'])} · 溯源链</summary>")
                for table, label in (
                    ("status_log", "状态变更"),
                    ("dev_progress", "进度"),
                    ("bugs", "BUG"),
                    ("acceptance", "验收"),
                ):
                    rows = [
                        dict(r)
                        for r in conn.execute(
                            f"SELECT * FROM {table} WHERE need_id = ? ORDER BY time, id", (nid,)
                        )
                    ]
                    parts.append(f"<p><b>{label}</b></p><ul>")
                    for r in rows:
                        if table == "status_log":
                            parts.append(
                                f"<li>{_esc(r['time'])}：{_esc(r['from_status'])} → "
                                f"<b>{_esc(r['to_status'])}</b> {_esc(r['reason'])}</li>"
                            )
                        else:
                            text = r.get("note") or r.get("evidence") or ""
                            parts.append(f"<li>{_esc(r['time'])}：{_esc(text)}</li>")
                    parts.append("</ul>" if rows else "<p>（无记录）</p>")
                parts.append("</details>")
        parts.append("</body></html>")
        report = _report_path()
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text("".join(parts), encoding="utf-8")
    finally:
        conn.close()


def _write_snapshot() -> None:
    # 裁决 ④-2：pool.db 出 git，写后导出 JSON 快照入库留痕（可 diff 可 merge，
    # 双机各写本地 db，git 侧只合快照）。
    path = _report_path().parent / "pool-snapshot.json"
    path = _report_path().parent / "pool-snapshot.json"
    conn = _connect()
    try:
        # personal_todo 无 id 列（owner 主键），排序键按表定制
        order = {t: "id" for t in ("needs", "dev_progress", "bugs", "acceptance", "status_log")}
        order["personal_todo"] = "owner"
        data = {
            t: [dict(r) for r in conn.execute(f"SELECT * FROM {t} ORDER BY {order[t]}")]
            for t in ("needs", "dev_progress", "bugs", "acceptance", "status_log", "personal_todo")
        }
    finally:
        conn.close()
    path.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")


# ---------- 入口 ----------

if __name__ == "__main__":
    mcp.run()  # stdio
