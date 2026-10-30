#!/usr/bin/env python3
"""pool_review_gen.py — 从 pool-snapshot.json 生成挂起需求复选框评审页。

用法：python3 tools/pool_review_gen.py [snapshot 路径] [输出 HTML 路径]
缺省：docs/pool/pool-snapshot.json → docs/pool/pool-review.html

取数纪律：数据只来自池库快照（不手抄）；页面为自包含静态 HTML
（条目 JSON 内嵌），浏览器 file:// 直开可用，无 CORS 问题。
每次池库变化后重跑本脚本重生成。
"""
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SNAPSHOT = REPO / "docs/pool/pool-snapshot.json"
OUT = REPO / "docs/pool/pool-review.html"

PRIORITY_ORDER = {"P0": 0, "P1": 1, "P2": 2, "P3": 3}


def main() -> None:
    snap_path = Path(sys.argv[1]) if len(sys.argv) > 1 else SNAPSHOT
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else OUT
    needs = json.loads(snap_path.read_text(encoding="utf-8"))["needs"]
    suspended = sorted(
        (n for n in needs if n.get("status") == "挂起"),
        key=lambda n: (PRIORITY_ORDER.get(n.get("priority", "P3"), 9), n["id"]),
    )
    payload = [
        {
            "id": n["id"],
            "pool": n.get("pool", ""),
            "priority": n.get("priority", ""),
            "title": n["title"],
            "blocker": n.get("blocker") or "",
            "body": (n.get("body") or "")[:200],
            "updated": n.get("updated", ""),
        }
        for n in suspended
    ]

    html = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>挂起需求评审 — 勾选「不做」</title>
<style>
  body { font-family: -apple-system, "PingFang SC", sans-serif; margin: 0;
         background: #241C15; color: #F5EDE4; }
  header { padding: 24px 32px 8px; }
  h1 { font-size: 22px; margin: 0 0 4px; color: #D97757; }
  .meta { color: #B9A99B; font-size: 13px; }
  main { padding: 8px 32px 96px; }
  .item { display: flex; gap: 12px; align-items: flex-start;
          background: rgba(245,237,228,.06); border: 1px solid rgba(217,119,87,.25);
          border-radius: 10px; padding: 12px 16px; margin: 10px 0; }
  .item input { width: 18px; height: 18px; margin-top: 3px; accent-color: #D97757; }
  .item.checked { background: rgba(217,119,87,.18); }
  .id { color: #D97757; font-weight: 700; min-width: 52px; }
  .prio { border: 1px solid #B9A99B; border-radius: 999px; padding: 1px 8px;
          font-size: 12px; color: #B9A99B; min-width: 30px; text-align: center; }
  .title { font-size: 15px; }
  .blocker { color: #B9A99B; font-size: 12px; margin-top: 2px; }
  footer { position: fixed; left: 0; right: 0; bottom: 0;
           background: #1A1410; border-top: 1px solid rgba(217,119,87,.35);
           padding: 12px 32px; display: flex; align-items: center; gap: 16px; }
  button { background: #D97757; color: #241C15; border: 0; border-radius: 8px;
           padding: 10px 24px; font-size: 15px; font-weight: 700; cursor: pointer; }
  #summary { flex: 1; color: #F5EDE4; font-size: 14px; white-space: pre-wrap; }
</style>
</head>
<body>
<header>
  <h1>挂起需求评审 — 勾选 = 不做</h1>
  <div class="meta">数据源 docs/pool/pool-snapshot.json · __COUNT__ 条挂起 · 生成时间 __GEN__ ·
  勾完点底部按钮，把汇总整段复制发回群里</div>
</header>
<main><div id="list"></div></main>
<footer>
  <button onclick="summary()">汇总勾选项</button>
  <div id="summary">（未勾选）</div>
</footer>
<script>
const NEEDS = __PAYLOAD__;
const list = document.getElementById('list');
for (const n of NEEDS) {
  const row = document.createElement('label');
  row.className = 'item';
  row.innerHTML = `<input type="checkbox" data-id="${n.id}">
    <span class="id">#${n.id}</span><span class="prio">${n.priority}</span>
    <span><div class="title">${n.title}</div>
    <div class="blocker">卡点：${n.blocker || '—'}${n.body ? ' · ' + n.body : ''}</div></span>`;
  const box = row.querySelector('input');
  const sync = () => row.classList.toggle('checked', box.checked);
  box.addEventListener('change', sync);
  list.appendChild(row);
}
function summary() {
  const picked = [...document.querySelectorAll('input:checked')]
    .map(b => NEEDS.find(n => String(n.id) === b.dataset.id))
    .filter(Boolean);
  const text = picked.length
    ? '以下挂起需求全部不做（' + picked.length + ' 条）：\\n' +
      picked.map(n => `#${n.id} ${n.title}`).join('\\n')
    : '（未勾选）';
  document.getElementById('summary').textContent = text;
}
</script>
</body>
</html>"""
    import datetime
    html = html.replace("__COUNT__", str(len(payload)))
    html = html.replace("__GEN__", datetime.datetime.now().strftime("%Y-%m-%d %H:%M"))
    html = html.replace("__PAYLOAD__", json.dumps(payload, ensure_ascii=False))
    out_path.write_text(html, encoding="utf-8")
    print(f"pool-review.html: {len(payload)} 条挂起 → {out_path}")


if __name__ == "__main__":
    main()
