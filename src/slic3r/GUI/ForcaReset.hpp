#pragma once

// Forca: "Reset Forca data" -- a factory reset for Forca and the parts that keep information (HQ PLAN_factory_reset.md,
// docs/HLSD/forca-reset.md). Nothing is deleted: every file and folder moves into a dated backup folder. The reset
// runs at the next start, before anything loads, because Forca's parts keep state in memory while it runs and the
// app data folder can't move while Forca runs from it.

#include <boost/filesystem/path.hpp>

#include <string>
#include <utility>
#include <vector>

class wxWindow;

namespace Slic3r { namespace GUI {

// ---- file side (no GUI; unit-tested in tests/slic3rutils/test_forca_reset.cpp) ----

struct ForcaResetParts
{
    bool app         = true; // Forca itself: the whole app data folder and %LOCALAPPDATA%\ForcaSlicer (a fresh install)
    bool academy     = true; // the Academy folder and the tracking of running prints
    bool ai          = true; // Forca AI: key, shared printers, pause permission, level, activity log, file ledger
    bool ai_files    = true; // Documents\Forca AI: AI-saved projects, backups, bed-check pictures
    bool calibration = true; // the Calibration Wizard's progress and results history
};

struct ForcaResetPaths
{
    boost::filesystem::path data_dir;     // the app data folder (%APPDATA%\ForcaSlicer, or --datadir)
    boost::filesystem::path local_dir;    // %LOCALAPPDATA%\ForcaSlicer (web-view cache, shared Forca AI key)
    boost::filesystem::path academy_dir;  // the Academy folder in use
    boost::filesystem::path ai_files_dir; // Documents\Forca AI
    boost::filesystem::path backup_dir;   // Documents\Forca reset backups\<stamp>
};

struct ForcaResetPlan
{
    std::vector<std::pair<boost::filesystem::path, boost::filesystem::path>> moves; // from -> to, in order
    std::vector<std::string> clear_settings; // app config keys to reset once the config has loaded
};

// "2026-10-15 1904" (local time), for the backup folder's name.
std::string forca_reset_stamp();
// What a reset of `parts` moves and clears.
ForcaResetPlan forca_reset_plan(const ForcaResetParts& parts, const ForcaResetPaths& paths);
// The plan file next to the data folder: "<data folder>.forca-reset".
boost::filesystem::path forca_reset_marker(const boost::filesystem::path& data_dir);
bool forca_reset_write_plan(const boost::filesystem::path& data_dir, const ForcaResetPlan& plan, std::string& err);
bool forca_reset_read_plan(const boost::filesystem::path& data_dir, ForcaResetPlan& plan);
// Moves a file or folder (parents of `to` created): a rename, else a copy and remove (another drive). A missing
// `from` is not an error (nothing to move).
bool forca_reset_move(const boost::filesystem::path& from, const boost::filesystem::path& to, std::string& err);
// Start-up, before anything reads the data folder: carry out a pending plan's moves. Returns the errors (empty when
// all moved). The plan stays for the settings step when it has settings to clear; otherwise it is removed.
std::vector<std::string> forca_reset_apply_moves(const boost::filesystem::path& data_dir);

// ---- GUI side ----

// After the app config has loaded: reset the settings a pending plan names, and remove the plan.
void forca_reset_apply_settings();
// The "Reset Forca data" dialog (Preferences, the Forca AI window, the Print Journal). On confirm it writes the plan
// and closes Forca; the reset happens when Forca next starts.
void forca_reset_show_dialog(wxWindow* parent);

}} // namespace Slic3r::GUI
