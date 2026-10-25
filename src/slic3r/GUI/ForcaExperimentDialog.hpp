#pragma once

#include <boost/filesystem/path.hpp>

#include <string>

class wxString;
class wxWindow;

namespace Slic3r { namespace GUI {

// Forca Academy -- "Plan an experiment": the user picks one setting, the values to try against the current one (the
// baseline), a hypothesis and a test model; Forca builds the test plate in a new project (one named copy per value,
// each with its per-object setting, printed by object when the setting needs it) and writes the experiment folder
// (ForcaExperiment.hpp). `tag` is the problem ("stringing", ...; empty = none), `tag_label` how the UI names it.
// Returns true when the plate was built: callers close their own window so the user sees it.
bool forca_plan_experiment(wxWindow* parent, const std::string& tag, const wxString& tag_label);

// "Compare the copies" for a planned experiment's folder: per copy better / same / worse than the baseline and a note,
// the winner (Forca proposes one), photos; then write a lesson, apply the winning value to the edited preset (unsaved;
// the user saves it) or plan the next experiment. NewPlate = the next experiment's plate was built.
enum class ForcaJudgeOutcome { Closed, NewPlate };
ForcaJudgeOutcome forca_judge_experiment(wxWindow* parent, const boost::filesystem::path& folder);

}} // namespace Slic3r::GUI
