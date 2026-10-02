#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把图标源图放进带留白的方形透明画布（对齐 macOS Dock 的观感）。

产物：<输出图> —— 边长 `--canvas`（默认 1024）的 RGBA PNG；
      内容按「最长边 = canvas × ratio」等比缩放后**居中**，四周为全透明留白。

【为什么需要它】2026-10-02 真机反馈：Dock 里 DevDesk 的图标比别的应用大。
根因不是尺寸参数，是**画布留白缺失** —— macOS Dock 用同一个小格子绘制所有应用
图标，苹果自家图标的内容只占格子约 80%、四周留白；源图内容满幅（alpha bbox 等于
整张画布）时，在同一个格子里自然显得大一圈。补留白即可，缩放参数一个都不用动。

【为什么不加圆角】DevDesk 现用的源图 `assets/icon.png` **本身已是圆角方形**：实测
外形是超椭圆（|x|^n+|y|^n=1，n≈3.4），左上角半径约 130/360 = 画布宽的 36%，四角
本来就是透明的。所以本脚本**只补留白**。再叠一层圆角蒙版只有两种结局——半径比源图
小就切掉源图已有的圆角边、比源图大则完全不起作用——都不如不做。

用法：
    python3 doc/scripts/pad_icon_canvas.py <源图> <输出图> [--ratio 0.82] [--canvas 1024]

依赖：Pillow（与 doc/scripts/gen_dmg_background.py 同一依赖）。
退出码：0 成功；2 依赖缺失 / 参数或输入非法（环境错，不静默绿）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError as exc:  # pragma: no cover - 只在缺 Pillow 的环境触发
    raise SystemExit(
        "错误：未找到 Pillow（PIL），无法给图标补画布留白。\n"
        "  安装：python3 -m pip install Pillow\n"
        "  或指定一个带 Pillow 的解释器：DEVDESK_PYTHON=<python> bash doc/scripts/gen_app_icns.sh\n"
        f"原始错误：{exc}"
    )

# 内容占画布的比例。0.82 ≈ 苹果自家 macOS 图标的内容占比（1024 画布里内容约 824），
# 是「Dock 里不再比别的应用大」的实测取值，不是精确复刻。
DEFAULT_RATIO = 0.82
DEFAULT_CANVAS = 1024


def pad_to_canvas(src_path: Path, out_path: Path, ratio: float, canvas: int) -> dict:
    """等比缩放并居中，返回自检数据（供调用方打印）。"""
    src = Image.open(src_path).convert("RGBA")

    # 最长边缩到 canvas×ratio，另一边按原比例——不拉伸、不裁切。
    longest = max(src.size)
    content_target = max(1, round(canvas * ratio))
    scale = content_target / longest
    new_size = (max(1, round(src.width * scale)), max(1, round(src.height * scale)))

    # 先转预乘 alpha（"RGBa"）再缩放，否则透明像素里的 RGB 会被插值进边缘、形成暗边。
    resized = src.convert("RGBa").resize(new_size, Image.LANCZOS).convert("RGBA")

    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    offset = ((canvas - new_size[0]) // 2, (canvas - new_size[1]) // 2)
    # 目标区域本来就是全透明，这里是整块 RGBA 覆写，不是 alpha 混合。
    out.paste(resized, offset)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out.save(out_path, "PNG")

    bbox = out.getchannel("A").getbbox()
    if bbox is None:
        raise SystemExit(f"[ERROR] 产物全透明（源图 {src_path} 没有任何不透明像素）")
    left, top, right, bottom = bbox
    margins = {
        "left": left,
        "top": top,
        "right": canvas - right,
        "bottom": canvas - bottom,
    }
    # 自检：内容必须真的缩进去了，四边都要有留白。留白没出现 = 这次修改没生效，
    # 与其把满幅图标装进 .app 不如直接报错。
    min_margin = round(canvas * 0.02)
    if min(margins.values()) < min_margin:
        raise SystemExit(
            f"[ERROR] 留白未生效：实测边距 {margins}（要求四边均 >= {min_margin}px）"
        )

    return {
        "src_size": src.size,
        "content_size": new_size,
        "offset": offset,
        "bbox": bbox,
        "margins": margins,
        "bytes": out_path.stat().st_size,
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="把图标源图等比缩放后居中放进带透明留白的方形画布",
    )
    ap.add_argument("src", type=Path, help="源 PNG")
    ap.add_argument("out", type=Path, help="输出的 1024×1024 RGBA PNG")
    ap.add_argument(
        "--ratio",
        type=float,
        default=DEFAULT_RATIO,
        help=f"内容占画布的比例（默认 {DEFAULT_RATIO}）",
    )
    ap.add_argument(
        "--canvas", type=int, default=DEFAULT_CANVAS, help=f"画布边长（默认 {DEFAULT_CANVAS}）"
    )
    args = ap.parse_args(argv)

    if not args.src.is_file():
        print(f"[ERROR] 源图不存在: {args.src}", file=sys.stderr)
        return 2
    if not 0.1 <= args.ratio <= 1.0:
        print(f"[ERROR] --ratio 必须在 0.1~1.0 之间，收到 {args.ratio}", file=sys.stderr)
        return 2
    if args.canvas < 16:
        print(f"[ERROR] --canvas 太小: {args.canvas}", file=sys.stderr)
        return 2

    info = pad_to_canvas(args.src, args.out, args.ratio, args.canvas)
    m = info["margins"]
    print(
        f"[OK] 画布留白: {info['src_size'][0]}×{info['src_size'][1]} "
        f"→ 内容 {info['content_size'][0]}×{info['content_size'][1]} "
        f"居中于 {args.canvas}×{args.canvas}（alpha bbox={info['bbox']}，"
        f"边距 左{m['left']} 上{m['top']} 右{m['right']} 下{m['bottom']}）"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
