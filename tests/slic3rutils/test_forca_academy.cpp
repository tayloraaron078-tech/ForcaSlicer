// Forca: Forca Academy's file side (src/slic3r/GUI/ForcaAcademy.cpp) -- folder names, the starter files, and the
// per-print record folders it writes when a print is sent.
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

#include "slic3r/GUI/ForcaAcademy.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <sstream>

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
