<div align="center">

<picture>
  <img alt="Forca Slicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

# Forca Slicer

**Another Forca of Orca** — by A.T. Creations · Built for Makers

**Alpha** · Windows 64-bit · Free and open source (AGPL-3.0) · Based on [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer)

[Download the latest alpha](https://github.com/tayloraaron078-tech/ForcaSlicer/releases) · [How to test and report](docs/forca/release/TESTING.md) · [Known issues](docs/forca/release/KNOWN_ISSUES.md)

</div>

> **This is an alpha.** Everything below works on the author's printers, but it has not been
> tested on yours yet — that's what this release is for. Keep your normal slicer installed, check the first layers
> of anything important, and please report what you find. Forca keeps its settings in its own folder, so it won't
> change your OrcaSlicer setup.

Forca Slicer is a fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). It keeps everything that makes
Orca a great slicer and adds three things:

1. **Regional support interfaces** — a clean, removable release (for example a PETG interface under a PLA part),
   exactly where you choose, instead of everywhere.
2. **A guided Calibration Wizard** — take a filament from untuned to calibrated, step by step, inside the slicer.
3. **Forca AI (experimental, optional)** — let *your own* AI assistant, such as Claude, work in the slicer with you,
   under safety rules built into Forca. **Forca has no AI built in:** no AI model, no AI account, nothing sent to an
   AI service. If you don't bring one, there is no AI.

Projects (`.3mf`) and printer/filament profiles are OrcaSlicer's formats, so your files open in both.

# What Forca adds

## Regional support interfaces
Give a bounded area of a model its own support-interface **filament**, top-Z **gap** (including a flush 0 mm gap)
and interface layers, while the rest of the supports follow the object's normal settings.

- **Place a box** (support-interface modifier) or **paint the surface** with a dedicated paint tool; each painted
  filament becomes its own region.
- Works with **Grid, Snug, Tree (Slim / Strong / Hybrid) and Organic** supports.
- Paint helpers: Vertical / Horizontal stroke lock and "on overhangs only".

## Calibration Wizard
A per-filament guided flow in its own tab: **Temperature → Max Volumetric Speed → Flow → Pressure Advance →
(flow recheck) → Retraction → Shrinkage → (final check print)**.

- Uses OrcaSlicer's own test prints, with a choice of method where Orca has several.
- Slice & send straight to the printer, or generate only (SD card).
- Progress bar and before/after table; results go into **one new "(calibrated)" filament preset** — your starting
  preset is never changed.

## Forca AI (experimental, off by default)
**Bring your own AI — or don't.** Forca doesn't contain an AI and doesn't need one. Forca AI is only a door that
*your* AI app can use, and it stays shut unless you open it: while it's off, the connection isn't running at all,
and Forca works exactly like a slicer with no AI. You choose which AI to use (Claude Desktop, Claude Code, or any
other app that supports MCP, cloud or local), and your account and data stay with that app.

When you do turn it on, it lets an AI assistant (Claude Code, or any app that speaks MCP) see and operate Forca — load and arrange models,
choose presets, add support regions, slice and read the results, save a copy. For printers you choose, it can read
status and take a camera snapshot (Bambu, over your local network).

Built-in rules the AI cannot switch off:
- It **never saves over your files or presets** — its saves get new "(Claude <date>)" names.
- It **never prints on its own** — it can only ask; you approve on a card in Forca, then send as usual.
- It only sees the printers you tick, runs only on your computer, and needs a key you give it.

See [SECURITY.md](SECURITY.md) for how it's protected.

# Install (Windows)
Download one of these from [Releases](https://github.com/tayloraaron078-tech/ForcaSlicer/releases):
- **Installer** — `Forca_Slicer_<version>_win64_setup.exe`: installs to Program Files, adds a Start-menu entry
  (and a desktop icon if you tick it) and an uninstaller.
- **Portable zip** — `Forca_Slicer_<version>_win64_portable.zip`: unzip anywhere (for example `C:\Forca Slicer`)
  and run `forca-slicer.exe` in the `ForcaSlicer` folder. Nothing is installed.

Then:
- Windows may show "Windows protected your PC" because the alpha isn't code-signed: click **More info → Run
  anyway**. (Compare the SHA-256 on the release page if you want to be sure the file is ours.)
- On first start, Forca can copy your OrcaSlicer settings. It copies — your OrcaSlicer folder is never changed.
- Forca checks this repository's releases for a newer Forca when it starts (not in Stealth mode); nothing else is
  sent.

macOS and Linux builds are not available yet.

# Report problems and ideas
Please use [GitHub Issues](https://github.com/tayloraaron078-tech/ForcaSlicer/issues/new/choose) (the forms ask for exactly what's
needed) and [Discussions](https://github.com/tayloraaron078-tech/ForcaSlicer/discussions) for questions. **Please don't report Forca
problems to OrcaSlicer** — if it's a Forca build, report it here even if you think the bug is Orca's; we'll
forward it upstream if it is. The [tester guide](docs/forca/release/TESTING.md) explains what's most useful.

# Built on OrcaSlicer
Everything outside the three features above is OrcaSlicer, and Orca's documentation applies:
<https://www.orcaslicer.com/wiki>. Forca is built on OrcaSlicer's 2.5.0 development line (merged up to late
September 2026) and merges newer Orca occasionally, not continuously. That includes OrcaSlicer's own experimental
**Design (CAD) tab**, which stays off until you enable it in Preferences.

Open-source slicing is built on a tradition of collaboration and attribution: [Slic3r](https://github.com/Slic3r/Slic3r)
(Alessandro Ranellucci and the RepRap community) → [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) →
[Bambu Studio](https://github.com/bambulab/BambuStudio) and [SuperSlicer](https://github.com/supermerill/SuperSlicer)
→ [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). Forca continues that chain. Full credit and thanks to
SoftFever and the OrcaSlicer community, and to everyone upstream of them. Forca is not affiliated with or endorsed
by OrcaSlicer or Bambu Lab.

# Building from source
See [OrcaSlicer — How to build](https://www.orcaslicer.com/wiki/how_to_build); it applies unchanged. Windows quick
reference: `build_win.bat -d` once for dependencies, then `build_win.bat -s` (together about an hour on a recent
PC). `build_forca_release.ps1 -Config Release` packages a portable zip; add `-Installer` for the installer (needs
[NSIS](https://nsis.sourceforge.io/)). Maintenance notes (upstream merges, the blue retint) are in [`docs/forca/`](docs/forca/README.md).

# License
- Forca Slicer is licensed under the **GNU Affero General Public License v3.0**, the same as OrcaSlicer. The complete
  source for every release is in this repository (and attached to each GitHub release).
- Forca includes a pressure advance pattern test adapted from Andrew Ellis' generator (GPL-3.0), itself adapted from
  Sineos' generator for Marlin (GPL-3.0).
- The **Bambu network plugin** is proprietary software from Bambu Lab, downloaded separately and optional; it
  provides Bambu printer features (device control, camera).
- No warranty (see the license). You are responsible for what you print — check the preview and the first layers.
