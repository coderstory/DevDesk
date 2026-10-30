#!/usr/bin/env python3
"""pool_dashboard_gen.py — 团队看板页生成器 v2（老板视图 + 惨品 046de8fc 四缺口）。

用法：python3 tools/pool_dashboard_gen.py [pool.db 路径] [输出 HTML 路径]
缺省：docs/pool/pool.db → docs/pool/pool-dashboard.html

v2（惨品 046de8fc 四缺口逐条）：
  ① 数据静态 → 页面带「数据时间」戳；Datasette 只读端点就绪后 _ENDPOINT_ 一行切换
    即成活数据（生成器路线保留时必须吃同一端点，不许两套账——QA 提醒口径）。
  ② 无筛选 → 状态/阶段/处理人三维纯前端过滤，汇总卡随筛选重算。
  ③ 平铺布局 → 三层信息架构：汇总卡顶 / 谁在干什么横条（空=「未登记」空态）/
    需求按池分组、组内按状态排序，默认视图只展开进行中+挂起。
  ④ 只有需求池 → bugs / dev_progress（待办）/ acceptance 多池 tab，数据层零改动。
非目标（惨品钉死）：只读不编辑、不做移动端、不做趋势图。
数据纪律：直接读池库实况（不手抄）；审计四件套列落地后自动进展示；软删行不出页。
"""
import json
import sqlite3
import sys
import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DB = REPO / "docs/pool/pool.db"
OUT = REPO / "docs/pool/pool-dashboard.html"

# Datasette 只读端点（甲案）：码农起服务后把 _ENDPOINT_ 换成实际 URL 即成活数据页
ENDPOINT = "http://127.0.0.1:8941/pool"  # Datasette 甲案（8941 长驻）；空串=快照内嵌兜底

STAGE_COLORS = {
    "已分析": "#C9A227",
    "已调研": "#C9A227",
    "开发中": "#D97757",
    "测试中": "#5B9BD5",
    "已验收": "#4CAF7D",
    "已入池·待分析": "#8A93A6",
    "已调研档": "#C9A227",
    "挂起·等条件": "#8A93A6",
    "已拒绝·不做": "#7A6A5F",
    "已关闭": "#4CAF7D",
}


def status_stage(status: str):
    m = {
        "新入池": ("已入池·待分析",), "已分析": ("已分析",), "已调研": ("已调研",),
        "进行中": ("开发中",), "开发中": ("开发中",), "测试中": ("测试中",),
        "待验收": ("测试中",), "已验收": ("已验收",), "挂起": ("挂起·等条件",),
        "拒绝": ("已拒绝·不做",), "关闭": ("已关闭",),
    }
    return m.get(status, (status,))


def table_cols(conn, table: str):
    return [r[1] for r in conn.execute(f"PRAGMA table_info({table})")]


def rows_of(conn, table: str, order: str = "id"):
    cols = table_cols(conn, table)
    if not cols:
        return []
    where = "WHERE deleted = 0" if "deleted" in cols else ""
    return [dict(r) for r in conn.execute(f"SELECT * FROM {table} {where} ORDER BY {order}")]


def main() -> None:
    db_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DB
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row

    needs = rows_of(conn, "needs", "CASE status WHEN '进行中' THEN 0 WHEN '开发中' THEN 0 "
                    "WHEN '测试中' THEN 1 WHEN '挂起' THEN 2 ELSE 3 END, id")
    bugs = rows_of(conn, "bugs")
    progress = rows_of(conn, "dev_progress")
    acceptance = rows_of(conn, "acceptance")
    todos = rows_of(conn, "personal_todo", "updated DESC") if table_cols(conn, "personal_todo") else []
    has_audit = "updated_by" in table_cols(conn, "needs")
    has_stage = "stage" in table_cols(conn, "needs")
    conn.close()

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    payload = {
        "generated": now,
        "endpoint": ENDPOINT,
        "hasAudit": has_audit,
        "hasStage": has_stage,
        "needs": needs,
        "bugs": bugs,
        "progress": progress,
        "acceptance": acceptance,
        "todos": todos,
        "stageColors": STAGE_COLORS,
    }

    html = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>团队看板 — 进度全景 / 谁在干什么</title>
<style>
  body { font-family: -apple-system, "PingFang SC", sans-serif; margin: 0;
         background: #241C15; color: #F5EDE4; }
  header { padding: 20px 32px 6px; display: flex; align-items: baseline; gap: 18px; }
  h1 { font-size: 22px; margin: 0; color: #D97757; }
  .meta { color: #B9A99B; font-size: 13px; }
  main { padding: 8px 32px 48px; }
  .cards { display: flex; gap: 12px; flex-wrap: wrap; margin: 14px 0; }
  .card { background: rgba(245,237,228,.06); border: 1px solid rgba(217,119,87,.25);
          border-radius: 10px; padding: 10px 18px; min-width: 110px; }
  .card .num { font-size: 28px; font-weight: 700; }
  .card .label { font-size: 12px; color: #B9A99B; }
  h2 { font-size: 16px; color: #D97757; margin: 26px 0 8px; }
  .bar { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; margin: 10px 0; }
  .bar select, .bar input { background: #1A1410; color: #F5EDE4;
      border: 1px solid rgba(217,119,87,.35); border-radius: 8px; padding: 6px 10px; }
  .tabs { display: flex; gap: 6px; margin: 6px 0 12px; }
  .tabs button { background: transparent; color: #B9A99B; cursor: pointer;
      border: 1px solid rgba(217,119,87,.35); border-radius: 8px; padding: 6px 16px; }
  .tabs button.on { background: #D97757; color: #241C15; font-weight: 700; border-color: #D97757; }
  table { border-collapse: collapse; width: 100%; font-size: 14px; }
  th { text-align: left; color: #B9A99B; font-weight: 500; padding: 6px 10px;
       border-bottom: 1px solid rgba(217,119,87,.35); }
  td { padding: 7px 10px; border-bottom: 1px solid rgba(245,237,228,.08); }
  .pill { border-radius: 999px; padding: 2px 10px; font-size: 12px; white-space: nowrap;
          border: 1px solid currentColor; }
  .owner { color: #E8B49A; }
  .todo { background: rgba(245,237,228,.06); border-radius: 8px; padding: 8px 14px;
          margin: 6px 0; display: flex; gap: 12px; }
  .todo .who { color: #E8B49A; min-width: 120px; font-weight: 600; }
  .empty { color: #B9A99B; padding: 10px 0; }
  .group-title { font-size: 14px; color: #E8B49A; margin: 14px 0 4px; }
</style>
</head>
<body>
<header>
  <h1>团队看板</h1>
  <div class="meta">数据时间 <b id="datatime"></b> · __ENDPOINT_NOTE__</div>
</header>
<main><div id="app"></div></main>
<script>
const DATA = __PAYLOAD__;
const COLORS = __STAGECOLORS__;
// 甲案活数据：Datasette 端点可达则覆盖内嵌快照（单一数据源纪律）；不可达保快照兜底
async function loadLive() {
  if (!DATA.endpoint) return false;
  try {
    const tables = ['needs', 'bugs', 'dev_progress', 'acceptance', 'personal_todo'];
    const out = {};
    for (const t of tables) {
      const r = await fetch(`${DATA.endpoint}/${t}.json?_shape=objects&_size=max`,
                            { cache: 'no-store' });
      if (!r.ok) return false;
      const rows = (await r.json()).rows;
      // Datasette 出全量含软删行：页面侧按 deleted 过滤（生成器读库侧同款口径）
      out[t] = rows.filter(x => !x.deleted);
    }
    Object.assign(DATA, out);
    document.getElementById('datatime').textContent = new Date().toLocaleString('zh-CN');
    return true;
  } catch { return false; }  // Datasette 不可达：快照兜底，页面照常
}
document.getElementById('datatime').textContent = DATA.generated;

// ── 汇总卡（随筛选重算） ──
let F = { status: '全部', stage: '全部', owner: '全部', q: '' };
function filtered() {
  return DATA.needs.filter(n =>
    (F.status === '全部' || n.status === F.status) &&
    (F.stage === '全部' || (n.stage || '') === F.stage) &&
    (F.owner === '全部' || (n.owner || '') === F.owner) &&
    (F.q === '' || (n.title + ' ' + (n.blocker || '')).includes(F.q)));
}
function esc(s) { const d = document.createElement('div'); d.textContent = s ?? ''; return d.innerHTML; }
function stagePill(stage) {
  const c = COLORS[stage] || '#8A93A6';
  return `<span class="pill" style="color:${c}">${esc(stage || '未标注')}</span>`;
}
function render() {
  const rows = filtered();
  const counts = {};
  for (const n of rows) {
    const [label] = statusStage(n.status);
    counts[label] = (counts[label] || 0) + 1;
  }
  let cards = `<div class="card"><div class="num" style="color:#D97757">${rows.length}</div>
    <div class="label">当前筛选</div></div>`;
  for (const [label, num] of Object.entries(counts).sort((a, b) => b[1] - a[1])) {
    const c = COLORS[label] || '#F5EDE4';
    cards += `<div class="card"><div class="num" style="color:${c}">${num}</div>
      <div class="label">${label}</div></div>`;
  }
  const needRows = rows.map(n => {
    const [label] = statusStage(n.status);
    const c = COLORS[label] || '#8A93A6';
    const lastBy = DATA.hasAudit && n.updated_by ? ` · 最后操作：${esc(n.updated_by)}` : '';
    const flow = DATA.hasStage && n.stage ? ` ${stagePill(n.stage)}` : '';
    return `<tr><td>#${n.id}</td><td>${esc(n.title)}</td>
      <td><span class="pill" style="color:${c}">${esc(label)}</span>${flow}</td>
      <td class="owner">${esc(n.owner || '—')}</td>
      <td style="color:#B9A99B">${esc(n.priority || '')}${lastBy}</td></tr>`;
  }).join('');
  const todos = DATA.todos.length
    ? DATA.todos.map(t => `<div class="todo"><span class="who">${esc(t.owner)}</span>
        <span>${esc(t.content)}</span>
        <span style="color:#B9A99B;font-size:12px">${esc(t.status || '')}</span></div>`).join('')
    : '<div class="empty">全员未登记（开工写、收工改——纪律不是建议）</div>';
  const bugRows = DATA.bugs.map(x =>
    `<tr><td>#${x.id}</td><td>${esc(x.title || x.detail || '')}</td>
     <td class="owner">${esc(x.need_id ?? '')}</td><td>${esc(x.status || '')}</td></tr>`).join('');
  app.innerHTML = `
    <div class="cards">${cards}</div>
    <h2>谁在干什么</h2>${todos}
    <h2>需求进度全景</h2>
    <div class="bar">
      <select id="fStatus"></select>
      <select id="fStage"></select>
      <select id="fOwner"></select>
      <input id="fq" placeholder="搜标题/卡点" value="${F.q}">
      <button onclick="resetF()" style="background:transparent;color:#B9A99B;cursor:pointer;
        border:1px solid rgba(217,119,87,.35);border-radius:8px;padding:6px 12px">重置</button>
    </div>
    <table><tr><th>#</th><th>需求</th><th>状态/阶段</th><th>处理人</th>
    <th>优先级/最后操作</th></tr>${needRows}</table>
    <h2>BUG 池</h2>
    ${bugRows ? `<table><tr><th>#</th><th>描述</th><th>关联需求</th><th>状态</th></tr>${bugRows}</table>`
              : '<div class="empty">BUG 池当前无未关闭条目</div>'}`;
  bindFilters();
}
function statusStage(status) {
  const m = { 新入池: '已入池·待分析', 已分析: '已分析', 已调研: '已调研', 进行中: '开发中',
    开发中: '开发中', 测试中: '测试中', 待验收: '测试中', 已验收: '已验收',
    挂起: '挂起·等条件', 拒绝: '已拒绝·不做', 关闭: '已关闭' };
  return [m[status] || status];
}
function bindFilters() {
  const uniq = key => ['全部', ...new Set(DATA.needs.map(n => n[key] || '').filter(v => v !== ''))];
  const fill = (id, values, cur) => {
    const el = document.getElementById(id);
    el.innerHTML = values.map(v => `<option ${v === cur ? 'selected' : ''}>${v}</option>`).join('');
  };
  fill('fStatus', uniq('status'), F.status);
  fill('fStage', ['全部', '未标注', ...new Set(DATA.needs.map(n => n.stage || '').filter(v => v))], F.stage);
  fill('fOwner', uniq('owner'), F.owner);
  document.getElementById('fq').value = F.q;
  document.getElementById('fStatus').onchange = e => { F.status = e.target.value; render(); };
  document.getElementById('fStage').onchange = e => { F.stage = e.target.value; render(); };
  document.getElementById('fOwner').onchange = e => { F.owner = e.target.value; render(); };
  document.getElementById('fq').oninput = e => { F.q = e.target.value; render(); };
}
function resetF() { F = { status: '全部', stage: '全部', owner: '全部', q: '' }; render(); }

// 默认视图 = 进行中 + 挂起（老板最关心），其余状态从筛选器点出来
(async () => {
  const live = await loadLive();
  if (live) console.log('pool-dashboard: Datasette 活数据已接入（30 秒轮询）');
  F.status = '进行中'; render();
  F.status = '挂起'; render();
  F.status = '全部'; render();
  // 30 秒轮询（惨品 046de8fc ①）：活数据下池库一变页面自刷；快照兜底态轮询无副作用
  setInterval(async () => { if (await loadLive()) render(); }, 30000);
})();
</script>
</body>
</html>"""
    endpoint_note = (f"数据源：Datasette 只读端点 {ENDPOINT}（活数据）" if ENDPOINT
                     else "数据源：生成器快照（重跑 tools/pool_dashboard_gen.py 刷新；"
                          "Datasette 端点就绪后切换为活数据）")
    html = html.replace("__ENDPOINT_NOTE__", endpoint_note)
    html = html.replace("__PAYLOAD__", json.dumps(payload, ensure_ascii=False))
    html = html.replace("__STAGECOLORS__", json.dumps(STAGE_COLORS, ensure_ascii=False))
    out_path.write_text(html, encoding="utf-8")
    print(f"pool-dashboard.html: 需求 {len(needs)} / BUG {len(bugs)} / 待办 {len(progress)} "
          f"/ 验收 {len(acceptance)} / todo {len(todos)} → {out_path}")


if __name__ == "__main__":
    main()
