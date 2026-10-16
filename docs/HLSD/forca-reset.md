# Reset Forca data

## Why it exists

The print journal, Forca AI and the Calibration Wizard keep what they learn: records, patterns, keys, permissions,
results. When that history no longer helps (test prints, a new printer, an AI setup the user wants to redo), the user
needs one way to start again without hunting for folders in AppData and Documents, and without losing anything they
might want back.

## What the user sees

"Reset Forca data" is in Preferences (Forca Academy section), the Forca AI window and the Print Journal. It opens one
dialog with five parts, all ticked:

- **Forca itself (like a fresh install):** the whole data folder (settings, user presets, printers, network plug-ins,
  logs) and the local app data folder (the shared Forca AI key). Ticking it covers the Forca AI and Calibration
  Wizard parts, so those two boxes are disabled.
- **Forca Academy print journal:** the Academy folder. It is always Forca's own `Forca Academy` folder; a journal
  kept directly in a folder of the user's (from before Forca made its own folder) moves without the user's other
  files: only the names Forca creates there.
- **Forca AI:** the AI key, activity log, grants and pauses, and the AI settings (switched off, no shared printers,
  the Guarded level).
- **Projects saved by the AI:** the `Documents/Forca AI` folder.
- **Calibration Wizard history:** the wizard's progress and results file. Values saved into presets stay.

"Reset and close Forca" asks once, then closes Forca. The reset happens when Forca next starts. Nothing is deleted:
every part moves to `Documents/Forca reset backups/<date time>/`.

## How it works

`slic3r/GUI/ForcaReset.{hpp,cpp}`, unit-tested in `tests/slic3rutils/test_forca_reset.cpp`.

- **Plan, then close.** The dialog turns the ticked parts into a plan (`forca_reset_plan`): a list of moves into the
  backup folder, plus the app settings to clear. The plan is written next to the data folder, as
  `<data dir>.forca-reset`, and Forca closes the way the user would close it (an unsaved project still asks).
- **Moves at the next start.** `GUI_App::init_app_config` calls `forca_reset_apply_moves` as soon as it knows the data
  folder and before it reads anything from it (the portable, default and `--datadir` paths). A move is a rename, or a
  copy and then removal of the original when a rename isn't possible (another drive). A source that doesn't exist is
  not an error. Afterwards the data folder exists (empty after a fresh-install reset), so Forca starts as on a new
  install, including the first-run wizard.
- **Settings after the config loads.** The Forca AI part also clears app settings, which only exist once the config
  has loaded. The marker then stays with no moves left, and `forca_reset_apply_settings` (end of the config load)
  clears them, saves, removes the marker and reports any move that failed.

## Constraints

- Everything runs at the next start, not while Forca is open: in-memory state (the AI key and activity feed, the
  calibration store, the Academy print follower) would otherwise be written back, and the data folder can't move while
  Forca uses it.
- Nothing is deleted. The only removal is of an original after it was copied into the backup.
- The marker sits outside the data folder, so a fresh-install reset can move the whole folder.
