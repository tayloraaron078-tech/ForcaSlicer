// Forca: "Reset Forca data" (src/slic3r/GUI/ForcaReset.cpp) -- what each part moves to the backup folder, and that the
// start-up step carries out a pending plan before Forca reads its data folder.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include "catch2/catch_test_macros.hpp"
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/ForcaAcademy.hpp"
#include "slic3r/GUI/ForcaReset.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <string>

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {

void touch(const fs::path& p)
{
    fs::create_directories(p.parent_path());
    boost::nowide::ofstream(p.string()) << "x";
}

struct ResetDirs
{
    ScopedTemporaryDir tmp{ "forca-reset" };
    ForcaResetPaths    paths;
    ResetDirs()
    {
        paths.data_dir     = tmp.path() / "Roaming" / "ForcaSlicer";
        paths.local_dir    = tmp.path() / "Local" / "ForcaSlicer";
        paths.academy_dir  = tmp.path() / "Documents" / "Forca Academy";
        paths.ai_files_dir = tmp.path() / "Documents" / "Forca AI";
        paths.backup_dir   = tmp.path() / "Documents" / "Forca reset backups" / "2026-10-15 1904";
        touch(paths.data_dir / "ForcaSlicer.conf");
        touch(paths.data_dir / "forca" / "academy_tracking.json");
        touch(paths.data_dir / "forca" / "calibration_results.json");
        touch(paths.data_dir / "forca" / "ai" / "activity.jsonl");
        touch(paths.local_dir / "forca-ai-key.txt");
        touch(paths.academy_dir / "prints" / "2026" / "a" / "record.json");
        touch(paths.ai_files_dir / "Benchy (Claude 2026-10-15).3mf");
    }
};

bool moves(const ForcaResetPlan& plan, const fs::path& from)
{
    return std::any_of(plan.moves.begin(), plan.moves.end(), [&](const auto& m) { return m.first == from; });
}

} // namespace

TEST_CASE("A fresh-install reset moves the whole data folders, plus the Documents folders ticked", "[ForcaReset]")
{
    ResetDirs       d;
    ForcaResetParts parts; // all ticked
    const auto      plan = forca_reset_plan(parts, d.paths);
    CHECK(moves(plan, d.paths.data_dir));
    CHECK(moves(plan, d.paths.local_dir));
    CHECK(moves(plan, d.paths.academy_dir));
    CHECK(moves(plan, d.paths.ai_files_dir));
    CHECK(plan.clear_settings.empty()); // the config moves with the data folder
    CHECK_FALSE(moves(plan, d.paths.data_dir / "forca" / "ai")); // covered by the whole folder
}

TEST_CASE("Without a fresh install each part moves only its own files and clears its own settings", "[ForcaReset]")
{
    ResetDirs       d;
    ForcaResetParts parts;
    parts.app = false;

    SECTION("Forca AI")
    {
        parts.academy = parts.ai_files = parts.calibration = false;
        const auto plan = forca_reset_plan(parts, d.paths);
        CHECK(moves(plan, d.paths.data_dir / "forca" / "ai"));
        CHECK(moves(plan, d.paths.local_dir / "forca-ai-key.txt"));
        CHECK_FALSE(moves(plan, d.paths.data_dir));
        CHECK(std::find(plan.clear_settings.begin(), plan.clear_settings.end(), "forca_ai_printers") != plan.clear_settings.end());
    }
    SECTION("the journal and the Calibration Wizard")
    {
        parts.ai = parts.ai_files = false;
        const auto plan = forca_reset_plan(parts, d.paths);
        CHECK(moves(plan, d.paths.academy_dir));
        CHECK(moves(plan, d.paths.data_dir / "forca" / "academy_tracking.json"));
        CHECK(moves(plan, d.paths.data_dir / "forca" / "calibration_results.json"));
        CHECK_FALSE(moves(plan, d.paths.ai_files_dir));
        CHECK(plan.clear_settings.empty());
    }
}

TEST_CASE("A pending reset is carried out at the next start, with everything kept in the backup", "[ForcaReset]")
{
    ResetDirs   d;
    std::string err;
    REQUIRE(forca_reset_write_plan(d.paths.data_dir, forca_reset_plan(ForcaResetParts(), d.paths), err));
    CHECK(fs::exists(forca_reset_marker(d.paths.data_dir)));

    const auto errors = forca_reset_apply_moves(d.paths.data_dir);
    CHECK(errors.empty());
    // Forca starts in an empty data folder; nothing was deleted.
    CHECK(fs::is_directory(d.paths.data_dir));
    CHECK_FALSE(fs::exists(d.paths.data_dir / "ForcaSlicer.conf"));
    CHECK(fs::exists(d.paths.backup_dir / "App data" / "ForcaSlicer.conf"));
    CHECK(fs::exists(d.paths.backup_dir / "Local app data" / "forca-ai-key.txt"));
    CHECK(fs::exists(d.paths.backup_dir / "Forca Academy" / "prints" / "2026" / "a" / "record.json"));
    CHECK(fs::exists(d.paths.backup_dir / "Forca AI" / "Benchy (Claude 2026-10-15).3mf"));
    CHECK_FALSE(fs::exists(d.paths.academy_dir));
    // Done: the plan is gone, so the next start doesn't reset again.
    CHECK_FALSE(fs::exists(forca_reset_marker(d.paths.data_dir)));
    CHECK(forca_reset_apply_moves(d.paths.data_dir).empty());
}

TEST_CASE("A reset with settings to clear keeps its plan for the step after the config loads", "[ForcaReset]")
{
    ResetDirs       d;
    ForcaResetParts parts;
    parts.app = parts.academy = parts.ai_files = parts.calibration = false; // Forca AI only
    std::string err;
    REQUIRE(forca_reset_write_plan(d.paths.data_dir, forca_reset_plan(parts, d.paths), err));
    CHECK(forca_reset_apply_moves(d.paths.data_dir).empty());
    CHECK(fs::exists(d.paths.backup_dir / "Forca data" / "ai" / "activity.jsonl"));
    CHECK(fs::exists(d.paths.data_dir / "ForcaSlicer.conf")); // the rest stays

    ForcaResetPlan left;
    REQUIRE(forca_reset_read_plan(d.paths.data_dir, left));
    CHECK(left.moves.empty());
    CHECK_FALSE(left.clear_settings.empty());
}

TEST_CASE("A journal kept directly in a folder of the user's moves without the user's other files", "[ForcaReset]")
{
    ResetDirs d;
    d.paths.academy_dir = d.tmp.path() / "Documents" / "Printing";
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(d.paths.academy_dir, err));
    touch(d.paths.academy_dir / "my model.stl");

    ForcaResetParts parts;
    parts.app = parts.ai = parts.ai_files = parts.calibration = false;
    const auto plan = forca_reset_plan(parts, d.paths);
    CHECK_FALSE(moves(plan, d.paths.academy_dir));
    REQUIRE(forca_reset_write_plan(d.paths.data_dir, plan, err));
    CHECK(forca_reset_apply_moves(d.paths.data_dir).empty());

    CHECK(fs::exists(d.paths.academy_dir / "my model.stl"));
    CHECK_FALSE(fs::exists(d.paths.academy_dir / "AGENTS.md"));
    CHECK_FALSE(fs::exists(d.paths.academy_dir / "prints"));
    CHECK(fs::exists(d.paths.backup_dir / "Forca Academy" / "AGENTS.md"));
    CHECK(fs::is_directory(d.paths.backup_dir / "Forca Academy" / "playbooks"));
}

TEST_CASE("Moving a missing file or folder is not an error", "[ForcaReset]")
{
    ScopedTemporaryDir tmp("forca-reset-move");
    std::string        err;
    CHECK(forca_reset_move(tmp.path() / "missing", tmp.path() / "backup" / "missing", err));
    CHECK(err.empty());
}
