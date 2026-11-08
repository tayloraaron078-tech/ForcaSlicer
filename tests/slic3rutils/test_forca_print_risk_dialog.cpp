// Forca: the Print Risks window (src/slic3r/GUI/ForcaPrintRiskDialog.cpp) -- selecting a risk moves the Preview's layer
// slider to the layer where it starts, and links it to the Forca Academy's playbook and journal prints.
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ForcaPrintRisk.hpp"
#include "slic3r/GUI/ForcaAcademy.hpp"
#include "slic3r/GUI/ForcaPrintRiskDialog.hpp"

#include <boost/filesystem/path.hpp>

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

TEST_CASE("A bridge links to the Bridging playbook, the other risks to Supports", "[ForcaPrintRiskDialog]")
{
    CHECK(forca_risk_academy_tag(PrintRiskType::LongBridge) == "bridging");
    CHECK(forca_risk_academy_tag(PrintRiskType::Overhang) == "supports");
    CHECK(forca_risk_academy_tag(PrintRiskType::FloatingIsland) == "supports");
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
