# Known issues — Forca Slicer alpha

Things we already know about. No need to report these (but do add a comment to the linked issue if it affects you
differently).

## General
- **Windows 64-bit only.** No macOS or Linux builds yet.
- **Not code-signed.** Windows SmartScreen warns on first run (More info → Run anyway).
- **Tested mainly on Bambu Lab printers** (mostly an H2S). Other brands use OrcaSlicer's own code paths and should
  behave like OrcaSlicer, but haven't been tested with Forca's features.
- **Built on OrcaSlicer's 2.5.0 development line** (merged up to late September 2026), not a stable Orca release.
  Some OrcaSlicer bugs that are fixed in newer Orca builds may still be here. Report them here anyway; we'll check and
  forward them if they're Orca's.
- **OrcaSlicer's Design (CAD) tab is experimental in OrcaSlicer itself** and comes with Forca as it is (off until you
  enable it in Preferences). Forca hasn't changed it.
- **Forca's own screens are English only** (Calibration Wizard, Forca AI window, support-region tools). The rest of
  the app keeps OrcaSlicer's translations.

## Regional support interfaces
- The top-Z gap is shared by all regions on an object (one gap for all regions; the filament can differ per region).

## Calibration Wizard
- Machine calibrations (input shaping, cornering, VFA) aren't in the wizard yet — use OrcaSlicer's Calibration menu.

## Forca AI (experimental)
- **Off by default**; only works on the same computer; needs the key shown in the Forca AI window.
- **One camera viewer at a time:** a Bambu printer's camera accepts one viewer. If Bambu Handy, another slicer or a
  print manager is watching, the AI's snapshot times out after 25 s — open the live view in Forca's Device tab and the
  AI uses that picture instead.
- Live printer status comes from the printer selected in Forca's Device tab.
- Exported G-code is the sliced file as-is: **post-processing scripts are not run** for AI exports.
- A plate picture from the AI shows the objects only (not the bed or modifier boxes).
- AI clients (for example Claude Code) load Forca's tool list when they connect; after updating Forca, reconnect them.
- No macOS entry point for the Forca AI window yet (Windows builds only anyway).
