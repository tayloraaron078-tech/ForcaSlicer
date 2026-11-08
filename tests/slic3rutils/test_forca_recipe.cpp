// Forca: preset recipes (src/slic3r/GUI/ForcaRecipe.cpp) -- which of a print's settings go into a recipe, how a
// filament preset gets its slot's values, and the recipe page in the Academy's recipes/ folder.
// Same Windows include prologue as test_forca_academy.cpp (wx pulls in <windows.h>).
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <ios>
#include "catch2/catch_message.hpp"
#include "catch2/catch_test_macros.hpp"
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ForcaAcademy.hpp"
#include "slic3r/GUI/ForcaRecipe.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

struct ScopedDir
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("forca-recipe-%%%%-%%%%");
    ~ScopedDir()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string read_file(const fs::path& p)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    std::stringstream       ss;
    ss << f.rdbuf();
    return ss.str();
}

const ForcaRecipeRow* find_row(const std::vector<ForcaRecipeRow>& rows, const std::string& key)
{
    for (const ForcaRecipeRow& r : rows)
        if (r.key == key)
            return &r;
    return nullptr;
}

DynamicPrintConfig process_parent()
{
    DynamicPrintConfig c;
    c.set_deserialize_strict({ { "layer_height", "0.2" }, { "wall_loops", "2" }, { "sparse_infill_density", "15%" },
                               { "inherits", "System process" } });
    return c;
}

} // namespace

TEST_CASE("A recipe lists only the settings in which the print differs from the parent", "[ForcaRecipe]")
{
    const json print = { { "layer_height", "0.2" }, { "wall_loops", "4" }, { "sparse_infill_density", "25%" },
                         { "inherits", "Another process" }, { "print_host_apikey", "secret" }, { "no_such_setting", "1" } };
    const std::vector<ForcaRecipeRow> rows = forca_recipe_rows(process_parent(), print, -1, nullptr);
    REQUIRE(rows.size() == 2); // layer_height is the same; bookkeeping, private and unknown keys are skipped
    const ForcaRecipeRow* walls = find_row(rows, "wall_loops");
    REQUIRE(walls != nullptr);
    CHECK(walls->parent == "2");
    CHECK(walls->value == "4");
    CHECK(walls->problem.empty());
    REQUIRE(find_row(rows, "sparse_infill_density") != nullptr);
}

TEST_CASE("A recipe can keep to the settings changed when the print was sent", "[ForcaRecipe]")
{
    const json print   = { { "wall_loops", "4" }, { "sparse_infill_density", "25%" } };
    const json changed = { { "wall_loops", "4" } };
    const std::vector<ForcaRecipeRow> rows = forca_recipe_rows(process_parent(), print, -1, &changed);
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().key == "wall_loops");
}

TEST_CASE("A filament recipe takes the values of the filament it is for", "[ForcaRecipe]")
{
    DynamicPrintConfig parent;
    parent.set_deserialize_strict({ { "nozzle_temperature", "220" }, { "filament_flow_ratio", "0.98" } });
    // The print's full config holds one value per filament.
    const json print = { { "nozzle_temperature", "220,235" }, { "filament_flow_ratio", "0.98,0.95" } };

    CHECK(forca_recipe_rows(parent, print, 0, nullptr).empty()); // filament 1 printed with the parent's values

    const std::vector<ForcaRecipeRow> second = forca_recipe_rows(parent, print, 1, nullptr);
    REQUIRE(second.size() == 2);
    CHECK(find_row(second, "nozzle_temperature")->value == "235");
    CHECK(find_row(second, "filament_flow_ratio")->value == "0.95");

    // A filament the print didn't have has no values to take.
    const std::vector<ForcaRecipeRow> missing = forca_recipe_rows(parent, print, 2, nullptr);
    REQUIRE(missing.size() == 2);
    CHECK(missing.front().problem == "invalid");
}

TEST_CASE("A value the journal cut short or that no longer parses is shown but not used", "[ForcaRecipe]")
{
    DynamicPrintConfig parent = process_parent();
    parent.set_deserialize_strict({ { "filename_format", "{input_filename_base}.gcode" } });
    const std::string long_value = std::string(200, 'x') + "...";
    const json print = { { "filename_format", long_value }, { "wall_loops", "lots" } };
    const std::vector<ForcaRecipeRow> rows = forca_recipe_rows(parent, print, -1, nullptr);
    REQUIRE(rows.size() == 2);
    CHECK(find_row(rows, "filename_format")->problem == "cut");
    CHECK(find_row(rows, "wall_loops")->problem == "invalid");

    // A cut value that starts like the parent's is most likely the parent's: no row.
    parent.set_deserialize_strict({ { "filename_format", std::string(250, 'x') } });
    CHECK(forca_recipe_rows(parent, { { "filename_format", long_value } }, -1, nullptr).empty());

    // Applying sets the usable rows only.
    DynamicPrintConfig config = process_parent();
    std::string        err;
    REQUIRE(forca_recipe_apply(config, forca_recipe_rows(config, { { "wall_loops", "lots" }, { "sparse_infill_density", "40%" } }, -1, nullptr), err));
    CHECK(config.opt_serialize("wall_loops") == "2");
    CHECK(config.opt_serialize("sparse_infill_density") == "40%");
}

TEST_CASE("A recipe has a filament preset for each filament the print used", "[ForcaRecipe]")
{
    const json weighed = { { "filaments", json::array({ { { "slot", 1 }, { "preset", "PLA" }, { "used_g", 12.5 } },
                                                        { { "slot", 2 }, { "preset", "PETG" } },
                                                        { { "slot", 3 }, { "preset", "TPU" }, { "used_g", 1.0 } } }) } };
    CHECK(forca_recipe_used_slots(weighed) == std::vector<int>{ 0, 2 });
    // Without weights (no slice result was recorded) every filament counts.
    const json unweighed = { { "filaments", json::array({ { { "slot", 1 }, { "preset", "PLA" } }, { { "slot", 2 }, { "preset", "PETG" } } }) } };
    CHECK(forca_recipe_used_slots(unweighed) == std::vector<int>{ 0, 1 });
    CHECK(forca_recipe_used_slots(json::object()).empty());
}

TEST_CASE("A recipe page links its print, lists each preset's settings and is never overwritten", "[ForcaRecipe]")
{
    ScopedDir   tmp;
    std::string err;
    const std::vector<ForcaRecipePreset> presets = {
        { "process", "PETG parts", "0.20mm Standard", -1, { { "wall_loops", "2", "4", "" } } },
        { "filament", "PETG parts", "Generic PETG", 1, { { "filament_start_gcode", "a", "b|c", "" } } },
    };
    const fs::path page = forca_recipe_write_page(tmp.dir, "PETG parts", "2026-11-01", "prints/2026/2026-11-01_0900_bracket",
                                                  "bracket", "Made from a successful print.", presets, err);
    REQUIRE(!page.empty());
    CHECK(page.filename() == "2026-11-01_petg-parts.md");
    const std::string text = read_file(page);
    CHECK(text.find("# PETG parts") == 0);
    CHECK(text.find("[bracket](../prints/2026/2026-11-01_0900_bracket)") != std::string::npos);
    CHECK(text.find("| wall_loops | `2` | `4` |") != std::string::npos);
    CHECK(text.find("Filament (filament 2): PETG parts") != std::string::npos);
    CHECK(text.find("`b\\|c`") != std::string::npos); // a | in a value doesn't end the table cell

    const fs::path again = forca_recipe_write_page(tmp.dir, "PETG parts", "2026-11-01", "", "", "", {}, err);
    CHECK(again.filename() == "2026-11-01_petg-parts-2.md");
    CHECK(read_file(page) == text);

    const auto listed = forca_recipe_list(tmp.dir);
    REQUIRE(listed.size() == 2);
    CHECK(listed.front().first == "2026-11-01_petg-parts-2");
    CHECK(listed.back().second == "PETG parts");
}

TEST_CASE("INDEX.md lists the recipes so an AI reading the Academy finds them", "[ForcaRecipe]")
{
    ScopedDir   tmp;
    std::string err;
    CHECK(forca_academy_index_facts(tmp.dir, {}, {}).find("Recipes") == std::string::npos);
    REQUIRE(!forca_recipe_write_page(tmp.dir, "PLA quick drafts", "2026-11-01", "", "", "", {}, err).empty());
    CHECK(forca_academy_index_facts(tmp.dir, {}, {}).find("[PLA quick drafts](recipes/2026-11-01_pla-quick-drafts.md)") !=
          std::string::npos);
    CHECK(read_file(tmp.dir / "AGENTS.md").find("`recipes/`") != std::string::npos);
}
