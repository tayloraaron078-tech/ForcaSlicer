// Forca: forca_brand() (src/slic3r/GUI/I18N.cpp) swaps Orca's product name for Forca's in translated GUI text.
// Same Windows include prologue as test_dev_mapping.cpp (wx pulls in <windows.h>).
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

#include "slic3r/GUI/I18N.hpp"

using Slic3r::GUI::I18N::forca_brand;

static std::string brand(const char* s) { return forca_brand(wxString::FromUTF8(s), wxString::FromUTF8(s)).ToStdString(); }

TEST_CASE("Orca's product name reads as Forca Slicer", "[ForcaBrand]")
{
    CHECK(brand("Allow only one OrcaSlicer instance") == "Allow only one Forca Slicer instance");
    CHECK(brand("Restart Orca Slicer.") == "Restart Forca Slicer.");
    CHECK(brand("OrcaSlicer's settings") == "Forca Slicer's settings");
    CHECK(brand("with OrcaSlicer so that Orca can open models from") == "with Forca Slicer so that Forca can open models from");
    CHECK(brand("the printer and Orca.") == "the printer and Forca.");
}

TEST_CASE("Orca's services, models and identifiers keep their names", "[ForcaBrand]")
{
    const char* keep = GENERATE("Log in to Orca Cloud", "Pull from OrcaCloud", "Orca Cube", "Orca Tolerance Test",
                                "Orca String Hell", "OrcaSliced Combo", "Orca Arena X1 Carbon", "Orca Preset Bundle",
                                "orcaslicer://open", "*.orca_printer", "https://github.com/OrcaSlicer/OrcaSlicer",
                                "No product name here");
    CHECK(brand(keep) == keep);
}

TEST_CASE("Text that already names Forca or tells Orca's history is left alone", "[ForcaBrand]")
{
    CHECK(brand("Forca Slicer is a fork of OrcaSlicer") == "Forca Slicer is a fork of OrcaSlicer");
    CHECK(brand("Today, OrcaSlicer is the most widely used open-source slicer") ==
          "Today, OrcaSlicer is the most widely used open-source slicer");
}

TEST_CASE("The swap applies to the translation, keyed on the English source", "[ForcaBrand]")
{
    CHECK(forca_brand("Restart OrcaSlicer", wxString::FromUTF8("OrcaSlicer neu starten")).ToStdString() ==
          "Forca Slicer neu starten");
}
