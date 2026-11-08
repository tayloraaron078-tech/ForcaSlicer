#pragma once

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {

// Forca Academy -- preset recipes (docs/HLSD/forca-academy.md, HQ PLAN_preset_recipes.md): the settings of a print
// that came out well, saved as new user presets on top of the presets it was printed with, plus a page in the
// Academy's recipes/ folder. Printer settings are never part of a recipe. This part has no GUI and is unit-tested;
// ForcaRecipeDialog shows a recipe and saves it.

// One setting of a recipe.
struct ForcaRecipeRow
{
    std::string key;
    std::string parent;  // the parent preset's value now
    std::string value;   // the value the print used (for a filament preset: its slot's)
    std::string problem; // "" = usable; "cut": the journal kept only the start of the value; "invalid": it doesn't fit the setting now
};

// The settings in which a print differs from `parent`, the preset the recipe's preset will be based on, as it is now.
// `print_values`: one section ("process" or "filament") of the print's settings.json, or of record.json
// "unsaved_changes" for a print recorded before settings.json. `slot`: for a filament preset the 0-based filament it
// is for (the print's list values hold one entry per filament), -1 for the process. `only_keys` non-null: just the
// keys in that object (the settings changed from the presets when the print was sent). Preset bookkeeping keys and
// private settings are skipped, and so are keys `parent` doesn't have.
std::vector<ForcaRecipeRow> forca_recipe_rows(const DynamicPrintConfig& parent, const nlohmann::json& print_values, int slot,
                                              const nlohmann::json* only_keys);
// Sets the usable rows in `config` (a copy of the parent). False, naming the setting in `err`, if one doesn't parse.
bool forca_recipe_apply(DynamicPrintConfig& config, const std::vector<ForcaRecipeRow>& rows, std::string& err);
// The filaments a recorded print used, 0-based: the ones with a recorded weight when any has one, else all of them.
std::vector<int> forca_recipe_used_slots(const nlohmann::json& record);

// One preset of a saved recipe, for its Academy page.
struct ForcaRecipePreset
{
    std::string                 type; // "process" | "filament"
    std::string                 name, parent;
    int                         slot = -1; // filament: 0-based
    std::vector<ForcaRecipeRow> rows;      // the usable rows that went into it
};
// recipes/<date>_<slug(name)>.md: the recipe's presets with their parents and settings, the print it came from
// (`print_rel`, relative to the Academy folder `dir`; empty when none) and `source` (one line: how it was made).
// Never overwrites ("-2", ...). Returns the file, or an empty path with `err` set.
boost::filesystem::path forca_recipe_write_page(const boost::filesystem::path& dir, const std::string& name, const std::string& date,
                                                const std::string& print_rel, const std::string& print_title,
                                                const std::string& source, const std::vector<ForcaRecipePreset>& presets,
                                                std::string& err);
// The recipes in `dir`/recipes, newest first: (file stem, the page's title).
std::vector<std::pair<std::string, std::string>> forca_recipe_list(const boost::filesystem::path& dir);

} // namespace GUI
} // namespace Slic3r
