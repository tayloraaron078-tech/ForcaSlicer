// Forca: forca_hollow_to_outline (src/libslic3r/ForcaCalibModel.cpp) -- the Max Volumetric Speed test model made
// thin-walled like Bambu Studio's, so its brim goes on both sides of the wall. Also the hole size calibration plate and
// the fit that turns its measured holes into X-Y hole compensation (mm and % of size).
#include <catch2/catch_all.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/libslic3r.h>
#include <cstddef>
#include <libslic3r/Point.hpp>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "libslic3r/ForcaCalibModel.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

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

// ---- Hole size calibration (forca_build_hole_plate / forca_hole_fit) ----

namespace {

// What a printer would measure for each designed hole: measured = k * designed + e.
std::vector<double> printed_holes(double k, double e)
{
    std::vector<double> out;
    for (double d : forca_hole_plate_sizes())
        out.push_back(k * d + e);
    return out;
}

} // namespace

TEST_CASE("The hole fit finds the compensation that makes every hole come out as designed", "[ForcaCalibModel]")
{
    using Catch::Matchers::WithinAbs;
    // Holes print 2 % small plus 0.2 mm small (0.1 mm per edge).
    const double       k = 0.98, e = -0.2;
    const ForcaHoleFit fit = forca_hole_fit(forca_hole_plate_sizes(), printed_holes(k, e));
    REQUIRE(fit.error == ForcaHoleFit::Error::None);
    CHECK(fit.used == forca_hole_plate_sizes().size());
    CHECK_FALSE(fit.clamped);
    CHECK_THAT(fit.worst_error, WithinAbs(0., 1e-9));
    // Printing each hole with the compensation applied (designed * (1 + %) + 2 * mm) gives the designed size back.
    for (double d : forca_hole_plate_sizes())
        CHECK_THAT(k * (d * (1. + fit.percent / 100.) + 2. * fit.mm) + e, WithinAbs(d, 1e-9));
    CHECK_THAT(fit.percent, WithinAbs(100. / 0.98 - 100., 1e-9));
    CHECK_THAT(fit.mm, WithinAbs(0.1 / 0.98, 1e-9));
}

TEST_CASE("The hole fit reports how far off the worst hole still is", "[ForcaCalibModel]")
{
    using Catch::Matchers::WithinAbs;
    std::vector<double> measured = printed_holes(0.99, -0.1);
    measured[1] += 0.03; // caliper noise on the 5 mm hole
    measured[3] -= 0.02;
    const ForcaHoleFit fit = forca_hole_fit(forca_hole_plate_sizes(), measured);
    REQUIRE(fit.error == ForcaHoleFit::Error::None);
    CHECK(fit.worst_error > 0.);
    CHECK(fit.worst_error < 0.04);
    CHECK_THAT(fit.percent, WithinAbs(100. / 0.99 - 100., 0.3));
}

TEST_CASE("The hole fit works from two measured holes and skips blank ones", "[ForcaCalibModel]")
{
    using Catch::Matchers::WithinAbs;
    std::vector<double> measured = printed_holes(0.98, -0.2);
    measured[1] = measured[2] = measured[3] = 0.; // only the 3 and 20 mm holes measured
    const ForcaHoleFit fit = forca_hole_fit(forca_hole_plate_sizes(), measured);
    REQUIRE(fit.error == ForcaHoleFit::Error::None);
    CHECK(fit.used == 2);
    CHECK_THAT(fit.percent, WithinAbs(100. / 0.98 - 100., 1e-9));
}

TEST_CASE("The hole fit needs two sizes and rejects typos", "[ForcaCalibModel]")
{
    const std::vector<double>& sizes = forca_hole_plate_sizes();
    CHECK(forca_hole_fit(sizes, {}).error == ForcaHoleFit::Error::TooFew);
    CHECK(forca_hole_fit(sizes, { 2.9, 0., 0., 0., 0. }).error == ForcaHoleFit::Error::TooFew);
    // 29 for a 3 mm hole (a missed decimal point).
    CHECK(forca_hole_fit(sizes, { 29., 4.9, 7.9, 11.8, 19.7 }).error == ForcaHoleFit::Error::Implausible);
}

TEST_CASE("The hole fit keeps its suggestion within the settings' range", "[ForcaCalibModel]")
{
    // 10 % small: more than X-Y hole compensation (% of size) allows.
    const ForcaHoleFit fit = forca_hole_fit(forca_hole_plate_sizes(), printed_holes(0.9, 0.));
    REQUIRE(fit.error == ForcaHoleFit::Error::None);
    CHECK(fit.clamped);
    CHECK(fit.percent == 5.);
}

TEST_CASE("The hole plate slices into a bar with one hole per size", "[ForcaCalibModel]")
{
    using Catch::Matchers::WithinAbs;
    Model        model;
    ModelObject* obj = model.add_object();
    forca_build_hole_plate(*obj);
    obj->add_instance();
    obj->ensure_on_bed();
    REQUIRE(obj->volumes.size() == forca_hole_plate_sizes().size() + 2); // bar, holes, notch

    Print              print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });
    print.auto_assign_extruders(obj);
    print.apply(model, config);
    print.validate();
    print.process();

    const PrintObject& po  = *print.objects().front();
    const Layer&       mid = *po.layers()[po.layers().size() / 2];
    REQUIRE(mid.lslices.size() == 1);
    std::vector<double> diameters;
    for (const Polygon& hole : mid.lslices.front().holes)
        diameters.push_back(2. * std::sqrt(std::abs(unscale<double>(unscale<double>(hole.area()))) / PI));
    std::sort(diameters.begin(), diameters.end());
    const std::vector<double>& sizes = forca_hole_plate_sizes();
    REQUIRE(diameters.size() == sizes.size()); // the notch is an edge cut, not a hole
    for (size_t i = 0; i < sizes.size(); ++i)
        CHECK_THAT(diameters[i], WithinAbs(sizes[i], 0.02));
}
