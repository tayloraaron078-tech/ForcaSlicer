// Forca: per-object filament overrides (docs/HLSD/per-object-filament-overrides.md) -- an object's retraction,
// pressure advance and max volumetric speed (and, printing by object, its fan and nozzle temperatures) replace the
// filament's while it prints; with none set the G-code doesn't change.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cstddef>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig base_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor", "klipper" },
        { "gcode_comments", "1" },
        { "gcode_label_objects", "1" },
        { "use_relative_e_distances", "1" },
        { "retraction_length", "0.8" },
        { "retraction_speed", "30" },
        { "wipe", "0" },
        { "z_hop", "0" },
        { "enable_pressure_advance", "1" },
        { "pressure_advance", "0.02" },
        { "adaptive_pressure_advance", "0" },
    });
    return config;
}

// The retraction lengths the G-code uses ("; retract" moves, relative E).
std::set<double> retraction_lengths(const std::string& gcode)
{
    std::set<double>   out;
    std::istringstream in(gcode);
    std::string        line;
    const std::regex   retract(R"(^G1 E(-[0-9.]+) F[0-9.]+.*; retract)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, retract))
            out.insert(-std::stod(m[1].str()));
    }
    return out;
}

// The highest feed rate (mm/min) of the extruding moves; F is modal, so the last F seen applies.
double max_extrusion_feedrate(const std::string& gcode)
{
    std::istringstream in(gcode);
    std::string        line;
    const std::regex   feed(R"(^G1 .*F([0-9.]+))");
    const std::regex   extrude(R"(^G1 .*[XY][-0-9.]+.* E([0-9.]+))");
    double             f = 0, highest = 0;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, feed))
            f = std::stod(m[1].str());
        if (std::regex_search(line, m, extrude) && std::stod(m[1].str()) > 0)
            highest = std::max(highest, f);
    }
    return highest;
}

std::string without_comments(const std::string& gcode)
{
    std::istringstream in(gcode);
    std::string        line, out;
    while (std::getline(in, line))
        if (!line.empty() && line[0] != ';')
            out += line.substr(0, line.find(';')) + "\n";
    return out;
}

} // namespace

TEST_CASE("An object's retraction override is used, and the filament's retraction stays for the other object", "[ForcaObjectFilament]")
{
    const std::string plain = slice_with_object_overrides({ cube(20), cube(20) }, base_config(), { {}, {} });
    const std::string gcode = slice_with_object_overrides({ cube(20), cube(20) }, base_config(),
                                                          { { { "object_retraction_length", "3.5" } }, {} });
    // Without the override only the filament's 0.8 mm retraction is used.
    REQUIRE(retraction_lengths(plain) == std::set<double>{ 0.8 });
    // With it, the object's 3.5 mm appears and the other object still retracts the filament's 0.8 mm.
    const std::set<double> lengths = retraction_lengths(gcode);
    CHECK(lengths.count(3.5) == 1);
    CHECK(lengths.count(0.8) == 1);
}

TEST_CASE("An object's pressure advance override is set for it and the filament's restored after it", "[ForcaObjectFilament]")
{
    const std::string gcode = slice_with_object_overrides({ cube(20), cube(20) }, base_config(),
                                                          { { { "object_pressure_advance", "0.05" } }, {} });
    const size_t override_at = gcode.find("SET_PRESSURE_ADVANCE ADVANCE=0.05");
    REQUIRE(override_at != std::string::npos);
    CHECK(gcode.find("SET_PRESSURE_ADVANCE ADVANCE=0.02", override_at) != std::string::npos);
}

TEST_CASE("An object's max volumetric speed override slows its fastest extrusions", "[ForcaObjectFilament]")
{
    // The filament allows 20 mm³/s; the object override allows 2, well below what the default speeds need.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "filament_max_volumetric_speed", "20" } });
    const std::string plain  = slice_with_object_overrides({ cube(20) }, config, { {} });
    const std::string capped = slice_with_object_overrides({ cube(20) }, config, { { { "object_max_volumetric_speed", "2" } } });
    CHECK(max_extrusion_feedrate(capped) < max_extrusion_feedrate(plain));
}

namespace {

DynamicPrintConfig sequential_config(const char* sequence)
{
    // The filament prints at 210 °C (215 °C on the first layer).
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "print_sequence", sequence },
                                    { "nozzle_temperature", "210" },
                                    { "nozzle_temperature_initial_layer", "215" } });
    return config;
}

// The part cooling fan values (0-255) the G-code sets with M106.
std::set<int> fan_values(const std::string& gcode)
{
    std::set<int>      out;
    std::istringstream in(gcode);
    std::string        line;
    const std::regex   m106(R"(^M106 (?:P\d+ )?S([0-9.]+))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, m106))
            out.insert(int(std::stod(m[1].str()) + 0.5));
    }
    return out;
}

} // namespace

TEST_CASE("Printing by object, an object's nozzle temperatures are waited for and then used", "[ForcaObjectFilament]")
{
    const std::string gcode = slice_with_object_overrides({ cube(20), cube(20) }, sequential_config("by object"),
        { { { "object_nozzle_temperature", "230" }, { "object_nozzle_temperature_initial_layer", "235" } }, {} });
    // The object's first layer is waited for at its own 235 °C, its later layers print at 230 °C, and the other object
    // goes back to the filament's 215 °C first layer.
    CHECK(gcode.find("M109 S235") != std::string::npos);
    CHECK(gcode.find("M104 S230") != std::string::npos);
    CHECK(gcode.find("S215") != std::string::npos);
}

TEST_CASE("Printing by object, an object's fan overrides set its fan speed", "[ForcaObjectFilament]")
{
    // 40 % of the fan's 0-255 range is 102.
    const std::string gcode = slice_with_object_overrides({ cube(20), cube(20) }, sequential_config("by object"),
        { { { "object_fan_min_speed", "40" }, { "object_fan_max_speed", "40" } }, {} });
    CHECK(fan_values(gcode).count(102) == 1);
}

TEST_CASE("Printing by layer, fan and nozzle temperature overrides change nothing and are reported", "[ForcaObjectFilament]")
{
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides = {
        { { "object_nozzle_temperature", "230" }, { "object_fan_max_speed", "40" } }, {} };
    const std::string plain = slice_with_object_overrides({ cube(20), cube(20) }, sequential_config("by layer"), { {}, {} });
    const std::string unset = slice_with_object_overrides({ cube(20), cube(20) }, sequential_config("by layer"), overrides);
    CHECK(without_comments(plain) == without_comments(unset));

    Print print;
    Model model;
    init_print({ cube(20), cube(20) }, print, model, sequential_config("by layer"), &overrides);
    std::vector<StringObjectException> warnings;
    print.validate(&warnings);
    const bool reported = std::any_of(warnings.begin(), warnings.end(), [](const StringObjectException& w) {
        return w.opt_key == "object_nozzle_temperature";
    });
    CHECK(reported);
}

TEST_CASE("Filament overrides left at -1 change nothing in the G-code", "[ForcaObjectFilament]")
{
    const std::string plain = slice_with_object_overrides({ cube(20), cube(20) }, base_config(), { {}, {} });
    const std::string unset = slice_with_object_overrides({ cube(20), cube(20) }, base_config(),
                                                          { { { "object_retraction_length", "-1" },
                                                              { "object_retraction_speed", "-1" },
                                                              { "object_deretraction_speed", "-1" },
                                                              { "object_z_hop", "-1" },
                                                              { "object_pressure_advance", "-1" },
                                                              { "object_max_volumetric_speed", "-1" },
                                                              { "object_nozzle_temperature", "-1" },
                                                              { "object_nozzle_temperature_initial_layer", "-1" },
                                                              { "object_fan_min_speed", "-1" },
                                                              { "object_fan_max_speed", "-1" },
                                                              { "object_overhang_fan_speed", "-1" } },
                                                            {} });
    CHECK(without_comments(plain) == without_comments(unset));
    CHECK(plain.find("ADVANCE=0.05") == std::string::npos);
}
