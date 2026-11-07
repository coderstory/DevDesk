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
    python3 doc/scripts/pad_icon_canvas.py <源图> <输出图> [--target-px 824] [--canvas 1024]
    # 旧模式（按 PNG 边缘比例）保留可用：--ratio 0.82（与 --target-px 互斥，--ratio 优先）

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
# 2026-11-07 bug#158：默认改为按 Apple HIG 图标网格**精确对齐**——内容（alpha bbox）
# 最长边 = 824px / 1024 画布（HIG 规定内容圆角矩形 ≈ 824/1024 ≈ 80.5%）。
# 按 bbox 而非 PNG 边缘缩放：源图自带透明边时（如 EUI-NEO icon.png 360×351、
# 素材只占其中 ~96%），按边缘算 ratio 会把留白算两次（实测 82% → 实际只有 79%）。
DEFAULT_TARGET_PX = 824


def pad_to_canvas(src_path: Path, out_path: Path, ratio: float | None, canvas: int,
                  target_px: int | None = None) -> dict:
    """等比缩放并居中，返回自检数据（供调用方打印）。

    target_px 给定时（默认 824）：按源图 alpha bbox 最长边缩放到恰好 target_px，
    并按 bbox 中心（而非 PNG 中心）居中——内容在画布里的占比精确等于
    target_px/canvas，与源图自带多少透明边无关。
    ratio 给定时：走旧模式（PNG 最长边 = canvas×ratio，按图片中心居中）。
    """
    src = Image.open(src_path).convert("RGBA")

    if target_px is not None:
        # 精确模式：以内容 alpha bbox 为准缩放与居中。
        src_bbox = src.getchannel("A").getbbox()
        if src_bbox is None:
            raise SystemExit(f"[ERROR] 源图全透明（无任何不透明像素）: {src_path}")
        bbox_longest = max(src_bbox[2] - src_bbox[0], src_bbox[3] - src_bbox[1])
        scale = target_px / bbox_longest
        new_size = (max(1, round(src.width * scale)), max(1, round(src.height * scale)))
        content_center = ((src_bbox[0] + src_bbox[2]) * scale / 2,
                          (src_bbox[1] + src_bbox[3]) * scale / 2)
        offset = (round(canvas / 2 - content_center[0]), round(canvas / 2 - content_center[1]))
    else:
        # 旧 ratio 模式：PNG 最长边缩到 canvas×ratio，按图片中心居中。
        longest = max(src.size)
        content_target = max(1, round(canvas * ratio))
        scale = content_target / longest
        new_size = (max(1, round(src.width * scale)), max(1, round(src.height * scale)))
        offset = ((canvas - new_size[0]) // 2, (canvas - new_size[1]) // 2)

    # 先转预乘 alpha（"RGBa"）再缩放，否则透明像素里的 RGB 会被插值进边缘、形成暗边。
    resized = src.convert("RGBa").resize(new_size, Image.LANCZOS).convert("RGBA")

    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
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
        "target_px": target_px,
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
        default=None,
        help=f"旧模式：PNG 最长边占画布比例（默认 {DEFAULT_RATIO}）；"
             f"与 --target-px 二选一，两者都不给时按 --target-px 默认值走",
    )
    ap.add_argument(
        "--target-px",
        type=int,
        default=DEFAULT_TARGET_PX,
        help=f"精确模式：内容 alpha bbox 最长边缩放到该像素数（默认 {DEFAULT_TARGET_PX}，"
             f"= HIG 网格 824/1024）；传 0 关闭精确模式改走 --ratio",
    )
    ap.add_argument(
        "--canvas", type=int, default=DEFAULT_CANVAS, help=f"画布边长（默认 {DEFAULT_CANVAS}）"
    )
    args = ap.parse_args(argv)

    if not args.src.is_file():
        print(f"[ERROR] 源图不存在: {args.src}", file=sys.stderr)
        return 2
    if args.ratio is not None and not 0.1 <= args.ratio <= 1.0:
        print(f"[ERROR] --ratio 必须在 0.1~1.0 之间，收到 {args.ratio}", file=sys.stderr)
        return 2
    if args.canvas < 16:
        print(f"[ERROR] --canvas 太小: {args.canvas}", file=sys.stderr)
        return 2

    target_px = args.target_px if args.target_px and args.ratio is None else None
    info = pad_to_canvas(args.src, args.out, args.ratio, args.canvas, target_px=target_px)
    m = info["margins"]
    mode = f"bbox精确 {info.get('target_px')}px" if info.get("target_px") else f"ratio {args.ratio}"
    print(
        f"[OK] 画布留白({mode}): {info['src_size'][0]}×{info['src_size'][1]} "
        f"→ 内容 {info['content_size'][0]}×{info['content_size'][1]} "
        f"居中于 {args.canvas}×{args.canvas}（alpha bbox={info['bbox']}，"
        f"边距 左{m['left']} 上{m['top']} 右{m['right']} 下{m['bottom']}）"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
