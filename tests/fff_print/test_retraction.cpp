#include <catch2/catch_all.hpp>

#include "test_helpers.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Retractions in the G-code of a 20 mm cube with sparse infill, sliced with the given infill retraction mode and
// filament metal stickiness.
size_t retractions(const char *mode, FilamentMetalStickiness stickiness)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "reduce_infill_retraction_mode", mode },
        { "sparse_infill_density",         "20%" },
        { "sparse_infill_pattern",         "grid" },
    });
    // Enum lists copied from full_print_config() carry no names map, so they are set by value.
    config.set_key_value("filament_metal_stickiness", new ConfigOptionEnumsGeneric{ int(stickiness) });
    size_t count = 0;
    GCodeReader parser;
    parser.parse_buffer(slice({ cube(20) }, config), [&count](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.retracting(self))
            ++count;
    });
    return count;
}

} // namespace

TEST_CASE("Reduce infill retraction skips retractions only where its mode allows", "[Retraction]")
{
    const size_t disabled = retractions("Disabled", fmsNone);
    const size_t enabled  = retractions("Enabled", fmsHigh);
    REQUIRE(enabled < disabled);
    // Auto skips them for filaments with low or unknown metal stickiness ...
    CHECK(retractions("Auto", fmsNone) == enabled);
    CHECK(retractions("Auto", fmsLow) == enabled);
    // ... and keeps them for sticky filaments.
    CHECK(retractions("Auto", fmsMedium) == disabled);
    CHECK(retractions("Auto", fmsHigh) == disabled);
}
