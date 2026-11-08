// Forca: print grants (src/slic3r/GUI/ForcaAIAutonomy.cpp) -- the printer slot names the AI gives for each filament
// ("A4", "Ext"), which Forca sets in the print dialog and checks before it presses Send.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#include "catch2/catch_test_macros.hpp"
#include "catch2/catch_message.hpp"
#include <vector>
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

#include "slic3r/GUI/ForcaAI.hpp"

#include <string>

using namespace Slic3r::GUI;

TEST_CASE("AMS slot names map to the printer's AMS and slot numbers", "[ForcaAIAutonomy]")
{
    std::string ams, slot;
    REQUIRE(forca_ai_parse_tray("A1", ams, slot));
    CHECK(ams == "0");
    CHECK(slot == "0");
    REQUIRE(forca_ai_parse_tray("b4", ams, slot)); // any case
    CHECK(ams == "1");
    CHECK(slot == "3");
    REQUIRE(forca_ai_parse_tray("HT-B", ams, slot)); // AMS HT units count from 128, one slot each
    CHECK(ams == "129");
    CHECK(slot == "0");
}

TEST_CASE("External spool names map to the virtual trays", "[ForcaAIAutonomy]")
{
    std::string ams, slot;
    for (const char* name : { "Ext", "EXT", "ext 1", "External" }) {
        INFO(name);
        REQUIRE(forca_ai_parse_tray(name, ams, slot));
        CHECK(ams == "255"); // VIRTUAL_TRAY_MAIN_ID
        CHECK(slot == "0");
    }
    REQUIRE(forca_ai_parse_tray("Ext 2", ams, slot));
    CHECK(ams == "254"); // VIRTUAL_TRAY_DEPUTY_ID
}

TEST_CASE("Anything that is not a slot name is refused", "[ForcaAIAutonomy]")
{
    std::string ams, slot;
    for (const char* name : { "", "A", "A5", "A0", "1", "AMS 1", "HT-1", "Ext 3", "spool" }) {
        INFO(name);
        CHECK_FALSE(forca_ai_parse_tray(name, ams, slot));
    }
}

TEST_CASE("A grant plate may hold only the grant's files, up to the copies still owed", "[ForcaAIAutonomy]")
{
    const std::vector<ForcaAIGrantFile> files = { { "lid.stl", 4, 1, "" }, { "box.step", 2, 0, "" } };
    std::vector<int>                    count;

    // 3 lids (4 asked, 1 sent; one part each) and 2 boxes: exactly what is still owed.
    CHECK(forca_ai_grant_files_reason(files, { { "lid.stl", 2 }, { "box.step", 2 }, { "lid.stl", 1 } }, count).empty());
    REQUIRE(count.size() == 2);
    CHECK(count[0] == 3);
    CHECK(count[1] == 2);

    // One lid too many.
    CHECK_FALSE(forca_ai_grant_files_reason(files, { { "lid.stl", 4 } }, count).empty());
    // Anything not from the grant's files, or made in Forca (no source file).
    CHECK_FALSE(forca_ai_grant_files_reason(files, { { "lid.stl", 1 }, { "benchy.stl", 1 } }, count).empty());
    CHECK_FALSE(forca_ai_grant_files_reason(files, { { "", 1 } }, count).empty());
}

TEST_CASE("One copy of a grant .3mf is all of its parts", "[ForcaAIAutonomy]")
{
    // A .3mf of 3 parts, 2 copies asked: 6 parts owed.
    const std::vector<ForcaAIGrantFile> files = { { "set.3mf", 2, 0, "", 3 } };
    std::vector<int>                    count;

    // Its 3 parts once each.
    CHECK(forca_ai_grant_files_reason(files, { { "set.3mf", 1 }, { "set.3mf", 1 }, { "set.3mf", 1 } }, count).empty());
    CHECK(count[0] == 3);
    // Each part twice: both copies.
    CHECK(forca_ai_grant_files_reason(files, { { "set.3mf", 2 }, { "set.3mf", 2 }, { "set.3mf", 2 } }, count).empty());
    CHECK(count[0] == 6);
    // 7 parts: one more than 2 copies hold.
    CHECK_FALSE(forca_ai_grant_files_reason(files, { { "set.3mf", 3 }, { "set.3mf", 2 }, { "set.3mf", 2 } }, count).empty());
}

TEST_CASE("One copy of a grant file may span several plates", "[ForcaAIAutonomy]")
{
    // A .3mf of 3 parts, 1 copy asked. The first plate sends 2 of them.
    ForcaAIGrantFile file{ "two plates.3mf", 1, 0, "", 3 };
    std::vector<int> count;
    CHECK(forca_ai_grant_files_reason({ file }, { { "two plates.3mf", 2 } }, count).empty());
    CHECK(count[0] == 2);
    file.sent += count[0];
    CHECK(file.copies_sent() == 0);
    CHECK(file.parts_owed() == 1);

    // The second plate may send the last part, but not two.
    CHECK_FALSE(forca_ai_grant_files_reason({ file }, { { "two plates.3mf", 2 } }, count).empty());
    CHECK(forca_ai_grant_files_reason({ file }, { { "two plates.3mf", 1 } }, count).empty());
    file.sent += count[0];
    CHECK(file.copies_sent() == 1);
    CHECK(file.parts_owed() == 0);

    // Nothing more once the copy is complete.
    CHECK_FALSE(forca_ai_grant_files_reason({ file }, { { "two plates.3mf", 1 } }, count).empty());
}

TEST_CASE("Grant files match however the path is spelled", "[ForcaAIAutonomy]")
{
    const std::vector<ForcaAIGrantFile> files = { { "parts/lid.stl", 2, 0, "" } };
    std::vector<int>                    count;
    CHECK(forca_ai_grant_files_reason(files, { { "parts/../parts/lid.stl", 1 } }, count).empty());
    CHECK(count[0] == 1);
}
