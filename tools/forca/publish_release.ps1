# Forca Slicer - publish a release to the public repository (Windows).
#
# Forca is developed in a private repository; the public repository gets one commit per release whose tree is
# exactly the validated private state (docs/forca/release-procedure.md explains the model and the full checklist).
# Run the stages in order; each outward-facing stage is its own command, so nothing public happens by accident.
#
#   powershell -ExecutionPolicy Bypass -File tools/forca/publish_release.ps1 -Prepare
#       Local only. Builds the release commit on branch public/candidate (parent = public main, plus the upstream
#       OrcaSlicer commit when this release merged a newer one) and verifies it (identical tree, author, identity
#       checks). Prints a summary to review.
#   ... -Push       PUBLIC. Pushes public/candidate to the public main (only as a fast-forward of the current main).
#   ... -Build      Local. Clones the public main into -WorkDir and builds the Release zip + installer there, reusing
#                   the kept Release dependencies (-DepsDir) when deps/ is unchanged; -RebuildDeps rebuilds them
#                   into -DepsDir first (about 30 more minutes). Stages the renamed files, SHA256SUMS.txt and the
#                   filled-in release notes in <WorkDir>\release_upload.
#   ... -Release    PUBLIC. Creates the GitHub release v<version> (pre-release unless -Stable) from release_upload.
#
# Common options: -Version (default: FORCA_VERSION in version.inc), -Source (default: Multi_Interface).

[CmdletBinding()]
param(
    [switch]$Prepare,
    [switch]$Push,
    [switch]$Build,
    [switch]$Release,
    [string]$Version,
    [string]$Source = 'Multi_Interface',
    [string]$WorkDir = (Join-Path $env:USERPROFILE 'Documents\GitHub\ForcaRelease'),
    [string]$DepsDir = (Join-Path $env:USERPROFILE 'Documents\GitHub\ForcaCleanBuildTest\deps\build'),
    [switch]$RebuildDeps,
    [switch]$Stable
)

$ErrorActionPreference = 'Stop'
$repo       = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$publicRepo = 'tayloraaron078-tech/ForcaSlicer'
$publicUrl  = "https://github.com/$publicRepo.git"
$authorName = 'A.T. Creations'
$authorMail = '250606374+tayloraaron078-tech@users.noreply.github.com'
$candidate  = 'public/candidate'

function Say($msg)  { Write-Host "[release] $msg" }
function Fail($msg) { throw "[release] $msg" }
function GitOut {      # run git in the private repo; returns trimmed stdout, throws on failure
    $out = & git -C $repo @args
    if ($LASTEXITCODE -ne 0) { Fail "git $($args -join ' ') failed (exit $LASTEXITCODE)" }
    if ($out -is [array]) { return ($out -join "`n").Trim() } else { return "$out".Trim() }
}
function Public-Main {
    & git -C $repo fetch -q $publicUrl main
    if ($LASTEXITCODE -ne 0) { Fail "could not fetch the public main" }
    return (GitOut rev-parse FETCH_HEAD)
}

if (-not ($Prepare -or $Push -or $Build -or $Release)) { Fail 'Pick a stage: -Prepare, -Push, -Build or -Release.' }
if (-not $Version) {
    $m = Select-String -Path (Join-Path $repo 'version.inc') -Pattern 'set\(FORCA_VERSION\s+"(.+?)"'
    if (-not $m) { Fail 'FORCA_VERSION not found in version.inc' }
    $Version = $m.Matches[0].Groups[1].Value
}
$tag = "v$Version"
Say "version $Version (tag $tag)"

# ---- Prepare: build and verify the public commit locally -------------------------------------------------------
if ($Prepare) {
    $src = GitOut rev-parse $Source
    if ((GitOut rev-parse HEAD) -ne $src) { Fail "check out $Source first (identity checks run on the working tree)" }
    if (GitOut status --porcelain --untracked-files=no) { Fail 'the working tree has uncommitted changes to tracked files' }

    Say 'identity checks on the release tree...'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\forca\check_identity.ps1')
    if ($LASTEXITCODE -ne 0) { Fail 'check_identity.ps1 failed' }

    $pub = Public-Main
    if ((GitOut rev-parse "$src^{tree}") -eq (GitOut rev-parse "$pub^{tree}")) { Fail "the public main already has exactly this tree" }

    # The OrcaSlicer commit this release is built on: the newest upstream commit merged into the source.
    & git -C $repo rev-parse -q --verify upstream/main *> $null
    if ($LASTEXITCODE -ne 0) { Fail "no upstream/main ref (git fetch upstream)" }
    $up = GitOut merge-base $src upstream/main
    $parents = @('-p', $pub)
    & git -C $repo merge-base --is-ancestor $up $pub
    if ($LASTEXITCODE -ne 0) { $parents += @('-p', $up); Say "this release adds OrcaSlicer up to $($up.Substring(0,10)) (second parent)" }

    $msg = "Forca Slicer $Version`n`nSee CHANGELOG.md for what changed. Built on OrcaSlicer $($up.Substring(0,10))."
    $env:GIT_AUTHOR_NAME = $authorName; $env:GIT_AUTHOR_EMAIL = $authorMail
    $env:GIT_COMMITTER_NAME = $authorName; $env:GIT_COMMITTER_EMAIL = $authorMail
    try { $commit = ($msg | & git -C $repo commit-tree "$src^{tree}" @parents) }
    finally { Remove-Item Env:GIT_AUTHOR_NAME, Env:GIT_AUTHOR_EMAIL, Env:GIT_COMMITTER_NAME, Env:GIT_COMMITTER_EMAIL }
    if ($LASTEXITCODE -ne 0 -or -not $commit) { Fail 'git commit-tree failed' }
    GitOut branch -f $candidate $commit | Out-Null

    # Verify what we built.
    & git -C $repo diff --quiet $src $candidate
    if ($LASTEXITCODE -ne 0) { Fail 'candidate tree differs from the source' }
    $who = GitOut log -1 --format='%an <%ae>|%cn <%ce>' $candidate
    if ($who -ne "$authorName <$authorMail>|$authorName <$authorMail>") { Fail "unexpected identity: $who" }

    Say ''
    Say "candidate $candidate = $($commit.Substring(0,10))  (tree == $Source $($src.Substring(0,10)))"
    Say "parents: public main $($pub.Substring(0,10))$(if ($parents.Count -gt 2) { ", OrcaSlicer $($up.Substring(0,10))" })"
    Say "author/committer: $authorName <$authorMail>"
    Say 'changes against the current public main:'
    & git -C $repo diff --stat $pub $candidate | Select-Object -Last 1
    Say 'Review, then run -Push.'
}

# ---- Push: fast-forward the public main to the candidate ------------------------------------------------------------
if ($Push) {
    $cand = GitOut rev-parse $candidate
    $pub = Public-Main
    if ((GitOut rev-parse "$candidate^1") -ne $pub) { Fail "the public main moved since -Prepare (now $($pub.Substring(0,10))); run -Prepare again" }
    Say "pushing $($cand.Substring(0,10)) to $publicRepo main ..."
    & git -C $repo push $publicUrl "${candidate}:refs/heads/main"
    if ($LASTEXITCODE -ne 0) { Fail 'push failed' }
    if ((Public-Main) -ne $cand) { Fail 'the public main does not show the candidate after the push' }
    Say 'public main updated. Next: -Build.'
}

# ---- Build: clean clone of the public main -> Release zip + installer + upload set ----------------------------------
if ($Build) {
    if (Test-Path -LiteralPath $WorkDir) { Fail "$WorkDir exists; delete it first (it is only a build folder)" }
    $pub = Public-Main
    $depsKey  = GitOut rev-parse "${pub}:deps"
    $marker   = Join-Path $DepsDir 'forca_deps_source.txt'
    $depsOk   = (Test-Path -LiteralPath $marker) -and ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $depsKey)
    if (-not $depsOk -and -not $RebuildDeps) {
        Fail "the kept dependencies in $DepsDir were built from a different deps/ (or are missing); rerun with -RebuildDeps"
    }

    Say "cloning the public main into $WorkDir ..."
    & git clone -q --depth 1 --branch main $publicUrl $WorkDir
    if ($LASTEXITCODE -ne 0) { Fail 'clone failed' }
    $head = (& git -C $WorkDir rev-parse HEAD).Trim()
    if ($head -ne $pub) { Fail "clone is at $head, expected $pub" }

    $t0 = Get-Date
    $bat = Join-Path $WorkDir 'build_win.bat'           # always by full path: build_win.bat cds to its own folder
    if ($RebuildDeps) {
        Say "building dependencies into $DepsDir, then the slicer ..."
        & cmd /c "$bat" -d -s --config release --deps-dir "$DepsDir"
        if ($LASTEXITCODE -ne 0) { Fail "build failed (exit $LASTEXITCODE)" }
        Set-Content -LiteralPath $marker -Value $depsKey -Encoding ascii -NoNewline
    } else {
        Say "building the slicer with the kept dependencies ..."
        & cmd /c "$bat" -s --config release --deps-dir "$DepsDir"
        if ($LASTEXITCODE -ne 0) { Fail "build failed (exit $LASTEXITCODE)" }
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $WorkDir 'build_forca_release.ps1') -Config Release -Installer
    if ($LASTEXITCODE -ne 0) { Fail "packaging failed (exit $LASTEXITCODE)" }
    Say ("build + packaging took {0:N0} min" -f ((Get-Date) - $t0).TotalMinutes)

    # The app must embed the public commit (Troubleshoot links to it).
    $dll = Join-Path $WorkDir 'build\src\Release\ForcaSlicer.dll'
    if (-not (Select-String -LiteralPath $dll -SimpleMatch $pub.Substring(0,8) -Quiet)) { Fail "ForcaSlicer.dll does not embed $($pub.Substring(0,8))" }

    $up = Join-Path $WorkDir 'release_upload'
    New-Item -ItemType Directory -Path $up | Out-Null
    $setup = Join-Path $up "Forca_Slicer_${Version}_win64_setup.exe"
    $zip   = Join-Path $up "Forca_Slicer_${Version}_win64_portable.zip"
    Copy-Item -LiteralPath (Join-Path $WorkDir "build\ForcaSlicer_Windows_Installer_V${Version}_x64.exe") -Destination $setup
    $zips = @(Get-ChildItem -LiteralPath (Join-Path $WorkDir 'dist') -Filter '*_portable.zip')
    if ($zips.Count -ne 1) { Fail "expected one portable zip in dist, found $($zips.Count)" }
    Copy-Item -LiteralPath $zips[0].FullName -Destination $zip
    $hSetup = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLower()
    $hZip   = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLower()
    Set-Content -LiteralPath (Join-Path $up 'SHA256SUMS.txt') -Encoding ascii -Value @(
        "$hSetup  $(Split-Path $setup -Leaf)", "$hZip  $(Split-Path $zip -Leaf)")

    # Release notes: docs/forca/release/RELEASE_NOTES_<version>.md in the private repo (not committed; it is the
    # release page text). Placeholders: <DATE>, <SHA256_SETUP>, <SHA256_ZIP>. The first line (# title) is dropped.
    $notes = Join-Path $repo "docs\forca\release\RELEASE_NOTES_$Version.md"
    if (Test-Path -LiteralPath $notes) {
        $text = (Get-Content -LiteralPath $notes -Raw -Encoding utf8).Replace('<DATE>', (Get-Date -Format 'yyyy-MM-dd')).Replace('<SHA256_SETUP>', $hSetup).Replace('<SHA256_ZIP>', $hZip)
        $body = ($text -split "`r?`n" | Select-Object -Skip 1) -join "`n"
        [IO.File]::WriteAllText((Join-Path $up 'release_notes.md'), $body.TrimStart(), (New-Object Text.UTF8Encoding $false))
        if ($body -match '<[A-Z_0-9]{3,}>') { Say "WARNING: placeholders left in the release notes: $($Matches[0])" }
    } else { Say "WARNING: no $notes - write it before -Release" }

    Say "upload set ready in $up :"
    Get-ChildItem -LiteralPath $up | ForEach-Object { Say ("  {0,-50} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB)) }
    Say "SHA-256 setup $hSetup"
    Say "SHA-256 zip   $hZip"
    Say 'Smoke-test the installer and zip, then run -Release.'
}

# ---- Release: GitHub release from the upload set --------------------------------------------------------------------
if ($Release) {
    $up    = Join-Path $WorkDir 'release_upload'
    $body  = Join-Path $up 'release_notes.md'
    $files = @("Forca_Slicer_${Version}_win64_setup.exe", "Forca_Slicer_${Version}_win64_portable.zip", 'SHA256SUMS.txt') | ForEach-Object { Join-Path $up $_ }
    foreach ($f in @($body) + $files) { if (-not (Test-Path -LiteralPath $f)) { Fail "missing $f - run -Build first" } }
    $pub = Public-Main
    $built = (& git -C $WorkDir rev-parse HEAD).Trim()
    if ($built -ne $pub) { Fail "the upload set was built from $built but the public main is $pub" }

    $title = "Forca Slicer $Version"
    $ghArgs = @('release', 'create', $tag, '--repo', $publicRepo, '--target', $pub, '--title', $title, '--notes-file', $body)
    if (-not $Stable) { $ghArgs += '--prerelease' }
    Say "creating GitHub release $tag on $($pub.Substring(0,10)) ..."
    & gh @ghArgs @files
    if ($LASTEXITCODE -ne 0) { Fail 'gh release create failed' }

    # GitHub's own digests must match ours.
    $sums = @{}
    Get-Content -LiteralPath (Join-Path $up 'SHA256SUMS.txt') | ForEach-Object { $h, $n = $_ -split '\s+', 2; $sums[$n] = $h }
    $assets = & gh release view $tag --repo $publicRepo --json assets --jq '.assets[] | .name + " " + .digest'
    foreach ($a in $assets) {
        $n, $d = $a -split ' ', 2
        if ($sums.ContainsKey($n) -and $d -ne "sha256:$($sums[$n])") { Fail "digest mismatch for $n" }
    }
    Say "released: https://github.com/$publicRepo/releases/tag/$tag"
    Say "Private archive: tag the source commit and advance safe/known-good (each push needs the maintainer's OK)."
}
