#!/usr/bin/env python3
"""pool_dashboard_gen.py — 团队看板页生成器（老板视图：谁在干什么/每个需求到哪一步）。

用法：python3 tools/pool_dashboard_gen.py [pool.db 路径] [输出 HTML 路径]
缺省：docs/pool/pool.db → docs/pool/pool-dashboard.html

取数纪律：直接读池库实况（与 tools/pool_review_gen.py 同源同纪律，不手抄）。
列自适应：池库表结构规范（业务方案 §6.5）的四件套审计列（created_by/updated_by/
deleted）迁移落地后自动进入展示（updated_by=「最后谁动的」）；deleted=1 的行
（软删）不出页。迁移前后均可运行。池库变化后重跑本脚本刷新页面。
"""
import json
import sqlite3
import sys
import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DB = REPO / "docs/pool/pool.db"
OUT = REPO / "docs/pool/pool-dashboard.html"

# 状态 → 展示阶段与色（老板口径：已分析/已调研/开发中/测试中/已验收/已关闭 + 挂起/拒绝）
STAGE_VIEW = {
    "新入池": ("已入池·待分析", "#8A93A6"),
    "已分析": ("已分析", "#C9A227"),
    "已调研": ("已调研", "#C9A227"),
    "进行中": ("开发中", "#D97757"),
    "开发中": ("开发中", "#D97757"),
    "测试中": ("测试中", "#5B9BD5"),
    "已验收": ("已验收", "#4CAF7D"),
    "挂起": ("挂起·等条件", "#8A93A6"),
    "拒绝": ("已拒绝·不做", "#7A6A5F"),
    "关闭": ("已关闭", "#4CAF7D"),
}


def stage_of(status: str):
    return STAGE_VIEW.get(status, (status, "#8A93A6"))


# stage 阶段字段（status 管生死、stage 管流程段）的展示档；业务侧补档只改这里
STAGE_LABEL = {
    "已分析": ("已分析", "#C9A227"),
    "已调研": ("已调研", "#C9A227"),
    "开发中": ("开发中", "#D97757"),
    "测试中": ("测试中", "#5B9BD5"),
    "已验收": ("已验收", "#4CAF7D"),
}


def table_cols(conn, table: str):
    return [r[1] for r in conn.execute(f"PRAGMA table_info({table})")]


def main() -> None:
    db_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DB
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row

    needs_cols = table_cols(conn, "needs")
    has_audit = "updated_by" in needs_cols  # 四件套迁移落地标志
    soft_delete = "deleted" in needs_cols
    where = "WHERE deleted = 0" if soft_delete else ""
    needs = [dict(r) for r in conn.execute(
        f"SELECT * FROM needs {where} ORDER BY "
        "CASE status WHEN '进行中' THEN 0 WHEN '开发中' THEN 0 WHEN '测试中' THEN 1 "
        "WHEN '已验收' THEN 2 ELSE 3 END, id")]

    todo_table = "personal_todo" in [
        r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")]
    todos = []
    if todo_table:
        todos = [dict(r) for r in conn.execute("SELECT * FROM personal_todo ORDER BY updated DESC")]

    conn.close()

    # 汇总卡（计数按 status 生死口径；stage 流程段并行展示在需求行）
    stage_counts = {}
    for n in needs:
        label = stage_of(n["status"])[0]
        stage_counts[label] = stage_counts.get(label, 0) + 1
    has_stage = "stage" in needs_cols
    not_done = sum(1 for n in needs if n["status"] not in ("关闭", "拒绝"))
    done = len(needs) - not_done

    payload = {
        "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
        "auditCols": has_audit,
        "hasStage": has_stage,
        "needs": needs,
        "todos": todos,
        "stageCounts": stage_counts,
        "notDone": not_done,
        "done": done,
        "stages": {k: v[0] for k, v in STAGE_VIEW.items()},
    }

    html = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8"><meta charset="UTF-8">
<title>团队看板 — 谁在干什么 / 需求进度全景</title>
<style>
  body { font-family: -apple-system, "PingFang SC", sans-serif; margin: 0;
         background: #241C15; color: #F5EDE4; }
  header { padding: 24px 32px 8px; }
  h1 { font-size: 22px; margin: 0 0 4px; color: #D97757; }
  .meta { color: #B9A99B; font-size: 13px; }
  main { padding: 8px 32px 48px; }
  .cards { display: flex; gap: 12px; flex-wrap: wrap; margin: 16px 0; }
  .card { background: rgba(245,237,228,.06); border: 1px solid rgba(217,119,87,.25);
          border-radius: 10px; padding: 12px 18px; min-width: 130px; }
  .card .num { font-size: 30px; font-weight: 700; }
  .card .label { font-size: 12px; color: #B9A99B; }
  h2 { font-size: 17px; color: #D97757; margin: 28px 0 8px; }
  table { border-collapse: collapse; width: 100%; font-size: 14px; }
  th { text-align: left; color: #B9A99B; font-weight: 500; padding: 6px 10px;
       border-bottom: 1px solid rgba(217,119,87,.35); }
  td { padding: 7px 10px; border-bottom: 1px solid rgba(245,237,228,.08); }
  .stage { border-radius: 999px; padding: 2px 10px; font-size: 12px; white-space: nowrap; }
  .owner { color: #E8B49A; }
  .todo { background: rgba(245,237,228,.06); border-radius: 8px;
          padding: 8px 14px; margin: 6px 0; display: flex; gap: 12px; }
  .todo .who { color: #E8B49A; min-width: 120px; font-weight: 600; }
</style>
</head>
<body>
<header>
  <h1>团队看板</h1>
  <div class="meta">池库实况 docs/pool/pool.db · 生成 __GEN__ ·
  数据变了重跑 tools/pool_dashboard_gen.py 刷新</div>
</header>
<main><div id="app"></div></main>
<script>
const DATA = __PAYLOAD__;
const COLORS = __STAGECOLORS__;
const app = document.getElementById('app');

// 汇总卡
const cards = Object.entries(DATA.stageCounts)
  .sort((a, b) => b[1] - a[1])
  .map(([label, num]) =>
    `<div class="card"><div class="num" style="color:${COLORS[label] || '#F5EDE4'}">${num}</div>
     <div class="label">${label}</div></div>`).join('');
app.innerHTML = `<div class="cards">
  <div class="card"><div class="num" style="color:#D97757">${DATA.notDone}</div>
    <div class="label">未完结需求</div></div>
  <div class="card"><div class="num" style="color:#4CAF7D">${DATA.done}</div>
    <div class="label">已关闭/已拒绝</div></div>${cards}</div>`;

// 需求总表
let rows = DATA.needs.map(n => {
  const stage = DATA.stages[n.status] || n.status;
  const color = COLORS[stage] || '#8A93A6';
  const who = n.owner || '—';
  const lastBy = DATA.auditCols && n.updated_by ? ` · 最后操作：${n.updated_by}` : '';
  // stage 流程段（已分析/已调研/开发中/测试中/已验收）与 status 并行展示；空 = 未标注
  const flow = DATA.hasStage && n.stage
    ? `<span class="stage" style="color:#C9A227;border:1px solid #C9A227">${n.stage}</span>`
    : '';
  return `<tr><td>#${n.id}</td><td>${n.title}</td>
    <td><span class="stage" style="color:${color};border:1px solid ${color}">${stage}</span>
        ${flow}</td>
    <td class="owner">${who}</td><td style="color:#B9A99B">${n.priority || ''}${lastBy}</td></tr>`;
}).join('');
app.innerHTML += `<h2>需求进度全景（${DATA.needs.length} 条）</h2>
<table><tr><th>#</th><th>需求</th><th>状态 / 流程段</th><th>处理人</th><th>优先级/最后操作</th></tr>${rows}</table>`;

// 谁在干什么
const todos = DATA.todos.map(t =>
  `<div class="todo"><span class="who">${t.owner}</span><span>${t.content}</span>
   <span style="color:#B9A99B;font-size:12px">${t.status || ''}</span></div>`).join('')
  || '<div style="color:#B9A99B">池内暂无个人 TODO</div>';
app.innerHTML += `<h2>谁在干什么（personal_todo 全池）</h2>${todos}`;
</script>
</body>
</html>"""
    html = html.replace("__GEN__", payload["generated"])
    html = html.replace("__PAYLOAD__", json.dumps(payload, ensure_ascii=False))
    html = html.replace("__STAGECOLORS__", json.dumps(
        {v[0]: v[1] for v in STAGE_VIEW.values()}, ensure_ascii=False))
    out_path.write_text(html, encoding="utf-8")
    print(f"pool-dashboard.html: 需求 {len(needs)} 条 / 个人 TODO {len(todos)} 条 → {out_path}"
          f"（审计列{'已接入' if has_audit else '未迁移，落地后自动接入'}）")


if __name__ == "__main__":
    main()
