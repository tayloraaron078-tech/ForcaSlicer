#pragma once

// Forca: the print-risk map. Where a sliced object is likely to fail, from what the slice already holds: overhangs past
// the support threshold angle, bridges longer than max_bridge_length, islands that start in mid-air, thin parts that
// stand tall, and a first layer too small for the object on it. Layers too short to cool come from the G-code's layer
// times instead (forca_short_layer_risks). Run on demand; nothing in the slicing pipeline calls it, so it costs nothing
// when unused.

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
    ThinTall,       // a narrow part standing much taller than it is wide: it wobbles under the nozzle
    SmallFootprint, // a first layer much smaller than the object on it: it may come loose or tip over
    ShortLayers,    // layers printed faster than the filament's layer time allows them to cool
};

// The rules' limits.
constexpr double FORCA_THIN_WIDTH       = 8.;  // ThinTall: a part narrower than this, mm,
constexpr double FORCA_TALL_RATIO       = 8.;  // ... standing this many times its width
constexpr double FORCA_FOOTPRINT_RATIO  = 0.2; // SmallFootprint: first layer area below this share of the largest layer's,
constexpr double FORCA_TIP_RATIO        = 3.;  // ... or the object this many times taller than its base is wide,
constexpr double FORCA_FOOTPRINT_MIN_H  = 10.; // ... and at least this tall, mm
constexpr double FORCA_SHORT_LAYER_KEEP = 0.9; // ShortLayers: a layer shorter than this share of the layer time

// One risk, gathered over the consecutive layers where it continues.
struct PrintRisk
{
    PrintRiskType type        = PrintRiskType::Overhang;
    size_t        first_layer = 0; // indices into the object's layers
    size_t        last_layer  = 0;
    double        z_min       = 0; // print_z of first_layer / last_layer, mm
    double        z_max       = 0;
    BoundingBoxf  bbox;            // XY in the object's slice coordinates, mm (add an instance's shift to place it)
    double        distance    = 0; // overhang: how far it reaches from the layer below; bridge: its span; thin part:
                                   // its widest, mm; small footprint: the object's height, mm; short layers: the
                                   // shortest layer time, s
    double        area        = 0; // the largest area over air on one layer; small footprint: the first layer's; mm^2
    double        limit       = 0; // small footprint: the largest layer's area, mm^2; short layers: the layer time, s
    bool          supported   = false; // support material is under it on every layer; small footprint: a brim or raft
                                       // holds it; never for a thin part or short layers
};

// A printed layer's top and how long it takes, from the G-code.
struct ForcaLayerTime
{
    double z       = 0; // mm
    double seconds = 0;
};

// The risks of a sliced object (posSlice done; support known only after posSupportMaterial), lowest first.
std::vector<PrintRisk> forca_print_risks(const PrintObject& object);

// The runs of layers shorter than FORCA_SHORT_LAYER_KEEP x `layer_time` seconds (the filament's slow_down_layer_time),
// lowest first; first_layer / last_layer index `layers`. Nothing when layer_time is 0.
std::vector<PrintRisk> forca_short_layer_risks(const std::vector<ForcaLayerTime>& layers, double layer_time);

// True when a risk is taken care of: support under it, or a brim or raft holding a small footprint.
inline bool forca_risk_handled(const PrintRisk& r) { return r.supported; }

} // namespace Slic3r
