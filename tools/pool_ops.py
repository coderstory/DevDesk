#!/usr/bin/env python3
"""池 SQLite 运维工具：备份 / 还原 / venv 修复（跨机部署三步的脚本化形态）。

子命令：
  backup                 备份整库为 SQL 文本 → docs/pool/pool-backup-<时间戳>.sql
  restore [FILE]         从 SQL 文件还原；无参数=取时间戳最新的 pool-backup-*.sql
  reinstall-venv         重建 tools/.pool-mcp-venv（pip install mcp），让 pool_mcp.py 可跑
  sync                   重生成 HTML 报告+JSON 快照（直写 SQL 后必跑，防 git 留痕漂移）

设计约定：
- 纯 stdlib（sqlite3/os/glob），无 venv 依赖——venv 坏了本工具仍能跑（restore 不依赖被救对象）。
- 备份走 conn.iterdump()（完整 SQL 文本，含表/索引/虚拟表建句与数据），跨 SQLite 版本可重放。
- 还原 = 先把当前库另存一份保险备份，再删库文件重建重放——半还原状态不可能存在。
- 还原后跑 PRAGMA foreign_key_check + needs 行数断言，坏还原当场非零退出。
- Windows（Git Bash）：python 传参同名；时间戳本地时区。

用法：
  tools/.pool-mcp-venv/bin/python tools/pool_ops.py backup
  tools/.pool-mcp-venv/bin/python tools/pool_ops.py restore            # 最新一份
  tools/.pool-mcp-venv/bin/python tools/pool_ops.py restore <file.sql>
  tools/.pool-mcp-venv/bin/python tools/pool_ops.py reinstall-venv
"""

from __future__ import annotations

import glob
import os
import sqlite3
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POOL_DIR = ROOT / "docs" / "pool"
# POOL_DB 覆盖与 pool_mcp.py 同约定；备份落盘目录跟随 db 所在目录
DB = Path(os.environ.get("POOL_DB") or str(POOL_DIR / "pool.db"))
POOL_DIR = DB.parent
BACKUP_GLOB = "pool-backup-*.sql"
VENV = ROOT / "tools" / ".pool-mcp-venv"


def _stamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def backup() -> int:
    if not DB.exists():
        print(f"❌ 库文件不存在：{DB}（首次部署应先跑 restore 或迁移脚本）")
        return 1
    out = POOL_DIR / f"pool-backup-{_stamp()}.sql"
    conn = sqlite3.connect(DB)
    try:
        with open(out, "w", encoding="utf-8") as f:
            # iterdump 逐行产出完整 SQL；BEGIN/COMMIT 包住保证单事务重放原子性。
            # needs_fts 行必须滤掉：contentless FTS5（content=''）不接受 iterdump
            # 产出的无列名 INSERT，重放必炸「no such table」——FTS 由尾部按 needs
            # 全量重建（它本就是 needs 的派生索引，无独立数据）。
            for line in conn.iterdump():
                if "needs_fts" in line:
                    continue
                f.write(line + "\n")
            f.write("CREATE VIRTUAL TABLE needs_fts USING fts5(title, body, content='');\n")
            f.write("INSERT INTO needs_fts(rowid, title, body) "
                    "SELECT id, title, body FROM needs;\n")
    finally:
        conn.close()
    n = sqlite3.connect(DB).execute("SELECT COUNT(*) FROM needs").fetchone()[0]
    print(f"✅ 已备份 {n} 条需求 → {out.name}")
    return 0


def _latest_backup() -> Path | None:
    files = sorted(glob.glob(str(POOL_DIR / BACKUP_GLOB)))
    # 文件名含同粒度时间戳，字典序=时间序，最后一个即最新
    return Path(files[-1]) if files else None


def restore(explicit: str | None) -> int:
    target = Path(explicit) if explicit else _latest_backup()
    if target is None:
        print(f"❌ 找不到备份文件（{POOL_DIR}/{BACKUP_GLOB}），先跑 backup")
        return 1
    if not target.exists():
        print(f"❌ 备份文件不存在：{target}")
        return 1
    sql = target.read_text(encoding="utf-8")
    if "CREATE TABLE" not in sql:
        print(f"❌ {target.name} 不像本工具产出的备份（无 CREATE TABLE），拒绝还原")
        return 1

    # 还原前保险：当前库先留一份带保险标记的备份，防手滑还原到更旧文件后无法回头
    if DB.exists():
        keep = POOL_DIR / f"pool-backup-{_stamp()}-pre-restore.sql"
        conn = sqlite3.connect(DB)
        with open(keep, "w", encoding="utf-8") as f:
            for line in conn.iterdump():
                f.write(line + "\n")
        conn.close()
        print(f"   还原前保险备份 → {keep.name}")

    # 删库重建：SQLite 单文件，删主文件+journal/wal 即干净重建，半状态不存在
    for suffix in ("", "-journal", "-wal", "-shm"):
        p = Path(str(DB) + suffix)
        if p.exists():
            p.unlink()
    conn = sqlite3.connect(DB)
    try:
        conn.executescript(sql)
        conn.execute("PRAGMA foreign_key_check")
        violations = conn.execute("PRAGMA foreign_key_check").fetchall()
        needs = conn.execute("SELECT COUNT(*) FROM needs").fetchone()[0]
        fts = conn.execute("SELECT COUNT(*) FROM needs_fts").fetchone()[0]
    finally:
        conn.close()
    if violations:
        print(f"❌ 还原后外键校验失败 {len(violations)} 条，库已可疑，用保险备份回滚")
        return 1
    print(f"✅ 已从 {target.name} 还原：needs={needs} needs_fts={fts}，外键校验通过")
    return 0


def reinstall_venv() -> int:
    py = sys.executable
    for step in ([py, "-m", "venv", str(VENV)],
                 [str(VENV / ("Scripts" if os.name == "nt" else "bin") / "pip"),
                  "install", "--quiet", "mcp"]):
        print("▶", " ".join(step))
        if subprocess.call(step) != 0:
            print("❌ 步骤失败（见上）")
            return 1
    check = subprocess.run([str(VENV / ("Scripts" if os.name == "nt" else "bin") / "python"),
                            "-c", "import mcp; import sqlite3; c=sqlite3.connect(':memory:');"
                                  "c.execute('CREATE VIRTUAL TABLE t USING fts5(x)');"
                                  "print('venv OK: mcp 可用, FTS5 可用')"],
                           capture_output=True, text=True)
    print(check.stdout.strip() or check.stderr.strip())
    return check.returncode


def sync() -> int:
    """直写 SQL（绕过 MCP 工具）后重生成报告+快照——报告/快照的自动重生成只挂在
    工具出口上，绕工具的写必须手动跑本命令，否则 git 侧留痕与库漂移。"""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import importlib.util
    spec = importlib.util.spec_from_file_location("pool_mcp", Path(__file__).parent / "pool_mcp.py")
    mod = importlib.util.module_from_spec(spec)
    os.environ.setdefault("POOL_DB", str(DB))
    spec.loader.exec_module(mod)
    mod._write_report()
    mod._write_snapshot()
    print(f"✅ 报告与快照已从 {DB.name} 重生成")
    return 0


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "backup":
        return backup()
    if cmd == "restore":
        return restore(sys.argv[2] if len(sys.argv) > 2 else None)
    if cmd == "reinstall-venv":
        return reinstall_venv()
    if cmd == "sync":
        return sync()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
