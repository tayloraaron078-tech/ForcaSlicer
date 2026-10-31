# OpenSCAD in Forca

## Why it exists

Many parametric models are OpenSCAD code: a `.scad` file and a Customizer of named, ranged parameters. Opening one
in a slicer normally means rendering it in OpenSCAD, exporting a mesh and importing that, again after every change.
Forca opens the `.scad` file itself and asks OpenSCAD to do the rendering, so the model arrives on the plate in one
step. OrcaSlicer's Design (CAD) tab is a different thing (an OCCT feature-history CAD, `docs/HLSD/design-tab.md`) and
is not involved.

## What the user sees

- **File > Import** lists `.scad` files with the other model files (and under their own "OpenSCAD files" filter), and
  a `.scad` file dropped on the window is imported the same way. Forca renders it in the background with the file's
  default parameters (the progress notification has Cancel) and adds the result to the plate as one undo step, named
  after the file. When OpenSCAD can't render it, its warnings and errors are shown.
- **Right-click the object > OpenSCAD parameters...** opens the file's Customizer inside Forca: the parameters by
  group, with the file's captions, as text fields (numbers, strings, lists), checkboxes and dropdowns, filled with the
  values chosen last time. Render model renders in the background and replaces the object's parts in place, as one undo
  step; Reset sets every parameter back to the file's default.
- **Edit source...** in that dialog opens the `.scad` file in a small editor for a quick fix: line numbers, OpenSCAD
  colouring, and OpenSCAD's console under it. The lines a warning or error names are marked and the first error is
  selected. OpenSCAD names the line where parsing stopped, which for a missing `;` or bracket is the line after the
  mistake, so for a syntax error the last code line before it is marked in yellow too, with a note in the console. Save and check saves the file and has OpenSCAD render it with the chosen values, showing the new console
  output. Closing the editor reads the parameters again, so a parameter added in the editor appears in the dialog.
  When OpenSCAD can't read the parameters, or a re-render fails, Forca offers the editor with OpenSCAD's output.
- **Right-click the object > Edit in OpenSCAD** opens the linked file in OpenSCAD's own window. Forca watches the file:
  when it is saved there, Forca asks whether to render the model again, and each object linked to the file is
  re-rendered with its own values (one undo step each).
- **File > Import > New OpenSCAD model...** saves a starter `.scad` file where the user chooses (a rounded box whose
  sizes, hollowness and wall are Customizer parameters, with comments on how parameters work), puts it on the plate
  and opens it in OpenSCAD, watched as above.
- **Colours**: a model's `color()`s arrive as filaments. Each OpenSCAD colour becomes the project filament closest to
  it, the faces in that colour are painted with it (multi-material painting), and the most common colour becomes the
  object's filament; faces without a `color()` keep the object's filament.
- **Forca AI** can use OpenSCAD: `forca_openscad_params` reads a file's parameters, and `forca_openscad_render`
  renders a `.scad` file, or code the AI writes, onto the plate (or in place of an OpenSCAD object) and returns
  OpenSCAD's messages with the lines they name, so the AI can fix its code and try again.
- **Preferences > OpenSCAD** shows which OpenSCAD Forca uses and its version, with Browse, Find automatically and
  Download (OpenSCAD's development snapshots page).
- Without a usable OpenSCAD, importing a `.scad` file says what to install or choose.

## How it works

- **Forca runs the user's own `openscad.exe` as a separate program**: a `.scad` file in, a 3MF out. No OpenSCAD code
  is built into Forca, so OpenSCAD's GPL and Forca's AGPL don't interact, and the user can always use the newest
  OpenSCAD.
- **Which OpenSCAD**: the one chosen in Preferences (app config `forca_openscad_path`), or the first usable one of the
  usual install places (`forca_openscad_candidate_paths`: the development snapshot first, then a release). "Usable"
  is read from `--help`: the Manifold backend (renders in about a second where the old CGAL backend can take minutes)
  and the `param` export (the file's Customizer as JSON). The 2021.01 release has neither, so a 2025-or-later
  development snapshot is needed. The result is remembered until the preference changes.
- **Rendering** (`ForcaOpenScadImport.cpp`): a job on the Plater's UI worker runs
  `openscad -o <stem>.3mf --backend Manifold -O export-3mf/color-mode=model <file>` without a console window, with
  its output in files in a temporary folder, polling for Cancel and stopping it after ten minutes. Program and
  arguments are passed as wide strings on Windows, so any path works. The 3MF is named after the `.scad` file and
  loaded like any imported model; OpenSCAD names every object "OpenSCAD Model", so the object takes the file's name.
- **Where `.scad` files are accepted**: `Plater::load_files` takes them out of the list and hands them to the
  OpenSCAD import (that covers Import, drag-and-drop and every other path into it); the Import dialog adds `.scad` to
  its filters. `FT_MODEL` itself does not list `.scad`, because "Replace with" and reload-from-disk read meshes
  directly.
- **The link** (`ModelObject::forca_openscad`, `OpenScadLink`): an imported object that came from one `.scad` file as
  one object remembers the file's path, its text at the last render and the values that differ from the file's
  defaults (so the rest keep following the file). It is copied with the object and kept in undo/redo, and the project
  3MF stores every linked object's link in `Metadata/forca_openscad.json` by the object's 1-based index (the index the
  layer height profiles use); a project without linked objects has no such entry, and readers that don't know it
  ignore it. Test: "An object's OpenSCAD link survives a 3MF round-trip on the same object" (`test_3mf.cpp`).
- **Changing the parameters** reads them with OpenSCAD from the file as it is now, or, when the file is gone, from the
  text of the last render saved to a temporary file. The new render's parts replace the object's model parts: the
  object keeps its instances (its place on the plate), its modifiers and other non-part volumes, and each part's
  settings and name carry over to the new part in the same position. The object is found again by its ID when the
  render finishes, so it may have moved in the list meanwhile.
- **The parameters** (`libslic3r/ForcaOpenScad`): `forca_openscad_parse_params` reads OpenSCAD's `param` export
  (name, group, caption, type, initial value, min / max / step, dropdown options) into `OpenScadParam`, with each
  value already in OpenSCAD syntax for `-D`. Tests: `tests/libslic3r/test_forca_openscad.cpp` against
  `tests/data/forca_openscad/box.param.json`, the real export of `box.scad` by OpenSCAD 2026.10.05.

- **The editor** (`ForcaOpenScadEditor`) is wxWidgets' `wxStyledTextCtrl` (Scintilla), so the wxWidgets dependency is
  built with `wxUSE_STC=ON` and its `src/stc/scintilla` and `src/stc/lexilla` submodules (upstream OrcaSlicer has it
  off), and the app links wx's `stc` component. OpenSCAD's syntax is close enough to C for Scintilla's C++ lexer, given
  OpenSCAD's keywords and built-ins. `forca_openscad_problem_lines` finds the lines of the edited file that OpenSCAD's
  warnings and errors name ("in file box.scad, line 9"), leaving out lines of included files. The editor keeps the
  file's own line endings. It is meant for a few lines, not as a replacement for OpenSCAD or a code editor.

- **The file watch** (`OpenScadWatch` in `ForcaOpenScadImport.cpp`): a one-second `wxTimer` on the UI thread, living
  as long as the Plater, checks the modification time of every file opened with Edit in OpenSCAD or New OpenSCAD
  model. A change counts once the time has stayed the same for one more check, so a save isn't read half-written. A
  file no linked object uses any more is dropped. While Forca's own parameters dialog or editor is open the watch
  doesn't ask (they save the file themselves), and what they saved counts as seen afterwards.
- **Colours** (`forca_openscad_read_colors`, `apply_colors`): OpenSCAD's 3MF (`export-3mf/color-mode=model`) has one
  `basematerials` entry per colour (the first, "Default", for no `color()`) and a material per triangle (`p1`).
  OrcaSlicer's 3MF reader ignores per-triangle materials, so Forca reads them from the 3MF itself (libslic3r, tested
  against a real OpenSCAD 3MF) and paints the loaded part's `mmu_segmentation_facets` by triangle index. The reader
  keeps OpenSCAD's triangle order; when the part's triangle count differs from the file's, the colours are left out.
  The closest filament is by CIE76 colour distance (`calc_color_distance`), among the project's filament colours.
- **Forca AI** (`ForcaAIOpenScad.cpp`): both tools run OpenSCAD off the GUI thread and change the plate on it. Code
  the AI writes goes through rule R1: a new file in `Documents\Forca AI\OpenSCAD` named "<name> (Claude <date>).scad",
  or an existing file only when the AI wrote it and nobody changed it since (any file at the Advanced level, backed up
  first). The user's own `.scad` files are otherwise only read. A render waits at most five minutes. `forca_scene`
  shows each OpenSCAD object's file and changed parameters.

## Limits

- A file whose top level renders as several objects (lazy-union) is imported unlinked, and a re-render that gives
  several objects is refused: an object is only ever replaced by one.
- A `.scad` file is code: it can read files on the disk (`import()`, `include`), but not run programs. Opening it in
  Forca trusts it as much as opening it in OpenSCAD does.
- Libraries a file `include`s (BOSL2 and the like) must be where OpenSCAD looks for them (its library folder or
  `OPENSCADPATH`).
- Colours only map to filaments the project already has: two colours closest to the same filament print in that one.
  Re-rendering replaces painting done by hand with the file's colours.
- The watch only covers files opened from Forca in this session; editing a linked file another way is picked up the
  next time the parameters are opened.
