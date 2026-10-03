// Forca: the Calibration Wizard's explicit line wrapping (forca_hard_wrap, src/slic3r/GUI/ForcaCalibrationWizard.cpp),
// which must also break translated text that has no spaces (Chinese, Japanese, Thai).
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

#include "slic3r/GUI/ForcaCalibrationWizard.hpp"

#include <wx/tokenzr.h>

#include <vector>

using Slic3r::GUI::forca_hard_wrap;

static std::vector<wxString> lines_of(const wxString& s)
{
    std::vector<wxString> out;
    wxStringTokenizer tok(s, "\n", wxTOKEN_RET_EMPTY_ALL);
    while (tok.HasMoreTokens())
        out.push_back(tok.GetNextToken());
    return out;
}

TEST_CASE("English text wraps at spaces within the line length", "[ForcaCalibrationWizard]")
{
    const wxString text = "Pick the filament you are calibrating in the picker above -- or an existing profile close to it "
                          "to start from -- then press Start. The steps run in order.";
    const wxString wrapped = forca_hard_wrap(text, 60);

    const std::vector<wxString> lines = lines_of(wrapped);
    CHECK(lines.size() > 1);
    for (const wxString& line : lines)
        CHECK(line.length() <= 60);
    wxString rejoined = wrapped;
    rejoined.Replace("\n", " ");
    CHECK(rejoined == text); // only spaces became line breaks
}

TEST_CASE("Text without spaces breaks into lines of at most the line length", "[ForcaCalibrationWizard]")
{
    // 70 CJK ideographs with an ideographic full stop after every 10th: 140 columns (each ideograph is 2 wide).
    wxString text;
    for (int i = 0; i < 70; ++i) {
        text += wxUniChar(0x4E00 + i);
        if (i % 10 == 9)
            text += wxUniChar(0x3002);
    }
    const wxString wrapped = forca_hard_wrap(text, 60);

    const std::vector<wxString> lines = lines_of(wrapped);
    REQUIRE(lines.size() >= 3); // 154 columns in lines of at most 60
    for (const wxString& line : lines) {
        CHECK(line.length() <= 31); // 30 ideographs, or 29 plus a full stop kept on its line
        CHECK(line[0] != wxUniChar(0x3002)); // never starts a line with closing punctuation
    }
    wxString rejoined = wrapped;
    rejoined.Replace("\n", "");
    CHECK(rejoined == text); // nothing lost or added but the breaks
}
