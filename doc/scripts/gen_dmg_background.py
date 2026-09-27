#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 macOS DMG 的窗口背景图（Windows 侧预生成，随仓库入库）。

产物（路径相对仓库根）：
    installer/macos/background.png       700×440    （与 create-dmg --window-size 严格等大）
    installer/macos/background@2x.png   1400×880    （Retina，Finder 按同名 @2x 自动取用）

画面（见 docs/superpowers/specs/2026-09-28-installer-and-dmg-design.md §4.2）：
    暖白 #FAF9F5 竖向渐变到 #F0EEE6；中间一支克制的 clay 箭头（#D97757，约 60% 不透明度）；
    箭头下方一行 13px #6B665E 中文说明；顶部留白给 Finder 标题栏；底部极浅色带收边。

坐标全部按 700×440 的「逻辑画布」定义（与 spec §4.1 的 create-dmg 参数对齐），绘制时整体乘以
缩放系数（1x → ×1，2x → ×2），两种尺寸共用同一份绘制逻辑，不存在两份硬编码坐标。

用法（本机唯一带 Pillow 的解释器，MSYS2 自带的 python 没有 PIL）：
    "C:/Program Files/Python311/python.exe" doc/scripts/gen_dmg_background.py

依赖：Pillow + 标准库。可重复执行（直接覆盖已有文件）。
生成后自检：读回两张 PNG 断言尺寸 700×440 / 1400×880，不符即报错退出（退出码 1）。
"""

from __future__ import annotations

import math
import sys
from functools import lru_cache
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageDraw, ImageFont
except ImportError as exc:  # 本机 Python 3.11 实测带 Pillow 12.2.0；MSYS2 的 python 没有
    raise SystemExit(
        "错误：未找到 Pillow（PIL），无法生成 DMG 背景图。\n"
        "请用本机带 Pillow 的解释器运行本脚本：\n"
        '  "C:/Program Files/Python311/python.exe" doc/scripts/gen_dmg_background.py\n'
        "（MSYS2 自带的 python 没有 PIL；若该解释器也缺 Pillow，可执行：\n"
        '  "C:/Program Files/Python311/python.exe" -m pip install Pillow）\n'
        f"原始错误：{exc}"
    ) from exc


def _print(msg: str) -> None:
    """打印一行；终端编码不支持某些字符时用 replace 兜底，不中断脚本。"""
    enc = sys.stdout.encoding or "utf-8"
    print(msg.encode(enc, "replace").decode(enc, "replace"))


# ---------------------------------------------------------------------------
# 逻辑画布与版面（全部是 700×440 下的逻辑像素）
# ---------------------------------------------------------------------------
LOGICAL_SIZE = (700, 440)  # create-dmg --window-size 700 440，背景图必须严格等于此尺寸
ICON_LEFT_X = 190  # --icon "DevDesk.app" 190 210 的 x
ICON_RIGHT_X = 510  # --app-drop-link 510 210 的 x
ICON_CENTER_Y = 210  # 两个图标的 y
ICON_SIZE = 120  # --icon-size 120

ICON_HALF = ICON_SIZE // 2
ARROW_EDGE_GAP = 40  # 箭头两端距图标边缘的留白
ARROW_X0 = ICON_LEFT_X + ICON_HALF + ARROW_EDGE_GAP  # 290
ARROW_X1 = ICON_RIGHT_X - ICON_HALF - ARROW_EDGE_GAP  # 410
ARROW_LINE_W = 3  # 线宽（克制：细线 + 小箭头，不做粗重实心箭头）
ARROW_HEAD_LEN = 15
ARROW_HEAD_DEG = 32
ARROW_ALPHA = 153  # 约 60% 不透明度（255 × 0.6）

TEXT = "拖入「应用程序」文件夹完成安装"
TEXT_PX = 13  # spec §4.2
TEXT_CENTER = (LOGICAL_SIZE[0] // 2, 262)  # 箭头正下方居中
FONT_PATH = Path("C:/Windows/Fonts/msyh.ttc")  # 微软雅黑；缺失时退化为无文字版本

BAND_H = 26  # 底部收边色带高度
HAIRLINE_H = 1  # 色带上沿 1px 发丝线

# ---------------------------------------------------------------------------
# 配色（DevDesk 主题 token；spec §4.2）
# ---------------------------------------------------------------------------
COLOR_TOP = (250, 249, 245)  # #FAF9F5
COLOR_BOTTOM = (240, 238, 230)  # #F0EEE6
COLOR_ARROW = (217, 119, 87)  # #D97757 Claude Clay
COLOR_TEXT = (107, 102, 94)  # #6B665E
COLOR_BAND = (231, 226, 213)  # #E7E2D5 极浅色带（低不透明度叠在渐变末端上）
COLOR_HAIRLINE = (214, 208, 192)  # #D6D0C0

BAND_ALPHA = 102  # 约 40%
HAIRLINE_ALPHA = 64  # 约 25%

SUPERSAMPLE = 4  # 过采样倍数：箭头/文字先放大 4 倍再降采样，得到平滑抗锯齿边缘


# ---------------------------------------------------------------------------
# 绘制工具
# ---------------------------------------------------------------------------
def _uniform_layer(k: int, color: tuple[int, int, int]) -> Image.Image:
    """建一张过采样层：整幅预填 color 但 alpha=0。

    这样降采样时透明区参与平均的 RGB 与图形同色，缩放后边缘不会出现暗色描边
    （非预乘 RGBA 直接缩放经典问题）。
    """
    return Image.new("RGBA", (LOGICAL_SIZE[0] * k, LOGICAL_SIZE[1] * k), (*color, 0))


def _scale_alpha(layer: Image.Image, alpha: int) -> Image.Image:
    """把整层 alpha 按 alpha/255 等比缩放（用 ImageChops 走 C 路径，不逐像素回调）。"""
    flat = Image.new("L", layer.size, alpha)
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), flat))
    return layer


def _stroke(d: ImageDraw.ImageDraw, p0: tuple[float, float], p1: tuple[float, float],
            width: float, fill: tuple[int, int, int, int]) -> None:
    """画一条带圆头端点的线段（PIL 的 line 没有 round cap，用端点圆补齐）。"""
    d.line([p0, p1], fill=fill, width=max(1, round(width)))
    r = width / 2
    for x, y in (p0, p1):
        d.ellipse([x - r, y - r, x + r, y + r], fill=fill)


def _vertical_gradient(size: tuple[int, int], top: tuple[int, int, int],
                       bottom: tuple[int, int, int]) -> Image.Image:
    """竖向线性渐变（先造 1px 宽的色柱，再横向拉伸成整幅）。"""
    w, h = size
    strip = Image.new("RGB", (1, h))
    px = strip.load()
    for y in range(h):
        t = y / (h - 1) if h > 1 else 0.0
        px[0, y] = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
    return strip.resize((w, h), Image.Resampling.BILINEAR)


@lru_cache(maxsize=None)
def _load_font(font_px: int):
    """加载中文字体；缺失/加载失败时返回 None（退化为无文字版本，只提示一次）。"""
    if not FONT_PATH.exists():
        _print(f"[WARN] 未找到中文字体 {FONT_PATH}，本次生成不带说明文字的版本")
        _print("       需要文字请在 Windows 上运行本脚本，或修改脚本里的 FONT_PATH 常量")
        return None
    try:
        return ImageFont.truetype(str(FONT_PATH), font_px)
    except OSError as exc:
        _print(f"[WARN] 字体加载失败（{exc}），本次生成不带说明文字的版本")
        return None


# ---------------------------------------------------------------------------
# 各图层（坐标一律 = 逻辑坐标 × k）
# ---------------------------------------------------------------------------
def _arrow_layer(k: int) -> Image.Image:
    """中间那支 clay 箭头：细线杆 + 双向圆头，整体 60% 不透明度。"""
    layer = _uniform_layer(k, COLOR_ARROW)
    d = ImageDraw.Draw(layer)
    ink = (*COLOR_ARROW, 255)
    y = ICON_CENTER_Y * k
    x0, x1 = ARROW_X0 * k, ARROW_X1 * k
    w = ARROW_LINE_W * k

    _stroke(d, (x0, y), (x1, y), w, ink)
    head = ARROW_HEAD_LEN * k
    ang = math.radians(ARROW_HEAD_DEG)
    for sign in (-1, 1):
        tip_back = (x1 - head * math.cos(ang), y + sign * head * math.sin(ang))
        _stroke(d, tip_back, (x1, y), w, ink)
    return _scale_alpha(layer, ARROW_ALPHA)


def _text_layer(k: int) -> Image.Image:
    """箭头下方的一行说明文字（无字体时返回全透明层）。"""
    layer = _uniform_layer(k, COLOR_TEXT)
    font = _load_font(round(TEXT_PX * k))
    if font is None:
        return layer
    d = ImageDraw.Draw(layer)
    d.text((TEXT_CENTER[0] * k, TEXT_CENTER[1] * k), TEXT, font=font,
           fill=(*COLOR_TEXT, 255), anchor="mm")
    return layer


def _solid_band_layer(k: int, color: tuple[int, int, int], alpha: int,
                      top_logical_y: int, height_logical: int) -> Image.Image:
    """横贯整幅的纯色矩形（底部色带 / 发丝线），整体低不透明度。"""
    layer = _uniform_layer(k, color)
    d = ImageDraw.Draw(layer)
    y0 = top_logical_y * k
    d.rectangle([0, y0, LOGICAL_SIZE[0] * k - 1, y0 + height_logical * k - 1],
                fill=(*color, 255))
    return _scale_alpha(layer, alpha)


# ---------------------------------------------------------------------------
# 渲染
# ---------------------------------------------------------------------------
def render(scale: int) -> Image.Image:
    """按缩放系数 scale（1 或 2）渲染背景图，返回 RGB 图像。"""
    target = (LOGICAL_SIZE[0] * scale, LOGICAL_SIZE[1] * scale)
    k = scale * SUPERSAMPLE  # 过采样画布坐标 = 逻辑坐标 × k

    base = _vertical_gradient(target, COLOR_TOP, COLOR_BOTTOM).convert("RGBA")
    layers = [
        _arrow_layer(k),
        _text_layer(k),
        _solid_band_layer(k, COLOR_BAND, BAND_ALPHA,
                          LOGICAL_SIZE[1] - BAND_H, BAND_H),
        _solid_band_layer(k, COLOR_HAIRLINE, HAIRLINE_ALPHA,
                          LOGICAL_SIZE[1] - BAND_H - HAIRLINE_H, HAIRLINE_H),
    ]
    for layer in layers:
        base.alpha_composite(layer.resize(target, Image.Resampling.LANCZOS))
    return base.convert("RGB")


def main() -> int:
    root = Path(__file__).resolve().parents[2]  # doc/scripts/xxx.py → 仓库根
    out_dir = root / "installer" / "macos"
    out_dir.mkdir(parents=True, exist_ok=True)
    _print(f"[INFO] 输出目录：{out_dir}")

    for scale in (1, 2):
        name = "background.png" if scale == 1 else "background@2x.png"
        path = out_dir / name
        expected = (LOGICAL_SIZE[0] * scale, LOGICAL_SIZE[1] * scale)

        render(scale).save(path, "PNG")

        # 读回自检：尺寸必须与 create-dmg --window-size / @2x 严格一致
        with Image.open(path) as back:
            got, fmt = back.size, back.format
        if fmt != "PNG":
            raise AssertionError(f"[FAIL] {name} 不是 PNG（读回格式 {fmt}）")
        if got != expected:
            raise AssertionError(
                f"[FAIL] {name} 尺寸不对：期望 {expected[0]}×{expected[1]}，读回 {got[0]}×{got[1]}")
        n_bytes = path.stat().st_size
        if n_bytes <= 0:
            raise AssertionError(f"[FAIL] {name} 是空文件")
        _print(f"[OK] installer/macos/{name}  {got[0]}×{got[1]}  {n_bytes / 1024:.1f} KB")

    _print("[OK] 两张背景图已生成并通过尺寸断言")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
