// Forca: height ranges (layer_config_ranges) that don't carry every key the GUI writes. A 3MF made by another tool can
// have a height range with only the settings it changes -- no "layer_height" or "extruder" -- and slicing it from the
// command line crashed reading the missing layer height. Such a range uses the object's layer height.
#include <catch2/catch_test_macros.hpp>

#include "test_helpers.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A 20 mm cube at 0.2 mm layers with one height range from 5 to 10 mm holding only `range_items`.
struct RangeTest
{
    Model model;
    Print print;

    explicit RangeTest(const std::vector<std::pair<std::string, std::string>>& range_items)
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });
        init_print({ cube(20) }, print, model, config, nullptr, false);
        DynamicPrintConfig range;
        for (const auto& [key, value] : range_items)
            range.set_deserialize_strict(key, value);
        model.objects.front()->layer_config_ranges[t_layer_height_range(5., 10.)].assign_config(std::move(range));
        print.apply(model, config);
        print.process();
    }

    const PrintObject& object() const { return *print.objects().front(); }
};

// Wall loops of the region that holds the layer's slices (a layer lists every region of the object).
int walls_printed(const Layer& layer)
{
    for (const LayerRegion* region : layer.regions())
        if (!region->slices.empty())
            return region->region().config().wall_loops.value;
    return -1;
}

} // namespace

TEST_CASE("A height range without a layer height slices at the object's layer height", "[ForcaHeightRanges]")
{
    RangeTest t({ { "wall_loops", "4" } }); // no "layer_height", no "extruder"
    // 20 mm at 0.2 mm, the range included.
    CHECK(t.object().layer_count() == 100);
    // The range's own setting still applies inside it, and only there.
    CHECK(walls_printed(*t.object().get_layer(35)) == 4); // z 7.2 mm
    CHECK(walls_printed(*t.object().get_layer(75)) != 4); // z 15.2 mm
}

TEST_CASE("A height range with a layer height but no extruder slices with that layer height", "[ForcaHeightRanges]")
{
    RangeTest t({ { "layer_height", "0.1" } }); // no "extruder"
    // 0-5 mm at 0.2 (25 layers), 5-10 mm at 0.1 (50), 10-20 mm at 0.2 (50).
    CHECK(t.object().layer_count() == 125);
}
