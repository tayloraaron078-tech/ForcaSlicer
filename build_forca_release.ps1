# [regional-supports fork] Forca Slicer - local release packager (Windows).
#
# Assembles the runnable app (exe + DLLs + resources) into a dated, ready-to-share portable .zip so you can
# hand a build to testers WITHOUT any cloud CI / GitHub Actions minutes. Optionally builds first and/or emits
# an NSIS installer.
#
# Usage (from the repo root, or via build_forca_release.bat):
#   powershell -ExecutionPolicy Bypass -File build_forca_release.ps1
#       Package the EXISTING RelWithDebInfo build (build-dbginfo). Runs the fast 'install' step if needed,
#       then zips it. This is the quick path right after your normal dev build.
#   ... -Config Release            Package/produce a leaner Release build (build\) instead.
#   ... -Build                     Build the slicer first (build_win.bat -s; the deps in deps\build must exist).
#   ... -Installer                 Also produce an NSIS installer via cpack (needs NSIS installed).
#
# The app ships as ForcaSlicer\forca-slicer.exe. Internal identities that users never see (OrcaSlicer.conf, the
# OrcaSlicer.mo translations, the .3mf "Application" tag) stay OrcaSlicer so projects and profiles stay compatible.

[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo','Release')]
    [string]$Config = 'RelWithDebInfo',
    [switch]$Build,
    [switch]$Installer
)

$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot

$buildDir = if ($Config -eq 'Release') { Join-Path $repo 'build' } else { Join-Path $repo 'build-dbginfo' }
$appDir   = Join-Path $buildDir 'ForcaSlicer'         # installed here explicitly (--prefix), whatever the cache says
$outDir   = Join-Path $repo 'dist'
$sevenzip = Join-Path $repo 'tools\7z.exe'

Write-Host "[forca] Config: $Config   build dir: $buildDir"

if (-not (Test-Path $sevenzip)) { throw "Bundled 7-Zip not found at $sevenzip." }

# 1) Optional full build (deps + slicer). Long-running.
if ($Build) {
    Write-Host '[forca] Full build starting (this can take a long time, especially the first deps build)...'
    # Both use build_win.bat (OrcaSlicer dropped build_release_vs2022.bat in 2026-09); both share deps\build.
    $cfgArg = if ($Config -eq 'Release') { 'release' } else { 'relwithdebinfo' }
    & cmd /c "`"$repo\build_win.bat`" -s --config $cfgArg -i"
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }
}

if (-not (Test-Path $buildDir)) {
    throw "Build dir '$buildDir' not found. Build the slicer first (e.g. build_win.bat -s --config relwithdebinfo), or re-run with 'build'."
}

# 2) (Re)assemble the runnable install tree (exe + DLLs + resources). Always run: it only COPIES changed files from
#    the already-built tree (no recompile), and skipping it when the folder existed zipped stale binaries.
Write-Host '[forca] Assembling the runnable app (install)...'
& cmake --install $buildDir --config $Config --prefix $appDir
if ($LASTEXITCODE -ne 0) { throw "Install failed (exit $LASTEXITCODE). Make sure the slicer is built first." }
if (-not (Test-Path $appDir)) { throw "Install tree still missing at $appDir." }

# 3) Build a stamp: display version (version.inc) + date + short git hash.
$ver = 'dev'
$m = Select-String -Path (Join-Path $repo 'version.inc') -Pattern 'set\(FORCA_VERSION\s+"(.+?)"'
if ($m) { $ver = $m.Matches[0].Groups[1].Value }
$date = Get-Date -Format 'yyyyMMdd'
$hash = 'nogit'
try {
    $h = (& git -C $repo rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $h) { $hash = $h.Trim() }
} catch { }
$name = "Forca_Slicer_V${ver}_${date}_${hash}_win64"

# 4) Zip the ForcaSlicer folder (stored at the zip root, matching the CI portable layout).
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$zip = Join-Path $outDir "${name}_portable.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }

Write-Host '[forca] Packaging portable zip...'
Push-Location $buildDir
try {
    & $sevenzip a -tzip $zip 'ForcaSlicer' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip packaging failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}

# 5) Optional NSIS installer.
if ($Installer) {
    Write-Host '[forca] Building NSIS installer via cpack (needs NSIS on PATH)...'
    Push-Location $buildDir
    try { & cpack -G NSIS } finally { Pop-Location }
}

$zipInfo = Get-Item $zip
Write-Host ''
Write-Host '[forca] DONE.'
Write-Host ("[forca] Portable zip: {0}  ({1:N1} MB)" -f $zip, ($zipInfo.Length / 1MB))
Write-Host '[forca] Testers unzip it and launch forca-slicer.exe inside the ForcaSlicer folder.'
if ($Config -eq 'RelWithDebInfo') {
    Write-Host '[forca] Note: RelWithDebInfo carries debug info (larger). For a leaner share build: build_forca_release.bat release build'
}
