# Forca Slicer — Changelog

All notable changes Forca makes on top of its OrcaSlicer base (the `2.5.0-dev` line, merged up to `8c03985818` on
2026-09-26; originally pinned at `c0c2cc5068`). OrcaSlicer's own changes are not listed here — see the
[OrcaSlicer releases](https://github.com/OrcaSlicer/OrcaSlicer/releases).

Forca has its own version number from `0.1.0-alpha.1` on. Entries up to the first public alpha are grouped by date,
newest first. Keep this file up to date with every change that ships (see
[`docs/forca/README.md`](docs/forca/README.md#keeping-the-changelog)).

Commit hashes in entries up to `0.1.0-alpha.1` refer to Forca's private development history, from before the public
repository; the public repository starts with Forca `0.1.0-alpha.1` as one commit on top of OrcaSlicer's history.
(OrcaSlicer's own commits, such as `8c03985818`, are in it.)

## Unreleased

## 0.1.0-alpha.2 — 2026-10-02

### Added
- **Forca AI control levels.** The Forca AI window now has a level: **Guarded** (the default, unchanged) or
  **Advanced**, which you confirm through a warning. At Advanced the AI may save over your files and edit your user
  presets -- Forca first copies each file into `Documents\Forca AI\Backups` -- and a print request opens the print
  dialog straight away instead of an approval card; you still press Send. Only you can change the level (no AI
  command can); the top bar shows "(Advanced)" while it is on, and `forca_status` tells the AI which rules apply.
- **Forca AI print grants.** At the Advanced level you can give the AI a grant to start prints itself: which printers,
  how many prints, for how many hours, and the longest print. Forca sends a grant print only to an idle printer whose
  bed is clear -- you click "Bed is clear" (one clearing per print), or, if you allow it, the AI checks the bed with
  the camera (`forca_bed_check`; the picture is kept). The AI names the printer slot for every filament (`A4`, `Ext`);
  Forca checks the slot holds that material, sets the print dialog to it and checks it again before sending (the
  dialog's own auto-mapping can pick a different slot). The send goes through the normal print dialog with a 10-second
  countdown you can cancel, and stops wherever the dialog would ask you to confirm something. Bambu printers only for
  now. Grants end when used up, on time, on Revoke, on choosing Guarded, or on restart.
- **Print grants can name the files to print.** Add files to a grant, each with a number of copies and a note for the
  AI (orientation, material, settings). The AI imports them, arranges copies, picks settings following your note and
  prints until every copy is sent; Forca sends only plates made of the grant's files, never more copies than are still
  owed, and shows the progress in the Forca AI window. The grant ends when all copies are sent.
- **Printer calibration in the Calibration Wizard.** Opening the wizard now asks what you are calibrating: a filament
  (as before) or **your printer** -- once per printer and nozzle, before the filaments. A **Calibrate something else**
  button on every wizard page brings the question back (a filament's progress is kept). The printer track runs
  OrcaSlicer's own tests in order: **Input Shaping** (frequency, then damping), **Cornering** (jerk, junction
  deviation or Klipper's square corner velocity) and **VFA** (speeds that cause fine vertical ribbing). The result
  page turns the height you measure on the tower into the value. You choose where results go: **update your printer
  preset** (its old values are backed up and a "Restore this printer's backup" button puts them back) or **a new
  "(calibrated)" printer preset**, which is also added to any network printer the original was on. Klipper printers
  also get the lines for printer.cfg. Bambu Lab printers only get VFA -- they tune their own vibration compensation
  and cornering.
- **The Calibration Wizard speaks your language.** Its text is translated into all 22 of OrcaSlicer's languages
  (machine translations, marked as such in the catalogs; recurring terms follow each language's OrcaSlicer
  wording). Chinese, Japanese and Thai text now wraps inside the wizard instead of stretching it.
- **Forca Academy print journal** (Preferences > Forca Academy, off by default). When on, every print Forca sends --
  to a Bambu printer, or uploaded to a network printer with "print" -- gets a folder in Documents\Forca Academy
  (or a folder you choose): the printer, presets, filaments (and, on Bambu printers, the AMS tray each one printed
  from), settings you changed, estimates and a picture of the
  plate, as plain JSON and Markdown that you and any AI can read. Nothing is uploaded.
  - **File > Print Journal:** your recorded prints with their picture and details; mark each Success / Partial /
    Failed with quick tags (stringing, warping, layer shift, ...) and a note, and add photos (or drop them on the
    window). "Add a print" records the plate for prints Forca didn't send, such as SD-card prints.
  - **Calibration history:** results you apply in the Calibration Wizard are added to the material's (or printer's)
    page in the journal.
  - **How it ended (Bambu):** Forca follows each recorded print until the printer reports it finished or failed
    (even across a restart), and adds that, the time it took and any printer errors to the record -- plus, if you
    like, a camera picture of the result (on by default) and pictures every few minutes while it prints (off by
    default; Preferences > Forca Academy).
  - **Printer pages:** each printer gets a page with what Forca knows about it (model, firmware, connection, AMS
    units, camera, preset), refreshed on every print; your own notes on the page are kept.
  - **Forca AI tools:** an AI can check the journal's status, list, read and search it, record a print's outcome
    (never over yours), write its own notes, lessons and playbooks (it can add to your notes but never replace
    them), and save a camera picture with a print (`forca_camera_snapshot` `save_to_print`).

### Fixed
- **Max Volumetric Speed test stays on the bed.** Its first layer is now a thin-walled outline, like Bambu Studio's
  test model, so the brim goes on both sides of the wall instead of only outside. Before, filaments such as PCTG could
  lift off partway through. The wall follows your nozzle's line width; above the first layer the test prints as
  before. Applies to the Calibration Wizard and to Calibration > Max flowrate.
- **Calibration Wizard pages open at the top.** Before, a new step often opened scrolled to the bottom (wherever
  the button you pressed was), so you had to scroll up to read it.
- **Forca AI's G-code export applies your post-processing scripts**, exactly like File → Export G-code. Before, an AI
  export skipped them on non-Bambu printers. If a script renames the output onto one of your files, the AI still
  refuses to overwrite it.
- **Two Preferences tooltips are translated again** (Stealth mode and Enable Bambu Cloud), in all 22 languages. They
  had fallen back to English when Forca reworded them for Orca Cloud being off.

## 0.1.0-alpha.1 — 2026-09-26 — first public alpha

Everything below, from the first dated section on, is in this release.

### Added
- **Update check against Forca's own releases.** At start (not in Stealth mode) and from Help → Check for Updates,
  Forca asks its GitHub releases whether a newer Forca exists, alphas included. Nothing else is sent.

### Fixed
- **Setup wizard: the "Get Started" button was hidden** below the Forca logo on the welcome page; the logo now fits.
  (`07b5526ef5`)
- **The installer offers a desktop icon.** Its new "Install Options" page has a "Create Forca Slicer Desktop Icon"
  checkbox (and a system-PATH choice that defaults to leaving PATH alone). (`07b5526ef5`)

## 2026-09-26 — OrcaSlicer merged up to `8c03985818`, CAD tab, About box (`1cfc86a92a`, `e372c6f70b`)

### Merged
- **Merged OrcaSlicer up to `8c03985818`** (upstream `main`, 2026-09-26; 836 commits past the old base). Everything
  OrcaSlicer added in between comes with it, including the new keyboard-shortcut system and its **experimental
  Design (CAD) tab**, which stays off until you turn on "enable CAD feature" in Preferences (restart needed). Forca's
  own features (regional support interface, Calibration Wizard, Forca AI, the Forca look and identity) carry over.

### Fixed
- **Dark mode: text in drop-down lists and text boxes was invisible** (dark text on a dark background, e.g. the
  printer, filament and process presets). The Forca blue recolour had stopped dark mode from recognising Orca's
  standard text colour; it converts again.
- **Calibration Wizard tab follows light/dark mode switches.** It used to keep the colours it was opened with, so
  after switching dark mode off it stayed partly dark.
- **Uploads to UltiMaker printers.** The fix-up OrcaSlicer applies to the G-code header for UltiMaker printers
  (colons in the date can crash them) only recognised OrcaSlicer's header, not Forca's; it now does. The printer's
  pairing request also names Forca Slicer.

### Changed
- **About box and License Info.** The About box now shows Forca's own notice (© 2026 A.T. Creations, GNU AGPL-3.0),
  keeps OrcaSlicer's copyright notice, links Forca's source code first and credits OrcaSlicer. License Info adds where
  the source code is, the pressure-advance-pattern (GPL-3.0) and Bambu network plug-in notices, and OrcaSlicer, Bambu
  Studio and Slic3r to the credits.

## 2026-09-25 (night) — Forca's own version, links to Forca (`dd32180a6b`, `426f78251b`)

### Changed
- **Forca has its own version number, starting at `0.1.0-alpha.1`.** It is what the splash screen, About box,
  Troubleshoot Center, crash report, installer, release zip and the G-code header (`; generated by Forca Slicer
  0.1.0-alpha.1 on …`) show; the About box also names the OrcaSlicer version Forca is built on. Project files and
  profiles keep OrcaSlicer's internal version, so they stay compatible. (`dd32180a6b`)
- **Bug reports and release links go to Forca, never to OrcaSlicer.** Help → "Report a Bug or Request a Feature"
  (new) and the Troubleshoot Center's "Report issue" open Forca's own GitHub issue forms (the version and OS are
  filled in); the "newer 3MF" dialog links to Forca's releases; the Network Test checks Forca's GitHub page; Klipper
  (Moonraker) sees Forca connect as "ForcaSlicer". OrcaSlicer's online help (wiki) links stay, since they document
  the OrcaSlicer features Forca is built on. (`426f78251b`)

## 2026-09-25 (later) — no Orca Cloud, no OrcaSlicer update checks (`113a719702`, `15ad776409`)

### Removed
- **Orca Cloud.** Orca Cloud is the OrcaSlicer team's own online service (sign-in, preset sync, cloud bundles and
  plugins), so Forca no longer uses it: no Orca Cloud sign-in on the home page, no Sync Presets / auto-sync, no
  sync prompts, and Forca never contacts Orca Cloud or touches the Orca Cloud sign-in that stock OrcaSlicer keeps
  on your computer. Your presets stay on your computer (preset bundles still export and import). If your presets
  lived under an Orca Cloud account, Forca moves copies of them into its local presets once, after backing up the
  whole preset folder (`user_backup-forca-orca-cloud-off`). Bambu Cloud is not affected. (`113a719702`)
- **OrcaSlicer update and profile checks.** Forca no longer asks OrcaSlicer's update server for new versions (it
  would have offered OrcaSlicer releases as updates) or for online printer-profile updates. Forca's printer profiles
  come with each Forca build. Help → Check for Updates says where Forca's releases will be announced; automatic
  checks against Forca's own GitHub releases arrive with the public release. (`15ad776409`)

## 2026-09-25 — Forca AI printers + Forca's own identity (`2c88766e62`, `c6446a18a6`, `5c731ff32b`, `571f4ec5c0`)

### Added
- **Forca AI — printers (phase 3, read-only):** a new "Printers the AI may use" list in the Forca AI window; the
  AI sees only the printers you tick (all off by default). For those it can read live status — state, job,
  progress, time left, layer, nozzle/bed/chamber temperatures, print errors and health (HMS) messages — and take a
  camera snapshot over the local network. When Forca's Device tab live view is showing that printer, the AI uses
  that picture instead of opening a second camera connection. The printer's address and access code are never
  shown to the AI, and nothing here sends a command to a printer. (`2c88766e62`)
- **Forca AI — safety pause (phase 3):** a separate "AI may pause" tick per shared printer (off by default; it
  only counts while the printer is shared, and unsharing clears it). The AI can then pause a running print when it
  sees a problem, and must give a reason. Forca saves a camera frame of the moment, logs it in the activity feed
  and shows a card with **Resume print** / **Keep paused**. The AI may resume only its own pause, while the print
  is still in it — never a pause you or the printer made. Nothing else (stop, cancel, heat, move) is allowed.
  (`c6446a18a6`)

### Changed
- **Forca now has its own settings folder** (`%APPDATA%\ForcaSlicer` on Windows) instead of sharing OrcaSlicer's,
  so running both never mixes their profiles or preferences. On the first start Forca offers once to **copy** your
  OrcaSlicer printers, filaments, process presets and preferences (OrcaSlicer's folder is only read, never changed;
  logs, caches, preset backups and the Orca cloud login are not copied). **If you already used Forca, say Yes** — your Forca
  calibration results and Forca AI settings lived in the shared folder and come along with the copy. (`5c731ff32b`)
- Forca no longer takes over OrcaSlicer's Windows file associations or `orcaslicer://` web links: its `.3mf` /
  `.stl` / `.step` associations use their own "Forca Slicer" entry (off after a copy — turn them on in Preferences),
  and "Open in OrcaSlicer" links go to Forca only if you tick `orcaslicer://` in Preferences. The Windows installer
  has its own name, folder and uninstall entry, so installing Forca no longer replaces an installed OrcaSlicer.
  (`5c731ff32b`)
- The program is now `forca-slicer.exe` (with `ForcaSlicer.dll`) in a `ForcaSlicer` folder, instead of
  `orca-slicer.exe` in an `OrcaSlicer` folder. (`5c731ff32b`)
- The interface says **Forca Slicer** wherever it named OrcaSlicer (preferences, dialogs, messages, window title,
  setup wizard and home page), in every language. Orca's own services and models keep their names (Orca Cloud, the
  Orca calibration models), and the About box still credits OrcaSlicer. (`5c731ff32b`)
- Forca's logo replaces the remaining Orca icons (window, dialog title bars, tray, message dialogs, setup wizard),
  and the info / question / warning icons no longer carry Orca's little orca. (`571f4ec5c0`)

### Fixed
- Forca AI: a camera request the printer never answered (it allows one viewer at a time) could stall every AI tool
  and stop Forca from closing. Camera requests now run separately with a time limit, and Forca always exits.
  (`2c88766e62`)
- About box: two Forca lines showed garbled characters in place of `·` and `—`. (`571f4ec5c0`)

## 2026-09-24 — Forca AI (`fac996af38`, `2ebfe64aef`, `b31e049d8a`)

### Added
- **Forca AI** — an AI assistant such as Claude Code can connect to Forca and work in it, the way Blender's AI
  connection works. Opt-in (off until turned on in the new **Forca AI** window in the top bar), this computer only,
  protected by a secret key, and every AI action appears in that window's activity feed. (`fac996af38`, `2ebfe64aef`)
  - **See:** status, plates and objects, presets and their settings, rendered plate images, screenshots of the
    Forca window.
  - **Operate:** import models (placed apart, never overlapping what is there), move/rotate/scale, add copies,
    remove, arrange, auto-orient; switch presets and create its own; per-object settings; modifiers, support
    blockers/enforcers and Forca support-interface regions; slice and read the result (time, filament per slot,
    layers, warnings); switch 3D/Preview and the camera; save a project copy; export G-code.
  - **Ask to print:** the AI can request a print; Forca shows an approval card (plate picture, printer,
    filaments, time, weight, the AI's reason). Only the user's click on **Approve** acts on it — once, for that exact
    slice (a re-slice or printer change voids it) — and it then opens Forca's normal print dialog for the user to send.
  - **Safety rules built into Forca:** AI saves and AI presets always get new "(Claude <date>)" names and never
    overwrite the user's files or presets (the AI may update only its own unchanged files, tracked in a ledger); the
    AI never discards the user's unsaved preset edits; every AI edit is one "AI: …" undo step, and the AI's own undo
    only undoes AI steps; printer secrets are never shown to the AI. The file-safety code is unit-tested
    (`tests/libslic3r/test_forca_ai_safe_path.cpp`).

### Fixed
- Forca AI: saving or exporting as the AI could freeze Forca (a file-name check looped forever). (`2ebfe64aef`)
- Forca AI: a multi-file import stacked every model on the plate centre; the first slice right after an import
  was reported as done although Forca had discarded it; AI arrange/orient could not be undone by the AI; a
  "bed too hot for this filament" warning was reported as a tip. (`b31e049d8a`)

## 2026-09-23 — Calibration Wizard tab (`178a5026b0`)

### Added
- **Calibration Wizard tab** — the wizard now lives in its own main tab (after Calibration) instead of a floating
  window. Left: the wizard, with the Forca banner below it. Top right: a progress bar across the six core steps
  (percent centered, Flow fills in two halves, clickable step labels, optional-step markers) and a before/after
  table of every calibrated value (changed values highlighted with the difference, per-row status, measured date on
  hover). Bottom right: the real build plate — the 3D view of the test, then the Preview once sliced.
- Resizable, remembered splits between the wizard, the progress panel and the plate.
- **Start page** — pick the filament (or a close profile to start from), then Start; it continues at the first
  unfinished step. Shown on first open and when returning to the tab after finishing a run.
- **Calibrate another filament** on the final-check page and the finish card.
- **Finish card** — "Calibration complete" summary that can be saved as a PNG image.
- **Skip this step** on every calibration (keeps the current value; the step can be re-run later).
- Warning banner when the printer or the active filament changes in another tab mid-run, with "Switch back".
- Results store: per-run "before" snapshot, skipped steps, and the flow pass count (older result files still load).
- The top Calibration menu's **Calibration Wizard** entry now opens the tab.

### Changed
- Launching a test, Slice & send, the top-bar Slice button and Ctrl+R keep you on the Calibration Wizard tab
  (the plate switches to Preview in place); a Bambu send no longer jumps to the Device tab from this tab.
- Leaving the tab mid-calibration and coming back continues exactly where you were.

### Fixed
- Calibrated values are written to **every extruder variant** of the filament preset (e.g. the H2S's Direct Drive
  Standard and High Flow). Presets saved with a single value made Forca throw "Assigning from an empty vector" as soon
  as a second filament was selected. _Presets calibrated before this fix should be deleted and re-calibrated._
- Creating a new calibrated preset under a name a deleted preset used before no longer inherits that preset's history.
- Opening the tab no longer crashes (the wizard's internal page changes were being taken for main-tab changes).

## 2026-09-23 — Calibration Wizard: complete per-filament sequence (`72a5020b4f`, `00d5f4d4a3`)

### Added
- **Pressure Advance method choice:** PA Tower (default), PA Line or PA Pattern, with Orca's per-method defaults
  for direct drive and Bowden; Pattern takes optional acceleration/speed lists. PA Tower result helper:
  `PA = start + step × measured height`.
- **Flow Rate method choice:** Classic (coarse + fine pass) or Orca YOLO (Recommended + Perfectionist).
- **Optional flow recheck** after Pressure Advance.
- **Retraction** step (Orca's retraction tower): count the rings below the first string-free section;
  `length = start + rings × step` (verified against a sliced tower). Writes the per-filament retraction length.
- **Shrinkage** step with Forca's own test (a 100 mm open frame with a 40 mm post, generated in code): enter the
  measured X, Y (and optionally Z) to set the filament's XY and Z shrinkage compensation.
- **Final check print** (optional): Orca's Autodesk FDM Test.

### Fixed
- PA Pattern froze Forca on the next re-slice (for example when opening Preview after sending).

## 2026-09-20 — Calibration Wizard (`4ffca2356b` … `d20c0ae54c`)

### Added
- **Forca Calibration Wizard** — native, guided per-filament calibration that drives Orca's own test generators:
  Temperature → Max Volumetric Speed → Flow Rate (2-pass) → Pressure Advance (Line).
- Each result is written into **one named "(calibrated)" filament preset per run** (non-destructive: the starting
  preset is untouched) and selected.
- **Slice & send** (slices and opens your printer's send dialog, then asks for the result) or **Generate only**
  (for SD-card / manual printing).
- Resumable runs: a JSON results store in the data folder remembers pending tests and results across restarts.

## 2026-09-20 — Fork maintenance (`2e57b41d0b`, `52f00ed0df`)

### Added
- README rewritten for Forca; `docs/forca/` fork-maintenance docs (divergence map, upstream-merge runbook, retint).
- `tools/forca/apply_retint.*` — regenerates the blue retint after an upstream merge.

## 2026-09-19 — Organic regional gap, gizmo comfort controls, release packager (`9acb155965`, `072daeabf8`, `9298355aef`)

### Added
- Regional flush 0 mm support-interface gap on **Organic** tree supports — the regional feature now covers every
  support style.
- Support-interface paint gizmo: **Vertical / Horizontal** stroke locking and **On overhangs only** (with the
  overhang highlight slider).
- Local Windows release packager (`build_forca_release.ps1` / `.bat`): builds a dated portable zip.

## 2026-09-18 — Forca Slicer (`86802ddb1e`)

### Changed
- Rebranded to **Forca Slicer** (name, icon, splash, About) with an app-wide blue retint. Internal keys stay
  `OrcaSlicer`, so the data folder, profiles and `.3mf` projects are unchanged.

## 2026-09-16/17 — Paint-on regions and multiple materials (`cabd049f92`, `588827342a` … `097cd34aa9`)

### Added
- **N-region material:** multiple support-interface modifiers, each with its own interface filament.
- **Paint-on support-interface regions:** a paint gizmo with a filament palette; each painted filament becomes its
  own interface region with a flush release gap. Painting round-trips through `.3mf`.

### Fixed
- Object support never reuses a filament reserved for a regional interface (base and interface guards).

## 2026-09-13 … 09-15 — Tree supports, suggestions, reliability (`0ed7c50f45` … `793ce79883`)

### Added
- Regional Top-Z gap for legacy tree supports (Slim, Strong, Hybrid).
- Suggestion dialog when a dissimilar interface filament is assigned to a modifier (offers the companion settings for
  a clean release).
- Assigning a dissimilar interface filament seeds a 0 mm regional gap.

### Fixed
- Deterministic regional interface material: a footprint-exact clip replaces the old per-point classification
  (the material no longer changes between slices). Validated on a real print.

## 2026-09-06 … 09-08 — Regional support interface (`bdaba82502` … `b3fb89d77f`)

### Added
- **Support-interface modifier** volume type: a placed modifier gives its region its own interface filament.
- Regional support Top-Z gap on the modifier, including zero gap, for classic (Grid / Snug) supports.
- The modifier's filament is shown in the object list; changing it re-slices.
