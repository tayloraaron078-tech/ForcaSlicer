# Nonplanar ironing and nonplanar top surface — High Level Design

## Purpose and scope

A gentle slope printed in flat layers shows a step per layer. Two process options
work on those steps with paths that leave the layer plane and follow the model's
surface:

- `nonplanar_ironing` irons all the steps of a slope in one pass on the surface.
- `nonplanar_top_surface` takes the steps out of their layers and prints the
  material as one skin on the surface.

Both move the nozzle down beside material that is already printed, so both are
used only where a model of the printhead shows that it stays clear. They are off
by default, and they are refused, with a warning, without a printhead outline,
with more than one object or instance on the plate, with supports, a prime
tower, a draft shield, a skirt taller than one layer, spiral mode or a belt
printer: the clearance check knows only the object's own layers.

## The printhead outline

`PrintheadProfile` (`PrintheadClearance.hpp`) describes the printhead as rings
around the nozzle axis, each a radius and a height above the nozzle tip. It
answers how far the nozzle axis must stay from material standing a given height
above the tip (`keep_out_radius`), the steepest slope it follows up to a given
height (`max_slope`), and the tallest surface it can work on
(`max_surface_height`).

The printer options `nozzle_cone_diameter`, `nozzle_cone_height`,
`printhead_body_distance` and `printhead_body_height` give two rings: the widest
part right above the tip (a silicone sock, a heater block) and the lowest part
of the printhead beside it. `PrintheadProfile::from_config()` starts the outline
at the nozzle axis, because nonplanar paths put the centre of the flat tip on
the surface, and lowers every ring by `printhead_clearance_margin`. With the
cone unset the outline is empty and nothing nonplanar is planned.

`nozzle_tip_diameter` is the flat of the tip. It does not enter the clearance;
the paths use it (see "The flat tip").

## Chains of steps

`plan_slopes()` in `NonplanarIroning.cpp` is shared by both options. A step is a
top surface of one layer. Steps of consecutive layers that touch form a chain,
and the chain's last layer, its home, works on all of them. A chain stops
growing when it gets taller than the printhead allows. A step narrower than the
printhead's keep-out radius for one layer is too steep and is left out.

Steps left out that way print as ordinary layers, which the user would
otherwise only find on the print, so both options warn about them and name the
angle the printhead follows. Narrow alone is not steep: the cut off end of a
slope and the top of a thin wall are narrow too. A narrow step counts when it
is wider than its layer is tall, so of a slope under 45 degrees, and lies on a
step that is too narrow as well.

Each lower step is then trimmed to what the home layer can reach
(`reachable_part()`): every layer between the step and the home must stay
outside the printhead at its height above the step. The slope's own next steps
are judged from their lower face, since a slope the printhead can follow keeps
them out of the way only just; anything else, a wall beside the slope, counts in
full.

## Nonplanar ironing

`PrintObject::plan_nonplanar_ironing()` runs at the start of the ironing step,
on the `stTop` surfaces. It records each reachable piece twice: as deferred on
its own layer, which then does not iron it, and as a source on the home layer.
`Layer::make_ironing()` adds the sources to the home layer's ironing area, so
the fill produces lines that run across the steps.

`Layer::make_contour_z()` gives the lines their Z. For each point it finds the
layer whose step the point lies on and casts a ray up from that layer's slice
plane to the mesh, the same way Z contouring does within one layer, then
expresses the result relative to the home layer. This runs for these passes even
when Z contouring is off.

### The flat tip

The tip is a flat, level disc. With its centre on a slope its uphill edge is in
the surface and its downhill edge above it, by the tip's radius times the slope.
Three rules follow from that.

- On a slope the centre is on the surface. Riding on the uphill edge instead
  keeps the tip clear but drops the ironing plastic from above.
- In a hollow, such as the foot of a slope, the surface under the tip's edge
  stands higher than the slope accounts for. There the tip is lifted so that it
  presses no deeper than it does on the slope itself.
- A rectilinear pass steps downhill from line to line
  (`nonplanar_ironing_downhill()` in `Fill.cpp`). Stepping uphill, the buried
  edge leads into plastic that is not ironed yet and ploughs it; stepping
  downhill it trails and irons. The monotonic lines are cut where their mean
  height turns and each part is run from its high side. The part that runs on
  over a crest starts before it, where the lines are less than a layer lower,
  so no part starts on a small peak that was printed moments earlier.

Lines that follow each other closely but that the fill left unlinked, as at the
rim of a round area, are linked where the link stays on the ironed area. Without
that each would be left and reached separately.

## Nonplanar top surface

`PrintObject::plan_nonplanar_shell()` runs at the end of slicing, before
perimeters exist, on what each layer shows from above (its slices less those of
the layer above). The reachable pieces are cut out of their layers and recorded
as `Layer::nonplanar_shell_sources` on the home layer. Everything downstream
sees ordinary layers that end one layer lower under the slope; the surface they
now show there becomes a top surface with solid layers under it in the usual
way. The first layer is never cut.

The home layer's own step is part of the skin too, cut out of that layer like
the others, so the skin runs on over the top of a slope and over a crest
between two slopes. Left in its layer, that step is a flat strip with walls of
its own across the skin. A level area that a slope ends in is such a step and
is printed as skin as well. A home layer may then have nothing left but the
skin.

`Layer::make_nonplanar_shell()` then adds the skin to the home layer's fills, in
2D: a wall around each area and monotonic rectilinear lines inside it.
`contour_nonplanar_shell()` puts those paths on the mesh and cuts each one where
it passes from one step to the next, because the amount to extrude differs per
step: a piece on the step of layer L is as thick as its height above the layer
under L. The pieces carry that as `ExtrusionPath::z_contour_shift`, and
`nonplanar_continues` on all but the last so the nozzle stays down between them.
A path segment is extruded for the mean thickness along it.

The nozzle comes straight down for each run of lines and leaves straight up, and
both can mark the surface. At the rim of a round skin the first and the last
line are short and end millimetres from the ends of the lines next to them, too
far for the fill to link them. They are linked to the line next to them all the
same: a link from one line to the next crosses nothing printed, so the lines of
such a skin are one run.

A layer sliced at its middle shows a step where the surface is within half a
layer of the layer's top, so the skin is half a layer to a layer and a half
thick. Z contouring slices the layers near their bottom, which moves every step
down the slope and would leave the skin that much thinner. The plan then slices
the model once more at the middle of each layer
(`PrintObject::slice_model_parts()`) and gives the lower edge of each step,
where the surface is below the middle of the step's layer, to the step below:
that edge is cut out of both layers and the skin there reaches one layer deeper.
The lowest step of a slope keeps its lower edge.

The skin's lines run in `solid_infill_direction`, which is also one of the two
directions the solid layers alternate between. Lines laid along the lines under
them leave the grooves between those open, so a top surface that lies mostly
under a skin (`Layer::nonplanar_shell_above`) is filled 45 degrees off that
direction: the skin crosses it, and it crosses the layer under it. This matters
on level areas and very gentle slopes; on a steeper slope the skin lies mostly
on the walls of the layers under it.

When the region irons its top surfaces, the home layer irons the skin along the
surface, and the layers under the skin are not ironed there.

## Painted seam

A seam enforcer painted on a slope moves two starts. `nonplanar_painted_seam()`
in `Fill.cpp` collects the painted areas of the home layer and of the layers
whose steps it covers, seen from above, and the middle of the largest one on an
area is the seam point.

- The ironing pass over that area is cut on the line through the seam point and
  works outward from it to both sides. This overrides the downhill ordering: one
  side may then step uphill, which is the user's choice.
- The wall around a nonplanar top surface starts at its point nearest to the
  seam point.

A change of seam painting remakes the fills of an object that has a nonplanar
plan, besides the G-code.

## Plate contents

Whether an object prints alone is not a property of the object, so nothing in
it changes when another object, a prime tower or a skirt comes or goes.
`PrintObject` remembers what `prints_alone()` gave when it was last planned, and
`Print::apply()` slices the object again when that differs.

## G-code

A path that runs deeper below its layer than Z contouring allows, or any path of
a nonplanar top surface, is treated as nonplanar by `GCode::_extrude_piece()`: the
nozzle travels to its first point at the layer's height, goes straight down,
and comes straight back up after the path, so no travel cuts through the print.

## Invalidation

`nonplanar_top_surface` and the printhead outline options change the slices, so
they invalidate slicing. `nonplanar_ironing` only changes the ironing step.

## Known limits

- The pass ordering uses the mean height of each ironing line, so a surface
  that tilts both ways along one line is not handled.
- The skin is one layer, and its lines are not ordered downhill.
- XY speed is not reduced for the Z axis's limits on a slope.
- Other objects, supports and the prime tower are not in the clearance check,
  which is why the options are refused with them.
