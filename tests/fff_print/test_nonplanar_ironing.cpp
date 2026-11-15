#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintheadClearance.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"

#include <admesh/stl.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

constexpr double LAYER_HEIGHT = 0.2;
constexpr double LENGTH       = 40.; // of the wedge, along X
constexpr double WIDTH        = 20.; // along Y

// A block whose top rises along X from `low` to `high`.
TriangleMesh wedge(double low, double high)
{
    indexed_triangle_set its;
    its.vertices = { { 0.f, 0.f, 0.f }, { float(LENGTH), 0.f, 0.f }, { float(LENGTH), float(WIDTH), 0.f }, { 0.f, float(WIDTH), 0.f },
                     { 0.f, 0.f, float(low) }, { float(LENGTH), 0.f, float(high) }, { float(LENGTH), float(WIDTH), float(high) }, { 0.f, float(WIDTH), float(low) } };
    its.indices  = { { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 }, { 0, 1, 5 }, { 0, 5, 4 },
                     { 1, 2, 6 }, { 1, 6, 5 }, { 2, 3, 7 }, { 2, 7, 6 }, { 3, 0, 4 }, { 3, 4, 7 } };
    return TriangleMesh(its);
}

// A block whose top rises along X from `low` to `high` at its middle and falls back to `low`.
TriangleMesh ridge(double low, double high)
{
    indexed_triangle_set its;
    its.vertices = { { 0.f, 0.f, 0.f }, { float(LENGTH), 0.f, 0.f }, { float(LENGTH), float(WIDTH), 0.f }, { 0.f, float(WIDTH), 0.f },
                     { 0.f, 0.f, float(low) }, { float(0.5 * LENGTH), 0.f, float(high) }, { float(LENGTH), 0.f, float(low) },
                     { 0.f, float(WIDTH), float(low) }, { float(0.5 * LENGTH), float(WIDTH), float(high) }, { float(LENGTH), float(WIDTH), float(low) } };
    its.indices  = { { 0, 2, 1 }, { 0, 3, 2 }, { 0, 1, 6 }, { 0, 6, 5 }, { 0, 5, 4 }, { 3, 7, 8 }, { 3, 8, 9 }, { 3, 9, 2 },
                     { 3, 0, 4 }, { 3, 4, 7 }, { 1, 2, 9 }, { 1, 9, 6 }, { 4, 5, 8 }, { 4, 8, 7 }, { 5, 6, 9 }, { 5, 9, 8 } };
    return TriangleMesh(its);
}

// The printhead used by the tests: a 1 mm tip, a cone 11 mm wide 2 mm above it (21.8 degrees), and a body 13.5 mm
// from the nozzle hanging 5.5 mm above the tip.
PrintheadProfile printhead() { return PrintheadProfile::cone(0.5, 5.5, 2., 13.5, 5.5); }

void slice(Print &print, const TriangleMesh &mesh, bool printhead_outline, bool nonplanar = true)
{
    init_and_process_print({ mesh }, print, {
        { "layer_height", LAYER_HEIGHT },
        { "initial_layer_print_height", LAYER_HEIGHT },
        { "ironing_type", "top" },
        { "nonplanar_ironing", nonplanar },
        { "nozzle_tip_diameter", 1. },
        { "nozzle_cone_diameter", printhead_outline ? 11. : 0. },
        { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 },
        { "printhead_body_height", 5.5 },
        { "skirt_loops", 0 },
    });
}

// A point of an ironing path, in the object's coordinates (mm), and how far below its layer it is.
struct IroningPoint
{
    double x;
    double y;
    double z;
    double depth;
    double layer_z;
};

void collect(const ExtrusionEntity &entity, const Layer &layer, std::vector<IroningPoint> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect(*child, layer, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity); path != nullptr && path->role() == erIroning) {
        for (const Point3 &point : path->polyline.points) {
            const double depth = path->z_contoured ? -unscale<double>(point.z()) : 0.;
            out.push_back({ unscale<double>(point.x()), unscale<double>(point.y()), layer.print_z - depth, depth, layer.print_z });
        }
    }
}

struct Ironing
{
    std::vector<IroningPoint> points;
    size_t                    layers_with_ironing = 0;
    size_t                    layers_ironing_lower_steps = 0;
    // Lower corner of the object's footprint: the mesh's origin in the object's coordinates.
    Vec2d                     origin;
};

Ironing ironing_of(const Print &print)
{
    Ironing            out;
    const PrintObject &object = *print.objects().front();
    const BoundingBox  bbox   = get_extents(object.layers().front()->lslices);
    out.origin = unscaled<double>(bbox.min);
    for (const Layer *layer : object.layers()) {
        const size_t before = out.points.size();
        for (const LayerRegion *layerm : layer->regions())
            collect(layerm->fills, *layer, out.points);
        if (out.points.size() > before)
            ++out.layers_with_ironing;
        if (! layer->nonplanar_ironing_sources.empty())
            ++out.layers_ironing_lower_steps;
    }
    return out;
}

// The number of separate ironing paths on the layers that iron the steps of lower layers.
size_t nonplanar_ironing_paths(const ExtrusionEntity &entity)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        size_t count = 0;
        for (const ExtrusionEntity *child : collection->entities)
            count += nonplanar_ironing_paths(*child);
        return count;
    }
    return entity.role() == erIroning ? 1 : 0;
}

size_t nonplanar_ironing_paths(const Print &print)
{
    size_t count = 0;
    for (const Layer *layer : print.objects().front()->layers())
        if (! layer->nonplanar_ironing_sources.empty())
            for (const LayerRegion *layerm : layer->regions())
                count += nonplanar_ironing_paths(layerm->fills);
    return count;
}

// A point of a nonplanar top surface, in the object's coordinates (mm), and how thick the skin is extruded there:
// its height above the layer under the step it lies on.
struct ShellPoint
{
    double x;
    double y;
    double z;
    double thickness;
};

void collect_shell(const ExtrusionEntity &entity, const Layer &layer, std::vector<ShellPoint> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_shell(*child, layer, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity); path != nullptr && path->nonplanar_shell && path->z_contoured) {
        // The first point of a path is where the one before it ended: nothing is extruded on the way to it.
        for (size_t i = 1; i < path->polyline.points.size(); ++i) {
            const Point3 &point = path->polyline.points[i];
            const double  dz    = unscale<double>(point.z());
            out.push_back({ unscale<double>(point.x()), unscale<double>(point.y()), layer.print_z + dz, path->height + dz + path->z_contour_shift });
        }
    }
}

std::vector<ShellPoint> shell_of(const Print &print)
{
    std::vector<ShellPoint> out;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layerm : layer->regions())
            collect_shell(layerm->fills, *layer, out);
    return out;
}

// The number of times the nozzle comes down to print lines of a nonplanar top surface: its runs of lines, each of
// which may be cut in pieces that follow each other without a lift.
size_t shell_line_runs(const ExtrusionEntity &entity)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        size_t count = 0;
        for (const ExtrusionEntity *child : collection->entities)
            count += shell_line_runs(*child);
        return count;
    }
    const auto *path = dynamic_cast<const ExtrusionPath*>(&entity);
    return path != nullptr && path->nonplanar_shell && path->role() == erTopSolidInfill && ! path->nonplanar_continues ? 1 : 0;
}

// Slices with the test printhead and a nonplanar top surface.
void slice_shell(Print &print, const TriangleMesh &mesh, bool shell, const char *ironing, bool z_contouring = false)
{
    init_and_process_print({ mesh }, print, {
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "ironing_type", ironing },
        { "zaa_enabled", z_contouring },
        { "nonplanar_top_surface", shell }, { "nonplanar_ironing", false }, { "top_shell_layers", 3 },
        { "nozzle_tip_diameter", 1. }, { "nozzle_cone_diameter", 11. }, { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 }, { "printhead_body_height", 5.5 }, { "skirt_loops", 0 },
    });
}

// The directions, in degrees from X, of the long lines of top surface in an extrusion: those of a nonplanar top
// surface, or those of the ordinary top surfaces.
void collect_top_directions(const ExtrusionEntity &entity, bool of_shell, std::vector<double> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_top_directions(*child, of_shell, out);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath*>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            collect_top_directions(path, of_shell, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&entity);
               path != nullptr && path->role() == erTopSolidInfill && path->nonplanar_shell == of_shell) {
        for (size_t i = 1; i < path->polyline.points.size(); ++i) {
            const double dx = unscale<double>(path->polyline.points[i].x() - path->polyline.points[i - 1].x());
            const double dy = unscale<double>(path->polyline.points[i].y() - path->polyline.points[i - 1].y());
            if (std::hypot(dx, dy) > 1.5)
                out.emplace_back(std::fmod(std::atan2(dy, dx) * 180. / M_PI + 180., 180.));
        }
    }
}

// Whether slicing or ironing the object left a warning that part of it is too steep.
bool warns_too_steep(const Print &print)
{
    const PrintObject &object = *print.objects().front();
    for (const PrintObjectStep step : { posSlice, posIroning })
        for (const auto &warning : object.step_state_with_warnings(step).warnings)
            if (warning.message.find("steeper than the printhead can follow") != std::string::npos)
                return true;
    return false;
}

double deepest(const Ironing &ironing)
{
    double depth = 0.;
    for (const IroningPoint &point : ironing.points)
        depth = std::max(depth, point.depth);
    return depth;
}

} // namespace

TEST_CASE("A gentle slope is ironed in one pass along its surface", "[NonplanarIroning]")
{
    // 2 mm to 6 mm over 40 mm: 5.7 degrees, 4 mm tall. The printhead follows 21.8 degrees up to 5.5 mm.
    const double low = 2., high = 6.;
    Print print;
    slice(print, wedge(low, high), true);
    const Ironing ironing = ironing_of(print);

    // The slope shows a step on each of its 20 layers, and its last layer irons them all.
    CHECK(ironing.layers_ironing_lower_steps == 1);
    CHECK(ironing.layers_with_ironing <= 2);
    CHECK(deepest(ironing) > 0.8 * (high - low));
    CHECK(deepest(ironing) <= printhead().max_surface_height());

    // The pass is on the model's surface.
    REQUIRE_FALSE(ironing.points.empty());
    // On the slope the centre of the nozzle tip is on the model's surface.
    REQUIRE_FALSE(ironing.points.empty());
    double worst = 0.;
    for (const IroningPoint &point : ironing.points)
        worst = std::max(worst, std::abs(point.z - (low + (high - low) * (point.x - ironing.origin.x()) / LENGTH)));
    CHECK(worst < 0.005);
}

TEST_CASE("At the foot of a slope the flat nozzle tip is lifted clear of it", "[NonplanarIroning]")
{
    // A 2 mm plate in front of the gentle wedge: the slope begins in a hollow, 20 mm from the plate's far end.
    const double low = 2., high = 6., plate = 20.;
    const double slope = (high - low) / LENGTH;
    TriangleMesh mesh  = wedge(low, high);
    TriangleMesh flat  = make_cube(plate + 0.1, WIDTH, low);
    flat.translate(float(-plate), 0.f, 0.f);
    mesh.merge(flat);
    Print print;
    slice(print, mesh, true);
    const Ironing ironing = ironing_of(print);
    REQUIRE(ironing.layers_ironing_lower_steps >= 1);

    // The tip's radius is 0.5 mm. On the plate, within that of the slope, the surface under the tip's edge is higher
    // than under its centre, and the tip rises with it. It never presses deeper into the surface than it does on
    // the slope itself, and away from the hollow it is on the surface.
    const double pressed_in_on_slope = 0.5 * slope;
    double       highest_lift = 0., deepest_press = 0., worst_away = 0.;
    for (const IroningPoint &point : ironing.points) {
        const double along   = point.x - ironing.origin.x() - plate;
        const double surface = low + std::max(0., along) * slope;
        if (along > -0.4 && along < 0.)
            highest_lift = std::max(highest_lift, point.z - surface);
        if (along < LENGTH - 1.)
            deepest_press = std::max(deepest_press, low + std::max(0., along + 0.5) * slope - point.z);
        if (along < -1. || (along > 1. && along < LENGTH - 1.))
            worst_away = std::max(worst_away, std::abs(point.z - surface));
    }
    CHECK(highest_lift > 0.4 * pressed_in_on_slope);
    CHECK(deepest_press < pressed_in_on_slope + 0.005);
    CHECK(worst_away < 0.005);
}

TEST_CASE("Across a ridge the pass starts at the ridge and steps downhill", "[NonplanarIroning]")
{
    // Two gentle wedges back to back: the top rises from 2 mm to 6 mm and falls again, with the ridge along Y. The
    // ironing lines run along Y too, so each line is level and the pass steps across the slopes.
    const double low = 2., high = 6.;
    TriangleMesh mesh = wedge(low, high);
    TriangleMesh down = wedge(high, low);
    down.translate(float(LENGTH - 0.1), 0.f, 0.f);
    mesh.merge(down);
    Print print;
    init_and_process_print({ mesh }, print, {
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "ironing_type", "top" },
        { "nonplanar_ironing", true }, { "ironing_angle_fixed", true }, { "ironing_angle", 90 },
        { "nozzle_tip_diameter", 1. }, { "nozzle_cone_diameter", 11. }, { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 }, { "printhead_body_height", 5.5 }, { "skirt_loops", 0 },
    });
    const Ironing ironing = ironing_of(print);
    REQUIRE(ironing.layers_ironing_lower_steps >= 1);
    REQUIRE(deepest(ironing) > 0.8 * (high - low));

    // From one line to the next the pass does not climb a slope: it rises in a jump, back to the ridge. What it
    // climbs in small steps stays far below the 4 mm of a slope.
    double climbed = 0.;
    for (size_t i = 1; i < ironing.points.size(); ++i) {
        const double rise = ironing.points[i].z - ironing.points[i - 1].z;
        if (rise > 0. && rise < 0.1)
            climbed += rise;
    }
    CHECK(climbed < 0.5);

    // No part of the pass starts on the ridge itself, which was printed last: the part that crosses it starts less
    // than a layer below it.
    bool starts_on_ridge = false;
    for (size_t i = 0; i < ironing.points.size(); ++i) {
        const bool start_of_part = i == 0 || std::abs(ironing.points[i].z - ironing.points[i - 1].z) > 0.1;
        if (start_of_part && ironing.points[i].depth > 0. && ironing.points[i].z > high - 0.05)
            starts_on_ridge = true;
    }
    CHECK_FALSE(starts_on_ridge);
}

TEST_CASE("Short lines at the rim of a round slope are ironed without leaving the surface", "[NonplanarIroning]")
{
    // A cone 4 mm tall on a 2 mm disc, 20 mm in radius: 11.3 degrees all round. Towards the rim the ironing lines get
    // short quickly and the fill leaves them unlinked. Each would be a path of its own, with its own lift and plunge.
    TriangleMesh mesh = make_cylinder(20., 2.);
    TriangleMesh cone = make_cone(20., 4.);
    cone.translate(0.f, 0.f, 2.f);
    mesh.merge(cone);
    Print print;
    slice(print, mesh, true);
    const Ironing ironing = ironing_of(print);
    REQUIRE(ironing.layers_ironing_lower_steps >= 1);
    REQUIRE(deepest(ironing) > 3.);
    // Two parts, each from the top to a rim, and a path or two at the tip of the cone.
    CHECK(nonplanar_ironing_paths(print) <= 6);
}

TEST_CASE("Without the option, or without a printhead outline, every layer irons its own step", "[NonplanarIroning]")
{
    const bool option = GENERATE(false, true);
    CAPTURE(option);
    Print print;
    slice(print, wedge(2., 6.), false, option);
    const Ironing ironing = ironing_of(print);
    CHECK(ironing.layers_ironing_lower_steps == 0);
    CHECK(ironing.layers_with_ironing >= 20);
    CHECK(deepest(ironing) < EPSILON);
}

TEST_CASE("A slope steeper than the printhead can follow is ironed layer by layer", "[NonplanarIroning]")
{
    // The sock edge is 5.5 mm from the nozzle and 2 mm above its tip: 20 degrees. With the 0.2 mm clearance margin
    // kept by default, 18.1 degrees.
    const double degrees = GENERATE(17.7, 18.5, 26.6);
    CAPTURE(degrees);
    Print print;
    slice(print, wedge(2., 2. + LENGTH * std::tan(degrees * std::acos(-1.) / 180.)), true);
    const Ironing ironing = ironing_of(print);
    if (degrees < 18.1) {
        CHECK(ironing.layers_ironing_lower_steps >= 1);
        CHECK(deepest(ironing) > 4.);
    } else {
        CHECK(ironing.layers_ironing_lower_steps == 0);
        CHECK(deepest(ironing) < EPSILON);
    }
    // The user is told when a slope is left out.
    CHECK(warns_too_steep(print) == (degrees > 18.1));
}

TEST_CASE("A slope too steep for a nonplanar top surface is reported", "[NonplanarIroning]")
{
    // 9 mm and 20 mm of rise over 40 mm: 12.7 and 26.6 degrees, either side of the 18.1 the test printhead follows.
    const double rise = GENERATE(9., 20.);
    CAPTURE(rise);
    Print print;
    slice_shell(print, wedge(2., 2. + rise), true, "no ironing");
    CHECK(shell_of(print).empty() == (rise > 15.));
    CHECK(warns_too_steep(print) == (rise > 15.));
}

TEST_CASE("The narrow top of a thin wall is not reported as a slope", "[NonplanarIroning]")
{
    // A 5.7 degree slope beside a wall 0.45 mm thick and 3 mm taller: the wall's top is narrower than any step the
    // printhead follows, but it is level.
    TriangleMesh mesh = wedge(2., 6.);
    TriangleMesh wall = make_cube(LENGTH, 0.45, 9.);
    wall.translate(0.f, float(WIDTH + 12.), 0.f);
    mesh.merge(wall);
    Print print;
    slice_shell(print, mesh, true, "no ironing");
    CHECK_FALSE(shell_of(print).empty());
    CHECK_FALSE(warns_too_steep(print));
}

TEST_CASE("A slope taller than the printhead's clearance is ironed in several passes", "[NonplanarIroning]")
{
    // 2 mm to 14 mm over 40 mm: 16.7 degrees, 12 mm tall, more than twice the 5.5 mm the printhead clears.
    Print print;
    slice(print, wedge(2., 14.), true);
    const Ironing ironing = ironing_of(print);
    CHECK(ironing.layers_ironing_lower_steps >= 3);
    CHECK(ironing.layers_ironing_lower_steps <= 4);
    CHECK(deepest(ironing) > 4.);
    CHECK(deepest(ironing) <= printhead().max_surface_height());
}

TEST_CASE("Beside a wall the nonplanar pass keeps the printhead clear of it", "[NonplanarIroning]")
{
    // The gentle wedge, with a 10 mm tall wall along its far side (Y from 19.9 to 30).
    const double     wall_y = WIDTH - 0.1;
    TriangleMesh     mesh   = wedge(2., 6.);
    TriangleMesh     wall   = make_cube(LENGTH, 10., 10.);
    wall.translate(0.f, float(wall_y), 0.f);
    mesh.merge(wall);
    Print print;
    slice(print, mesh, true);
    const Ironing ironing = ironing_of(print);
    const PrintheadProfile head = printhead();

    // Away from the wall the slope still gets its pass.
    CHECK(ironing.layers_ironing_lower_steps >= 1);
    CHECK(deepest(ironing) > 2.);
    size_t touching = 0;
    for (const IroningPoint &point : ironing.points) {
        // While this layer irons, the wall stands as high as the layer. One layer of it is let through, as for the
        // slope's own steps.
        const double wall_above_tip = std::min(point.layer_z, 10.) - point.z - LAYER_HEIGHT;
        const double distance       = ironing.origin.y() + wall_y - point.y;
        if (distance < WIDTH && wall_above_tip > 0. && distance + 0.01 < head.keep_out_radius(wall_above_tip) - head.tip_radius())
            ++touching;
    }
    CHECK(touching == 0);
}

TEST_CASE("The nozzle travels above the print, not along a nonplanar pass", "[NonplanarIroning]")
{
    Print print;
    slice(print, wedge(2., 6.), true);

    // The highest extrusion so far is the layer being printed. A travel in XY far below it would cut through the print.
    double       layer_z = 0.;
    double       lowest_travel = 0.;
    size_t       deep_extrusions = 0;
    GCodeReader  parser;
    parser.parse_buffer(Test::gcode(print), [&](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
        if (line.extruding(reader) && line.dist_XY(reader) > 0.f) {
            layer_z = std::max(layer_z, double(line.new_Z(reader)));
            if (line.new_Z(reader) < layer_z - 2 * LAYER_HEIGHT)
                ++deep_extrusions;
        } else if (line.travel() && line.dist_XY(reader) > 0.f)
            lowest_travel = std::min(lowest_travel, double(std::min(line.new_Z(reader), reader.z())) - layer_z);
    });
    CHECK(deep_extrusions > 100);
    CHECK(lowest_travel > -2 * LAYER_HEIGHT);
}

TEST_CASE("Changing the printhead outline plans the ironing again", "[NonplanarIroning][Regression]")
{
    // 19.4 degrees: within reach of the sock edge alone (20 degrees), out of reach once 0.5 mm of margin is kept (15.3).
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "ironing_type", "top" }, { "nonplanar_ironing", true },
        { "nozzle_tip_diameter", 1. }, { "nozzle_cone_diameter", 11. }, { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 }, { "printhead_body_height", 5.5 }, { "printhead_clearance_margin", 0. }, { "skirt_loops", 0 } });
    Print print;
    Model model;
    init_print({ wedge(2., 2. + LENGTH * std::tan(19.4 * std::acos(-1.) / 180.)) }, print, model, config);
    print.process();
    REQUIRE(ironing_of(print).layers_ironing_lower_steps >= 1);

    config.set_deserialize_strict({ { "printhead_clearance_margin", 0.5 } });
    print.apply(model, config);
    print.process();
    const Ironing after = ironing_of(print);
    CHECK(after.layers_ironing_lower_steps == 0);
    CHECK(deepest(after) < EPSILON);
}

TEST_CASE("An object left alone on the plate is planned again", "[NonplanarIroning][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "ironing_type", "top" }, { "nonplanar_ironing", true },
        { "nonplanar_top_surface", true }, { "nozzle_tip_diameter", 1. }, { "nozzle_cone_diameter", 11. }, { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 }, { "printhead_body_height", 5.5 }, { "skirt_loops", 0 } });
    const auto planned = [](const Print &print) {
        size_t layers = 0;
        for (const PrintObject *object : print.objects())
            for (const Layer *layer : object->layers())
                if (! layer->nonplanar_shell_sources.empty() || ! layer->nonplanar_ironing_sources.empty())
                    ++layers;
        return layers;
    };

    // With a second object on the plate nothing nonplanar is planned: the clearance check does not know about it.
    Print print;
    Model model;
    init_print({ wedge(2., 6.), make_cube(10., 10., 5.) }, print, model, config);
    print.process();
    REQUIRE(print.objects().size() == 2);
    REQUIRE(planned(print) == 0);

    // Taking the other object away changes nothing in the wedge itself, and still it is planned again.
    model.delete_object(size_t(1));
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);
    CHECK(planned(print) >= 1);
    CHECK_FALSE(shell_of(print).empty());
}

TEST_CASE("A painted seam places the start of the ironing pass and of the wall of the top surface", "[NonplanarIroning]")
{
    // A gentle wedge whose sloping top is painted as a seam, or not. Its middle is the middle of the slope. The
    // ironing lines run along Y, across the slope.
    const bool   painted = GENERATE(false, true);
    const double low = 2., high = 6.;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "ironing_type", "top" }, { "nonplanar_ironing", true },
        { "nonplanar_top_surface", true }, { "ironing_angle_fixed", true }, { "ironing_angle", 90 },
        { "nozzle_tip_diameter", 1. }, { "nozzle_cone_diameter", 11. }, { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 }, { "printhead_body_height", 5.5 }, { "skirt_loops", 0 } });
    const TriangleMesh mesh = wedge(low, high);
    Print print;
    Model model;
    init_print({ mesh }, print, model, config);
    if (painted) {
        ModelVolume     &volume = *model.objects.front()->volumes.front();
        TriangleSelector selector(volume.mesh());
        size_t           faces = 0;
        for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i) {
            // The top is the only face that lies above the base and is not vertical.
            const auto  &face = volume.mesh().its.indices[i];
            const Vec3f &a = volume.mesh().its.vertices[face[0]], &b = volume.mesh().its.vertices[face[1]], &c = volume.mesh().its.vertices[face[2]];
            if ((b - a).cross(c - a).z() > 0.f) {
                selector.set_facet(int(i), EnforcerBlockerType::ENFORCER);
                ++faces;
            }
        }
        REQUIRE(faces == 2);
        volume.seam_facets.set(selector);
        print.apply(model, config);
    }
    print.process();
    const Ironing                 ironing = ironing_of(print);
    const std::vector<ShellPoint> shell   = shell_of(print);
    REQUIRE(ironing.layers_ironing_lower_steps == 1);
    REQUIRE_FALSE(shell.empty());

    // Where the pass over the slope starts: its first point below its layer.
    double pass_start = -1.;
    for (const IroningPoint &point : ironing.points)
        if (point.depth > LAYER_HEIGHT) {
            pass_start = point.x - ironing.origin.x();
            break;
        }
    REQUIRE(pass_start >= 0.);
    // The wall is the first path of the skin.
    const double wall_start = shell.front().x - ironing.origin.x();
    if (painted) {
        // Both start in the middle of the slope, 20 mm along it.
        CHECK(std::abs(pass_start - 0.5 * LENGTH) < 3.);
        CHECK(std::abs(wall_start - 0.5 * LENGTH) < 3.);
    } else {
        // The pass starts at the top of the slope and steps down, and the wall starts at a corner.
        CHECK(pass_start > LENGTH - 6.);
        CHECK(std::abs(wall_start - 0.5 * LENGTH) > 10.);
    }
}

TEST_CASE("A nonplanar top surface is printed along the slope in place of its steps", "[NonplanarIroning]")
{
    // 2 mm to 6 mm over 40 mm: 5.7 degrees.
    const double low = 2., high = 6.;
    Print print;
    slice_shell(print, wedge(low, high), true, "no ironing");
    const PrintObject &object = *print.objects().front();
    const Vec2d        origin = unscaled<double>(get_extents(object.layers().front()->lslices).min);

    // One layer prints the skin, and the steps it covers are gone from their layers.
    size_t homes = 0, sources = 0;
    double left_in_layers = 0.;
    for (const Layer *layer : object.layers()) {
        if (! layer->nonplanar_shell_sources.empty())
            ++homes;
        for (const Layer::NonplanarIroningSource &source : layer->nonplanar_shell_sources) {
            ++sources;
            for (const ExPolygon &piece : intersection_ex(ExPolygons{ source.area }, source.layer->lslices))
                left_in_layers += piece.area() * SCALING_FACTOR * SCALING_FACTOR;
        }
    }
    CHECK(homes == 1);
    CHECK(sources >= 15);
    CHECK(left_in_layers < 0.01);

    // The skin is on the model's surface, from near the foot of the slope to near its top.
    const std::vector<ShellPoint> shell = shell_of(print);
    REQUIRE_FALSE(shell.empty());
    double worst = 0., thinnest = 1e9, thickest = 0., lowest = 1e9, highest = 0.;
    for (const ShellPoint &point : shell) {
        worst    = std::max(worst, std::abs(point.z - (low + (high - low) * (point.x - origin.x()) / LENGTH)));
        thinnest = std::min(thinnest, point.thickness);
        thickest = std::max(thickest, point.thickness);
        lowest   = std::min(lowest, point.z);
        highest  = std::max(highest, point.z);
    }
    CHECK(worst < 0.005);
    CHECK(lowest < low + 0.5);
    CHECK(highest > high - 0.5);
    // It is extruded as thick as the gap to the layer under it: half a layer to a layer and a half.
    CHECK(thinnest > 0.4 * LAYER_HEIGHT);
    CHECK(thickest < 1.6 * LAYER_HEIGHT);
}

TEST_CASE("A nonplanar top surface is as thick with Z contouring as without it", "[NonplanarIroning]")
{
    // Z contouring slices each layer near its bottom, which would leave the skin a quarter of a layer thinner.
    const double low = 2., high = 6.;
    Print        print;
    slice_shell(print, wedge(low, high), true, "no ironing", true);
    const std::vector<ShellPoint> shell = shell_of(print);
    REQUIRE_FALSE(shell.empty());
    // Away from the foot of the slope, whose lowest step has no step below it to hand its lower edge to.
    double thinnest = 1e9, thickest = 0., sum = 0.;
    size_t count = 0;
    for (const ShellPoint &point : shell)
        if (point.z > low + 3 * LAYER_HEIGHT) {
            thinnest = std::min(thinnest, point.thickness);
            thickest = std::max(thickest, point.thickness);
            sum += point.thickness;
            ++count;
        }
    REQUIRE(count > 100);
    CHECK(thinnest > 0.4 * LAYER_HEIGHT);
    CHECK(thickest < 1.6 * LAYER_HEIGHT);
    CHECK_THAT(sum / double(count), Catch::Matchers::WithinAbs(LAYER_HEIGHT, 0.15 * LAYER_HEIGHT));
}

TEST_CASE("A nonplanar top surface goes on over the top of the slope", "[NonplanarIroning]")
{
    // The last layer's own step is part of the skin. Left in its layer, it is a flat strip across the top.
    const double low = 2., high = 6.;
    Print        print;
    slice_shell(print, wedge(low, high), true, "no ironing");
    double top = 0.;
    for (const ShellPoint &point : shell_of(print))
        top = std::max(top, point.z);
    CHECK_THAT(top, Catch::Matchers::WithinAbs(high, 0.25 * LAYER_HEIGHT));
    // Nothing but the skin is a top surface on the layer that prints it.
    std::vector<double> flat;
    for (const Layer *layer : print.objects().front()->layers())
        if (! layer->nonplanar_shell_sources.empty())
            for (const LayerRegion *layerm : layer->regions())
                collect_top_directions(layerm->fills, false, flat);
    CHECK(flat.empty());
}

TEST_CASE("A nonplanar top surface is one skin over a crest", "[NonplanarIroning]")
{
    const bool   z_contouring = GENERATE(false, true);
    const double low = 2., high = 6.;
    Print        print;
    slice_shell(print, ridge(low, high), true, "no ironing", z_contouring);
    // One layer prints both slopes and the crest between them.
    size_t homes = 0;
    for (const Layer *layer : print.objects().front()->layers())
        if (! layer->nonplanar_shell_sources.empty())
            ++homes;
    CHECK(homes == 1);
    double top = 0., thinnest = 1e9, thickest = 0.;
    size_t count = 0;
    for (const ShellPoint &point : shell_of(print)) {
        top = std::max(top, point.z);
        if (point.z > high - LAYER_HEIGHT) {
            thinnest = std::min(thinnest, point.thickness);
            thickest = std::max(thickest, point.thickness);
            ++count;
        }
    }
    CHECK_THAT(top, Catch::Matchers::WithinAbs(high, 0.25 * LAYER_HEIGHT));
    REQUIRE(count > 20);
    CHECK(thinnest > 0.4 * LAYER_HEIGHT);
    CHECK(thickest < 1.6 * LAYER_HEIGHT);
}

TEST_CASE("The lines of a round nonplanar top surface are printed in one run", "[NonplanarIroning]")
{
    // A cone 4 mm tall on a 2 mm disc, 20 mm in radius. The first and the last line of the skin are short and end
    // far from the ends of the lines next to them, so the fill leaves them unlinked. Each would be a run of its own,
    // with its own plunge and lift at the rim.
    TriangleMesh mesh = make_cylinder(20., 2.);
    TriangleMesh cone = make_cone(20., 4.);
    cone.translate(0.f, 0.f, 2.f);
    mesh.merge(cone);
    Print print;
    slice_shell(print, mesh, true, "no ironing");
    size_t homes = 0, runs = 0;
    for (const Layer *layer : print.objects().front()->layers())
        if (! layer->nonplanar_shell_sources.empty()) {
            ++homes;
            for (const LayerRegion *layerm : layer->regions())
                runs += shell_line_runs(layerm->fills);
        }
    REQUIRE(homes == 1);
    CHECK(runs == 1);
}

TEST_CASE("The lines of a nonplanar top surface cross the lines they are printed on", "[NonplanarIroning]")
{
    // Solid layers alternate between this direction and the one across it, and the skin takes this one: without
    // more, the skin would lie along the lines of every other layer under it and leave the grooves between them open.
    // A slope so gentle that its steps are much wider than the walls around them.
    const double direction = 30.;
    Print        print;
    init_and_process_print({ wedge(2., 3.) }, print, {
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "ironing_type", "no ironing" },
        { "solid_infill_direction", direction }, { "top_surface_pattern", "monotonicline" },
        { "nonplanar_top_surface", true }, { "nonplanar_ironing", false }, { "top_shell_layers", 3 },
        { "nozzle_tip_diameter", 1. }, { "nozzle_cone_diameter", 11. }, { "nozzle_cone_height", 2. },
        { "printhead_body_distance", 13.5 }, { "printhead_body_height", 5.5 }, { "skirt_loops", 0 },
    });
    std::vector<double> skin, under;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *layerm : layer->regions()) {
            collect_top_directions(layerm->fills, true, skin);
            if (! layer->nonplanar_shell_above.empty())
                collect_top_directions(layerm->fills, false, under);
        }
    REQUIRE(skin.size() > 10);
    REQUIRE(under.size() > 10);
    for (const double angle : skin)
        CHECK_THAT(angle, Catch::Matchers::WithinAbs(direction, 1.));
    // Half way between the two directions of the solid layers, so they cross the layer under them as well.
    for (const double angle : under)
        CHECK_THAT(angle, Catch::Matchers::WithinAbs(direction + 45., 1.));
}

TEST_CASE("Without the option, or without a printhead outline, no nonplanar top surface is made", "[NonplanarIroning]")
{
    const bool option = GENERATE(false, true);
    Print print;
    init_and_process_print({ wedge(2., 6.) }, print, {
        { "layer_height", LAYER_HEIGHT }, { "initial_layer_print_height", LAYER_HEIGHT }, { "nonplanar_top_surface", option },
        { "nozzle_cone_diameter", option ? 0. : 11. }, { "nozzle_cone_height", 2. }, { "skirt_loops", 0 },
    });
    size_t homes = 0;
    for (const Layer *layer : print.objects().front()->layers())
        if (! layer->nonplanar_shell_sources.empty())
            ++homes;
    CHECK(homes == 0);
    CHECK(shell_of(print).empty());
}

TEST_CASE("Ironing follows a nonplanar top surface", "[NonplanarIroning]")
{
    const double low = 2., high = 6.;
    Print print;
    slice_shell(print, wedge(low, high), true, "top");
    const Ironing ironing = ironing_of(print);
    REQUIRE_FALSE(shell_of(print).empty());

    // The skin is ironed by the layer that printed it, on the model's surface. The layers under it are not ironed.
    CHECK(ironing.layers_ironing_lower_steps == 1);
    CHECK(ironing.layers_with_ironing <= 2);
    REQUIRE_FALSE(ironing.points.empty());
    double worst = 0.;
    for (const IroningPoint &point : ironing.points)
        worst = std::max(worst, std::abs(point.z - (low + (high - low) * (point.x - ironing.origin.x()) / LENGTH)));
    CHECK(worst < 0.005);
    CHECK(deepest(ironing) > 0.8 * (high - low));
}

TEST_CASE("A nonplanar top surface is extruded from above the print and with the material of the steps it replaces", "[NonplanarIroning]")
{
    // The filament used, the lowest travel below the highest layer printed so far, and the extrusions far below it.
    struct Result
    {
        double filament = 0.;
        double lowest_travel = 0.;
        size_t deep_extrusions = 0;
        // Filament and length of the deep extrusions that climb and of those that descend.
        double up_filament = 0., up_length = 0., down_filament = 0., down_length = 0.;
    };
    const auto print_wedge = [](bool shell) {
        Print print;
        slice_shell(print, wedge(2., 6.), shell, "no ironing");
        Result      out;
        double      layer_z = 0.;
        GCodeReader parser;
        parser.parse_buffer(Test::gcode(print), [&](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            if (line.extruding(reader) && line.dist_XY(reader) > 0.f) {
                out.filament += line.dist_E(reader);
                layer_z = std::max(layer_z, double(line.new_Z(reader)));
                if (line.new_Z(reader) < layer_z - 2 * LAYER_HEIGHT) {
                    ++out.deep_extrusions;
                    if (line.dist_Z(reader) > 0.01f) {
                        out.up_filament += line.dist_E(reader);
                        out.up_length += line.dist_XY(reader);
                    } else if (line.dist_Z(reader) < -0.01f) {
                        out.down_filament += line.dist_E(reader);
                        out.down_length += line.dist_XY(reader);
                    }
                }
            } else if (line.travel() && line.dist_XY(reader) > 0.f)
                out.lowest_travel = std::min(out.lowest_travel, double(std::min(line.new_Z(reader), reader.z())) - layer_z);
        });
        return out;
    };
    const Result planar = print_wedge(false), shell = print_wedge(true);
    CHECK(planar.deep_extrusions == 0);
    CHECK(shell.deep_extrusions > 100);
    CHECK(shell.lowest_travel > -2 * LAYER_HEIGHT);
    // The model is the same, so the filament nearly is. The layers under the skin get a top surface of their own, with
    // solid layers under it, where there was sparse infill: that takes a little more.
    CHECK(shell.filament > 0.95 * planar.filament);
    CHECK(shell.filament < 1.1 * planar.filament);
    // A line gets the same material whether it climbs the slope or descends it.
    REQUIRE(shell.up_length > 0.);
    REQUIRE(shell.down_length > 0.);
    CHECK_THAT(shell.up_filament / shell.up_length, Catch::Matchers::WithinRel(shell.down_filament / shell.down_length, 0.1));
}
