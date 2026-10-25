# Per-object filament overrides: High Level Design

## Why it exists

OrcaSlicer lets most process settings differ per object (speeds, walls, infill, supports, bridges, a per-object flow
ratio), so two copies on one plate can be printed with, say, different bridge speeds and compared side by side.
Filament settings can't: retraction and pressure advance belong to the filament, so the same plate couldn't compare
two retraction lengths. Forca adds a small set of "Filament overrides" to an object for exactly that kind of
side-by-side test, and for parts that simply need different retraction or pressure advance than the rest of the plate.

## What the user sees

An object's process settings (right-click the object > Edit Process Settings) have a "Filament overrides" group on
the Others page, with:

- Retraction length (this object)
- Retraction speed (this object)
- Deretraction speed (this object)
- Z-hop height (this object)
- Pressure advance (this object)
- Max volumetric speed (this object)
- Nozzle temperature (this object), Nozzle temperature, first layer (this object)
- Min fan speed, Max fan speed and Overhang fan speed (this object)

The temperature and fan overrides only work when objects print one after another (Print sequence: By object): in
"By layer" mode all objects share every layer's nozzle temperature and fan, so slicing shows a warning on the object
instead of silently ignoring them.

Each defaults to -1, meaning "use the filament's value" (the filament's value is the filament override if set, else
the printer's). An override applies to everything the object prints: all its filaments and its supports. The brim
(and skirt) are printed before any object's settings take effect, so they keep the filament's values.

## How it works

- The settings are PrintObjectConfig options (`object_*`). Changing one invalidates only the G-code export step.
  TabPrint builds their group only for the model tabs (`m_type == Preset::TYPE_MODEL`); the object tab keeps them,
  the part, layer and plate tabs filter them out, and the global process tab never shows them (they would apply to
  every object there).
- While generating G-code, each time the printed object may change, `GCode` copies that object's config into its
  working config (existing Orca behaviour) and then calls `GCode::forca_apply_object_overrides()`:
  - Retraction: the object's values go into an `ObjectRetractionOverrides` struct that every `Extruder` points to
    while that object prints; `Extruder::retraction_length()`, `retract_speed()`, `deretract_speed()` and
    `retract_lift()` (and the writer's z-hop) read the override first. When the values change between objects, the
    generator retracts before switching, so the nozzle leaves an object with that object's retraction and the next
    object's values apply from its first deretraction. Tool-change retraction keeps the filament's values.
  - Pressure advance: entering an object with an override emits its value; inside it adaptive pressure advance is off
    and tool changes re-emit the object's value. Leaving it restores the filament's value (if the filament doesn't
    enable pressure advance, its pressure advance value is emitted anyway, since a value is needed after the
    override).
  - Max volumetric speed: the speed caps in `GCode::_extrude` ask `GCode::forca_max_volumetric_speed()`, which
    returns the printed object's override, else the filament's value. Tool-change, flush and wipe-tower speeds keep
    the filament's value. The value of the last printed object stays in effect until the next object starts, so a
    skirt printed at the start of a later layer follows it too.
- Print by object only: before each object starts, `GCode::forca_apply_sequential_object_overrides()` gives the
  cooling buffer that object's fan values (`CoolingBuffer::set_object_fan_overrides`, read in place of the filament's
  min/max/overhang fan) and, when the object has a temperature override (or the previous one had), sets its
  first-layer temperature and waits for it (M109). The existing "second layer" switch then uses the object's regular
  temperature. `Print::validate` warns when an object has these overrides and the print sequence is "By layer".
- With no object override on the plate, the override struct stays "none" and no extra G-code is emitted: the
  output is the same as without the feature (test "Filament overrides left at -1 change nothing in the G-code").

## Constraints

- Inert when unused; one Forca block per upstream file (PrintConfig, PrintObject invalidation, Preset, Extruder,
  GCodeWriter, GCode, CoolingBuffer, Print validation, Tab, GUI_Factories), noted in docs/forca/upstream-merge.md.
- Projects keep the overrides as ordinary object settings; an OrcaSlicer without this feature ignores the unknown keys.
- Fan and nozzle temperature can't differ per object in "By layer" printing (objects share each layer's time and the
  nozzle would heat and cool between objects on every layer), so they are tied to print by object.

Tests: tests/fff_print/test_forca_object_filament.cpp ([ForcaObjectFilament]).
