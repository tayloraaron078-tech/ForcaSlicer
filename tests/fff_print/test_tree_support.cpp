#include <catch2/catch_all.hpp>

#include <algorithm>
#include <initializer_list>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <cstddef>
#include "libslic3r/libslic3r.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"

#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

// The upper plate overhangs both the lower plate and open air, so branches land on the model and on
// the bed in the same slice.
TriangleMesh two_tier_mesh()
{
    TriangleMesh lower  = make_cube(30, 30, 3);
    TriangleMesh column = make_cube(8, 8, 15);
    TriangleMesh upper  = make_cube(50, 50, 3);
    // Each part overlaps the one below rather than resting on it; a coplanar join slices ambiguously.
    column.translate(11.f, 11.f, 2.f);
    upper.translate(-10.f, -10.f, 16.f);
    TriangleMesh mesh = lower;
    mesh.merge(column);
    mesh.merge(upper);
    return mesh;
}

TriangleMesh scaled(TestMesh id, float scale)
{
    TriangleMesh mesh = Slic3r::Test::mesh(id);
    mesh.scale(scale);
    return mesh;
}

// `extra` is applied last, so a caller can add or override any key.
void slice_with_tree_support(const TriangleMesh &mesh, Slic3r::Print &print, const char *style,
                             int threshold_angle = 30, int build_plate_only = 0, int raft_layers = 0,
                             std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                "tree(auto)" },
        { "support_style",               style },
        { "support_on_build_plate_only", build_plate_only },
        { "support_threshold_angle",     threshold_angle },
        { "raft_layers",                 raft_layers },
        { "layer_height",                0.2 },
    });
    config.set_deserialize_strict(extra);
    Slic3r::Test::init_and_process_print({ mesh }, print, config);
}

Points support_points(const Slic3r::Print &print)
{
    Points points;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        layer->support_fills.collect_points(points);
    return points;
}

size_t support_point_count(const TriangleMesh &mesh, const char *style, int threshold_angle = 30,
                           int build_plate_only = 0)
{
    Slic3r::Print print;
    slice_with_tree_support(mesh, print, style, threshold_angle, build_plate_only);
    return support_points(print).size();
}

// Index of the first differing point, or the common length when they match. An index keeps a
// failure readable; comparing the vectors themselves dumps thousands of points.
size_t first_difference(const Points &a, const Points &b)
{
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i)
        if (a[i] != b[i])
            return i;
    return common;
}

// Slice `mesh` twice and require an identical support point sequence. Point counts and total
// length are order insensitive, so the sequence is what a reordering shows up in.
void sliced_twice_matches(const TriangleMesh &mesh, int build_plate_only, const char *style = "tree_slim",
                          std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    Slic3r::Print first_print, second_print;
    slice_with_tree_support(mesh, first_print, style, 30, build_plate_only, 0, extra);
    slice_with_tree_support(mesh, second_print, style, 30, build_plate_only, 0, extra);
    const Points first  = support_points(first_print);
    const Points second = support_points(second_print);
    REQUIRE(first.size() > 1000); // without support the comparison below passes vacuously
    REQUIRE(second.size() == first.size());
    REQUIRE(first_difference(first, second) == first.size());
}

// Slices two_tier_mesh() with 0.1 mm layers from 6 to 18 mm (the column and the upper plate's overhang) between
// 0.2 mm layers. Requires validate() to pass, which a refused combination of supports and layer heights fails.
void slice_two_tier_with_thin_layers(Slic3r::Print &print, Slic3r::Model &model,
                                     std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",             1 },
        { "support_type",               "tree(auto)" },
        { "support_threshold_angle",    30 },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        // Keeps validate() free of the relative-extrusion warning.
        { "layer_change_gcode",         "G92 E0" },
    });
    config.set_deserialize_strict(extra);
    init_print({ two_tier_mesh() }, print, model, config);
    DynamicPrintConfig range;
    range.set_deserialize_strict({ { "layer_height", 0.1 } });
    model.objects.front()->layer_config_ranges[t_layer_height_range(6., 18.)].assign_config(std::move(range));
    print.apply(model, config);
    const std::string error = print.validate().string;
    CAPTURE(error);
    REQUIRE(error.empty());
    print.process();
}

} // namespace

TEST_CASE("Tree support is generated for an overhang and not for a plain cube", "[TreeSupport]")
{
    REQUIRE(support_point_count(scaled(TestMesh::overhang, 2.f), "tree_slim") > 1000);
    REQUIRE(support_point_count(Slic3r::Test::cube(20), "tree_slim") == 0);
}

TEST_CASE("Restricting tree support to the build plate changes what is generated", "[TreeSupport]")
{
    const TriangleMesh mesh = two_tier_mesh();
    const size_t anywhere    = support_point_count(mesh, "tree_slim", 30, 0);
    const size_t plate_only  = support_point_count(mesh, "tree_slim", 30, 1);
    REQUIRE(anywhere > 1000);
    REQUIRE(plate_only > 1000);
    // The upper plate overhangs the lower one, so some branches would land on the model.
    REQUIRE(plate_only != anywhere);
}

TEST_CASE("Tree support layers rise monotonically within the layer height limits", "[TreeSupport]")
{
    Slic3r::Print print;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), print, "tree_slim");
    const double nozzle = print.config().nozzle_diameter.values.front();

    size_t checked = 0;
    double previous = 0;
    bool   previous_was_adjacent = false;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        if (layer->print_z <= 0 || layer->height <= 0) {
            // Layers with no nodes are left at zero. Skipping one leaves a hole, so the next pair
            // spans more than one layer and its gap says nothing about the layer height limit.
            previous_was_adjacent = false;
            continue;
        }
        if (previous > 0) {
            CAPTURE(previous, layer->print_z);
            REQUIRE(layer->print_z > previous);
            if (previous_was_adjacent)
                REQUIRE(layer->print_z - previous <= nozzle + EPSILON);
        }
        previous = layer->print_z;
        previous_was_adjacent = true;
        ++checked;
    }
    REQUIRE(checked > 10);
}

TEST_CASE("A raft is still generated under tree support", "[TreeSupport]")
{
    // The mesh supports itself, so a layer count alone passes with no raft at all.
    Slic3r::Print rafted, unrafted;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), rafted, "tree_slim", 30, 0, 3);
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), unrafted, "tree_slim", 30, 0, 0);
    const PrintObject *rafted_object   = rafted.objects().front();
    const PrintObject *unrafted_object = unrafted.objects().front();
    REQUIRE(rafted_object->support_layers().size() > unrafted_object->support_layers().size());
    // The raft goes under the object.
    REQUIRE(rafted_object->layers().front()->print_z > unrafted_object->layers().front()->print_z);
}

// drop_nodes() decides the node merges and spawns the next layer's nodes in parallel. Every one of
// those decisions has to be applied in a fixed order, or the same model gives different branches on
// each slice.
TEST_CASE("Tree support toolpaths do not depend on thread scheduling", "[TreeSupport][Regression]")
{
    // Scaled up so that a layer holds enough nodes for the parallel range to be split. At stock
    // size it stays in one chunk and the order never varies.
    SECTION("overhang")            { sliced_twice_matches(scaled(TestMesh::overhang, 2.f), 0); }
    SECTION("bridge with hole")    { sliced_twice_matches(scaled(TestMesh::bridge_with_hole, 3.f), 0); }
    // Dropping every branch that cannot reach the bed leaves the survivors dense enough that the
    // neighbour merge fires in bulk.
    SECTION("on the build plate")  { sliced_twice_matches(scaled(TestMesh::overhang, 4.f), 1); }
    // Branches resting on the model are what put nodes in a part group other than 0, which is the
    // only way to reach the prune in the second pass. tree_hybrid additionally builds polygon
    // nodes, so it is the only style that exercises the overhang merge.
    SECTION("resting on the model") { sliced_twice_matches(two_tier_mesh(), 0); }
    SECTION("hybrid on the model")  { sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid"); }
}

// Prim breaks equal-distance ties by heap address. A 1 mm branch diameter puts neighbours close
// enough to tie, and an explicit line width pins max_move_dist, so the moved tie winner reaches
// the support toolpaths.
TEST_CASE("Tree support toolpaths do not depend on the MST tie order", "[TreeSupport][Regression]")
{
    sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid", {
        { "tree_support_branch_diameter", 1.0 },
        { "tree_support_branch_distance", 5.0 },
        { "tree_support_branch_angle",    40 },
        { "support_line_width",           0.4 },
    });
}

// Painted, smoothed or height-range layers used to make Organic supports refuse to slice. They are planned on the
// object's real layers now, so every support layer is printed at the height of an object layer.
TEST_CASE("Organic support is printed on the layers of a variable layer height", "[TreeSupport]")
{
    const int raft_layers = GENERATE(0, 3);
    CAPTURE(raft_layers);
    Slic3r::Model model;
    Slic3r::Print print;
    slice_two_tier_with_thin_layers(print, model, { { "support_style", "organic" }, { "raft_layers", raft_layers } });

    const PrintObject &object = *print.objects().front();
    std::vector<double> object_z;
    for (const Layer *layer : object.layers())
        object_z.push_back(layer->print_z);
    // 0-6 mm at 0.2 (30 layers), 6-18 mm at 0.1 (120), 18-19 mm at 0.2 (5), on top of any raft.
    REQUIRE(object_z.size() == 155);

    size_t checked        = 0;
    size_t on_thin_layers = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        // The raft has layers of its own below the object.
        if (layer->support_fills.empty() || layer->print_z < object_z.front() - EPSILON)
            continue;
        const auto it = std::lower_bound(object_z.begin(), object_z.end(), layer->print_z - EPSILON);
        CAPTURE(layer->print_z);
        REQUIRE(it != object_z.end());
        REQUIRE_THAT(*it, Catch::Matchers::WithinAbs(layer->print_z, EPSILON));
        ++checked;
        // A layer between two 0.2 mm steps can only be one of the 0.1 mm layers.
        if (std::abs(layer->print_z / 0.2 - std::round(layer->print_z / 0.2)) > 0.25)
            ++on_thin_layers;
    }
    REQUIRE(checked > 100);
    // Supports reach up into the 0.1 mm layers below the upper plate.
    REQUIRE(on_thin_layers > 10);
}

TEST_CASE("Organic support gaps and branch moves follow each layer's height", "[TreeSupport]")
{
    using namespace Slic3r::TreeSupport3D;
    TreeSupportMeshGroupSettings settings;
    settings.layer_height            = scaled<coord_t>(0.2);
    settings.support_top_distance    = scaled<coord_t>(0.2);
    settings.support_bottom_distance = scaled<coord_t>(0.2);
    SlicingParameters slicing_params;
    slicing_params.layer_height = 0.2;

    SECTION("fixed layers keep the uniform values") {
        const TreeSupportSettings config(settings, slicing_params);
        REQUIRE_FALSE(config.has_variable_layer_height());
        REQUIRE(config.z_distance_top_layers == 1); // 0.2 mm gap at 0.2 mm layers
        REQUIRE(config.z_distance_top_layers_at(5) == config.z_distance_top_layers);
        REQUIRE(config.maximum_move_distance_at(5) == config.maximum_move_distance);
        REQUIRE(config.support_layer_below_overhang(10) == 8);
    }
    SECTION("variable layers measure them per layer") {
        // Layers 0-2 are 0.2 mm, 3-6 are 0.1 mm, 7 is 0.2 mm.
        settings.object_layer_print_z = { 0.2, 0.4, 0.6, 0.7, 0.8, 0.9, 1.0, 1.2 };
        const TreeSupportSettings config(settings, slicing_params);
        REQUIRE(config.has_variable_layer_height());
        REQUIRE_THAT(layer_z(slicing_params, config, 4), Catch::Matchers::WithinAbs(0.8, EPSILON));
        REQUIRE(layer_idx_ceil(slicing_params, config, 0.75) == 4);
        REQUIRE(layer_idx_floor(slicing_params, config, 0.75) == 3);
        // The 0.2 mm top gap is one 0.2 mm layer above layer 0 but two 0.1 mm layers above layer 2.
        REQUIRE(config.z_distance_top_layers_at(0) == 1);
        REQUIRE(config.z_distance_top_layers_at(2) == 2);
        // Likewise below: two 0.1 mm layers under layer 5, one 0.2 mm layer under layer 2.
        REQUIRE(config.z_distance_bottom_layers_at(5) == 2);
        REQUIRE(config.z_distance_bottom_layers_at(2) == 1);
        // The overhang at layer 7 (bottom at 1.0 mm) is supported from layer 4 (top at 0.8 mm): a 0.2 mm gap.
        REQUIRE(config.support_layer_below_overhang(7) == 4);
        // A branch moves half as far over a 0.1 mm layer as over a 0.2 mm one.
        REQUIRE(config.maximum_move_distance_at(1) > 0);
        REQUIRE(std::abs(2 * config.maximum_move_distance_at(4) - config.maximum_move_distance_at(1)) <= 2);
        REQUIRE(std::abs(2 * config.maximum_move_distance_slow_at(4) - config.maximum_move_distance_slow_at(1)) <= 2);
    }
}

// The classic Tree styles plan their support layers in mm, either on the object's layers or, with an independent
// support layer height, on layers of their own. Either way the support stays out of the object and its top gap.
TEST_CASE("Classic tree support stays clear of the object on a variable layer height", "[TreeSupport]")
{
    const char *style       = GENERATE("tree_slim", "tree_strong", "tree_hybrid");
    const int   independent = GENERATE(0, 1);
    const double top_z      = 0.2;
    CAPTURE(style, independent);
    Slic3r::Model model;
    Slic3r::Print print;
    slice_two_tier_with_thin_layers(print, model, {
        { "support_style",                    style },
        { "independent_support_layer_height", independent },
        { "support_top_z_distance",           top_z },
    });

    const PrintObject &object = *print.objects().front();
    size_t checked        = 0;
    size_t on_thin_layers = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        if (layer->support_fills.empty())
            continue;
        CAPTURE(layer->print_z);
        ++checked;
        if (std::abs(layer->print_z / 0.2 - std::round(layer->print_z / 0.2)) > 0.25)
            ++on_thin_layers;
        Points points;
        layer->support_fills.collect_points(points);
        for (const Layer *object_layer : object.layers()) {
            // Object layers this support layer shares a height with, or that lie in the gap above it.
            if (object_layer->print_z < layer->print_z - layer->height + EPSILON ||
                object_layer->bottom_z() > layer->print_z + top_z - EPSILON)
                continue;
            CAPTURE(object_layer->print_z);
            size_t inside = 0;
            for (const Point &point : points)
                for (const ExPolygon &island : object_layer->lslices)
                    if (island.contains(point)) {
                        ++inside;
                        break;
                    }
            REQUIRE(inside == 0);
        }
        if (independent == 0) {
            const auto it = std::find_if(object.layers().begin(), object.layers().end(), [layer](const Layer *l) {
                return std::abs(l->print_z - layer->print_z) < EPSILON;
            });
            REQUIRE(it != object.layers().end());
        }
    }
    REQUIRE(checked > 40);
    // Supports reach up into the 0.1 mm layers below the upper plate.
    REQUIRE(on_thin_layers > 10);
}

// Branches landing on the lower plate get the bottom gap and then every bottom interface layer asked for. The contact
// search stops at the gap and interface height below each layer, so it must still reach the plate from the highest
// interface layer, and the layers above must not get an interface.
TEST_CASE("Tree support resting on the model gets every bottom interface layer above the gap", "[TreeSupport]")
{
    const int    interface_layers = 2;
    const double lower_top = 3., upper_bottom = 16.; // two_tier_mesh()
    Slic3r::Print print;
    slice_with_tree_support(two_tier_mesh(), print, "tree_slim", 30, 0, 0, {
        { "support_interface_top_layers",    interface_layers },
        { "support_interface_bottom_layers", interface_layers },
        { "support_bottom_z_distance",       0.2 },
    });

    int above_plate = 0, between_plates = 0;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        bool has_interface = false;
        for (const ExtrusionEntity *entity : layer->support_fills.flatten().entities)
            has_interface |= entity->role() == erSupportMaterialInterface;
        if (! has_interface)
            continue;
        if (layer->print_z > lower_top && layer->print_z < lower_top + 1.5)
            ++above_plate;
        else if (layer->print_z < upper_bottom - 1.5)
            ++between_plates;
    }
    CHECK(above_plate == interface_layers);
    CHECK(between_plates == 0);
}

// Each node under an overhang adds the whole overhang to its roof list, so the top interface is a solid sheet rather
// than the branch circles alone. Counting interface layers would not notice the sheet missing.
TEST_CASE("Tree support top interface covers the whole overhang", "[TreeSupport]")
{
    const double upper_bottom = 16.; // two_tier_mesh()
    Slic3r::Print print;
    slice_with_tree_support(two_tier_mesh(), print, "tree_slim", 30, 0, 0, {
        // The test print is centred on the origin, and tree support drops nodes off the bed.
        { "printable_area",               "-100x-100,100x-100,100x100,-100x100" },
        { "support_interface_top_layers", 2 },
        { "support_interface_spacing",    0 },
    });
    const PrintObject &object = *print.objects().front();

    // The underside of the upper plate, less the column and a margin for the xy distance kept around it.
    ExPolygons underside, column;
    for (const Layer *layer : object.layers())
        if (layer->print_z < upper_bottom)
            column = layer->lslices;
        else if (underside.empty())
            underside = layer->lslices;
    const ExPolygons overhang = diff_ex(underside, offset_ex(column, scaled<float>(2.)));
    REQUIRE(area(overhang) > 0);

    const SupportLayer *top_interface = nullptr;
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z < upper_bottom)
            for (const ExtrusionEntity *entity : layer->support_fills.flatten().entities)
                if (entity->role() == erSupportMaterialInterface)
                    top_interface = layer;
    REQUIRE(top_interface != nullptr);

    Polygons covered;
    for (const ExtrusionEntity *entity : top_interface->support_fills.flatten().entities)
        if (entity->role() == erSupportMaterialInterface)
            entity->polygons_covered_by_width(covered, scaled<float>(0.05));
    const double uncovered = area(diff_ex(overhang, covered)) / area(overhang);
    INFO("uncovered fraction " << uncovered);
    CHECK(uncovered < 0.02);
}
