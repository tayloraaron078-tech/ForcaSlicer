#pragma once

// Forca: the plate report's fixed rules. Plain facts about a plate (where its objects stand on the bed, the layer
// heights, each filament's temperatures) go in; the checks that fail come out, with the numbers they were judged on.
// No AI and no guessing: every check is a comparison with a setting or a stated limit. The GUI (ForcaPlateReportDialog)
// gathers the facts and words the checks; Forca AI gets the same checks by id.

#include "BoundingBox.hpp"

#include <string>
#include <vector>

namespace Slic3r {

// The rules' limits.
constexpr double FORCA_EDGE_MARGIN       = 5.;   // NearEdge: closer than this to the edge of the bed, mm
constexpr double FORCA_HEIGHT_SHARE      = 0.9;  // NearMaxHeight: taller than this share of the printable height
constexpr double FORCA_LAYER_SHARE       = 0.8;  // LayerTooThick / FirstLayerTooThick: above this share of the nozzle
constexpr double FORCA_NOZZLE_TEMP_SPREAD = 30.; // NozzleTempsFarApart: filaments more than this apart, °C
constexpr double FORCA_BED_TEMP_SPREAD   = 15.;  // BedTempsFarApart: filaments more than this apart on this plate, °C

// One filament the plate prints with.
struct ForcaPlateFilament
{
    int         slot = 0;                 // 1-based
    std::string name;                     // the preset
    double      nozzle_temp       = 0;    // °C, other layers
    double      first_nozzle_temp = 0;    // °C, first layer
    double      range_low         = 0;    // the filament's recommended nozzle range, °C (both 0: unknown)
    double      range_high        = 0;
    double      bed_temp          = 0;    // on this plate's bed type, other layers, °C
};

struct ForcaPlateFacts
{
    BoundingBoxf                    bed;                  // the printable area, plate coordinates, mm
    double                          max_height = 0;       // printable height, mm (0: unknown)
    BoundingBoxf3                   objects;              // every object on the plate, plate coordinates, mm
    bool                            toolpath_outside = false; // the G-code goes outside the printable area
    double                          nozzle_diameter    = 0;
    double                          layer_height       = 0;
    double                          first_layer_height = 0;
    std::vector<ForcaPlateFilament> filaments;
};

enum class ForcaPlateCheckId
{
    OutsideBed,             // warning: an object (or the G-code) is outside the printable area
    TooTall,                // warning: taller than the printable height (value: height, limit: printable height)
    NearMaxHeight,          // note: above FORCA_HEIGHT_SHARE of it
    NearEdge,               // note: closer than FORCA_EDGE_MARGIN to an edge (value: the distance)
    LayerTooThick,          // warning: layer height above FORCA_LAYER_SHARE x nozzle (value, limit)
    FirstLayerTooThick,     // warning: first layer height likewise
    NozzleTempOutsideRange, // warning: a filament's nozzle temperature outside its range (filament, value, low, limit)
    NozzleTempsFarApart,    // note: the filaments' nozzle temperatures differ by more than FORCA_NOZZLE_TEMP_SPREAD
    BedTempsFarApart,       // warning: their bed temperatures on this plate differ by more than FORCA_BED_TEMP_SPREAD
};

struct ForcaPlateCheck
{
    ForcaPlateCheckId id;
    bool              warning  = false; // else a note
    std::string       filament;         // the filament it is about, if any
    double            value    = 0;
    double            low      = 0;
    double            limit    = 0;
};

// The checks a plate fails, warnings first. An empty plate (objects not defined) fails none of the position checks.
std::vector<ForcaPlateCheck> forca_plate_checks(const ForcaPlateFacts& facts);

// A check's id for Forca AI ("outside_bed", "too_tall", ...).
std::string forca_plate_check_key(ForcaPlateCheckId id);

} // namespace Slic3r
