#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintheadClearance.hpp"

#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

double degrees(double radians) { return radians * 180. / std::acos(-1.); }
double radians(double degrees) { return degrees * std::acos(-1.) / 180.; }

// A 0.5 mm flat tip, a cone widening to 3.5 mm radius 3 mm above the tip (45 degrees), nothing else.
PrintheadProfile plain_cone() { return PrintheadProfile::cone(0.5, 3.5, 3.); }

} // namespace

TEST_CASE("A printhead outline gives the head's height at a distance from the nozzle", "[PrintheadClearance]")
{
    const PrintheadProfile head = plain_cone();
    // Under the flat tip, halfway up the cone, at its top, and past it where the head stays at the cone's height.
    CHECK_THAT(head.height_at(0.2), WithinAbs(0., 1e-9));
    CHECK_THAT(head.height_at(2.), WithinAbs(1.5, 1e-9));
    CHECK_THAT(head.height_at(3.5), WithinAbs(3., 1e-9));
    CHECK_THAT(head.height_at(50.), WithinAbs(3., 1e-9));
    CHECK(head.clears(2., 1.4));
    CHECK_FALSE(head.clears(2., 1.6));
}

TEST_CASE("A cone nozzle follows slopes up to its own cone angle", "[PrintheadClearance]")
{
    const PrintheadProfile head = plain_cone();
    // The cone rises 3 mm over 3 mm: 45 degrees. A surface 2 mm tall stays under the cone's top.
    CHECK(head.can_follow(radians(40.), 2.));
    CHECK_FALSE(head.can_follow(radians(50.), 2.));
    CHECK_THAT(degrees(head.max_slope(2.)), WithinAbs(45., 0.01));
}

TEST_CASE("A surface taller than the head's clearance cannot be followed at any slope", "[PrintheadClearance]")
{
    const PrintheadProfile head = plain_cone();
    CHECK_THAT(head.max_surface_height(), WithinAbs(3., 1e-9));
    CHECK(head.can_follow(radians(5.), 3.));
    CHECK_FALSE(head.can_follow(radians(5.), 3.1));
    CHECK_THAT(head.max_slope(3.1), WithinAbs(0., 1e-9));
}

TEST_CASE("A part hanging low beside the nozzle limits the slope more than the cone does", "[PrintheadClearance]")
{
    // The same cone, with a fan duct 15 mm from the nozzle whose lip is only 2 mm above the tip.
    const PrintheadProfile head = PrintheadProfile::cone(0.5, 3.5, 3., 15., 2.);
    CHECK_THAT(head.height_at(15.), WithinAbs(2., 1e-9));
    CHECK_THAT(head.max_surface_height(), WithinAbs(2., 1e-9));
    // A 1 mm tall surface passes under the duct, so the cone's 45 degrees still hold.
    CHECK_THAT(degrees(head.max_slope(1.)), WithinAbs(45., 0.01));
    // A 2.5 mm tall surface would reach the duct however gentle it is.
    CHECK_FALSE(head.can_follow(radians(5.), 2.5));
}

TEST_CASE("Rings may be given in any order", "[PrintheadClearance]")
{
    const PrintheadProfile head({ { 3.5, 3. }, { 0.5, 0. } });
    CHECK_THAT(head.height_at(2.), WithinAbs(1.5, 1e-9));
}

TEST_CASE("A head without an outline follows nothing", "[PrintheadClearance]")
{
    const PrintheadProfile head;
    CHECK(head.empty());
    CHECK_FALSE(head.can_follow(0., 0.));
    CHECK_THAT(head.max_slope(1.), WithinAbs(0., 1e-9));
}

TEST_CASE("The printer's settings give the printhead outline", "[PrintheadClearance]")
{
    FullPrintConfig config = FullPrintConfig::defaults();
    // Without a nozzle cone the outline is not known.
    CHECK(PrintheadProfile::from_config(config).empty());

    // A 1 mm tip, an 11 mm wide sock edge 2 mm above it, and a shroud 16 mm away hanging 5.5 mm above the tip.
    config.nozzle_tip_diameter.value     = 1.;
    config.nozzle_cone_diameter.value    = 11.;
    config.nozzle_cone_height.value      = 2.;
    config.printhead_body_distance.value = 16.;
    config.printhead_body_height.value   = 5.5;
    config.printhead_clearance_margin.value = 0.;
    const PrintheadProfile head = PrintheadProfile::from_config(config);
    CHECK_THAT(head.height_at(5.5), WithinAbs(2., 1e-9));
    CHECK_THAT(head.max_surface_height(), WithinAbs(5.5, 1e-9));
    // The outline starts at the nozzle's axis. A surface as tall as the sock edge is limited by the sock: 2 mm up
    // over 5.5 mm.
    CHECK_THAT(degrees(head.max_slope(2.)), WithinAbs(degrees(std::atan2(2., 5.5)), 0.01));
    // A surface as tall as the shroud is limited by the shroud: 5.5 mm up over 16 mm.
    CHECK_THAT(degrees(head.max_slope(5.5)), WithinAbs(degrees(std::atan2(5.5, 16.)), 0.01));
}

TEST_CASE("The clearance margin lowers the printhead outline", "[PrintheadClearance]")
{
    FullPrintConfig config = FullPrintConfig::defaults();
    config.nozzle_tip_diameter.value        = 1.;
    config.nozzle_cone_diameter.value       = 11.;
    config.nozzle_cone_height.value         = 2.;
    config.printhead_body_distance.value    = 13.5;
    config.printhead_body_height.value      = 5.5;
    config.printhead_clearance_margin.value = 0.5;
    const PrintheadProfile head = PrintheadProfile::from_config(config);
    // The tip stays where it is; everything above it counts as 0.5 mm lower.
    CHECK_THAT(head.height_at(0.), WithinAbs(0., 1e-9));
    CHECK_THAT(head.height_at(5.5), WithinAbs(1.5, 1e-9));
    CHECK_THAT(head.max_surface_height(), WithinAbs(5., 1e-9));
    // A margin as large as the cone is tall leaves no outline to work with.
    config.printhead_clearance_margin.value = 2.;
    CHECK(PrintheadProfile::from_config(config).empty());
}

TEST_CASE("Printed material must stay farther from the nozzle the higher it stands", "[PrintheadClearance]")
{
    // The cone with a fan duct 15 mm away, 2 mm above the tip: lower than the top of the cone.
    const PrintheadProfile head = PrintheadProfile::cone(0.5, 3.5, 3., 15., 2.);
    CHECK_THAT(head.tip_radius(), WithinAbs(0.5, 1e-9));
    // Nothing higher than the tip is in the way; just above it, the flat tip is.
    CHECK_THAT(head.keep_out_radius(0.), WithinAbs(0., 1e-9));
    CHECK_THAT(head.keep_out_radius(0.001), WithinAbs(0.501, 1e-9));
    // 1 mm tall material clears the duct, and the cone from 1.5 mm out.
    CHECK_THAT(head.keep_out_radius(1.), WithinAbs(1.5, 1e-9));
    // 2.5 mm tall material is taller than the duct hangs: it is in the way at any distance.
    CHECK(std::isinf(head.keep_out_radius(2.5)));
    CHECK(std::isinf(PrintheadProfile().keep_out_radius(0.1)));
    // Without the duct the head stays 3 mm up beyond the cone.
    CHECK_THAT(PrintheadProfile::cone(0.5, 3.5, 3.).keep_out_radius(2.5), WithinAbs(3., 1e-9));
}
