#!/bin/bash
# scripts/check-consistency.sh
#
# 对比 HTML 原型与 C++ 二进制的截图，输出本次实况（产物清单 + 各步骤结果），不做静态结论。
#   1. Playwright (headless Chromium) 截 HTML demo 各页面
#   2. 启动 C++ 二进制截主窗口：Windows → PowerShell CopyFromScreen 按窗口矩形截 + 平均亮度检查；
#      macOS → screencapture -x 抓整屏（只能整屏，没有按窗口矩形裁剪的能力）；
#      截屏不可用时回退 Pillow 合成占位图
#
# 用法：
#   ./scripts/check-consistency.sh             # 完整流程
#   ./scripts/check-consistency.sh --html-only # 只跑 HTML 截图
#   ./scripts/check-consistency.sh --cpp-only  # 只跑 C++ 截图
#   ./scripts/check-consistency.sh --no-launch # 不启动 DevDesk（只走 Pillow 合成）
#
# 前置：
#   - Windows 11 + Git Bash / MSYS2（cygpath / taskkill）；或 macOS（screencapture）
#   - Windows 交互式桌面会话：CopyFromScreen 需要未锁屏、RDP 未断开
#     （锁屏/断开时可能截到黑图——脚本会打印平均亮度并 WARN）
#   - macOS：终端需被授予「屏幕录制」权限，否则 screencapture 拿不到窗口内容
#     （只 WARN，随后回退 Pillow 合成）
#   - build/DevDesk[.exe] 已编译
#   - python3 + playwright（HTML 截图）、Pillow（回退合成）——缺失只 WARN 不 fail
#
# 退出码：0 = 流程跑完（含已 WARN 的降级）；1 = 前置缺失（HTML / exe 不存在）

set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCREENSHOTS_DIR="$PROJECT_ROOT/docs/screenshots"
HTML_FILE="$PROJECT_ROOT/docs/mockups/devdesk-prototype.html"
DOCS_FILE="$PROJECT_ROOT/docs/CONSISTENCY-CHECK.md"

# 平台标识 DEVDESK_OS 决定走哪条截屏分支（Windows CopyFromScreen / macOS screencapture），
# 二进制名也按平台取（Windows 是 DevDesk.exe）。
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) DEVDESK_OS="windows"; CPP_BIN="$PROJECT_ROOT/build/DevDesk.exe" ;;
    Darwin)               DEVDESK_OS="darwin";  CPP_BIN="$PROJECT_ROOT/build/DevDesk" ;;
    *)                    DEVDESK_OS="linux";   CPP_BIN="$PROJECT_ROOT/build/DevDesk" ;;
esac

# Chromium 认 Windows 盘符路径（file:///D:/...），MSYS 的 /d/... 加载不了
HTML_URL="file://$PROJECT_ROOT/docs/mockups/devdesk-prototype.html"
if command -v cygpath >/dev/null 2>&1; then
    HTML_URL="file:///$(cygpath -m "$HTML_FILE")"
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

mkdir -p "$SCREENSHOTS_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
log() { echo -e "${CYAN}[$(date +%H:%M:%S)]${NC} $*"; }
ok() { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err() { echo -e "${RED}[ERROR]${NC} $*"; }

# 文件大小（字节）：BSD 是 stat -f%z、GNU 是 stat -c%s，参数不通用（macOS 上 -c%s 直接报错），
# 故抽 helper。取不到（文件不存在等）回退打印 0；调用处只拿它做展示与 KB 换算，不参与判定
#（判定另用 [ -s ]），所以这个 0 不会漏进结果。
file_size() {
    if [ "$DEVDESK_OS" = "darwin" ]; then
        stat -f%z "$1" 2>/dev/null || echo 0
    else
        stat -c%s "$1" 2>/dev/null || echo 0
    fi
}

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

HTML_RESULT="未运行"
CPP_RESULT="未运行"

log "项目根: $PROJECT_ROOT"
log "HTML:   $HTML_URL"
log "C++ bin: $CPP_BIN"

if [ ! -f "$HTML_FILE" ]; then
    [ "$CPP_ONLY" = true ] || { err "HTML demo 不存在: $HTML_FILE"; exit 1; }
fi
if [ ! -f "$CPP_BIN" ]; then
    [ "$HTML_ONLY" = true ] || { err "C++ binary 不存在: ${CPP_BIN}（先构建：cmake --build build）"; exit 1; }
fi
[ -f "$DOCS_FILE" ] && log "历史分析文档（可能过期）: $DOCS_FILE"

PYTHON3="$(command -v python3 || command -v python || true)"
py_has() {
    [ -n "$PYTHON3" ] && "$PYTHON3" -c "import $1" 2>/dev/null
}

if [ "$CPP_ONLY" = false ]; then
    log "=== 任务 1: 截取 HTML demo 页面 ==="
    if [ -z "$PYTHON3" ]; then
        warn "找不到 python3 —— 跳过 HTML 截图（不影响 C++ 侧）"
        HTML_RESULT="跳过（无 python3）"
    elif ! py_has playwright; then
        warn "python3 无 playwright 模块 —— 跳过 HTML 截图（装：$PYTHON3 -m pip install playwright）"
        HTML_RESULT="跳过（缺 playwright）"
    else
        cat > "$TMP_DIR/check_html.py" << 'PYEOF'
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
        if HTML_URL="$HTML_URL" OUT_DIR="$SCREENSHOTS_DIR" "$PYTHON3" "$TMP_DIR/check_html.py"; then
            ok "HTML 截图完成（初始 + 5 页）"
            HTML_RESULT="完成（6 张 PNG）"
        else
            warn "HTML 截图失败（playwright 浏览器未安装？跑: $PYTHON3 -m playwright install chromium）"
            HTML_RESULT="失败"
        fi
    fi
fi

# 生成的 .ps1 / .py 写在 ${TMP_DIR}（mktemp -d）里，不往仓库落临时文件。
write_shot_ps1() {
    # 注意：ps1 保持纯 ASCII —— Windows PowerShell 5.1 读无 BOM 的 UTF-8 .ps1
    # 会按 ANSI(GBK) 解码，非 ASCII 字符串会变乱码/报错
    cat > "$TMP_DIR/shot-main-window.ps1" << 'PS1'
param([string]$Title = "DevDesk", [string]$Out = "cpp-home.png", [int]$TimeoutSec = 10)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class WinShot {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    // 必须 public：作为 public extern 方法的 out 参数，私有类型会 CS0051（可访问性不一致）
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static IntPtr Find(string title) {
        IntPtr exact = IntPtr.Zero, partial = IntPtr.Zero;
        EnumWindows((h, lp) => {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(256);
            GetWindowTextW(h, sb, 256);
            var t = sb.ToString();
            if (t == title) { exact = h; return false; }
            if (partial == IntPtr.Zero && t.IndexOf(title, StringComparison.OrdinalIgnoreCase) >= 0) partial = h;
            return true;
        }, IntPtr.Zero);
        return exact != IntPtr.Zero ? exact : partial;
    }
    public static int[] RectOf(IntPtr hWnd) {
        RECT r;
        GetWindowRect(hWnd, out r);
        return new int[] { r.L, r.T, r.R - r.L, r.B - r.T };
    }
}
"@
[WinShot]::SetProcessDPIAware() | Out-Null
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$h = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    $h = [WinShot]::Find($Title)
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 300
}
if ($h -eq [IntPtr]::Zero) { Write-Output "shot-error: no visible window with title containing '$Title' within ${TimeoutSec}s"; exit 3 }
$r = [WinShot]::RectOf($h)
$w = $r[2]; $ht = $r[3]
if ($w -le 0 -or $ht -le 0) { Write-Output "shot-error: bad window rect ${w}x${ht}"; exit 4 }
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r[0], $r[1], 0, 0, (New-Object System.Drawing.Size $w, $ht)) | Out-Null
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
# 平均亮度（32x32 网格抽样；锁屏 / RDP 断开时 CopyFromScreen 会截到全黑）
$sum = 0.0; $n = 0
$stepX = [Math]::Max(1, [int]($w / 32)); $stepY = [Math]::Max(1, [int]($ht / 32))
for ($y = [int]($stepY / 2); $y -lt $ht; $y += $stepY) {
    for ($x = [int]($stepX / 2); $x -lt $w; $x += $stepX) {
        $c = $bmp.GetPixel($x, $y)
        $sum += ($c.R + $c.G + $c.B) / 3.0
        $n++
    }
}
$bright = 0.0
if ($n -gt 0) { $bright = $sum / $n }
$bmp.Dispose()
Write-Output ("shot-ok: {0}x{1} -> {2} brightness={3:N1}" -f $w, $ht, $Out, $bright)
PS1
}

write_synth_py() {
    cat > "$TMP_DIR/check_cpp_synth.py" << 'PYEOF'
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 1600, 1000
BG = (250, 249, 245); TITLEBAR = (243, 243, 243)
TXT = (31, 29, 26); TXT2 = (107, 102, 96); TXT3 = (154, 148, 138)
ACCENT = (217, 119, 87)

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)

font_candidates = ["C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                   "/System/Library/Fonts/Hiragino Sans GB.ttc"]
def f(sz):
    for p in font_candidates:
        try:
            return ImageFont.truetype(p, sz)
        except Exception:
            pass
    return ImageFont.load_default()

d.rectangle([(0, 0), (W, 28)], fill=TITLEBAR)
for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 202, 63)]):
    d.ellipse([(15 + i * 20, 9), (27 + i * 20, 21)], fill=c)
b = d.textbbox((0, 0), "DevDesk", font=f(14))
d.text(((W - (b[2] - b[0])) // 2, 7), "DevDesk", fill=TXT, font=f(14))

lines = [
    ("DevDesk（C++ 版）截图占位", 22, TXT),
    ("", 12, TXT2),
    ("PowerShell 截屏不可用（无桌面会话 / 窗口未出现），此图为 Pillow 合成占位", 13, TXT2),
    ("真实截图请看同目录的 html-*.png（HTML 原型）", 12, TXT3),
]
y = 160
for txt, sz, col in lines:
    if not txt:
        y += 8
        continue
    ff = f(sz)
    b = d.textbbox((0, 0), txt, font=ff)
    d.text(((W - (b[2] - b[0])) // 2, y), txt, fill=col, font=ff)
    y += sz + 10

px, py, pw, ph = 60, 300, W - 120, 460
d.rectangle([(px, py), (px + pw, py + ph)], fill=(255, 255, 255), outline=(232, 230, 220))
d.text((px + 28, py + 22), "当前 C++ theme.cpp tokens (Light)", font=f(18), fill=TXT)
rows = [("background", "#FAF9F5", (250, 249, 245)), ("surface", "#FFFFFF", (255, 255, 255)),
        ("surfaceElevated", "#FFFFFF", (255, 255, 255)), ("hover", "#F5F2EC", (245, 242, 236)),
        ("active", "#EBE7DF", (235, 231, 223)), ("primary (Claude Clay)", "#D97757", ACCENT),
        ("primaryHover", "#E0896B", (224, 137, 107)), ("primaryPressed", "#B85F3F", (184, 95, 63)),
        ("primarySoftBg", "#F5E6DE", (245, 230, 222)), ("textPrimary", "#1F1D1A", TXT),
        ("textSecondary", "#6B6660", TXT2), ("textTertiary", "#9A948A", TXT3),
        ("borderDefault", "#E8E6DC", (232, 230, 220))]
for i, (n, hx, rgb) in enumerate(rows):
    yy = py + 78 + i * 21
    d.text((px + 28, yy), f"{n:<22} {hx}", font=f(13), fill=TXT)
    d.rectangle([(px + 320, yy + 2), (px + 360, yy + 18)], fill=rgb, outline=(0, 0, 0))

out = os.path.join(os.environ["OUT_DIR"], "cpp-home.png")
img.save(out)
print(f"Wrote {out} ({os.path.getsize(out)} bytes)")
PYEOF
}

synth_fallback() {
    if [ -n "$PYTHON3" ] && py_has PIL; then
        if OUT_DIR="$SCREENSHOTS_DIR" "$PYTHON3" "$TMP_DIR/check_cpp_synth.py"; then
            ok "Pillow 合成 cpp-home.png 完成"
            return 0
        fi
        warn "Pillow 合成失败"
    else
        warn "python3 无 Pillow —— 无法合成占位图（装：${PYTHON3:-python3} -m pip install Pillow）"
    fi
    return 1
}

if [ "$HTML_ONLY" = false ]; then
    log "=== 任务 2: 截取 C++ 编译版 ==="
    if [ "$NO_LAUNCH" = true ]; then
        log "--no-launch：跳过启动，直接 Pillow 合成 cpp-home.png"
        if synth_fallback; then CPP_RESULT="Pillow 合成（--no-launch）"; else CPP_RESULT="未产出（--no-launch 且无 Pillow）"; fi
    elif [ "$DEVDESK_OS" = "darwin" ]; then
        # macOS 分支只能整屏抓：screencapture 没有 CopyFromScreen 那种按窗口矩形裁剪的能力。
        # 保留 Pillow 合成回退：需要 TCC「屏幕录制」权限，没授权时截到的是桌面而没有窗口内容
        #（或直接非零退出），此时只 WARN，不 fail。
        log "启动 $CPP_BIN ..."
        "$CPP_BIN" >/dev/null 2>&1 &
        APP_PID=$!
        sleep 3   # 等窗口出现：无 CLI 手段探测窗口是否已起来，只能有界等待
        SHOT_OUT="$SCREENSHOTS_DIR/cpp-home.png"
        log "screencapture 整屏 -> $SHOT_OUT"
        if screencapture -x "$SHOT_OUT" 2>"$TMP_DIR/sc_err" && [ -s "$SHOT_OUT" ]; then
            ok "screencapture 成功（$(( $(file_size "$SHOT_OUT") / 1024 )) KB）"
            CPP_RESULT="screencapture 整屏（$(file_size "$SHOT_OUT") bytes）"
        else
            warn "screencapture 失败: $(cat "$TMP_DIR/sc_err" 2>/dev/null || echo unknown)"
            warn "macOS 可能未授予屏幕录制权限 —— 在「系统设置 → 隐私与安全性 → 屏幕录制」里允许你的 terminal"
            warn "回退 Pillow 合成 cpp-home.png"
            if synth_fallback; then CPP_RESULT="Pillow 合成（screencapture 失败）"; else CPP_RESULT="未产出（screencapture 失败且无 Pillow）"; fi
        fi
        log "清理 DevDesk 进程..."
        # 用启动时记下的 PID（只杀本次启动的实例），不按进程名 pkill（会误伤别的 worktree）
        kill "$APP_PID" >/dev/null 2>&1 || true
    elif ! command -v cygpath >/dev/null 2>&1; then
        warn "找不到 cygpath（非 Git Bash 环境）—— 跳过 PowerShell 截屏"
        if synth_fallback; then CPP_RESULT="Pillow 合成（无 cygpath）"; else CPP_RESULT="未产出"; fi
    else
        log "启动 $CPP_BIN ..."
        "$CPP_BIN" >/dev/null 2>&1 &
        write_shot_ps1
        SHOT_OUT="$SCREENSHOTS_DIR/cpp-home.png"
        SHOT_LOG="$TMP_DIR/shot.log"
        powershell -NoProfile -ExecutionPolicy Bypass \
            -File "$(cygpath -w "$TMP_DIR/shot-main-window.ps1")" \
            -Title DevDesk \
            -Out "$(cygpath -w "$SHOT_OUT")" \
            -TimeoutSec 10 > "$SHOT_LOG" 2>&1
        SHOT_RC=$?
        cat "$SHOT_LOG"
        if [ "$SHOT_RC" -eq 0 ] && [ -s "$SHOT_OUT" ]; then
            ok "PowerShell 截屏成功（$(( $(file_size "$SHOT_OUT") / 1024 )) KB）"
            CPP_RESULT="PowerShell 截屏（$(file_size "$SHOT_OUT") bytes）"
            BRIGHT="$(sed -n 's/.*brightness=\([0-9.]*\).*/\1/p' "$SHOT_LOG" | head -1)"
            if [ -n "$BRIGHT" ] && awk -v b="$BRIGHT" 'BEGIN{exit !(b < 5)}'; then
                # 锁屏 / RDP 断开时 CopyFromScreen 会截到全黑，用亮度阈值（< 5）识别
                warn "截图平均亮度 ${BRIGHT}（疑似全黑：锁屏 / RDP 断开？）"
                CPP_RESULT="$CPP_RESULT · 疑似全黑（亮度 ${BRIGHT}）"
            fi
        else
            warn "PowerShell 截屏失败（rc=${SHOT_RC}）—— 回退 Pillow 合成"
            if synth_fallback; then CPP_RESULT="Pillow 合成（截屏失败 rc=${SHOT_RC}）"; else CPP_RESULT="未产出（截屏失败且无 Pillow）"; fi
        fi
        log "清理 DevDesk 进程..."
        taskkill -F -IM DevDesk.exe >/dev/null 2>&1 || true
    fi
fi

log "=== 任务 3: 本次实况 ==="
echo ""
echo "───── docs/screenshots/（本次产物）─────"
ls -la "$SCREENSHOTS_DIR"
echo ""
echo "───── 本次各步骤结果 ─────"
printf '  HTML 截图 : %s\n' "$HTML_RESULT"
printf '  C++  截图 : %s\n' "$CPP_RESULT"
if [ -f "$DOCS_FILE" ]; then
    log "历史对比分析: ${DOCS_FILE}（其结论可能是旧快照，以本次实况与最新 spec 为准）"
fi
ok "完成"
