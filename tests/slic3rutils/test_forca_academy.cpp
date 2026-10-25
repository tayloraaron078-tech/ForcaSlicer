// Forca: Forca Academy's file side (src/slic3r/GUI/ForcaAcademy.cpp) -- folder names, the starter files, and the
// per-print record folders it writes when a print is sent.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <ios>
#include "catch2/catch_test_macros.hpp"
#include <cstddef>
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

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {

struct ScopedDir
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("forca-academy-%%%%-%%%%");
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

const std::string TEMP_TOWER = "\xe6\xb8\xa9\xe5\xba\xa6\xe5\xa1\x94"; // 温度塔
const std::string TEST_KANA  = "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"; // テスト
const std::string IDEO_SPACE = "\xe3\x80\x80";                         // ideographic space

} // namespace

TEST_CASE("A print name becomes a safe folder name", "[ForcaAcademy]")
{
    CHECK(forca_academy_slug("My Benchy v2.3mf") == "my-benchy-v2-3mf");
    CHECK(forca_academy_slug("a<b>c:d\"e/f\\g|h?i*j") == "a-b-c-d-e-f-g-h-i-j"); // Windows-forbidden characters
    CHECK(forca_academy_slug("  ** ** ") == "print");                             // nothing left -> a default
    CHECK(forca_academy_slug(TEMP_TOWER + IDEO_SPACE + TEST_KANA) == TEMP_TOWER + "-" + TEST_KANA);
    CHECK(forca_academy_slug(std::string(100, 'x')).size() == 48);
}

TEST_CASE("The journal goes in its own Forca Academy folder inside the folder the user chose", "[ForcaAcademy]")
{
    ScopedDir tmp;
    fs::create_directories(tmp.dir / "Documents");
    CHECK(forca_academy_folder_in(tmp.dir / "Documents") == tmp.dir / "Documents" / "Forca Academy");
    CHECK(forca_academy_folder_in(tmp.dir / "Forca Academy") == tmp.dir / "Forca Academy"); // not nested twice

    // A journal kept directly in a chosen folder, from before the rule, stays where it is.
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir / "My journal", err));
    CHECK(forca_academy_folder_in(tmp.dir / "My journal") == tmp.dir / "My journal");
}

TEST_CASE("The starter files are created once and never overwritten", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    for (const char* f : { "AGENTS.md", "INDEX.md", "lessons.md" })
        CHECK(fs::is_regular_file(tmp.dir / f));
    for (const char* d : { "prints", "printers", "materials", "playbooks", "experiments" })
        CHECK(fs::is_directory(tmp.dir / d));

    {
        boost::nowide::ofstream f((tmp.dir / "AGENTS.md").string(), std::ios::binary | std::ios::trunc);
        f << "my own notes";
    }
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    CHECK(read_file(tmp.dir / "AGENTS.md") == "my own notes");
}

TEST_CASE("Each sent print gets its own dated record folder", "[ForcaAcademy]")
{
    ScopedDir            tmp;
    std::string          err;
    const nlohmann::json record = { { "time", "2026-09-27T14:03:12" },
                                    { "title", "Benchy" },
                                    { "printer", { { "preset", "Bambu Lab H2S 0.4 nozzle" }, { "device", "My H2S" } } },
                                    { "outcome", nullptr } };

    const fs::path first = forca_academy_write_record(tmp.dir, record, "PNGDATA", err);
    REQUIRE_FALSE(first.empty());
    CHECK(first == tmp.dir / "prints" / "2026" / "2026-09-27_1403_benchy");
    CHECK(nlohmann::json::parse(read_file(first / "record.json")) == record);
    CHECK(read_file(first / "thumbnail.png") == "PNGDATA");
    CHECK(fs::is_directory(first / "photos"));
    const std::string notes = read_file(first / "notes.md");
    CHECK(notes.find("# Benchy") == 0);
    CHECK(notes.find("My H2S") != std::string::npos);
    CHECK(fs::is_regular_file(tmp.dir / "AGENTS.md")); // the skeleton comes with the first record

    // The same print name in the same minute does not overwrite the first record; no thumbnail -> no file.
    const fs::path second = forca_academy_write_record(tmp.dir, record, "", err);
    CHECK(second == tmp.dir / "prints" / "2026" / "2026-09-27_1403_benchy-2");
    CHECK_FALSE(fs::exists(second / "thumbnail.png"));
}

TEST_CASE("The journal lists recorded prints newest first", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    CHECK(forca_academy_list_prints(tmp.dir).empty()); // no folder yet
    forca_academy_write_record(tmp.dir, { { "time", "2026-09-26T09:00:00" }, { "title", "Old" }, { "printer", { { "preset", "P1S" } } } }, "", err);
    forca_academy_write_record(tmp.dir, { { "time", "2026-09-27T10:30:00" }, { "title", "New" }, { "printer", { { "device", "My H2S" } } } }, "", err);
    fs::create_directories(tmp.dir / "prints" / "2026" / "not-a-record"); // skipped: no record.json

    const auto prints = forca_academy_list_prints(tmp.dir);
    REQUIRE(prints.size() == 2);
    CHECK(prints[0].title == "New");
    CHECK(prints[0].printer == "My H2S");
    CHECK(prints[1].title == "Old");
    CHECK(prints[1].printer == "P1S");
    CHECK(prints[1].result.empty());
    CHECK(forca_academy_list_prints(tmp.dir, 1).size() == 1);
}

TEST_CASE("The journal lists the process, the filaments used and the outcome tags", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    const nlohmann::json record = { { "time", "2026-10-13T08:00:00" },
                           { "title", "Bracket" },
                           { "printer", { { "device", "My H2S" } } },
                           { "process", "0.20mm Standard" },
                           { "filaments", { { { "slot", 1 }, { "preset", "Sunlu PETG" }, { "used_g", 12.5 } },
                                            { { "slot", 2 }, { "preset", "Bambu PLA" }, { "used_g", 0.0 } }, // unused slot
                                            { { "slot", 3 }, { "preset", "Old record PLA" } } } },        // no use figure
                           { "outcome", { { "result", "partial" }, { "tags", nlohmann::json::array({ "stringing", "warping" }) } } } };
    forca_academy_write_record(tmp.dir, record, "", err);

    const auto prints = forca_academy_list_prints(tmp.dir);
    REQUIRE(prints.size() == 1);
    CHECK(prints[0].process == "0.20mm Standard");
    CHECK(prints[0].filaments == std::vector<std::string>{ "Sunlu PETG", "Old record PLA" });
    CHECK(prints[0].tags == std::vector<std::string>{ "stringing", "warping" });
}

TEST_CASE("Print history counts outcomes for one printer and filament preset", "[ForcaAcademy]")
{
    auto print = [](const char* time, const char* printer, const char* filament, const char* result, std::vector<std::string> tags) {
        ForcaAcademyPrint p;
        p.folder    = fs::path("prints") / time;
        p.time      = time;
        p.printer   = printer;
        p.filaments = { filament };
        p.result    = result;
        p.tags      = std::move(tags);
        return p;
    };
    // Newest first, as forca_academy_list_prints returns them.
    const std::vector<ForcaAcademyPrint> prints = {
        print("2026-10-13", "H2S", "Sunlu PETG", "partial", { "stringing" }),
        print("2026-10-12", "H2S", "Sunlu PETG", "success", {}),
        print("2026-10-11", "H2S", "Sunlu PETG", "", {}),                                  // not recorded
        print("2026-10-10", "H2S", "Sunlu PETG", "failed", { "stringing", "warping" }),
        print("2026-10-09", "AD5M", "Sunlu PETG", "failed", { "bed_adhesion" }),           // other printer
        print("2026-10-08", "H2S", "Bambu PLA", "failed", { "spaghetti" }),                // other filament
    };

    const ForcaAcademyHistory h = forca_academy_history(prints, "H2S", "Sunlu PETG");
    CHECK(h.success == 1);
    CHECK(h.partial == 1);
    CHECK(h.failed == 1);
    CHECK(h.not_recorded == 1);
    CHECK(h.recorded() == 3);
    REQUIRE(h.tags.size() == 2);
    CHECK(h.tags[0] == std::pair<std::string, int>{ "stringing", 2 }); // most common first
    CHECK(h.tags[1] == std::pair<std::string, int>{ "warping", 1 });
    REQUIRE(h.folders.size() == 4);
    CHECK(h.folders[0] == fs::path("prints") / "2026-10-13"); // newest first

    CHECK(forca_academy_history(prints, "", "Sunlu PETG").recorded() == 4); // any printer
    CHECK(forca_academy_history(prints, "H2S", "Unknown").folders.empty());
}

TEST_CASE("Insights group the journal by printer, filament or process", "[ForcaAcademy]")
{
    auto print = [](const char* time, const char* printer, std::vector<std::string> filaments, const char* process, const char* result,
                    std::vector<std::string> tags) {
        ForcaAcademyPrint p;
        p.folder    = fs::path("prints") / time;
        p.time      = time;
        p.printer   = printer;
        p.filaments = std::move(filaments);
        p.process   = process;
        p.result    = result;
        p.tags      = std::move(tags);
        return p;
    };
    const std::vector<ForcaAcademyPrint> prints = {
        print("2026-10-13", "H2S", { "PETG", "PLA" }, "0.20", "failed", { "warping" }),
        print("2026-10-12", "H2S", { "PETG" }, "0.20", "success", {}),
        print("2026-10-11", "AD5M", { "PLA" }, "0.16", "partial", { "stringing" }),
        print("2026-10-10", "H2S", { "PLA" }, "", "success", {}), // no process: skipped when grouping by process
    };

    const auto by_printer = forca_academy_insights(prints, ForcaAcademyGroup::Printer);
    REQUIRE(by_printer.size() == 2);
    CHECK(by_printer[0].name == "H2S"); // most prints first
    CHECK(by_printer[0].history.folders.size() == 3);
    CHECK(by_printer[0].history.success == 2);
    CHECK(by_printer[0].history.failed == 1);
    CHECK(by_printer[0].last_failure == "2026-10-13");
    CHECK(by_printer[1].name == "AD5M");
    CHECK(by_printer[1].history.tags.front().first == "stringing");

    const auto by_filament = forca_academy_insights(prints, ForcaAcademyGroup::Filament);
    REQUIRE(by_filament.size() == 2);
    CHECK(by_filament[0].name == "PLA"); // 3 prints (the two-filament print counts under both)
    CHECK(by_filament[0].history.folders.size() == 3);
    CHECK(by_filament[1].name == "PETG");
    CHECK(by_filament[1].history.folders.size() == 2);

    const auto by_process = forca_academy_insights(prints, ForcaAcademyGroup::Process);
    REQUIRE(by_process.size() == 2);
    CHECK(by_process[0].name == "0.20");
    CHECK(by_process[1].name == "0.16");
    CHECK(by_process[1].last_failure == "2026-10-11");
}

TEST_CASE("An outcome goes into the record and replaces Forca's block in the notes", "[ForcaAcademy]")
{
    ScopedDir      tmp;
    std::string    err;
    const fs::path folder = forca_academy_write_record(tmp.dir, { { "time", "2026-09-27T10:30:00" }, { "title", "Benchy" }, { "outcome", nullptr } }, "", err);
    REQUIRE_FALSE(folder.empty());

    const nlohmann::json first = { { "result", "failed" }, { "tags", nlohmann::json::array({ "stringing", "warping" }) }, { "note", "Lifted at the corner." } };
    REQUIRE(forca_academy_set_outcome(folder, first, err));
    CHECK(nlohmann::json::parse(read_file(folder / "record.json"))["outcome"] == first);
    std::string notes = read_file(folder / "notes.md");
    CHECK(notes.find("**Failed** -- stringing, warping") != std::string::npos);
    CHECK(notes.find("Lifted at the corner.") != std::string::npos);
    CHECK(notes.find("Not recorded yet.") == std::string::npos);

    // Saving again replaces Forca's block instead of adding a second one.
    REQUIRE(forca_academy_set_outcome(folder, { { "result", "success" }, { "tags", nlohmann::json::array() }, { "note", "" } }, err));
    notes = read_file(folder / "notes.md");
    CHECK(notes.find("**Success**") != std::string::npos);
    CHECK(notes.find("Failed") == std::string::npos);

    // Notes the user rewrote are left alone; the record still gets the outcome.
    {
        boost::nowide::ofstream f((folder / "notes.md").string(), std::ios::binary | std::ios::trunc);
        f << "my own notes";
    }
    REQUIRE(forca_academy_set_outcome(folder, first, err));
    CHECK(read_file(folder / "notes.md") == "my own notes");
    CHECK(nlohmann::json::parse(read_file(folder / "record.json"))["outcome"]["result"] == "failed");
}

TEST_CASE("Photos are copied into the print without overwriting", "[ForcaAcademy]")
{
    ScopedDir      tmp;
    std::string    err;
    const fs::path folder = forca_academy_write_record(tmp.dir, { { "time", "2026-09-27T10:30:00" }, { "title", "Benchy" } }, "", err);
    fs::create_directories(tmp.dir / "camera");
    {
        boost::nowide::ofstream f((tmp.dir / "camera" / "IMG_1.jpg").string(), std::ios::binary);
        f << "JPEG";
    }
    const auto first  = forca_academy_add_photos(folder, { tmp.dir / "camera" / "IMG_1.jpg" }, err);
    const auto second = forca_academy_add_photos(folder, { tmp.dir / "camera" / "IMG_1.jpg" }, err);
    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    CHECK(first[0] == folder / "photos" / "IMG_1.jpg");
    CHECK(second[0] == folder / "photos" / "IMG_1-2.jpg");
    CHECK(read_file(second[0]) == "JPEG");
}

TEST_CASE("AI paths stay inside the Academy folder", "[ForcaAcademy]")
{
    const fs::path dir = fs::path("C:/Academy");
    fs::path       out;
    std::string    err;
    REQUIRE(forca_academy_resolve(dir, "prints/2026/x/notes.md", out, err));
    CHECK(out == dir / "prints" / "2026" / "x" / "notes.md");
    CHECK(forca_academy_relative(dir, out) == "prints/2026/x/notes.md");
    REQUIRE(forca_academy_resolve(dir, "./lessons.md", out, err));
    CHECK(out == dir / "lessons.md");
    CHECK_FALSE(forca_academy_resolve(dir, "", out, err));
    CHECK_FALSE(forca_academy_resolve(dir, "../secret.md", out, err));
    CHECK_FALSE(forca_academy_resolve(dir, "prints/../../x.md", out, err));
    CHECK_FALSE(forca_academy_resolve(dir, "/etc/passwd", out, err));
#ifdef _WIN32
    CHECK_FALSE(forca_academy_resolve(dir, "C:/Windows/x.md", out, err));
    CHECK_FALSE(forca_academy_resolve(dir, "D:x.md", out, err));
#endif
}

TEST_CASE("Only an untouched starter file counts as Forca's", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    CHECK(forca_academy_is_untouched_starter(tmp.dir / "INDEX.md"));
    CHECK(forca_academy_is_untouched_starter(tmp.dir / "lessons.md"));
    CHECK_FALSE(forca_academy_is_untouched_starter(tmp.dir / "AGENTS.md")); // never the AI's to replace
    {
        boost::nowide::ofstream f((tmp.dir / "INDEX.md").string(), std::ios::binary | std::ios::app);
        f << "\nmy printer: H2S\n";
    }
    CHECK_FALSE(forca_academy_is_untouched_starter(tmp.dir / "INDEX.md"));
}

TEST_CASE("Calibration results build up a page's history without touching the user's text", "[ForcaAcademy]")
{
    ScopedDir      tmp;
    std::string    err;
    const fs::path page = tmp.dir / "materials" / "my-pla.md";
    REQUIRE(forca_academy_append_history(page, "My PLA", "2026-09-27 -- flow: 0.98", err));
    {
        boost::nowide::ofstream f(page.string(), std::ios::binary | std::ios::app);
        f << "Dries in 4 h at 50 C.\n"; // the user's own line
    }
    REQUIRE(forca_academy_append_history(page, "My PLA", "2026-09-28 -- pa: 0.030", err));
    const std::string text = read_file(page);
    CHECK(text.find("# My PLA") == 0);
    CHECK(text.find("## Calibration history") != std::string::npos);
    CHECK(text.find("- 2026-09-27 -- flow: 0.98") != std::string::npos);
    CHECK(text.find("Dries in 4 h at 50 C.") != std::string::npos);
    CHECK(text.find("- 2026-09-28 -- pa: 0.030") > text.find("Dries in 4 h"));
    CHECK(text.find("# My PLA", 1) == std::string::npos); // the heading is written once
}

TEST_CASE("AMS trays get the names the printer shows", "[ForcaAcademy]")
{
    CHECK(forca_academy_tray_label(0, 0) == "A1");
    CHECK(forca_academy_tray_label(0, 3) == "A4");
    CHECK(forca_academy_tray_label(1, 0) == "B1");
    CHECK(forca_academy_tray_label(128, 0) == "HT-A");
    CHECK(forca_academy_tray_label(129, 0) == "HT-B");
    CHECK(forca_academy_tray_label(255, 0) == "Ext");
    CHECK(forca_academy_tray_label(254, 0) == "Ext 2");
}

TEST_CASE("AMS tray colours are #RRGGBB with or without the printer's #", "[ForcaAcademy]")
{
    CHECK(forca_academy_tray_colour("057748FF") == "#057748");
    CHECK(forca_academy_tray_colour("#057748FF") == "#057748");
    CHECK(forca_academy_tray_colour("#8E9089") == "#8E9089");
    CHECK(forca_academy_tray_colour("") == "");
    CHECK(forca_academy_tray_colour("#") == "");
}

TEST_CASE("How the printer said a print ended goes into the record and the notes", "[ForcaAcademy]")
{
    ScopedDir      tmp;
    std::string    err;
    const fs::path folder = forca_academy_write_record(tmp.dir, { { "time", "2026-09-28T17:29:00" }, { "title", "Cube" } }, "", err);
    REQUIRE(forca_academy_set_print_end(folder, { { "state", "failed" }, { "time_from_send_s", 1260 }, { "print_error", "Nozzle clog" } }, err));
    CHECK(nlohmann::json::parse(read_file(folder / "record.json"))["print_end"]["state"] == "failed");
    std::string notes = read_file(folder / "notes.md");
    CHECK(notes.find("**Printer reported:** failed, 21 min after Forca sent it.") != std::string::npos);
    CHECK(notes.find("Printer error: Nozzle clog") != std::string::npos);

    // A second report replaces Forca's block instead of adding one.
    REQUIRE(forca_academy_set_print_end(folder, { { "state", "finished" } }, err));
    notes = read_file(folder / "notes.md");
    CHECK(notes.find("**Printer reported:** finished.") != std::string::npos);
    CHECK(notes.find("failed") == std::string::npos);
    CHECK(forca_academy_list_prints(tmp.dir)[0].ended == "finished");
}

TEST_CASE("A printer page keeps the user's notes while Forca refreshes its facts", "[ForcaAcademy]")
{
    ScopedDir      tmp;
    std::string    err;
    const fs::path page = tmp.dir / "printers" / "h2s.md";
    REQUIRE(forca_academy_write_printer_facts(page, "H2S", "- Firmware: 1.0\n", err));
    {
        boost::nowide::ofstream f(page.string(), std::ios::binary | std::ios::app);
        f << "Nozzle swapped to hardened steel.\n";
    }
    REQUIRE(forca_academy_write_printer_facts(page, "H2S", "- Firmware: 1.1\n", err));
    std::string text = read_file(page);
    CHECK(text.find("# H2S") == 0);
    CHECK(text.find("- Firmware: 1.1") != std::string::npos);
    CHECK(text.find("- Firmware: 1.0") == std::string::npos);
    CHECK(text.find("Nozzle swapped to hardened steel.") != std::string::npos);

    // A page Forca started for calibration history gets the facts under its title.
    const fs::path calib = tmp.dir / "printers" / "my-preset.md";
    REQUIRE(forca_academy_append_history(calib, "My preset", "2026-09-28 -- vfa: none", err));
    REQUIRE(forca_academy_write_printer_facts(calib, "My preset", "- Model: X\n", err));
    text = read_file(calib);
    CHECK(text.find("- Model: X") < text.find("## Calibration history"));
    CHECK(text.find("- 2026-09-28 -- vfa: none") != std::string::npos);

    // A page the user wrote without Forca's markers is left alone.
    const fs::path own = tmp.dir / "printers" / "own.md";
    {
        boost::nowide::ofstream f(own.string(), std::ios::binary);
        f << "# My notes\n";
    }
    REQUIRE(forca_academy_write_printer_facts(own, "own", "- Model: X\n", err));
    CHECK(read_file(own) == "# My notes\n");
}

TEST_CASE("A record's settings go to settings.json, not record.json", "[ForcaAcademy]")
{
    ScopedDir            tmp;
    std::string          err;
    const nlohmann::json settings = { { "process", { { "layer_height", "0.2" } } }, { "printer", nlohmann::json::object() },
                                      { "filament", { { "nozzle_temperature", "250" } } } };
    const nlohmann::json record   = { { "time", "2026-10-14T09:00:00" }, { "title", "Cube" }, { "settings", settings } };
    const fs::path       folder   = forca_academy_write_record(tmp.dir, record, "", err);
    REQUIRE_FALSE(folder.empty());
    CHECK_FALSE(nlohmann::json::parse(read_file(folder / "record.json")).contains("settings"));
    nlohmann::json read;
    REQUIRE(forca_academy_read_settings(folder, read));
    CHECK(read == settings);
}

TEST_CASE("Printer secrets and connection details never reach settings.json or its readers", "[ForcaAcademy]")
{
    CHECK(forca_academy_private_setting("printhost_apikey"));
    CHECK(forca_academy_private_setting("printhost_password"));
    CHECK(forca_academy_private_setting("print_host"));
    CHECK(forca_academy_private_setting("print_host_webui"));
    CHECK(forca_academy_private_setting("flashforge_serial_number"));
    CHECK_FALSE(forca_academy_private_setting("layer_height"));
    CHECK_FALSE(forca_academy_private_setting("host_type"));

    ScopedDir            tmp;
    std::string          err;
    const nlohmann::json settings = { { "process", { { "layer_height", "0.2" } } },
                                      { "printer", { { "printhost_apikey", "a1b2c3" }, { "print_host", "192.168.1.6" },
                                                     { "flashforge_serial_number", "SN1" }, { "gcode_flavor", "klipper" } } } };
    const fs::path folder = forca_academy_write_record(tmp.dir, { { "time", "2026-10-15T18:00:00" }, { "title", "Disc" }, { "settings", settings } }, "", err);
    REQUIRE_FALSE(folder.empty());
    const std::string on_disk = read_file(folder / "settings.json");
    CHECK(on_disk.find("a1b2c3") == std::string::npos);
    CHECK(on_disk.find("192.168.1.6") == std::string::npos);
    CHECK(on_disk.find("SN1") == std::string::npos);
    CHECK(on_disk.find("klipper") != std::string::npos);

    // A file written before the filter existed still reads without them.
    {
        boost::nowide::ofstream f((folder / "settings.json").string(), std::ios::binary | std::ios::trunc);
        f << settings.dump();
    }
    nlohmann::json read;
    REQUIRE(forca_academy_read_settings(folder, read));
    CHECK_FALSE(read["printer"].contains("printhost_apikey"));
    CHECK_FALSE(read["printer"].contains("print_host"));
    CHECK(read["printer"]["gcode_flavor"] == "klipper");
}

TEST_CASE("Comparing two prints lists the record facts and the settings that differ", "[ForcaAcademy]")
{
    const nlohmann::json ra = { { "printer", { { "device", "My H2S" } } },
                                { "process", "0.20mm Standard" },
                                { "filaments", { { { "preset", "Sunlu PETG" }, { "used_g", 10.0 } } } },
                                { "estimate", { { "print_time", "1h2m" }, { "filament_total_g", 10.04 } } },
                                { "outcome", { { "result", "partial" }, { "tags", { "stringing" } } } },
                                { "unsaved_changes", { { "process", { { "wall_loops", "3" } } } } } };
    nlohmann::json       rb = ra;
    rb["outcome"]           = { { "result", "success" }, { "tags", nlohmann::json::array() } };
    rb["unsaved_changes"]   = { { "process", { { "sparse_infill_density", "20%" } } } };

    auto find = [](const ForcaAcademyComparison& c, const std::string& section, const std::string& key) -> const ForcaAcademyDiff* {
        for (const ForcaAcademyDiff& d : c.rows)
            if (d.section == section && d.key == key)
                return &d;
        return nullptr;
    };

    SECTION("without settings.json only the unsaved changes are compared")
    {
        const ForcaAcademyComparison c = forca_academy_compare(ra, nullptr, rb, nullptr);
        CHECK_FALSE(c.full);
        const ForcaAcademyDiff* printer = find(c, "print", "printer");
        REQUIRE(printer);
        CHECK_FALSE(printer->differs); // record facts are always listed
        const ForcaAcademyDiff* grams = find(c, "print", "filament_g");
        REQUIRE(grams);
        CHECK(grams->a == "10.0 g");
        const ForcaAcademyDiff* result = find(c, "print", "result");
        REQUIRE(result);
        CHECK(result->differs);
        const ForcaAcademyDiff* walls = find(c, "process", "wall_loops");
        REQUIRE(walls);
        CHECK(walls->a == "3");
        CHECK(walls->b.empty()); // B used the preset's value
        CHECK(find(c, "process", "sparse_infill_density"));
    }
    SECTION("with both settings.json files every differing setting is listed, and only those")
    {
        const nlohmann::json sa = { { "process", { { "layer_height", "0.2" }, { "wall_loops", "2" } } },
                                    { "filament", { { "nozzle_temperature", "250" } } } };
        nlohmann::json       sb = sa;
        sb["process"]["wall_loops"] = "3";
        const ForcaAcademyComparison c = forca_academy_compare(ra, sa, rb, sb);
        CHECK(c.full);
        CHECK(find(c, "process", "wall_loops"));
        CHECK_FALSE(find(c, "process", "layer_height"));
        CHECK_FALSE(find(c, "filament", "nozzle_temperature"));
        CHECK_FALSE(find(c, "process", "sparse_infill_density")); // unsaved_changes are not used then
    }
}

TEST_CASE("An experiment page links both prints and is never overwritten", "[ForcaAcademy]")
{
    ScopedDir         tmp;
    std::string       err;
    ForcaAcademyPrint a;
    a.folder = tmp.dir / "prints" / "2026" / "2026-10-12_1000_cube";
    a.title  = "Cube";
    a.time   = "2026-10-12T10:00:00";
    a.result = "partial";
    a.tags   = { "stringing" };
    ForcaAcademyPrint b = a;
    b.folder            = tmp.dir / "prints" / "2026" / "2026-10-13_1000_cube";
    b.result            = "success";
    b.tags.clear();

    const fs::path file = forca_academy_write_experiment(tmp.dir, "Retraction 0.8 vs 1.2", "2026-10-14", a, b,
                                                         { "Retraction length: A 0.8 mm, B 1.2 mm" }, err);
    REQUIRE_FALSE(file.empty());
    CHECK(file == tmp.dir / "experiments" / "2026-10-14_retraction-0-8-vs-1-2.md");
    const std::string text = read_file(file);
    CHECK(text.find("(../prints/2026/2026-10-12_1000_cube)") != std::string::npos);
    CHECK(text.find("partial (stringing)") != std::string::npos);
    CHECK(text.find("- Retraction length: A 0.8 mm, B 1.2 mm") != std::string::npos);

    const fs::path again = forca_academy_write_experiment(tmp.dir, "Retraction 0.8 vs 1.2", "2026-10-14", a, b, {}, err);
    CHECK(again == tmp.dir / "experiments" / "2026-10-14_retraction-0-8-vs-1-2-2.md");

    auto list = forca_academy_list_experiments(tmp.dir);
    REQUIRE(list.size() == 2);
    CHECK(list[0].file == again); // newest (last-named) first
    CHECK(list[1].title == "Retraction 0.8 vs 1.2");
    CHECK(list[1].date == "2026-10-14");
    CHECK_FALSE(list[1].has_result);

    // The user writes the result in place of the placeholder.
    std::string  edited = text;
    const size_t at     = edited.find("Not written yet");
    REQUIRE(at != std::string::npos);
    edited.replace(at, edited.find('\n', at) - at, "B was cleaner: keep 1.2 mm.");
    {
        boost::nowide::ofstream f(file.string(), std::ios::binary | std::ios::trunc);
        f << edited;
    }
    list = forca_academy_list_experiments(tmp.dir);
    CHECK(list[1].has_result);
}

TEST_CASE("A possible pattern is the same tag on N of the last M prints for one printer and filament", "[ForcaAcademy]")
{
    auto print = [](const char* time, const char* printer, const char* result, std::vector<std::string> tags) {
        ForcaAcademyPrint p;
        p.folder    = fs::path("prints") / time;
        p.time      = time;
        p.printer   = printer;
        p.filaments = { "Sunlu PETG" };
        p.result    = result;
        p.tags      = std::move(tags);
        return p;
    };
    const std::vector<ForcaAcademyPrint> prints = {
        print("2026-10-14", "H2S", "partial", { "stringing" }),
        print("2026-10-13", "H2S", "", { "stringing" }), // no outcome: not counted
        print("2026-10-12", "H2S", "success", {}),
        print("2026-10-11", "H2S", "partial", { "stringing", "warping" }),
        print("2026-10-10", "H2S", "failed", { "stringing" }),
        print("2026-10-09", "AD5M", "failed", { "stringing" }), // other printer
    };

    const auto three_of_ten = forca_academy_patterns(prints, 3, 10);
    REQUIRE(three_of_ten.size() == 1);
    CHECK(three_of_ten[0].printer == "H2S");
    CHECK(three_of_ten[0].filament == "Sunlu PETG");
    CHECK(three_of_ten[0].tag == "stringing");
    CHECK(three_of_ten[0].count == 3);
    CHECK(three_of_ten[0].window == 4); // only 4 prints with an outcome so far
    CHECK(three_of_ten[0].folders.front() == fs::path("prints") / "2026-10-14");
    CHECK(three_of_ten[0].id == "H2S|Sunlu PETG|stringing");

    CHECK(forca_academy_patterns(prints, 3, 3).empty());      // the last 3 with an outcome hold only 2 stringing prints
    CHECK(forca_academy_patterns(prints, 1, 10).size() == 3); // H2S stringing + warping, AD5M stringing
}

TEST_CASE("Dismissed patterns are remembered once each", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    CHECK(forca_academy_dismissed(tmp.dir).empty());
    REQUIRE(forca_academy_dismiss(tmp.dir, "H2S|PETG|stringing", err));
    REQUIRE(forca_academy_dismiss(tmp.dir, "H2S|PETG|stringing", err));
    CHECK(forca_academy_dismissed(tmp.dir) == std::vector<std::string>{ "H2S|PETG|stringing" });
}

TEST_CASE("A lesson draft links its evidence and is appended to lessons.md", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    ForcaAcademyPattern p;
    p.printer  = "H2S";
    p.filament = "Sunlu PETG";
    p.tag      = "layer_shift";
    p.count    = 3;
    p.window   = 7;
    p.folders  = { tmp.dir / "prints" / "2026" / "2026-10-14_0900_cube" };

    const std::string draft = forca_academy_lesson_draft(tmp.dir, p, "medium", "2026-10-14");
    CHECK(draft.find("## Layer shift with Sunlu PETG on H2S") == 0);
    CHECK(draft.find("Confidence: medium") != std::string::npos);
    CHECK(draft.find("[2026-10-14_0900_cube](prints/2026/2026-10-14_0900_cube)") != std::string::npos);
    CHECK(draft.find("3 of the last 7") != std::string::npos);

    const std::string before = read_file(tmp.dir / "lessons.md");
    REQUIRE(forca_academy_append_lesson(tmp.dir, draft, err));
    const std::string after = read_file(tmp.dir / "lessons.md");
    CHECK(after.find(before.substr(0, before.size() - 1)) == 0); // the existing text stays first
    CHECK(after.find("\n\n## Layer shift") != std::string::npos);
}

TEST_CASE("INDEX.md gets Forca's facts block once and keeps the user's text", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    ForcaAcademyPrint p;
    p.folder    = tmp.dir / "prints" / "2026" / "x";
    p.printer   = "H2S";
    p.filaments = { "Sunlu PETG" };
    p.result    = "success";
    const std::string facts = forca_academy_index_facts(tmp.dir, { p }, {});
    CHECK(facts.find("1 recorded, 1 with an outcome: 1 success") != std::string::npos);
    CHECK(facts.find("- H2S -- 1 prints, 1 of 1 with an outcome succeeded") != std::string::npos);

    REQUIRE(forca_academy_write_index_facts(tmp.dir, "first", err));
    std::string index = read_file(tmp.dir / "INDEX.md");
    CHECK(index.find("# Forca Academy -- index\n\n<!-- forca:facts -->\nfirst\n<!-- /forca:facts -->") == 0);
    CHECK(index.find("## Top lessons") != std::string::npos); // the rest of the page stays

    CHECK(forca_academy_is_untouched_starter(tmp.dir / "INDEX.md")); // Forca's block isn't the user's edit

    REQUIRE(forca_academy_write_index_facts(tmp.dir, "second", err));
    index = read_file(tmp.dir / "INDEX.md");
    CHECK(index.find("first") == std::string::npos);
    CHECK(index.find("second") != std::string::npos);

    // The user removed the block: Forca doesn't put it back.
    {
        boost::nowide::ofstream f((tmp.dir / "INDEX.md").string(), std::ios::binary | std::ios::trunc);
        f << "# My index\n";
    }
    REQUIRE(forca_academy_write_index_facts(tmp.dir, "third", err));
    CHECK(read_file(tmp.dir / "INDEX.md") == "# My index\n");
}

TEST_CASE("Starter playbooks are written once per tag and never recreated", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    for (const std::string& tag : forca_academy_tag_keys())
        CHECK(fs::is_regular_file(tmp.dir / "playbooks" / (tag + ".md")));
    CHECK(read_file(tmp.dir / "playbooks" / "stringing.md").find("# Stringing") == 0);
    CHECK(forca_academy_is_untouched_starter(tmp.dir / "playbooks" / "stringing.md"));

    {
        boost::nowide::ofstream f((tmp.dir / "playbooks" / "warping.md").string(), std::ios::binary | std::ios::trunc);
        f << "my warping notes";
    }
    fs::remove(tmp.dir / "playbooks" / "ringing.md");
    REQUIRE(forca_academy_write_starter_playbooks(tmp.dir, err));
    CHECK(read_file(tmp.dir / "playbooks" / "warping.md") == "my warping notes");
    CHECK_FALSE(forca_academy_is_untouched_starter(tmp.dir / "playbooks" / "warping.md"));
    CHECK_FALSE(fs::exists(tmp.dir / "playbooks" / "ringing.md"));
}

TEST_CASE("A journal made before a tag existed gets that tag's playbook, and only that one", "[ForcaAcademy]")
{
    ScopedDir   tmp;
    std::string err;
    REQUIRE(forca_academy_ensure_skeleton(tmp.dir, err));
    // A journal from before the bridging tag: its playbook was never written, and the user removed the stringing one.
    fs::remove(tmp.dir / "playbooks" / "bridging.md");
    fs::remove(tmp.dir / "playbooks" / "stringing.md");
    nlohmann::json  insights = nlohmann::json::parse(read_file(tmp.dir / "forca" / "insights.json"));
    nlohmann::json& written  = insights["playbooks_written"];
    written.erase(std::remove(written.begin(), written.end(), nlohmann::json("bridging")), written.end());
    {
        boost::nowide::ofstream f((tmp.dir / "forca" / "insights.json").string(), std::ios::binary | std::ios::trunc);
        f << insights.dump();
    }

    REQUIRE(forca_academy_write_starter_playbooks(tmp.dir, err));
    const std::string bridging = read_file(tmp.dir / "playbooks" / "bridging.md");
    CHECK(bridging.find("# Bridging") == 0);
    CHECK(bridging.find("Thick external bridges") != std::string::npos);
    CHECK_FALSE(fs::exists(tmp.dir / "playbooks" / "stringing.md"));
}

TEST_CASE("Thick external bridges is suggested only after a print tagged bridging, and only while it is off", "[ForcaAcademy]")
{
    auto print = [](const char* time, const char* filament, std::vector<std::string> tags) {
        ForcaAcademyPrint p;
        p.folder    = fs::path("prints") / time;
        p.time      = time;
        p.printer   = "AD5M";
        p.filaments = { filament };
        p.result    = "partial";
        p.tags      = std::move(tags);
        return p;
    };
    const std::vector<ForcaAcademyPrint> prints = {
        print("2026-10-16", "Generic PLA", { "bridging", "warping" }),
        print("2026-10-15", "Sunlu PETG", { "stringing" }),
    };
    const ForcaAcademyHistory pla  = forca_academy_history(prints, "AD5M", "Generic PLA");
    const ForcaAcademyHistory petg = forca_academy_history(prints, "AD5M", "Sunlu PETG");

    CHECK(forca_academy_suggest_thick_bridges({ petg, pla }, false));
    CHECK_FALSE(forca_academy_suggest_thick_bridges({ petg, pla }, true));
    CHECK_FALSE(forca_academy_suggest_thick_bridges({ petg }, false));
    CHECK_FALSE(forca_academy_suggest_thick_bridges({}, false));
}

TEST_CASE("A problem tagged on a successful print counts, and is reported as successful", "[ForcaAcademy]")
{
    auto print = [](const char* time, const char* result, std::vector<std::string> tags) {
        ForcaAcademyPrint p;
        p.folder    = fs::path("prints") / time;
        p.time      = time;
        p.printer   = "H2S";
        p.filaments = { "Bambu ABS" };
        p.result    = result;
        p.tags      = std::move(tags);
        return p;
    };
    const std::vector<ForcaAcademyPrint> prints = {
        print("2026-10-14", "success", { "stringing" }), // a little stringing on a good print
        print("2026-10-13", "success", { "stringing" }),
        print("2026-10-12", "partial", { "stringing" }),
        print("2026-10-11", "success", {}),
    };

    const ForcaAcademyHistory h = forca_academy_history(prints, "H2S", "Bambu ABS");
    REQUIRE(h.tags.size() == 1);
    CHECK(h.tags[0] == std::pair<std::string, int>{ "stringing", 3 });
    CHECK(h.tags_on_success.at("stringing") == 2);

    const auto patterns = forca_academy_patterns(prints, 3, 10);
    REQUIRE(patterns.size() == 1);
    CHECK(patterns[0].count == 3);
    CHECK(patterns[0].on_success == 2);

    ScopedDir tmp;
    CHECK(forca_academy_lesson_draft(tmp.dir, patterns[0], "low", "2026-10-15").find("3 of the last 4 prints with an outcome, 2 of them successful") !=
          std::string::npos);
}
