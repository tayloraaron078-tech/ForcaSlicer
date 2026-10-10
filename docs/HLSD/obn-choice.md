# Open Bamboo Networking Choice: High Level Design

## Why it exists

Bambu Lab printers are driven through Bambu Lab's network plug-in, a closed library Forca loads at run time. On the
plug-in version Forca uses, it sometimes crashed Forca about 25 seconds after a print was sent. Open Bamboo Networking
(OBN, github.com/ClusterM/open-bamboo-networking, AGPL-3.0) is an open-source replacement with the same interface; in a
trial of eight sends on an H2S it never crashed.

OBN costs features: printers must be in LAN-only mode with Developer Mode on, and cloud printing, the camera away from
home, Go Live, HMS photos and MakerWorld history don't work. So Bambu Lab's plug-in stays the default, and OBN is a
choice the user makes, and can undo, in one place.

## What the user sees

- Preferences, under the Bambu network plug-in settings: "Network plug-in source", Bambu Lab (official) or Open Bamboo
  Networking (open source), with Update and Roll back buttons for OBN.
- Choosing OBN shows what it costs before anything happens; then Forca downloads it, checks it and puts it in place,
  and the user closes and reopens Forca to finish. Choosing Bambu Lab puts everything back the same way.
- When Forca's last crash happened inside Bambu Lab's plug-in, opening the Device tab offers OBN once for that crash,
  with "Don't show again".

## How it works

`slic3r/Utils/ForcaOBN.{hpp,cpp}` does the work; `slic3r/GUI/ForcaObnChoice.{hpp,cpp}` holds the dialogs and
settings.

- **Where OBN comes from.** The latest stable release on GitHub (`releases/latest`; drafts and pre-releases are
  refused). Forca follows OBN's releases instead of pinning one, so the user decides when to update; it never updates
  silently. Only the `obn-windows-x64.zip` asset is used, and only when GitHub publishes a SHA-256 digest for it; the
  download must match that digest. From the zip Forca takes just `lib/v<series>/bambu_networking.dll` and
  `BambuSource.dll` for the plug-in series it loads; OBN's own install scripts are never run.
- **Where it lives.** Each release is kept in `<data_dir>/plugins/obn/<tag>/`. The one in use is copied to
  `plugins/bambu_networking_<series>-obn.dll`: a custom-named build, which OrcaSlicer's plug-in housekeeping never
  renames or removes, and which its version picker lists and loads like any other build of that series.
- **The camera DLL.** `plugins/BambuSource.dll` must match the plug-in in use. Switching to OBN keeps Bambu Lab's copy
  in `plugins/obn/bambu-camera/` first (unless the file there is already OBN's); switching back restores it, or the
  copy from Bambu Lab's last plug-in download. A DLL that is loaded is renamed aside (`.old`, swept before the plug-in
  next loads).
- **Why a restart and not a hot reload.** OrcaSlicer can reload the network plug-in in place, but the camera player
  keeps the camera DLL's function addresses for the whole session (`StaticBambuLib`, and a copy in every live-view
  control). Unloading that DLL while Forca runs makes the next live view call into freed code and crash (seen on the
  first hardware test). So a switch only changes files and the setting; everything loads fresh at the next start. If
  OBN then fails to load, Forca switches back to Bambu Lab's plug-in and says so (`forca_obn_check_loaded`).
- **Update and roll back.** An update downloads the newer release and keeps the one in use as the previous release;
  roll back swaps them.
- **After a crash.** At start-up Forca reads its newest crash log once; when the faulting module is Bambu Lab's
  plug-in, the Device tab makes its one-time offer.

## Constraints

- **Trust.** Following OBN's releases means trusting each release OBN publishes. The digest check proves the file is
  the one GitHub serves for that release, not that the release is good; that is why updates are offered, never
  automatic, and why the previous release and Bambu Lab's plug-in are always one click away.
- **Upstream merges.** OrcaSlicer owns the plug-in loading, the version picker and the plug-in reload; Forca only adds
  files and app-config keys (`forca_obn_*`) and calls them. If upstream changes how custom-named builds are found or
  loaded, the `-obn` name must still be loadable as its series.
- **Licence.** OBN's AGPL-3.0 notice and source link are in About -> License Info.
