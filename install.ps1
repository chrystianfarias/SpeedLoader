# Installs SpeedLoader into the game's scripts\ folder.
#
# What ends up in the game:
#
#   scripts\SpeedLoader.asi        the injected loader
#   scripts\SpeedLoader.ini        configuration
#   scripts\SpeedLoader\           Chromium runtime and helper
#   scripts\SpeedLoader\ui\        shell.html and the generated mods.json
#   scripts\SpeedLoader\mods\      one subdirectory per mod
#
#   .\install.ps1                  install everything
#   .\install.ps1 -ModsOnly        mods and UI only (leaves Chromium alone)

param(
    [string]$Game = "F:\Games\NFSU2",
    [switch]$ModsOnly
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build\Release"
$cef = Join-Path $root "third_party\cef"
$dest = Join-Path $Game "scripts"
$runtime = Join-Path $dest "SpeedLoader"

if (-not (Test-Path $Game)) { throw "game not found at $Game" }
New-Item -ItemType Directory -Force -Path $runtime | Out-Null

# ---- mods and UI: what changes on every iteration ------------------------
# Delete before copying: Copy-Item -Recurse only merges, so a mod removed from
# the source tree would stay installed in the game, haunting the UI.
foreach ($folder in "ui", "mods") {
    $target = Join-Path $runtime $folder
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Copy-Item (Join-Path $root $folder) $runtime -Recurse -Force
}

$ini = Join-Path $dest "SpeedLoader.ini"
if (-not (Test-Path $ini)) {
    # The installed ini is never overwritten: it belongs to the user, not to
    # the build.
    Copy-Item (Join-Path $root "SpeedLoader.ini") $ini
}

if ($ModsOnly) { "[ok] mods and UI updated"; exit 0 }

# ---- binaries ------------------------------------------------------------
if (-not (Test-Path (Join-Path $build "SpeedLoader.asi"))) {
    throw "SpeedLoader.asi not found. Run build.bat first."
}

if (Get-Process SPEED2 -ErrorAction SilentlyContinue) {
    throw "the game is running: close SPEED2.EXE before installing the .asi " +
          "(or use -ModsOnly, which does not touch the binaries)"
}

Copy-Item (Join-Path $build "SpeedLoader.asi") $dest -Force
Copy-Item (Join-Path $build "SpeedLoaderHelper.exe") $runtime -Force

# ---- Chromium runtime ----------------------------------------------------
if (-not (Test-Path (Join-Path $cef "Release\libcef.dll"))) {
    throw "CEF is missing. Run tools\fetch_cef.ps1 first."
}

# Binaries (dll + .bin) and resources (.pak, icudtl.dat, locales\).
Get-ChildItem (Join-Path $cef "Release") -File |
    Where-Object { $_.Extension -in ".dll", ".bin", ".json" } |
    ForEach-Object { Copy-Item $_.FullName $runtime -Force }

Copy-Item (Join-Path $cef "Resources\*") $runtime -Recurse -Force

"[ok] SpeedLoader installed in $dest"
