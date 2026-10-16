#pragma once

// Forca: Open Bamboo Networking (OBN), the open-source replacement for Bambu Lab's network plug-in, as a one-click
// choice next to Bambu's own (DECISIONS 2026-10-04 / 2026-10-08, docs/HLSD/obn-choice.md).
//
// OBN builds live in <data_dir>/plugins/obn/<release tag>/v<series>/ (bambu_networking.dll + BambuSource.dll, one folder
// per plug-in series the release's obn-windows-x64.zip ships). Choosing OBN copies the build for this Forca's plug-in
// series to plugins/bambu_networking_<series>-obn.dll (a custom-named build, which OrcaSlicer's plug-in housekeeping
// never renames or removes) and its camera DLL to plugins/BambuSource.dll; Bambu's camera DLL is kept in
// plugins/obn/bambu-camera/ for switching back. The choice itself is stored apart from the version name (app config
// "forca_obn_chosen"), so it survives an upstream change of the plug-in series: at start-up OBN follows the series
// (forca_obn_startup).

#include <string>
#include <vector>

namespace Slic3r {

struct ForcaObnRelease
{
    std::string tag;     // "v2.2.0"
    std::string zip_url; // browser_download_url of obn-windows-x64.zip
    std::string sha256;  // lower-case hex, from the asset's GitHub digest
};

// ---- pure helpers (unit-tested in tests/slic3rutils/test_forca_obn.cpp) ----

// The plug-in version name Forca uses for OBN on a series: "02.08.01" -> "02.08.01-obn".
std::string forca_obn_version(const std::string& series);
bool        forca_obn_is_obn_version(const std::string& version);
// A GitHub "latest release" reply -> the Windows x64 asset and its SHA-256. False (with err) for a draft or
// pre-release, a missing asset, or an asset without a sha256 digest.
bool forca_obn_parse_release(const std::string& github_json, ForcaObnRelease& out, std::string& err);
// True when a zip entry name is <anything>/lib/v<series>/<file> (either slash).
bool forca_obn_zip_entry_matches(const std::string& entry, const std::string& series, const std::string& file);
// The plug-in series ("02.08.04") for which a zip holds both bambu_networking.dll and BambuSource.dll, sorted.
std::vector<std::string> forca_obn_zip_series(const std::vector<std::string>& entries);

// What start-up has to do for OBN before the plug-in loads.
enum class ForcaObnStartup
{
    NotChosen, // the user didn't choose OBN
    Ready,     // OBN for this series is selected and in place
    Activate,  // the cached release has this series: put it in place and select it (no download)
    Download,  // nothing on disk for this series: ask to download the latest release
};
// `chosen`: the user chose OBN; `configured`: the selected plug-in version; `series`: this Forca's plug-in series;
// `cached_series`: the series of the cached release in use; `plugin_present`: bambu_networking_<series>-obn.dll exists.
ForcaObnStartup forca_obn_startup(bool chosen, const std::string& configured, const std::string& series,
                                  const std::vector<std::string>& cached_series, bool plugin_present);
// True when one of Forca's crash logs says the crash happened inside Bambu's network plug-in.
bool forca_obn_crash_in_network_plugin(const std::string& crash_log);
// Lower-case hex SHA-256 of a buffer.
std::string forca_obn_sha256_hex(const std::string& data);

// ---- files (blocking: network + disk; call under a busy cursor) ----

std::string forca_obn_cache_dir(const std::string& tag);  // <data_dir>/plugins/obn/<tag>
std::vector<std::string> forca_obn_cached_tags();         // builds on disk, newest first
std::vector<std::string> forca_obn_cached_series(const std::string& tag); // series cached for a release, sorted
// The cached release has a build for `series`. A release can be on disk without it (an older Forca kept one series),
// so this, not the tag alone, decides whether a download is needed.
bool forca_obn_has_series(const std::string& tag, const std::string& series);
// Forca before 2026-10 kept one series flat in <tag>/: move it to <tag>/v<series>/ (`series` = the one it ran).
void forca_obn_adopt_flat_cache(const std::string& tag, const std::string& series);
// <data_dir>/plugins/bambu_networking_<series>-obn.dll exists.
bool forca_obn_plugin_present(const std::string& series);
// The latest stable release from GitHub.
bool forca_obn_fetch_latest(ForcaObnRelease& out, std::string& err);
// Download, check the SHA-256, and unpack the two DLLs of every series in the zip into forca_obn_cache_dir(tag)/v<series>.
bool forca_obn_download(const ForcaObnRelease& release, std::string& err);
// Unpack an OBN Windows zip (already checked) into forca_obn_cache_dir(tag)/v<series>, one folder per series it ships.
// Each series is swapped in only when both DLLs are written, so a failure leaves the cached builds as they were.
bool forca_obn_unpack(const std::string& zip, const std::string& tag, std::string& err);
// Put a cached OBN build for `series` in place (plug-in + camera DLL), keeping Bambu's camera DLL for switching back.
// Fails when the release has no build for that series. The caller then selects forca_obn_version(series).
bool forca_obn_activate(const std::string& tag, const std::string& series, std::string& err);
// Put Bambu's camera DLL back. The caller then selects the Bambu series and reloads the plug-in.
bool forca_obn_restore_bambu_camera(std::string& err);

} // namespace Slic3r
