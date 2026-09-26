#Requires -Version 5.1
<#
.SYNOPSIS
    Re-applies the Forca Slicer blue retint to OrcaSlicer's GUI chrome colors.

.DESCRIPTION
    Forca recolors OrcaSlicer's grey interface chrome to a blue palette. Rather than
    carrying ~20 scattered hex edits across ~77 files as permanent source changes that
    fight every upstream merge, this script REGENERATES the retint from the base Orca
    colors. Run it on a freshly-merged upstream tree (after taking upstream's version of
    conflicted chrome files) to re-apply the Forca blue in one step.

    The color mapping lives in tools/forca/retint_map.tsv and the exclude list in
    tools/forca/retint_exclude.txt — those two files are the canonical source of truth
    (see docs/forca/retint.md). The script rewrites each grey #RRGGBB hex literal to its
    Forca-blue counterpart across GUI source and the webview CSS.

    It is IDEMPOTENT (running twice changes nothing the second time) and ORDER-INDEPENDENT
    (no new color equals any old key, so there is no accidental chaining).

.PARAMETER DryRun
    Report what WOULD change, per file, without writing anything.

.PARAMETER RepoRoot
    Repository root. Defaults to two levels up from this script (tools/forca/ -> repo root).

.EXAMPLE
    powershell -File tools/forca/apply_retint.ps1 -DryRun
    powershell -File tools/forca/apply_retint.ps1

.NOTES
    EXCLUDED files (tools/forca/retint_exclude.txt) are NOT touched:
      - BitmapCache.cpp : holds original Orca hexes as SVG recolor-table KEYS. Retinting
                          them silently breaks dark-mode icon recoloring. NEVER include it.
      - GLCanvas3D.cpp / Tab.cpp / GUI_App.cpp / BBLTopbar.cpp / MainFrame.cpp / Notebook.cpp
                          : hand-edited (decimal RGB, theme-aware panel, splash art). After a
                          merge that touches these, re-apply their edits manually — see
                          docs/forca/upstream-merge.md. They carry no chrome hex literals for
                          this script to manage, so it leaves them alone.
#>
[CmdletBinding()]
param(
    [switch]$DryRun,
    [string]$RepoRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
}

# --- Load canonical mapping + exclude list. ------------------------------------------
$mapFile = Join-Path $scriptDir 'retint_map.tsv'
$excFile = Join-Path $scriptDir 'retint_exclude.txt'
if (-not (Test-Path $mapFile)) { throw "Missing mapping file: $mapFile" }

$map = [ordered]@{}
foreach ($line in Get-Content -Path $mapFile) {
    # A mapping line is exactly "#RRGGBB<ws>#RRGGBB"; everything else (incl. # comments) is ignored.
    if ($line -match '^\s*(#[0-9A-Fa-f]{6})\s+(#[0-9A-Fa-f]{6})\s*$') {
        $map[$matches[1]] = $matches[2]
    }
}
if ($map.Count -eq 0) { throw "No mappings parsed from $mapFile" }

$excludeNames = @()
if (Test-Path $excFile) {
    $excludeNames = Get-Content -Path $excFile |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -ne '' -and -not $_.StartsWith('#') }
}

# --- Target file set: all GUI cpp/hpp + the webview CSS, minus excludes. -------------
$guiDir = Join-Path $RepoRoot 'src/slic3r/GUI'
if (-not (Test-Path $guiDir)) { throw "GUI dir not found: $guiDir  (is -RepoRoot correct?)" }

$files = @()
$files += Get-ChildItem -Path $guiDir -Recurse -File -Include '*.cpp', '*.hpp' |
    Where-Object { $excludeNames -notcontains $_.Name }
foreach ($css in @(
        'resources/web/homepage/css/dark.css',
        'resources/web/homepage/css/home.css',
        'resources/web/include/global.css')) {
    $p = Join-Path $RepoRoot $css
    if ((Test-Path $p) -and ($excludeNames -notcontains (Split-Path $p -Leaf))) { $files += Get-Item $p }
}

# --- Apply. --------------------------------------------------------------------------
$totalHits = 0
$changedFiles = 0
foreach ($f in $files) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $enc = New-Object System.Text.UTF8Encoding($hasBom)
    $start = if ($hasBom) { 3 } else { 0 }
    $text = $enc.GetString($bytes, $start, $bytes.Length - $start)

    $orig = $text
    $fileHits = 0
    foreach ($old in $map.Keys) {
        $new = $map[$old]
        # Match the hex only when not part of a longer hex token (e.g. #262E30FF).
        $pat = '(?<![0-9A-Fa-f])' + [regex]::Escape($old) + '(?![0-9A-Fa-f])'
        $rx = [regex]::new($pat, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        $m = $rx.Matches($text)
        if ($m.Count -gt 0) {
            $fileHits += $m.Count
            $text = $rx.Replace($text, $new)
        }
    }

    if ($fileHits -gt 0) {
        $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart('\', '/')
        Write-Host ("  {0,4}  {1}" -f $fileHits, $rel)
        $totalHits += $fileHits
        $changedFiles++
        if (-not $DryRun -and $text -ne $orig) {
            [System.IO.File]::WriteAllText($f.FullName, $text, $enc)
        }
    }
}

Write-Host ''
if ($DryRun) {
    Write-Host ("DRY RUN: {0} hex replacement(s) across {1} file(s) would be applied." -f $totalHits, $changedFiles)
} else {
    Write-Host ("Applied {0} hex replacement(s) across {1} file(s)." -f $totalHits, $changedFiles)
}
Write-Host 'Reminder: hand-edited files are NOT touched by this script — see docs/forca/upstream-merge.md.'
Write-Host 'Verify BitmapCache.cpp recolor keys are intact:  git diff -- src/slic3r/GUI/BitmapCache.cpp   (should be empty)'
