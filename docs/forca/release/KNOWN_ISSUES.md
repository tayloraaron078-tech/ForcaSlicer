# Known issues — Forca Slicer alpha

Things we already know about. No need to report these (but do add a comment to the linked issue if it affects you
differently).

## General
- **Windows 64-bit only.** No macOS or Linux builds are planned; the source builds on other systems for anyone who
  wants to try.
- **Code-signed since 0.1.0-alpha.4**, but SmartScreen may still warn for a while until the certificate builds a
  reputation (More info → Run anyway). The publisher shown should be Aaron Taylor.
- **Tested mainly on Bambu Lab printers** (mostly an H2S), plus an Elegoo OrangeStorm Giga (Klipper) and a stock
  Flashforge Adventurer 5M for the non-Bambu printer features. Other brands use OrcaSlicer's own code paths and should
  behave like OrcaSlicer, but haven't been tested with Forca's features.
- **Built on OrcaSlicer's 2.5.0 development line** (merged up to early October 2026), not a stable Orca release.
  Some OrcaSlicer bugs that are fixed in newer Orca builds may still be here. Report them here anyway; we'll check and
  forward them if they're Orca's.
- **OrcaSlicer's Design (CAD) tab is experimental in OrcaSlicer itself** and comes with Forca as it is (off until you
  enable it in Preferences). Forca hasn't changed it.
- **Some of Forca's own screens are English only** (Forca AI window, support-region tools). The Calibration Wizard
  is translated into all of OrcaSlicer's languages, by machine for now -- corrections welcome. The rest of the app
  keeps OrcaSlicer's translations.

## Regional support interfaces
- The top-Z gap is shared by all regions on an object (one gap for all regions; the filament can differ per region).

## Calibration Wizard
- Printer calibration (Input Shaping, Cornering, VFA) is new. It runs OrcaSlicer's own tests, but it has only been
  tried on a few printers so far -- Marlin and RepRapFirmware printers not at all yet. Printer reports especially
  welcome.

## Forca Academy (print journal)
- Prints started from an SD card aren't recorded automatically -- add them with "Add a print" in File > Print Journal.
- Multi-device sends are recorded, but how the print ended is only followed on the printer selected in the Device tab.
- Klipper and Flashforge prints are followed while Forca is running and can reach the printer; a stock Flashforge
  printer needs its access code in the printer's network settings (API Key / Password).
- Journal files (AGENTS.md, notes) are written in English.

## Non-Bambu printers
- **Happy Hare and Moonraker filament sync fixes are experimental and untested by us** (no Happy Hare, Spoolman lane
  sync or ACE setup here). They come from two OrcaSlicer pull requests that are not merged upstream yet. If you use
  Happy Hare or Moonraker filament data, please tell us on
  [#3](https://github.com/tayloraaron078-tech/ForcaSlicer/issues/3) whether "Synchronize filament list" works.
- **Flashforge status page:** tested on an Adventurer 5M (firmware 5.1.8). It needs the printer's access code in the
  printer's network settings (API Key / Password). Other Flashforge models with the same local API should work but
  are untested. Forca Academy pictures from a Flashforge printer's built-in camera are untested (no camera here).
- **Klipper web interface search** looks in the usual places (the printer's address, ports 81 / 4408 / 4409, /fluidd,
  /mainsail); type the address in Device UI if yours is elsewhere.

## Forca AI (experimental)
- **Off by default**; only works on the same computer; needs the key shown in the Forca AI window.
- **One camera viewer at a time:** a Bambu printer's camera accepts one viewer. If Bambu Handy, another slicer or a
  print manager is watching, the AI's snapshot times out after 25 s — open the live view in Forca's Device tab and the
  AI uses that picture instead.
- Live printer status comes from the printer selected in Forca's Device tab.
- A plate picture from the AI shows the objects only (not the bed or modifier boxes).
- AI clients (for example Claude Code) load Forca's tool list when they connect; after updating Forca, reconnect them.
- No macOS entry point for the Forca AI window yet (Windows builds only anyway).
- **Print grants are Bambu Lab only** for now (other printers: the AI can still ask, and you send).
- **The camera bed check can't see the whole plate:** a Bambu H2S camera, for one, doesn't show the strip nearest the
  door. Turn on camera bed checks only if you are fine with that; your own "Bed is clear" click has no blind spot.

## Bambu network plug-in and Open Bamboo Networking
- **Open Bamboo Networking** (Preferences → Bambu network plug-in → "Network plug-in source") needs printers in
  LAN-only mode with Developer Mode on, and cloud printing, the camera away from home, Go Live, HMS photos and
  MakerWorld history don't work with it. Tested on an H2S. A switch takes effect after closing and reopening Forca.
- **Use "Network plug-in source" to switch between the two**, not the "Network plug-in version" list next to it: that
  list reloads the plug-in while Forca runs, and the camera live view can then crash Forca.

## Crashes we know about
- **Shortly after sending a print to a Bambu printer** (about 15-30 seconds), Forca can close unexpectedly with Bambu
  Lab's network plug-in. The print itself is already on the printer and carries on; restart Forca and, with the print
  journal on, it picks the print up again. The crash is inside Bambu's plug-in, which Forca can't change; Open Bamboo
  Networking didn't have it in our tests, and Forca offers it after such a crash.
- **Rarely, when a print grant opens the print dialog**, Forca has closed unexpectedly before anything was sent.
  Seen twice, not since; Forca now logs each step so the next one can be found. If it happens to you, please report
  it and attach the newest `debug_*.log.0` from Help → Show Configuration Folder → `log`.
