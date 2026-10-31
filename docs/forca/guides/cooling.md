# Cooling slowdown

When a layer prints faster than the filament's **Layer time** (filament settings, Cooling page, **Slow printing down
for better layer cooling**), Forca slows it down so each layer has time to cool, as OrcaSlicer does. Forca adds two
choices about *how* it slows down. Both are filament settings on the Cooling page and are off by default, so prints
don't change until you set them.

## Keep the outer wall at speed: Consistent surface

**Cooling slowdown logic** decides what gets slower. Like Bambu Studio, Forca shows it in **Develop** mode only (the
mode switch at the top of the settings):

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
