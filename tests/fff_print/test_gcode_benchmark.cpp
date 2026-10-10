#include <catch2/catch_all.hpp>

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include "libslic3r/GCodeReader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// Measures slicing and G-code export on a fixed set of models and prints the export time, travel distance,
// retraction count and estimated print time, to compare tool path and G-code generation changes.
// Hidden: it asserts nothing and takes a while. Run it with `fff_print_tests "[Benchmark]"`; set BENCH_PRINT_ORDER
// (e.g. as_obj_list) to measure another print order, BENCH_CONFIG ("key=value;key=value") for other settings.
// The hash identifies the G-code, ignoring comments.
TEST_CASE("G-code generation benchmark", "[Benchmark][.]")
{
    struct Case {
        const char                                   *name;
        std::function<void(Print&, Model&)>           init;
    };
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "reduce_crossing_wall",        true },
        { "slow_down_for_layer_cooling", true },
        { "sparse_infill_density",       "15%" },
    });
    if (const char *order = std::getenv("BENCH_PRINT_ORDER"))
        config.set_deserialize_strict({ { "print_order", order } });
    // BENCH_CONFIG: further settings as "key=value;key=value".
    if (const char *extra = std::getenv("BENCH_CONFIG"))
        for (const std::string &item : [](std::string list) {
                 std::vector<std::string> items;
                 for (size_t start = 0, end; start <= list.size(); start = end + 1) {
                     end = std::min(list.find(';', start), list.size());
                     if (end > start) items.emplace_back(list.substr(start, end - start));
                 }
                 return items;
             }(extra)) {
            const size_t eq = item.find('=');
            config.set_deserialize_strict(item.substr(0, eq), item.substr(eq + 1));
        }
    const auto with_instances = [&config](const char *obj, size_t count) {
        return [&config, obj, count](Print &print, Model &model) {
            ModelObject *object = model.add_object();
            object->name = obj;
            object->add_volume(load_model(obj));
            for (size_t i = 0; i < count; ++ i)
                object->add_instance()->set_offset(Vec3d(30. + 45. * double(i % 4), 30. + 45. * double(i / 4), 0.));
            object->ensure_on_bed();
            print.auto_assign_extruders(object);
            print.apply(model, config);
            print.validate();
        };
    };
    // One object of 16 separate pillars of different heights, so every layer has many islands to order.
    const auto pillars = [&config](Print &print, Model &model) {
        TriangleMesh mesh;
        for (int i = 0; i < 16; ++ i) {
            TriangleMesh pillar = make_cube(8., 8., 10. + 2. * double(i));
            pillar.translate(float(15 * (i % 4) + 3 * (i / 4 % 2)), float(15 * (i / 4)), 0.f);
            mesh.merge(pillar);
        }
        init_print({ std::move(mesh) }, print, model, config);
    };
    const Case cases[] = {
        { "16 pillars",           pillars },
        { "extruder_idler",       [&config](Print &print, Model &model) { init_print({ load_model("extruder_idler.obj") }, print, model, config); } },
        { "frog_legs",            [&config](Print &print, Model &model) { init_print({ load_model("frog_legs.obj") }, print, model, config); } },
        { "ipadstand",            [&config](Print &print, Model &model) { init_print({ TestMesh::ipadstand }, print, model, config); } },
        { "sphere_50mm",          [&config](Print &print, Model &model) { init_print({ TestMesh::sphere_50mm }, print, model, config); } },
        { "8x extruder_idler",    with_instances("extruder_idler.obj", 8) },
    };

    for (const Case &c : cases) {
        Print print;
        Model model;
        c.init(print, model);
        print.set_status_silent();

        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        print.process();
        const auto t1 = clock::now();
        ScopedTemporaryFile temp(".gcode");
        print.export_gcode(temp.string(), nullptr, nullptr);
        const auto t2 = clock::now();

        std::ifstream file(temp.string());
        const std::string gcode((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        // Hash of the commands only: comments carry the generation date.
        std::string commands;
        for (size_t start = 0, end; start < gcode.size(); start = end + 1) {
            end = std::min(gcode.find('\n', start), gcode.size());
            if (gcode[start] != ';')
                commands.append(gcode, start, end - start + 1);
        }
        double travel = 0.;
        size_t retractions = 0;
        GCodeReader parser;
        parser.parse_buffer(gcode, [&travel, &retractions](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.cmd_is("G0") || line.cmd_is("G1")) {
                if (line.retracting(self))
                    ++ retractions;
                else if (! line.extruding(self))
                    travel += line.dist_XY(self);
            }
        });
        std::string estimate = "?";
        if (size_t pos = gcode.find("; estimated printing time (normal mode) = "); pos != std::string::npos)
            estimate = gcode.substr(pos + 42, gcode.find('\n', pos) - pos - 42);

        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::printf("BENCH %-20s slice %8.0f ms  export %8.0f ms  travel %9.1f mm  retractions %6zu  estimate %-12s  hash %016zx\n",
                    c.name, ms(t1 - t0), ms(t2 - t1), travel, retractions, estimate.c_str(), std::hash<std::string>{}(commands));
    }
}
