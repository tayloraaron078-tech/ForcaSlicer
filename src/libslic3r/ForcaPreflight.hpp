#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

// Forca: the facts behind the send dialogs' "Before you print" block, from configs only (no GUI; unit-tested).
// Values are serialized as the config stores them; the GUI turns keys into labels and enum values into names.

struct ForcaPreflightValue
{
    std::string key;   // config option key
    std::string value; // the value that will print
    std::string saved; // for a change: the saved preset's value; empty otherwise
};

// The options among `keys` whose value in `edited` differs from `saved`, in the order of `keys`. Keys missing from
// either config are skipped.
std::vector<ForcaPreflightValue> forca_preflight_changes(const DynamicPrintConfig& saved, const DynamicPrintConfig& edited,
                                                         const std::vector<std::string>& keys);

// The process and printer values that most often decide a print: layer height, sparse infill pattern and density
// (infill can show through top surfaces and outer walls), supports (on, type), brim, nozzle diameter (first
// extruder). Options missing from `full` are skipped.
std::vector<ForcaPreflightValue> forca_preflight_process_details(const DynamicPrintConfig& full);

// One filament slot's values (0-based `idx`): nozzle temperature for the first and other layers, the bed
// temperature for the plate's bed type (curr_bed_type), max volumetric speed. A slot past the end of a list uses its
// last value. Options missing from `full` are skipped.
std::vector<ForcaPreflightValue> forca_preflight_filament_details(const DynamicPrintConfig& full, size_t idx);

} // namespace Slic3r
