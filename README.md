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
Orca a great slicer and adds:

1. **Regional support interfaces** — a clean, removable release (for example a PETG interface under a PLA part),
   exactly where you choose, instead of everywhere.
2. **A guided Calibration Wizard** — take a filament, or your printer, from untuned to calibrated, step by step,
   inside the slicer.
3. **Forca Academy (optional)** — a print journal that records every print you send, how it went, and pictures
   from the printer's camera, as plain files on your computer.
4. **Bambu Studio's extra settings** — Bambu Lab profiles and Bambu Studio projects keep settings that OrcaSlicer
   drops, such as Bambu's smarter infill retraction.
5. **Open Bamboo Networking (optional)** — a one-click switch to an open-source replacement for Bambu Lab's network
   plug-in.
6. **Faster toolpaths** — less travel between parts of a layer, and more accurate cooling on small layers.
7. **More for non-Bambu printers** — a status page for stock Flashforge printers, and Fluidd / Mainsail found for you
   on Klipper printers.
8. **Forca AI (experimental, optional)** — let *your own* AI assistant, such as Claude, work in the slicer with you,
   under safety rules built into Forca. **Forca has no AI built in:** no AI model, no AI account, nothing sent to an
   AI service. If you don't bring one, there is no AI.

Plus a set of smaller changes, listed [at the end of this section](#smaller-changes). Projects (`.3mf`) and
printer/filament profiles are OrcaSlicer's formats, so your files open in both.

# What Forca adds

## Regional support interfaces
Give a bounded area of a model its own support-interface **filament**, top-Z **gap** (including a flush 0 mm gap)
and interface layers, while the rest of the supports follow the object's normal settings.

- **Place a box** (support-interface modifier) or **paint the surface** with a dedicated paint tool; each painted
  filament becomes its own region.
- Works with **Grid, Snug, Tree (Slim / Strong / Hybrid) and Organic** supports.
- Paint helpers: Vertical / Horizontal stroke lock and "on overhangs only".

https://github.com/user-attachments/assets/23ea01ea-bb8d-438b-8dfe-0c72c96b8af4

## Calibration Wizard
A per-filament guided flow in its own tab: **Temperature → Max Volumetric Speed → Flow → Pressure Advance →
(flow recheck) → Retraction → Shrinkage → (final check print)**.

- Uses OrcaSlicer's own test prints, with a choice of method where Orca has several.
- Slice & send straight to the printer, or generate only (SD card).
- Progress bar and before/after table; results go into **one new "(calibrated)" filament preset** — your starting
  preset is never changed.
- **Printer calibration**, once per printer and nozzle: **Input Shaping** (frequency, then damping), **Cornering**
  and **VFA**. The result page turns the height you measure on the tower into the value, and you choose whether it
  updates your printer preset (with a backup you can restore) or goes into a new "(calibrated)" printer preset.
- Translated into all of OrcaSlicer's 22 languages.

<img width="1906" height="1026" alt="Screenshot 2026-10-03 210209" src="https://github.com/user-attachments/assets/d9b69b5b-ec0a-406b-acbf-ba388dec8257" />
  
## Forca Academy (print journal, off by default)
Turn it on in **Preferences → Forca Academy** and every print Forca sends gets its own folder in
`Documents\Forca Academy` (or a folder you choose): the printer, presets and filaments (on Bambu printers, the AMS
tray each one came from), the settings you changed, the estimates and a picture of the plate. While the print runs,
Forca follows it to the end — on Bambu, Klipper (Moonraker) and stock Flashforge printers — and records how it ended,
with pictures from the printer's camera along the way and at the end. **File → Print Journal** lists your prints:
mark each one Success / Partial / Failed with quick tags (stringing, warping, layer shift, ...) and a note, and add
your own photos. "Add a print" records prints Forca didn't send, such as SD-card prints. Everything is plain JSON and
Markdown on your computer, readable by you or an AI you choose; nothing is uploaded, and printer keys and access codes
are never stored.

## Bambu Studio settings
Bambu Lab's profiles and Bambu Studio projects use settings OrcaSlicer doesn't know, so Orca quietly drops them.
Forca understands them, ported from Bambu Studio's open-source code. Prints on other printers don't change unless you
use the new settings.

- **"Reduce infill retraction"** is Disabled / Auto / Enabled, with a filament **"Metal stickiness"** setting: on
  Bambu printers PLA skips retraction inside infill, while sticky filaments (PETG, PCTG, PA, TPU) retract again, as
  in Bambu Studio. (OrcaSlicer retracted on nearly every infill travel on Bambu printers, which made prints slower.)
- **First-layer infill line width**, **filament overhang and bridge speeds**, **short-travel acceleration**, **slow
  down by height**, **pre-start fan time**, **support ironing inset / direction / speed**, **cooling slowdown logic**,
  **top / bottom paint penetration layers** and **"Avoid crossing walls - Includes support"**.
- Four settings Bambu Studio names differently are kept when you open its projects and profiles, and opening a newer
  Bambu Studio project lists any settings Forca still doesn't know instead of just saying there are some.
- **P2S and X2D** print outer walls clockwise, as Bambu Studio does on those printers.

## Open Bamboo Networking (optional)
**Preferences → Bambu network plug-in → "Network plug-in source"** switches between Bambu Lab's network plug-in and
[Open Bamboo Networking](https://github.com/ClusterM/open-bamboo-networking), an open-source replacement that, in
our tests, didn't have the crash Bambu's plug-in sometimes has after sending a print. It needs your printers in
**LAN-only mode with Developer Mode on**, and you lose Bambu's cloud features; Forca explains this before switching,
downloads it from its GitHub releases, checks the file, and keeps the previous version so you can roll back. A switch
takes effect after you close and reopen Forca. Bambu Lab's plug-in stays the default, and if it ever crashes Forca,
the Device tab offers Open Bamboo Networking once.

## Faster toolpaths
- **Shorter travel moves:** Forca orders islands, infill and support so the nozzle travels less between them (up to
  2.5% less travel on test models; print time is never longer).
- **More accurate minimum layer time:** the cooling slowdown counts the time spent accelerating and braking, so small
  layers get the cooling time they're set to without being slowed more than needed. A `G4` dwell in your custom
  G-code counts toward that layer time too.

## Non-Bambu printers
- **Stock Flashforge printers** (Adventurer 5M / 5X ...) have no web interface, so their Device tab used to show
  "refused to connect". Forca shows its own status page instead: state, file, progress, layer, times, temperatures,
  filament, Pause / Resume / Stop and the light, and "Plate is clear" after a print. It needs the printer's access
  code (from its screen) in the printer's network settings.
- **Klipper printers:** a **Search** button in the network settings finds Fluidd or Mainsail on the printer for the
  Device tab, and a printer set up with Moonraker's port 7125 now opens Fluidd / Mainsail instead of Moonraker's API.
- **Filament sync from Happy Hare and Moonraker lane data** includes two OrcaSlicer fixes that aren't merged upstream
  yet (*experimental*). On some setups (KX-Bridge) filaments can still land in the wrong slots or come in as generic
  presets; a fix is in testing.

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

Only you can relax the first two, by choosing the **Advanced** level in the Forca AI window: then the AI may save over
your files and presets (Forca keeps a backup copy of each in `Documents\Forca AI\Backups`), and a print request opens
the print dialog without a card. You still press Send yourself — unless you also give the AI a **print grant**: up to
a number of prints, on printers you tick, for a few hours. Within it Forca sends the AI's print itself, only to an idle
printer whose bed you marked clear, after a 10-second countdown you can cancel. A grant can name the files to print
(with copies and a note), so the AI does the whole job and prints nothing else.

See [SECURITY.md](SECURITY.md) for how it's protected.

<table>
  <tr>
    <td><img src="https://github.com/user-attachments/assets/162c2f1a-9dfe-47eb-952c-39b18ed9629d" width="100%" alt="Screenshot 210254"></td>
    <td><img src="https://github.com/user-attachments/assets/31153daf-cfd9-4cf7-8050-541eeba3229c" width="100%" alt="Screenshot 210332"></td>
  </tr>
</table>

## Smaller changes
- **Its own settings folder** (`%APPDATA%\ForcaSlicer`). On first start Forca offers to copy your OrcaSlicer
  settings; it never changes OrcaSlicer's.
- **No Orca Cloud and no OrcaSlicer update server.** Forca doesn't sign in to or sync with Orca Cloud (the OrcaSlicer
  team's service); your presets stay on your computer. It checks only its own GitHub releases for a newer Forca (not
  in Stealth mode), and bug reports and release links go to Forca, never to the OrcaSlicer team. Bambu Cloud is not
  affected.
- **Import dialog file-type list:** "Supported files", "Model files without 3MF", 3MF, STL, STEP or OBJ; Forca
  remembers your choice.
- **A pause or colour change left above the top of a print** (for example from a taller model the project used to
  hold) no longer fires on the last layer. The layer slider hides such marks, so they couldn't be seen or removed.
- **The input shaping and cornering calibrations switch off overhang slowdown**, so it can't skew those tests.
- **The Max Volumetric Speed test stays on the bed:** its first layer is a thin-walled outline, as in Bambu Studio.
- **A print the printer refused ("Printer is busy") no longer counts as sent.**
- **The installer can create a desktop icon.**


# Install (Windows)
Download one of these from [Releases](https://github.com/tayloraaron078-tech/ForcaSlicer/releases):
- **Installer** — `Forca_Slicer_<version>_win64_setup.exe`: installs to Program Files, adds a Start-menu entry
  (and a desktop icon if you tick it) and an uninstaller.
- **Portable zip** — `Forca_Slicer_<version>_win64_portable.zip`: unzip anywhere (for example `C:\Forca Slicer`)
  and run `forca-slicer.exe` in the `ForcaSlicer` folder. Nothing is installed.

Then:
- The installer and the app are code-signed; Windows should show **Aaron Taylor** as the publisher. While the
  certificate is new, Windows may still show "Windows protected your PC": click **More info → Run anyway**.
  (Compare the SHA-256 on the release page if you want to be sure the file is ours.)
- On first start, Forca can copy your OrcaSlicer settings. It copies — your OrcaSlicer folder is never changed.
- Forca checks this repository's releases for a newer Forca when it starts (not in Stealth mode); nothing else is
  sent.

macOS and Linux builds are not available yet.

# Report problems and ideas
Please use [GitHub Issues](https://github.com/tayloraaron078-tech/ForcaSlicer/issues/new/choose) (the forms ask for exactly what's
needed) and [Discussions](https://github.com/tayloraaron078-tech/ForcaSlicer/discussions) for questions. **Please don't report Forca
problems to OrcaSlicer** — if it's a Forca build, report it here even if you think the bug is Orca's; we'll
forward it upstream if it is. The [tester guide](docs/forca/release/TESTING.md) explains what's most useful.

# How Forca is developed
Forca is developed in a private repository, and each release is published here as a single commit on top of
OrcaSlicer's history. That's why you'll see one commit per release rather than the day-to-day work: experiments,
features that don't pan out, and work in progress stay private until they're tested and ready.

Every release is the complete source code of what you download (AGPL-3.0), and the [changelog](CHANGELOG.md)
describes everything that changed.

**Contributing:** bug reports, ideas and feedback are very welcome in
[Issues](https://github.com/tayloraaron078-tech/ForcaSlicer/issues) and
[Discussions](https://github.com/tayloraaron078-tech/ForcaSlicer/discussions). Pull requests are welcome too; since
the public history is release-only, accepted changes are brought into the development repository and ship in the
next release, with credit to you.

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
- **Open Bamboo Networking** ([github.com/ClusterM/open-bamboo-networking](https://github.com/ClusterM/open-bamboo-networking),
  AGPL-3.0) is an optional open-source replacement for that plug-in. Forca downloads it only if you choose it in
  Preferences.
- No warranty (see the license). You are responsible for what you print — check the preview and the first layers.
