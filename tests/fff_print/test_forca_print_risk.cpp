// Forca: the print-risk map (ForcaPrintRisk). Small models with one risk each: a bridge, an overhang, a floating island,
// a thin tall pillar, a small first layer; and layer times too short to cool.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "test_helpers.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/ForcaPrintRisk.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// A box of the given size whose lowest corner is at (x, y, z), mm.
TriangleMesh box(double sx, double sy, double sz, double x, double y, double z)
{
    TriangleMesh m = make_cube(sx, sy, sz);
    m.translate(float(x), float(y), float(z));
    return m;
}

// The risks of one object made of `boxes`, sliced at 0.2 mm layers with `items` set.
std::vector<PrintRisk> risks_of(std::initializer_list<TriangleMesh> boxes, std::initializer_list<ConfigBase::SetDeserializeItem> items = {})
{
    TriangleMesh mesh;
    for (const TriangleMesh& b : boxes)
        mesh.merge(b);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });
    config.set_deserialize_strict(items);
    Model model;
    Print print;
    init_print({ mesh }, print, model, config);
    print.process();
    return forca_print_risks(*print.objects().front());
}

size_t count(const std::vector<PrintRisk>& risks, PrintRiskType type)
{
    return size_t(std::count_if(risks.begin(), risks.end(), [type](const PrintRisk& r) { return r.type == type; }));
}

const PrintRisk& first(const std::vector<PrintRisk>& risks, PrintRiskType type)
{
    return *std::find_if(risks.begin(), risks.end(), [type](const PrintRisk& r) { return r.type == type; });
}

} // namespace

TEST_CASE("A plain cube has no print risks", "[ForcaPrintRisk]")
{
    CHECK(risks_of({ box(20, 20, 20, 0, 0, 0) }).empty());
}

TEST_CASE("A bridge longer than max_bridge_length is a risk, with its span", "[ForcaPrintRisk]")
{
    // Two 10 mm pillars 30 mm apart, joined by a 2 mm slab on top at 10 mm.
    const auto bridge = { box(10, 10, 10, 0, 0, 0), box(10, 10, 10, 40, 0, 0), box(50, 10, 2, 0, 0, 10) };

    const std::vector<PrintRisk> risks = risks_of(bridge, { { "max_bridge_length", "10" } });
    REQUIRE(count(risks, PrintRiskType::LongBridge) == 1);
    const PrintRisk& r = first(risks, PrintRiskType::LongBridge);
    CHECK_THAT(r.distance, WithinAbs(30., 1.));
    CHECK_THAT(r.z_min, WithinAbs(10.2, 0.01)); // the first layer over the gap
    CHECK_FALSE(r.supported);
    CHECK(count(risks, PrintRiskType::Overhang) == 0);

    // The same 30 mm span is within a 40 mm max_bridge_length.
    CHECK(count(risks_of(bridge, { { "max_bridge_length", "40" } }), PrintRiskType::LongBridge) == 0);
}

TEST_CASE("A slab held on one side is an overhang, with how far it reaches", "[ForcaPrintRisk]")
{
    // A 10 mm pillar with a 30 mm slab on top: 20 mm of it over air, held on one side.
    const std::vector<PrintRisk> risks = risks_of({ box(10, 10, 10, 0, 0, 0), box(30, 10, 2, 0, 0, 10) });
    REQUIRE(count(risks, PrintRiskType::Overhang) == 1);
    const PrintRisk& r = first(risks, PrintRiskType::Overhang);
    CHECK_THAT(r.distance, WithinAbs(20., 1.));
    CHECK_THAT(r.bbox.size().x(), WithinAbs(20., 1.)); // from where the pillar ends to the slab's end
    CHECK(count(risks, PrintRiskType::LongBridge) == 0);
}

TEST_CASE("A part that starts in mid-air is a floating island, supported only when support is on", "[ForcaPrintRisk]")
{
    // A cube on the bed and, beside it, a cube that starts 15 mm up.
    const auto model = { box(10, 10, 10, 0, 0, 0), box(10, 10, 10, 20, 0, 15) };

    const std::vector<PrintRisk> risks = risks_of(model);
    REQUIRE(count(risks, PrintRiskType::FloatingIsland) == 1);
    const PrintRisk& r = first(risks, PrintRiskType::FloatingIsland);
    CHECK_THAT(r.z_min, WithinAbs(15.2, 0.01));
    CHECK_THAT(r.area, WithinAbs(100., 2.));
    CHECK_FALSE(r.supported);

    const std::vector<PrintRisk> with_support = risks_of(model, { { "enable_support", "1" }, { "support_type", "normal(auto)" } });
    REQUIRE(count(with_support, PrintRiskType::FloatingIsland) == 1);
    CHECK(first(with_support, PrintRiskType::FloatingIsland).supported);
}

TEST_CASE("A pillar standing more than eight times its width is a thin tall risk", "[ForcaPrintRisk]")
{
    // 4 mm wide: 40 mm tall is 10 times its width, 20 mm only 5 times.
    const std::vector<PrintRisk> tall = risks_of({ box(4, 4, 40, 0, 0, 0) }, { { "brim_type", "no_brim" } });
    REQUIRE(count(tall, PrintRiskType::ThinTall) == 1);
    CHECK_THAT(first(tall, PrintRiskType::ThinTall).distance, WithinAbs(4., 0.5));
    CHECK_FALSE(first(tall, PrintRiskType::ThinTall).supported);

    CHECK(count(risks_of({ box(4, 4, 20, 0, 0, 0) }), PrintRiskType::ThinTall) == 0);
}

TEST_CASE("A first layer much smaller than the object is a small footprint, held when there is a brim", "[ForcaPrintRisk]")
{
    // A 6 x 6 mm stem under a 30 x 30 mm block: the first layer is 4% of the largest layer, the object 12 mm tall.
    const auto model = { box(6, 6, 6, 12, 12, 0), box(30, 30, 6, 0, 0, 6) };

    const std::vector<PrintRisk> bare = risks_of(model, { { "brim_type", "no_brim" } });
    REQUIRE(count(bare, PrintRiskType::SmallFootprint) == 1);
    const PrintRisk& r = first(bare, PrintRiskType::SmallFootprint);
    CHECK_THAT(r.area, WithinAbs(36., 2.));
    CHECK_THAT(r.limit, WithinAbs(900., 10.));
    CHECK_THAT(r.distance, WithinAbs(12., 0.01));
    CHECK_FALSE(r.supported);

    const std::vector<PrintRisk> brim = risks_of(model, { { "brim_type", "outer_only" }, { "brim_width", "5" } });
    REQUIRE(count(brim, PrintRiskType::SmallFootprint) == 1);
    CHECK(first(brim, PrintRiskType::SmallFootprint).supported);

    // A plain 20 mm cube stands on all of its base.
    CHECK(count(risks_of({ box(20, 20, 20, 0, 0, 0) }, { { "brim_type", "no_brim" } }), PrintRiskType::SmallFootprint) == 0);
}

TEST_CASE("Runs of layers shorter than the layer time are short-layer risks", "[ForcaPrintRisk]")
{
    // Layer time 10 s: a layer under 9 s is short.
    const std::vector<ForcaLayerTime> layers = { { 0.2, 30 }, { 0.4, 8 }, { 0.6, 5 }, { 0.8, 9.5 }, { 1.0, 3 } };
    const std::vector<PrintRisk>      risks  = forca_short_layer_risks(layers, 10.);
    REQUIRE(risks.size() == 2);
    CHECK(risks[0].type == PrintRiskType::ShortLayers);
    CHECK(risks[0].first_layer == 1);
    CHECK(risks[0].last_layer == 2);
    CHECK_THAT(risks[0].z_min, WithinAbs(0.4, 1e-9));
    CHECK_THAT(risks[0].z_max, WithinAbs(0.6, 1e-9));
    CHECK_THAT(risks[0].distance, WithinAbs(5., 1e-9));
    CHECK_THAT(risks[0].limit, WithinAbs(10., 1e-9));
    CHECK(risks[1].first_layer == 4);

    CHECK(forca_short_layer_risks(layers, 0.).empty());
}
