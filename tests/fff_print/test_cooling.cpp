#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

#include <algorithm>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// The fan is held off for the first close_fan_the_first_x_layers layers, so an explicit
// fan-off command is emitted.
TEST_CASE("Fan is held off for the initial layers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "cooling",                      true },
        { "close_fan_the_first_x_layers", 5 },
    });
    CHECK(gcode.find("M106 S0") != std::string::npos);
}

// The cooling pass resolves and strips its internal speed placeholders; none leak into
// the final G-code.
TEST_CASE("Cooling consumes its internal speed markers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, { { "layer_height", 0.2 } });
    CHECK(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
}

TEST_CASE("Overhang fan transitions do not depend on overhang speed", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bridge_speed",                   2.0 },
        { "enable_arc_fitting",             false },
        { "enable_overhang_bridge_fan",     true },
        { "enable_overhang_speed",          false },
        { "initial_layer_print_height",     0.3 },
        { "inner_wall_speed",              30.0 },
        { "layer_height",                   0.3 },
        { "outer_wall_speed",              30.0 },
        { "overhang_1_4_speed",            "30" },
        { "overhang_2_4_speed",            "29" },
        { "overhang_3_4_speed",             "6" },
        { "overhang_4_4_speed",             "3" },
        { "slow_down_for_layer_cooling",    false },
        { "slowdown_for_curled_perimeters", false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    config.set_key_value("overhang_fan_threshold", new ConfigOptionEnumsGeneric{Overhang_threshold_2_4});
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER_Z=[layer_z]"});

    const auto fan_commands = [](const std::string &gcode) {
        std::vector<std::pair<std::string, std::string>> commands;
        std::istringstream input(gcode);
        std::string layer;
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER_Z=", 0) == 0)
                layer = line;
            else if (!layer.empty() && (line.rfind("M106", 0) == 0 || line.rfind("M107", 0) == 0))
                commands.emplace_back(layer, line);
        }
        return commands;
    };
    const auto feedrates = [](const std::string &gcode) {
        std::vector<std::string> values;
        std::istringstream input(gcode);
        std::string word;
        while (input >> word)
            if (!word.empty() && word.front() == 'F')
                values.push_back(word);
        return values;
    };

    constexpr double sphere_radius = 50.0; // 100 mm diameter.
    const std::string without_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);
    config.set_deserialize_strict({{"enable_overhang_speed", true}});
    const std::string with_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);

    const auto without_speed_fan = fan_commands(without_speed_gcode);
    const auto with_speed_fan = fan_commands(with_speed_gcode);
    const auto without_speed_feedrates = feedrates(without_speed_gcode);
    const auto with_speed_feedrates = feedrates(with_speed_gcode);

    REQUIRE_FALSE(without_speed_fan.empty());
    REQUIRE(std::any_of(without_speed_fan.begin(), without_speed_fan.end(),
                        [](const auto &command) { return command.second.find("S255") != std::string::npos; }));
    REQUIRE(with_speed_feedrates != without_speed_feedrates);
    CHECK(with_speed_fan == without_speed_fan);
}

// A dwell given in milliseconds (G4 P) counts toward the layer time exactly like the same
// dwell given in seconds (G4 S), so both get the same cooling slowdown.
TEST_CASE("Dwell in milliseconds counts toward layer time", "[Cooling]")
{
    const auto slice_with_dwell = [](const std::string &dwell) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "slow_down_for_layer_cooling", true },
            { "slow_down_layer_time",        "30" },
            { "slow_down_min_speed",         "1" },
        });
        // Role changes happen after the first extrusion of a layer, so the dwell lands inside the timed part.
        config.set_key_value("change_extrusion_role_gcode", new ConfigOptionString{dwell});
        // The moves only: comments carry the generation time and object ids.
        std::istringstream input(slice({ cube(10) }, config));
        std::string        moves;
        for (std::string line; std::getline(input, line);)
            if (line.rfind("G1 ", 0) == 0)
                moves += line.substr(0, line.find(';')) + '\n';
        return moves;
    };
    const std::string moves_ms = slice_with_dwell("G4 P2000");
    const std::string moves_s  = slice_with_dwell("G4 S2");
    REQUIRE_FALSE(moves_ms.empty());
    CHECK(moves_ms == moves_s);
}

// Time spent accelerating counts toward the minimum layer time, so a layer of short moves on a slowly
// accelerating machine needs less slowdown than the plain length / feedrate estimate suggests.
TEST_CASE("Acceleration time reduces the cooling slowdown", "[Cooling]")
{
    const auto lowest_feedrate = [](double acceleration) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "slow_down_for_layer_cooling", true },
            { "slow_down_layer_time",        "20" },
            { "slow_down_min_speed",         "1" },
            { "default_acceleration",        "0" },
        });
        // Without acceleration commands in the G-code, the cooling buffer assumes the machine limit.
        // 0 disables the acceleration model.
        config.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{ acceleration });
        std::istringstream input(slice({ cube(5) }, config));
        double lowest = std::numeric_limits<double>::max();
        for (std::string line; std::getline(input, line);)
            if (size_t pos = line.find(" F"); line.rfind("G1 ", 0) == 0 && pos != std::string::npos)
                lowest = std::min(lowest, std::stod(line.substr(pos + 2)));
        return lowest;
    };
    CHECK(lowest_feedrate(200.) > lowest_feedrate(0.));
}

// A filament's pre-start fan time moves the overhang fan command ahead of the overhang.
TEST_CASE("The pre-start fan time starts the overhang fan earlier", "[Cooling]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_arc_fitting",          false },
        { "enable_overhang_bridge_fan",  true },
        { "layer_height",                0.3 },
        { "initial_layer_print_height",  0.3 },
        { "slow_down_for_layer_cooling", false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    // The first full-speed fan command, which only the overhangs of the sphere's lower half trigger.
    const auto first_overhang_fan = [](const std::string &gcode) { return gcode.find("M106 S255"); };

    const std::string gcode = slice({make_sphere(20., PI / 24.0)}, config);
    config.set_key_value("pre_start_fan_time", new ConfigOptionFloats{3.0});
    const std::string pre_start_gcode = slice({make_sphere(20., PI / 24.0)}, config);

    REQUIRE(first_overhang_fan(gcode) != std::string::npos);
    REQUIRE(first_overhang_fan(pre_start_gcode) != std::string::npos);
    CHECK(first_overhang_fan(pre_start_gcode) < first_overhang_fan(gcode));
}

// "Consistent surface" reaches the minimum layer time by slowing the other features down, keeping the outer walls at
// their speed, where "Uniform cooling" slows the outer walls down with everything else.
TEST_CASE("Consistent surface cooling keeps the outer wall speed", "[Cooling]")
{
    const auto slice_cube = [](CoolingSlowdownLogicType logic, double transition_distance) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "slow_down_for_layer_cooling", true },
            { "slow_down_layer_time",        "30" },
            { "slow_down_min_speed",         "0.5" },
            { "outer_wall_speed",            "77" },
        });
        // The default volumetric limit would cap the outer wall well below its speed.
        config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{ 50. });
        config.set_key_value("cooling_slowdown_logic", new ConfigOptionEnumsGeneric{ int(logic) });
        config.set_key_value("cooling_perimeter_transition_distance", new ConfigOptionFloats{ transition_distance });
        return slice({ cube(10) }, config);
    };
    const auto count_lines = [](const std::string &gcode, const std::string &start, const std::string &word) {
        std::istringstream input(gcode);
        size_t count = 0;
        for (std::string line; std::getline(input, line);)
            if (line.rfind(start, 0) == 0 && line.find(word) != std::string::npos)
                ++ count;
        return count;
    };

    // 77 mm/s is written as F4620.
    CHECK(count_lines(slice_cube(cslUniformCooling, 0.), "G1", "F4620") == 0);
    CHECK(count_lines(slice_cube(cslConsistentSurface, 0.), "G1", "F4620") > 0);
    // The transition distance adds a feedrate command near the end of slowed down extrusions.
    CHECK(count_lines(slice_cube(cslConsistentSurface, 10.), "G1 F", "") >
          count_lines(slice_cube(cslConsistentSurface, 0.), "G1 F", ""));
}
