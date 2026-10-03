# Forca Slicer - write Forca's own interface translations into the gettext catalogs (Windows).
#
# Forca-only strings (text Forca reworded or added) and their translations live in tools/forca/forca_translations*.json
# (forca_translations.json: Orca wording Forca changed; forca_translations_wizard.json: the Calibration Wizard, whose
# sources are not in localization/i18n/list.txt). This script puts each one into
# localization/i18n/<lang>/OrcaSlicer_<lang>.po, marked "# AI Translated":
#   - an existing entry with the same msgid gets the translation (and loses a "fuzzy" flag);
#   - otherwise the entry is added right after the Orca entry named by "after" (the wording it replaced), or before the
#     obsolete (#~) entries when there is no "after" or that anchor is gone.
# Then msgfmt --check-format validates every catalog it changed. Rerun it after an upstream merge (take upstream's
# .po files, then run this). -Check only reports (exit 1 if any catalog lacks a translation); check_identity.ps1 uses it.
#
#   powershell -ExecutionPolicy Bypass -File tools/forca/apply_forca_translations.ps1 [-Check]

[CmdletBinding()]
param([switch]$Check)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$entries = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'forca_translations*.json' | Sort-Object Name | ForEach-Object {
    (Get-Content -LiteralPath $_.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).entries })
$utf8 = New-Object Text.UTF8Encoding $false
$eol  = "`r`n"

function PoEscape([string]$s) { $s.Replace('\', '\\').Replace('"', '\"').Replace("`n", '\n').Replace("`t", '\t') }
function Short([string]$s) { if ($s.Length -gt 40) { $s.Substring(0, 40) + '...' } else { $s } }

$missing = 0; $changedFiles = @()
foreach ($lang in ($entries | ForEach-Object { $_.translations.PSObject.Properties.Name } | Sort-Object -Unique)) {
    $po = Join-Path $repo "localization\i18n\$lang\OrcaSlicer_$lang.po"
    if (-not (Test-Path -LiteralPath $po)) { Write-Host "[translations] no catalog for '$lang'"; $missing++; continue }
    $lines = [Collections.Generic.List[string]]::new([IO.File]::ReadAllText($po, $utf8) -split "`r?`n")
    $changed = $false

    $tail = [Collections.Generic.List[string]]::new() # new entries without an anchor, added in one block
    foreach ($e in $entries) {
        $tr = $e.translations.$lang
        if (-not $tr) { continue }
        $idLine  = 'msgid "' + (PoEscape $e.msgid) + '"'
        $strLine = 'msgstr "' + (PoEscape $tr) + '"'
        # An obsolete (#~) entry with the same msgid would duplicate ours (msgfmt fails): drop it, comments included.
        $o = $lines.IndexOf('#~ ' + $idLine)
        if ($o -gt 0 -and -not $lines[$o - 1].StartsWith('#~ msgctxt') -and -not $Check) {
            $end = $o; while ($end -lt $lines.Count -and $lines[$end] -ne '') { $end++ }
            $begin = $o; while ($begin -gt 0 -and $lines[$begin - 1].StartsWith('#') -and -not $lines[$begin - 1].StartsWith('#~')) { $begin-- }
            $lines.RemoveRange($begin, [Math]::Min($end + 1, $lines.Count) - $begin)
            $changed = $true
        }
        $i = $lines.IndexOf($idLine)
        while ($i -gt 0 -and $lines[$i - 1].StartsWith('msgctxt')) { $i = $lines.IndexOf($idLine, $i + 1) } # not a homonym
        if ($i -ge 0) {
            # Existing entry: its msgstr (plus continuation lines) must be ours, with no fuzzy flag.
            $j = $i + 1
            $k = $j + 1
            while ($k -lt $lines.Count -and $lines[$k].StartsWith('"')) { $k++ }
            $ok = ($lines[$j] -eq $strLine) -and ($k -eq $j + 1)
            $start = $i; while ($start -gt 0 -and $lines[$start - 1].StartsWith('#')) { $start-- }
            $comments = @(); for ($c = $start; $c -lt $i; $c++) { $comments += $lines[$c] }
            $fuzzy = @($comments | Where-Object { $_ -match '^#,.*\bfuzzy\b' }).Count -gt 0
            $marked = $comments -contains '# AI Translated'
            if ($ok -and -not $fuzzy -and $marked) { continue }
            if ($Check) { Write-Host "[translations] $lang : outdated '$(Short $e.msgid)'"; $missing++; continue }
            $keep = @($comments | Where-Object { $_ -notmatch '^#,.*\bfuzzy\b' -and $_ -ne '# AI Translated' })
            $lines.RemoveRange($start, $k - $start)
            $lines.InsertRange($start, [string[]](@('# AI Translated') + $keep + @($idLine, $strLine)))
            $changed = $true
            continue
        }
        if ($Check) { Write-Host "[translations] $lang : missing '$(Short $e.msgid)'"; $missing++; continue }

        # New entry: after the block of the Orca wording it replaced (its msgid may continue over several "..." lines),
        # else (later, in one block) just before the obsolete (#~) entries.
        $at = -1
        if ($e.after) {
            $anchor = PoEscape $e.after
            for ($a = 0; $a -lt $lines.Count -and $at -lt 0; $a++) {
                if (-not $lines[$a].StartsWith('msgid "')) { continue }
                $full = $lines[$a].Substring(7).TrimEnd('"')
                for ($b = $a + 1; $b -lt $lines.Count -and $lines[$b].StartsWith('"'); $b++) { $full += $lines[$b].Substring(1).TrimEnd('"') }
                if ($full.StartsWith($anchor)) { $at = $a; while ($at -lt $lines.Count -and $lines[$at] -ne '') { $at++ } }
            }
        }
        if ($at -ge 0) {
            $lines.InsertRange($at, [string[]]@('', '# AI Translated', $idLine, $strLine))
        } else {
            $tail.AddRange([string[]]@('# AI Translated', $idLine, $strLine, ''))
        }
        $changed = $true
    }
    if ($tail.Count -gt 0) {
        $at = $lines.FindIndex([Predicate[string]]{ param($l) $l.StartsWith('#~') })
        if ($at -lt 0) {
            $lines.Add('')
            $lines.AddRange($tail)
        } else {
            while ($at -gt 0 -and $lines[$at - 1].StartsWith('#') -and -not $lines[$at - 1].StartsWith('#~')) { $at-- } # its comments
            $lines.InsertRange($at, $tail)
        }
    }

    if ($changed) {
        [IO.File]::WriteAllText($po, ($lines -join $eol), $utf8)
        $changedFiles += $po
    }
}

if ($Check) {
    if ($missing -gt 0) { Write-Host "[translations] $missing Forca translation(s) missing or outdated - run tools/forca/apply_forca_translations.ps1"; exit 1 }
    Write-Host "[translations] all Forca translations present"; exit 0
}

$msgfmt = Join-Path $repo 'tools\msgfmt.exe'
$bad = 0
foreach ($po in $changedFiles) {
    $mo = [IO.Path]::GetTempFileName()
    & $msgfmt --check-format -o $mo $po
    if ($LASTEXITCODE -ne 0) { Write-Host "[translations] msgfmt FAILED: $po"; $bad++ }
    Remove-Item -LiteralPath $mo -Force -ErrorAction SilentlyContinue
}
Write-Host "[translations] updated $($changedFiles.Count) catalog(s); msgfmt failures: $bad"
if ($bad -gt 0) { exit 1 }
