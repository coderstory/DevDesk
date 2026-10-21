#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DevDesk .icns 生成器（Windows 兼容）——从 assets/icon.png 生成全尺寸 .icns。

与 macOS 链对等：
  pad_icon_canvas.py 补画布留白 → 缩放多档 PNG → 手工 ICNS 二进制拼装

产物：
  installer/macos/DevDesk.icns     App 图标（build-app.sh 优先取 prebuilt）
  installer/macos/.VolumeIcon.icns  DMG 卷图标

用法：
  python3 doc/scripts/gen_icns.py [仓库根目录]

依赖：Pillow
"""

from __future__ import annotations

import io
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:
    raise SystemExit(
        f"错误：未找到 Pillow（PIL）。\n安装：python3 -m pip install Pillow\n原始错误：{exc}"
    )

# ---------------------------------------------------------------------------
# 参数（与 doc/scripts/pad_icon_canvas.py 保持一致）
# ---------------------------------------------------------------------------
RATIO = 0.82          # 内容占画布比例（苹果原生约 0.82，Dock 里不显得大）
CANVAS = 1024         # 母图画布边长

# ICNS 条目： (类型码, PNG 像素尺寸)
# 说明：ic11 像素 = icp5（32×32，macOS 在 Retina 上用作 16pt@2x）；
#       ic12 = 64×64（32pt@2x）；ic13 同 ic08；ic14 同 ic09。
ICON_ENTRIES: list[tuple[bytes, int]] = [
    (b"icp4", 16),      # 16×16
    (b"icp5", 32),      # 32×32
    (b"ic11", 32),      # 32×32 （Retina 16pt@2x）
    (b"ic07", 128),     # 128×128
    (b"ic12", 64),      # 64×64 （Retina 32pt@2x）
    (b"ic08", 256),     # 256×256
    (b"ic13", 256),     # 256×256 （Retina 128pt@2x）
    (b"ic09", 512),     # 512×512
    (b"ic14", 512),     # 512×512 （Retina 256pt@2x）
    (b"ic10", 1024),    # 1024×1024
]


def pad_to_canvas(src: Image.Image, canvas: int, ratio: float) -> Image.Image:
    """pad_icon_canvas.py 同款算法：等比缩放 + 居中 + 透明方形画布。

    关键细节：
    - 内容最长边缩到 canvas * ratio，另一边等比——不拉伸、不裁切
    - 缩放前先转 RGBa（预乘 alpha）再缩放，防止透明像素 RGB 插值进边缘形成暗边
    """
    longest = max(src.size)
    content_target = max(1, round(canvas * ratio))
    scale = content_target / longest
    new_w = max(1, round(src.width * scale))
    new_h = max(1, round(src.height * scale))

    resized = src.convert("RGBa").resize((new_w, new_h), Image.LANCZOS).convert("RGBA")

    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    offset_x = (canvas - new_w) // 2
    offset_y = (canvas - new_h) // 2
    out.paste(resized, (offset_x, offset_y))
    return out


def png_bytes(img: Image.Image) -> bytes:
    """把 RGBA Image 编码为 PNG 字节。"""
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def build_icns(entries: list[tuple[bytes, bytes]]) -> bytes:
    """手工拼装 .icns 二进制（header + TOC + 各条目）。

    每项为 (type_code: 4 bytes, png_data: bytes)。
    会自动插入 TOC 条目（'TOC '）。
    """
    # 计算 TOC 大小：8 字节头 + 每项 8 字节（type 4 + size 4）
    toc_body = b""
    for type_code, png in entries:
        entry_size = 8 + len(png)  # type(4) + size(4) + data
        toc_body += type_code + struct.pack(">I", entry_size)
    toc_entry = b"TOC " + struct.pack(">I", 8 + len(toc_body)) + toc_body

    # 总大小：header(8) + TOC + 各条目
    total_size = 8 + len(toc_entry)
    for type_code, png in entries:
        total_size += 8 + len(png)

    buf = io.BytesIO()
    buf.write(b"icns")
    buf.write(struct.pack(">I", total_size))
    buf.write(toc_entry)
    for type_code, png in entries:
        buf.write(type_code)
        buf.write(struct.pack(">I", 8 + len(png)))
        buf.write(png)

    return buf.getvalue()


def gen_one(src_png: Path, out_icns: Path) -> dict:
    """从源 PNG 生成一个 .icns 文件，返回验证数据。"""
    src = Image.open(src_png).convert("RGBA")

    # 1) 母图：1024x1024 带留白
    master = pad_to_canvas(src, CANVAS, RATIO)

    # 2) 预渲染各 PNG 尺寸
    png_cache: dict[int, bytes] = {}
    needed_sizes = sorted(set(sz for _, sz in ICON_ENTRIES))
    for sz in needed_sizes:
        if sz == CANVAS:
            png_cache[sz] = png_bytes(master.copy())
        else:
            png_cache[sz] = png_bytes(master.resize((sz, sz), Image.LANCZOS))

    # 3) 拼装 .icns 二进制
    entries = [(code, png_cache[sz]) for code, sz in ICON_ENTRIES]
    data = build_icns(entries)

    # 4) 写盘
    out_icns.parent.mkdir(parents=True, exist_ok=True)
    out_icns.write_bytes(data)

    # 5) 回读验证：逐条解析 ICNS
    pos = 8  # skip header
    found_entries: dict[str, int] = {}
    while pos < len(data):
        type_code = data[pos : pos + 4].decode("ascii", errors="replace")
        entry_size = struct.unpack(">I", data[pos + 4 : pos + 8])[0]
        if type_code != "TOC ":
            found_entries[type_code] = entry_size
        pos += entry_size

    expected_codes = {code.decode() for code, _ in ICON_ENTRIES}
    missing_codes = expected_codes - set(found_entries)
    extra_codes = set(found_entries) - expected_codes - {"TOC "}

    result = {
        "name": out_icns.name,
        "path": str(out_icns),
        "bytes": len(data),
        "entries": found_entries,
        "missing": sorted(missing_codes),
        "extra": sorted(extra_codes),
    }
    return result


def main(argv: list[str]) -> int:
    repo = Path(argv[0]) if len(argv) > 0 else Path.cwd()
    src_png = repo / "assets" / "icon.png"

    if not src_png.is_file():
        print(f"[ERROR] 源图不存在: {src_png}", file=sys.stderr)
        return 2

    print(f"源图: {src_png}")

    targets = ["DevDesk.icns", ".VolumeIcon.icns"]
    ok = True

    for name in targets:
        out_path = repo / "installer" / "macos" / name
        before_size = out_path.stat().st_size if out_path.is_file() else 0

        r = gen_one(src_png, out_path)
        after_size = r["bytes"]

        # 回读校验
        if r["missing"]:
            print(f"[FAIL] {name}: 缺条目 {r['missing']}", file=sys.stderr)
            ok = False
        else:
            codes = sorted(r["entries"])
            sizes_str = ", ".join(
                f"{c}({r['entries'][c]/1024:.1f}K)" for c in codes
            )
            print(f"[OK] {name}: {len(r['entries'])} 条目 {sizes_str}")

        # 前后对照
        print(f"      {before_size:>10,} → {after_size:>10,} bytes")
        if r.get("extra"):
            print(f"      (额外条目: {r['extra']}，无害)")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))