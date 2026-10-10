#pragma once

// Forca: Open Bamboo Networking (OBN), the open-source replacement for Bambu Lab's network plug-in, as a one-click
// choice next to Bambu's own (DECISIONS 2026-10-04 / 2026-10-08, docs/HLSD/obn-choice.md).
//
// OBN builds live in <data_dir>/plugins/obn/<release tag>/ (bambu_networking.dll + BambuSource.dll, as published in
// the release's obn-windows-x64.zip). Choosing OBN copies the build in use to plugins/bambu_networking_<series>-obn.dll
// (a custom-named build, which OrcaSlicer's plug-in housekeeping never renames or removes) and its camera DLL to
// plugins/BambuSource.dll; Bambu's camera DLL is kept in plugins/obn/bambu-camera/ for switching back.

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
// True when one of Forca's crash logs says the crash happened inside Bambu's network plug-in.
bool forca_obn_crash_in_network_plugin(const std::string& crash_log);
// Lower-case hex SHA-256 of a buffer.
std::string forca_obn_sha256_hex(const std::string& data);

// ---- files (blocking: network + disk; call under a busy cursor) ----

std::string forca_obn_cache_dir(const std::string& tag);  // <data_dir>/plugins/obn/<tag>
std::vector<std::string> forca_obn_cached_tags();         // builds on disk, newest first
// The latest stable release from GitHub.
bool forca_obn_fetch_latest(ForcaObnRelease& out, std::string& err);
// Download, check the SHA-256, and unpack the two DLLs for `series` into forca_obn_cache_dir(tag).
bool forca_obn_download(const ForcaObnRelease& release, const std::string& series, std::string& err);
// Put a cached OBN build in place (plug-in + camera DLL), keeping Bambu's camera DLL for switching back.
// The caller then selects forca_obn_version(series) and reloads the plug-in.
bool forca_obn_activate(const std::string& tag, const std::string& series, std::string& err);
// Put Bambu's camera DLL back. The caller then selects the Bambu series and reloads the plug-in.
bool forca_obn_restore_bambu_camera(std::string& err);

} // namespace Slic3r
