// Forca: the Print Risks window (src/slic3r/GUI/ForcaPrintRiskDialog.cpp) -- selecting a risk moves the Preview's layer
// slider to the layer where it starts, and links it to the Forca Academy's playbook and journal prints.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "libslic3r/ForcaPrintRisk.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/ForcaAcademy.hpp"
#include "slic3r/GUI/ForcaPrintRiskDialog.hpp"

#include <boost/filesystem/path.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;

TEST_CASE("A risk's height maps to the layer slider position of its layer", "[ForcaPrintRiskDialog]")
{
    const std::vector<double> zs = { 0.2, 0.4, 0.6, 0.8 }; // the slider's values: layer print_z, mm

    // A layer's own height, also when the float is a hair below it.
    CHECK(forca_risk_slider_index(zs, 0.6) == 2);
    CHECK(forca_risk_slider_index(zs, 0.6 - 1e-7) == 2);
    // Between layers: the next layer up.
    CHECK(forca_risk_slider_index(zs, 0.5) == 2);
    // Below the first and above the last layer.
    CHECK(forca_risk_slider_index(zs, 0.0) == 0);
    CHECK(forca_risk_slider_index(zs, 5.0) == 3);
    // No slider values (nothing sliced).
    CHECK(forca_risk_slider_index({}, 0.6) == -1);
}

TEST_CASE("Each risk links to the journal tag of the failure it leads to", "[ForcaPrintRiskDialog]")
{
    CHECK(forca_risk_academy_tag(PrintRiskType::LongBridge) == "bridging");
    CHECK(forca_risk_academy_tag(PrintRiskType::Overhang) == "supports");
    CHECK(forca_risk_academy_tag(PrintRiskType::FloatingIsland) == "supports");
    CHECK(forca_risk_academy_tag(PrintRiskType::SmallFootprint) == "bed_adhesion");
    CHECK(forca_risk_academy_tag(PrintRiskType::ThinTall) == "spaghetti");
    CHECK(forca_risk_academy_tag(PrintRiskType::ShortLayers).empty()); // no journal tag for cooling
}

TEST_CASE("Layer times add up each layer's moves, and only layers that extrude count", "[ForcaPrintRiskDialog]")
{
    using Catch::Matchers::WithinAbs;
    GCodeProcessorResult result;
    auto move = [&](unsigned layer, EMoveType type, float z, float seconds) {
        GCodeProcessorResult::MoveVertex m;
        m.layer_id   = layer;
        m.type       = type;
        m.position   = Vec3f(0, 0, z);
        m.time[0]    = seconds;
        result.moves.push_back(m);
    };
    move(0, EMoveType::Travel, 0.2f, 1.f);
    move(0, EMoveType::Extrude, 0.2f, 10.f);
    move(1, EMoveType::Extrude, 0.4f, 2.f);
    move(1, EMoveType::Travel, 5.0f, 0.5f); // a z-hop travel: not the layer's height
    move(1, EMoveType::Extrude, 0.4f, 3.f);
    move(2, EMoveType::Travel, 10.f, 4.f); // the end: travel only

    const std::vector<ForcaLayerTime> layers = forca_gcode_layer_times(result);
    REQUIRE(layers.size() == 2);
    CHECK_THAT(layers[0].z, WithinAbs(0.2, 1e-6));
    CHECK_THAT(layers[0].seconds, WithinAbs(11., 1e-6));
    CHECK_THAT(layers[1].z, WithinAbs(0.4, 1e-6));
    CHECK_THAT(layers[1].seconds, WithinAbs(5.5, 1e-6));
}

TEST_CASE("A risk's journal prints are those with its tag, this printer and one of the plate's filaments", "[ForcaPrintRiskDialog]")
{
    auto print = [](const char* folder, const char* printer, std::vector<std::string> filaments, std::vector<std::string> tags) {
        ForcaAcademyPrint p;
        p.folder    = folder;
        p.printer   = printer;
        p.result    = "partial";
        p.filaments = std::move(filaments);
        p.tags      = std::move(tags);
        return p;
    };
    const std::vector<ForcaAcademyPrint> prints = {
        print("a", "P1S", { "PLA Basic" }, { "bridging" }),
        print("b", "P1S", { "PETG HF" }, { "bridging", "stringing" }),
        print("c", "P1S", { "PLA Basic" }, { "stringing" }),           // another tag
        print("d", "Giga", { "PLA Basic" }, { "bridging" }),           // another printer
        print("e", "P1S", { "ASA" }, { "bridging" }),                  // a filament the plate doesn't use
    };

    const std::vector<boost::filesystem::path> found =
        forca_risk_tagged_prints(prints, "P1S", { "PLA Basic", "PETG HF" }, "bridging");
    REQUIRE(found.size() == 2);
    CHECK(found[0] == boost::filesystem::path("a")); // in the journal's order (newest first)
    CHECK(found[1] == boost::filesystem::path("b"));

    // No printer: any printer.
    CHECK(forca_risk_tagged_prints(prints, "", { "PLA Basic" }, "bridging").size() == 2);
    // No filaments on the plate: nothing matches.
    CHECK(forca_risk_tagged_prints(prints, "P1S", {}, "bridging").empty());
}
