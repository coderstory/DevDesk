#!/usr/bin/env python3
"""字体子集化管线（need:132 F-03）。

从源字体生成随包分发的子集字体：GB2312 全集 + 源码非 ASCII 码点 + ASCII +
常用全角符号段，pyftsubset 裁剪，fontTools 改名避开 OFL Reserved Font Name。

依赖：fonttools（pip install fonttools brotli）。建议隔离 venv 运行，不装进系统 Python。

用法：
  python3 -I scripts/make_font_subset.py \
      --source /path/NotoSansCJKsc-Regular.otf \
      --output assets/DevDeskSansSC-Regular.otf \
      --family "DevDesk Sans SC" \
      --src-root src

产出：子集字体 + 同名 .unicodes.txt（字符集清单，供 verify 与审计复核）。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

# 常用全角标点与符号补充段（CJK 标点 + 全角 ASCII 变体）
EXTRA_RANGES = [(0x3000, 0x3040), (0xFF00, 0xFF60)]


def gb2312_codepoints() -> tuple[list[int], int]:
    """GB2312 双字节枚举。返回 (码点列表, 未指派跳过数)。"""
    cps: list[int] = []
    skipped = 0
    for b1 in range(0xB0, 0xF8):
        for b2 in range(0xA1, 0xFF):
            try:
                cps.append(ord(bytes([b1, b2]).decode("gb2312")))
            except UnicodeDecodeError:
                skipped += 1
    return cps, skipped


def source_codepoints(src_root: str) -> list[int]:
    """扫源码文件，收集全部非 ASCII 码点（粗粒度但零漏：不解析字面量边界）。"""
    cps: set[int] = set()
    for dirpath, _, names in os.walk(src_root):
        for name in names:
            if not name.endswith((".cpp", ".h", ".mm")):
                continue
            try:
                text = open(os.path.join(dirpath, name), encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for ch in text:
                if ord(ch) > 0x7F:
                    cps.add(ord(ch))
    return sorted(cps)


def build_charset(src_root: str) -> tuple[list[int], int, int]:
    gb, skipped = gb2312_codepoints()
    lit = source_codepoints(src_root)
    cps = set(gb) | set(lit) | set(range(0x20, 0x7F))
    for lo, hi in EXTRA_RANGES:
        cps |= set(range(lo, hi))
    return sorted(cps), len(lit), skipped


def rename_font(path: str, family: str, style: str) -> None:
    """改 name 表 ID 1/3/4/6/16（OFL 条款3：Modified Version 不得用 Reserved Font Name）。"""
    from fontTools.ttLib import TTFont

    font = TTFont(path)
    new_values = {
        1: family,
        3: f"{family}:{style}",
        4: f"{family} {style}",
        6: family.replace(" ", "") + "-" + style,
        16: family,
    }
    for record in font["name"].names:
        if record.nameID in new_values:
            record.string = new_values[record.nameID].encode(record.getEncoding())
    font.save(path)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="字体子集化管线（need:132 F-03）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--source", required=True, help="源字体文件（静态 OTF/TTF）")
    ap.add_argument("--output", required=True, help="子集输出路径")
    ap.add_argument("--family", default="DevDesk Sans SC", help="子集后 family 名（避开 RFN）")
    ap.add_argument("--style", default="Regular", help="字重名")
    ap.add_argument("--src-root", default="src", help="源码根目录（扫描码点）")
    ap.add_argument("--max-mb", type=float, default=4.0, help="体积上限 MB，超出报错")
    ap.add_argument("--no-rename", action="store_true", help="跳过改名（仅调试）")
    args = ap.parse_args()

    if not os.path.isfile(args.source):
        print(f"❌ 源字体不存在：{args.source}", file=sys.stderr)
        return 2

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    cps, lit_count, skipped = build_charset(args.src_root)
    print(f"[1] 字符集：GB2312 全集 + 源码非 ASCII {lit_count} + ASCII + 全角符号段")
    print(f"    合并去重后 {len(cps)} 码点（GB2312 未指派跳过 {skipped} 个）")

    uni_path = args.output + ".unicodes.txt"
    with open(uni_path, "w", encoding="utf-8") as f:
        for cp in cps:
            f.write(f"U+{cp:04X}\n")
    print(f"[2] 字符集清单 → {uni_path}")

    pyftsubset = shutil.which("pyftsubset")
    if pyftsubset is not None:
        cmd = [pyftsubset]
    else:
        # 回退：用当前解释器的 fontTools 模块入口（venv 里 pyftsubset 未必在 PATH）
        probe = subprocess.run(
            [sys.executable, "-c", "import fontTools.subset"],
            capture_output=True,
            text=True,
        )
        if probe.returncode != 0:
            print("❌ 找不到 pyftsubset 且当前解释器无 fontTools（pip install fonttools）", file=sys.stderr)
            return 2
        cmd = [sys.executable, "-m", "fontTools.subset"]

    print("[3] 子集化…")
    result = subprocess.run(
        cmd
        + [
            args.source,
            f"--unicodes-file={uni_path}",
            f"--output-file={args.output}",
            "--layout-features=*",
            "--drop-tables+=DSIG",
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print("❌ pyftsubset 失败：", file=sys.stderr)
        print(result.stdout[-3000:], file=sys.stderr)
        print(result.stderr[-3000:], file=sys.stderr)
        return 1

    if not args.no_rename:
        print(f"[4] 改名 family={args.family}（OFL 条款3：Modified Version 不得用 RFN）")
        rename_font(args.output, args.family, args.style)

    src_size = os.path.getsize(args.source)
    out_size = os.path.getsize(args.output)
    print(
        f"[5] 体积 {src_size:,}B → {out_size:,}B "
        f"({out_size / 1048576:.2f}MB，压缩至 {out_size / src_size * 100:.1f}%)"
    )
    if out_size > int(args.max_mb * 1024 * 1024):
        print(f"❌ 超出体积上限 {args.max_mb}MB", file=sys.stderr)
        return 1
    print(f"[6] 产出：{args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
