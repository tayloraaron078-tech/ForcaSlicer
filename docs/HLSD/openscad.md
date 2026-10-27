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

## Limits

- A file whose top level renders as several objects (lazy-union) is imported unlinked, and a re-render that gives
  several objects is refused: an object is only ever replaced by one.
- A `.scad` file is code: it can read files on the disk (`import()`, `include`), but not run programs. Opening it in
  Forca trusts it as much as opening it in OpenSCAD does.
- Libraries a file `include`s (BOSL2 and the like) must be where OpenSCAD looks for them (its library folder or
  `OPENSCADPATH`).
