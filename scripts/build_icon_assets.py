# -*- coding: utf-8 -*-
"""DevDesk 品牌图资产生成器（C 方案：绿瓦圆角终端窗 + 白窗 + 绿色 >_ 字形）。

用途：logo 设计迭代后重新生成全应用共用的五份图标资产（已随 git 提交，
构建**不依赖**本脚本与 PIL——本脚本只在设计变更时人工运行）。

产出（默认写到仓库 assets/，--out 可指到任意目录做候选评审）：
  icon.ico                    16/24/32/48/64/128/256 七档全彩 ICO（exe 内嵌资源
                              + 安装包 SetupIconFile + Windows 托盘 tray-icon-win.ico 同源）
  tray-icon-win.ico           同 icon.ico 内容（Windows ExtractIconExW 只认 .ico）
  icon.png                    256px 全彩 PNG（EUI-NEO dsl_app 默认窗口图标
                              applyWindowIcon → glfwSetWindowIcon = 任务栏/alt-tab；
                              macOS build-app.sh 亦从它派生 .icns）
  tray-icon-template.png      22px 纯黑+alpha template 蒙版（macOS 托盘，setTemplate 反色）
  tray-icon-template@2x.png   44px 倍图（EUI-NEO eui_tray_image 按同名 @2x 惯例拾取）

几何契约（256 单位设计画布，与已确认 demo 一致——改这里必须过用户评审，
且 src/ui_kit/logo.cpp 的矢量 logo 用同一组归一化坐标，两处同步动）：
  绿瓦  [8,8]-[248,248] rx=52  #07C160
  白窗  [46,62]-[210,194] rx=22
  >_    (116,102)→(88,126)→(116,150) 折线 + (132,150)→(168,150) 短横，
        stroke 13 圆帽，绿色（同绿瓦）
macOS template 蒙版为黑描边窗（stroke 22）+ 黑 >_（stroke 15），透明底。

幂等：内容不变则不落盘（比对字节后再替换，git status 不产生噪声）。

运行：python scripts/build_icon_assets.py [--out D:/somewhere]
依赖：Pillow（仅本脚本需要；pip install pillow）
"""
import argparse
import io
import pathlib
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("缺少 Pillow：pip install pillow 后重试", file=sys.stderr)
    sys.exit(1)

# Windows GBK 控制台打印中文/emoji 会 UnicodeEncodeError（memory 07 惯例）
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DESIGN = 256          # 设计画布边长（逻辑单位）
SUPER = 4             # 超采样倍数（抗锯齿；1024px 光栅后 LANCZOS 缩回）
GREEN = (7, 193, 96, 255)    # 微信绿 #07C160（= theme wechat.primary）
WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
TEMPLATE_SIZES = {"tray-icon-template.png": 22, "tray-icon-template@2x.png": 44}


def capline(draw, p1, p2, width, color):
    """圆帽线段：粗线 + 两端各一枚实心圆（PIL 无 round cap，手工补帽）。"""
    scale = SUPER
    draw.line([p1[0] * scale, p1[1] * scale, p2[0] * scale, p2[1] * scale],
              fill=color, width=int(width * scale))
    r = width * scale / 2
    for cx, cy in (p1, p2):
        draw.ellipse([cx * scale - r, cy * scale - r, cx * scale + r,
                      cy * scale + r], fill=color)


def render_fullcolor():
    """全彩主图：绿瓦 + 白窗 + 绿 >_。返回 256px RGBA Image。"""
    scale = SUPER
    img = Image.new("RGBA", (DESIGN * scale, DESIGN * scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8 * scale, 8 * scale, 248 * scale, 248 * scale],
                        radius=52 * scale, fill=GREEN)
    d.rounded_rectangle([46 * scale, 62 * scale, 210 * scale, 194 * scale],
                        radius=22 * scale, fill=WHITE)
    capline(d, (116, 102), (88, 126), 13, GREEN)
    capline(d, (88, 126), (116, 150), 13, GREEN)
    capline(d, (132, 150), (168, 150), 13, GREEN)
    return img.resize((DESIGN, DESIGN), Image.LANCZOS)


def render_template():
    """macOS template 蒙版：黑描边窗 + 黑 >_，透明底（模板渲染只取 alpha）。"""
    scale = SUPER
    img = Image.new("RGBA", (DESIGN * scale, DESIGN * scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([46 * scale, 62 * scale, 210 * scale, 194 * scale],
                        radius=22 * scale, outline=BLACK, width=int(22 * scale))
    capline(d, (116, 102), (88, 126), 15, BLACK)
    capline(d, (88, 126), (116, 150), 15, BLACK)
    capline(d, (132, 150), (168, 150), 15, BLACK)
    return img.resize((DESIGN, DESIGN), Image.LANCZOS)


def build_ico(master_256):
    """七档 PNG 拼成多尺寸 ICO（结构同 build_icons.py 初版，字段手工打包）。"""
    entries = []
    for size in ICO_SIZES:
        buf = io.BytesIO()
        master_256.resize((size, size), Image.LANCZOS).save(buf, "PNG")
        entries.append((size, buf.getvalue()))
    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    directory = b""
    for size, data in entries:
        width = 0 if size >= 256 else size  # 256 档宽度字段按规范写 0
        directory += struct.pack("<BBBBHHII", width, width, 0, 0, 1, 32,
                                 len(data), offset)
        offset += len(data)
    return header + directory + b"".join(data for _, data in entries)


def write_if_changed(path, payload):
    """幂等落盘：内容一致不写（不动 mtime，git status 零噪声）。返回是否替换。"""
    if path.exists() and path.read_bytes() == payload:
        return False
    path.write_bytes(payload)
    return True


def main():
    parser = argparse.ArgumentParser(
        description="生成 DevDesk 品牌图标五件套（详见模块 docstring）")
    parser.add_argument("--out", default="assets",
                        help="输出目录（默认仓库 assets/）")
    args = parser.parse_args()

    out_dir = pathlib.Path(__file__).resolve().parent.parent / args.out \
        if not pathlib.Path(args.out).is_absolute() else pathlib.Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    full = render_fullcolor()
    template = render_template()

    ico = build_ico(full)
    changed = []
    changed += ["icon.ico"] if write_if_changed(out_dir / "icon.ico", ico) else []
    changed += ["tray-icon-win.ico"] if write_if_changed(
        out_dir / "tray-icon-win.ico", ico) else []

    png_buf = io.BytesIO()
    full.save(png_buf, "PNG")
    changed += ["icon.png"] if write_if_changed(
        out_dir / "icon.png", png_buf.getvalue()) else []

    for name, size in TEMPLATE_SIZES.items():
        buf = io.BytesIO()
        template.resize((size, size), Image.LANCZOS).save(buf, "PNG")
        changed += [name] if write_if_changed(out_dir / name, buf.getvalue()) else []

    if changed:
        print(f"OK 已更新 {len(changed)} 份：{', '.join(changed)}（目录 {out_dir}）")
        print("注意：icon.ico/icon.png 的几何与 src/ui_kit/logo.cpp 矢量版同契约，"
              "改设计时两处同步。")
    else:
        print(f"无变化（{out_dir} 下五份产物与当前几何一致）")
    # 自检采样：角全透明 / 瓦绿 / 窗白 / >_ 折点绿（防绘制参数被误动后静默产空图；
    # 采样点须在圆角内：瓦心取 (128,30)——(20,20) 落在 52px 圆角外、本来就是透明）
    corner = full.getpixel((4, 4))
    tile = full.getpixel((128, 30))
    win = full.getpixel((128, 128))
    glyph = full.getpixel((88, 126))
    assert corner[3] == 0, f"角应为全透明，实际 {corner}"
    assert tile == GREEN, f"瓦心应 #07C160，实际 {tile}"
    assert win == WHITE, f"窗内应为白，实际 {win}"
    assert glyph == GREEN, f">_ 折点应为绿，实际 {glyph}"
    print("自检采样通过（角透明/瓦绿/窗白/折点绿）")


if __name__ == "__main__":
    main()
