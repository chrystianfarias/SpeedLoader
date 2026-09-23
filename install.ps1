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
# Cada mod passa pelo modcheck antes de ir para o jogo: uma funcao chamada e
# inexistente so aparece quando alguem clica no botao que passa por ela, e ai
# ja custou uma partida. Sem node instalado, segue em frente.
if ((Get-Command node -ErrorAction SilentlyContinue) -and
    (Test-Path (Join-Path $root "mods"))) {
    $check = Join-Path $root "tools\modcheck.js"
    foreach ($mod in Get-ChildItem (Join-Path $root "mods") -Directory) {
        $main = Join-Path $mod.FullName "main.js"
        if (-not (Test-Path $main)) { continue }
        $saida = & node $check $main 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "$($mod.Name): $saida"
        }
    }
}

foreach ($folder in "ui", "mods") {
    $source = Join-Path $root $folder
    # mods\ is not in the repository: a fresh clone has the platform and no
    # mods, and that installs perfectly well.
    if (-not (Test-Path $source)) { continue }

    $target = Join-Path $runtime $folder
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Copy-Item $source $runtime -Recurse -Force
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

# O plugin nativo: o .asi vai ao lado do loader e a pagina dele numa pasta com
# o seu nome, que e onde panel_open("SpawnCar\ui.html") procura.
if (Test-Path (Join-Path $build "SpawnCar.asi")) {
    Copy-Item (Join-Path $build "SpawnCar.asi") $dest -Force
    $spawnDir = Join-Path $dest "SpawnCar"
    New-Item -ItemType Directory -Force -Path $spawnDir | Out-Null
    Copy-Item (Join-Path $root "plugins\spawn-car\ui.html") $spawnDir -Force
}

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
