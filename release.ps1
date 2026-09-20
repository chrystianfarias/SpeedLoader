# Stages a distributable build of SpeedLoader into release\.
#
# The release folder is local — it is in .gitignore and never committed.
#
#   .\release.ps1                     build if needed, stage and zip
#   .\release.ps1 -Version 1.0.0      name the package
#   .\release.ps1 -Mods tachometer    ship only these mods
#   .\release.ps1 -NoZip              leave the folder, skip the .zip
#   .\release.ps1 -Rebuild            build first, even if binaries exist
#
# What the package looks like (drop `scripts` into the game folder):
#
#   SpeedLoader-<version>\
#     INSTALL.txt
#     LICENSE
#     scripts\SpeedLoader.asi
#     scripts\SpeedLoader.ini
#     scripts\SpeedLoader\        Chromium runtime, helper, ui\ and mods\

param(
    [string]$Version = "0.1.0",
    [string[]]$Mods,
    [switch]$NoZip,
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build\Release"
$cef   = Join-Path $root "third_party\cef"

# ---- build ---------------------------------------------------------------
$asi = Join-Path $build "SpeedLoader.asi"
if ($Rebuild -or -not (Test-Path $asi)) {
    "[..] building"
    & (Join-Path $root "build.bat") nomod
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
if (-not (Test-Path $asi)) { throw "SpeedLoader.asi not found. Run build.bat first." }
if (-not (Test-Path (Join-Path $cef "Release\libcef.dll"))) {
    throw "CEF is missing. Run tools\fetch_cef.ps1 first."
}

# ---- a clean staging tree ------------------------------------------------
$name    = "SpeedLoader-$Version"
$stage   = Join-Path $root "release\$name"
$scripts = Join-Path $stage "scripts"
$runtime = Join-Path $scripts "SpeedLoader"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runtime | Out-Null

# ---- binaries ------------------------------------------------------------
Copy-Item $asi $scripts -Force
Copy-Item (Join-Path $build "SpeedLoaderHelper.exe") $runtime -Force
Copy-Item (Join-Path $root "SpeedLoader.ini") $scripts -Force

# ---- Chromium runtime ----------------------------------------------------
# Binaries (dll + .bin + .json) and resources (.pak, icudtl.dat, locales\).
Get-ChildItem (Join-Path $cef "Release") -File |
    Where-Object { $_.Extension -in ".dll", ".bin", ".json" } |
    ForEach-Object { Copy-Item $_.FullName $runtime -Force }

Copy-Item (Join-Path $cef "Resources\*") $runtime -Recurse -Force

# ---- UI shell and mods ---------------------------------------------------
Copy-Item (Join-Path $root "ui") $runtime -Recurse -Force

$modsOut = Join-Path $runtime "mods"
New-Item -ItemType Directory -Force -Path $modsOut | Out-Null

$sources = Get-ChildItem (Join-Path $root "mods") -Directory
if ($Mods) {
    $sources = $sources | Where-Object { $Mods -contains $_.Name }
    $missing = $Mods | Where-Object { $sources.Name -notcontains $_ }
    if ($missing) { throw "mod not found: $($missing -join ', ')" }
}
if (-not $sources) { throw "no mods to ship" }
$sources | ForEach-Object { Copy-Item $_.FullName $modsOut -Recurse -Force }

# ---- paperwork -----------------------------------------------------------
Copy-Item (Join-Path $root "LICENSE") $stage -Force
Copy-Item (Join-Path $cef "LICENSE.txt") (Join-Path $stage "LICENSE-CEF.txt") -Force

$shipped = ($sources.Name | Sort-Object) -join ", "
@"
SpeedLoader $Version
A modding platform for NFS Underground 2 (SPEED2.EXE v1.2 NTSC, 4,800,512 bytes).

INSTALL

  1. Close the game.
  2. Copy the "scripts" folder into your NFSU2 folder, next to SPEED2.EXE,
     and let it merge with the one already there.
  3. You need an .asi loader installed (the same one other NFSU2 mods use).
  4. Start the game.

  Upgrading: your scripts\SpeedLoader.ini is yours — keep it, and compare it
  with the one in this package if a new key shows up.

KEYS

  F1   hands keyboard and mouse to the UI, and back to the game
       (configurable in SpeedLoader.ini)

WHAT IS IN HERE

  scripts\SpeedLoader.asi        the loader
  scripts\SpeedLoader.ini        configuration
  scripts\SpeedLoader\           Chromium runtime and the helper process
  scripts\SpeedLoader\ui\        the shell that mounts each mod's UI
  scripts\SpeedLoader\mods\      $shipped

TROUBLE

  scripts\SpeedLoader.log is the first place to look; Chromium's own log is
  SpeedLoaderCef.log next to it. Most surprises are a conflict with another
  .asi in the main loop — say which ones you have when reporting a problem.

LICENSE

  CC BY-NC 4.0 - Copyright (c) 2025 Chrystian Farias. See LICENSE.
  Redistribution is fine with credit, and not for commercial purposes.
  CEF/Chromium keeps its own license, in LICENSE-CEF.txt.

  Not affiliated with Electronic Arts. No game file is distributed here.
"@ | Set-Content (Join-Path $stage "INSTALL.txt") -Encoding utf8

# ---- zip -----------------------------------------------------------------
$size = "{0:N1} MB" -f ((Get-ChildItem $stage -Recurse -File |
    Measure-Object Length -Sum).Sum / 1MB)

if (-not $NoZip) {
    $zip = Join-Path $root "release\$name.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
    "[ok] release\$name.zip ($size unpacked, mods: $shipped)"
} else {
    "[ok] release\$name ($size, mods: $shipped)"
}
