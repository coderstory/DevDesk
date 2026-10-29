#!/usr/bin/env python3
"""旧单表 pool_items → 三池五表 一次性迁移（可双机复跑，幂等：全量重建五表）。

pool_items（惨品 10-29 建的唯一真相，21 条）保留作审计参照，不删不改；
needs/dev_progress/bugs/acceptance/status_log/needs_fts 按 POOL_ITEMS 全量重建。
状态映射：open→在池｜in_progress→进行中｜blocked→挂起｜closed→关闭；
blocker：auth→等授权｜env→等环境｜condition→条件未触发｜unplanned→登记不派工。
关闭项自动补一行 acceptance（状态机关闭前置），evidence 注明迁移来源；
编号 = 全局单序列，池内顺序 need→bug→debt，编号即 id（展示层拼 需-N/BUG-N/债-N 前缀）。
用法：POOL_DB=docs/pool/pool.db tools/.pool-mcp-venv/bin/python tools/pool_migrate_legacy.py
"""

from __future__ import annotations

import re
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

STATUS_MAP = {"open": "在池", "in_progress": "进行中", "blocked": "挂起", "closed": "关闭"}
BLOCKER_MAP = {"auth": "等授权", "env": "等环境", "condition": "条件未触发",
               "unplanned": "登记不派工", "none": None}
POOL_ORDER = ["need", "bug", "debt"]


def _summarize(detail: str) -> str:
    return (detail or "")[:400]


def main() -> None:
    db = Path(sys.argv[1] if len(sys.argv) > 1 else
              Path(__file__).resolve().parent.parent / "docs" / "pool" / "pool.db")
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    now = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M:%S %z")
    legacy = [dict(r) for r in conn.execute("SELECT * FROM pool_items")]
    if not legacy:
        print("pool_items 为空，无事可迁"); return

    # 编号：按池序 + 旧 id 数字部分排序，生成全局单序列
    def sort_key(item: dict):
        m = re.search(r"\d+", item["id"])
        return (POOL_ORDER.index(item["pool"]), int(m.group()) if m else 0)
    legacy.sort(key=sort_key)

    conn.execute("PRAGMA foreign_keys = OFF")
    # needs_fts 是 contentless 表，DELETE 全表被禁——直接 DROP 重建（迁移本就全量重灌）
    conn.execute("DROP TABLE needs_fts")
    for table in ("dev_progress", "bugs", "acceptance", "status_log", "needs"):
        conn.execute(f"DELETE FROM {table}")
    conn.execute("CREATE VIRTUAL TABLE needs_fts USING fts5(title, body, content='')")
    conn.commit()

    for new_id, item in enumerate(legacy, start=1):
        status = STATUS_MAP[item["status"]]
        blocker = BLOCKER_MAP[item["blocker"]]
        detail = item["detail"] or ""
        conn.execute(
            "INSERT INTO needs(id, pool, title, priority, status, blocker, owner, detail,"
            " body, created, updated) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            (new_id, item["pool"], item["title"], item["priority"], status, blocker,
             item["owner"] or None, detail, _summarize(detail), item["created"], now),
        )
        conn.execute(
            "INSERT INTO status_log(need_id, time, from_status, to_status, reason)"
            " VALUES (?,?,NULL,?,?)",
            (new_id, item["created"], status, f"自 pool_items {item['id']} 迁移"),
        )
        if status == "关闭":
            conn.execute(
                "INSERT INTO acceptance(need_id, time, evidence) VALUES (?,?,?)",
                (new_id, now, f"迁移前已关闭（pool_items {item['id']}）"),
            )
        conn.execute(
            "INSERT INTO needs_fts(rowid, title, body) VALUES (?,?,?)",
            (new_id, item["title"], _summarize(detail)),
        )
    conn.commit()

    counts = {t: conn.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
              for t in ("pool_items", "needs", "status_log", "needs_fts", "acceptance")}
    print("迁移完成：", counts)
    for r in conn.execute("SELECT id, pool, title, status, blocker FROM needs ORDER BY id"):
        print(dict(r))
    conn.close()
    if counts["needs"] != counts["pool_items"]:
        print("❌ 条数不一致"); sys.exit(1)


if __name__ == "__main__":
    main()
