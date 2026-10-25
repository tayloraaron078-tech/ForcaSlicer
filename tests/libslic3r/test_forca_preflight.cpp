// Forca: the facts behind the send dialogs' "Before you print" block (libslic3r/ForcaPreflight).
#include <catch2/catch_all.hpp>

#include "catch2/catch_test_macros.hpp"
#include "libslic3r/ForcaPreflight.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const ForcaPreflightValue* find(const std::vector<ForcaPreflightValue>& values, const std::string& key)
{
    auto it = std::find_if(values.begin(), values.end(), [&key](const ForcaPreflightValue& v) { return v.key == key; });
    return it == values.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("Pre-flight changes list only the options that differ from the saved preset", "[ForcaPreflight]")
{
    DynamicPrintConfig saved = DynamicPrintConfig::full_print_config();
    saved.set_deserialize_strict({ { "sparse_infill_density", "20%" }, { "wall_loops", "2" }, { "layer_height", "0.2" } });
    DynamicPrintConfig edited = saved;
    edited.set_deserialize_strict({ { "sparse_infill_density", "15%" }, { "wall_loops", "3" } });

    const auto changes = forca_preflight_changes(saved, edited, { "sparse_infill_density", "wall_loops", "layer_height", "no_such_key" });

    REQUIRE(changes.size() == 2);
    CHECK(changes[0].key == "sparse_infill_density");
    CHECK(changes[0].value == "15%");
    CHECK(changes[0].saved == "20%");
    CHECK(changes[1].key == "wall_loops");
    CHECK(changes[1].value == "3");
    CHECK(changes[1].saved == "2");
}

TEST_CASE("Pre-flight process details include layer height, infill, supports, brim and nozzle", "[ForcaPreflight]")
{
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.set_deserialize_strict({ { "layer_height", "0.16" }, { "sparse_infill_pattern", "gyroid" }, { "sparse_infill_density", "25%" },
                                  { "enable_support", "1" }, { "nozzle_diameter", "0.6,0.4" } });

    const auto details = forca_preflight_process_details(full);

    for (const char* key : { "layer_height", "sparse_infill_pattern", "sparse_infill_density", "enable_support", "support_type", "brim_type",
                             "nozzle_diameter" })
        CHECK(find(details, key) != nullptr);
    CHECK(find(details, "layer_height")->value == "0.16");
    CHECK(find(details, "sparse_infill_pattern")->value == "gyroid");
    CHECK(find(details, "sparse_infill_density")->value == "25%");
    CHECK(find(details, "enable_support")->value == "1");
    CHECK(find(details, "nozzle_diameter")->value == "0.6"); // the first extruder
}

TEST_CASE("Pre-flight filament details use the slot's values and the plate's bed type", "[ForcaPreflight]")
{
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.set_deserialize_strict({ { "nozzle_temperature_initial_layer", "220,250" }, { "nozzle_temperature", "215,245" },
                                  { "textured_plate_temp", "60,70" }, { "hot_plate_temp", "55,80" },
                                  { "filament_max_volumetric_speed", "18,12" }, { "curr_bed_type", "Textured PEI Plate" } });

    const auto second = forca_preflight_filament_details(full, 1);
    CHECK(find(second, "nozzle_temperature_initial_layer")->value == "250");
    CHECK(find(second, "nozzle_temperature")->value == "245");
    REQUIRE(find(second, "textured_plate_temp") != nullptr); // Textured PEI -> textured_plate_temp
    CHECK(find(second, "textured_plate_temp")->value == "70");
    CHECK(find(second, "hot_plate_temp") == nullptr);
    CHECK(find(second, "filament_max_volumetric_speed")->value == "12");

    // A slot past the end of a list uses its last value.
    CHECK(find(forca_preflight_filament_details(full, 5), "nozzle_temperature")->value == "245");
}
