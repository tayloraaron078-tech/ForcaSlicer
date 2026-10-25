#pragma once

#include <boost/filesystem/path.hpp>

#include <cstddef>

#include <string>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {

// Forca Academy -- guided experiments (docs/HLSD/forca-academy.md, HQ PLAN_experiment_generator.md): one setting,
// a baseline and up to three variants, printed as copies side by side, each copy with its own per-object setting.
// The plates always print by object, so each copy prints on its own and no copy's travel, strings or temperature
// reaches another. This part has no GUI and is unit-tested; ForcaExperimentDialog builds the plates.

// The settings Forca suggests trying for a problem tag ("stringing", ...), most likely first. Each is a key that can
// differ per object on one plate (a per-object process setting or a Forca filament override). Empty for unknown tags.
std::vector<std::string> forca_experiment_candidates(const std::string& tag);
// True when `key` can differ per copy on one plate (a print-object or print-region setting).
bool forca_experiment_varies_on_one_plate(const std::string& key);
// For a Forca filament override (object_*), the filament/printer setting it stands in for ("object_nozzle_temperature"
// -> "nozzle_temperature"); `key` itself for everything else. Where the baseline value is read from.
std::string forca_experiment_source_key(const std::string& key);
// The value `key` has now in `config` (the full config of the current presets), as text: for a Forca override the
// value of its source setting (a filament override wins over the printer's), for a list the first entry.
std::string forca_experiment_current_value(const DynamicPrintConfig& config, const std::string& key);
// Up to `count` values to try against `current`: the other state of a switch, other choices of a list setting, or
// numbers one or two sensible steps away (within the setting's limits).
std::vector<std::string> forca_experiment_suggest_values(const std::string& key, const std::string& current, size_t count);

// How copies of a `width` x `depth` x `height` model go on a `bed_width` x `bed_depth` bed printed by object, the way
// Orca's collision check (Print.cpp, sequential_print_clearance_valid) accepts them: `gap` apart (the extruder
// clearance), in a grid while the model is no taller than `height_to_rod`; taller, one column, since a copy may only
// pass the gantry rod's height when no copy printed after it shares its row; taller than `height_to_lid`, one per
// plate (only the last copy on a plate may pass the lid's height). per_plate = 0: it fits on no plate.
struct ForcaExperimentLayout
{
    size_t per_plate = 0;
    size_t cols      = 0;     // copies per row
    bool   turned    = false; // turned 90 degrees, when that fits more
};
ForcaExperimentLayout forca_experiment_layout(double width, double depth, double height, double bed_width, double bed_depth,
                                              double printable_height, double gap, double height_to_rod, double height_to_lid);
// The plate (0-based) of each of `copies` copies when `per_plate` fit on one: as few plates as needed, the copies
// spread evenly over them in order (A on the first).
std::vector<size_t> forca_experiment_plates(size_t copies, size_t per_plate);

struct ForcaExperimentPlan
{
    std::string              title, date;                // date = "YYYY-MM-DD"
    std::string              printer, filament, process; // the presets the plate is for
    std::string              tag;                        // the problem tag that led to it ("" = none)
    std::string              key, label;                 // the one setting, and its label as the UI shows it
    std::string              baseline;                   // copy A
    std::vector<std::string> variants;                   // copies B, C, D
    std::vector<std::string> shown; // the values as the UI shows them ("1 mm", "On"), baseline first; empty = raw
    std::string              hypothesis;
    std::string              model; // "bridge", "overhang", "stringing" (built-in) or the user's object name
    bool                     by_object = false; // false only in plans written before every plate printed by object
    std::vector<size_t>      plates;            // per copy, baseline first: its plate (0-based); empty = all on one
    // Filled in later (forca_experiment_load):
    std::string              state = "planned"; // planned -> printed (a print was sent) -> judged
    std::vector<std::string> prints;            // the linked print folders, relative to the Academy folder
    std::vector<std::string> ratings;           // per variant: "better" / "same" / "worse" than the baseline
    std::vector<std::string> notes;             // per copy, baseline first
    int                      winner = -1;       // 0 = the baseline, 1.. = a variant; -1 = not judged
};
// The copies' object names: "A baseline - <value>", "B - <value>", ... (the project is named after the experiment).
std::vector<std::string> forca_experiment_copy_names(const ForcaExperimentPlan& plan);
// Writes experiments/<date>_<slug>/ with plan.json (Forca's) and experiment.md (the user's page: hypothesis, the
// copies and their values, a Result section). Never overwrites ("-2", ...). Returns the folder, or empty with `err`.
boost::filesystem::path forca_experiment_create(const boost::filesystem::path& dir, const ForcaExperimentPlan& plan, std::string& err);
// Reads a plan written by forca_experiment_create; false if the folder has none.
bool forca_experiment_load(const boost::filesystem::path& folder, ForcaExperimentPlan& plan);

// ---- after the plate is printed ----
// The newest experiment with a plate whose copies (by object name) are all among `object_names` (a sent plate's
// objects), as its folder relative to the Academy folder `dir` ("experiments/<date>_<slug>"); empty when none.
std::string forca_experiment_for_plate(const boost::filesystem::path& dir, const std::vector<std::string>& object_names);
// Links a sent print: plan.json "prints" gets it (state planned -> printed) and the page's Prints block lists it.
// `print_rel` is the print folder relative to the Academy folder, `when` its record time.
bool forca_experiment_add_print(const boost::filesystem::path& folder, const std::string& print_rel, const std::string& when,
                                std::string& err);
// The copy Forca proposes as the winner: the first variant rated better than the baseline, else 0 (the baseline).
int forca_experiment_proposed_winner(const std::vector<std::string>& ratings);
// Saves the comparison: per variant a rating, per copy (baseline first) a note, and the winner. State -> judged, and
// the page's Comparison block gets the table and the verdict. The user's own Result section is left alone.
bool forca_experiment_judge(const boost::filesystem::path& folder, const std::vector<std::string>& ratings,
                            const std::vector<std::string>& notes, int winner, std::string& err);
// A lessons.md entry for a judged experiment, for the user to edit before saving. `folder_rel` as above.
std::string forca_experiment_lesson_draft(const ForcaExperimentPlan& plan, const std::string& folder_rel, const std::string& date);

// "Apply to preset": which preset and key take the winning value. A process setting goes to the process preset; a
// Forca filament override to the filament setting it stands in for (retraction and z-hop as the filament's own
// override of the printer's value, "filament_retraction_length").
struct ForcaExperimentTarget
{
    enum Kind { Process, Filament } kind = Process;
    std::string key;
};
ForcaExperimentTarget forca_experiment_preset_target(const std::string& key);
// Sets `key` in a preset's config to `value`, in every slot of a list setting. False if the key is missing or the
// value doesn't parse.
bool forca_experiment_set_value(DynamicPrintConfig& config, const std::string& key, const std::string& value);

} // namespace GUI
} // namespace Slic3r
