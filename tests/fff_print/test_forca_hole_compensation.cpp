// Forca: X-Y hole compensation as a percentage of each hole's size (xy_hole_compensation_percent, issue #10) --
// every hole moves by the fixed X-Y hole compensation plus its own share, so small and large holes both come out
// right; at 0 % slicing doesn't change.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A round hole of diameter d (mm) at (x, y), as a clockwise polygon.
Polygon round_hole(double x, double y, double d)
{
    Polygon hole;
    const size_t n = 720;
    for (size_t i = 0; i < n; ++i) {
        const double a = 2. * PI * double(i) / double(n);
        hole.points.emplace_back(Point::new_scale(x + d / 2. * std::cos(a), y + d / 2. * std::sin(a)));
    }
    hole.make_clockwise();
    return hole;
}

// A 60 x 30 mm plate with round holes of 3, 10 and 20 mm.
ExPolygons plate_with_holes()
{
    ExPolygon plate;
    plate.contour.points = {Point::new_scale(0, 0), Point::new_scale(60, 0), Point::new_scale(60, 30), Point::new_scale(0, 30)};
    plate.holes = {round_hole(8, 15, 3), round_hole(22, 15, 10), round_hole(45, 15, 20)};
    return {plate};
}

// Equivalent diameters (mm) of all holes, smallest first.
std::vector<double> hole_diameters(const ExPolygons &polys)
{
    std::vector<double> out;
    for (const ExPolygon &ex : polys)
        for (const Polygon &hole : ex.holes)
            out.push_back(2. * std::sqrt(std::abs(unscaled<double>(unscaled<double>(hole.area()))) / PI));
    std::sort(out.begin(), out.end());
    return out;
}

const std::vector<double> designed{3., 10., 20.};

} // namespace

TEST_CASE("Hole compensation by percentage grows each hole by its own share", "[ForcaHoleCompensation]")
{
    // 2 % of each diameter: 3 -> 3.06, 10 -> 10.2, 20 -> 20.4 mm.
    const std::vector<double> d = hole_diameters(shrink_contour_holes(0., 0., 0.02, 1, plate_with_holes()));
    REQUIRE(d.size() == 3);
    for (size_t i = 0; i < 3; ++i)
        CHECK_THAT(d[i], WithinAbs(designed[i] * 1.02, 0.005));
}

TEST_CASE("Hole compensation by percentage adds to the fixed hole compensation", "[ForcaHoleCompensation]")
{
    // 0.1 mm on each edge (0.2 mm on the diameter) plus 1 % of the diameter.
    const std::vector<double> d = hole_diameters(shrink_contour_holes(0., scaled<double>(0.1), 0.01, 1, plate_with_holes()));
    REQUIRE(d.size() == 3);
    for (size_t i = 0; i < 3; ++i)
        CHECK_THAT(d[i], WithinAbs(designed[i] * 1.01 + 0.2, 0.005));
}

TEST_CASE("Hole compensation without a percentage keeps the fixed offset", "[ForcaHoleCompensation]")
{
    const double fixed = GENERATE(0., 0.1, -0.1);
    CAPTURE(fixed);
    // The two passes slice_volumes() runs: growing, then shrinking.
    ExPolygons polys = shrink_contour_holes(0., scaled<double>(fixed), 0., 1, plate_with_holes());
    polys            = shrink_contour_holes(0., scaled<double>(fixed), 0., -1, polys);
    const std::vector<double> d = hole_diameters(polys);
    REQUIRE(d.size() == 3);
    for (size_t i = 0; i < 3; ++i)
        CHECK_THAT(d[i], WithinAbs(designed[i] + 2. * fixed, 0.005));
}

TEST_CASE("Hole compensation passes keep only their own sign per hole", "[ForcaHoleCompensation]")
{
    // -0.1 mm fixed and +2 %: the 3 mm hole shrinks (-0.1 + 0.03 on the radius), the 10 mm hole stays
    // (-0.1 + 0.1), the 20 mm hole grows (-0.1 + 0.2).
    const double fixed = scaled<double>(-0.1);

    const std::vector<double> grown = hole_diameters(shrink_contour_holes(0., fixed, 0.02, 1, plate_with_holes()));
    REQUIRE(grown.size() == 3);
    CHECK_THAT(grown[0], WithinAbs(3., 0.005));
    CHECK_THAT(grown[1], WithinAbs(10., 0.005));
    CHECK_THAT(grown[2], WithinAbs(20.2, 0.005));

    const std::vector<double> both = hole_diameters(
        shrink_contour_holes(0., fixed, 0.02, -1, shrink_contour_holes(0., fixed, 0.02, 1, plate_with_holes())));
    REQUIRE(both.size() == 3);
    CHECK_THAT(both[0], WithinAbs(2.86, 0.005));
    CHECK_THAT(both[1], WithinAbs(10., 0.005));
    CHECK_THAT(both[2], WithinAbs(20.2, 0.005));
}

namespace {

// Area (mm^2) of the holes in the middle layer of a sliced 20 mm cube with a 10 x 10 mm square hole.
double sliced_hole_area(const std::string &percent, bool with_modifier)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", "0.2"},
                                   {"initial_layer_print_height", "0.2"},
                                   {"elefant_foot_compensation", "0"},
                                   {"xy_hole_compensation_percent", percent}});
    Print print;
    Model model;
    init_print({mesh(TestMesh::cube_with_hole)}, print, model, config, nullptr, false);
    if (with_modifier) {
        // A modifier over half the cube (more walls there) splits each layer into two regions.
        DynamicPrintConfig modifier;
        modifier.set_deserialize_strict({{"wall_loops", "4"}});
        model.objects.front()->add_volume(make_cube(10, 20, 10), ModelVolumeType::PARAMETER_MODIFIER, false)->config.apply(modifier);
        print.apply(model, config);
    }
    print.process();

    const PrintObject &object = *print.objects().front();
    const Layer       &layer  = *object.get_layer(int(object.layer_count() / 2));
    if (with_modifier) {
        size_t regions = 0;
        for (const LayerRegion *region : layer.regions())
            regions += region->slices.empty() ? 0 : 1;
        REQUIRE(regions == 2);
    }
    double area = 0.;
    for (const ExPolygon &ex : layer.lslices)
        for (const Polygon &hole : ex.holes)
            area += std::abs(unscaled<double>(unscaled<double>(hole.area())));
    return area;
}

} // namespace

TEST_CASE("Slicing applies hole compensation by percentage", "[ForcaHoleCompensation]")
{
    const bool with_modifier = GENERATE(false, true);
    CAPTURE(with_modifier);

    // At 0 % the 10 x 10 mm hole stays as modelled.
    CHECK_THAT(sliced_hole_area("0%", with_modifier), WithinRel(100., 0.01));

    // At 2 % each edge moves out by 2 % of the hole's equivalent radius sqrt(100 / PI) = 5.64 mm.
    const double edge = 10. + 2. * 0.02 * std::sqrt(100. / PI);
    CHECK_THAT(sliced_hole_area("2%", with_modifier), WithinRel(edge * edge, 0.01));
}
