#pragma once

// Forca: the print-risk map. Where a sliced object is likely to fail, from what the slice already holds: overhangs past
// the support threshold angle, bridges longer than max_bridge_length, and islands that start in mid-air. Run on demand
// over a sliced PrintObject; nothing in the slicing pipeline calls it, so it costs nothing when unused.

#include "BoundingBox.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

class PrintObject;

enum class PrintRiskType
{
    Overhang,       // printed over air, held on one side
    LongBridge,     // printed over air between two or more anchors, longer than max_bridge_length
    FloatingIsland, // a new part of the slice with nothing below it
};

// One risk, gathered over the consecutive layers where it continues.
struct PrintRisk
{
    PrintRiskType type        = PrintRiskType::Overhang;
    size_t        first_layer = 0; // indices into the object's layers
    size_t        last_layer  = 0;
    double        z_min       = 0; // print_z of first_layer / last_layer, mm
    double        z_max       = 0;
    BoundingBoxf  bbox;            // XY in the object's slice coordinates, mm (add an instance's shift to place it)
    double        distance    = 0; // overhang: how far it reaches from the layer below; bridge: its span; mm
    double        area        = 0; // the largest area over air on one layer, mm^2
    bool          supported   = false; // support material is under it on every layer
};

// The risks of a sliced object (posSlice done; support known only after posSupportMaterial), lowest first.
std::vector<PrintRisk> forca_print_risks(const PrintObject& object);

} // namespace Slic3r
