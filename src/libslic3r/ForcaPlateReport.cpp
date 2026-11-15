#include "ForcaPlateReport.hpp"

#include "BoundingBox.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace Slic3r {

namespace {

constexpr double TOLERANCE = 0.01; // mm, °C

} // namespace

std::vector<ForcaPlateCheck> forca_plate_checks(const ForcaPlateFacts& f)
{
    std::vector<ForcaPlateCheck> out;
    auto add = [&out](ForcaPlateCheckId id, bool warning, double value = 0, double limit = 0, const std::string& filament = {},
                      double low = 0) { out.push_back({ id, warning, filament, value, low, limit }); };

    // Where the objects stand.
    if (f.objects.defined && f.bed.defined) {
        const double left   = f.objects.min.x() - f.bed.min.x();
        const double right  = f.bed.max.x() - f.objects.max.x();
        const double front  = f.objects.min.y() - f.bed.min.y();
        const double back   = f.bed.max.y() - f.objects.max.y();
        const double margin = std::min({ left, right, front, back });
        if (margin < -TOLERANCE || f.toolpath_outside)
            add(ForcaPlateCheckId::OutsideBed, true);
        else if (margin < FORCA_EDGE_MARGIN)
            add(ForcaPlateCheckId::NearEdge, false, std::max(margin, 0.), FORCA_EDGE_MARGIN);
    } else if (f.toolpath_outside)
        add(ForcaPlateCheckId::OutsideBed, true);
    if (f.objects.defined && f.max_height > 0) {
        const double height = f.objects.max.z();
        if (height > f.max_height + TOLERANCE)
            add(ForcaPlateCheckId::TooTall, true, height, f.max_height);
        else if (height > FORCA_HEIGHT_SHARE * f.max_height)
            add(ForcaPlateCheckId::NearMaxHeight, false, height, f.max_height);
    }

    // Layer heights against the nozzle.
    if (f.nozzle_diameter > 0) {
        const double most = FORCA_LAYER_SHARE * f.nozzle_diameter;
        if (f.layer_height > most + TOLERANCE)
            add(ForcaPlateCheckId::LayerTooThick, true, f.layer_height, most);
        if (f.first_layer_height > most + TOLERANCE)
            add(ForcaPlateCheckId::FirstLayerTooThick, true, f.first_layer_height, most);
    }

    // Each filament's temperatures, then the filaments against each other.
    for (const ForcaPlateFilament& fil : f.filaments) {
        if (fil.range_high <= 0)
            continue;
        for (const double t : { fil.first_nozzle_temp, fil.nozzle_temp })
            if (t > 0 && (t < fil.range_low - TOLERANCE || t > fil.range_high + TOLERANCE)) {
                add(ForcaPlateCheckId::NozzleTempOutsideRange, true, t, fil.range_high, fil.name, fil.range_low);
                break;
            }
    }
    if (f.filaments.size() > 1) {
        double nmin = 1e9, nmax = -1e9, bmin = 1e9, bmax = -1e9;
        for (const ForcaPlateFilament& fil : f.filaments) {
            nmin = std::min(nmin, fil.nozzle_temp);
            nmax = std::max(nmax, fil.nozzle_temp);
            bmin = std::min(bmin, fil.bed_temp);
            bmax = std::max(bmax, fil.bed_temp);
        }
        if (bmax - bmin > FORCA_BED_TEMP_SPREAD + TOLERANCE)
            add(ForcaPlateCheckId::BedTempsFarApart, true, bmax - bmin, FORCA_BED_TEMP_SPREAD, {}, bmin);
        if (nmax - nmin > FORCA_NOZZLE_TEMP_SPREAD + TOLERANCE)
            add(ForcaPlateCheckId::NozzleTempsFarApart, false, nmax - nmin, FORCA_NOZZLE_TEMP_SPREAD, {}, nmin);
    }

    std::stable_sort(out.begin(), out.end(), [](const ForcaPlateCheck& a, const ForcaPlateCheck& b) { return a.warning && !b.warning; });
    return out;
}

std::string forca_plate_check_key(ForcaPlateCheckId id)
{
    switch (id) {
    case ForcaPlateCheckId::OutsideBed: return "outside_bed";
    case ForcaPlateCheckId::TooTall: return "too_tall";
    case ForcaPlateCheckId::NearMaxHeight: return "near_max_height";
    case ForcaPlateCheckId::NearEdge: return "near_edge";
    case ForcaPlateCheckId::LayerTooThick: return "layer_too_thick";
    case ForcaPlateCheckId::FirstLayerTooThick: return "first_layer_too_thick";
    case ForcaPlateCheckId::NozzleTempOutsideRange: return "nozzle_temp_outside_range";
    case ForcaPlateCheckId::NozzleTempsFarApart: return "nozzle_temps_far_apart";
    case ForcaPlateCheckId::BedTempsFarApart: return "bed_temps_far_apart";
    }
    return {};
}

} // namespace Slic3r
