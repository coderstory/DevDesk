#!/usr/bin/env python3
"""memory-lab 命令行入口。

用法：
    python run.py ingest [--project NAME] [--since TIME] [--limit N]
    python run.py stats
    python run.py init

所有子命令支持 --home / --db / --cc-root 覆盖对应配置。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# 允许直接 `python run.py`：把本文件所在目录放进 import 路径
sys.path.insert(0, str(Path(__file__).resolve().parent))

from memory_lab.config import load_config  # noqa: E402
from memory_lab.db import connect, init_schema, stats  # noqa: E402
from memory_lab.ingest import ingest  # noqa: E402


def _config_from_args(args):
    overrides = {}
    if getattr(args, "home", None):
        overrides["home"] = args.home
    if getattr(args, "db", None):
        overrides["db_path"] = args.db
    if getattr(args, "cc_root", None):
        overrides["cc_projects_root"] = args.cc_root
    return load_config(overrides)


def _add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--home", help="记忆库根目录（默认 ~/.devdesk/memory-lab）")
    parser.add_argument("--db", help="数据库文件路径（默认 <home>/memory.db）")
    parser.add_argument("--cc-root", help="Claude Code 会话根（默认 ~/.claude/projects）")


def cmd_ingest(args) -> int:
    cfg = _config_from_args(args)
    summary = ingest(cfg, project=args.project, since=args.since, limit=args.limit)
    if args.json:
        print(
            json.dumps(
                {
                    "sessions": summary.sessions,
                    "events": summary.events,
                    "skipped": summary.skipped,
                    "errors": summary.errors,
                    "db": str(cfg.db_path),
                },
                ensure_ascii=False,
            )
        )
    else:
        print(
            f"摄取完成：会话 {summary.sessions} 个，事件 {summary.events} 条，"
            f"坏行跳过 {summary.skipped} 条，失败 {summary.errors} 个"
        )
        print(f"数据库：{cfg.db_path}")
    return 0 if summary.errors == 0 else 1


def cmd_stats(args) -> int:
    cfg = _config_from_args(args)
    conn = connect(cfg.db_path)
    try:
        init_schema(conn)
        data = stats(conn)
    finally:
        conn.close()
    if args.json:
        print(json.dumps(data, ensure_ascii=False))
    else:
        print(f"数据库：{cfg.db_path}")
        print(f"会话 {data['sessions']} / 事件 {data['events']} / 观察 {data['observations']}")
        print(f"嵌入 {data['embeddings']} / 提案 {data['proposals']}")
        if data["active_by_project"]:
            print("活跃观察（按项目）：")
            for project, count in data["active_by_project"].items():
                print(f"  {project}: {count}")
    return 0


def cmd_init(args) -> int:
    cfg = _config_from_args(args)
    conn = connect(cfg.db_path)
    try:
        init_schema(conn)
    finally:
        conn.close()
    print(f"已初始化：{cfg.db_path}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="memory-lab", description="DevDesk 本地记忆实验台")
    sub = parser.add_subparsers(dest="command", required=True)

    p_ingest = sub.add_parser("ingest", help="摄取 Claude Code 会话 jsonl")
    _add_common(p_ingest)
    p_ingest.add_argument("--project", help="只摄取某个项目目录（如 D--project-gui）")
    p_ingest.add_argument("--since", help="只摄取 mtime 晚于该时间的文件（epoch 或 ISO）")
    p_ingest.add_argument("--limit", type=int, help="最多处理最新的 N 个会话文件")
    p_ingest.add_argument("--json", action="store_true", help="输出 JSON")
    p_ingest.set_defaults(func=cmd_ingest)

    p_stats = sub.add_parser("stats", help="显示记忆库统计")
    _add_common(p_stats)
    p_stats.add_argument("--json", action="store_true", help="输出 JSON")
    p_stats.set_defaults(func=cmd_stats)

    p_init = sub.add_parser("init", help="初始化数据库 schema")
    _add_common(p_init)
    p_init.set_defaults(func=cmd_init)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())