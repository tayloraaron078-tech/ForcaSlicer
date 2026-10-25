// Forca: guided experiments (src/slic3r/GUI/ForcaExperiment.cpp) -- which settings Forca offers per problem, the values
// it suggests, and the experiment folder it writes.
// Same Windows include prologue as test_forca_academy.cpp (wx pulls in <windows.h>).
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <ios>
#include "catch2/catch_message.hpp"
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

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/ForcaAcademy.hpp"
#include "slic3r/GUI/ForcaExperiment.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {

struct ScopedDir
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("forca-experiment-%%%%-%%%%");
    ~ScopedDir()
    {
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string read_file(const fs::path& p)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    std::stringstream       ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("Every setting offered for a problem can differ per copy on one plate", "[ForcaExperiment]")
{
    for (const std::string& tag : forca_academy_tag_keys()) {
        const std::vector<std::string> keys = forca_experiment_candidates(tag);
        CAPTURE(tag);
        CHECK_FALSE(keys.empty());
        for (const std::string& key : keys) {
            CAPTURE(key);
            CHECK(print_config_def.get(key) != nullptr);
            CHECK(forca_experiment_varies_on_one_plate(key));
            CHECK(print_config_def.get(forca_experiment_source_key(key)) != nullptr);
        }
    }
    CHECK(forca_experiment_candidates("no such tag").empty());
    // Plate-wide settings can't be part of an experiment.
    CHECK_FALSE(forca_experiment_varies_on_one_plate("initial_layer_speed"));
    CHECK_FALSE(forca_experiment_varies_on_one_plate("hot_plate_temp"));
    CHECK_FALSE(forca_experiment_varies_on_one_plate("print_sequence"));
}

TEST_CASE("A Forca override reads its baseline from the setting it stands in for", "[ForcaExperiment]")
{
    CHECK(forca_experiment_source_key("object_nozzle_temperature") == "nozzle_temperature");
    CHECK(forca_experiment_source_key("object_max_volumetric_speed") == "filament_max_volumetric_speed");
    CHECK(forca_experiment_source_key("thick_bridges") == "thick_bridges");
}

TEST_CASE("The baseline is the value the presets give now", "[ForcaExperiment]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "retraction_length", "0.8" }, { "nozzle_temperature", "215,230" }, { "thick_bridges", "1" } });
    CHECK(forca_experiment_current_value(config, "thick_bridges") == "1");
    CHECK(forca_experiment_current_value(config, "object_nozzle_temperature") == "215"); // the first filament
    CHECK(forca_experiment_current_value(config, "object_retraction_length") == "0.8");
    // A filament's own retraction overrides the printer's.
    config.set_deserialize_strict({ { "filament_retraction_length", "1.5" } });
    CHECK(forca_experiment_current_value(config, "object_retraction_length") == "1.5");
}

TEST_CASE("Forca suggests values a sensible step from the current one", "[ForcaExperiment]")
{
    CHECK(forca_experiment_suggest_values("thick_bridges", "0", 3) == std::vector<std::string>{ "1" });
    CHECK(forca_experiment_suggest_values("thick_bridges", "1", 3) == std::vector<std::string>{ "0" });
    CHECK(forca_experiment_suggest_values("object_nozzle_temperature", "220", 3) == std::vector<std::string>{ "225", "215", "230" });
    CHECK(forca_experiment_suggest_values("object_retraction_length", "0.8", 2) == std::vector<std::string>{ "0.9", "0.7" });
    CHECK(forca_experiment_suggest_values("object_fan_min_speed", "50", 2) == std::vector<std::string>{ "60", "40" });
    CHECK(forca_experiment_suggest_values("outer_wall_speed", "200", 1) == std::vector<std::string>{ "220" });
    // Never below zero (the overrides' -1 means "not set") and never above the setting's maximum.
    CHECK(forca_experiment_suggest_values("object_z_hop", "0", 2) == std::vector<std::string>{ "0.1", "0.2" });
    CHECK(forca_experiment_suggest_values("object_fan_max_speed", "100", 2) == std::vector<std::string>{ "90", "80" });
    // A list setting: the other choices.
    const std::vector<std::string> brims = forca_experiment_suggest_values("brim_type", "auto_brim", 2);
    CHECK(brims.size() == 2);
    for (const std::string& b : brims)
        CHECK(b != "auto_brim");
    CHECK(forca_experiment_suggest_values("object_retraction_length", "not a number", 2).empty());
}

TEST_CASE("Copies are laid out so the print head clears them printed by object", "[ForcaExperiment]")
{
    // A 30 x 20 x 20 mm test on a 220 x 220 bed, 37 mm apart (a 35 mm clearance radius), rod at 34 mm, lid at 120 mm:
    // (220 + 37) / (30 + 37) = 3 across, (220 + 37) / (20 + 37) = 4 deep.
    ForcaExperimentLayout l = forca_experiment_layout(30, 20, 20, 220, 220, 250, 37, 34, 120);
    CHECK(l.per_plate == 12);
    CHECK(l.cols == 3);
    CHECK_FALSE(l.turned);
    // Taller than the rod: one column, so no copy printed later shares a row with a taller one.
    l = forca_experiment_layout(30, 20, 60, 220, 220, 250, 37, 34, 120);
    CHECK(l.cols == 1);
    CHECK(l.per_plate == 4);
    // Taller than the lid: one copy per plate.
    CHECK(forca_experiment_layout(30, 20, 150, 220, 220, 250, 37, 34, 120).per_plate == 1);
    // Taller than the printer, or wider than the bed either way: no plate.
    CHECK(forca_experiment_layout(30, 20, 300, 220, 220, 250, 37, 34, 120).per_plate == 0);
    CHECK(forca_experiment_layout(300, 240, 20, 220, 220, 250, 37, 34, 120).per_plate == 0);
    // Turned round when that fits more: a 150 x 20 part on a 160 x 100 bed with a 5 mm gap.
    l = forca_experiment_layout(20, 150, 10, 160, 100, 250, 5, 34, 120);
    CHECK(l.turned);
    CHECK(l.per_plate == 4);
}

TEST_CASE("Copies that don't fit on one plate are spread evenly over more", "[ForcaExperiment]")
{
    CHECK(forca_experiment_plates(4, 12) == std::vector<size_t>{ 0, 0, 0, 0 });
    CHECK(forca_experiment_plates(4, 3) == std::vector<size_t>{ 0, 0, 1, 1 }); // 2 + 2, not 3 + 1
    CHECK(forca_experiment_plates(3, 2) == std::vector<size_t>{ 0, 0, 1 });
    CHECK(forca_experiment_plates(4, 1) == std::vector<size_t>{ 0, 1, 2, 3 });
    CHECK(forca_experiment_plates(4, 0).empty());
}

TEST_CASE("An experiment is written as a folder with its plan and page, and listed", "[ForcaExperiment]")
{
    ScopedDir           tmp;
    ForcaExperimentPlan plan;
    plan.title      = "Thick bridges on AD5M";
    plan.date       = "2026-10-18";
    plan.printer    = "AD5M";
    plan.filament   = "Generic PLA";
    plan.process    = "0.20mm Standard";
    plan.tag        = "bridging";
    plan.key        = "thick_bridges";
    plan.label      = "Thick external bridges";
    plan.baseline   = "0";
    plan.variants   = { "1" };
    plan.hypothesis = "Thick external bridges sag less.";
    plan.model      = "bridge";

    const std::vector<std::string> names = forca_experiment_copy_names(plan);
    CHECK(names == std::vector<std::string>{ "A baseline - off", "B - on" });
    ForcaExperimentPlan shown = plan;
    shown.shown               = { "Off", "On" }; // as the UI shows them
    CHECK(forca_experiment_copy_names(shown) == std::vector<std::string>{ "A baseline - Off", "B - On" });

    std::string    err;
    const fs::path folder = forca_experiment_create(tmp.dir, plan, err);
    REQUIRE_FALSE(folder.empty());
    CHECK(folder == tmp.dir / "experiments" / "2026-10-18_thick-bridges-on-ad5m");
    const std::string page = read_file(folder / "experiment.md");
    CHECK(page.find("Thick external bridges sag less.") != std::string::npos);
    CHECK(page.find("| B - on | on |") != std::string::npos);
    CHECK(page.find("<!-- forca:results -->") != std::string::npos);

    ForcaExperimentPlan loaded;
    REQUIRE(forca_experiment_load(folder, loaded));
    CHECK(loaded.key == "thick_bridges");
    CHECK(loaded.baseline == "0");
    CHECK(loaded.variants == plan.variants);
    CHECK(loaded.hypothesis == plan.hypothesis);
    CHECK_FALSE(loaded.by_object);
    CHECK_FALSE(forca_experiment_load(tmp.dir, loaded));

    // Never overwritten: the same plan again gets "-2".
    const fs::path again = forca_experiment_create(tmp.dir, plan, err);
    CHECK(again == tmp.dir / "experiments" / "2026-10-18_thick-bridges-on-ad5m-2");

    const std::vector<ForcaAcademyExperiment> list = forca_academy_list_experiments(tmp.dir);
    REQUIRE(list.size() == 2);
    CHECK(list[0].file == again / "experiment.md"); // newest (last-named) first
    CHECK(list[1].title == "Thick bridges on AD5M");
    CHECK(list[1].date == "2026-10-18");
    CHECK_FALSE(list[1].has_result);
}

namespace {

ForcaExperimentPlan retraction_plan()
{
    ForcaExperimentPlan plan;
    plan.title    = "Retraction on H2S";
    plan.date     = "2026-10-18";
    plan.printer  = "H2S";
    plan.filament = "Sunlu PETG";
    plan.process  = "0.20mm Standard";
    plan.tag      = "stringing";
    plan.key      = "object_retraction_length";
    plan.label    = "Retraction length";
    plan.baseline = "0.8";
    plan.variants = { "1", "1.2" };
    plan.shown    = { "0.8 mm", "1 mm", "1.2 mm" };
    plan.model    = "stringing";
    return plan;
}

} // namespace

TEST_CASE("A sent plate with an experiment's copies is linked to the experiment", "[ForcaExperiment]")
{
    ScopedDir      tmp;
    std::string    err;
    const fs::path folder = forca_experiment_create(tmp.dir, retraction_plan(), err);
    REQUIRE_FALSE(folder.empty());
    const std::vector<std::string> names = forca_experiment_copy_names(retraction_plan());

    CHECK(forca_experiment_for_plate(tmp.dir, names) == "experiments/2026-10-18_retraction-on-h2s");
    CHECK(forca_experiment_for_plate(tmp.dir, { names[0], names[1] }).empty()); // a copy is missing
    CHECK(forca_experiment_for_plate(tmp.dir, { "Benchy" }).empty());

    // Through the journal's record writer, as when the plate is sent.
    const nlohmann::json record = { { "time", "2026-10-18T09:30:00" },
                                    { "title", "Retraction on H2S" },
                                    { "plate", { { "number", 1 }, { "objects", names } } },
                                    { "outcome", nullptr } };
    const fs::path print = forca_academy_write_record(tmp.dir, record, "", err);
    REQUIRE_FALSE(print.empty());
    CHECK(nlohmann::json::parse(read_file(print / "record.json")).value("experiment", std::string()) ==
          "experiments/2026-10-18_retraction-on-h2s");

    ForcaExperimentPlan loaded;
    REQUIRE(forca_experiment_load(folder, loaded));
    CHECK(loaded.state == "printed");
    REQUIRE(loaded.prints.size() == 1);
    CHECK(loaded.prints[0] == "prints/2026/2026-10-18_0930_retraction-on-h2s");
    CHECK(read_file(folder / "experiment.md").find("](../../prints/2026/2026-10-18_0930_retraction-on-h2s)") != std::string::npos);
    CHECK(forca_academy_list_experiments(tmp.dir).front().state == "printed");

    // A plate without those copies gets no link.
    const nlohmann::json other = { { "time", "2026-10-18T10:00:00" }, { "title", "Benchy" }, { "plate", { { "objects", { "Benchy" } } } } };
    const fs::path benchy = forca_academy_write_record(tmp.dir, other, "", err);
    CHECK_FALSE(nlohmann::json::parse(read_file(benchy / "record.json")).contains("experiment"));
}

TEST_CASE("Comparing the copies saves the ratings and the verdict", "[ForcaExperiment]")
{
    CHECK(forca_experiment_proposed_winner({ "same", "better" }) == 2);
    CHECK(forca_experiment_proposed_winner({ "better", "better" }) == 1); // the first one; the user can change it
    CHECK(forca_experiment_proposed_winner({ "worse", "same" }) == 0);    // the baseline stays

    ScopedDir      tmp;
    std::string    err;
    const fs::path folder = forca_experiment_create(tmp.dir, retraction_plan(), err);
    REQUIRE_FALSE(folder.empty());
    // The user's own Result text is never touched.
    {
        std::string md = read_file(folder / "experiment.md");
        md.replace(md.find(FORCA_EXPERIMENT_RESULT_UNWRITTEN), std::string(FORCA_EXPERIMENT_RESULT_UNWRITTEN).size(), "My own notes.");
        boost::nowide::ofstream f((folder / "experiment.md").string(), std::ios::binary | std::ios::trunc);
        f << md;
    }
    REQUIRE(forca_experiment_judge(folder, { "same", "better" }, { "", "a few hairs", "clean" }, 2, err));

    ForcaExperimentPlan plan;
    REQUIRE(forca_experiment_load(folder, plan));
    CHECK(plan.state == "judged");
    CHECK(plan.winner == 2);
    CHECK(plan.ratings == std::vector<std::string>{ "same", "better" });
    CHECK(plan.notes == std::vector<std::string>{ "", "a few hairs", "clean" });
    const std::string md = read_file(folder / "experiment.md");
    CHECK(md.find("| C - 1.2 mm | 1.2 mm | better | clean |") != std::string::npos);
    CHECK(md.find("**Verdict:** C - 1.2 mm works best: Retraction length 1.2 mm.") != std::string::npos);
    CHECK(md.find("My own notes.") != std::string::npos);
    CHECK(forca_academy_list_experiments(tmp.dir).front().state == "judged");

    const std::string lesson = forca_experiment_lesson_draft(plan, "experiments/2026-10-18_retraction-on-h2s", "2026-10-19");
    CHECK(lesson.find("- Rule: Use Retraction length 1.2 mm instead of 0.8 mm.") != std::string::npos);
    CHECK(lesson.find("(experiments/2026-10-18_retraction-on-h2s/experiment.md)") != std::string::npos);
    CHECK(lesson.find("playbooks/stringing.md") != std::string::npos);
}

TEST_CASE("The winning value goes to the preset that owns the setting", "[ForcaExperiment]")
{
    CHECK(forca_experiment_preset_target("thick_bridges").kind == ForcaExperimentTarget::Process);
    CHECK(forca_experiment_preset_target("thick_bridges").key == "thick_bridges");
    CHECK(forca_experiment_preset_target("object_retraction_length").kind == ForcaExperimentTarget::Filament);
    CHECK(forca_experiment_preset_target("object_retraction_length").key == "filament_retraction_length");
    CHECK(forca_experiment_preset_target("object_nozzle_temperature").key == "nozzle_temperature");
    CHECK(forca_experiment_preset_target("object_max_volumetric_speed").key == "filament_max_volumetric_speed");

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "nozzle_temperature", "215,230" } });
    REQUIRE(forca_experiment_set_value(config, "nozzle_temperature", "225"));
    CHECK(config.option("nozzle_temperature")->serialize() == "225,225"); // every slot
    REQUIRE(forca_experiment_set_value(config, "filament_retraction_length", "1.2"));
    CHECK(forca_experiment_current_value(config, "object_retraction_length") == "1.2");
    CHECK_FALSE(forca_experiment_set_value(config, "nozzle_temperature", "hot"));
    CHECK_FALSE(forca_experiment_set_value(config, "no_such_key", "1"));
}

TEST_CASE("Each plate of an experiment spread over plates is linked when it is sent", "[ForcaExperiment]")
{
    ScopedDir           tmp;
    std::string         err;
    ForcaExperimentPlan plan = retraction_plan();
    plan.variants            = { "1", "1.2", "1.4" };
    plan.shown               = { "0.8 mm", "1 mm", "1.2 mm", "1.4 mm" };
    plan.by_object           = true;
    plan.plates              = { 0, 0, 1, 1 };
    const fs::path folder    = forca_experiment_create(tmp.dir, plan, err);
    REQUIRE_FALSE(folder.empty());
    const std::vector<std::string> names = forca_experiment_copy_names(plan);
    CHECK(read_file(folder / "experiment.md").find("| D - 1.4 mm | 1.4 mm | 2 |") != std::string::npos);

    ForcaExperimentPlan loaded;
    REQUIRE(forca_experiment_load(folder, loaded));
    CHECK(loaded.plates == plan.plates);
    CHECK(loaded.by_object);

    const std::string exp = "experiments/2026-10-18_retraction-on-h2s";
    CHECK(forca_experiment_for_plate(tmp.dir, { names[0], names[1] }) == exp); // plate 1
    CHECK(forca_experiment_for_plate(tmp.dir, { names[2], names[3] }) == exp); // plate 2
    CHECK(forca_experiment_for_plate(tmp.dir, { names[1], names[2] }).empty()); // neither plate complete
}
