# Stages a distributable build of SpeedLoader into release\.
#
# The release folder is local - it is in .gitignore and never committed.
#
#   .\release.ps1                     build if needed, stage and zip
#   .\release.ps1 -Version 1.0.0      name the package
#   .\release.ps1 -Mods tachometer    ship only these mods
#   .\release.ps1 -NoZip              leave the folder, skip the .zip
#   .\release.ps1 -Rebuild            build first, even if binaries exist
#   .\release.ps1 -NoAsiLoader        package without the .asi loader
#
# What the package looks like. Its contents go into the game folder, next to
# SPEED2.EXE, exactly as they are:
#
#   SpeedLoader-<version>\
#     INSTALL.txt
#     LICENSE.txt                 ours, plus CEF's and the loader's
#     dinput8.dll                 Ultimate ASI Loader - what loads the .asi
#     scripts\SpeedLoader.asi
#     scripts\SpeedLoader.ini
#     scripts\SpeedLoader\        Chromium runtime, helper, ui\ and mods\

param(
    [string]$Version = "0.1.0",
    [string[]]$Mods,
    [switch]$NoZip,
    [switch]$Rebuild,
    [switch]$NoAsiLoader
)

$ErrorActionPreference = "Stop"
$root   = $PSScriptRoot
$build  = Join-Path $root "build\Release"
$cef    = Join-Path $root "third_party\cef"
$loader = Join-Path $root "third_party\asi-loader"

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

# The .asi loader is what loads SpeedLoader.asi in the first place. Shipping it
# is the convention for NFSU2 mods - ExtraOptions does the same - and it saves
# the player a second download. -NoAsiLoader is for whoever already has one.
if (-not $NoAsiLoader -and -not (Test-Path (Join-Path $loader "dinput8.dll"))) {
    & (Join-Path $root "tools\fetch_asi_loader.ps1")
    if ($LASTEXITCODE -ne 0) { throw "could not fetch the ASI loader" }
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

if (-not $NoAsiLoader) {
    Copy-Item (Join-Path $loader "dinput8.dll") $stage -Force
}

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
# One LICENSE.txt for the whole package: ours, then the components we ship.
# Both of them are permissive, and both require their notice to travel along.
$parts = @(
    "SpeedLoader - LICENSE",
    "",
    "This package contains SpeedLoader, and third-party components that keep",
    "their own licenses. Each one is reproduced in full below.",
    "",
    ("=" * 76),
    "1. SpeedLoader",
    ("=" * 76),
    "",
    (Get-Content (Join-Path $root "LICENSE") -Raw).TrimEnd(),
    "",
    ("=" * 76),
    "2. Chromium Embedded Framework (CEF) and Chromium",
    ("=" * 76),
    "",
    (Get-Content (Join-Path $cef "LICENSE.txt") -Raw).TrimEnd()
)

# QuickJS is linked into SpeedLoader.asi, so its notice travels too.
$quickjs = Join-Path $root "build\_deps\quickjs-src\LICENSE"
if (Test-Path $quickjs) {
    $parts += @(
        "",
        ("=" * 76),
        "3. QuickJS, by Fabrice Bellard and Charlie Gordon (linked into the .asi)",
        ("=" * 76),
        "",
        (Get-Content $quickjs -Raw).TrimEnd()
    )
}

if (-not $NoAsiLoader) {
    $parts += @(
        "",
        ("=" * 76),
        "4. Ultimate ASI Loader (dinput8.dll), by ThirteenAG",
        ("=" * 76),
        "   https://github.com/ThirteenAG/Ultimate-ASI-Loader",
        "",
        (Get-Content (Join-Path $loader "LICENSE.txt") -Raw).TrimEnd()
    )
}

($parts -join "`r`n") | Set-Content (Join-Path $stage "LICENSE.txt") -Encoding utf8

$shipped = ($sources.Name | Sort-Object) -join ", "
$loaderLines = if ($NoAsiLoader) {
@"
  You need an .asi loader already installed - this package does not bring one.
"@
} else {
@"
  dinput8.dll is Ultimate ASI Loader, by ThirteenAG. It is what loads the
  .asi at startup. If you already have an .asi loader (dinput8.dll, dsound.dll,
  vorbisFile.dll, ...) in the game folder, keep yours and skip this file.
"@
}

@"
SpeedLoader $Version
A modding platform for NFS Underground 2 (SPEED2.EXE v1.2 NTSC, 4,800,512 bytes).

INSTALL

  1. Close the game.
  2. Copy everything in this folder into your NFSU2 folder, next to SPEED2.EXE,
     and let "scripts" merge with the one already there.
  3. Start the game.

$loaderLines

  Upgrading: your scripts\SpeedLoader.ini is yours - keep it, and compare it
  with the one in this package if a new key shows up.

KEYS

  F1   hands keyboard and mouse to the UI, and back to the game
       (configurable in SpeedLoader.ini)

WHAT IS IN HERE

  dinput8.dll                    the .asi loader (Ultimate ASI Loader)
  scripts\SpeedLoader.asi        SpeedLoader itself
  scripts\SpeedLoader.ini        configuration
  scripts\SpeedLoader\           Chromium runtime and the helper process
  scripts\SpeedLoader\ui\        the shell that mounts each mod's UI
  scripts\SpeedLoader\mods\      $shipped

TROUBLE

  scripts\SpeedLoader.log is the first place to look; Chromium's own log is
  SpeedLoaderCef.log next to it. Most surprises are a conflict with another
  .asi in the main loop - say which ones you have when reporting a problem.

LICENSE

  SpeedLoader by Chrystian Farias
  https://github.com/chrystianfarias/SpeedLoader

  CC BY-NC 4.0 - Copyright (c) 2025 Chrystian Farias. See LICENSE.
  Redistribution is fine with credit, and not for commercial purposes.
  CEF/Chromium and Ultimate ASI Loader keep their own licenses. All of them
  are in LICENSE.txt, in full.

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
