// Forca: nonplanar ironing and the nonplanar top surface.
//
// Ironing normally irons the top surface of every layer flat, right after that layer. On a gentle slope each layer
// shows only a narrow step, so the slope is ironed step by step and the steps stay. Here the steps of a slope are
// collected into a chain, and the chain's last layer irons all of them in one pass that follows the model's surface
// (Layer::make_ironing() adds the areas, Layer::make_contour_z() gives the pass its Z).
//
// That pass moves the nozzle down beside layers that are already printed, so a step joins a chain only as long as the
// printhead's outline (PrintheadProfile) stays clear of every layer printed by then.
//
// The nonplanar top surface goes one step further. The steps of a chain are cut out of their layers right after
// slicing, so the layers under the slope end one layer lower, and the chain's last layer prints the missing material as
// one skin on the model's surface (Layer::make_nonplanar_shell()). Ironing then irons that skin.

#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
#include "Geometry.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "Polygon.hpp"
#include "Print.hpp"
#include "PrintBase.hpp"
#include "PrintConfig.hpp"
#include "PrintheadClearance.hpp"
#include "Surface.hpp"
#include "format.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

// Steps of two layers belong to one slope when they are no farther apart than this.
constexpr double TOUCH_DISTANCE = 0.05; // mm
// Rounding only. A step of a slope is about half a millimetre wide, so anything more would let through a slope
// noticeably steeper than the printhead follows.
constexpr double CLEARANCE_TOLERANCE = 0.002; // mm
// A piece of a step smaller than this is not worth a pass of its own.
constexpr double MIN_STEP_AREA = 0.2; // mm2

// One top surface of one layer.
struct SlopeStep
{
    size_t    layer;
    size_t    region_id;
    ExPolygon area;
};

// Steps of consecutive layers that touch each other. The last layer of a chain irons all of them.
struct Chain
{
    std::vector<SlopeStep> steps;
    // The steps of the chain's highest layer: the next layer's steps must touch these to join.
    ExPolygons        top;
    size_t            top_layer = 0;
    // Top of the chain's lowest layer: the pass that irons the chain goes down to it.
    double            lowest_z = 0.;
    bool              merged_away = false;
};

// A nonplanar top surface no smaller than this is worth printing.
constexpr double MIN_SHELL_AREA = 4.; // mm2

// Whether the region irons its top surfaces on this layer, and only those.
bool irons_tops(const LayerRegion &layerm, bool spiral_mode)
{
    const PrintRegionConfig &config = layerm.region().config();
    return config.ironing_type != IroningType::AllSolid && config.top_shell_layers > 0 &&
           Layer::choose_ironing_extruder(config, spiral_mode, layerm.layer()->upper_layer == nullptr) != -1;
}

// Whether the region irons its top surfaces on this layer, and wants it done along the surface.
bool irons_nonplanar(const LayerRegion &layerm, bool spiral_mode)
{
    return layerm.region().config().nonplanar_ironing.value && irons_tops(layerm, spiral_mode);
}

// How far the nozzle's axis must stay from printed material standing `height` above the nozzle tip.
double clearance(const PrintheadProfile &head, double height)
{
    return head.keep_out_radius(height);
}

// The part of a step that the layer `home` can iron: the nozzle goes down to the step while the layers up to `home`
// stand around it, and each of them must stay outside the printhead at its height above the step.
//
// The layers above the step are mostly the slope itself: its next steps. A slope the printhead can follow keeps them
// out of the way, but only just, so a layer that begins at the top surface of the layer below it (a step) counts
// from its lower face, and the layer right above the step only where it is not a step. Everything else, a wall
// beside the slope, counts in full.
ExPolygons reachable_part(const SlopeStep &step, size_t home, const LayerPtrs &layers, const std::vector<ExPolygons> &tops, const PrintheadProfile &head, double tip_radius)
{
    const double step_z = layers[step.layer]->print_z;
    ExPolygons   area{ step.area };
    for (size_t above = step.layer + 1; above <= home && ! area.empty(); ++above) {
        const Layer &layer = *layers[above];
        Polygons     keep_out;
        if (above == step.layer + 1) {
            append(keep_out, offset(diff_ex(layer.lslices, offset(tops[above], float(scale_(TOUCH_DISTANCE)))), float(scale_(tip_radius))));
        } else {
            const double as_step = std::max(0., clearance(head, layers[above - 1]->print_z - step_z) - CLEARANCE_TOLERANCE);
            append(keep_out, offset(layer.lslices, float(scale_(as_step))));
            const ExPolygons walls = diff_ex(layer.lslices, offset(tops[above - 1], float(scale_(head.keep_out_radius(layer.height)))));
            append(keep_out, offset(walls, float(scale_(clearance(head, layer.print_z - step_z)))));
        }
        area = diff_ex(area, keep_out);
    }
    area.erase(std::remove_if(area.begin(), area.end(), [](const ExPolygon &piece) { return piece.area() * SCALING_FACTOR * SCALING_FACTOR < MIN_STEP_AREA; }),
               area.end());
    return area;
}

// A piece of a step, and the layer that works on it: the last layer of the step's chain.
struct PlannedPiece
{
    size_t    home;
    size_t    layer;
    size_t    region_id;
    ExPolygon area;
};

// The steps of slopes that rise more steeply than the printhead can follow, and so are left to their layers.
struct SteepSteps
{
    double area  = 0.; // mm2
    // The steepest slope the printhead follows, in radians.
    double limit = 0.;
};

// The warning about them, empty when they are not worth one. `unused` is the sentence about what was left out there.
std::string steep_steps_warning(const SteepSteps &steep, const std::string &unused)
{
    return steep.area < MIN_SHELL_AREA ? std::string() : Slic3r::format(unused, int(std::lround(Geometry::rad2deg(steep.limit))));
}

// The top surfaces of a region on a layer that take part, empty when the region does not.
using RegionTops = std::function<ExPolygons(size_t layer_id, const LayerRegion &layerm)>;

// Chains the steps of the layers and gives, for every step below the last layer of its chain, the pieces that layer
// can reach. `tops` are all top surfaces of each layer. With `with_last_layer`, the steps of the last layer itself
// follow the pieces of a chain that has any. `steep` gets the steps that are too steep for any chain.
std::vector<PlannedPiece> plan_slopes(const LayerPtrs &layers, const std::vector<ExPolygons> &tops, const PrintheadProfile &head, double tip_radius,
                                      const RegionTops &region_tops_of, const std::function<void()> &throw_if_canceled, bool with_last_layer,
                                      SteepSteps &steep)
{
    std::vector<Chain> chains;
    // The steps of the layer below that are too narrow to follow.
    ExPolygons         narrow_below;
    for (size_t layer_id = 0; layer_id < layers.size(); ++layer_id) {
        throw_if_canceled();
        ExPolygons narrow;
        const Layer &layer = *layers[layer_id];
        // A step narrower than this rises to the next layer more steeply than the printhead can follow.
        const double min_width = layer.upper_layer == nullptr ? 0. :
            std::max(0., clearance(head, layer.upper_layer->slice_z - layer.slice_z) - CLEARANCE_TOLERANCE);
        // Chains that reach up to the layer below and are not too tall yet, and their steps of this layer.
        std::vector<size_t>     open;
        std::vector<ExPolygons> open_new_top;
        for (size_t i = 0; i < chains.size(); ++i)
            if (! chains[i].merged_away && layer_id > 0 && chains[i].top_layer == layer_id - 1 &&
                layer.print_z - chains[i].lowest_z <= head.max_surface_height()) {
                open.emplace_back(i);
                open_new_top.emplace_back();
            }
        std::vector<Chain> started;
        for (size_t region_id = 0; region_id < layer.regions().size(); ++region_id) {
            const LayerRegion &layerm = *layer.regions()[region_id];
            if (layerm.slices.empty())
                continue;
            ExPolygons region_tops = region_tops_of(layer_id, layerm);
            if (region_tops.empty())
                continue;
            region_tops = union_ex(region_tops);
            if (min_width > 0.) {
                ExPolygons wide = opening_ex(region_tops, float(scale_(0.5 * min_width)));
                // What is left out here as too steep: steps wider than the layer is tall, so of a slope under 45
                // degrees, on a step that is too narrow as well. A single narrow step is the cut off end of a slope
                // or the top of a thin wall.
                for (ExPolygon &lost : diff_ex(opening_ex(region_tops, float(scale_(0.5 * layer.height))), wide))
                    if (const double area = lost.area() * SCALING_FACTOR * SCALING_FACTOR; area >= MIN_STEP_AREA) {
                        if (! intersection_ex(offset_ex(lost, float(scale_(TOUCH_DISTANCE))), narrow_below).empty()) {
                            steep.area += area;
                            steep.limit = std::atan2(layer.upper_layer->slice_z - layer.slice_z, min_width);
                        }
                        narrow.emplace_back(std::move(lost));
                    }
                region_tops = std::move(wide);
            }
            for (ExPolygon &top : region_tops) {
                SlopeStep             step{ layer_id, region_id, std::move(top) };
                const ExPolygons grown = offset_ex(step.area, float(scale_(TOUCH_DISTANCE)));
                // The step joins the chain it touches; a step touching several chains makes them one.
                size_t joined = open.size();
                for (size_t i = 0; i < open.size(); ++i) {
                    Chain &chain = chains[open[i]];
                    if (chain.merged_away || intersection_ex(grown, chain.top).empty())
                        continue;
                    if (joined == open.size()) {
                        joined = i;
                        continue;
                    }
                    Chain &first = chains[open[joined]];
                    append(first.steps, std::move(chain.steps));
                    append(first.top, std::move(chain.top));
                    append(open_new_top[joined], std::move(open_new_top[i]));
                    first.lowest_z    = std::min(first.lowest_z, chain.lowest_z);
                    chain.merged_away = true;
                }
                if (joined == open.size()) {
                    Chain chain;
                    chain.top       = { step.area };
                    chain.top_layer = layer_id;
                    chain.lowest_z  = layer.print_z;
                    chain.steps.emplace_back(std::move(step));
                    started.emplace_back(std::move(chain));
                } else {
                    open_new_top[joined].emplace_back(step.area);
                    chains[open[joined]].steps.emplace_back(std::move(step));
                }
            }
        }
        for (size_t i = 0; i < open.size(); ++i)
            if (Chain &chain = chains[open[i]]; ! chain.merged_away && ! open_new_top[i].empty()) {
                chain.top       = std::move(open_new_top[i]);
                chain.top_layer = layer_id;
            }
        append(chains, std::move(started));
        narrow_below = std::move(narrow);
    }

    std::vector<PlannedPiece> pieces;
    for (const Chain &chain : chains) {
        if (chain.merged_away)
            continue;
        throw_if_canceled();
        const size_t before = pieces.size();
        for (const SlopeStep &step : chain.steps)
            if (step.layer < chain.top_layer)
                for (ExPolygon &piece : reachable_part(step, chain.top_layer, layers, tops, head, tip_radius))
                    pieces.push_back({ chain.top_layer, step.layer, step.region_id, std::move(piece) });
        if (with_last_layer && pieces.size() > before)
            for (const SlopeStep &step : chain.steps)
                if (step.layer == chain.top_layer)
                    pieces.push_back({ chain.top_layer, step.layer, step.region_id, step.area });
    }
    return pieces;
}

} // namespace

// The clearance check only knows the object's own layers.
bool PrintObject::prints_alone() const
{
    const PrintConfig &config = m_print->config();
    return m_print->objects().size() == 1 && this->instances().size() == 1 && ! m_print->has_wipe_tower() && ! this->has_support_material() &&
           config.draft_shield.value == dsDisabled && (! m_print->has_skirt() || config.skirt_height.value <= 1) &&
           ! config.spiral_mode.value && ! config.belt_printer.value;
}

void PrintObject::plan_nonplanar_ironing()
{
    for (Layer *layer : m_layers) {
        layer->nonplanar_ironing_sources.clear();
        layer->nonplanar_ironing_deferred.clear();
    }

    // A nonplanar top surface is ironed where it is: along the model's surface, by the layer that printed it. The
    // layers under it are not ironed there.
    bool shell = false;
    for (Layer *home : m_layers)
        for (const Layer::NonplanarIroningSource &source : home->nonplanar_shell_sources) {
            shell = true;
            if (source.layer->lower_layer != nullptr)
                source.layer->lower_layer->nonplanar_ironing_deferred.emplace_back(source.area);
            if (irons_tops(*home->regions()[std::min(source.region_id, home->regions().size() - 1)], m_print->config().spiral_mode.value))
                home->nonplanar_ironing_sources.push_back(source);
        }
    if (shell)
        return;

    bool wanted = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions() && ! wanted; ++region_id) {
        const PrintRegionConfig &config = this->printing_region(region_id).config();
        wanted = config.nonplanar_ironing.value && config.ironing_type != IroningType::NoIroning;
    }
    if (! wanted || m_layers.size() < 2)
        return;

    const PrintConfig      &print_config = m_print->config();
    const PrintheadProfile  head         = PrintheadProfile::from_config(print_config);
    // Print::apply() plans again when this changes: nothing in the object does.
    m_nonplanar_planned_alone = int(this->prints_alone());
    if (head.empty() || ! this->prints_alone()) {
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
            _u8L("Nonplanar ironing was not used. It needs the printhead outline in the printer settings and a single object "
                 "on the plate, without supports, a prime tower, a draft shield or a skirt taller than one layer."));
        return;
    }

    std::vector<ExPolygons> tops(m_layers.size());
    for (size_t layer_id = 0; layer_id < m_layers.size(); ++layer_id) {
        for (const LayerRegion *layerm : m_layers[layer_id]->regions())
            for (const Surface &surface : layerm->slices.surfaces)
                if (surface.surface_type == stTop)
                    tops[layer_id].emplace_back(surface.expolygon);
        tops[layer_id] = union_ex(tops[layer_id]);
    }

    const auto region_tops = [spiral_mode = print_config.spiral_mode.value](size_t, const LayerRegion &layerm) {
        ExPolygons out;
        if (irons_nonplanar(layerm, spiral_mode))
            for (const Surface &surface : layerm.slices.surfaces)
                if (surface.surface_type == stTop)
                    out.emplace_back(surface.expolygon);
        return out;
    };
    SteepSteps steep;
    for (PlannedPiece &piece : plan_slopes(m_layers, tops, head, 0.5 * print_config.nozzle_tip_diameter.value, region_tops,
                                           [this]() { m_print->throw_if_canceled(); }, false, steep)) {
        m_layers[piece.layer]->nonplanar_ironing_deferred.emplace_back(piece.area);
        m_layers[piece.home]->nonplanar_ironing_sources.push_back({ m_layers[piece.layer], piece.region_id, std::move(piece.area) });
    }
    // TRN %1% is an angle in whole degrees.
    if (const std::string warning = steep_steps_warning(steep, _u8L("Part of the model is steeper than the printhead can follow, which is about "
                                                                    "%1%°. Nonplanar ironing was not used there."));
        ! warning.empty())
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, warning);
}

void PrintObject::plan_nonplanar_shell()
{
    bool wanted = false;
    for (size_t region_id = 0; region_id < this->num_printing_regions() && ! wanted; ++region_id) {
        const PrintRegionConfig &config = this->printing_region(region_id).config();
        wanted = config.nonplanar_top_surface.value && config.top_shell_layers > 0;
    }
    if (! wanted || m_layers.size() < 3)
        return;

    const PrintConfig      &print_config = m_print->config();
    const PrintheadProfile  head         = PrintheadProfile::from_config(print_config);
    // Print::apply() plans again when this changes: nothing in the object does.
    m_nonplanar_planned_alone = int(this->prints_alone());
    if (head.empty() || ! this->prints_alone()) {
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
            _u8L("Nonplanar top surface was not used. It needs the printhead outline in the printer settings and a single "
                 "object on the plate, without supports, a prime tower, a draft shield or a skirt taller than one layer."));
        return;
    }

    // What each layer shows from above.
    std::vector<ExPolygons> tops(m_layers.size());
    for (size_t layer_id = 0; layer_id < m_layers.size(); ++layer_id)
        tops[layer_id] = layer_id + 1 < m_layers.size() ? diff_ex(m_layers[layer_id]->lslices, m_layers[layer_id + 1]->lslices) : m_layers[layer_id]->lslices;
    const auto region_tops = [&tops](size_t layer_id, const LayerRegion &layerm) {
        const PrintRegionConfig &config = layerm.region().config();
        if (! config.nonplanar_top_surface.value || config.top_shell_layers <= 0)
            return ExPolygons();
        return intersection_ex(to_expolygons(layerm.slices.surfaces), tops[layer_id]);
    };
    // The skin goes on over the top of the slope: the last layer's own step is part of it, cut out of that layer like
    // the others. Left in its layer it would be a flat strip with walls of its own across the skin.
    SteepSteps                steep;
    std::vector<PlannedPiece> pieces = plan_slopes(m_layers, tops, head, 0.5 * print_config.nozzle_tip_diameter.value, region_tops,
                                                   [this]() { m_print->throw_if_canceled(); }, true, steep);
    // TRN %1% is an angle in whole degrees.
    if (const std::string warning = steep_steps_warning(steep, _u8L("Part of the model is steeper than the printhead can follow, which is about "
                                                                    "%1%°. Nonplanar top surface was not used there."));
        ! warning.empty())
        this->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL, warning);

    // The first layer stays whole, and a skin of a few square millimetres of slope is not worth it.
    std::map<size_t, double> area_of_home;
    for (const PlannedPiece &piece : pieces)
        if (piece.layer > 0 && piece.layer < piece.home)
            area_of_home[piece.home] += piece.area.area() * SCALING_FACTOR * SCALING_FACTOR;
    const auto accepted = [&area_of_home](const PlannedPiece &piece) { return piece.layer > 0 && area_of_home[piece.home] >= MIN_SHELL_AREA; };

    // The skin on a step is as thick as its height above the layer under the step. A layer sliced at its middle shows
    // a step where the surface is within half a layer of the layer's top, so the skin is half a layer to a layer and a
    // half thick. Z contouring slices the layers near their bottom instead, which moves every step down the slope and
    // leaves the skin that much thinner. The lower edge of each step, where the surface is below the middle of the
    // step's layer, is then given to the step below: the skin there reaches one layer deeper.
    bool sliced_low = false;
    for (size_t layer_id = 1; layer_id < m_layers.size() && ! sliced_low; ++layer_id)
        sliced_low = m_layers[layer_id]->slice_z < m_layers[layer_id]->print_z - 0.5 * m_layers[layer_id]->height - EPSILON;
    std::vector<ExPolygons>             middles;
    std::set<std::pair<size_t, size_t>> steps_of_home;
    if (sliced_low) {
        std::vector<float> zs;
        zs.reserve(m_layers.size());
        for (const Layer *layer : m_layers)
            zs.emplace_back(float(layer->print_z - 0.5 * layer->height));
        middles = this->slice_model_parts(zs);
        for (const PlannedPiece &piece : pieces)
            if (accepted(piece))
                steps_of_home.emplace(piece.home, piece.layer);
    }

    std::vector<Polygons> cut(m_layers.size());
    for (PlannedPiece &piece : pieces)
        if (accepted(piece)) {
            polygons_append(cut[piece.layer], to_polygons(piece.area));
            // Only where the layer that prints the skin already reaches the step below.
            if (sliced_low && piece.layer > 1 && steps_of_home.count({ piece.home, piece.layer - 1 }) > 0) {
                ExPolygons lower_edge = diff_ex(ExPolygons{ piece.area }, middles[piece.layer]);
                lower_edge.erase(std::remove_if(lower_edge.begin(), lower_edge.end(),
                                                [](const ExPolygon &part) { return part.area() * SCALING_FACTOR * SCALING_FACTOR < MIN_STEP_AREA; }),
                                 lower_edge.end());
                if (! lower_edge.empty()) {
                    polygons_append(cut[piece.layer - 1], to_polygons(lower_edge));
                    for (ExPolygon &part : diff_ex(ExPolygons{ piece.area }, lower_edge))
                        m_layers[piece.home]->nonplanar_shell_sources.push_back({ m_layers[piece.layer], piece.region_id, std::move(part) });
                    for (ExPolygon &part : lower_edge)
                        m_layers[piece.home]->nonplanar_shell_sources.push_back({ m_layers[piece.layer - 1], piece.region_id, std::move(part) });
                    continue;
                }
            }
            m_layers[piece.home]->nonplanar_shell_sources.push_back({ m_layers[piece.layer], piece.region_id, std::move(piece.area) });
        }
    // The layers under the skin lay the lines it is printed on across its own (group_fills()).
    for (const Layer *home : m_layers)
        for (const Layer::NonplanarIroningSource &source : home->nonplanar_shell_sources)
            if (source.layer->lower_layer != nullptr)
                source.layer->lower_layer->nonplanar_shell_above.emplace_back(source.area);
    for (size_t layer_id = 0; layer_id < m_layers.size(); ++layer_id)
        if (! cut[layer_id].empty()) {
            Layer &layer = *m_layers[layer_id];
            for (LayerRegion *layerm : layer.regions())
                layerm->slices.set(diff_ex(to_expolygons(layerm->slices.surfaces), cut[layer_id]), stInternal);
            layer.make_slices();
        }
}

} // namespace Slic3r
