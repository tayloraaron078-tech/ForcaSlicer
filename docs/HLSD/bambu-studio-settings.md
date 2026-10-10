# Bambu Studio Settings in Forca: High Level Design

## Why it exists

Bambu Lab's printer, process and filament profiles come from Bambu Studio, and so do the 3MF projects people share
for Bambu printers. Bambu Studio has settings that OrcaSlicer doesn't define. OrcaSlicer drops an unknown key when it
loads a profile or a project, so a Bambu profile or project prints differently in OrcaSlicer than in Bambu Studio,
usually without saying so. One such loss, `reduce_infill_retraction_mode`, made Bambu prints hours longer.

Forca defines those settings so Bambu profiles and projects keep their values and print as Bambu intends. A newer
Bambu Studio project still lists any setting Forca doesn't know when it is opened.

## The rules every ported setting follows

- **Bambu Studio's name, type and values.** A ported setting uses the same key, option type and enum names as Bambu
  Studio, so profiles and 3MFs load without translation. Where OrcaSlicer already has the same setting under another
  name, the Bambu Studio name is mapped to OrcaSlicer's in `PrintConfigDef::handle_legacy` instead of adding a second
  setting (role-based wipe speed, outer-wall slowdown, wipe tower purge speed, support ironing).
- **The default keeps today's output.** Where Bambu Studio's default would change prints that don't set the key, Forca
  uses a default that keeps OrcaSlicer's behaviour, usually 0 meaning "as before" (first-layer infill width, short-travel
  acceleration, paint penetration layers) or "Disabled" (infill retraction). Non-Bambu G-code is unchanged; Bambu
  profiles that set a value get Bambu Studio's behaviour.
- **Replaced keys load into the new one.** When a ported setting replaces an OrcaSlicer key (the
  `reduce_infill_retraction` bool became `reduce_infill_retraction_mode`), `handle_legacy` maps old values, and
  `scripts/orca_profile_tool.py` refuses a profile that holds both (`CONFLICT_KEYS`).
- **Same visibility as Bambu Studio.** Settings Bambu Studio keeps in Develop mode are `comDevelop` here too.

## Where each setting acts

| Setting | Where it acts |
| --- | --- |
| `reduce_infill_retraction_mode`, `filament_metal_stickiness` | `GCode::needs_retraction`: Auto skips the retraction on infill-only travels only for filaments with no or low metal stickiness |
| `initial_layer_infill_line_width` | `PrintRegion::flow`, first layer's sparse, solid and top infill only |
| `override_process_overhang_speed` + the `filament_*` overhang and bridge speeds | `GCode::_extrude`: the filament's speeds replace the process overhang and bridge speeds |
| `travel_short_distance_acceleration` | `GCode::travel_to`, short travels to an outer or overhang wall |
| `enable_height_slowdown` + `slowdown_*` | `GCode::height_slowdown_limits`, used by `_extrude` and `travel_to` |
| `pre_start_fan_time` | `make_fan_mover` in `GCode.cpp`, OrcaSlicer's FanMover |
| `support_ironing_inset`, `_direction`, `_speed` | `Support/SupportCommon.cpp` (inset, angle relative to the interface lines) and `GCode::extrude_support` (speed) |
| `cooling_slowdown_logic`, `cooling_perimeter_transition_distance` | `GCode/CoolingBuffer.cpp`; Forca restores the original speed for the transition distance, which Bambu Studio only accounts for in its time estimate |
| `avoid_crossing_wall_includes_support` | `GCode/AvoidCrossingPerimeters.cpp`, turning OrcaSlicer's compile-time `INCLUDE_SUPPORTS_IN_BOUNDARY` into a run-time choice |
| `top_color_penetration_layers`, `bottom_color_penetration_layers` | `MultiMaterialSegmentation.cpp` through `top/bottom_paint_penetration_layers()`, in place of the shell layers |

Bambu Studio's `print_in_clockwise` (P2S, X2D) is not a setting in Forca: OrcaSlicer's `wall_direction` has only ccw
and cw, so a printer-level key could not tell a user's explicit choice from the default. The P2S and X2D process
profiles set `"wall_direction": "cw"` instead. `seam_placement_away_from_overhangs` is not ported: OrcaSlicer's seam
placer has no equivalent of the data Bambu Studio's version depends on.

## Constraints

- **Upstream merges.** These settings live in files every OrcaSlicer merge touches (`PrintConfig`, `GCode.cpp`,
  `CoolingBuffer.cpp`). `docs/forca/upstream-merge.md` lists, per setting, what to keep when upstream reworks the code
  around it, and what to do if upstream ports the same setting itself (take upstream's, keep Forca's default and
  legacy mapping unless they give the same output).
- **Profiles.** Bambu's profiles are synced from Bambu Studio. A sync can bring back a key Forca has renamed or replaced;
  `orca_profile_tool.py check` catches the conflicting pairs, and the merge notes say how to resolve them.
- **Translations.** The strings are kept in `tools/forca/forca_translations_bambu.json` and written into the catalogs
  by `tools/forca/apply_forca_translations.ps1`.
