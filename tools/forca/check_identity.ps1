#Requires -Version 5.1
<#
.SYNOPSIS
    Checks that Forca's identity changes survived an upstream OrcaSlicer merge.

.DESCRIPTION
    Forca renames itself in many small places: its own settings folder, exe/DLL and installer names, file
    associations, the product name in the interface (forca_brand / ForcaBrand), dialog titles, and icon/logo
    files. An upstream merge can silently undo any of them. This script is the executable list of those
    changes: every FAIL is something the merge lost and must be restored (see docs/forca/upstream-merge.md).

    It also lists untranslated string literals in src/slic3r that still mention Orca ("REVIEW"). Translated
    strings (_L, _u8L, ...) are renamed automatically at runtime and are not listed; a listed literal is only
    a problem if it is shown to the user.

    Exit code 0 = all checks pass, 1 = at least one FAIL.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/forca/check_identity.ps1
#>
param([string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path)

$ErrorActionPreference = 'Stop'
$OrcaBase = 'c0c2cc5068'   # Forca's pinned OrcaSlicer base; update when an upstream merge moves it.
$fails = 0

function Fail([string]$msg) { Write-Host "FAIL  $msg" -ForegroundColor Red; $script:fails++ }
function Read-Repo([string]$rel) { [IO.File]::ReadAllText((Join-Path $RepoRoot $rel)) }

# 1) Code markers that must be present (file -> literal text).
$must = @(
    @('version.inc',                                'set(SLIC3R_APP_NAME "Forca Slicer")'),
    @('version.inc',                                'set(SLIC3R_APP_KEY "OrcaSlicer")'),      # must STAY Orca (config file, translations, .3mf)
    @('src/slic3r/GUI/GUI_App.cpp',                 'FORCA_DATA_DIR_NAME = "ForcaSlicer"'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'SetAppName(FORCA_DATA_DIR_NAME)'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'forca_copy_orca_data_dir(s_forca_orca_data_dir'),
    @('src/slic3r/GUI/GUI_App.cpp',                 '#define FORCA_PROG_ID L"Forca.Slicer.1"'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'libslic3r_translate_callback(const char *s) { return _u8L(s); }'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'SLIC3R_APP_NAME, wxICON_QUESTION | wxYES_NO); // Forca'),
    @('src/slic3r/GUI/Preferences.cpp',             'create_item_link_association(L"orcaslicer"'),
    @('src/slic3r/GUI/I18N.hpp',                    'return forca_brand(s, wxGetTranslation(s)); }'),
    @('src/slic3r/GUI/I18N.cpp',                    'wxString forca_brand(const wxString &source, const wxString &translated)'),
    @('tests/slic3rutils/CMakeLists.txt',           'test_forca_brand.cpp'),
    @('resources/web/data/text.js',                 'function ForcaBrand(s)'),
    @('src/CMakeLists.txt',                         'OUTPUT_NAME "forca-slicer"'),
    @('src/CMakeLists.txt',                         'set_target_properties(OrcaSlicer PROPERTIES OUTPUT_NAME "ForcaSlicer")'),
    @('src/OrcaSlicer_app_msvc.cpp',                'L"ForcaSlicer.dll"'),
    @('src/slic3r/Utils/Process.cpp',               '"forca-slicer.exe"'),
    @('src/slic3r/Utils/BBLNetworkPlugin.cpp',      'find("forca-slicer.exe")'),
    @('CMakeLists.txt',                             'set (CPACK_PACKAGE_INSTALL_REGISTRY_KEY "ForcaSlicer")'),
    @('CMakeLists.txt',                             'set (CPACK_PACKAGE_NAME "Forca Slicer")'),
    @('CMakeLists.txt',                             '"${CMAKE_BINARY_DIR}/ForcaSlicer"'),
    @('src/slic3r/GUI/Plater.cpp',                  'SetTitle(title + " - " SLIC3R_APP_NAME)'),
    @('src/slic3r/GUI/Plater.cpp',                  'SetTitle(m_project_name + " - " SLIC3R_APP_NAME)'),
    @('src/slic3r/GUI/MainFrame.cpp',               'wxBITMAP_TYPE_ICO), SLIC3R_APP_NAME)'),
    @('src/slic3r/GUI/WebGuideDialog.cpp',          'wxID_ANY, SLIC3R_APP_NAME, wxDefaultPosition'),
    @('src/slic3r/GUI/WebUserLoginDialog.cpp',      'wxID_ANY, SLIC3R_APP_NAME)'),
    @('src/slic3r/GUI/WebDownPluginDlg.cpp',        'wxID_ANY, SLIC3R_APP_NAME)'),
    @('src/slic3r/GUI/HttpServer.cpp',              'You can return to Forca Slicer'),
    @('src/slic3r/GUI/WipeTowerDialog.cpp',         'Forca would re-calculate'),
    @('resources/web/flush/WipingDialog.html',      'Forca would re-calculate'),
    @('src/slic3r/plugin/host/PluginHostUi.cpp',    'py::arg("title") = "Forca Slicer"'),
    @('src/slic3r/Config/Snapshot.cpp',             'SLIC3R_APP_NAME " was unable to create a directory at "'),
    @('src/slic3r/GUI/MsgDialog.cpp',               '"OrcaSlicer_192px.png" /* Forca logo'),
    @('src/slic3r/GUI/ReleaseNote.cpp',             'create_scaled_bitmap("OrcaSlicer_192px.png"'),
    @('src/slic3r/GUI/UpdateDialogs.cpp',           'create_scaled_bitmap("OrcaSlicer_192px.png"'),
    @('src/slic3r/GUI/SysInfoDialog.cpp',           'ScalableBitmap(this, "OrcaSlicer_192px.png", 192)'),
    @('src/slic3r/GUI/TroubleshootDialog.cpp',      '"ForcaSlicer_Logs_"'),
    @('src/slic3r/GUI/AboutDialog.cpp',             'wxString::FromUTF8("Forca Slicer © 2026 A.T. Creations, licensed under the GNU AGPL-3.0. "'),
    @('src/slic3r/GUI/AboutDialog.cpp',             '_u8L("Source code") + ": <a style=\"color:#009789\" href=\"" + FORCA_REPO_URL'),
    @('src/slic3r/GUI/AboutDialog.cpp',             '_L("The complete source code of this version of Forca Slicer is available at")'),
    # B9: Orca Cloud switched off
    @('src/slic3r/Utils/ForcaFeatures.hpp',         'constexpr bool FORCA_ORCA_CLOUD_ENABLED = false;'),
    @('src/slic3r/Utils/OrcaCloudServiceAgent.cpp', 'return BAMBU_NETWORK_SUCCESS; // Forca: never restore an Orca Cloud login'),
    @('src/slic3r/Utils/OrcaCloudServiceAgent.cpp', 'return false; // Forca: Orca Cloud is off'),
    @('src/slic3r/Utils/OrcaCloudServiceAgent.cpp', 'return; // Forca: deleting it would sign the user out of stock OrcaSlicer'),
    @('src/slic3r/Utils/OrcaCloudServiceAgent.cpp', 'return false; // Forca: see persist_user_secret'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'forca_merge_account_presets_into_default(app_config);'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'if (FORCA_ORCA_CLOUD_ENABLED && is_editor() && m_last_config_version'),
    @('src/slic3r/GUI/MainFrame.cpp',               'if (FORCA_ORCA_CLOUD_ENABLED) // Forca: Sync Presets is Orca Cloud only'),
    @('src/slic3r/GUI/Preferences.cpp',             'if (FORCA_ORCA_CLOUD_ENABLED) { // Forca: preset sync is Orca Cloud only'),
    @('src/slic3r/GUI/CreatePresetsDialog.cpp',     'FORCA_ORCA_CLOUD_ENABLED && // Forca: no Orca Cloud sync prompt'),
    @('resources/web/homepage/index.html',          '<div id="OrcaAccount" class="AccountRow" style="display:none"'),
    @('tests/libslic3r/CMakeLists.txt',             'test_forca_preset_merge.cpp'),
    # B2: no update or profile checks against Orca's servers
    @('src/slic3r/GUI/GUI_App.cpp',                 'auto http = Http::get(FORCA_RELEASES_API_URL);'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'if (!app_config->get_stealth_mode()) // Forca: Stealth mode means offline'),
    @('src/slic3r/Utils/ForcaFeatures.hpp',         'FORCA_RELEASES_API_URL = "https://api.github.com/repos/tayloraaron078-tech/ForcaSlicer/releases"'),
    @('src/slic3r/GUI/GUI_App.cpp',                 '#if 0 // Forca: Orca''s update-server request'),
    @('src/slic3r/Utils/PresetUpdater.cpp',         'if (!FORCA_ORCA_PROFILE_UPDATES_ENABLED) return; // Forca (B2)'),
    @('src/slic3r/Utils/ForcaFeatures.hpp',         'constexpr bool FORCA_ORCA_PROFILE_UPDATES_ENABLED = false;'),
    # B3: Forca's own version
    @('version.inc',                                'set(FORCA_VERSION "'),
    @('src/libslic3r/libslic3r_version.h.in',       '#define FORCA_VERSION "@FORCA_VERSION@"'),
    @('src/libslic3r/utils.cpp',                    'return std::string(SLIC3R_APP_NAME " " FORCA_VERSION);'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'version_display = FORCA_VERSION;'),
    @('src/slic3r/GUI/GUI_App.cpp',                 'Semver    current_version = get_version(FORCA_VERSION, matcher);'),
    @('src/slic3r/GUI/AboutDialog.cpp',             'auto          version_string = std::string(FORCA_VERSION);'),
    @('CMakeLists.txt',                             'set (CPACK_PACKAGE_FILE_NAME "ForcaSlicer_Windows_Installer_V${FORCA_VERSION}")'),
    @('tests/libslic3r/CMakeLists.txt',             'test_forca_version.cpp'),
    # B4: Forca links, never OrcaSlicer's tracker/releases
    @('src/slic3r/Utils/ForcaFeatures.hpp',         'constexpr const char* FORCA_REPO_URL = "https://github.com/'),
    @('src/slic3r/GUI/TroubleshootDialog.cpp',      'wxString(FORCA_REPO_URL) + "/issues/new?template=bug_report.yml"'),
    @('src/slic3r/GUI/MainFrame.cpp',               'std::string(FORCA_REPO_URL) + "/issues/new/choose"'),
    @('src/slic3r/GUI/MsgDialog.cpp',               'std::string(FORCA_REPO_URL) + "/releases"'),
    @('src/slic3r/Utils/MoonrakerPrinterAgent.cpp', 'identify["params"]["client_name"] = "ForcaSlicer";'),
    @('src/slic3r/Utils/UltiMaker.cpp',             'boost::starts_with(line, "; generated by " SLIC3R_APP_NAME)'),  # header fix-up must see Forca's header
    @('src/slic3r/GUI/Widgets/StateColor.cpp',      'static const std::map<wxColour, wxColour> gForcaRetintedKeys'),  # dark mode for un-retinted numeric colours
    @('CMakeLists.txt',                             'set (CPACK_NSIS_MODIFY_PATH "ON")'),                             # installer desktop-icon option
    @('resources/web/guide/1/1.css',                '#SlicerIcon img')                                                # wizard welcome logo fits
)
foreach ($m in $must) {
    if (-not (Read-Repo $m[0]).Contains($m[1])) { Fail "$($m[0]): missing Forca change: $($m[1])" }
}

# 2) Orca identity that must NOT come back.
$mustNot = @(
    @('SECURITY.md',                'softfeverever@gmail.com'),        # Orca's security policy (Forca's replaces it)
    @('src/slic3r/GUI/GUI_App.cpp', 'associate_url(L"orcaslicer");'),   # Forca never claims orcaslicer:// on its own
    @('src/slic3r/GUI/GUI_App.cpp', 'L" Orca.Slicer.1"'),               # Orca's file-association ProgID
    @('src/slic3r/GUI/GUI_App.cpp', 'SetAppName(SLIC3R_APP_KEY);'),     # shared settings folder with Orca
    @('resources/web/homepage/index.html', 'https://cloud.orcaslicer.com'),                  # OrcaCloud shortcut (B9)
    @('resources/web/dialog/ExportPresetDialog/index.html', 'id="export_cloud_btn"')         # Export to OrcaCloud (B9)
)
foreach ($m in $mustNot) {
    if ((Read-Repo $m[0]).Contains($m[1])) { Fail "$($m[0]): Orca identity is back: $($m[1])" }
}

# B9: every Orca Cloud HTTP helper (get/post/put/delete/post_token/post_auth) must refuse to connect.
$guards = ([regex]::Matches((Read-Repo 'src/slic3r/Utils/OrcaCloudServiceAgent.cpp'), '// Forca: no Orca Cloud traffic')).Count
if ($guards -ne 6) { Fail "src/slic3r/Utils/OrcaCloudServiceAgent.cpp: expected 6 Orca Cloud HTTP guards, found $guards" }

# B4: no link in the app may send users (or their bug reports) to OrcaSlicer's GitHub repo, tracker or releases.
$orcaGitHub = Get-ChildItem (Join-Path $RepoRoot 'src\slic3r') -Recurse -Include *.cpp,*.hpp |
    Select-String -Pattern 'github\.com/(OrcaSlicer/OrcaSlicer|SoftFever/OrcaSlicer)' |
    Where-Object { $_.Line -notmatch '^\s*//' -and $_.Line -notmatch '^\s*\{ "OrcaSlicer",' }   # the About box's credit entry is intended
foreach ($o in $orcaGitHub) { Fail ("{0}:{1}: link to OrcaSlicer's GitHub: {2}" -f $o.Path.Substring($RepoRoot.Length + 1), $o.LineNumber, $o.Line.Trim()) }

# B6: nothing that goes public (tracked files + the draft release docs) may carry private references.
$private = & git -C $RepoRoot grep -n -I -i -E 'prsda|OrcaSlicer-RegionalSupports|AI-Developer-HQ|Developer HQ|CLAUDE\.local' -- . 2>$null
$private += Get-ChildItem (Join-Path $RepoRoot 'docs\forca\release') -Recurse -File -ErrorAction SilentlyContinue |
    Select-String -Pattern 'prsda|OrcaSlicer-RegionalSupports|AI-Developer-HQ|Developer HQ|CLAUDE\.local' |
    ForEach-Object { "{0}:{1}:{2}" -f $_.Path.Substring($RepoRoot.Length + 1), $_.LineNumber, $_.Line.Trim() }
foreach ($p in $private) { if ($p -notmatch 'check_identity\.ps1') { Fail "private reference: $p" } }

# B5: Orca's GitHub automation (workflows/bots with Orca's secrets, funding, dependabot) must not come back with a merge.
foreach ($g in '.github/workflows/build_orca.yml', '.github/workflows/pr-merge-bot.yml', '.github/workflows/dedupe-issues.yml',
               '.github/workflows/publish_release.yml', '.github/FUNDING.yml', '.github/dependabot.yml') {
    if (Test-Path (Join-Path $RepoRoot $g)) { Fail "${g}: Orca's GitHub automation is back (keep .github deleted, see upstream-merge.md)" }
}

# 3) Icons/logos Forca replaced must still differ from the Orca base (upstream may have overwritten them).
$assets = 'OrcaSlicer.ico','OrcaSlicer.png','OrcaSlicer_192px.png','OrcaSlicer_128px.png','OrcaSlicer_192px_grayscale.png',
          'OrcaSlicer_192px_transparent.png','OrcaSlicerTitle.ico','OrcaSlicer-mac_256px.ico','OrcaSlicer_about.png',
          'OrcaSlicer_about_dark.png','OrcaSlicer_horizontal_dark.png','OrcaSlicer_horizontal_light.png',
          'info.svg','question.svg','exclamation.svg','notification_slicing_complete.svg'
foreach ($a in $assets) {
    & git -C $RepoRoot diff --quiet $OrcaBase -- "resources/images/$a" 2>$null
    if ($LASTEXITCODE -eq 0) { Fail "resources/images/${a}: is Orca's original again (regenerate/restore the Forca version)" }
}

# 4) Orca's little orca-and-wave drawing must not be in any interface icon.
Get-ChildItem (Join-Path $RepoRoot 'resources\images') -Filter *.svg | ForEach-Object {
    if ([IO.File]::ReadAllText($_.FullName).Contains('M23.527,29.084')) { Fail "resources/images/$($_.Name): has Orca's orca-and-wave drawing" }
}

# 5) Review list: untranslated literals mentioning Orca (not failures; check whether each is shown to users).
#    Skipped: translated strings and their continuation lines (renamed at runtime by forca_brand), comments, logs,
#    URLs, Orca's services/models/file names, and the known-intentional files/identifiers below.
$skipLine = '_L\(|_u8L\(|_L_CONTEXT\(|_u8L_CONTEXT\(|_L_PLURAL\(|\bL\(|^\s*//|///<|#include|BOOST_LOG|https?://|com\.orcaslicer|X-Orca|OrcaCloud|Orca Cloud|\.orca_|Orca Arena|Orca YOLO|Orca-LinearFlow|runtime_error|RuntimeError|images/|_192px|"OrcaSlicer_|OrcaSlicer/Auth|OrcaSlicerVersion|program_name|m\.doc\(\)|pbdoc|legacy_data_dir|body\.find|OrcaSlicer[\\/]+build|/Volumes/OrcaSlicer|Open in OrcaSlicer|based on OrcaSlicer|// e\.g\.|Forca'
$skipFile = @(
    'I18N.cpp',                   # the rename rule itself
    'DesktopIntegrationDialog.cpp', # Linux desktop entries: renamed with the Linux release, not Windows alpha
    'InstanceCheck.cpp',          # D-Bus identifiers (Linux)
    'CustomWidgetsPlugin.cpp', 'DPIAwarePlugin.cpp', # developer-only wxInspector plugins
    '3DPrinterOS.cpp', 'MoonrakerPrinterAgent.cpp', 'Obico.cpp', 'SimplyPrint.cpp', # client IDs registered with those services
    'OrcaPrinterAgent.cpp', 'OrcaCloudServiceAgent.cpp', # Orca's own protocol / cloud identifiers
    'LabeledStaticBox.cpp',       # text used only to measure a font
    'GUI_App.hpp'                 # logo_name(): icon file name
)
$review = Get-ChildItem (Join-Path $RepoRoot 'src\slic3r') -Recurse -Include *.cpp,*.hpp |
    Where-Object { $skipFile -notcontains $_.Name } |
    Select-String -Pattern '"[^"]*\bOrca[^"]*"' -CaseSensitive -Context 8,0 |
    Where-Object { $_.Line -notmatch $skipLine -and
                   -not ($_.Line.TrimStart().StartsWith('"') -and (($_.Context.PreContext -join ' ') -match '_L\(|_u8L\(|_L_CONTEXT\(|_u8L_CONTEXT\(')) }
foreach ($r in $review) {
    Write-Host ("REVIEW {0}:{1}: {2}" -f $r.Path.Substring($RepoRoot.Length + 1), $r.LineNumber, $r.Line.Trim())
}
Write-Host ''
if ($fails -gt 0) { Write-Host "$fails check(s) FAILED - restore the Forca changes listed above." -ForegroundColor Red; exit 1 }
Write-Host "All Forca identity checks passed ($($must.Count + $mustNot.Count) code checks, $($assets.Count) assets, icons). $($review.Count) literal(s) to review." -ForegroundColor Green
exit 0
