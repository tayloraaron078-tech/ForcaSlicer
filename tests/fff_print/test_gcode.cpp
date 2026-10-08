#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_helpers.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Point.hpp"
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("Klipper object labels name each copy without the characters Klipper cannot parse", "[GCode]")
{
    const auto [name, label] = GENERATE(table<std::string, std::string>({
        {"my part (2)", "my_part_2"},
        {"(cube)", "cube"},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"exclude_object", "1"}});
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.)}, print, model, config, nullptr, false, 2);
    model.objects.front()->name = name;
    arrange_objects(model, BoundingBox{Point::new_scale(0., 0.), Point::new_scale(500., 500.)},
                    ArrangeParams{scaled(min_object_distance(config))});
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);
    for (const char *copy : {"0", "1"}) {
        const std::string instance_label = label + "_id_0_copy_" + copy;
        INFO(instance_label);
        CHECK(gcode.find("EXCLUDE_OBJECT_DEFINE NAME=" + instance_label + " ") != std::string::npos);
        CHECK(gcode.find("EXCLUDE_OBJECT_START NAME=" + instance_label + "\n") != std::string::npos);
    }
}

TEST_CASE("A filament can override the process bridge speed", "[GCode]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bridge_speed",                "13" },
        { "slow_down_for_layer_cooling", "0" },
    });
    config.set_key_value("filament_bridge_speed", new ConfigOptionFloatsNullable{7.5});
    // 13 and 7.5 mm/s are written as F780 and F450.
    const auto has_feedrate = [](const std::string &gcode, const std::string &feedrate) {
        std::istringstream input(gcode);
        std::string word;
        while (input >> word)
            if (word == feedrate)
                return true;
        return false;
    };

    const std::string process_gcode = Test::slice({ Test::TestMesh::bridge }, config);
    CHECK(has_feedrate(process_gcode, "F780"));
    CHECK_FALSE(has_feedrate(process_gcode, "F450"));

    config.set_key_value("override_process_overhang_speed", new ConfigOptionBoolsNullable{true});
    const std::string filament_gcode = Test::slice({ Test::TestMesh::bridge }, config);
    CHECK(has_feedrate(filament_gcode, "F450"));
    CHECK_FALSE(has_feedrate(filament_gcode, "F780"));
}

TEST_CASE("Short travels to an outer wall can have their own acceleration", "[GCode]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor",               "marlin2" },
        { "default_acceleration",       "1000" },
        { "outer_wall_acceleration",    "700" },
        { "travel_acceleration",        "2000" },
        { "wall_loops",                 "2" },
        { "retraction_minimum_travel",  "2" },
    });
    // Whether an acceleration command sets `value`.
    const auto sets_acceleration = [](const std::string &gcode, const std::string &value) {
        std::istringstream input(gcode);
        std::string line;
        while (std::getline(input, line))
            if (line.rfind("M204", 0) == 0 && line.find(value) != std::string::npos)
                return true;
        return false;
    };

    const std::string default_gcode = Test::slice({ Test::cube(20) }, config);
    CHECK_FALSE(sets_acceleration(default_gcode, "333"));

    config.set_key_value("travel_short_distance_acceleration", new ConfigOptionFloatsNullable{333.});
    const std::string short_travel_gcode = Test::slice({ Test::cube(20) }, config);
    CHECK(sets_acceleration(short_travel_gcode, "333"));
}

TEST_CASE("Slowing down by height caps speed and acceleration above the starting height", "[GCode]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor",                "marlin2" },
        { "default_acceleration",        "1000" },
        { "slow_down_for_layer_cooling", "0" },
        { "layer_height",                "0.3" },
        { "initial_layer_print_height",  "0.3" },
    });
    // A 20 mm cube ends 10 mm above the ending height, so its top layers print at the ending values:
    // 7 mm/s (F420) and 333 mm/s2.
    config.set_key_value("slowdown_start_height", new ConfigOptionFloatsNullable{5.});
    config.set_key_value("slowdown_end_height",   new ConfigOptionFloatsNullable{10.});
    config.set_key_value("slowdown_end_speed",    new ConfigOptionFloatsNullable{7.});
    config.set_key_value("slowdown_end_acc",      new ConfigOptionFloatsNullable{333.});
    const auto has_line = [](const std::string &gcode, const std::string &start, const std::string &word) {
        std::istringstream input(gcode);
        std::string line;
        while (std::getline(input, line))
            if (line.rfind(start, 0) == 0 && line.find(word) != std::string::npos)
                return true;
        return false;
    };

    const std::string off_gcode = Test::slice({ Test::cube(20) }, config);
    CHECK_FALSE(has_line(off_gcode, "G1", "F420"));
    CHECK_FALSE(has_line(off_gcode, "M204", "333"));

    config.set_key_value("enable_height_slowdown", new ConfigOptionBoolsNullable{true});
    const std::string on_gcode = Test::slice({ Test::cube(20) }, config);
    CHECK(has_line(on_gcode, "G1", "F420"));
    CHECK(has_line(on_gcode, "M204", "333"));
}

TEST_CASE("A pause above the top layer is not emitted", "[GCode]")
{
    // A 20 mm cube at 0.2 mm layers tops out at 20 mm. A pause at or below that fires; one above it is a leftover
    // from a taller model that the layer slider hides, so it must not fire on the last layer.
    const auto [pause_z, emitted] = GENERATE(table<double, bool>({
        {10., true},
        {20., true},
        {30., false},
    }));
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "machine_pause_gcode",        "M601 ; test pause" },
    });
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{Test::cube(20.)}, print, model, config);
    model.plates_custom_gcodes[model.curr_plate_index].gcodes = {{pause_z, CustomGCode::PausePrint, 1, "", ""}};
    print.apply(model, config);

    INFO("pause at " << pause_z << " mm");
    // Match the emitted line, not the settings dump at the end of the file.
    CHECK((Test::gcode(print).find("\nM601 ; test pause") != std::string::npos) == emitted);
}
