#!/usr/bin/env python3
"""pool_mcp 验收脚本：F-01~F-06 逐条实跑取证（决策文档 §7 口径）。

真实 MCP stdio 调用：把 tools/pool_mcp.py 当子进程拉起，走 ClientSession 调四工具，
再用 sqlite3 直连临时库做 db 断言。POOL_DB/POOL_REPORT 指向临时目录，不碰真实池。
退出码 0 = 全过；任何一条红即非零（自定义 check 必须 raise）。
"""

from __future__ import annotations

import asyncio
import os
import sqlite3
import sys
import tempfile
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

SERVER = Path(__file__).resolve().parent / "pool_mcp.py"
VENV_BIN = Path(__file__).resolve().parent / ".pool-mcp-venv" / "bin" / "python"


def db_rows(db: str, sql: str, args: tuple = ()) -> list[sqlite3.Row]:
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    try:
        return conn.execute(sql, args).fetchall()
    finally:
        conn.close()


async def run() -> None:
    tmp = tempfile.mkdtemp(prefix="pool-mcp-acceptance-")
    db = str(Path(tmp) / "pool.db")
    report = str(Path(tmp) / "pool-report.html")
    env = {**os.environ, "POOL_DB": db, "POOL_REPORT": report}
    params = StdioServerParameters(
        command=str(VENV_BIN if VENV_BIN.exists() else sys.executable),
        args=[str(SERVER)],
        env=env,
    )
    failures: list[str] = []

    def check(name: str, cond: bool, detail: str = "") -> None:
        print(f"{'✅' if cond else '❌'} {name}{('：' + detail) if detail else ''}")
        if not cond:
            failures.append(name)

    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await session.list_tools()
            names = {t.name for t in tools.tools}
            check("工具面注册", {"pool_add", "pool_fetch", "pool_update", "pool_search"} <= names,
                  f"tools={sorted(names)}")

            # F-01 入池 → needs 一行 + status_log 一行「在池」
            r = (await session.call_tool("pool_add", {
                "pool": "need", "title": "验收用示例需求", "priority": "P1",
                "owner": "元气小萝莉", "detail": "FTS 检索正文关键词冲鸭",
            })).content[0].text
            check("F-01 add 返回 ok", '"ok": true' in r.replace(" ", " "), r)
            need_id = 1
            log = db_rows(db, "SELECT * FROM status_log WHERE need_id=1")
            check("F-01 status_log 一行→在池", len(log) == 1 and log[0]["to_status"] == "在池")
            need = db_rows(db, "SELECT * FROM needs WHERE id=1")
            check("F-01 needs 一行", len(need) == 1 and need[0]["status"] == "在池")

            # F-02 指派 jQuery 排产 → status=排产 + dev_progress 起记录
            r = (await session.call_tool("pool_update", {
                "id": need_id, "status": "排产", "owner": "jQuery", "note": "排产起记录",
            })).content[0].text
            check("F-02 排产 ok", '"ok": true' in r.replace(" ", " "), r)
            prog = db_rows(db, "SELECT * FROM dev_progress WHERE need_id=1")
            check("F-02 dev_progress 一行", len(prog) == 1 and prog[0]["note"] == "排产起记录")
            st = db_rows(db, "SELECT status FROM needs WHERE id=1")[0]["status"]
            check("F-02 status=排产", st == "排产")

            # F-03 进行中发现 BUG → bugs 一行外键回 needs，可从需求查到 BUG
            await session.call_tool("pool_update", {"id": need_id, "status": "进行中"})
            r = (await session.call_tool("pool_update", {
                "id": need_id, "bug": "示例 BUG：边界条件炸了",
            })).content[0].text
            check("F-03 bug 记录 ok", '"ok": true' in r.replace(" ", " "), r)
            bugs = db_rows(
                db,
                "SELECT b.note FROM bugs b JOIN needs n ON n.id=b.need_id WHERE n.id=?",
                (need_id,),
            )
            check("F-03 bugs 外键可查", len(bugs) == 1 and "边界条件" in bugs[0]["note"])

            # 状态机：待验收→关闭必须带 evidence；不带必须拒（先红判据）
            await session.call_tool("pool_update", {"id": need_id, "status": "待验收"})
            r = (await session.call_tool("pool_update", {"id": need_id, "status": "关闭"})).content[0].text
            check("状态机：无 evidence 关闭被拒", '"ok": false' in r.replace(" ", " "), r)
            # 关闭闸：F-03 挂的 BUG 未处理 → 带 evidence 也拒（调研采纳：子 BUG 全关闭前置）
            r = (await session.call_tool("pool_update", {
                "id": need_id, "status": "关闭", "evidence": "全绿证据物",
            })).content[0].text
            check("关闭闸：子 BUG 未处理被拒", '"ok": false' in r.replace(" ", " ") and "未处理 BUG" in r, r)
            # 标记已处理后放行
            r = (await session.call_tool("pool_update", {"id": need_id, "bug_resolved": 1})).content[0].text
            check("bug_resolved 标记 ok", '"ok": true' in r.replace(" ", " "), r)
            r = (await session.call_tool("pool_update", {
                "id": need_id, "status": "关闭", "evidence": "全绿证据物",
            })).content[0].text
            check("F-04 处理完关闭 ok", '"ok": true' in r.replace(" ", " "), r)
            acc = db_rows(db, "SELECT * FROM acceptance WHERE need_id=1")
            check("F-04 acceptance 一行 + status=关闭",
                  len(acc) == 1
                  and db_rows(db, "SELECT status FROM needs WHERE id=1")[0]["status"] == "关闭",
                  f"acceptance={len(acc)} status={db_rows(db, 'SELECT status FROM needs WHERE id=1')[0]['status']}")

            # F-05 FTS 检索命中 + 按 P 级/状态捞取
            r = (await session.call_tool("pool_search", {"keyword": "冲鸭"})).content[0].text
            check("F-05 FTS 命中正文", "验收用示例需求" in r, r)
            r = (await session.call_tool("pool_search", {"keyword": "不存在词xyz"})).content[0].text
            check("F-05 FTS 未命中返回空", '"rows": []' in r.replace(" ", " "))
            r = (await session.call_tool("pool_add", {
                "pool": "bug", "title": "P0 示例 bug", "priority": "P0", "owner": "史密斯专员",
            })).content[0].text
            r = (await session.call_tool("pool_fetch", {"pool": "bug"})).content[0].text
            check("F-05 按池捞取命中", "P0 示例 bug" in r)
            r = (await session.call_tool("pool_fetch", {"priority": "P0"})).content[0].text
            check("F-05 按 P 级捞取命中", "P0 示例 bug" in r and "验收用示例需求" not in r)

            # F-06 报告重生成 + 内容含最新状态与溯源链
            mtime1 = Path(report).stat().st_mtime_ns
            await session.call_tool("pool_update", {"id": 2, "status": "挂起", "blocker": "等环境"})
            mtime2 = Path(report).stat().st_mtime_ns
            check("F-06 写操作后报告重生成（mtime 变化）", mtime2 > mtime1)
            content = Path(report).read_text(encoding="utf-8")
            check("F-06 报告含三池总览与挂起态", "需求池" in content and "挂起" in content and "等环境" in content)
            check("F-06 报告含溯源链与验收", "状态变更" in content and "全绿证据物" in content)

            # 状态机非法迁移反证：关闭→排产 必须拒
            r = (await session.call_tool("pool_update", {"id": need_id, "status": "排产"})).content[0].text
            check("反证：关闭→排产 被拒", '"ok": false' in r.replace(" ", " "), r)

    print()
    if failures:
        print(f"❌ 验收失败 {len(failures)} 条：{failures}")
        sys.exit(1)
    print("✅ F-01~F-06 全过（真实 MCP stdio 调用 + db 断言）")


if __name__ == "__main__":
    asyncio.run(run())
