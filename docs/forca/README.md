# Forca Slicer — fork maintenance

Forca Slicer is a fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). This folder documents how the fork is *maintained* — specifically how to pull in upstream OrcaSlicer changes without fighting Forca's own modifications by hand every time.

If you're looking for what Forca *is*, see the top-level [`README.md`](../../README.md).

## Documents here

| Doc | What it covers |
| --- | --- |
| [`upstream-merge.md`](upstream-merge.md) | The exact `git fetch upstream` → merge → resolve → rebuild → validate procedure. **Start here when you want to pull in newer Orca.** |
| [`retint.md`](retint.md) | The blue-retint color mapping and the `tools/forca/apply_retint.*` scripts that regenerate it after a merge. |
| [`../../CHANGELOG.md`](../../CHANGELOG.md) | What Forca changed and when (newest first). See [Keeping the changelog](#keeping-the-changelog). |

## How Forca diverges from upstream (the maintenance-relevant map)

Forca sits on a **pinned OrcaSlicer base** (`c0c2cc5068`, the `2.5.0-dev` line) plus the fork's own commits. The changes fall into six buckets, and each is resolved differently during an upstream merge:

1. **Feature — the regional support-interface engine + gizmo.** The crown jewels. Deep edits in the slicing engine and a paint gizmo. Merge conflicts here are rare but dangerous; resolve by hand and re-validate on hardware.
2. **Feature — the Calibration Wizard tab.** Mostly Forca-only files (`src/slic3r/GUI/ForcaCalibration*.{cpp,hpp}`, the banner image), plus small, commented hooks in `Plater.{cpp,hpp}` and `MainFrame.{cpp,hpp}` (default-off `skip_confirm` parameters on the `calib_*` launchers, the tab page, and the stay-on-tab redirects). It drives Orca's own calibration generators, so upstream changes to those (`Plater::calib_*`, `calib.hpp`) are what to watch.
3. **Feature — Forca AI.** Forca-only files: the MCP server and tools `src/slic3r/GUI/ForcaAI*.{cpp,hpp}` (`ForcaAI`, `ForcaAITools` = read tools, `ForcaAIHands` = editing tools, `ForcaAIPrint` = the print-approval card, `ForcaAIPrinters` = printer opt-in, status, camera and the safety pause, `ForcaAIFiles`, `ForcaAIDialog`), the file-safety core `src/libslic3r/ForcaAISafePath.{cpp,hpp}` + its test `tests/libslic3r/test_forca_ai_safe_path.cpp`, and the icons `resources/images/forca_ai*.svg`. Small, commented hooks: the top-bar button (`BBLTopbar`), `MainFrame::show_forca_ai`, start/stop in `GUI_App`, one Plater hook (`set_forca_ai_slice_listener`), and two camera hooks (`wxMediaCtrl3::ForcaCurrentFrame`, `StatusBasePanel::forca_media_ctrl`). It drives Plater, presets and Orca's own print dialog through their public APIs, so upstream changes to those are what to watch. Its two safety rules (never overwrite the user's files; print only after the user's click in Forca) live in this code, not in AI instructions — keep them intact.
4. **Branding.** App name, icon, splash, About text, wordmark/logo images. Keep Forca's version; **do not** "fix" the internal `SLIC3R_APP_KEY`, which intentionally stays `"OrcaSlicer"` so the config-file name, translation catalog and `.3mf` identity are unchanged. Forca's *settings folder* is its own (`ForcaSlicer`, `FORCA_DATA_DIR_NAME` in `GUI_App.cpp`, with a one-time "copy my OrcaSlicer settings" offer), and it has its own Windows file-association ProgID (`Forca.Slicer.1`) and installer identity (`CPACK_*` in the root `CMakeLists.txt`), so it never takes over a stock OrcaSlicer install.
5. **Blue retint.** ~20 hex color swaps across ~77 GUI/CSS files. This is the single biggest merge-noise source, and it is why the [`apply_retint`](retint.md) scripts exist: during a merge you take upstream's version of a tinted file and *regenerate* the tint instead of hand-merging colors.
6. **Build/docs tooling.** `build_forca_release.*`, `tools/forca/`, `docs/forca/`, `README.md`, `CHANGELOG.md`. Keep Forca's.

The full per-file categorization lives in [`upstream-merge.md`](upstream-merge.md#conflict-resolution-by-file-category).

## Merge philosophy

- **Merges are occasional and deliberate, not continuous.** There is no requirement to track every Orca release. Merge only when upstream ships something worth having, and treat each merge as one focused session ending in a full rebuild + validation.
- **`upstream` (OrcaSlicer) is pull-only.** Never push to it; Forca's work lives in its own repo (`origin`, [tayloraaron078-tech/ForcaSlicer](https://github.com/tayloraaron078-tech/ForcaSlicer)). Every push needs the maintainer's explicit approval.
- **The feature must stay inert when unused** — a no-modifier / no-paint slice must behave like stock Orca. Any merge that could disturb that is re-validated with a plain support model.

## Commit conventions (keep merges cheap)

Keep the six buckets above in **separate commits**. A cosmetic retint change and a slicing-engine change should never share a commit. Rationale:

- A future upstream merge can reason about (and, for the retint, regenerate) a cosmetic commit independently of the engine work.
- If a merge breaks something, an isolated history makes it obvious whether the regression is in the feature, the branding, or the retint.
- The retint in particular is *regenerable* (see [`retint.md`](retint.md)) — keeping it out of feature commits means you can always rebuild it from the mapping rather than untangling it from logic.

Suggested prefixes: `feat:` (a feature — say which, e.g. "calibration wizard …", "Forca AI …"), `fix:`, `chore(brand):` (name/icon/splash/about), `chore(retint):` (color swaps), `build:`/`docs:` (tooling & docs).

## Keeping the changelog

[`CHANGELOG.md`](../../CHANGELOG.md) records every user-visible change Forca makes on top of Orca, newest first.

- Add to the **Unreleased** section in the same commit as the change (or the commit that lands a batch).
- When a batch ships (pushed and `safe/known-good` advanced), rename **Unreleased** to a dated heading with the commit(s), and start a new empty **Unreleased**.
- Group entries under **Added / Changed / Fixed**; write them for someone using Forca, not for someone reading the code.
- Upstream Orca changes pulled in by a merge get one line ("Merged OrcaSlicer up to `<commit or tag>`") — not Orca's own changelog.
- Forca has its own version (`FORCA_VERSION` in `version.inc`, starting at `0.1.0-alpha.1`); from the first public release on, name the headings after the version (`## 0.1.0-alpha.1 — <date>`).
