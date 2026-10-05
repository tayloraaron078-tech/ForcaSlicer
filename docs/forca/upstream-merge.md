# Pulling in upstream OrcaSlicer changes

This is the step-by-step procedure for merging newer OrcaSlicer into Forca. Do it as one focused session that ends in a full rebuild and validation. Merges are **occasional and deliberate** — there is no need to track every Orca release.

## Ground rules

- **`upstream` (OrcaSlicer) is pull-only** — never push to it. `origin` is Forca's own repo. Every `push` needs the maintainer's explicit approval.
- Forca's base is the `2.5.0-dev` line: originally pinned at `c0c2cc5068`, merged up to `8c03985818` on 2026-09-26. **Merge forward only** — a newer dev commit, or the `2.5.0` stable tag once it exists. Do not merge an *older* stable release (it would be a downgrade).
- Never merge directly on `Multi_Interface`. Always use a throwaway merge branch so you can abort cleanly.
- Builds are run locally by the maintainer; the exact build/run/log commands are in [§7](#7-rebuild-full) and [§8](#8-validate).

## Remotes (expected layout)

```
origin    https://github.com/tayloraaron078-tech/ForcaSlicer.git                     (Forca)
upstream  https://github.com/SoftFever/OrcaSlicer.git                              (pull only)
```

---

## 1. Preconditions

```bash
git switch Multi_Interface
git status                     # working tree must be clean; commit or stash WIP first
git rev-parse origin/safe/known-good   # note the current safe point, in case you need to bail
```

## 2. Fetch upstream and pick a target

```bash
git fetch upstream
git log --oneline --decorate upstream/main | head -40   # choose a target commit, or a version tag
git tag -l | grep -i '^v2\.' | sort -V | tail            # list upstream version tags
```

Pick a specific target (a commit hash or a `vX.Y.Z` tag), not a moving branch, so the merge is reproducible. Call it `<TARGET>` below.

## 3. Create a merge branch

```bash
git switch -c merge/upstream-$(date +%Y%m%d) Multi_Interface
```

## 4. Merge and expect conflicts

```bash
git merge <TARGET>
```

Do **not** commit yet. Resolve conflicts by category (next section). If it goes wrong at any point:

```bash
git merge --abort                                   # undo the in-progress merge
git switch Multi_Interface
git branch -D merge/upstream-YYYYMMDD               # discard the merge branch
```

## 5. Conflict resolution by file category

Forca's divergence falls into a few buckets. Resolve each the same way every time.

| Category | Files | How to resolve |
| --- | --- | --- |
| **Branding — keep ours** | `version.inc`, `src/libslic3r/libslic3r.h`, `src/slic3r/GUI/AboutDialog.cpp`, `resources/images/OrcaSlicer*.{ico,png,svg}`, `resources/images/forca_splash.png`, `resources/web/image/logo.png` | Keep Forca's version. **Do not** change `SLIC3R_APP_KEY` — it intentionally stays `"OrcaSlicer"` so the config-file name, translations and `.3mf` identity are stable. Keep Forca's separation from stock Orca in `GUI_App.cpp` (`FORCA_DATA_DIR_NAME` in `init_app_config`, the copy-Orca-settings offer in `on_init_inner`, `FORCA_PROG_ID` in the association functions, no automatic `associate_url(L"orcaslicer")`), the `orcaslicer` row in `Preferences.cpp`'s web-link section, and the Forca `CPACK_*` values + install-prefix default in the root `CMakeLists.txt`. The exe/DLL are `forca-slicer.exe` / `ForcaSlicer.dll` (`OUTPUT_NAME`s in `src/CMakeLists.txt`, the DLL name in `src/OrcaSlicer_app_msvc.cpp`, the exe name in `Process.cpp` and `BBLNetworkPlugin.cpp`). The displayed product name comes from `forca_brand()` in `src/slic3r/GUI/I18N.cpp` (every `translate()` in `I18N.hpp` goes through it, test `tests/slic3rutils/test_forca_brand.cpp`) and `ForcaBrand()` in `resources/web/data/text.js` — so upstream's new "OrcaSlicer" strings are renamed automatically; only new *untranslated* literals (dialog titles etc.) need a manual look. Forca also replaced icons: the `OrcaSlicer*` logo/icon files listed in `tools/forca/check_identity.ps1`, and it removed Orca's orca-and-wave drawing from `info` / `question` / `exclamation` / `notification_slicing_complete.svg` (take ours on conflict). Orca Cloud is switched off (`FORCA_ORCA_CLOUD_ENABLED = false` in `src/slic3r/Utils/ForcaFeatures.hpp`): the six HTTP helpers, `start()` and the stored-secret functions in `OrcaCloudServiceAgent.cpp` return early, GUI entry points are gated on the flag (`GUI_App.cpp`, `MainFrame.cpp`, `Preferences.cpp`, `CreatePresetsDialog.cpp`), the home page hides the Orca Cloud section, and `libslic3r/ForcaPresetMerge.{hpp,cpp}` (test `tests/libslic3r/test_forca_preset_merge.cpp`) merges account presets into `user/default` once. If upstream adds a new Orca Cloud HTTP helper or entry point, give it the same guard. Likewise Forca never contacts Orca's update server: `GUI_App::check_new_version_sf` uses `FORCA_RELEASES_API_URL` (Orca's query/signature code is kept under `#if 0`) and `PresetUpdater::priv::sync_vendor_config` returns early on `FORCA_ORCA_PROFILE_UPDATES_ENABLED` — keep both if upstream reworks the updater. **After every merge run `tools/forca/check_identity.ps1`** ([§8](#8-validate)): it is the executable list of all these identity changes, fails on any the merge lost, and lists new untranslated "Orca" literals to review. If upstream bumped the version number, take upstream's number but keep the `"Forca Slicer"` name/full-name strings. Forca's **own** version is `FORCA_VERSION` in `version.inc` (→ `libslic3r_version.h.in`): it is shown to users (splash, About, Troubleshoot, crash text, G-code header via `header_slic3r_generated()`, installer, release zip) and compared by the update check; `SLIC3R_VERSION` / `SoftFever_VERSION` stay Orca's for file/profile compatibility. Keep the G-code header version one word (Moonraker parses `<name> <version> on`). Links for bug reports, releases and "about Forca" use `FORCA_REPO_URL` (`ForcaFeatures.hpp`): Troubleshoot "Report issue" + build-hash link, Help → "Report a Bug or Request a Feature", the newer-3MF dialog, Network Test, Moonraker identify. `check_identity.ps1` fails on any new link to OrcaSlicer's GitHub repo in `src/slic3r`; orcaslicer.com **wiki** links are fine. |
| **Feature — engine (careful, by hand)** | `src/libslic3r/GCode.cpp`, `GCode/ToolOrdering.cpp`, `Model.{cpp,hpp}`, `Print.{cpp,hpp}`, `PrintApply.cpp`, `PrintObjectSlice.cpp`, `Support/SupportMaterial.{cpp,hpp}`, `Support/SupportLayer.hpp`, `Support/TreeSupport.{cpp,hpp}`, `Support/TreeSupport3D.cpp`, `TriangleSelector.hpp`, `Format/3mf.cpp`, `Format/bbs_3mf.cpp` | The crown jewels. Merge upstream's changes **into** Forca's, preserving the regional support-interface logic. Invariants to protect: the `SUPPORT_INTERFACE_MODIFIER` `ModelVolumeType` stays **at the end of the enum**; the 5th `FacetsAnnotation` channel (`support_interface_region_facets`) round-trips in both 3mf loaders; the don't-care support/interface guards in `GCode.cpp`. See `AGENTS.md` and the per-feature notes in [`README.md`](README.md) for the seams. **This is where post-merge compile errors will concentrate** (upstream refactors signatures here). |
| **Feature — GUI (keep ours + integrate)** | `src/slic3r/GUI/Gizmos/GLGizmoSupportInterfaceRegions.{cpp,hpp}` (new), `Gizmos/GLGizmosManager.{cpp,hpp}`, `Gizmos/GLGizmoPainterBase.hpp`, `GUI_Factories.{cpp,hpp}`, `GUI_ObjectList.{cpp,hpp}`, `ObjectDataViewModel.cpp`, `3DScene.cpp`, `resources/images/menu_support_interface_modifier.svg` (new), `src/slic3r/CMakeLists.txt` | Keep Forca's additions (gizmo registration, CMake entry for the new gizmo). `SupportInterfaceRegions` sits in `EType` and in the `m_gizmos` registration order just *before* upstream's `#ifdef SLIC3R_CAD` gizmos (Primitive, Sketch): those are registered only when CAD is enabled, so they must stay last or `m_gizmos[EType]` indexing breaks and fold in upstream changes around them. `3DScene.cpp` also carries a **Debug-only** glsafe-assert downgrade — keep it. |
| **Feature — Calibration Wizard (keep ours + integrate)** | New, Forca-only: `src/slic3r/GUI/ForcaCalibrationWizard.{cpp,hpp}`, `ForcaPrinterCalibration.{cpp,hpp}` (printer track), `ForcaCalibrationTab.{cpp,hpp}`, `ForcaCalibrationStore.{cpp,hpp}`, `resources/images/forca_calibration_banner.png`, its translations in `tools/forca/forca_translations_wizard.json` (previous row). Hooks: `src/slic3r/GUI/Plater.{cpp,hpp}`, `MainFrame.{cpp,hpp}`, `src/slic3r/CMakeLists.txt` | The new files never conflict — keep them. In the hooked files keep Forca's additions (each is commented `Forca`): in **Plater** `calib_max_vol_speed`: `bottom_shell_layers` **1** (Orca: 0) and, after the height cut, `forca_hollow_to_outline()` (`src/libslic3r/ForcaCalibModel.{cpp,hpp}`, tests `tests/fff_print/test_forca_calib_model.cpp`; CMake entries in `src/libslic3r/CMakeLists.txt` and `tests/fff_print/CMakeLists.txt`) hollows the first layer to a one-line ring + `wall_generator` Arachne on the object, so the brim goes on both sides of the wall (Bambu Studio's thin-walled test; Orca's solid model gets an outer brim only) -- keep both if upstream touches that function or swaps the model; the default-`false` `skip_confirm` parameter on `calib_temp` / `calib_max_vol_speed` / `calib_flowrate` / `calib_pa` / `calib_retraction` and (printer track) `calib_input_shaping_freq` / `calib_input_shaping_damp` / `Calib_Cornering` / `calib_VFA`, `print_current_plate()`, `set_one_shot_slice_completed_callback()`, `canvas_host_panel()`, `set_forca_calibration_mode()` (+ `sidebar_layout.forca_calibration` honored in `update_sidebar`) and the Device-jump skip in `print_job_finished`; in **MainFrame** `TAB_ID_FORCA_CALIB` + its `InsertPage` after Calibration, the page-changed block (only for `e.GetEventObject() == m_tabpanel`), the `select_tab` / Slice-button / Ctrl+R stay-on-tab redirects, the Calibration-menu entries, and `restore_to_creation` removing *every* page backed by `m_plater`. If upstream changed a `calib_*` launcher's signature, `calib.hpp` or the defaults in `calib_dlg.cpp` (the printer track mirrors Orca's Input Shaping / Cornering / VFA dialog defaults and limits), re-check the wizard's calls (and the PA-Pattern rule: `CalibPressureAdvancePattern` keeps a *reference* to its params). `MainFrame.cpp` is also a hand-edited retint file (next row). |
| **Feature — Forca Academy print journal (keep ours + integrate)** | New, Forca-only: `src/slic3r/GUI/ForcaAcademy.{cpp,hpp}` (records, folder layout, outcomes, photos, calibration history), `ForcaPrintJournal.{cpp,hpp}` (the window), `tests/slic3rutils/test_forca_academy.cpp`, translations in `tools/forca/forca_translations_academy.json`. Hooks: `Plater.cpp`, `PrintHostDialogs.cpp`, `SelectMachine.hpp`, `Preferences.cpp`, `MainFrame.cpp`, `ForcaCalibrationWizard.cpp`, `ForcaPrinterCalibration.cpp`, `ForcaAIPrint.cpp`, `ForcaAIPrinters.cpp`, `src/slic3r/CMakeLists.txt` | The new files never conflict — keep them. In **Plater** `print_job_finished` keep the `forca_academy_on_print_sent` call *before* the Calibration-Wizard early return (it records a successful Bambu send; not `FROM_SDCARD_VIEW`); in **SelectMachine.hpp** the `get_print_plate_idx()` accessor; in **PrintHostDialogs** `append_job` (hold a snapshot for a `StartPrint` / `QueuePrint` upload -- this runs on the background slicing thread, so `forca_academy_hold_upload` only notes the job and snapshots via `CallAfter`), and `forca_academy_upload_done` in `on_progress` (100%), `on_error` and `on_cancel`; in **Preferences** the "Forca Academy" section (`forca_academy_enabled`, `forca_academy_dir`); in **MainFrame** the File-menu "Print Journal" item in the shared file menu (after Export -- not the macOS-only block). In **GUI_App** `forca_academy_start()` right after `ForcaAI::instance().start_if_enabled()` (settings defaults; resumes following sent Bambu prints from `<data_dir>/forca/academy_tracking.json`). The print-end follower polls `MachineObject` (`is_in_printing`, `print_status`, `subtask_name`, `curr_layer`, `print_error`, `GetHMS`) and the printer pages read `get_ota_version`, `GetFilaSystem()->GetAmsList()` -- re-check them if upstream reworks the Device code. Its camera pictures go through `forca_printer_camera_jpeg` (`ForcaAIPrinters.cpp`, not gated by AI sharing). The wizard, printer track and AI approval call `forca_academy_mark_source` just before `print_current_plate`, and the two wizard tracks call `forca_academy_log_calibration` after a result is applied. If upstream reworks the send paths (`PrintJob` success event, `PrintHostJobQueue`), re-hook them there: a record must only be written for a send that succeeded. |
| **Feature — Forca AI (keep ours + integrate)** | New, Forca-only: `src/slic3r/GUI/ForcaAI.{cpp,hpp}`, `ForcaAITools.cpp`, `ForcaAIHands.cpp`, `ForcaAIPrint.cpp`, `ForcaAIPrinters.cpp`, `ForcaAIAutonomy.cpp` (print grants), `ForcaAIAcademy.cpp` (Forca Academy tools, next row), `ForcaAIFiles.{cpp,hpp}`, `ForcaAIDialog.{cpp,hpp}`, `src/libslic3r/ForcaAISafePath.{cpp,hpp}`, `tests/libslic3r/test_forca_ai_safe_path.cpp`, `resources/images/forca_ai{,_inactive}.svg`. Hooks: `BBLTopbar.{cpp,hpp}`, `MainFrame.{cpp,hpp}`, `GUI_App.cpp`, `Plater.{cpp,hpp}`, `wxMediaCtrl3.{cpp,h}`, `StatusPanel.hpp`, `SelectMachine.{cpp,hpp}`, `src/slic3r/CMakeLists.txt`, `src/libslic3r/CMakeLists.txt`, `tests/libslic3r/CMakeLists.txt` | The new files never conflict — keep them. In the hooked files keep Forca's additions: in **BBLTopbar** the `ID_FORCA_AI` button, `OnForcaAIToolItem` / `UpdateForcaAIItem` and the listener; in **MainFrame** `show_forca_ai()` + `m_forca_ai_dialog`; in **GUI_App** `ForcaAI::instance().start_if_enabled()` at startup and `shutdown()` on exit; in **Plater** `set_forca_ai_slice_listener()` + `m_forca_ai_slice_listener`, fired in `on_process_completed` next to the calibration one-shot hook; in **wxMediaCtrl3** `ForcaCurrentFrame()`; in **SelectMachine** (`SelectMachineDialog`, print grants) `forca_clean_ready()` / `forca_auto_send()`, the `m_forca_auto_send` / `m_forca_auto_refused` members and the two `m_forca_auto_send` early returns in `on_ok_btn` (before the `ConfirmBeforeSendDialog` and before `start_timelapse_storage_check`) -- if upstream adds another confirmation to `on_ok_btn`, give it the same early return so an AI send never answers it; if it renames `m_print_status`, `m_pre_print_checker` or `m_printer_last_select`, adapt `forca_clean_ready`; in **StatusPanel** (`StatusBasePanel`) the public `forca_media_ctrl()` accessor; the three CMake entries. The printer tools read `DeviceManager` / `MachineObject` / `DevHMS`, pause/resume through `MachineObject::command_task_pause` / `command_task_resume`, and open the camera through the Bambu plugin (`bambulib_get`, `AVVideoDecoder`, the `bambu:///` URL scheme from `MediaPlayCtrl`) — re-check them if upstream reworks the Device code. The tools call Plater / PresetCollection / Tab / ObjectList / PartPlate APIs (`load_files`, `reslice`, `export_3mf`, `print_current_plate`, `save_current_preset`, `select_preset`, `take_snapshot`, …): **compile errors after a merge land here when upstream renames them** — adapt the calls, never the two safety rules (R1 file paths through `ForcaAISafePath`; printing only from the user's click on the approval card). |
| **Small Forca changes and early upstream PRs** | `src/slic3r/GUI/GUI_App.cpp` (`import_model`, `format_wildcards`), `src/slic3r/Utils/MoonrakerPrinterAgent.cpp`, `src/libslic3r/Preset.{cpp,hpp}`, `src/libslic3r/PresetBundle.cpp`, `docs/HLSD/filament_id.md` | **Import file-type dropdown** (ForcaSlicer issue #4): `GUI_App::import_model` offers "Supported files" (default), "Model files without 3MF", 3MF, STL, STEP and OBJ, and remembers the choice in app config `forca_import_filter`; `file_wildcards` formats through the static `format_wildcards` helper. Keep both if upstream reworks the dialog. **Upstream PRs taken before upstream merged them** (ForcaSlicer issue #3): OrcaSlicer [#14798](https://github.com/OrcaSlicer/OrcaSlicer/pull/14798) (Happy Hare gate data when `lane_data` is an empty skeleton) and [#14423](https://github.com/OrcaSlicer/OrcaSlicer/pull/14423) (match Moonraker `lane_data` by `filament_id` / `setting_id` / preset name). If upstream has merged a PR by the time of a merge, take upstream's version; if it is still open or was closed, keep ours and check it still applies. |
| **Feature — non-Bambu printer status (keep ours + integrate)** | New, Forca-only: `src/slic3r/GUI/ForcaHostStatus.{cpp,hpp}` (Moonraker / Flashforge status readers, Flashforge `/control`, Fluidd / Mainsail search), `ForcaFlashforgePanel.{cpp,hpp}` (Device-tab handler), `resources/web/forca/flashforge/index.html`, `tests/slic3rutils/test_forca_host_status.cpp`, translations in `tools/forca/forca_translations_devices.json`. Hooks: `src/slic3r/Utils/PrintHost.cpp` (`get_print_host_webui`), `PrinterWebViewHandler.cpp`, `PhysicalPrinterDialog.{cpp,hpp}`, `PrintHostDialogs.cpp`, `TaskManager.cpp`, both CMake lists | The new files never conflict — keep them. In **PrintHostJobQueue::priv::progress_fn** keep the byte progress capped at 99 (100 = the printer accepted the job; the Academy records a send on 100). In **PrintHost::get_print_host_webui** keep the `htFlashforge` case (Forca's page when Device UI is empty) and the Moonraker `:7125` strip; in **create_printer_webview_handler** the `htFlashforge` case (only when Device UI is empty); in **PhysicalPrinterDialog** the Search button on the Device UI line (`m_forca_find_webui_btn`); in **PrintHostQueueDialog::append_job** the upload path passed to `forca_academy_hold_upload`; in **TaskManager** `forca_academy_hold_task` in `start_print` and `forca_academy_task_done` after the send result in `schedule`. If upstream gives Flashforge printers a web UI or a printer agent of its own, revisit the page. |
| **Retint — hand-edited** | `GLCanvas3D.cpp`, `Tab.cpp`, `GUI_App.cpp`, `BBLTopbar.cpp`, `MainFrame.cpp`, `Notebook.cpp` | Take upstream's structure, then re-apply the small hand edits — see [§6a](#6a-hand-edited-retint-files). The retint script does **not** touch these. |
| **Retint — scripted** | every other `src/slic3r/GUI/**/*.{cpp,hpp}` + `resources/web/{homepage/css/dark.css,homepage/css/home.css,include/global.css}` | Don't hand-merge colors. Take **upstream's** version of the conflicted file, then regenerate the tint in [§6b](#6b-regenerate-the-retint): `git checkout --theirs <file> && git add <file>`. |
| **GitHub automation — keep Orca's deleted** | `.github/**` (Orca's workflows, bots, funding, templates) | Forca removed Orca's `.github` folder (it ran Orca's bots/releases with Orca's secrets). Resolve modify/delete conflicts on Orca's files by keeping them deleted (`git rm -f <path>`; upstream's *new* files arrive staged, hence `-f`). **Exception — keep Forca's:** `.github/ISSUE_TEMPLATE/` holds Forca's issue forms (`bug_report.yml` must keep the `version` and `os` field ids the app pre-fills); on a conflict take ours. `check_identity.ps1` fails if Orca's workflows come back. |
| **Translations — take theirs, re-apply ours** | `localization/i18n/*/OrcaSlicer_*.po` | Take upstream's catalogs on conflict (`git checkout --theirs`), then run `powershell -ExecutionPolicy Bypass -File tools/forca/apply_forca_translations.ps1`: it re-adds Forca's own strings (`tools/forca/forca_translations*.json`, marked `# AI Translated`: Orca wording Forca changed, and the Calibration Wizard's strings, whose sources are not in `localization/i18n/list.txt`) and validates the changed catalogs with `msgfmt`. `check_identity.ps1` fails if any are missing. When Forca rewords or adds a translatable string, add it to the matching JSON with its translations. |
| **Build/docs — keep ours** | `build_forca_release.{bat,ps1}`, `tools/forca/**`, `docs/forca/**`, `README.md`, `SECURITY.md`, `CHANGELOG.md` | Keep Forca's. `SECURITY.md` is Forca's policy (Orca's sends reports to Orca's maintainer); `check_identity.ps1` fails if Orca's comes back. |

> Not every file in a category conflicts on every merge — only the ones upstream also changed. Use `git status` to see the actual conflict set and match each against the table.

## 6. Regenerate the cosmetic layers

### 6a. Hand-edited retint files

Only if the merge touched them. To see exactly what each edit is (authoritative, never stale):

```bash
git diff <previous upstream base>..Multi_Interface -- src/slic3r/GUI/GLCanvas3D.cpp   # e.g. 8c03985818
```

What each file carries:

- **`GLCanvas3D.cpp`** — 3D viewport background: `DEFAULT_BG_LIGHT_COLOR` and `DEFAULT_BG_LIGHT_COLOR_DARK` set to blue decimal-RGB constants.
- **`Tab.cpp`** — settings panel background made theme-aware (`dark_mode() ? navy : light-blue`), in ~2 spots.
- **`GUI_App.cpp`** — splash loads `resources/images/forca_splash.png` at 600×400 (3:2) with a fallback to the stock splash, plus white centered version/status text overlay.
- **`BBLTopbar.cpp`** — titlebar/topbar brush `wxColour(26, 44, 76)` (= `#1A2C4C`), ~2 spots.
- **`MainFrame.cpp`** — topbar panel `SetBackgroundColour(wxColour(26, 44, 76))`.
- **`Notebook.cpp`** — nav-tab Normal-state color `wxColour(45, 68, 104)` (= `#2D4468`), ~2 spots.

### 6b. Regenerate the retint

After the tinted-file conflicts are resolved in upstream's favor, re-apply the blue in one step:

```bash
# Windows
powershell -ExecutionPolicy Bypass -File tools/forca/apply_retint.ps1

# macOS / Linux
tools/forca/apply_retint.sh
```

Then sanity-check:

```bash
git diff --stat                                # colors reappear across the tinted files
git diff -- src/slic3r/GUI/BitmapCache.cpp     # MUST be empty (recolor keys intact)
```

See [`retint.md`](retint.md) for how the script and its data files work.

## 7. Rebuild (full)

An upstream merge changes headers and possibly `version.inc`, so this is a **full** rebuild with reconfigure (drop `--no-configure`). **Close Forca/OrcaSlicer first** or the link fails with `LNK1104`.

```bash
./build_win.bat -s --config relwithdebinfo --tests
```

(`--tests` keeps the unit tests built; a reconfigure without it turns them off.)

**If upstream changed `deps/`** (`git diff --stat <old base> <TARGET> -- deps`), rebuild the dependencies first, in the same run: `./build_win.bat -d -s --config relwithdebinfo --tests`. Unchanged dependencies are skipped. A configure error like "could not find a package configuration file provided by X" means a new dependency (2026-09-26: `SLVS` + OCCT's ModelingAlgorithms module for the CAD tab). If a dependency whose patch recipe changed fails with "patch does not apply" / "already exists in working directory", its old source folder is already patched: delete `deps\build\dep_<Name>-prefix` and rerun (the source archive is cached in `deps\DL_CACHE`).

Fix compile errors — they cluster in the feature engine files where upstream refactored APIs. Confirm the build actually **linked** before trusting anything (a failed compile leaves the old binary in place).

## 8. Validate

First the automated checks (no app needed):

```bash
powershell -ExecutionPolicy Bypass -File tools/forca/check_identity.ps1   # must pass; look at any REVIEW lines
build-dbginfo\tests\slic3rutils\RelWithDebInfo\slic3rutils_tests.exe "[ForcaBrand]"
build-dbginfo\tests\libslic3r\RelWithDebInfo\libslic3r_tests.exe "[ForcaAISafePath],[ForcaPresetMerge]"
```

Then run the freshly built dev exe against the isolated datadir:

```
build-dbginfo\src\RelWithDebInfo\forca-slicer.exe --datadir "%USERPROFILE%\ForcaSlicer-DevData"   # an isolated dev settings folder, never your real one
```

Checklist:

- [ ] App launches; window title and **About** say **Forca Slicer**; icon and splash are the Forca art.
- [ ] Interface is blue (viewport background, panels, topbar, tabs) — spot-check light and dark mode.
- [ ] Dark-mode toolbar/gizmo icons recolor correctly (confirms `BitmapCache.cpp` keys survived).
- [ ] Datadir is still your isolated dev folder; an existing `.3mf` project opens.
- [ ] **Regional feature (box):** a support-interface modifier gives its region a dissimilar interface filament + flush 0 gap, on classic (grid/snug), legacy tree, and organic.
- [ ] **Regional feature (paint):** the support-interface paint gizmo appears, paints, and round-trips through 3MF (save → reopen → paint persists).
- [ ] Paint comfort tickboxes (Vertical / Horizontal / On-overhangs-only) are present.
- [ ] **Inert when unused:** a plain support model with no modifier/paint slices like stock Orca (eyeball colors, tool changes, no crash).
- [ ] **Calibration Wizard tab:** it opens (Start page, progress panel, banner, plate); the Prepare sidebar returns when you go back to Prepare.
- [ ] Each calibration generates its test on the plate (spot-check Temperature, Flow, PA Pattern, Retraction, Shrinkage); Slice & send slices, shows Preview and stays on the tab; applying a result updates the progress bar and the before/after table.
- [ ] Leaving the tab mid-run and coming back resumes in place; a project with **two filaments** (one calibrated) selects without errors.
- [ ] **Forca Academy:** Preferences > Forca Academy: tick "Keep a print journal", Open creates the folder (AGENTS.md, INDEX.md, lessons.md). Send a print (Bambu, and a network printer if one is available): a folder appears under `prints/<year>/` with record.json, notes.md, thumbnail.png. File > Print Journal lists it; set a result + tag + note and add a photo; reopen and it stuck. With Forca AI on: `forca_academy_status`, `forca_academy_list_prints`, `forca_academy_read` (a thumbnail) work, and `forca_academy_write_note` refuses to replace a note you edited. Run `slic3rutils_tests "[ForcaAcademy]"`.
- [ ] **Forca AI:** turn it on in the Forca AI window (top bar); connect Claude Code with the shown command; `forca_status` answers, an import + slice + `forca_slice_result` work, `forca_save_project` writes a new "(Claude <date>)" file, and `forca_request_print` shows the approval card (Decline it). With a printer ticked: `forca_printer_status` is live and `forca_camera_snapshot` returns a picture (live view open and closed); with "AI may pause" ticked on a throwaway print, `forca_pause_print` pauses + shows the card, `forca_resume_print` resumes it, and is refused on a pause you made; Forca exits cleanly afterwards. Also run `libslic3r_tests "[ForcaAISafePath]"` if tests are built.
- [ ] Read the dev slice log for errors: `<dev datadir>\log` (newest by mtime).

> Support generation is non-deterministic run-to-run, so a before/after G-code byte-diff is meaningless. "Inert when unused" is a code-structure guarantee, verified by slicing + eyeballing.

## 9. Land and push (needs approval)

Only after validation passes. **Each push requires Aaron's explicit approval.** First add a line to [`CHANGELOG.md`](../../CHANGELOG.md) ("Merged OrcaSlicer up to `<TARGET>`") and commit it on the merge branch.

```bash
git switch Multi_Interface
git merge --no-ff merge/upstream-YYYYMMDD          # bring the validated merge onto the working branch
git push origin Multi_Interface                    # APPROVAL REQUIRED
git push origin Multi_Interface:safe/known-good     # advance the safe point — APPROVAL REQUIRED, only after validation
git tag -a known-good-YYYYMMDD -m "<why>" Multi_Interface && git push origin known-good-YYYYMMDD   # dated snapshot
git branch -d merge/upstream-YYYYMMDD              # clean up
```

Never move `safe/known-good` backward. If a merge is abandoned, delete the merge branch and stay on the previous `safe/known-good`.

## Recovery

```bash
git merge --abort                          # during a conflicted merge
git switch Multi_Interface                 # then delete the merge branch
git switch --detach origin/safe/known-good # inspect the last known-good state
```
