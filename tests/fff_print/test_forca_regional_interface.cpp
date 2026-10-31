// [regional-supports fork] Regional support interfaces: a support-interface modifier (ModelVolumeType::
// SUPPORT_INTERFACE_MODIFIER) gives the support interface inside its footprint its own filament, while the rest of
// the interface keeps the object's. Checked on the emitted G-code, so a change anywhere in support generation or
// G-code export that loses the regional split fails here.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "test_helpers.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Support-interface extrusion moves printed with one filament, and the sum of their X (for the mean).
struct InterfaceMoves
{
    size_t count = 0;
    double x_sum = 0.;
    double x_min = 1e9;
    double x_max = -1e9;
    double mean_x() const { return count > 0 ? x_sum / double(count) : 0.; }
};

// Slices a T: a 6 x 6 mm pillar under a 30 x 30 x 2 mm plate at 10 mm, so the plate's underside needs support. With
// `modifier`, a support-interface modifier under the +X third of the plate sets interface filament 2.
std::pair<InterfaceMoves, InterfaceMoves> slice_t(const std::string &support_type, const std::string &support_style, bool modifier,
                                                  bool prime_tower = false)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "enable_support", "1" },
        { "support_type", support_type },
        { "support_style", support_style },
        { "support_interface_top_layers", "2" },
        { "enable_prime_tower", prime_tower ? "1" : "0" },
        // On the 200 x 200 mm bed, clear of the part at X/Y 0..30 (the default position is off this bed).
        { "wipe_tower_x", "150" },
        { "wipe_tower_y", "150" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
    });
    TriangleMesh part = make_cube(6, 6, 10);
    part.translate(12.f, 12.f, 0.f);
    TriangleMesh plate = make_cube(30, 30, 2);
    plate.translate(0.f, 0.f, 10.f);
    part.merge(plate);

    Model model;
    Print print;
    init_print({ part }, print, model, config, nullptr, false);
    if (modifier) {
        TriangleMesh box = make_cube(12, 30, 4);
        box.translate(18.f, 0.f, 7.f);
        ModelVolume *v = model.objects.front()->add_volume(std::move(box), ModelVolumeType::SUPPORT_INTERFACE_MODIFIER, false);
        v->config.set_key_value("support_interface_filament", new ConfigOptionInt(2));
        print.apply(model, config);
    }

    const std::string gcode = Test::gcode(print);
    InterfaceMoves    filament[2];
    std::istringstream in(gcode);
    std::string        line;
    int                tool      = 0;
    bool               interface = false;
    while (std::getline(in, line)) {
        if (line.size() >= 2 && line[0] == 'T' && (line[1] == '0' || line[1] == '1') && (line.size() == 2 || line[2] == ' ' || line[2] == '\r'))
            tool = line[1] - '0';
        else if (line.rfind(";TYPE:", 0) == 0 || line.rfind("; FEATURE: ", 0) == 0)
            interface = line.find("Support interface") != std::string::npos;
        else if (interface && line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos) {
            const size_t x = line.find(" X");
            if (x == std::string::npos)
                continue;
            const double xv = std::stod(line.substr(x + 2));
            filament[tool].count += 1;
            filament[tool].x_sum += xv;
            filament[tool].x_min = std::min(filament[tool].x_min, xv);
            filament[tool].x_max = std::max(filament[tool].x_max, xv);
        }
    }
    return { filament[0], filament[1] };
}

} // namespace

TEST_CASE("A support-interface modifier prints the interface inside it with its own filament", "[ForcaRegionalInterface]")
{
    const auto [support_type, support_style] =
        GENERATE(std::pair<std::string, std::string>{ "normal(auto)", "grid" }, std::pair<std::string, std::string>{ "tree(auto)", "organic" });
    CAPTURE(support_type, support_style);

    const auto [object_filament, region_filament] = slice_t(support_type, support_style, true);
    INFO("filament 1: " << object_filament.count << " moves, mean X " << object_filament.mean_x());
    INFO("filament 2: " << region_filament.count << " moves, mean X " << region_filament.mean_x());
    // Both filaments print interface: the region's inside the box, the object's outside it.
    CHECK(region_filament.count > 0);
    CHECK(object_filament.count > 0);
    // The box covers the +X side of the plate.
    CHECK(region_filament.mean_x() > object_filament.mean_x());
}

TEST_CASE("Without a support-interface modifier the interface keeps the object's filament", "[ForcaRegionalInterface]")
{
    const auto [support_type, support_style] =
        GENERATE(std::pair<std::string, std::string>{ "normal(auto)", "grid" }, std::pair<std::string, std::string>{ "tree(auto)", "organic" });
    CAPTURE(support_type, support_style);

    const auto [object_filament, region_filament] = slice_t(support_type, support_style, false);
    CHECK(object_filament.count > 0);
    CHECK(region_filament.count == 0);
}


// The box spans X 18..30 (the object prints where it is modelled), so the region's filament must stay there; a line
// width of slack on each side. With grid supports the lower interface layer is a support-only layer whose only planned
// filament was the region's, and the "don't care" interface outside the box used to merge into it (ToolOrdering's
// regional_dontcare_extruder()); this checks both support types.
TEST_CASE("The modifier's filament prints only inside the modifier", "[ForcaRegionalInterface]")
{
    const auto [support_type, support_style] =
        GENERATE(std::pair<std::string, std::string>{ "normal(auto)", "grid" }, std::pair<std::string, std::string>{ "tree(auto)", "organic" });
    CAPTURE(support_type, support_style);

    const auto [object_filament, region_filament] = slice_t(support_type, support_style, true);
    INFO("filament 2: X " << region_filament.x_min << ".." << region_filament.x_max);
    REQUIRE(region_filament.count > 0);
    CHECK(region_filament.x_min > 17.5);
    CHECK(region_filament.x_max < 30.5);
}

// The fix plans the object's filament on the support-only interface layer; with a prime tower that plan drives the
// tower's tool changes, so slice it with one too.
TEST_CASE("Grid supports with a prime tower print the modifier's filament only inside the modifier", "[ForcaRegionalInterface]")
{
    const auto [object_filament, region_filament] = slice_t("normal(auto)", "grid", true, true);
    INFO("filament 2: X " << region_filament.x_min << ".." << region_filament.x_max);
    REQUIRE(region_filament.count > 0);
    CHECK(object_filament.count > 0);
    CHECK(region_filament.x_min > 17.5);
    CHECK(region_filament.x_max < 30.5);
}
