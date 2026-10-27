# Importing presets from OrcaSlicer, Bambu Studio and Forca

## Why it exists

Filament makers, sites and repositories publish ready-made profiles, often only for Bambu Studio, and users keep their
own in more than one slicer. Starting from one of those is usually closer to right than starting from a generic
profile. OrcaSlicer, Bambu Studio and Forca save a user preset in the same JSON format (a name, `inherits` for the
preset it is based on, a `version`, and only the settings that differ from that parent), so the file can be read as
it is. What differs between them is which parents exist, which printers are installed, and a few settings each slicer
names or stores its own way. The import makes those differences visible and lets the user settle them, instead of
skipping a file or dropping settings without a word.

## What the user sees

File > Import > Import Configs, or dropping files on the window, accepts `.json` presets, OrcaSlicer bundles
(`.orca_filament`, `.orca_printer`, `.orca_bundle`, `.zip`) and Bambu Studio bundles (`.bbsflmt`, `.bbscfg`).

Every single preset file, and every filament inside a Bambu Studio bundle, goes through the **import review** before
anything is saved. One row per file:

- **Name**: the file's, or the same name with "(imported)" when a preset already has it. Typing the name of one of
  the user's own presets asks before replacing it; a system preset's name is refused.
- **Based on**: the parent the import uses. When Forca has it, it is shown; when Forca knows it under another name (a
  renamed system preset, or a "Generic" filament of another printer, which maps to the filament library), the match
  is shown; when Forca doesn't have it, the best match is suggested (the same name for another printer, then the same
  vendor and type, then "Generic" of the type, each for one of the file's printers) and every system filament of that
  type for those printers can be picked. A file with no parent is a full preset of its own.
- **Printer**: "As in the file", or one of the installed printers. When none of the file's printers is installed,
  the selected printer is offered first, so the preset doesn't import invisibly.
- A status line (from Bambu Studio, missing parent, settings Forca doesn't have, renamed) and **Details**: the
  settings the file changes next to the parent's values, the ones Forca knows under another name, the ones Forca
  doesn't have, and the printers that aren't installed.

Settings Forca doesn't have are not lost: the import writes them, with their values, into the filament's notes.

The result message says how many presets were imported and, for each file that wasn't, why: it couldn't be read as a
preset, the preset it is based on is missing, or a system preset already has its name.

**Preset repair.** A user preset whose parent no longer exists (removed from a vendor profile, or a file copied from
elsewhere) can't be loaded. At start-up Forca says so in a notification; "Repair them" opens the same review for those
files, worded as a repair, with a parent suggested for each. Nothing changes unless the user repairs.

## How it works

- `libslic3r/ForcaPresetImport`: `forca_analyse_preset_file()` reads a file without changing anything and returns
  what the review shows (`PresetImportItem`). `forca_import_preset()` writes a copy of the file with the user's
  choices (name, `inherits`, `compatible_printers`, the notes) and imports that copy through
  `PresetBundle::import_json_presets()`, the same path as before the review, so an imported preset is exactly what
  loads at the next start. Bambu Studio bundles are unpacked by `forca_extract_bambu_bundle()` into a temporary
  folder: filaments go to the review, process presets import directly, and the bundle's printer, which is the
  installed system printer itself, is left out.
- **Bambu Studio's "nil".** For a printer with nozzle variants (Standard, High Flow, ...), Bambu Studio writes `nil`
  where a variant keeps its parent's value. Options that can't hold nil (nozzle temperatures, max volumetric speed)
  would reject the whole file, and at start-up the loader would even delete it. With
  `ConfigSubstitutionContext::nil_items` set, `load_from_json()` takes such an item as the value's first other item
  and records its position; `Preset::restore_nil_items()` then puts the parent's value back, matched by variant
  name. Without a parent, `Preset::fill_root_nil_items()` gives a per-variant option's nil the option's first value;
  a filament override's nil ("no override") is kept. Import and start-up both merge a file into its parent per nozzle
  variant (`update_diff_values_to_child_config()`).
- `PresetCollection::presets_missing_parent()` lists the user presets the start-up load skipped for a missing parent;
  `forca_offer_preset_repair()` offers them.
- Renamed settings come from `PrintConfigDef::handle_legacy()`; a setting it blanks is one Forca doesn't have.

## Limits

- The review covers filament presets; process and printer presets in the same files import as before.
- Bambu Studio's per-filament hole and contour compensation and its per-filament scarf seam settings have no Forca
  equivalent yet, so they end up in the notes.
- A file without a `version` can't be read as a preset.
