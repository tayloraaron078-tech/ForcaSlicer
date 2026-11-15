# Nonplanar ironing and nonplanar top surface (experimental)

A gentle slope printed in flat layers shows a step at every layer. Forca has two options that work on those steps
with moves that leave the layer plane and follow the model's surface:

- **Nonplanar ironing** irons all the steps of a slope in one pass along the surface, instead of ironing each step
  flat.
- **Nonplanar top surface** takes the steps out of their layers and prints the top of the slope as one skin over
  them, so the surface is smooth before any ironing.

Both are off by default, in the process settings under **Quality > Ironing** (Advanced mode). They can be used
together: the skin is then ironed along the surface too.

## The printhead outline comes first

These moves bring the nozzle down beside plastic that is already printed. Forca only plans them where it is sure the
printhead stays clear, and for that it needs the shape of the printhead around the nozzle. That is the **Printhead
outline** group in the printer settings (**Basic information**, Advanced mode):

| Setting | What to measure |
| --- | --- |
| Nozzle tip diameter | The flat at the very tip of the nozzle. |
| Nozzle cone diameter | The nozzle where its cone ends: the top of the cone, or the lower edge of the silicone sock. |
| Nozzle cone height | How far above the tip that diameter is. |
| Distance to printhead body | From the centre of the nozzle to the nearest part that hangs low beside it: a fan duct, the shroud or the heater block. |
| Printhead body height | How far above the tip that lowest part is. |
| Printhead clearance margin | Extra distance kept on top of the outline, for measuring error (0.2 mm by default). |

With **Nozzle cone diameter** at 0 the outline is unknown, and both options do nothing. The Flashforge Adventurer 5M
0.4 and 0.6 profiles have it filled in. On that printer the nozzle can follow slopes up to about 18 degrees at the
default margin. A different nozzle (a longer one, a sock or no sock) changes the outline: measure it again.

## Where it works

- One object on the plate (one instance), without supports, a raft, a prime tower, a draft shield or a skirt taller
  than one layer, not in spiral mode. The clearance check knows only the object's own layers; anything else on the
  plate could be in the printhead's way. Forca warns and prints the plate as usual when one of these is missing.
- Gentle slopes: where a slope is steeper than the printhead can follow, that part prints planar, and Forca says so
  with the steepest slope it can follow.
- Turning on **Z contouring enabled** (Quality, just below the nonplanar options) helps the top surface: Forca
  slices the slope again for the skin, so it keeps its full thickness.

## Tips

- **Seam:** paint a seam (Seam painting tool) on the slope to choose where the ironing pass starts and works outward
  from, and where the wall around the top surface starts. Each run of lines starts and ends with a straight move down
  and up, which can leave a small mark; the seam puts it where you want it.
- **Ironing direction:** with the rectilinear ironing pattern the pass starts at the highest lines and works downhill,
  which keeps the level nozzle tip from ploughing into the slope beside it.
- **Flow:** if the ironed slope looks dull or has fine gaps, raise the ironing flow a little.

## Limits

- The top surface is one skin layer.
- The skin's lines are not ordered downhill, and there is no separate speed limit for the Z axis: the moves use the
  printer's Z limits.
- Valleys next to side walls may stay a little unfilled.

This is new and experimental: print a test piece before relying on it. The design is described in
[docs/HLSD/nonplanar.md](../../HLSD/nonplanar.md).
