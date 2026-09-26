# Forca blue retint

Forca recolors OrcaSlicer's grey interface chrome to a blue palette. This document is the human-readable companion to the two data files that define the retint and the scripts that apply it.

## Files

| File | Role |
| --- | --- |
| [`tools/forca/retint_map.tsv`](../../tools/forca/retint_map.tsv) | **Canonical** color mapping: `base-grey<TAB>Forca-blue`, one per line. |
| [`tools/forca/retint_exclude.txt`](../../tools/forca/retint_exclude.txt) | **Canonical** list of files the scripts must not touch. |
| [`tools/forca/apply_retint.ps1`](../../tools/forca/apply_retint.ps1) | Windows (PowerShell 5.1+) applier. |
| [`tools/forca/apply_retint.sh`](../../tools/forca/apply_retint.sh) | POSIX (bash + perl) applier, for macOS/Linux merges. |

Both scripts read the two data files, so the mapping and exclude list live in exactly one place.

## Why a script instead of committed edits

The retint is ~20 hex swaps spread across ~77 GUI/CSS files. Carried as permanent source edits, every one of those files conflicts when OrcaSlicer touches it upstream — a large, low-value merge burden. Instead, the retint is **regenerable**: during an upstream merge you take Orca's version of a tinted file and re-run the script to re-apply the blue in one step. See [`upstream-merge.md`](upstream-merge.md).

> The committed tree still contains the retint (it was not reverted). The scripts are the *tool* used at merge time; they are not run as part of a normal build, and running them on the current tree is not required.

## The mapping

Grey → blue. Darker greys become navy chrome; light greys become pale blue panels.

| Base Orca grey | Forca blue | Typical use |
| --- | --- | --- |
| `#242428` | `#1E2436` | very dark chrome / side tabbar |
| `#262E30` | `#1A2C4C` | primary dark chrome (topbars, dark panels) |
| `#323A3C` | `#29384F` | dark chrome |
| `#323A3D` | `#24395A` | dark chrome |
| `#363636` | `#26395A` | dark chrome / some label text (navy, accepted) |
| `#3B4446` | `#2D4468` | dark chrome / nav tab |
| `#3E3E45` | `#333A4E` | dark-mode counterpart in light/dark color pairs |
| `#CECECE` | `#C2CFE4` | mid-grey borders |
| `#D7D7D7` | `#CDD8EA` | mid-grey |
| `#D9D9D9` | `#CDDAEC` | mid-grey borders |
| `#DBDBDB` | `#CFDBED` | borders |
| `#DFDFDF` | `#D3DEEF` | button backgrounds |
| `#EEEEEE` | `#D7E2F3` | light panel backgrounds |
| `#EFEFF0` | `#E4EBF8` | light panel backgrounds |
| `#F0F0F1` | `#D9E4F5` | light backgrounds |
| `#F1F1F1` | `#DAE5F5` | light backgrounds |
| `#F4F4F4` | `#DDE8F6` | light backgrounds |
| `#F7F7F7` | `#E9EFFA` | light backgrounds |
| `#F8F8F8` | `#E3EBF8` | lightest panel backgrounds |

Text greys (e.g. `#6B6B6B`), accent colors (teal `#009688`, orange `#FF6F00`, reds), and whites are **not** in the mapping — they are deliberately kept.

The mapping is **idempotent** (no Forca-blue value equals any base-grey key, so re-running changes nothing) and **order-independent**.

## Excluded files (never retinted by the scripts)

- **`BitmapCache.cpp`** — holds base-Orca hexes (`#262E30`, `#D9D9D9`, …) as **SVG recolor-table keys**. These must match the hex strings inside the shipped `.svg` files, so retinting them silently breaks dark-mode icon recoloring. This file is byte-identical to upstream and must stay that way. After any merge, verify:
  ```bash
  git diff -- src/slic3r/GUI/BitmapCache.cpp   # should be empty
  ```
- **`GLCanvas3D.cpp`** — the 3D viewport background is hand-tuned decimal RGB constants (`DEFAULT_BG_LIGHT_COLOR` / `_DARK`), not hex literals.
- **`Tab.cpp`** — the settings panel background is theme-aware (`dark_mode() ? navy : light-blue`), hand-written.
- **`GUI_App.cpp`** — splash-screen art and its white centered text overlay.
- **`BBLTopbar.cpp`, `MainFrame.cpp`, `Notebook.cpp`** — titlebar / topbar / nav-tab colors set via decimal `wxColour(...)`. The only hex tokens left in them are inside `// … was #xxxxxx grey` comments, which the scripts must not rewrite.

After an upstream merge that touches any excluded file, re-apply its edit by hand (the diffs are small and documented in [`upstream-merge.md`](upstream-merge.md#hand-edited-retint-files)).

## Running the script

```bash
# Windows
powershell -ExecutionPolicy Bypass -File tools/forca/apply_retint.ps1 -DryRun   # preview
powershell -ExecutionPolicy Bypass -File tools/forca/apply_retint.ps1           # apply

# macOS / Linux
tools/forca/apply_retint.sh --dry-run
tools/forca/apply_retint.sh
```

`-DryRun` / `--dry-run` lists per-file hit counts and writes nothing. Both scripts print a total and a reminder to check `BitmapCache.cpp`.

### Known behavior on the current tree

A dry-run against the current committed tree reports **9 replacements in 3 files** (`StateColor.cpp`, `home.css`, `global.css`). These are greys the original blind sed *missed* — the script would *complete* the retint (e.g. the dark side of light/dark color pairs, a couple of `global.css` variables still on their pass-1 value). Applying them is safe and consistent with the retint's intent, but is not required and has been left unapplied so the committed look is unchanged. It is normal for a post-merge regenerate to also normalize these.

## Extending the retint

To add or change a color: edit `retint_map.tsv` (and this table). To protect a new file: add its basename to `retint_exclude.txt`. Do not hard-code colors or filenames in the scripts — they intentionally carry no mapping of their own.

**Dark mode and numeric colors.** The scripts rewrite only `"#RRGGBB"` strings, so a key of the dark-mode table (`gDarkColors` in `Widgets/StateColor.cpp`) gets retinted while code that passes the same grey as a number (`0x262E30`, `wxColour(38, 46, 48)`) keeps Orca's value, and dark mode would stop converting it (dark text on a dark background). `gForcaRetintedKeys` in the same file aliases each Orca grey to its retinted key, written as RGB numbers so the retint leaves it alone. If a mapping in `retint_map.tsv` changes a `gDarkColors` key, update that alias table too.
