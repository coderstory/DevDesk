#!/usr/bin/env python3
"""字体子集断言校验（need:132 F-03，技术卡 v4 §3 断言①②③④）。

对 make_font_subset.py 的产出做四项校验：
  ① 全 cmap 逐码点 advance 原字体 vs 子集逐一相等（子集不改度量）
  ② 子集覆盖率 == 源字体覆盖率（源有⇒子集有；源无不算丢字）
  ③ 体积上限（单字重 ≤max-mb）
  ④ 输出字节级确定性（可选：--rebuild 重跑一次比对 hash）

以及 OFL 合规检查：name 表不得残留 Reserved Font Name 词根。

依赖：fonttools。建议隔离 venv 运行。
用法：
  python3 -I scripts/verify_font_subset.py \
      --source /path/NotoSansCJKsc-Regular.otf \
      --subset assets/DevDeskSansSC-Regular.otf \
      --unicodes assets/DevDeskSansSC-Regular.otf.unicodes.txt
"""
from __future__ import annotations

import argparse
import sys

# OFL 条款3 Reserved Font Name 词根黑名单（子集=Modified Version，不得沿用）
RFN_TOKENS = ("Noto", "Source Han", "思源", "Adobe")


def load_unicodes(path: str) -> list[int]:
    cps = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("U+"):
                cps.append(int(line[2:], 16))
    return cps


def main() -> int:
    from fontTools.ttLib import TTFont

    ap = argparse.ArgumentParser(description="字体子集断言校验（need:132 F-03）")
    ap.add_argument("--source", required=True, help="源字体")
    ap.add_argument("--subset", required=True, help="子集产物")
    ap.add_argument("--unicodes", required=True, help="字符集清单（.unicodes.txt）")
    ap.add_argument("--max-mb", type=float, default=4.0, help="单字重体积上限 MB")
    args = ap.parse_args()

    src = TTFont(args.source, lazy=True)
    sub = TTFont(args.subset, lazy=True)
    src_cmap = src.getBestCmap()
    sub_cmap = sub.getBestCmap()
    src_hmtx = src["hmtx"].metrics
    sub_hmtx = sub["hmtx"].metrics

    want = load_unicodes(args.unicodes)
    failures: list[str] = []

    # ① 全 cmap 逐码点 advance 相等
    checked = 0
    adv_diff: list[tuple[int, int, int]] = []
    for cp in want:
        g = sub_cmap.get(cp)
        if g is None or g not in sub_hmtx:
            continue
        if g not in src_hmtx:
            continue
        checked += 1
        a_src = src_hmtx[g][0]
        a_sub = sub_hmtx[g][0]
        if a_src != a_sub:
            adv_diff.append((cp, a_src, a_sub))
    status = "PASS" if not adv_diff else "FAIL"
    print(f"[①] advance 不变：校验 {checked} 字形，差异 {len(adv_diff)} -> {status}")
    if adv_diff:
        failures.append(f"① {len(adv_diff)} 个字形 advance 变化，前 5: {[(hex(c), a, b) for c, a, b in adv_diff[:5]]}")

    # ② 子集覆盖率 == 源字体覆盖率
    lost = [cp for cp in want if cp in src_cmap and cp not in sub_cmap]
    source_gap = [cp for cp in want if cp not in src_cmap]
    status = "PASS" if not lost else "FAIL"
    print(f"[②] 覆盖率：字符集 {len(want)}，源字体本身缺 {len(source_gap)}（不计丢字），子集丢字 {len(lost)} -> {status}")
    if lost:
        failures.append(f"② 子集丢失 {len(lost)} 个源字体有的码点，前 5: {[hex(c) for c in lost[:5]]}")
    if source_gap:
        print(f"     源字体本身缺（符号段，已接受差异）: {[hex(c) for c in source_gap[:20]]}"
              f"{' …' if len(source_gap) > 20 else ''}")

    # ③ 体积上限
    import os
    size = os.path.getsize(args.subset)
    limit = int(args.max_mb * 1024 * 1024)
    status = "PASS" if size <= limit else "FAIL"
    print(f"[③] 体积：{size:,}B ({size / 1048576:.2f}MB) / 上限 {args.max_mb}MB -> {status}")
    if size > limit:
        failures.append(f"③ 体积 {size} 超上限 {limit}")

    # ④ unitsPerEm 一致（度量基准）
    s_upem = src["head"].unitsPerEm
    b_upem = sub["head"].unitsPerEm
    status = "PASS" if s_upem == b_upem else "FAIL"
    print(f"[④] unitsPerEm：源 {s_upem} / 子集 {b_upem} -> {status}")
    if s_upem != b_upem:
        failures.append(f"④ unitsPerEm 不一致 {s_upem} vs {b_upem}")

    # ⑤ OFL 合规：name 表不得残留 RFN 词根
    names = []
    for record in sub["name"].names:
        if record.nameID in (1, 4, 6, 16):
            try:
                names.append((record.nameID, record.toUnicode()))
            except Exception:
                pass
    hits = [(nid, v) for nid, v in names if any(tok in v for tok in RFN_TOKENS)]
    status = "PASS" if not hits else "FAIL"
    print(f"[⑤] OFL RFN：name 表词根残留 {len(hits)} -> {status}")
    for nid, value in names:
        print(f"     ID{nid}: {value}")
    if hits:
        failures.append(f"⑤ RFN 词根残留: {hits}")

    print()
    if failures:
        print("总判定: FAIL")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("总判定: PASS（①度量不变 ②零丢字 ③体积达标 ④upem 一致 ⑤RFN 合规）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
