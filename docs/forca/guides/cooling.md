# Cooling slowdown

When a layer prints faster than the filament's **Layer time** (filament settings, Cooling page, **Slow printing down
for better layer cooling**), Forca slows it down so each layer has time to cool, as OrcaSlicer does. Forca adds two
choices about *how* it slows down. Both are filament settings on the Cooling page and are off by default, so prints
don't change until you set them.

## Keep the outer wall at speed: Consistent surface

**Cooling slowdown logic** decides what gets slower. Forca shows it in **Advanced** mode (the mode switch at the top
of the settings); Bambu Studio keeps it in Develop mode:

- **Uniform cooling** (default, as in OrcaSlicer): everything on the layer slows down together, the outer wall too.
- **Consistent surface** (from Bambu Studio): the inner walls, infill and other features slow down first, and the
  outer wall only if the layer still isn't long enough. The visible surface keeps its speed, so it keeps its look.

With Consistent surface, **Transition distance** lets the last few millimetres of each slowed extrusion speed back up,
so the nozzle is back at speed when it reaches the outer wall.

## Speed back up gradually: Slowdown recovery layers

A model whose layers suddenly get much longer, such as a wide part above a thin neck or post, goes from a slowed-down
layer to full speed in one layer. The jump can leave a visible band on the wall where the gloss or width changes.

**Slowdown recovery layers** spreads that return over several layers. With 5, the speed comes back in five equal
steps: a neck that printed at 20 % speed is followed by layers at about 36 %, 52 %, 68 %, 84 % and then 100 %.
Slowing down still happens at once, since a short layer needs its cooling time. 0 (the default) turns it off.

The fan follows the real layer time as always, so a layer that prints slower during the recovery also gets the fan
speed that layer time calls for.

Use a small number (3–5) on models with a thin section under a wide one; it costs only the extra time on those few
layers.

## How the layer time is counted

The layer time that the slowdown and the fan go by is the one the Preview shows for that layer: Forca times each
layer as the G-code is written, with the printer's acceleration, cornering and motion limits, not just the length
of each move over its speed. On a fast printer with moderate acceleration that is often 1.3 to 2 times the plain
estimate, so short layers are slowed less than a plain estimate would ask for, and still cool as long as you set.

Also counted: a `G4` dwell in your own G-code, and on a **toolchanger** the **Tool change time** (printer settings)
for each tool change on the layer, since the layer below keeps cooling while the tools are swapped. A layer that
starts with a filament change is timed from its first extrusion. Filament load and unload times are not counted.
