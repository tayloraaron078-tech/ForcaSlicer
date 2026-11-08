// Forca: the print-risk map (ForcaPrintRisk). Small models with one risk each: a bridge, an overhang, a floating island.
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
