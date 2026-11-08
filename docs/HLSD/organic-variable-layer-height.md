# Organic supports with a variable layer height — High Level Design

## Purpose and scope

Organic supports (`support_style` Organic, and Default, which resolves to it)
come from Thomas Rahm's tree supports, which plan a tree on layer indices and
assume every layer has the same height. With adaptive, smoothed or painted
layer heights, or a height range with its own layer height, that assumption
fails. OrcaSlicer, like PrusaSlicer, refuses to slice the combination
("Variable layer height is not supported with Organic supports"). Forca plans
the tree on the object's real layers instead, so the combination slices.

Objects with fixed layers take exactly the path they took before. Every change
is behind `TreeSupportSettings::has_variable_layer_height()`, and the support
toolpaths of a fixed-layer object are byte-identical with and without this code.
The classic Tree styles (Slim, Strong, Hybrid) are a separate generator
(`TreeSupport.cpp`) that already plans its layers in mm, on the object's layers
or on independent support layers, and limits each branch move by the node's own
layer height; it never refused variable layer heights and is not changed here.

## When an object counts as variable

`TreeSupportMeshGroupSettings` fills `object_layer_print_z` with the print_z of
every object layer when the object has custom layering and its layer height
profile is not a fixed one (`check_object_layers_fixed`). That is the condition
under which `Print::validate` used to refuse. Otherwise the vector stays empty.
The values are in the tree's local Z (minus the belt printer's global offset),
like `layer_z()`.

## What changes for a variable object

`TreeSupportSettings` builds `variable_layer_z`: the raft layers, then the object
layers. From it come:

- **Layer positions.** `layer_z()`, `layer_idx_ceil()` and `layer_idx_floor()`
  look the real heights up instead of computing them from the nominal layer
  height. Support layers are therefore printed at the heights of object layers.
  This also covers the organic branch meshes, which are sliced at `layer_z()`,
  and the collision spheres of the organic smoothing.
- **Branch movement.** A branch may lean by the support angle, so how far it may
  move between two layers depends on the height between them.
  `maximum_move_distance_at()` and `maximum_move_distance_slow_at()` give that
  per layer, and the avoidance volumes (`TreeModelVolumes::calculateAvoidance`),
  the downward growth (`increase_areas_one_layer`) and the smoothing all use
  them. The value for layer L is the move from L down to L - 1, over L's height.
  With the nominal distances a branch would lean past the support angle on thin
  layers, which prints as an overhang in the support.
- **Z gaps.** The top and bottom Z distances are set in mm, but the tree keeps
  them as numbers of empty layers. `z_distance_top_layers_at()` and
  `z_distance_bottom_layers_at()` count, per layer, the layers that the gap
  covers, where a layer counts when at least half of it is inside the gap. For
  uniform layers that is `round(distance / layer_height)`, the fixed-layer
  value. `support_layer_below_overhang()` picks the support layer under an
  overhang as the highest layer whose own top gap stays below it, so the
  collision areas (which use the same per-layer counts) never cover the
  overhang. Two overhangs can map to the same support layer; tip and roof
  placement already lock for that.

Values counted in layers keep the nominal layer height: the tip length, the
radius growth, the elephant-foot growth near the bed, the minimum height to the
model and the number of interface layers. The radius only shrinks going up, so
faster growth per mm on thin layers prints without an overhang.

## Constraints

- No project or profile change: the detection reads the layer height profile
  the object already has.
- The regional support interface's flush gap (`support_interface_modifier_gap`)
  counts its kept layers with the same per-layer rule
  (`gap_layers_above()`).
- Tests: `tests/fff_print/test_tree_support.cpp`, "Organic support is printed on
  the layers of a variable layer height" (with and without a raft) and "Organic
  support gaps and branch moves follow each layer's height".
