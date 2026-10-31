# Forca Slicer guides

Forca Slicer is [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) with additions. Everything that isn't listed
here works as in OrcaSlicer, and [OrcaSlicer's wiki](https://www.orcaslicer.com/wiki) applies. These guides cover what
Forca adds, one page per feature.

**New to Forca?** Start with [Getting started](getting-started.md): installing next to your current slicer, bringing
your OrcaSlicer settings over, and starting again if you need to.

## Printing

- [Regional support interfaces](regional-support-interfaces.md): a different support interface (filament, gap) for
  part of a model, by box or by painting.
- [Per-object filament overrides](per-object-filament-overrides.md): retraction, pressure advance, max volumetric
  speed, and (printing by object) temperature and fans, per object.
- [OpenSCAD models](../openscad.md): open `.scad` files, change their parameters in Forca, fix them in a small
  editor.
- [Cooling slowdown](cooling.md): keep the outer wall at speed while a short layer cools (Consistent surface), and
  bring the speed back gradually after a thin section (Slowdown recovery layers).

## Tuning

- [Calibration Wizard](calibration-wizard.md): your printer and each filament, step by step, from untuned to
  calibrated.
- [Hole compensation by size](hole-compensation.md): holes that fit at every size.
- [Forca Academy](forca-academy.md): a print journal of what you printed, how it went and what you learned.
- [Guided experiments](guided-experiments.md): test one setting on one plate and keep the winner.

## Profiles and printers

- [Importing filament profiles](preset-import.md): from OrcaSlicer, Bambu Studio and Forca, with a review first;
  repairing presets whose base is gone.
- [Open Bamboo Networking](open-bamboo-networking.md): an open-source replacement for Bambu Lab's network plug-in.
- [Flashforge and Klipper printers](flashforge-and-klipper.md): a status page for stock Flashforge printers, and
  Fluidd / Mainsail found for you.

## AI

- [Forca AI](forca-ai.md): let *your own* AI assistant work in Forca with you, under rules it can't switch off.
  Optional and off by default; Forca has no AI built in.

The settings Forca ported from Bambu Studio are listed in the [README](../../../README.md#bambu-studio-settings); each
has a tooltip in Forca. Problems and ideas: [GitHub Issues](https://github.com/tayloraaron078-tech/ForcaSlicer/issues/new/choose).
