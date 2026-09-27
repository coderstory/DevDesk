# DevDesk 窗口效果真机探针
# 用法: powershell -File probe_winfx.ps1 [-SampleMain] [-SamplePet]
# 输出: 主窗/宠物窗 rect + 关键点像素（品红底衬下采样：透=偏品红，不透=主题底色）
param(
    [switch]$SampleMain,
    [switch]$SamplePet,
    [switch]$MenuTest,
    [int]$WaitSec = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class WinProbe {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lp);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr hWnd, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int w, int h, uint flags);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public class WinInfo { public IntPtr H; public string Title; public string Class; public int L, T, R, B; }

    public static List<WinInfo> ListWindows() {
        var list = new List<WinInfo>();
        EnumWindows((h, lp) => {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(256);
            GetWindowTextW(h, sb, 256);
            var title = sb.ToString();
            var cb = new StringBuilder(256);
            GetClassNameW(h, cb, 256);
            // #32768（弹出菜单）无窗口标题，不能按标题过滤
            if (string.IsNullOrEmpty(title) && cb.ToString() != "#32768") return true;
            RECT r;
            GetWindowRect(h, out r);
            var wi = new WinInfo { H = h, Title = title, Class = cb.ToString(), L = r.L, T = r.T, R = r.R, B = r.B };
            list.Add(wi);
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

[WinProbe]::SetProcessDPIAware() | Out-Null
if ($WaitSec -gt 0) { Start-Sleep -Seconds $WaitSec }

$wins = [WinProbe]::ListWindows()
$devdesk = $wins | Where-Object { $_.Title -eq 'DevDesk' } | Select-Object -First 1
$pet = $wins | Where-Object { $_.Title -eq 'DevDesk Pet' } | Select-Object -First 1

Write-Output ("windows-found: devdesk={0} pet={1}" -f ($devdesk -ne $null), ($pet -ne $null))
if ($devdesk) { Write-Output ("devdesk-rect: {0},{1} - {2},{3}  ({4}x{5})" -f $devdesk.L, $devdesk.T, $devdesk.R, $devdesk.B, ($devdesk.R-$devdesk.L), ($devdesk.B-$devdesk.T)) }
if ($pet)     { Write-Output ("pet-rect: {0},{1} - {2},{3}  ({4}x{5})" -f $pet.L, $pet.T, $pet.R, $pet.B, ($pet.R-$pet.L), ($pet.B-$pet.T)) }

if (-not ($SampleMain -or $SamplePet -or $MenuTest)) { exit 0 }
if ($MenuTest -and -not $pet) { Write-Output "ERROR: no pet window"; exit 1 }
if (-not $devdesk -and -not ($MenuTest -and $pet)) { Write-Output "ERROR: no DevDesk window"; exit 1 }

# 品红底衬窗：盖住采样目标区域，z 序放在 DevDesk 主窗正下方（仅像素采样需要）
if ($SampleMain -or $SamplePet) {
function New-MagentaForm([int]$x, [int]$y, [int]$w, [int]$h) {
    $f = New-Object System.Windows.Forms.Form
    $f.FormBorderStyle = 'None'
    $f.StartPosition = 'Manual'
    $f.Location = New-Object System.Drawing.Point($x, $y)
    $f.Size = New-Object System.Drawing.Size($w, $h)
    $f.BackColor = [System.Drawing.Color]::FromArgb(255, 0, 255)
    $f.Show() | Out-Null
    # 无 Application::Run 的消息泵，手动 DoEvents + Refresh 驱动一次同步绘制
    for ($i = 0; $i -lt 8; $i++) {
        [System.Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 50
    }
    $f.Refresh()
    return $f
}

# 覆盖主窗 + 宠物窗的联合区域（外扩 40px）
$left = [Math]::Min($devdesk.L, $(if ($pet) { $pet.L } else { $devdesk.L })) - 40
$top = [Math]::Min($devdesk.T, $(if ($pet) { $pet.T } else { $devdesk.T })) - 40
$right = [Math]::Max($devdesk.R, $(if ($pet) { $pet.R } else { $devdesk.R })) + 40
$bottom = [Math]::Max($devdesk.B, $(if ($pet) { $pet.B } else { $devdesk.B })) + 40
$mag = New-MagentaForm $left $top ($right - $left) ($bottom - $top)
Start-Sleep -Milliseconds 400
# 品红窗放到 DevDesk 主窗正下方（z 序）
[WinProbe]::SetWindowPos($mag.Handle, $devdesk.H, 0, 0, 0, 0, 0x0014) | Out-Null  # SWP_NOMOVE|SWP_NOSIZE
Start-Sleep -Milliseconds 400
}

function Sample-Point([int]$x, [int]$y, [string]$label) {
    $bmp = New-Object System.Drawing.Bitmap 1, 1
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size 1, 1)) | Out-Null
    $g.Dispose()
    $c = $bmp.GetPixel(0, 0)
    $bmp.Dispose()
    Write-Output ("{0}: ({1},{2}) = #{3:X2}{4:X2}{5:X2}  R={6} G={7} B={8}" -f $label, $x, $y, $c.R, $c.G, $c.B, $c.R, $c.G, $c.B)
}

if ($SampleMain) {
    $w = $devdesk.R - $devdesk.L; $h = $devdesk.B - $devdesk.T
    # 客户区采样：跳过标题栏（~32px 物理高），sidebar 中心（x≈130），内容区中心
    Sample-Point ($devdesk.L + [int]($w * 0.5)) ($devdesk.T + [int]($h * 0.55)) "main-content-center"
    Sample-Point ($devdesk.L + 130) ($devdesk.T + [int]($h * 0.55)) "main-sidebar-center"
    Sample-Point ($devdesk.L + [int]($w * 0.75)) ($devdesk.T + [int]($h * 0.8)) "main-content-br"
    # 标题栏（磨砂独活区域）
    Sample-Point ($devdesk.L + [int]($w * 0.5)) ($devdesk.T + 12) "main-titlebar"
    Sample-Point $left ($top + 10) "magenta-ref(main-rect-outside)"
}

if ($SamplePet -and $pet) {
    $pw = $pet.R - $pet.L; $ph = $pet.B - $pet.T
    # 四角（内缩 3px 避开边框）+ 中心（宠物本体）
    Sample-Point ($pet.L + 3) ($pet.T + 3) "pet-corner-tl"
    Sample-Point ($pet.R - 3) ($pet.T + 3) "pet-corner-tr"
    Sample-Point ($pet.L + 3) ($pet.B - 3) "pet-corner-bl"
    Sample-Point ($pet.R - 3) ($pet.B - 3) "pet-corner-br"
    Sample-Point ($pet.L + [int]($pw * 0.5)) ($pet.T + [int]($ph * 0.65)) "pet-center"
    Sample-Point ($pet.R + 12) ($pet.T + [int]($ph * 0.5)) "magenta-ref(pet-right)"
}

if ($mag) { $mag.Close(); $mag.Dispose() }

# ---- 菜单测试：右键宠物中心 → 枚举原生菜单窗（#32768）→ 截屏 → ESC 关闭 ----
if ($MenuTest -and $pet) {
    $cx = $pet.L + [int](($pet.R - $pet.L) / 2)
    $cy = $pet.T + [int](($pet.B - $pet.T) / 2)
    Write-Output ("right-click at pet center: ({0},{1})" -f $cx, $cy)
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class NativeInput {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
    [NativeInput]::SetCursorPos($cx, $cy) | Out-Null
    Start-Sleep -Milliseconds 200
    [NativeInput]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)  # RIGHTDOWN
    Start-Sleep -Milliseconds 60
    [NativeInput]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)  # RIGHTUP
    Start-Sleep -Milliseconds 900

    $menus = [WinProbe]::ListWindows() | Where-Object { $_.Class -eq '#32768' }
    if (-not $menus) {
        Write-Output "menu-test: NO native menu window found (FAIL)"
    } else {
        foreach ($m in $menus) {
            $mw = $m.R - $m.L; $mh = $m.B - $m.T
            Write-Output ("menu-window: {0},{1} - {2},{3}  ({4}x{5})  pet-window-is-128x128" -f $m.L, $m.T, $m.R, $m.B, $mw, $mh)
            if ($mw -gt 128 -or $mh -gt 128) {
                Write-Output "menu-test: menu extends beyond 128px pet window bounds (PASS)"
            }
        }
        # 截菜单区域（证明完整可见，含超出宠物窗的部分）
        $m0 = $menus | Select-Object -First 1
        $w = $m0.R - $m0.L; $h = $m0.B - $m0.T
        $bmp = New-Object System.Drawing.Bitmap $w, $h
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($m0.L, $m0.T, 0, 0, (New-Object System.Drawing.Size $w, $h)) | Out-Null
        $g.Dispose()
        # 输出到本脚本所属仓库的 build/probe/：原先硬编码的 wt-winfx worktree 早已删除，
        # 且 build/ 已在 .gitignore 内（截图不污染工作区）；目录缺了就建，
        # 否则 GDI+（Bitmap.Save）在目录不存在时会直接抛错
        $shotDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\probe'
        New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
        $shot = Join-Path $shotDir 'pet_menu_proof.png'
        $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Output "menu-screenshot: $shot"
    }
    # ESC 关菜单（keybd_event）
    [NativeInput]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [NativeInput]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
}
