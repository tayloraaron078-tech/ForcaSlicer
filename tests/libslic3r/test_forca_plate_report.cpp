// Forca: the plate report's fixed rules (ForcaPlateReport): facts in, the failed checks out.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ForcaPlateReport.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A 256 x 256 x 250 mm bed, a 0.4 mm nozzle at 0.2 mm layers, and one 20 mm cube in the middle: nothing to report.
ForcaPlateFacts good_plate()
{
    ForcaPlateFacts f;
    f.bed                = BoundingBoxf(Vec2d(0, 0), Vec2d(256, 256));
    f.max_height         = 250;
    f.objects            = BoundingBoxf3(Vec3d(118, 118, 0), Vec3d(138, 138, 20));
    f.nozzle_diameter    = 0.4;
    f.layer_height       = 0.2;
    f.first_layer_height = 0.2;
    f.filaments.push_back({ 1, "PLA", 220, 220, 190, 240, 60 });
    return f;
}

const ForcaPlateCheck* find(const std::vector<ForcaPlateCheck>& checks, ForcaPlateCheckId id)
{
    const auto it = std::find_if(checks.begin(), checks.end(), [id](const ForcaPlateCheck& c) { return c.id == id; });
    return it == checks.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("A centred cube with matching settings passes every check", "[ForcaPlateReport]")
{
    CHECK(forca_plate_checks(good_plate()).empty());
}

TEST_CASE("Objects past the bed are a warning, within 5 mm of its edge a note", "[ForcaPlateReport]")
{
    ForcaPlateFacts f = good_plate();
    f.objects         = BoundingBoxf3(Vec3d(-2, 100, 0), Vec3d(18, 120, 20)); // 2 mm past the left edge
    const auto out    = forca_plate_checks(f);
    REQUIRE(find(out, ForcaPlateCheckId::OutsideBed) != nullptr);
    CHECK(find(out, ForcaPlateCheckId::OutsideBed)->warning);
    CHECK(find(out, ForcaPlateCheckId::NearEdge) == nullptr);

    f.objects       = BoundingBoxf3(Vec3d(3, 100, 0), Vec3d(23, 120, 20)); // 3 mm from the left edge
    const auto near = forca_plate_checks(f);
    REQUIRE(find(near, ForcaPlateCheckId::NearEdge) != nullptr);
    CHECK_FALSE(find(near, ForcaPlateCheckId::NearEdge)->warning);
    CHECK_THAT(find(near, ForcaPlateCheckId::NearEdge)->value, WithinAbs(3., 1e-9));

    // G-code outside the printable area counts even when the objects are inside.
    ForcaPlateFacts g  = good_plate();
    g.toolpath_outside = true;
    CHECK(find(forca_plate_checks(g), ForcaPlateCheckId::OutsideBed) != nullptr);
}

TEST_CASE("Height is a warning above the printable height and a note above 90% of it", "[ForcaPlateReport]")
{
    ForcaPlateFacts f = good_plate();
    f.objects.max.z() = 260; // printable height 250
    REQUIRE(find(forca_plate_checks(f), ForcaPlateCheckId::TooTall) != nullptr);
    CHECK_THAT(find(forca_plate_checks(f), ForcaPlateCheckId::TooTall)->limit, WithinAbs(250., 1e-9));

    f.objects.max.z() = 230; // 92%
    CHECK(find(forca_plate_checks(f), ForcaPlateCheckId::TooTall) == nullptr);
    CHECK(find(forca_plate_checks(f), ForcaPlateCheckId::NearMaxHeight) != nullptr);
}

TEST_CASE("Layers thicker than 80% of the nozzle are warnings", "[ForcaPlateReport]")
{
    ForcaPlateFacts f    = good_plate();
    f.layer_height       = 0.36; // 0.4 mm nozzle: at most 0.32
    f.first_layer_height = 0.3;
    const auto out       = forca_plate_checks(f);
    REQUIRE(find(out, ForcaPlateCheckId::LayerTooThick) != nullptr);
    CHECK_THAT(find(out, ForcaPlateCheckId::LayerTooThick)->limit, WithinAbs(0.32, 1e-9));
    CHECK(find(out, ForcaPlateCheckId::FirstLayerTooThick) == nullptr);
}

TEST_CASE("A nozzle temperature outside the filament's range is a warning for that filament", "[ForcaPlateReport]")
{
    ForcaPlateFacts f              = good_plate();
    f.filaments[0].first_nozzle_temp = 250; // range 190 to 240
    const auto out                 = forca_plate_checks(f);
    REQUIRE(find(out, ForcaPlateCheckId::NozzleTempOutsideRange) != nullptr);
    const ForcaPlateCheck& c = *find(out, ForcaPlateCheckId::NozzleTempOutsideRange);
    CHECK(c.filament == "PLA");
    CHECK_THAT(c.value, WithinAbs(250., 1e-9));
    CHECK_THAT(c.low, WithinAbs(190., 1e-9));
    CHECK_THAT(c.limit, WithinAbs(240., 1e-9));

    // No range known: nothing to compare with.
    f.filaments[0].range_low = f.filaments[0].range_high = 0;
    CHECK(forca_plate_checks(f).empty());
}

TEST_CASE("Filaments far apart in bed temperature are a warning, in nozzle temperature a note", "[ForcaPlateReport]")
{
    ForcaPlateFacts f = good_plate();
    f.filaments.push_back({ 2, "PETG", 255, 255, 230, 260, 80 }); // 35 °C hotter nozzle, 20 °C hotter bed
    const auto out = forca_plate_checks(f);
    REQUIRE(find(out, ForcaPlateCheckId::BedTempsFarApart) != nullptr);
    CHECK(find(out, ForcaPlateCheckId::BedTempsFarApart)->warning);
    CHECK_THAT(find(out, ForcaPlateCheckId::BedTempsFarApart)->value, WithinAbs(20., 1e-9));
    REQUIRE(find(out, ForcaPlateCheckId::NozzleTempsFarApart) != nullptr);
    CHECK_FALSE(find(out, ForcaPlateCheckId::NozzleTempsFarApart)->warning);
    // Warnings come before notes.
    CHECK(out.front().warning);
    CHECK_FALSE(out.back().warning);
}

TEST_CASE("Each check has a key for Forca AI", "[ForcaPlateReport]")
{
    CHECK(forca_plate_check_key(ForcaPlateCheckId::OutsideBed) == "outside_bed");
    CHECK(forca_plate_check_key(ForcaPlateCheckId::NozzleTempOutsideRange) == "nozzle_temp_outside_range");
}
