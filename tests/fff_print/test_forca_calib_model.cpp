// Forca: forca_hollow_to_outline (src/libslic3r/ForcaCalibModel.cpp) -- the Max Volumetric Speed test model made
// thin-walled like Bambu Studio's, so its brim goes on both sides of the wall.
#include <catch2/catch_all.hpp>

#include "libslic3r/ForcaCalibModel.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

// A 40 x 20 x 10 mm block hollowed to a `wall` mm outline, sliced like the Max Volumetric Speed test (vase mode, one
// wall, one bottom layer, brim outer + inner). Built by hand rather than with test_helpers' init_print, which takes
// plain meshes: the hollowing adds a negative volume to the ModelObject before the Print sees it.
struct HollowTest
{
    Model model;
    Print print;

    explicit HollowTest(double wall)
    {
        ModelObject* obj = model.add_object();
        obj->add_volume(make_cube(40, 20, 10));
        obj->add_instance();
        hollowed = forca_hollow_to_outline(*obj, wall, 0.32); // the first layer (initial_layer_print_height below)
        obj->ensure_on_bed();

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "spiral_mode", "1" },
                                        { "wall_loops", "1" },
                                        { "top_shell_layers", "0" },
                                        { "bottom_shell_layers", "1" },
                                        { "sparse_infill_density", "0%" },
                                        { "layer_height", "0.32" },
                                        { "initial_layer_print_height", "0.32" },
                                        { "outer_wall_line_width", "0.7" },
                                        { "wall_generator", "arachne" },
                                        { "detect_thin_wall", "0" },
                                        { "enable_support", "0" },
                                        { "brim_type", "outer_and_inner" },
                                        { "brim_width", "5" },
                                        { "brim_object_gap", "0" } });
        print.auto_assign_extruders(obj);
        print.apply(model, config);
        print.validate();
        print.process();
    }
    bool hollowed = false;
};

} // namespace

TEST_CASE("Hollowing a solid model leaves a thin wall with an inside edge on the first layer", "[ForcaCalibModel]")
{
    const double wall = 0.7; // the test's line width for a 0.4 mm nozzle (1.75 x nozzle)
    HollowTest   t(wall);
    REQUIRE(t.hollowed);
    const ModelObject& obj = *t.model.objects.front();
    REQUIRE(obj.volumes.size() == 2);
    CHECK(obj.volumes.back()->is_negative_volume());

    const PrintObject& po    = *t.print.objects().front();
    const ExPolygons&  first = po.layers().front()->lslices;
    REQUIRE(first.size() == 1);
    REQUIRE(first.front().holes.size() == 1); // the inside edge the inner brim needs
    const BoundingBox outer = first.front().contour.bounding_box();
    const BoundingBox inner = first.front().holes.front().bounding_box();
    using Catch::Matchers::WithinAbs;
    CHECK_THAT(unscale<double>(outer.size().x()), WithinAbs(40., 0.1));
    CHECK_THAT(unscale<double>(inner.size().x()), WithinAbs(40. - 2 * wall, 0.1));
    CHECK_THAT(unscale<double>(inner.size().y()), WithinAbs(20. - 2 * wall, 0.1));
}

TEST_CASE("Above the bottom layer the hollowed test is still a single spiral wall", "[ForcaCalibModel]")
{
    HollowTest t(0.7);
    REQUIRE(t.hollowed);
    const PrintObject& po = *t.print.objects().front();
    REQUIRE(po.layers().size() > 10);
    // 10 mm at 0.32 mm layers; check a layer in the middle: one solid island, one wall loop (vase mode stays one path).
    const Layer& mid = *po.layers()[po.layers().size() / 2];
    REQUIRE(mid.lslices.size() == 1);
    CHECK(mid.lslices.front().holes.empty());
    CHECK(po.layers()[1]->lslices.front().holes.empty()); // only the first layer is hollowed
    size_t loops = 0;
    for (const LayerRegion* region : mid.regions())
        loops += region->perimeters.flatten().entities.size();
    CHECK(loops == 1);
}

TEST_CASE("The hollowed test gets a brim on both sides of its wall", "[ForcaCalibModel]")
{
    HollowTest t(0.7);
    REQUIRE(t.hollowed);
    const PrintObject& po    = *t.print.objects().front();
    const ExPolygon    shape = [&] {
        ExPolygon e = po.layers().front()->lslices.front();
        e.translate(po.instances().front().shift); // brim paths are in print coordinates
        return e;
    }();

    Points brim;
    for (auto& [id, collection] : t.print.get_brimMap())
        collection.collect_points(brim);
    REQUIRE_FALSE(brim.empty());
    size_t inside = 0, outside = 0;
    for (const Point& p : brim) {
        if (shape.holes.front().contains(p))
            ++inside;
        else if (!shape.contour.contains(p))
            ++outside;
    }
    CHECK(inside > 0);  // inner brim, in the hollow
    CHECK(outside > 0); // outer brim, around the wall
}

TEST_CASE("Without hollowing, the solid test model gets no inner brim", "[ForcaCalibModel]")
{
    // What Orca's own test did: a solid first layer outline has no inside edge, so "outer and inner" is outer only.
    Model model;
    ModelObject* obj = model.add_object();
    obj->add_volume(make_cube(40, 20, 10));
    obj->add_instance();
    CHECK(obj->volumes.size() == 1);
    CHECK_FALSE(forca_hollow_to_outline(*obj, 0., 0.32));  // no wall width: nothing done
    CHECK_FALSE(forca_hollow_to_outline(*obj, 0.7, 0.));   // no height: nothing done
    CHECK_FALSE(forca_hollow_to_outline(*obj, 30., 0.32)); // wider than the model: nothing left to hollow
    CHECK(obj->volumes.size() == 1);
}
