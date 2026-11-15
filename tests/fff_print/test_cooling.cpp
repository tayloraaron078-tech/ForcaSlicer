#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

#include "libslic3r/GCodeReader.hpp"
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
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

TEST_CASE("Arc fitting remains active with overhang fan control", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_arc_fitting",             "enabled" },
        { "enable_overhang_bridge_fan",     true },
        { "enable_overhang_speed",          true },
        { "initial_layer_print_height",     0.2 },
        { "layer_height",                   0.2 },
        { "slow_down_for_layer_cooling",    false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    config.set_key_value("overhang_fan_threshold", new ConfigOptionEnumsGeneric{Overhang_threshold_2_4});
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER=[layer_num]"});

    const auto has_arc_after_first_layer = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line;
        size_t layers_seen = 0;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER=", 0) == 0)
                ++layers_seen;
            else if (layers_seen > 1 && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };
    const auto has_arc_with_overhang_fan = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line;
        bool overhang_fan_active = false;
        bool outer_wall = false;
        while (std::getline(input, line)) {
            if (line.rfind("M106 S255", 0) == 0)
                overhang_fan_active = true;
            else if (line.rfind("M106 S0", 0) == 0 || line.rfind("M107", 0) == 0)
                overhang_fan_active = false;
            else if (line.rfind(";TYPE:", 0) == 0)
                outer_wall = line == ";TYPE:Outer wall";
            else if (overhang_fan_active && outer_wall && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };

    const std::string with_overhang_speed = slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_after_first_layer(with_overhang_speed));

    config.set_deserialize_strict({ { "enable_overhang_speed", false } });
    const std::string without_overhang_speed = slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_after_first_layer(without_overhang_speed));

    const std::string uniform_overhang_gcode = slice({ make_sphere(50.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_with_overhang_fan(uniform_overhang_gcode));
}

TEST_CASE("Overhang fan transitions do not depend on overhang speed", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bridge_speed",                   2.0 },
        { "enable_arc_fitting",             "disabled" },
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

// Forca: layer-time smoothing. A 20 mm block (layers 0-9), a 2 mm neck slowed down for its minimum layer time (10-19),
// then the block again (20-39), at 0.2 mm. Without slowdown recovery the first wide layer after the neck is back at
// full speed at once; with 5 recovery layers it starts slower and is at full speed again 5 layers later.
TEST_CASE("Slowdown recovery brings the speed back over the set number of layers", "[Cooling]")
{
    TriangleMesh model = make_cube(20., 20., 2.);
    TriangleMesh neck  = make_cube(2., 2., 2.);
    neck.translate(9.f, 9.f, 2.f);
    TriangleMesh top = make_cube(20., 20., 4.);
    top.translate(0.f, 0.f, 4.f);
    model.merge(neck);
    model.merge(top);

    // The time each layer spends extruding (seconds): a slowdown stretches it.
    const auto time_per_layer = [&model](int recovery_layers) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "layer_height",                0.2 },
            { "initial_layer_print_height",  0.2 },
            { "skirt_loops",                 0 },
            { "slow_down_for_layer_cooling", true },
            { "slow_down_layer_time",        "8" },
            { "slow_down_min_speed",         "5" },
        });
        config.set_key_value("slow_down_recovery_layers", new ConfigOptionInts{ recovery_layers });
        std::vector<double> times;
        GCodeReader         parser;
        parser.parse_buffer(slice({ model }, config), [&times](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            if (line.comment() == "LAYER_CHANGE" || line.comment() == " CHANGE_LAYER")
                times.push_back(0.);
            else if (const double feedrate = line.has_f() ? line.f() : reader.f();
                     !times.empty() && line.extruding(reader) && feedrate > 0.)
                times.back() += line.dist_XY(reader) / (feedrate / 60.);
        });
        return times;
    };

    const std::vector<double> at_once = time_per_layer(0);
    const std::vector<double> faded   = time_per_layer(5);
    REQUIRE(at_once.size() == 40);
    REQUIRE(faded.size() == 40);
    CAPTURE(at_once, faded);
    // Without recovery, the solid layers right above the neck already print at their own speed (21 and 22 alike).
    CHECK_THAT(at_once[21], Catch::Matchers::WithinRel(at_once[22], 0.01));
    // With 5 recovery layers (20-24) they start slower and get faster layer by layer, and from layer 24 on they print
    // as without recovery.
    CHECK(faded[20] > at_once[20] * 1.5);
    CHECK(faded[20] > faded[21]);
    CHECK(faded[21] > faded[22]);
    CHECK(faded[22] > at_once[22]);
    CHECK_THAT(faded[24], Catch::Matchers::WithinRel(at_once[24], 0.01));
    CHECK_THAT(faded[30], Catch::Matchers::WithinRel(at_once[30], 0.01));
}

// On a toolchanger every tool carries its own part fan, and a fan command reaches the tool that is active when it
// arrives. So each tool select has to be followed by the fan speed before the new tool prints, even when both
// filaments ask for the same speed.
TEST_CASE("A toolchanger sets the fan after every tool select", "[Cooling]")
{
    const bool prime_tower = GENERATE(false, true);
    DYNAMIC_SECTION((prime_tower ? "with a prime tower" : "without a prime tower")) {
        DynamicPrintConfig config = multifilament_config(2, {
            { "nozzle_diameter", "0.4,0.4" },
            { "printer_extruder_id", "1,2" },
            { "printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard" },
            { "extruder_printable_height", "0,0" },
            { "filament_self_index", "1,2" },
            { "single_extruder_multi_material", 0 },
            { "enable_prime_tower", prime_tower ? 1 : 0 },
            { "wipe_tower_x", "150" }, // inside the 200x200 test bed
            { "wipe_tower_y", "150" },
            { "gcode_flavor", "klipper" },
            // The same constant fan on both filaments from the second layer on.
            { "close_fan_the_first_x_layers", "1,1" },
            { "fan_min_speed", "100,100" },
            { "fan_max_speed", "100,100" },
            { "skirt_loops", 0 },
            { "brim_type", "no_brim" },
        });
        size_flush_to_nozzles(config);

        // Two cubes, one per tool: every layer changes tools.
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", 1 } }, { { "extruder", 2 } } };
        Print print;
        Model model;
        init_print({ cube(10), cube(10) }, print, model, config, &overrides);
        const std::string gcode = Test::gcode(print);

        int                selects = 0, without_fan = 0;
        bool               fan_on = false, waiting = false;
        std::istringstream stream(gcode);
        for (std::string line; std::getline(stream, line);) {
            if (line.rfind("M106 S", 0) == 0) {
                fan_on  = fan_on || line.rfind("M106 S0", 0) != 0;
                waiting = false;
            } else if (!fan_on) {
                continue; // the first layer prints with the fan off
            } else if (line.size() >= 2 && line[0] == 'T' && (line[1] == '0' || line[1] == '1')) {
                ++selects;
                waiting = true;
            } else if (waiting && line.rfind("G1 ", 0) == 0 && line.find(" X") != std::string::npos) {
                const size_t e = line.find(" E");
                if (e != std::string::npos && line[e + 2] != '-') { // the new tool extrudes before any fan command
                    ++without_fan;
                    waiting = false;
                }
            }
        }
        // A 10 mm cube at the default 0.2 mm layer height: 50 layers, a tool select on each.
        REQUIRE(selects >= 40);
        CHECK(without_fan == 0);
    }
}

namespace {

// What the cooling pass made of each layer of two 10 mm cubes on a two-tool toolchanger.
struct TwoCubeLayers
{
    // Seconds per layer, from the layer's first extrusion on (length / feedrate, as the cooling pass counts it).
    std::vector<double> times;
    // The part fan commands (M106 S values) of each layer.
    std::vector<std::vector<int>> fans;
    // Tool selects per layer.
    std::vector<int> selects;
    int tool_selects = 0;
};

// The second cube prints with `second_tool` (1 = the same tool as the first cube, 2 = the other tool).
TwoCubeLayers slice_two_cubes(int second_tool, std::initializer_list<ConfigBase::SetDeserializeItem> cooling)
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "nozzle_diameter", "0.4,0.4" },
        { "printer_extruder_id", "1,2" },
        { "printer_extruder_variant", "Direct Drive Standard,Direct Drive Standard" },
        { "extruder_printable_height", "0,0" },
        { "filament_self_index", "1,2" },
        { "single_extruder_multi_material", 0 },
        { "enable_prime_tower", 0 },
        { "gcode_flavor", "klipper" },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        // The cooling pass times a layer with the G-code processor (acceleration, cornering). Limits far beyond the
        // moves of these cubes make that time length / feedrate, which the parser below measures.
        { "default_acceleration", "0" },
        { "machine_max_acceleration_extruding", "1000000,1000000" },
        { "machine_max_acceleration_retracting", "1000000,1000000" },
        { "machine_max_acceleration_travel", "1000000,1000000" },
        { "machine_max_acceleration_x", "1000000,1000000" },
        { "machine_max_acceleration_y", "1000000,1000000" },
        { "machine_max_acceleration_z", "1000000,1000000" },
        { "machine_max_acceleration_e", "1000000,1000000" },
        { "machine_max_jerk_x", "100000,100000" },
        { "machine_max_jerk_y", "100000,100000" },
        { "machine_max_jerk_z", "100000,100000" },
        { "machine_max_jerk_e", "100000,100000" },
        { "machine_max_speed_x", "100000,100000" },
        { "machine_max_speed_y", "100000,100000" },
        { "machine_max_speed_z", "100000,100000" },
        { "machine_max_speed_e", "100000,100000" },
        { "close_fan_the_first_x_layers", "1,1" },
    });
    config.set_deserialize_strict(cooling);
    size_flush_to_nozzles(config);

    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", 1 } }, { { "extruder", second_tool } } };
    Print print;
    Model model;
    init_print({ cube(10), cube(10) }, print, model, config, &overrides);

    TwoCubeLayers out;
    bool          extruded = false;
    GCodeReader   parser;
    parser.parse_buffer(Test::gcode(print), [&out, &extruded](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
        if (line.comment() == "LAYER_CHANGE" || line.comment() == " CHANGE_LAYER") {
            out.times.push_back(0.);
            out.fans.emplace_back();
            out.selects.push_back(0);
            extruded = false;
        } else if (out.times.empty()) {
            return;
        } else if (float speed = 0.f; line.cmd_is("M106") && line.has_value('S', speed)) {
            out.fans.back().push_back(int(speed));
        } else if (line.cmd() == "T0" || line.cmd() == "T1") {
            ++out.tool_selects;
            ++out.selects.back();
        } else if (const double feedrate = line.has_f() ? line.f() : reader.f(); feedrate > 0.) {
            extruded = extruded || line.extruding(reader);
            // A move's length is its XYZ distance, or the filament moved by a retraction or its undoing.
            const double xyz = std::hypot(double(line.dist_XY(reader)), double(line.dist_Z(reader)));
            if (extruded)
                out.times.back() += (xyz > 0. ? xyz : std::abs(double(line.dist_E(reader)))) / (feedrate / 60.);
        }
    });
    return out;
}

} // namespace

// The minimum layer time is a rule for the whole layer: a layer printed by two tools is slowed down until both
// tools' shares together reach it, exactly like the same layer printed by one tool.
TEST_CASE("A layer printed by two tools reaches the minimum layer time as a whole", "[Cooling]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> cooling{
        { "slow_down_for_layer_cooling", "1,1" },
        { "slow_down_layer_time", "40,40" },
        { "slow_down_min_speed", "1,1" },
    };
    const TwoCubeLayers one_tool  = slice_two_cubes(1, cooling);
    const TwoCubeLayers two_tools = slice_two_cubes(2, cooling);
    // 10 mm cubes at 0.2 mm layers: 50 layers, and with two tools a tool select on each.
    REQUIRE(one_tool.times.size() == 50);
    REQUIRE(two_tools.times.size() == 50);
    REQUIRE(two_tools.tool_selects >= 40);
    CAPTURE(one_tool.times, two_tools.times);
    for (size_t layer = 1; layer < 50; ++layer) {
        CHECK(one_tool.times[layer] > 40. * 0.99);
        CHECK(two_tools.times[layer] > 40. * 0.99);
        CHECK(one_tool.times[layer] < 40. * 1.01);
        CHECK(two_tools.times[layer] < 40. * 1.01);
    }
}

// While a toolchanger swaps tools the layer below keeps cooling, so the tool change time counts toward the minimum
// layer time: a layer with one tool select needs that much less time of its own moves.
TEST_CASE("The tool change time counts toward the minimum layer time", "[Cooling]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> cooling{
        { "slow_down_for_layer_cooling", "1,1" },
        { "slow_down_layer_time", "40,40" },
        { "slow_down_min_speed", "1,1" },
        { "machine_tool_change_time", 5 },
    };
    const TwoCubeLayers two_tools = slice_two_cubes(2, cooling);
    REQUIRE(two_tools.times.size() == 50);
    int checked = 0;
    for (size_t layer = 1; layer < 50; ++layer) {
        CAPTURE(layer, two_tools.times[layer], two_tools.selects[layer]);
        // Only a select after the layer's first extrusion falls in the layer's counted time.
        if (two_tools.selects[layer] != 1)
            continue;
        ++checked;
        const double expected = 40. - 5.;
        CHECK_THAT(two_tools.times[layer], Catch::Matchers::WithinRel(expected, 0.01));
    }
    CHECK(checked >= 40);
}

// The fan follows the time of the whole layer too: between the minimum layer time and the fan's layer time
// threshold, both tools of a two-tool layer get the speed the same layer gets when one tool prints it.
TEST_CASE("Both tools of a layer get the fan speed of the whole layer's time", "[Cooling]")
{
    const std::initializer_list<ConfigBase::SetDeserializeItem> cooling{
        { "slow_down_for_layer_cooling", "0,0" },
        { "slow_down_layer_time", "1,1" },
        { "fan_cooling_layer_time", "30,30" },
        { "fan_min_speed", "0,0" },
        { "fan_max_speed", "100,100" },
        { "overhang_fan_speed", "0,0" },
    };
    const TwoCubeLayers one_tool  = slice_two_cubes(1, cooling);
    const TwoCubeLayers two_tools = slice_two_cubes(2, cooling);
    REQUIRE(one_tool.fans.size() == 50);
    REQUIRE(two_tools.fans.size() == 50);
    int checked = 0;
    for (size_t layer = 5; layer < 45; ++layer) {
        // A layer with nothing but the tool change's pair of commands: the old tool's speed, repeated for the new tool.
        const std::vector<int> &fans = two_tools.fans[layer];
        if (fans.size() != 2)
            continue;
        ++checked;
        CAPTURE(layer, two_tools.times[layer]);
        CHECK(fans[0] == fans[1]);
        // Fan 100 % at the 1 s minimum layer time, falling to 0 % at the 30 s fan threshold; M106 S is 0-255.
        const double expected = 255. * (1. - (two_tools.times[layer] - 1.) / (30. - 1.));
        CHECK_THAT(double(fans[0]), Catch::Matchers::WithinAbs(expected, 6.));
        // A single tool's share is about half the layer, which would ask for a much faster fan.
        CHECK(two_tools.times[layer] > 8.);
    }
    CHECK(checked >= 20);
}

// One nozzle fed several filaments has one part fan, which keeps its speed across a filament change: the fan
// command a toolchanger needs after the select would only be noise there.
TEST_CASE("A single nozzle does not repeat the fan after a filament change", "[Cooling]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "enable_prime_tower", 0 },
        { "gcode_flavor", "klipper" },
        { "close_fan_the_first_x_layers", "1,1" },
        { "fan_min_speed", "100,100" },
        { "fan_max_speed", "100,100" },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
    });
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { { "extruder", 1 } }, { { "extruder", 2 } } };
    Print print;
    Model model;
    init_print({ cube(10), cube(10) }, print, model, config, &overrides);
    const std::string gcode = Test::gcode(print);

    int                selects = 0, fan_after_select = 0;
    bool               after_select = false;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.size() >= 2 && line[0] == 'T' && (line[1] == '0' || line[1] == '1')) {
            ++selects;
            after_select = true;
        } else if (after_select) {
            if (line.rfind("M106 S", 0) == 0)
                ++fan_after_select;
            after_select = false;
        }
    }
    REQUIRE(selects >= 40);
    CHECK(fan_after_select == 0);
}

// Speeds of the extruding moves, in mm/s, in print order.
static std::vector<double> extrusion_speeds(const std::string &gcode, const DynamicPrintConfig &config)
{
    std::vector<double> speeds;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(gcode, [&speeds](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.dist_XY(self) > 0.)
            speeds.push_back(line.new_F(self) / MM_PER_MIN);
    });
    return speeds;
}

// A line that only sets the feed rate takes no time, however the extrusion distances are written.
TEST_CASE("Layer cooling slowdown does not depend on relative extrusion distances", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        // High enough that the defaults print at their own speeds.
        { "filament_max_volumetric_speed", 100 },
        { "initial_layer_print_height",    0.3 },
        { "layer_height",                  0.3 },
        { "slow_down_for_layer_cooling",   true },
        // Longer than the cube's layers take, so they are slowed down.
        { "slow_down_layer_time",          10 },
        { "slow_down_min_speed",           1 },
    });
    config.set_deserialize_strict({ { "use_relative_e_distances", false } });
    const std::vector<double> absolute = extrusion_speeds(slice({ cube(20) }, config), config);
    config.set_deserialize_strict({ { "use_relative_e_distances", true } });
    const std::vector<double> relative = extrusion_speeds(slice({ cube(20) }, config), config);

    REQUIRE(relative.size() == absolute.size());
    double largest_difference = 0.;
    for (size_t i = 0; i < relative.size(); ++i)
        largest_difference = std::max(largest_difference, std::abs(relative[i] - absolute[i]));
    // Feed rates are written in whole mm/min.
    CHECK_THAT(largest_difference, Catch::Matchers::WithinAbs(0., 1. / MM_PER_MIN));
}

// Per layer: its time as the G-code preview estimates it, with acceleration, and the part cooling fan speed of its first
// extrusion, in percent.
struct EstimatedLayer
{
    double time = 0.;
    float  fan  = -1.f;
};

static std::vector<EstimatedLayer> estimated_layers(const TriangleMesh &mesh, const DynamicPrintConfig &config,
                                                    std::string *gcode_text = nullptr)
{
    Print print;
    Model model;
    init_print({ mesh }, print, model, config);
    GCodeProcessorResult result;
    std::string text = gcode(print, &result);
    if (gcode_text)
        *gcode_text = std::move(text);
    std::vector<EstimatedLayer> layers;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.layer_id >= layers.size())
            layers.resize(move.layer_id + 1);
        EstimatedLayer &layer = layers[move.layer_id];
        layer.time += move.time[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
        if (layer.fan < 0.f && move.type == EMoveType::Extrude)
            layer.fan = move.fan_speed;
    }
    return layers;
}

// Print speeds that a low acceleration never reaches on a small part, so its layers take several times their time at the feed rates.
static DynamicPrintConfig slowly_accelerating_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "filament_max_volumetric_speed",      100 },
        { "initial_layer_print_height",         0.3 },
        { "inner_wall_speed",                   200 },
        { "internal_solid_infill_speed",        200 },
        { "layer_height",                       0.3 },
        { "machine_max_acceleration_extruding", "300,300" },
        { "outer_wall_speed",                   200 },
        { "sparse_infill_speed",                200 },
        { "top_surface_speed",                  200 },
    });
    return config;
}

TEST_CASE("Layer cooling slowdown reaches the minimum layer time with acceleration", "[Cooling]")
{
    // A little longer than most layers of the cube take, so they slow down just a little and still accelerate a lot.
    constexpr double minimum_layer_time = 8.;
    DynamicPrintConfig config = slowly_accelerating_config();
    config.set_deserialize_strict({
        { "slow_down_for_layer_cooling", false },
        { "slow_down_layer_time",        minimum_layer_time },
        { "slow_down_min_speed",         1 },
    });
    const std::vector<EstimatedLayer> unslowed = estimated_layers(cube(10), config);
    config.set_deserialize_strict({ { "slow_down_for_layer_cooling", true } });
    const std::vector<EstimatedLayer> slowed = estimated_layers(cube(10), config);
    REQUIRE(slowed.size() == unslowed.size());

    // Layer 0 holds the start G-code, and the last layer the end G-code.
    const size_t quick = std::count_if(unslowed.begin() + 1, unslowed.end() - 1,
                                       [](const EstimatedLayer &layer) { return layer.time < minimum_layer_time; });
    REQUIRE(quick > 10);
    for (size_t i = 1; i + 1 < slowed.size(); ++i) {
        if (unslowed[i].time < minimum_layer_time) {
            CHECK(slowed[i].time > 0.99 * minimum_layer_time);
            CHECK(slowed[i].time < 1.03 * minimum_layer_time);
        } else
            CHECK_THAT(slowed[i].time, Catch::Matchers::WithinRel(unslowed[i].time, 0.001));
    }
}

TEST_CASE("Fan speed follows the layer time with acceleration", "[Cooling]")
{
    constexpr double full_fan_time    = 2.;
    constexpr double minimum_fan_time = 100.;
    constexpr double full_fan         = 100.;
    constexpr double minimum_fan      = 20.;
    DynamicPrintConfig config = slowly_accelerating_config();
    config.set_deserialize_strict({
        { "close_fan_the_first_x_layers", 0 },
        { "fan_cooling_layer_time",       minimum_fan_time },
        { "fan_max_speed",                full_fan },
        { "fan_min_speed",                minimum_fan },
        { "full_fan_speed_layer",         0 },
        // A pause before the first extrusion of each layer, which counts as in the Preview.
        { "layer_change_gcode",           "G4 S3" },
        { "slow_down_for_layer_cooling",  false },
        { "slow_down_layer_time",         full_fan_time },
    });
    // The low acceleration is the machine limit, or the one the start G-code sets when the layers set none of their own.
    if (GENERATE(false, true))
        config.set_deserialize_strict({
            { "default_acceleration",               0 },
            { "machine_max_acceleration_extruding", "20000,20000" },
            { "machine_start_gcode",                "G90\n  M204 S300 ; indented\nG92 E0" },
        });
    const std::vector<EstimatedLayer> layers = estimated_layers(cube(20), config);
    REQUIRE(layers.size() > 4);
    for (size_t i = 1; i + 1 < layers.size(); ++i) {
        REQUIRE(layers[i].time > full_fan_time);
        REQUIRE(layers[i].time < minimum_fan_time);
        const double t = (layers[i].time - full_fan_time) / (minimum_fan_time - full_fan_time);
        // The cooling rounds the speed down to a whole percent, and M106 takes it in 1/255 steps.
        CHECK_THAT(layers[i].fan, Catch::Matchers::WithinAbs(t * minimum_fan + (1. - t) * full_fan, 1.5));
    }
}

// The speeds of the outer wall extrusions, in mm/s.
static std::vector<double> outer_wall_speeds(const std::string &gcode, const DynamicPrintConfig &config)
{
    std::vector<double> speeds;
    bool outer_wall = false;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(gcode, [&speeds, &outer_wall](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.rfind("TYPE:", 0) == 0)
            outer_wall = comment == "TYPE:Outer wall";
        if (outer_wall && line.extruding(self) && line.dist_XY(self) > 0.)
            speeds.push_back(line.new_F(self) / MM_PER_MIN);
    });
    return speeds;
}

TEST_CASE("Layer cooling slowdown keeps outer walls out of the resonance avoidance range", "[Cooling]")
{
    constexpr double min_speed = 40.;
    constexpr double max_speed = 90.;
    // With inner walls and infill to slow down instead, the outer walls stay at the top of the range. A lone outer
    // wall cannot pass its time on, so it drops to the bottom; its layers are quick, hence the shorter layer time.
    const bool   lone_wall          = GENERATE(false, true);
    const double held_speed         = lone_wall ? min_speed : max_speed;
    const double minimum_layer_time = lone_wall ? 2. : 10.;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bottom_shell_layers",           lone_wall ? 0 : 3 },
        { "dont_slow_down_outer_wall",     false },
        { "enable_arc_fitting",            "disabled" },
        { "enable_overhang_speed",         false },
        { "filament_max_volumetric_speed", 100 },
        { "initial_layer_print_height",    0.3 },
        // Below the range, so resonance avoidance leaves it alone.
        { "initial_layer_speed",           30 },
        { "layer_height",                  0.3 },
        { "max_resonance_avoidance_speed", max_speed },
        { "min_resonance_avoidance_speed", min_speed },
        { "outer_wall_speed",              200 },
        { "slow_down_for_layer_cooling",   true },
        { "slow_down_layer_time",          minimum_layer_time },
        { "slow_down_layers",              0 },
        { "slow_down_min_speed",           10 },
        { "sparse_infill_density",         lone_wall ? "0%" : "15%" },
        { "top_shell_layers",              lone_wall ? 0 : 3 },
        { "wall_loops",                    lone_wall ? 1 : 3 },
    });

    // A cone's layers shrink upwards, so the slowdown deepens through the range.
    std::string without_gcode;
    std::string with_gcode;
    config.set_deserialize_strict({ { "resonance_avoidance", false } });
    const std::vector<EstimatedLayer> without = estimated_layers(make_cone(15., 15.), config, &without_gcode);
    config.set_deserialize_strict({ { "resonance_avoidance", true } });
    const std::vector<EstimatedLayer> with = estimated_layers(make_cone(15., 15.), config, &with_gcode);

    std::vector<double> without_in_range;
    std::vector<double> with_in_range;
    bool                held = false;
    for (double speed : outer_wall_speeds(without_gcode, config))
        if (speed > min_speed + 0.5 && speed < max_speed - 0.5)
            without_in_range.push_back(speed);
    for (double speed : outer_wall_speeds(with_gcode, config)) {
        if (speed > min_speed + 0.5 && speed < max_speed - 0.5)
            with_in_range.push_back(speed);
        held |= std::abs(speed - held_speed) < 0.1;
    }
    REQUIRE_FALSE(without_in_range.empty());
    CHECK(with_in_range.empty());
    CHECK(held);

    // No layer gets shorter than the slowdown made it without resonance avoidance, within the slowdown's tolerance.
    REQUIRE(with.size() == without.size());
    for (size_t i = 0; i < with.size(); ++i)
        CHECK(with[i].time > 0.99 * std::min(without[i].time, minimum_layer_time));
}
