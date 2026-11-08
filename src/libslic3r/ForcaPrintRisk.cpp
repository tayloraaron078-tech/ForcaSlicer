#include "ForcaPrintRisk.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

// Tree supports use 30 degrees when the threshold angle is 0; the risk map uses the same.
constexpr double DEFAULT_THRESHOLD_ANGLE = 30.;
// A risk on the next layer continues one below when their boxes come this close, mm.
constexpr double CONTINUE_GAP = 1.;

double area_mm2(const ExPolygon& e) { return e.area() * SCALING_FACTOR * SCALING_FACTOR; }

BoundingBoxf box_mm(const ExPolygon& e)
{
    const BoundingBox b = get_extents(e);
    return BoundingBoxf(unscaled(b.min), unscaled(b.max));
}

// How far `air` reaches from `lower`: the smallest offset of `lower` that covers all of it, to `step` mm.
double reach_mm(const ExPolygon& air, const ExPolygons& lower, double start, double step)
{
    const BoundingBox b  = get_extents(air);
    double            hi = start + unscaled(b.size().cast<double>().norm());
    BoundingBox       clip_box(b);
    clip_box.offset(scaled(hi));
    const ExPolygons near = intersection_ex(lower, Polygons{ clip_box.polygon() });
    double           lo   = start;
    while (hi - lo > step) {
        const double mid = 0.5 * (lo + hi);
        (diff_ex(air, offset_ex(near, float(scaled(mid)))).empty() ? hi : lo) = mid;
    }
    return hi;
}

// The support printed just under a layer: the support layers between one layer height below its bottom and its bottom.
ExPolygons support_under(const PrintObject& object, const Layer& layer)
{
    const double bottom = layer.print_z - layer.height;
    const double eps    = EPSILON;
    ExPolygons   out;
    for (const SupportLayer* s : object.support_layers()) {
        if (s->print_z < bottom - 3. * layer.height - eps || s->print_z > bottom + eps)
            continue;
        if (!s->support_islands.empty())
            append(out, s->support_islands);
        else // the tree generators may leave support_islands empty
            append(out, union_ex(s->support_fills.polygons_covered_by_width()));
    }
    return out;
}

struct Found
{
    PrintRiskType type;
    BoundingBoxf  bbox;
    double        distance;
    double        area;
    bool          supported;
};

// The risks one layer adds over the layer below.
std::vector<Found> layer_risks(const PrintObject& object, const Layer& layer, double threshold_deg, double max_bridge)
{
    std::vector<Found> out;
    const ExPolygons&  lower     = layer.lower_layer->lslices;
    const ExPolygons   support   = support_under(object, layer);
    auto               supported = [&](const ExPolygon& e) { return !support.empty() && overlaps(support, e); };

    ExPolygons held; // the parts of the slice that stand on something
    for (const ExPolygon& e : layer.lslices) {
        if (intersection_ex(e, lower).empty())
            out.push_back({ PrintRiskType::FloatingIsland, box_mm(e), 0., area_mm2(e), supported(e) });
        else
            held.push_back(e);
    }
    if (held.empty())
        return out;

    // Each layer may step out by height / tan(threshold) (the slope of the support threshold angle) without support.
    const double step = layer.height / std::tan(threshold_deg * PI / 180.);
    ExPolygons   air  = diff_ex(held, offset_ex(lower, float(scaled(step))));
    air               = opening_ex(air, float(scaled(step) / 2.)); // slivers narrower than one step are no risk
    for (const ExPolygon& a : air) {
        // The anchors: the pieces of the layer below it touches.
        const size_t anchors = intersection_ex(offset_ex(a, float(scaled(1.5 * step + 0.1))), lower).size();
        const double reach   = reach_mm(a, lower, step, 0.1);
        if (anchors >= 2) {
            // The farthest point of a bridge from its anchors is half its span.
            const double span = 2. * reach;
            if (span > max_bridge)
                out.push_back({ PrintRiskType::LongBridge, box_mm(a), span, area_mm2(a), supported(a) });
        } else
            out.push_back({ PrintRiskType::Overhang, box_mm(a), reach, area_mm2(a), supported(a) });
    }
    return out;
}

// `f` belongs to `r`: the same kind of risk, close by.
bool joins(const PrintRisk& r, const Found& f)
{
    if (r.type != f.type)
        return false;
    BoundingBoxf b = r.bbox;
    b.offset(CONTINUE_GAP);
    return b.overlap(f.bbox);
}

} // namespace

std::vector<PrintRisk> forca_print_risks(const PrintObject& object)
{
    const PrintObjectConfig& config    = object.config();
    const double             threshold = config.support_threshold_angle.value > 0 ? config.support_threshold_angle.value
                                                                                   : DEFAULT_THRESHOLD_ANGLE;
    const double             max_bridge = config.max_bridge_length.value;

    // `open`: the risks the layer below had, which this layer may continue; `next`: the ones this layer has.
    std::vector<PrintRisk> done, open;
    for (size_t i = 0; i < object.layer_count(); ++i) {
        const Layer& layer = *object.get_layer(int(i));
        if (!layer.lower_layer)
            continue;
        std::vector<PrintRisk> next;
        for (const Found& f : layer_risks(object, layer, threshold, max_bridge)) {
            // A risk this layer already has, then one it continues from the layer below, else a new one.
            auto it = std::find_if(next.begin(), next.end(), [&](const PrintRisk& r) { return joins(r, f); });
            if (it == next.end()) {
                auto below = std::find_if(open.begin(), open.end(), [&](const PrintRisk& r) { return joins(r, f); });
                if (below != open.end()) {
                    next.push_back(std::move(*below));
                    open.erase(below);
                } else {
                    PrintRisk r;
                    r.type        = f.type;
                    r.first_layer = i;
                    r.z_min       = layer.print_z;
                    r.bbox        = f.bbox;
                    r.supported   = true;
                    next.push_back(r);
                }
                it = next.end() - 1;
            }
            it->last_layer = i;
            it->z_max      = layer.print_z;
            it->bbox.merge(f.bbox);
            it->distance  = std::max(it->distance, f.distance);
            it->area      = std::max(it->area, f.area);
            it->supported = it->supported && f.supported;
        }
        append(done, std::move(open));
        open = std::move(next);
    }
    append(done, std::move(open));
    std::stable_sort(done.begin(), done.end(), [](const PrintRisk& a, const PrintRisk& b) { return a.first_layer < b.first_layer; });
    return done;
}

} // namespace Slic3r
