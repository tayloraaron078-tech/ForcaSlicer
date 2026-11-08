#pragma once

#include <boost/filesystem/path.hpp>

#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

struct ForcaExperimentPlan;

// Forca Academy -- "Make a recipe" (ForcaRecipe.hpp): the settings in which a print differs from its process and
// filament presets, each with a checkbox, saved as new user presets on top of those presets and put in use, plus a
// page in the Academy's recipes/ folder. Printer settings are left out.

// For a journal print (its folder under prints/). Returns true when a recipe was saved.
bool forca_make_recipe_from_print(wxWindow* parent, const boost::filesystem::path& print_folder);
// For an experiment's winning copy (`winner`: 0 = the baseline, 1.. = a variant): the newest recorded print of its
// plate with the winning value, or the plan's presets with that value when no print was recorded.
bool forca_make_recipe_from_experiment(wxWindow* parent, const boost::filesystem::path& folder, const ForcaExperimentPlan& plan,
                                       int winner);

}} // namespace Slic3r::GUI
