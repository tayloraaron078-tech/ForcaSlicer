#include "ForcaPreflight.hpp"

#include "Config.hpp"
#include "PrintConfig.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

namespace {

// The serialized value of `key`; for a list, its element `idx` (the last one past the end).
bool serialized_value(const DynamicPrintConfig& config, const std::string& key, size_t idx, std::string& out)
{
    const ConfigOption* opt = config.option(key);
    if (opt == nullptr)
        return false;
    if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt)) {
        const std::vector<std::string> values = vec->vserialize();
        if (values.empty())
            return false;
        out = values[idx < values.size() ? idx : values.size() - 1];
    } else {
        out = opt->serialize();
    }
    return true;
}

void append(std::vector<ForcaPreflightValue>& out, const DynamicPrintConfig& config, const std::string& key, size_t idx)
{
    std::string value;
    if (serialized_value(config, key, idx, value))
        out.push_back({ key, value, {} });
}

} // namespace

std::vector<ForcaPreflightValue> forca_preflight_changes(const DynamicPrintConfig& saved, const DynamicPrintConfig& edited,
                                                         const std::vector<std::string>& keys)
{
    std::vector<ForcaPreflightValue> out;
    for (const std::string& key : keys) {
        const ConfigOption* before = saved.option(key);
        const ConfigOption* after  = edited.option(key);
        if (before == nullptr || after == nullptr)
            continue;
        const std::string a = after->serialize();
        const std::string b = before->serialize();
        if (a != b)
            out.push_back({ key, a, b });
    }
    return out;
}

std::vector<ForcaPreflightValue> forca_preflight_process_details(const DynamicPrintConfig& full)
{
    std::vector<ForcaPreflightValue> out;
    for (const char* key : { "layer_height", "sparse_infill_pattern", "sparse_infill_density", "enable_support", "support_type",
                             "brim_type", "nozzle_diameter" })
        append(out, full, key, 0);
    return out;
}

std::vector<ForcaPreflightValue> forca_preflight_filament_details(const DynamicPrintConfig& full, size_t idx)
{
    std::vector<ForcaPreflightValue> out;
    append(out, full, "nozzle_temperature_initial_layer", idx);
    append(out, full, "nozzle_temperature", idx);
    if (const auto* bed = full.option<ConfigOptionEnum<BedType>>("curr_bed_type"))
        append(out, full, get_bed_temp_key(bed->value), idx);
    append(out, full, "filament_max_volumetric_speed", idx);
    return out;
}

} // namespace Slic3r
