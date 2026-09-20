# Captures the game window while it runs.
#   .\shot.ps1 -Out D:\tmp\menu.png

param([string]$Out = "$PSScriptRoot\build\shot.png")

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Shot -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct RECT { public int Left, Top, Right, Bottom; }
'@

$proc = Get-Process SPEED2 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $proc) { "the game is not running"; exit 1 }

$hwnd = $proc.MainWindowHandle

# The game freezes rendering without focus, and the capture grabs whatever is
# composited on screen: without bringing the window forward, the shot comes out
# black or catches another window.
[void][Shot.Win]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 700

$r = New-Object Shot.Win+RECT
[void][Shot.Win]::GetWindowRect($hwnd, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)

# CopyFromScreen grabs what is composited on screen: it works with windowed D3D,
# unlike BitBlt on the window DC, which usually comes back black.
$gfx.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$gfx.Dispose()

$dir = Split-Path $Out -Parent
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved: $Out ($w x $h)"
