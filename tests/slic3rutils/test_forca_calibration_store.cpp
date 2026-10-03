// Forca: ForcaCalibrationStore (src/slic3r/GUI/ForcaCalibrationStore.cpp) -- the Calibration Wizard's persisted
// progress and results, including the printer track's optional record fields and printer runs.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
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

#include "slic3r/GUI/ForcaCalibrationStore.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

using Slic3r::GUI::ForcaCalibrationStore;
namespace fs = boost::filesystem;

namespace {

// Points data_dir() at a fresh temporary folder for one test and puts the old one back afterwards.
struct ScopedDataDir
{
    std::string old_dir;
    fs::path    dir;
    ScopedDataDir()
    {
        old_dir = Slic3r::data_dir();
        dir     = fs::temp_directory_path() / fs::unique_path("forca-calib-store-%%%%-%%%%");
        fs::create_directories(dir / "forca");
        Slic3r::set_data_dir(dir.string());
    }
    ~ScopedDataDir()
    {
        Slic3r::set_data_dir(old_dir);
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
};

ForcaCalibrationStore::Key printer_key(const std::string& calibration)
{
    return ForcaCalibrationStore::Key{ "My AD5M", "0.40", "", calibration };
}

} // namespace

TEST_CASE("Printer records keep their values and note across a reload", "[ForcaCalibrationStore]")
{
    ScopedDataDir data;
    {
        ForcaCalibrationStore store;
        ForcaCalibrationStore::Record r;
        r.status         = "done";
        r.start          = 15;
        r.end            = 110;
        r.values["rx"]   = 42.5;
        r.values["ry"]   = 38.0;
        r.values["height"] = 60.0;
        r.note           = "MZV";
        r.derived_preset = "My AD5M";
        store.set_record(printer_key("printer_shaper_freq"), r);
    }
    ForcaCalibrationStore reloaded;
    ForcaCalibrationStore::Record r;
    REQUIRE(reloaded.get(printer_key("printer_shaper_freq"), r));
    CHECK(r.status == "done");
    CHECK(r.start == Catch::Approx(15));
    CHECK(r.end == Catch::Approx(110));
    CHECK(r.values.at("rx") == Catch::Approx(42.5));
    CHECK(r.values.at("ry") == Catch::Approx(38.0));
    CHECK(r.values.at("height") == Catch::Approx(60.0));
    CHECK(r.note == "MZV");
    CHECK(r.derived_preset == "My AD5M");
    CHECK_FALSE(r.updated_at.empty());
}

TEST_CASE("Printer runs are separate from filament runs and can be forgotten", "[ForcaCalibrationStore]")
{
    ScopedDataDir data;
    {
        ForcaCalibrationStore store;
        ForcaCalibrationStore::Run run;
        run.kind    = "printer";
        run.printer = "My AD5M";
        run.nozzle  = "0.40";
        run.base    = "My AD5M";
        run.target  = "My AD5M";
        run.before_text["machine_max_jerk_x"] = "9,9";
        run.before_text["input_shaping_type"] = "Default";
        store.start_run(run);

        run.before_text["machine_max_jerk_x"] = "20,20"; // a second start never replaces the backup
        store.start_run(run);
    }
    ForcaCalibrationStore store;
    ForcaCalibrationStore::Run out;
    REQUIRE(store.get_printer_run("My AD5M", out));
    CHECK(out.kind == "printer");
    CHECK(out.before_text.at("machine_max_jerk_x") == "9,9");
    CHECK(out.before_text.at("input_shaping_type") == "Default");
    // A filament lookup with the same names must not see the printer run.
    CHECK_FALSE(store.get_run("My AD5M", "0.40", "My AD5M", out));

    store.forget_printer_run("My AD5M");
    ForcaCalibrationStore after;
    CHECK_FALSE(after.get_printer_run("My AD5M", out));
}

TEST_CASE("A calibration file from before the printer track still loads", "[ForcaCalibrationStore]")
{
    ScopedDataDir data;
    {
        // Exactly the fields the store wrote before the printer track (no "values" / "note" / "kind" / "before_text").
        nlohmann::json entry = { { "printer", "Bambu Lab H2S 0.4 nozzle" }, { "nozzle", "0.40" }, { "filament", "PLA (calibrated)" },
                                 { "calibration", "temperature" }, { "status", "done" }, { "start", 250 }, { "end", 190 },
                                 { "value", 215 }, { "pass", 0 }, { "derived_preset", "PLA (calibrated)" }, { "updated_at", "2026-09-20" } };
        nlohmann::json run   = { { "printer", "Bambu Lab H2S 0.4 nozzle" }, { "nozzle", "0.40" }, { "target", "PLA (calibrated)" },
                                 { "base", "Generic PLA" }, { "started_at", "2026-09-20" },
                                 { "before", { { "nozzle_temperature", 220 } } } };
        nlohmann::json file  = { { "version", 1 }, { "entries", nlohmann::json::array({ entry }) }, { "runs", nlohmann::json::array({ run }) } };
        std::ofstream f((data.dir / "forca" / "calibration_results.json").string());
        f << file.dump(2);
    }
    ForcaCalibrationStore store;
    ForcaCalibrationStore::Record r;
    REQUIRE(store.get({ "Bambu Lab H2S 0.4 nozzle", "0.40", "PLA (calibrated)", "temperature" }, r));
    CHECK(r.value == Catch::Approx(215));
    CHECK(r.values.empty());
    CHECK(r.note.empty());
    ForcaCalibrationStore::Run run;
    REQUIRE(store.get_run("Bambu Lab H2S 0.4 nozzle", "0.40", "PLA (calibrated)", run));
    CHECK(run.kind.empty());
    CHECK(run.before.at("nozzle_temperature") == Catch::Approx(220));
    CHECK(run.before_text.empty());
}
