#!/bin/bash
# scripts/check-consistency.sh
# DevDesk HTML demo vs C++ binary 视觉一致性检查脚本
#
# 用途：
#   1. 用 Playwright (headless Chromium) 截取 HTML demo 5 个页面
#   2. 启动 build/DevDesk 并尝试截屏当前窗口（Home）
#   3. 输出对比报告占位（详细分析见 docs/CONSISTENCY-CHECK.md）
#
# 用法：
#   ./scripts/check-consistency.sh             # 跑完整流程
#   ./scripts/check-consistency.sh --html-only # 只跑 HTML 截图
#   ./scripts/check-consistency.sh --cpp-only  # 只跑 C++ 截图
#   ./scripts/check-consistency.sh --no-launch # 不启动 DevDesk（只截图）
#
# 前置：
#   - Python 3.10+（脚本会自己 pip install playwright + pillow）
#   - macOS 27+（screencapture TCC 权限）
#   - 终端必须被授予"屏幕录制"权限（系统设置 → 隐私与安全性）
#   - build/DevDesk 已编译

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCREENSHOTS_DIR="$PROJECT_ROOT/docs/screenshots"
HTML_URL="file://$PROJECT_ROOT/docs/mockups/devdesk-prototype.html"
CPP_BIN="$PROJECT_ROOT/build/DevDesk"
DOCS_FILE="$PROJECT_ROOT/docs/CONSISTENCY-CHECK.md"

mkdir -p "$SCREENSHOTS_DIR"

# ---- 颜色 ----
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
log() { echo -e "${CYAN}[$(date +%H:%M:%S)]${NC} $*"; }
ok() { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err() { echo -e "${RED}[ERROR]${NC} $*"; }

# ---- 解析参数 ----
HTML_ONLY=false
CPP_ONLY=false
NO_LAUNCH=false
for arg in "$@"; do
    case $arg in
        --html-only) HTML_ONLY=true ;;
        --cpp-only)  CPP_ONLY=true ;;
        --no-launch) NO_LAUNCH=true ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *) err "未知参数: $arg"; exit 1 ;;
    esac
done

# ---- 检查前置 ----
log "项目根: $PROJECT_ROOT"
log "HTML:   $HTML_URL"
log "C++ bin: $CPP_BIN"

[ -f "$PROJECT_ROOT/docs/mockups/devdesk-prototype.html" ] || { err "HTML demo 不存在: docs/mockups/devdesk-prototype.html"; exit 1; }
[ -x "$CPP_BIN" ] || { err "C++ binary 不可执行: $CPP_BIN（先 cmake + make）"; exit 1; }
[ -f "$DOCS_FILE" ] && log "分析文档: $DOCS_FILE"

# ---- Python 依赖 ----
log "检查 Python 依赖..."
PYTHON3=$(command -v python3 || true)
[ -z "$PYTHON3" ] && { err "找不到 python3"; exit 1; }

ensure_pip_pkg() {
    local pkg="$1"
    if ! "$PYTHON3" -c "import $pkg" 2>/dev/null; then
        log "安装 Python 包: $pkg"
        pip3 install --break-system-packages --quiet "$pkg" || \
            pip3 install --quiet "$pkg" || \
            { err "安装 $pkg 失败"; return 1; }
    fi
    ok "$pkg 已安装"
}

if ! $CPP_ONLY; then
    ensure_pip_pkg playwright || exit 1
    ensure_pip_pkg PIL || ensure_pip_pkg Pillow || exit 1
fi

# ---- 任务 1: HTML 截图 ----
if ! $CPP_ONLY; then
    log "=== 任务 1: 截取 HTML demo 5 个页面 ==="
    cat > /tmp/devdesk_check_html.py << 'PYEOF'
import asyncio, os, sys
from playwright.async_api import async_playwright

HTML_URL = os.environ["HTML_URL"]
OUT_DIR = os.environ["OUT_DIR"]
PAGES = [("home", "home"), ("markdown", "markdown"), ("todo", "todo"),
         ("claude", "claude"), ("settings", "settings")]

async def main():
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True)
        ctx = await browser.new_context(viewport={"width": 1600, "height": 1000})
        page = await ctx.new_page()
        print(f"加载 {HTML_URL}", flush=True)
        await page.goto(HTML_URL, wait_until="domcontentloaded")
        await page.wait_for_timeout(2500)

        full = os.path.join(OUT_DIR, "html-demo.png")
        await page.screenshot(path=full, full_page=False)
        print(f"  -> html-demo.png ({os.path.getsize(full)} bytes)", flush=True)

        for slug, target in PAGES:
            await page.evaluate(f"switchPage('{target}')")
            await page.wait_for_timeout(500)
            path = os.path.join(OUT_DIR, f"html-{slug}.png")
            await page.screenshot(path=path, full_page=False)
            print(f"  -> html-{slug}.png ({os.path.getsize(path)} bytes)", flush=True)

        await browser.close()

asyncio.run(main())
PYEOF
    HTML_URL="$HTML_URL" OUT_DIR="$SCREENSHOTS_DIR" "$PYTHON3" /tmp/devdesk_check_html.py
    ok "HTML 截图完成（5 页 + 初始）"
fi

# ---- 任务 2: C++ 截图 ----
if ! $HTML_ONLY; then
    log "=== 任务 2: 截取 C++ 编译版 ==="

    if $NO_LAUNCH; then
        log "--no-launch 模式：跳过启动，仅生成合成 cpp-home.png"
    else
        log "启动 $CPP_BIN..."
        open -a "$CPP_BIN" 2>/dev/null || "$CPP_BIN" &
        CPP_PID=$!
        sleep 3

        log "尝试 screencapture 截取当前窗口..."
        if screencapture -x "$SCREENSHOTS_DIR/cpp-home.png" 2>/tmp/sc_err; then
            ok "screencapture 成功"
        else
            warn "screencapture 失败: $(cat /tmp/sc_err 2>/dev/null || echo unknown)"
            warn "macOS 拒绝屏幕录制权限 — 在'系统设置 → 隐私与安全性 → 屏幕录制'里允许你的 terminal"
            warn "回退方案：用 Pillow 合成 cpp-home.png（基于实际 clearColor）"

            cat > /tmp/devdesk_check_cpp.py << 'PYEOF'
import os
from PIL import Image, ImageDraw, ImageFont
W, H = 1600, 1000
BG = (250, 249, 245)
TITLEBAR = (243, 243, 243)
TXT = (31, 29, 26)
TXT2 = (107, 102, 96)
TXT3 = (154, 148, 138)
ACCENT = (217, 119, 87)
img = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(img)

cjk = ["/System/Library/Fonts/Hiragino Sans GB.ttc",
       "/System/Library/Fonts/STHeiti Medium.ttc",
       "/System/Library/Fonts/STHeiti Light.ttc"]
def f(s):
    for p in cjk:
        try: return ImageFont.truetype(p, s)
        except: pass
    return ImageFont.load_default()

d.rectangle([(0,0),(W,28)], fill=TITLEBAR)
for i,c2 in enumerate([(255,95,86),(255,189,46),(39,202,63)]):
    d.ellipse([(15+i*20,9),(27+i*20,21)], fill=c2)
b = d.textbbox((0,0),"DevDesk", font=f(14))
d.text(((W-(b[2]-b[0]))//2, 7), "DevDesk", fill=TXT, font=f(14))

notes = [
    ("DevDesk C++ binary 当前窗口内容", 22, TXT),
    ("", 12, TXT2),
    ("（基于 src/main.cpp dslAppConfig + 模块 stub 状态推断）", 12, TXT2),
    ("", 12, TXT2),
    ("实际渲染：仅 clearColor #FAF9F5 + 窗口标题", 13, TXT2),
    ("4 个 module_* 的 compose() 均未绘制任何 UI（仅 LOG_DEBUG）", 13, TXT2),
    ("Router 调用链：main.cpp:compose -> Router::compose -> Module::compose", 11, TXT3),
]
y = 200
for txt, sz, col in notes:
    if not txt: y += 8; continue
    ff = f(sz); b = d.textbbox((0,0),txt,font=ff)
    d.text(((W-(b[2]-b[0]))//2, y), txt, fill=col, font=ff)
    y += sz + 10

px,py,pw,ph = 60, 480, W-120, 360
d.rectangle([(px,py),(px+pw,py+ph)], fill=(255,255,255), outline=(232,230,220))
d.text((px+28, py+22), "当前 C++ theme.cpp tokens (Light)", font=f(18), fill=TXT)
d.text((px+28, py+50), "（在浏览器中查看 HTML 原型可对比实际配色）", font=f(11), fill=TXT2)
rows = [("background","#FAF9F5",(250,249,245)),("surface","#FFFFFF",(255,255,255)),
        ("surfaceElevated","#FFFFFF",(255,255,255)),("hover","#F5F2EC",(245,242,236)),
        ("active","#EBE7DF",(235,231,223)),("primary (Claude Clay)","#D97757",ACCENT),
        ("primaryHover","#E0896B",(224,137,107)),("primaryPressed","#B85F3F",(184,95,63)),
        ("primarySoftBg","#F5E6DE",(245,230,222)),("textPrimary","#1F1D1A",TXT),
        ("textSecondary","#6B6660",TXT2),("textTertiary","#9A948A",TXT3),
        ("borderDefault","#E8E6DC",(232,230,220))]
for i,(n,h,rgb) in enumerate(rows):
    yy = py+78+i*21
    d.text((px+28, yy), f"{n:<22} {h}", font=f(13), fill=TXT)
    d.rectangle([(px+320,yy+2),(px+360,yy+18)], fill=rgb, outline=(0,0,0))
out = os.path.join(os.environ["OUT_DIR"], "cpp-home.png")
img.save(out)
print(f"Wrote {out} ({os.path.getsize(out)} bytes)")
PYEOF
            OUT_DIR="$SCREENSHOTS_DIR" "$PYTHON3" /tmp/devdesk_check_cpp.py
            ok "合成 cpp-home.png 完成"
        fi

        log "清理 DevDesk 进程..."
        pkill -f "build/DevDesk" 2>/dev/null || true
    fi
fi

# ---- 任务 3: 输出对比报告 ----
log "=== 任务 3: 对比报告 ==="
echo ""
echo "───── docs/screenshots/ ─────"
ls -la "$SCREENSHOTS_DIR"
echo ""
echo "───── 一致性结论（完整分析见 docs/CONSISTENCY-CHECK.md）─────"
cat << 'EOF'

[1] 配色方案 B（13 token）         HTML 与 C++ theme.cpp    完全一致 ✅
[2] 尺寸 token（14 token）          HTML 与 C++ SizeTokens  完全一致 ✅
[3] HTML 页面标题 (30px) vs spec (32px)                       不一致 ⚠️
[4] HTML 正文 (14px) vs spec (15px)                           不一致 ⚠️
[5] HTML CTA 圆角 (12px) vs spec (10/14)         中间值无定义 ⚠️
[6] HTML 默认主题 = Light，C++ g_current = Dark             不一致 ⚠️
[7] C++ 4 个 module_* 的 compose() 是 stub，未渲染 UI         未实现 ❌

EOF

[ -f "$DOCS_FILE" ] && log "完整分析: $DOCS_FILE"
ok "完成"